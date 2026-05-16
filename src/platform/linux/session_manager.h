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
   * - WAYLAND_DISPLAY (Wayland compositor access; optional for private Headless Stream)
   * - DBUS_SESSION_BUS_ADDRESS (D-Bus for lock screen, KWin)
   * - XDG_RUNTIME_DIR (Wayland sockets, PipeWire)
   *
   * @return true if the required variables for the selected runtime are present
   */
  bool validate_environment();

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
   * @brief Inhibit the screen saver and idle lock via D-Bus.
   * Prevents the lock screen from activating during an active streaming session.
   * @return true if inhibition was successful
   */
  bool inhibit_lock();

  /**
   * @brief Release the lock screen inhibitor.
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

#ifdef POLARIS_TESTS
  void set_command_hooks_for_tests(
    std::function<std::string(const std::string &)> exec_hook,
    std::function<bool(const std::string &)> run_hook
  );
  void reset_command_hooks_for_tests();
#endif

}  // namespace session_manager
