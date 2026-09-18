/**
 * @file src/platform/linux/session_manager.h
 * @brief Session lifecycle management for Polaris streaming on Linux.
 *
 * Manages the streaming session lifecycle using the cage-as-window architecture:
 * - Environment validation (Wayland, DBUS, XDG_RUNTIME_DIR)
 * - Lock screen inhibition via D-Bus during active sessions
 * - Minimal desktop state save/restore (no display switching)
 *
 * What this does NOT do (by design):
 * - No kscreen-doctor display switching (cage is just a window)
 * - No KWin window routing scripts (games run inside cage)
 * - No X11 window watchers (games render in cage's Wayland session)
 * - No focus steal prevention config (cage captures input when focused)
 */
#pragma once

#ifdef POLARIS_TESTS
  #include <functional>
  #include <sys/types.h>
#endif
#include <string>
#include <vector>

namespace session_manager {

  /**
   * @brief Minimal desktop state captured before streaming.
   * With cage-as-window, there's very little to save/restore since
   * we don't modify display configuration.
   */
  struct desktop_state_t {
    bool lock_inhibited = false;     ///< Whether we inhibited the lock screen
    bool valid = false;              ///< Whether state was successfully captured
  };

  /**
   * @brief Validate that the session environment supports streaming.
   *
   * Checks for required environment variables:
   * - WAYLAND_DISPLAY (Wayland compositor access; optional for private Private Stream)
   * - DBUS_SESSION_BUS_ADDRESS (D-Bus for lock screen, KWin)
   * - XDG_RUNTIME_DIR (Wayland sockets, PipeWire)
   *
   * @return true if the required variables for the selected runtime are present
   */
  bool validate_environment();

  /**
   * @brief Import missing desktop session variables from the user systemd manager.
   *
   * This recovers source/manual launches started from SSH or a tty while the
   * graphical user manager already knows the active Wayland/X11 session.
   * @return true when at least one environment variable was repaired
   */
  bool repair_desktop_session_environment();

  /**
   * @brief Whether Polaris repaired desktop session variables during this process lifetime.
   */
  bool desktop_session_environment_was_repaired();

  /**
   * @brief Save minimal desktop state before streaming session.
   */
  desktop_state_t save_state();

  /**
   * @brief Restore desktop state after streaming session ends.
   */
  void restore_state(const desktop_state_t &state);

  /**
   * @brief Start a background watchdog that dismisses KDE edit mode during streaming.
   * KDE Plasma enters widget edit mode whenever a wlroots window changes state
   * (close, focus loss, resize). This watchdog polls every 500ms and resets it.
   */
  void start_edit_mode_watchdog();

  /**
   * @brief Stop the edit mode watchdog.
   */
  void stop_edit_mode_watchdog();

  /**
   * @brief Inhibit the screen saver and idle lock.
   *
   * Prefers the session bus ScreenSaver.Inhibit call. Where that is unavailable
   * — headless and gamescope user sessions usually have no ScreenSaver service —
   * it falls back to a systemd-inhibit child anchored to a pipe Polaris holds
   * open, so the inhibitor is released even if Polaris is killed.
   *
   * @return true if either inhibitor was established
   */
  bool inhibit_lock();

  /**
   * @brief Release the lock screen inhibitor.
   *
   * Safe to call when no inhibitor is held.
   */
  void release_lock();

  /**
   * @brief Check if the lock screen is currently active.
   * @return true if the screen is locked
   */
  bool is_screen_locked();

  /**
   * @brief Attempt to dismiss the lock screen.
   * @return true if unlock was successful
   */
  bool unlock_screen();

  /**
   * @brief Whether logind would accept a suspend request from Polaris.
   *
   * Probed rather than inferred from the presence of systemctl. logind's
   * CanSuspend answers the three cases a client needs to tell apart: it will
   * suspend, polkit will demand interactive authentication that a remote
   * request cannot answer, or the host cannot suspend at all.
   */
  struct host_sleep_readiness_t {
    bool supported = false;   ///< A suspend request would be accepted
    std::string reason;       ///< Machine readable code when it would not be
    std::string message;      ///< Human readable detail, safe to show a client
  };

  host_sleep_readiness_t host_sleep_readiness();

  /**
   * @brief Outcome of a host suspend request.
   */
  struct host_sleep_result_t {
    bool ok = false;          ///< logind accepted the request
    std::string reason;       ///< Machine readable code when it did not
    std::string message;      ///< Human readable detail, safe to show a client
  };

  /**
   * @brief Ask logind to suspend the host without an interactive polkit prompt.
   *
   * Checks readiness first, so a host that cannot suspend fails with a reason
   * instead of a silent no-op.
   */
  host_sleep_result_t suspend_host();

#ifdef POLARIS_TESTS
  void set_command_hooks_for_tests(
    std::function<std::string(const std::string &)> exec_hook,
    std::function<bool(const std::string &)> run_hook
  );
  void reset_command_hooks_for_tests();

  /// @return true while the anchored idle inhibitor child is held.
  bool inhibitor_held_for_tests();

  /// @return pid of the anchored idle inhibitor child, or -1 when none is held.
  pid_t inhibitor_pid_for_tests();
#endif

}  // namespace session_manager
