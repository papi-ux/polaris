/**
 * @file src/platform/linux/private_session_attach.cpp
 * @brief Prove that a launched app actually attached to the private session.
 */

#include "private_session_attach.h"

#ifdef __linux__

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
