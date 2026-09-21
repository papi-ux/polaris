/**
 * @file src/platform/linux/game_mode_repaint.cpp
 * @brief Get a first frame out of a Steam Game Mode screen that is standing still.
 */

#include "game_mode_repaint.h"

#ifdef __linux__

  #include <algorithm>
  #include <array>
  #include <atomic>
  #include <charconv>
  #include <cstdlib>
  #include <cstring>
  #include <string_view>
  #include <thread>

  #ifdef POLARIS_BUILD_X11_XCB
    #include <xcb/xcb.h>
  #endif

  #include "src/logging.h"

using namespace std::literals;

namespace platf::game_mode_host {
  namespace {

    /// A host has a handful of X servers at most. The cap keeps a directory full of stale sockets
    /// from turning one repaint into a long walk.
    constexpr std::size_t k_max_displays = 8;

    /// gamescope's property is declared as 32-bit cardinals but filled from a C string, and Xlib
    /// keeps only the low half of each 64-bit word it is handed. The name survives in the first
    /// word and nowhere else.
    constexpr std::size_t k_focus_display_bytes = 4;

    std::optional<unsigned> display_number(std::string_view text) {
      if (text.empty() || text.size() > 3) {
        return std::nullopt;
      }
      unsigned number = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
      if (error != std::errc {} || end != text.data() + text.size()) {
        return std::nullopt;
      }
      return number;
    }

  #ifdef POLARIS_BUILD_X11_XCB

    struct connection_t {
      xcb_connection_t *conn {nullptr};
      xcb_window_t root {XCB_WINDOW_NONE};

      explicit connection_t(const std::string &display) {
        conn = xcb_connect(display.c_str(), nullptr);
        if (!conn || xcb_connection_has_error(conn)) {
          close();
          return;
        }
        const auto screen = xcb_setup_roots_iterator(xcb_get_setup(conn));
        if (!screen.data) {
          close();
          return;
        }
        root = screen.data->root;
      }

      connection_t(const connection_t &) = delete;
      connection_t &operator=(const connection_t &) = delete;

      ~connection_t() {
        close();
      }

      explicit operator bool() const {
        return conn != nullptr;
      }

      void close() {
        if (conn) {
          xcb_disconnect(conn);
          conn = nullptr;
        }
      }

      /// The bytes of a root property, or nothing when this server never heard of it.
      std::optional<std::vector<std::uint8_t>> root_property(std::string_view name) const {
        auto *atom = xcb_intern_atom_reply(
          conn,
          xcb_intern_atom(conn, 1, static_cast<std::uint16_t>(name.size()), name.data()),
          nullptr
        );
        if (!atom) {
          return std::nullopt;
        }
        const xcb_atom_t id = atom->atom;
        free(atom);
        if (id == XCB_ATOM_NONE) {
          return std::nullopt;
        }

        auto *reply = xcb_get_property_reply(
          conn,
          xcb_get_property(conn, 0, root, id, XCB_GET_PROPERTY_TYPE_ANY, 0, 4),
          nullptr
        );
        if (!reply) {
          return std::nullopt;
        }
        std::optional<std::vector<std::uint8_t>> value;
        if (reply->type != XCB_ATOM_NONE) {
          const auto *bytes = static_cast<const std::uint8_t *>(xcb_get_property_value(reply));
          const int length = xcb_get_property_value_length(reply);
          value.emplace(bytes, bytes + std::max(length, 0));
        }
        free(reply);
        return value;
      }

      /// A gamescope numbers every Xwayland it starts. No other X server carries this.
      bool belongs_to_gamescope() const {
        return root_property("GAMESCOPE_XWAYLAND_SERVER_ID"sv).has_value();
      }
    };

    bool send_expose(const connection_t &x, xcb_window_t window) {
      auto *geometry = xcb_get_geometry_reply(x.conn, xcb_get_geometry(x.conn, window), nullptr);
      if (!geometry) {
        return false;  // the window went away, or never lived on this server
      }

      // The wire takes a full 32-byte event whatever the event's own size is.
      std::array<char, 32> wire {};
      xcb_expose_event_t event {};
      event.response_type = XCB_EXPOSE;
      event.window = window;
      event.width = geometry->width;
      event.height = geometry->height;
      std::memcpy(wire.data(), &event, sizeof(event));
      free(geometry);

      auto *error = xcb_request_check(
        x.conn,
        xcb_send_event_checked(x.conn, 0, window, XCB_EVENT_MASK_EXPOSURE, wire.data())
      );
      if (error) {
        free(error);
        return false;
      }
      return true;
    }

  #endif

  }  // namespace

  std::optional<std::string> focus_display_from_property(std::span<const std::uint8_t> value) {
    const auto usable = value.first(std::min(value.size(), k_focus_display_bytes));
    const auto end = std::find(usable.begin(), usable.end(), std::uint8_t {0});
    if (end == usable.end()) {
      return std::nullopt;  // no terminator where the name has to fit, so the rest was cut off
    }

    const std::string name {usable.begin(), end};
    if (name.size() < 2 || name.front() != ':' || !display_number(std::string_view {name}.substr(1))) {
      return std::nullopt;
    }
    return name;
  }

  std::vector<std::string> local_x_displays(const std::filesystem::path &socket_dir) {
    std::vector<unsigned> numbers;
    std::error_code ec;
    for (std::filesystem::directory_iterator it {socket_dir, ec}, last; !ec && it != last; it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (name.size() < 2 || name.front() != 'X') {
        continue;
      }
      if (const auto number = display_number(std::string_view {name}.substr(1))) {
        numbers.push_back(*number);
      }
    }

    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    if (numbers.size() > k_max_displays) {
      numbers.resize(k_max_displays);
    }

    std::vector<std::string> displays;
    displays.reserve(numbers.size());
    for (const auto number : numbers) {
      displays.push_back(":" + std::to_string(number));
    }
    return displays;
  }

  repaint_result_e request_focused_window_repaint() {
  #ifdef POLARIS_BUILD_X11_XCB
    bool saw_gamescope = false;
    for (const auto &display : local_x_displays()) {
      const connection_t first {display};
      if (!first || !first.belongs_to_gamescope()) {
        continue;
      }
      saw_gamescope = true;

      // Only the server a gamescope started first carries its focus properties.
      const auto window_value = first.root_property("GAMESCOPE_FOCUSED_WINDOW"sv);
      if (!window_value || window_value->size() < sizeof(std::uint32_t)) {
        continue;
      }
      std::uint32_t window = 0;
      std::memcpy(&window, window_value->data(), sizeof(window));
      if (window == XCB_WINDOW_NONE) {
        continue;
      }

      // Window ids are per server and the same number can exist on two of them, so the server is
      // taken from gamescope's word and never from which one happens to know the id.
      const auto display_value = first.root_property("GAMESCOPE_FOCUS_DISPLAY"sv);
      const auto focus_display = display_value ? focus_display_from_property(*display_value) : std::nullopt;
      if (!focus_display) {
        continue;
      }

      if (*focus_display == display) {
        return send_expose(first, window) ? repaint_result_e::sent : repaint_result_e::no_focused_window;
      }
      const connection_t holder {*focus_display};
      if (!holder || !holder.belongs_to_gamescope()) {
        continue;
      }
      return send_expose(holder, window) ? repaint_result_e::sent : repaint_result_e::no_focused_window;
    }
    return saw_gamescope ? repaint_result_e::no_focused_window : repaint_result_e::no_session_display;
  #else
    return repaint_result_e::unavailable;
  #endif
  }

  void request_focused_window_repaint_async() {
    static std::atomic_flag in_flight = ATOMIC_FLAG_INIT;
    if (in_flight.test_and_set()) {
      return;
    }

    std::thread([]() {
      const auto result = request_focused_window_repaint();
      switch (result) {
        case repaint_result_e::sent:
          BOOST_LOG(info) << "game_mode: the screen is standing still, so the window Game Mode is showing was asked to draw again"sv;
          break;
        case repaint_result_e::no_session_display:
          BOOST_LOG(debug) << "game_mode: no X display of the session's gamescope to ask for a first frame"sv;
          break;
        case repaint_result_e::no_focused_window:
          BOOST_LOG(debug) << "game_mode: the session's gamescope names no window to ask for a first frame"sv;
          break;
        case repaint_result_e::unavailable:
          BOOST_LOG(debug) << "game_mode: this build has no X client, so a still Game Mode screen stays black until it moves"sv;
          break;
      }
      in_flight.clear();
    }).detach();
  }

}  // namespace platf::game_mode_host

#endif
