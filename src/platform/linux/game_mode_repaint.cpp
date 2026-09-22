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
  #include <future>
  #include <memory>
  #include <mutex>
  #include <string_view>
  #include <system_error>
  #include <thread>

  #include <fcntl.h>
  #include <fstream>
  #include <sys/stat.h>
  #include <unistd.h>

  #ifdef POLARIS_BUILD_X11_XCB
    #include <xcb/xcb.h>
  #endif

  // libdrm comes with every capture that can show a Game Mode screen.
  #if defined(POLARIS_BUILD_DRM) || defined(POLARIS_BUILD_PORTAL)
    #define POLARIS_GAME_MODE_READS_DRM 1
    #include <xf86drm.h>
    #include <xf86drmMode.h>
  #endif

  #include "gamescope_process.h"
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

    /// gamescope's degrees for each of the kernel's panel orientations, which are numbered normal,
    /// bottom up, left side up and right side up.
    std::optional<int> turn_for_drm_panel_orientation(std::uint64_t value) {
      switch (value) {
        case 0:
          return 0;
        case 1:
          return 180;
        case 2:
          return 90;
        case 3:
          return 270;
        default:
          return std::nullopt;
      }
    }

    bool is_internal_connector_name(std::string_view type) {
      return type == "eDP"sv || type == "LVDS"sv || type == "DSI"sv;
    }

    std::optional<std::pair<int, int>> mode_size(std::string_view text) {
      const auto x = text.find('x');
      if (x == std::string_view::npos) {
        return std::nullopt;
      }
      int width = 0;
      int height = 0;
      const auto [width_end, width_error] = std::from_chars(text.data(), text.data() + x, width);
      const auto [height_end, height_error] = std::from_chars(text.data() + x + 1, text.data() + text.size(), height);
      if (width_error != std::errc {} || height_error != std::errc {} || width_end != text.data() + x ||
          height_end != text.data() + text.size() || width <= 0 || height <= 0) {
        return std::nullopt;
      }
      return std::pair {width, height};
    }

    /// The first connected internal connector, from sysfs, which needs no device access but says
    /// nothing of the panel's orientation.
    std::optional<internal_panel_t> internal_panel_from_sysfs() {
      std::error_code ec;
      std::vector<std::filesystem::path> connectors;
      for (std::filesystem::directory_iterator it {"/sys/class/drm", ec}, last; !ec && it != last; it.increment(ec)) {
        // card<N>-<type>-<M>, such as card0-eDP-1
        const auto name = it->path().filename().string();
        const auto first = name.find('-');
        const auto second = name.rfind('-');
        if (!name.starts_with("card"sv) || first == std::string::npos || second == first ||
            !is_internal_connector_name(std::string_view {name}.substr(first + 1, second - first - 1))) {
          continue;
        }
        connectors.push_back(it->path());
      }
      std::sort(connectors.begin(), connectors.end());
      for (const auto &connector : connectors) {
        std::ifstream status_file {connector / "status"};
        std::string status;
        if (!std::getline(status_file, status) || status != "connected"sv) {
          continue;
        }
        std::ifstream modes_file {connector / "modes"};
        std::string mode;
        internal_panel_t panel;
        if (std::getline(modes_file, mode)) {
          if (const auto size = mode_size(mode)) {
            panel.native_width = size->first;
            panel.native_height = size->second;
          }
        }
        return panel;
      }
      return std::nullopt;
    }

    /// The first connected internal panel, with the orientation the kernel gives it.
    [[maybe_unused]] std::optional<internal_panel_t> connected_internal_panel() {
  #ifdef POLARIS_GAME_MODE_READS_DRM
      std::error_code ec;
      std::vector<std::filesystem::path> cards;
      for (std::filesystem::directory_iterator it {"/dev/dri", ec}, last; !ec && it != last; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.size() > 4 && name.starts_with("card"sv) &&
            std::all_of(name.begin() + 4, name.end(), [](char c) {
              return c >= '0' && c <= '9';
            })) {
          cards.push_back(it->path());
        }
      }
      std::sort(cards.begin(), cards.end());
      for (const auto &card : cards) {
        const int fd = ::open(card.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
          continue;
        }
        std::optional<internal_panel_t> found;
        if (auto *resources = drmModeGetResources(fd)) {
          for (int i = 0; !found && i < resources->count_connectors; ++i) {
            // The current state, so asking never makes the kernel probe the connector again.
            auto *connector = drmModeGetConnectorCurrent(fd, resources->connectors[i]);
            if (!connector) {
              continue;
            }
            const bool internal = connector->connector_type == DRM_MODE_CONNECTOR_eDP ||
                                  connector->connector_type == DRM_MODE_CONNECTOR_LVDS ||
                                  connector->connector_type == DRM_MODE_CONNECTOR_DSI;
            if (internal && connector->connection == DRM_MODE_CONNECTED) {
              internal_panel_t panel;
              const drmModeModeInfo *mode = nullptr;
              for (int m = 0; m < connector->count_modes && !mode; ++m) {
                if (connector->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
                  mode = &connector->modes[m];
                }
              }
              if (!mode && connector->count_modes > 0) {
                mode = &connector->modes[0];
              }
              if (mode) {
                panel.native_width = mode->hdisplay;
                panel.native_height = mode->vdisplay;
              }
              for (int p = 0; p < connector->count_props; ++p) {
                if (auto *property = drmModeGetProperty(fd, connector->props[p])) {
                  if (std::string_view {property->name} == "panel orientation"sv) {
                    panel.drm_orientation = connector->prop_values[p];
                  }
                  drmModeFreeProperty(property);
                }
              }
              found = panel;
            }
            drmModeFreeConnector(connector);
          }
          drmModeFreeResources(resources);
        }
        ::close(fd);
        if (found) {
          return found;
        }
      }
  #endif
      return internal_panel_from_sysfs();
    }

    std::vector<std::string> split_nul(const std::string &text) {
      std::vector<std::string> parts;
      std::size_t start = 0;
      while (start < text.size()) {
        const auto end = text.find('\0', start);
        parts.emplace_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      return parts;
    }

    /// A forced orientation from any gamescope this account runs.
    [[maybe_unused]] std::optional<int> session_forced_orientation() {
      constexpr std::size_t k_max_cmdline = 64 * 1024;
      const auto uid = ::getuid();
      std::error_code ec;
      for (std::filesystem::directory_iterator it {"/proc", ec}, last; !ec && it != last; it.increment(ec)) {
        const auto name = it->path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) {
              return c >= '0' && c <= '9';
            })) {
          continue;
        }
        struct stat info {};
        if (::stat(it->path().c_str(), &info) != 0 || info.st_uid != uid) {
          continue;
        }
        std::ifstream file {it->path() / "cmdline", std::ios::binary};
        std::string cmdline(k_max_cmdline, '\0');
        file.read(cmdline.data(), static_cast<std::streamsize>(cmdline.size()));
        cmdline.resize(static_cast<std::size_t>(std::max<std::streamsize>(file.gcount(), 0)));
        const auto argv = split_nul(cmdline);
        if (argv.empty() || std::filesystem::path {argv.front()}.filename() != "gamescope") {
          continue;
        }
        if (const auto forced = forced_orientation_from_args(argv)) {
          return forced;
        }
      }
      return std::nullopt;
    }

  #ifdef POLARIS_BUILD_X11_XCB

    struct connection_t {
      xcb_connection_t *conn {nullptr};
      xcb_window_t root {XCB_WINDOW_NONE};
      int width = 0;
      int height = 0;

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
        width = screen.data->width_in_pixels;
        height = screen.data->height_in_pixels;
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

  std::optional<int> forced_orientation_from_args(const std::vector<std::string> &argv) {
    constexpr auto flag = "--force-orientation"sv;
    std::optional<std::string_view> value;
    for (std::size_t i = 1; i < argv.size(); ++i) {
      const std::string_view arg {argv[i]};
      if (arg == "--"sv) {
        // What follows is the command gamescope runs, not its own options.
        break;
      }
      if (arg == flag) {
        if (i + 1 < argv.size()) {
          value = argv[++i];
        }
      } else if (arg.starts_with(flag) && arg.size() > flag.size() && arg[flag.size()] == '=') {
        value = arg.substr(flag.size() + 1);
      }
    }
    if (!value) {
      return std::nullopt;
    }
    if (*value == "normal"sv) {
      return 0;
    }
    if (*value == "left"sv) {
      return 90;
    }
    if (*value == "upsidedown"sv) {
      return 180;
    }
    if (*value == "right"sv) {
      return 270;
    }
    return std::nullopt;
  }

  touch_turn_t touch_turn_for(std::optional<bool> external, std::optional<int> forced, const std::optional<internal_panel_t> &panel) {
    if (external.value_or(false)) {
      return {0, "an external screen"sv};
    }
    // A forced orientation is for the internal screen, so it counts only once that is the one shown.
    if (forced && (external.has_value() || panel)) {
      return {*forced, "gamescope --force-orientation"sv};
    }
    if (!panel) {
      return {0, "a host with no internal panel"sv};
    }
    if (panel->drm_orientation) {
      if (const auto turn = turn_for_drm_panel_orientation(*panel->drm_orientation)) {
        return {*turn, "the internal panel's orientation"sv};
      }
    }
    if (panel->native_width > 0 && panel->native_width < panel->native_height) {
      return {270, "a portrait internal panel"sv};
    }
    return {0, "a landscape internal panel"sv};
  }

  std::optional<session_screen_t> session_screen() {
  #ifdef POLARIS_BUILD_X11_XCB
    // The idle compositor of a gamescope stream is Polaris's own gamescope, never the session's,
    // even when its Xwayland took the lower display number.
    std::optional<std::string> polaris_display;
    if (const char *runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
      namespace gp = stream_runtime::gamescope_process;
      if (const auto marker = gp::validated_marker(std::filesystem::path {runtime} / "polaris-gamescope.pid")) {
        polaris_display = gp::discover_owned_x11_display(*marker);
      }
    }
    for (const auto &display : local_x_displays()) {
      if (polaris_display && display == *polaris_display) {
        continue;
      }
      const connection_t x {display};
      // The server a gamescope started first carries its focus properties, and its root is the
      // screen gamescope composites and exports.
      if (!x || !x.belongs_to_gamescope() || !x.root_property("GAMESCOPE_FOCUSED_WINDOW"sv)) {
        continue;
      }
      if (x.width <= 0 || x.height <= 0) {
        continue;
      }
      // gamescope says here whether the screen it shows is an external one.
      std::optional<bool> external;
      if (const auto value = x.root_property("GAMESCOPE_DISPLAY_IS_EXTERNAL"sv); value && value->size() >= sizeof(std::uint32_t)) {
        std::uint32_t word = 0;
        std::memcpy(&word, value->data(), sizeof(word));
        external = word != 0;
      }
      session_screen_t screen {x.width, x.height};
      screen.touch_turn = external.value_or(false) ?
                            touch_turn_for(external, std::nullopt, std::nullopt) :
                            touch_turn_for(external, session_forced_orientation(), connected_internal_panel());
      return screen;
    }
  #endif
    return std::nullopt;
  }

  std::optional<session_screen_t> session_screen_within(std::chrono::milliseconds limit) {
    // A reading stuck on an Xwayland that stopped answering keeps its thread, so one is enough.
    static std::atomic_bool in_flight {false};
    if (in_flight.exchange(true)) {
      BOOST_LOG(warning) << "game_mode: the last reading of the Game Mode screen has not come back, so touch is placed across the whole frame"sv;
      return std::nullopt;
    }
    auto answer = std::make_shared<std::promise<std::optional<session_screen_t>>>();
    auto future = answer->get_future();
    try {
      std::thread([answer]() {
        try {
          answer->set_value(session_screen());
        } catch (...) {
          answer->set_exception(std::current_exception());
        }
        in_flight = false;
      }).detach();
    } catch (const std::exception &) {
      in_flight = false;
      return std::nullopt;
    }
    if (future.wait_for(limit) != std::future_status::ready) {
      BOOST_LOG(warning) << "game_mode: the Game Mode screen did not answer within "sv << limit.count()
                         << " ms, so touch is placed across the whole frame and not turned"sv;
      return std::nullopt;
    }
    try {
      return future.get();
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "game_mode: reading the Game Mode screen failed: "sv << e.what();
      return std::nullopt;
    }
  }

  void request_focused_window_repaint_async() {
    static std::atomic_flag in_flight = ATOMIC_FLAG_INIT;
    if (in_flight.test_and_set()) {
      return;
    }

    const auto ask = []() {
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
          {
            // Said once where it is seen: without it a still screen is simply black.
            static std::once_flag said;
            std::call_once(said, []() {
              BOOST_LOG(info) << "game_mode: this build has no X client, so a Game Mode screen that is standing still stays black until something on it moves"sv;
            });
          }
          break;
      }
      in_flight.clear();
    };

    try {
      std::thread(ask).detach();
    } catch (const std::exception &error) {
      // No thread to be had. The flag is let go so a later capture can ask again.
      BOOST_LOG(warning) << "game_mode: could not start the thread that asks a still Game Mode screen to draw: "sv << error.what();
      in_flight.clear();
    }
  }

}  // namespace platf::game_mode_host

#endif
