/**
 * @file src/platform/linux/cage_display_router.h
 * @brief Cage lifecycle manager — runs cage as a windowed Wayland compositor on DP-3.
 *
 * The Polaris architecture runs cage as a regular Wayland window on the user's
 * primary display (DP-3). Games render inside cage at the client's requested
 * resolution. PipeWire window capture grabs the cage window for streaming.
 *
 * This eliminates all display switching (kscreen-doctor, HDMI-A-1 hotplug,
 * KWin layout chaos, KMS output index shifting). Cage is just a window.
 *
 * Lifecycle:
 *   start()          — spawn cage as a Wayland window, configure resolution
 *   wrap_cmd()       — produce a command that runs inside cage's Wayland session
 *   stop()           — SIGTERM cage (kills the game inside too)
 *   is_healthy()     — check cage process and wayland socket
 *
 * The caller (process.cpp) should:
 *   1. Call start() with the client's requested resolution
 *   2. Use wrap_cmd(original_cmd) for the app command
 *   3. Call stop() in the cleanup/fail_guard
 */
#pragma once

#ifdef __linux__

#include "src/platform/common.h"

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
   * @param game_cmd Optional game command to run as cage's primary client
   * @param force_windowed Force the compositor to run windowed even if headless
   *                       mode is configured. Used when GPU-native capture must
   *                       take priority over invisibility.
   * @return true if cage started successfully and wayland socket is available
   */
  bool start(int width = 1920, int height = 1080, const std::string &game_cmd = "", bool force_windowed = false);

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
   * @brief Stop cage and all processes running inside it.
   * Sends SIGTERM, waits up to 3s, then SIGKILL if needed.
   */
  void stop();

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
   * @brief Returns the PID of the running cage process (0 if not running).
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
   * @brief Returns whether a session should attempt a windowed labwc probe
   *        to validate GPU-native capture.
   */
  bool should_attempt_windowed_gpu_native_probe(
    bool requested_headless,
    bool prefer_gpu_native_capture,
    bool encoder_requires_gpu_native_capture
  );

  /**
   * @brief Returns whether the current cage runtime should attempt GPU-native
   *        capture instead of forcing SHM fallback.
   */
  bool should_attempt_gpu_native_cage_capture(const platf::runtime_state_t &runtime_state);

  /**
   * @brief Returns the cached result of the windowed GPU-native probe for the
   *        current Polaris process, if one exists.
   */
  std::optional<bool> cached_windowed_gpu_native_probe_result();

  /**
   * @brief Update the cached result of the windowed GPU-native probe for the
   *        current Polaris process.
   */
  void update_windowed_gpu_native_probe_result(bool supported);

  /**
   * @brief Returns whether RAM-capture fallback logging should describe the
   *        current labwc runtime as headless.
   */
  bool should_report_headless_ram_capture_fallback(const platf::runtime_state_t &runtime_state);

  /**
   * @brief Returns whether RAM-capture fallback logging should describe the
   *        current labwc runtime as windowed.
   */
  bool should_report_windowed_ram_capture_fallback(const platf::runtime_state_t &runtime_state);

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
    int height
  );

  /**
   * @brief Test helper for Wayland socket file name filtering.
   */
  bool is_wayland_socket_name_for_tests(std::string_view name);
#endif

}  // namespace cage_display_router

#endif  // __linux__
