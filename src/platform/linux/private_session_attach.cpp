/**
 * @file src/platform/linux/private_session_attach.cpp
 * @brief Prove that a launched app actually attached to the private session.
 */

#include "private_session_attach.h"

#ifdef __linux__

  #include <algorithm>
  #include <cstdlib>
  #include <filesystem>
  #include <sstream>
  #include <string_view>
  #include <vector>

  #ifdef POLARIS_BUILD_WAYLAND
    #include <ext-foreign-toplevel-list-v1.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <wayland-client.h>
  #endif

  #ifdef POLARIS_BUILD_X11_XCB
    #include <vector>

    #include <xcb/xcb.h>
  #endif

using namespace std::literals;

namespace private_session_attach {
  namespace {

  #ifdef POLARIS_BUILD_WAYLAND

    /// A wedged compositor must not strand the watcher thread in a blocking
    /// roundtrip, so every probe bounds its own reads.
    constexpr int k_probe_socket_timeout_ms = 3000;

    struct probe_state_t {
      ext_foreign_toplevel_list_v1 *list = nullptr;
      int toplevel_count = 0;
    };

    void handle_global(
      void *data,
      wl_registry *registry,
      std::uint32_t name,
      const char *interface,
      std::uint32_t version
    ) {
      auto *state = static_cast<probe_state_t *>(data);
      if (state->list || std::string_view(interface) != ext_foreign_toplevel_list_v1_interface.name) {
        return;
      }
      (void) version;
      state->list = static_cast<ext_foreign_toplevel_list_v1 *>(
        wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1)
      );
    }

    void handle_global_remove(void *, wl_registry *, std::uint32_t) {}

    constexpr wl_registry_listener k_registry_listener {
      .global = handle_global,
      .global_remove = handle_global_remove,
    };

    void handle_toplevel(
      void *data,
      ext_foreign_toplevel_list_v1 *,
      ext_foreign_toplevel_handle_v1 *toplevel
    ) {
      auto *state = static_cast<probe_state_t *>(data);
      ++state->toplevel_count;
      // Only the count matters here. Releasing each handle as it arrives keeps
      // the probe from accumulating objects for windows it never inspects.
      ext_foreign_toplevel_handle_v1_destroy(toplevel);
    }

    void handle_finished(void *, ext_foreign_toplevel_list_v1 *) {}

    constexpr ext_foreign_toplevel_list_v1_listener k_list_listener {
      .toplevel = handle_toplevel,
      .finished = handle_finished,
    };

    void bound_socket_reads(wl_display *display) {
      const int fd = wl_display_get_fd(display);
      if (fd < 0) {
        return;
      }
      timeval timeout {};
      timeout.tv_sec = k_probe_socket_timeout_ms / 1000;
      timeout.tv_usec = (k_probe_socket_timeout_ms % 1000) * 1000;
      (void) setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

  #endif  // POLARIS_BUILD_WAYLAND

    /// Split a command line on whitespace. Quoting is not honored: this feeds an
    /// advisory warning, and every launcher form seen in issue #234 is unquoted.
    std::vector<std::string> split_command(const std::string &cmd) {
      std::vector<std::string> tokens;
      std::istringstream stream(cmd);
      std::string token;
      while (stream >> token) {
        tokens.push_back(token);
      }
      return tokens;
    }

    std::string token_basename(const std::string &token) {
      return std::filesystem::path(token).filename().string();
    }

  }  // namespace

  probe_result_t probe_toplevels(const std::string &wayland_socket) {
    probe_result_t result {};
    if (wayland_socket.empty()) {
      return result;
    }

  #ifdef POLARIS_BUILD_WAYLAND
    wl_display *display = wl_display_connect(wayland_socket.c_str());
    if (!display) {
      return result;  // unavailable
    }
    bound_socket_reads(display);

    probe_state_t state {};
    wl_registry *registry = wl_display_get_registry(display);
    if (!registry) {
      wl_display_disconnect(display);
      return result;
    }
    wl_registry_add_listener(registry, &k_registry_listener, &state);

    // First roundtrip settles the global advertisement, so a missing toplevel
    // list after it is a real absence rather than a race.
    if (wl_display_roundtrip(display) < 0) {
      wl_registry_destroy(registry);
      wl_display_disconnect(display);
      return result;
    }

    if (!state.list) {
      result.status = probe_status_e::unsupported;
      wl_registry_destroy(registry);
      wl_display_disconnect(display);
      return result;
    }

    ext_foreign_toplevel_list_v1_add_listener(state.list, &k_list_listener, &state);

    // The compositor emits one toplevel event per existing window in response to
    // the bind, and Wayland orders those ahead of this sync's reply, so a single
    // roundtrip is enough to see all of them.
    if (wl_display_roundtrip(display) < 0) {
      ext_foreign_toplevel_list_v1_destroy(state.list);
      wl_registry_destroy(registry);
      wl_display_disconnect(display);
      return result;
    }

    result.status = probe_status_e::ok;
    result.toplevel_count = state.toplevel_count;

    ext_foreign_toplevel_list_v1_destroy(state.list);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
  #endif

    return result;
  }

  probe_result_t probe_x11_windows(const std::string &x11_display) {
    probe_result_t result {};
    if (x11_display.empty()) {
      return result;
    }

  #ifdef POLARIS_BUILD_X11_XCB
    xcb_connection_t *conn = xcb_connect(x11_display.c_str(), nullptr);
    if (!conn || xcb_connection_has_error(conn)) {
      xcb_disconnect(conn);
      return result;  // unavailable
    }

    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t screen = xcb_setup_roots_iterator(setup);
    if (!screen.data) {
      xcb_disconnect(conn);
      return result;
    }

    auto *tree = xcb_query_tree_reply(conn, xcb_query_tree(conn, screen.data->root), nullptr);
    if (!tree) {
      xcb_disconnect(conn);
      return result;
    }

    const xcb_window_t *children = xcb_query_tree_children(tree);
    const int count = xcb_query_tree_children_length(tree);

    // Send every attribute request before reading any reply: one round trip for
    // the whole tree instead of one per window.
    std::vector<xcb_get_window_attributes_cookie_t> cookies;
    cookies.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      cookies.push_back(xcb_get_window_attributes(conn, children[i]));
    }

    int viewable = 0;
    for (auto &cookie : cookies) {
      auto *attrs = xcb_get_window_attributes_reply(conn, cookie, nullptr);
      if (!attrs) {
        // The window went away between the tree query and this reply. Not an
        // error, and not a window either.
        continue;
      }
      if (attrs->map_state == XCB_MAP_STATE_VIEWABLE) {
        ++viewable;
      }
      free(attrs);
    }

    free(tree);
    xcb_disconnect(conn);

    result.status = probe_status_e::ok;
    result.toplevel_count = viewable;
  #endif

    return result;
  }

  probe_result_t combine(const probe_result_t &wayland, const probe_result_t &x11) {
    const bool wayland_measured = wayland.status == probe_status_e::ok;
    const bool x11_measured = x11.status == probe_status_e::ok;

    if (wayland_measured || x11_measured) {
      probe_result_t result {};
      result.status = probe_status_e::ok;
      // The signals overlap: a managed X11 window is both a Wayland toplevel and a
      // viewable child of the X root. Adding them would report one window as two,
      // so take the larger and treat the count as a lower bound.
      result.toplevel_count = std::max(
        wayland_measured ? wayland.toplevel_count : 0,
        x11_measured ? x11.toplevel_count : 0
      );
      return result;
    }

    // Neither measured. Prefer unsupported over unavailable, so a compositor that
    // simply cannot answer is skipped rather than retried to no purpose.
    if (wayland.status == probe_status_e::unsupported ||
        x11.status == probe_status_e::unsupported) {
      return probe_result_t {.status = probe_status_e::unsupported, .toplevel_count = 0};
    }
    return probe_result_t {.status = probe_status_e::unavailable, .toplevel_count = 0};
  }

  probe_result_t probe_private_session(
    const std::string &wayland_socket,
    const std::string &x11_display
  ) {
    return combine(probe_toplevels(wayland_socket), probe_x11_windows(x11_display));
  }

  verdict_e evaluate(
    const probe_result_t &probe,
    std::chrono::milliseconds elapsed,
    std::chrono::milliseconds grace
  ) {
    switch (probe.status) {
      case probe_status_e::unsupported:
        // Nothing to measure with; never accuse a compositor Polaris cannot ask.
        return verdict_e::skipped;
      case probe_status_e::unavailable:
        // The session may still be coming up. Past the grace period this stays a
        // failed measurement, not a failed launch.
        return elapsed >= grace ? verdict_e::skipped : verdict_e::waiting;
      case probe_status_e::ok:
        break;
    }

    if (probe.toplevel_count > 0) {
      return verdict_e::attached;
    }
    return elapsed >= grace ? verdict_e::never_attached : verdict_e::waiting;
  }

  bool may_lose_display_to_flatpak_portal(const std::string &cmd) {
    const auto tokens = split_command(cmd);
    bool saw_flatpak = false;
    for (const auto &token : tokens) {
      // Exported launcher wrappers run `flatpak run` internally, so the app id is
      // the only thing on the command line.
      if (token.find("/flatpak/exports/bin/") != std::string::npos) {
        return true;
      }
      const auto name = token_basename(token);
      if (name == "flatpak-spawn") {
        return true;
      }
      if (name == "flatpak") {
        saw_flatpak = true;
        continue;
      }
      // `flatpak run` launches an app; `flatpak list` and friends do not, and
      // warning on those would be noise.
      if (saw_flatpak && token == "run") {
        return true;
      }
    }
    return false;
  }

}  // namespace private_session_attach

#endif
