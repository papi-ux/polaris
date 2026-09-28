/**
 * @file src/platform/linux/private_session_attach.cpp
 * @brief Prove that a launched app actually attached to the private session.
 */

#include "private_session_attach.h"

#ifdef __linux__

  #include <algorithm>
  #include <cctype>
  #include <cstdlib>
  #include <filesystem>
  #include <future>
  #include <memory>
  #include <sstream>
  #include <string_view>
  #include <system_error>
  #include <thread>
  #include <vector>

  #ifdef POLARIS_BUILD_WAYLAND
    #include <ext-foreign-toplevel-list-v1.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <wayland-client.h>
  #endif

  #ifdef POLARIS_BUILD_X11_XCB
    #include <cstdlib>
    #include <cstring>
    #include <vector>

    #include <xcb/xcb.h>
    #ifdef POLARIS_BUILD_X11_XCB_RES
      #include <xcb/res.h>
    #endif
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

  std::vector<std::uint32_t> close_targets(
    const std::vector<x11_window_t> &windows,
    const window_owner_test_t &belongs_to_app
  ) {
    std::vector<std::uint32_t> targets;
    for (const auto &window : windows) {
      if (!window.viewable || window.override_redirect || !window.accepts_delete) {
        continue;
      }
      bool belongs = false;
      if (window.client_pid) {
        // The server's word, from the client's socket, decides; the window's own is not asked.
        belongs = belongs_to_app.host_pid && belongs_to_app.host_pid(*window.client_pid);
      } else if (window.pid) {
        const auto &test = belongs_to_app.net_wm_pid ? belongs_to_app.net_wm_pid : belongs_to_app.host_pid;
        belongs = test && test(*window.pid);
      }
      if (belongs) {
        targets.push_back(window.id);
      }
    }
    return targets;
  }

  std::vector<std::uint32_t> close_targets(
    const std::vector<x11_window_t> &windows,
    const std::function<bool(std::uint32_t pid)> &belongs_to_app
  ) {
    return close_targets(windows, window_owner_test_t {belongs_to_app, {}});
  }

  close_request_result_t request_x11_window_close(
    const std::string &x11_display,
    const window_owner_test_t &belongs_to_app
  ) {
    close_request_result_t result {};
    if (x11_display.empty()) {
      return result;
    }

  #ifdef POLARIS_BUILD_X11_XCB
    xcb_connection_t *conn = xcb_connect(x11_display.c_str(), nullptr);
    if (!conn || xcb_connection_has_error(conn)) {
      xcb_disconnect(conn);
      return result;  // unavailable
    }

    const auto intern = [conn](const char *name) -> xcb_atom_t {
      auto *reply = xcb_intern_atom_reply(
        conn,
        xcb_intern_atom(conn, 1, static_cast<std::uint16_t>(std::strlen(name)), name),
        nullptr
      );
      if (!reply) {
        return XCB_ATOM_NONE;
      }
      const auto atom = reply->atom;
      free(reply);
      return atom;
    };
    // only_if_exists: an atom no client ever interned is one no window of this session carries.
    const auto wm_protocols = intern("WM_PROTOCOLS");
    const auto wm_delete_window = intern("WM_DELETE_WINDOW");
    const auto net_wm_pid = intern("_NET_WM_PID");

  #ifdef POLARIS_BUILD_X11_XCB_RES
    // Asked for now, so its answer arrives with the tree's rather than as a round trip of its own.
    xcb_prefetch_extension_data(conn, &xcb_res_id);
  #endif
    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t screen = xcb_setup_roots_iterator(setup);
    auto *tree = screen.data ? xcb_query_tree_reply(conn, xcb_query_tree(conn, screen.data->root), nullptr) : nullptr;
    if (!tree) {
      xcb_disconnect(conn);
      return result;
    }
    result.status = probe_status_e::ok;
    bool client_pids = false;
  #ifdef POLARIS_BUILD_X11_XCB_RES
    // X-Resource names the process behind each window from the client's socket, as a host pid.
    // Xwayland has it; a server without it leaves only each window's own _NET_WM_PID.
    const auto *resource_extension = xcb_get_extension_data(conn, &xcb_res_id);
    client_pids = resource_extension && resource_extension->present;
  #endif
    result.client_pids = client_pids;
    if (wm_protocols == XCB_ATOM_NONE || wm_delete_window == XCB_ATOM_NONE ||
        (net_wm_pid == XCB_ATOM_NONE && !client_pids)) {
      // Nothing here can be asked to close, or nothing names its process.
      free(tree);
      xcb_disconnect(conn);
      return result;
    }

    const xcb_window_t *children = xcb_query_tree_children(tree);
    const int count = xcb_query_tree_children_length(tree);

    // Every request out before any reply is read, as the probe above does: one round trip.
    struct pending_t {
      xcb_window_t id;
      xcb_get_window_attributes_cookie_t attributes;
      std::optional<xcb_get_property_cookie_t> pid;
      xcb_get_property_cookie_t protocols;
  #ifdef POLARIS_BUILD_X11_XCB_RES
      std::optional<xcb_res_query_client_ids_cookie_t> client;
  #endif
    };

    std::vector<pending_t> pending;
    pending.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
      pending_t entry {
        children[i],
        xcb_get_window_attributes(conn, children[i]),
        std::nullopt,
        xcb_get_property(conn, 0, children[i], wm_protocols, XCB_ATOM_ATOM, 0, 32),
      };
      if (net_wm_pid != XCB_ATOM_NONE) {
        entry.pid = xcb_get_property(conn, 0, children[i], net_wm_pid, XCB_ATOM_CARDINAL, 0, 1);
      }
  #ifdef POLARIS_BUILD_X11_XCB_RES
      if (client_pids) {
        xcb_res_client_id_spec_t spec {children[i], XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
        entry.client = xcb_res_query_client_ids(conn, 1, &spec);
      }
  #endif
      pending.push_back(entry);
    }

    std::vector<x11_window_t> windows;
    windows.reserve(pending.size());
    for (auto &entry : pending) {
      x11_window_t window {};
      window.id = entry.id;
      if (auto *attrs = xcb_get_window_attributes_reply(conn, entry.attributes, nullptr)) {
        window.viewable = attrs->map_state == XCB_MAP_STATE_VIEWABLE;
        window.override_redirect = attrs->override_redirect != 0;
        free(attrs);
      }
      if (entry.pid) {
        if (auto *pid = xcb_get_property_reply(conn, *entry.pid, nullptr)) {
          if (pid->format == 32 && xcb_get_property_value_length(pid) >= 4) {
            window.pid = *static_cast<const std::uint32_t *>(xcb_get_property_value(pid));
          }
          free(pid);
        }
      }
  #ifdef POLARIS_BUILD_X11_XCB_RES
      if (entry.client) {
        if (auto *client = xcb_res_query_client_ids_reply(conn, *entry.client, nullptr)) {
          for (auto ids = xcb_res_query_client_ids_ids_iterator(client); ids.rem > 0; xcb_res_client_id_value_next(&ids)) {
            if ((ids.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) != 0 &&
                xcb_res_client_id_value_value_length(ids.data) >= 1) {
              window.client_pid = *xcb_res_client_id_value_value(ids.data);
            }
          }
          free(client);
        }
      }
  #endif
      if (auto *protocols = xcb_get_property_reply(conn, entry.protocols, nullptr)) {
        if (protocols->format == 32) {
          const auto *atoms = static_cast<const xcb_atom_t *>(xcb_get_property_value(protocols));
          const auto atom_count = xcb_get_property_value_length(protocols) / 4;
          for (int i = 0; i < atom_count; ++i) {
            if (atoms[i] == wm_delete_window) {
              window.accepts_delete = true;
              break;
            }
          }
        }
        free(protocols);
      }
      windows.push_back(window);
    }
    free(tree);

    for (const auto id : close_targets(windows, belongs_to_app)) {
      xcb_client_message_event_t event {};
      event.response_type = XCB_CLIENT_MESSAGE;
      event.format = 32;
      event.window = id;
      event.type = wm_protocols;
      event.data.data32[0] = wm_delete_window;
      event.data.data32[1] = XCB_CURRENT_TIME;
      xcb_send_event(conn, 0, id, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&event));
      ++result.windows_asked;
    }
    // Delivered before the connection goes: a request still in the output buffer is one never sent.
    if (result.windows_asked > 0) {
      free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), nullptr));
    }
    xcb_disconnect(conn);
  #else
    (void) belongs_to_app;
  #endif

    return result;
  }

  close_request_result_t request_x11_window_close_within(
    const std::string &x11_display,
    window_owner_test_t belongs_to_app,
    std::chrono::milliseconds timeout,
    close_request_t request
  ) {
    if (x11_display.empty() || !request) {
      return {};
    }
    // Shared with the thread, which outlives this call when the display stops answering.
    auto answer = std::make_shared<std::promise<close_request_result_t>>();
    auto future = answer->get_future();
    try {
      std::thread {[answer, x11_display, belongs_to_app = std::move(belongs_to_app), request = std::move(request)]() {
        close_request_result_t result {};
        try {
          result = request(x11_display, belongs_to_app);
        } catch (...) {
          // Nothing was asked that is known of, which is how an unreachable display reads too.
        }
        answer->set_value(result);
      }}.detach();
    } catch (const std::system_error &) {
      return {};
    }
    if (future.wait_for(timeout) != std::future_status::ready) {
      close_request_result_t result {};
      result.timed_out = true;
      return result;
    }
    return future.get();
  }

  namespace {
    bool looks_like_flatpak_app_id(std::string_view token) {
      if (std::count(token.begin(), token.end(), '.') < 2 || token.front() == '.' || token.back() == '.') {
        return false;
      }
      return std::all_of(token.begin(), token.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-';
      });
    }

    std::optional<std::string> app_id_from_ref(std::string token) {
      // app/<id>/<arch>/<branch> or <id>//<branch>
      if (token.starts_with("app/")) {
        token = token.substr(4);
      }
      if (const auto slash = token.find('/'); slash != std::string::npos) {
        token = token.substr(0, slash);
      }
      if (!looks_like_flatpak_app_id(token)) {
        return std::nullopt;
      }
      return token;
    }
  }  // namespace

  std::optional<std::string> flatpak_run_app_id(const std::string &cmd) {
    const auto tokens = split_command(cmd);
    bool saw_flatpak = false;
    bool saw_run = false;
    for (const auto &raw : tokens) {
      auto token = raw;
      // Quotes are not honored when splitting, so a quoted app id keeps its quotes.
      while (!token.empty() && (token.front() == '\'' || token.front() == '"')) {
        token.erase(token.begin());
      }
      while (!token.empty() && (token.back() == '\'' || token.back() == '"')) {
        token.pop_back();
      }
      if (token.empty()) {
        continue;
      }
      if (token.find("/flatpak/exports/bin/") != std::string::npos) {
        return app_id_from_ref(token_basename(token));
      }
      if (!saw_flatpak) {
        saw_flatpak = token_basename(token) == "flatpak";
        continue;
      }
      if (!saw_run) {
        saw_run = token == "run";
        continue;
      }
      if (token.starts_with('-')) {
        continue;
      }
      if (auto app_id = app_id_from_ref(token)) {
        return app_id;
      }
    }
    return std::nullopt;
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
