/**
 * @file src/platform/linux/cage_display_router.h
 * @brief labwc private compositor lifecycle (implementation detail of stream_runtime::labwc).
 *        External call sites must use stream_runtime / stream_runtime::labwc — not this header.
 */
#pragma once

#ifdef __linux__

#include "src/platform/common.h"
#include "wlgrab_capture_policy.h"

#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

namespace cage_display_router {

  /**
   * @brief Start cage as either a headless or windowed Wayland compositor.
   *
   * Launches cage with WLR_BACKENDS=wayland WLR_RENDERER=vulkan. Cage opens
   * as a regular window on the user's KDE desktop. The internal output is
   * configured to the requested resolution via wlr-randr.
   *
   * If game_cmd is provided, it runs as cage's primary client (displayed fullscreen).
   * If empty, cage runs with a placeholder and games must connect via the socket.
   *
   * @param width    Resolution width inside cage (default 1920)
   * @param height   Resolution height inside cage (default 1080)
   * @param refresh_hz Refresh rate inside cage (default 60)
   * @param game_cmd Optional game command to run as cage's primary client
   * @param force_windowed Force the compositor to run windowed even if headless
   *                       mode is configured. Used when GPU-native capture must
   *                       take priority over invisibility.
   * @param session_instance_id Immutable generation inherited only by the cage
   *                            child and its descendants; never by Polaris itself.
   * @param requested_refresh_hz The client's raw requested FPS (whole hertz or
   *                             millihertz; 0 = unknown). When refresh_hz runs
   *                             below it, the launch was deliberately clamped
   *                             and resumes are held to that ceiling.
   * @return true if cage started successfully and wayland socket is available
   */
  bool start(
    int width = 1920,
    int height = 1080,
    int refresh_hz = 60,
    const std::string &game_cmd = "",
    bool force_windowed = false,
    bool allow_mangohud = true,
    const std::string &session_instance_id = "",
    int requested_refresh_hz = 0
  );

  /**
   * @brief Normalize a session FPS value to whole hertz (millihertz-aware).
   */
  int normalize_session_refresh_hz(int session_fps);

  /**
   * @brief The refresh a resume should apply, honoring a launch-time clamp.
   *
   * @param session_fps The resuming session's FPS (whole hertz or millihertz).
   * @param recorded_ceiling_hz Non-zero when the launch deliberately ran below
   *                            the client's request; resumes stay at or under it.
   */
  int resolve_resume_refresh_hz(int session_fps, int recorded_ceiling_hz);

  /**
   * @brief Re-apply the running compositor's output refresh for a resuming session.
   *
   * The cage outlives the launch that started it and the output mode is only
   * ever set from the startup command, so a client resuming with a different
   * refresh than the original launch would otherwise stay capped at the old
   * rate for the whole session (issue #367). Keeps the launch-time resolution
   * and any launch-time refresh clamp; only an unclamped refresh follows the
   * resuming client.
   *
   * @param session_fps The resuming session's FPS (whole hertz or millihertz).
   * @return true when the mode already matches or was re-applied; false when
   *         the cage is not running, startup has not recorded a settled mode
   *         yet, or the compositor did not settle on the new mode.
   */
  bool ensure_output_refresh(int session_fps);

  /**
   * @brief Wrap a command to run inside the cage compositor.
   *
   * Sets WAYLAND_DISPLAY to cage's socket so the app renders inside cage.
   * If cage is not running, returns cmd unchanged.
   *
   * @param cmd  The original app command (e.g., "steam steam://rungameid/123")
   * @return     Wrapped command with cage's WAYLAND_DISPLAY
   */
  std::string wrap_cmd(const std::string &cmd);

  /**
   * @brief Stop the owned labwc supervisor and all private runtime processes.
   * Sends SIGTERM, waits up to 3s, then kills the anchored private group if needed.
   */
  void stop();

  /**
   * @brief Clear router state after the session owner has pidfd-terminated cage.
   *
   * This function never signals cage_pid or its process group. It only performs
   * a non-blocking reap of the original child and clears cached router state.
   */
  void reset_after_external_stop();

#if defined(POLARIS_TESTS)
  void reset_after_external_stop_for_tests(pid_t pid);
  void force_cage_pidfd_open_failure_for_tests(bool force_failure);
  bool cage_pidfd_available_for_tests();
#endif

  /**
   * @brief Returns true if cage is running and its wayland socket exists.
   */
  bool is_running();

  /**
   * @brief Returns true if cage is running and responsive.
   * Checks PID liveness and wayland socket existence.
   */
  bool is_healthy();

  /**
   * @brief Returns the PID of the owned labwc supervisor (0 if not running).
   */
  pid_t get_pid();

  /**
   * @brief Returns the Wayland socket name cage is serving on (e.g., "wayland-5").
   * Empty string if cage is not running.
   */
  std::string get_wayland_socket();

  /**
   * @brief Returns the XWayland display exposed by cage (e.g., ":1").
   * Empty string if cage has not started XWayland.
   */
  std::string get_x11_display();

  /**
   * @brief Returns the current runtime state for the labwc backend.
   */
  platf::runtime_state_t runtime_state();

  /**
   * @brief Returns whether a specific encoder, route, and modifier combination
   *        has a known-safe GPU-native DMA-BUF import/convert path.
   *
   * This is the stability policy behind every GPU-native capture route, and it
   * belongs in one place: it used to be applied only to the true-headless
   * extcopy path, so a VAAPI host that took the windowed GPU-native override
   * reached the same conversion boundary the headless path was refusing, and
   * segfaulted at stream start.
   */
  bool gpu_native_dmabuf_is_safe(
    platf::mem_type_e hwdevice_type,
    wlgrab_capture_policy::gpu_native_capture_route_e route,
    std::optional<std::uint64_t> modifier
  );

  /**
   * @brief Returns whether a headless labwc runtime should try the
   *        ext-image-copy-capture DMA-BUF path before falling back to SHM.
   *
   * VAAPI stays on the conservative SHM path even when the private headless
   * route exports a linear buffer: modifier layout alone did not prevent the
   * historical first-frame import crash from recurring on affected hardware.
   */
  bool should_attempt_headless_extcopy_dmabuf(
    const platf::runtime_state_t &runtime_state,
    platf::mem_type_e hwdevice_type
  );

  /**
   * @brief Returns whether a windowed GPU-native override should actually take
   *        the DMA-BUF capture path.
   *
   * The override exists to keep GPU-native capture when true-headless cannot
   * provide it, so it is worth nothing to an encoder whose GPU-native path is
   * unsafe: the session gives up true headless and then crashes anyway.
   */
  bool should_attempt_gpu_native_cage_capture(
    const platf::runtime_state_t &runtime_state,
    platf::mem_type_e hwdevice_type
  );

  /**
   * @brief Returns whether a live frame conversion failure should disable the
   *        true-headless ext-image-copy-capture DMA-BUF path and retry with SHM.
   */
  bool should_disable_headless_extcopy_after_conversion_failure(
    const platf::runtime_state_t &runtime_state,
    const platf::frame_metadata_t &source_metadata
  );

  /**
   * @brief Returns whether a live frame conversion failure should disable the
   *        windowed GPU-native override and retry capture through SHM.
   */
  bool should_disable_windowed_gpu_native_after_conversion_failure(
    const platf::runtime_state_t &runtime_state,
    const platf::frame_metadata_t &source_metadata
  );

  /**
   * @brief Returns the cached result of the windowed GPU-native probe for the
   *        current Polaris process, if one exists.
   */
  std::optional<bool> cached_windowed_gpu_native_probe_result();

  /**
   * @brief Returns the cached result of the headless ext-image-copy-capture
   *        DMA-BUF probe for the current Polaris process, if one exists.
   */
  std::optional<bool> cached_headless_extcopy_dmabuf_probe_result();

  /**
   * @brief Update the cached result of the windowed GPU-native probe for the
   *        current Polaris process.
   */
  void update_windowed_gpu_native_probe_result(bool supported);

  /**
   * @brief Update the cached result of the headless ext-image-copy-capture
   *        DMA-BUF probe for the current Polaris process.
   */
  void update_headless_extcopy_dmabuf_probe_result(bool supported);

  /**
   * @brief Returns whether RAM-capture fallback logging should describe the
   *        current labwc runtime as headless.
   */
  bool should_report_headless_ram_capture_fallback(const platf::runtime_state_t &runtime_state);

  /**
   * @brief Returns whether the headless labwc RAM-capture fallback warning
   *        should be emitted for this process.
   */
  bool should_log_headless_ram_capture_warning();

  /**
   * @brief Returns whether the windowed labwc RAM-capture fallback warning
   *        should be emitted for this process.
   */
  bool should_log_windowed_ram_capture_warning();

#ifdef POLARIS_TESTS
  /**
   * @brief Reset the warning dedupe state for unit tests.
   */
  void reset_windowed_ram_capture_warning_for_tests();

  /**
   * @brief Test helper for parsing wlr-randr output and determining whether
   *        an output has settled to the requested current mode.
   */
  bool output_reports_current_mode_for_tests(
    std::string_view wlr_randr_output,
    std::string_view output_name,
    int width,
    int height,
    int refresh_hz = 0
  );

  /**
   * @brief Test helper for formatting a wlr-randr custom mode string.
   */
  std::string format_wlr_custom_mode_for_tests(int width, int height, int refresh_hz);

  /**
   * @brief Test helper for Wayland socket file name filtering.
   */
  bool is_wayland_socket_name_for_tests(std::string_view name);

  /**
   * @brief Test helper for MangoHud command prefix policy.
   */
  std::string mangohud_prefix_for_command_for_tests(
    std::string_view game_cmd,
    bool allow_mangohud,
    std::string_view mangohud_value,
    std::string_view mangohud_config
  );

  /**
   * @brief Test helper for labwc process environment policy.
   */
  std::string labwc_process_environment_value_for_tests(
    bool headless,
    std::string_view key
  );
#endif

}  // namespace cage_display_router

#endif  // __linux__
