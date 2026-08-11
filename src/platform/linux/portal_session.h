/**
 * @file src/platform/linux/portal_session.h
 * @brief XDG Desktop Portal ScreenCast session + process-wide capture release.
 *
 * D-Bus CreateSession/SelectSources/Start/OpenPipeWireRemote live in
 * portal_session.cpp. portal_grab.cpp owns display_t wiring and the single
 * session media cache (portal session + PipeWire capture under one mutex).
 *
 * Ownership (S4): process-wide cache is a session media cache — created lazily
 * by portal_display_t, released only via release_global_capture() (idempotent).
 * session_media owns ordered stop (signal → release → join → nested kill).
 *
 * Teardown contract (session_media sole ordered stop path):
 *   1. signal capture shutdown
 *   2. portal::release_global_capture()  — non-blocking after short budget
 *   3. bounded join of capture threads
 *   4. only then kill gamescope/labwc
 */
#pragma once

#ifdef __linux__

  #include <cstdint>
  #include <memory>
  #include <optional>
  #include <string>
  #include <string_view>

  #include <gio/gio.h>

namespace portal {

  constexpr uint32_t portal_source_monitor = 1;
  constexpr uint32_t portal_source_window = 2;

  /**
   * @brief One ScreenCast portal session (D-Bus handle + PipeWire remote).
   */
  struct portal_session_t {
    GDBusConnection *conn = nullptr;
    std::string session_handle;
    uint32_t pw_node_id = 0;
    uint64_t pw_node_serial = 0;
    std::optional<std::string> capture_render_node;
    int pw_remote_fd = -1;
    std::string restore_token;
    bool ready = false;
    bool failed = false;

    ~portal_session_t();
  };

  bool is_portal_available();

  /**
   * @brief Cancel in-flight portal D-Bus calls and response waits.
   *
   * Safe to call from teardown before waiting for admitted media starts.
   */
  void cancel_pending_requests(const void *owner_tag = nullptr);

  uint32_t capture_type_for_stream_display(bool headless_mode, bool use_cage_compositor,
                                           std::string_view stream_mode = {});

  /**
   * @brief CreateSession → SelectSources → Start → OpenPipeWireRemote.
   */
  std::unique_ptr<portal_session_t> create_portal_session(uint32_t capture_type = portal_source_window);

  /**
   * @brief OpenPipeWireRemote on an existing ScreenCast session handle.
   * @return Owned fd, or -1 on failure.
   */
  int open_pipewire_remote_fd(
    GDBusConnection *conn,
    const std::string &session_handle
  );

#if defined(POLARIS_TESTS)
  uint32_t portal_pick_cursor_mode_for_tests(uint32_t available);
  bool portal_cancel_pending_request_for_tests();
  bool portal_cancel_request_owner_for_tests();
  bool portal_cancel_source_wakes_wait_for_tests();
#endif

  /**
   * @brief Drop the session media cache (ScreenCast + PipeWire capture).
   *
   * Sole public release entry (S4). Implemented in portal_grab.cpp.
   * Idempotent when cache is empty. Prefer session_media::prepare_for_stop()
   * for ordered teardown; concurrent callers are safe (no double free).
   */
  void release_global_capture();

  /**
   * @brief Process-exit / deinit path: release portal capture before other Linux teardown.
   * Same cache as release_global_capture(); exists so platf::init deinit can call it.
   */
  void shutdown();

}  // namespace portal

#endif  // __linux__
