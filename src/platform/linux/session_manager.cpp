/**
 * @file src/platform/linux/session_manager.cpp
 * @brief Session lifecycle management for Polaris streaming on Linux.
 *
 * With the cage-as-window architecture, session management is dramatically
 * simplified. We no longer touch display configuration — cage is just a
 * window on the desktop. The session manager handles:
 *
 *   1. Environment validation (ensure we have Wayland/D-Bus access)
 *   2. Lock screen inhibition (prevent lock during streaming)
 *   3. Lock screen detection and dismissal
 *
 * All display switching, KWin scripting, window routing, and focus management
 * code has been removed. Those problems are eliminated by running games
 * inside cage instead of on a separate physical display.
 */

#include "session_manager.h"
#include "../../logging.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace std::literals;

namespace session_manager {

  // D-Bus inhibitor cookie (0 = not inhibited)
  static uint32_t screensaver_cookie = 0;

  // Edit mode watchdog
  static std::atomic<bool> g_watchdog_running{false};
  static std::thread g_watchdog_thread;

  // -----------------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------------

  static std::string exec(const std::string &cmd) {
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buf[512];
    std::string result;
    while (fgets(buf, sizeof(buf), pipe)) result += buf;
    pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
      result.pop_back();
    return result;
  }

  static bool run(const std::string &cmd) {
    BOOST_LOG(debug) << "session_manager: "sv << cmd;
    return std::system(cmd.c_str()) == 0;
  }

  // -----------------------------------------------------------------------
  // Public API
  // -----------------------------------------------------------------------

  bool validate_environment() {
    bool ok = true;

    const char *vars[] = {"WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"};
    for (auto var : vars) {
      const char *val = getenv(var);
      if (!val || val[0] == '\0') {
        BOOST_LOG(error) << "session_manager: Required environment variable "sv
                         << var << " is not set. "
                         << "Polaris requires a graphical session. "
                         << "Run via systemctl --user or from a desktop terminal."sv;
        ok = false;
      } else {
        BOOST_LOG(debug) << "session_manager: "sv << var << "="sv << val;
      }
    }

    return ok;
  }

  desktop_state_t save_state() {
    desktop_state_t state;

    if (!validate_environment()) {
      BOOST_LOG(warning) << "session_manager: Environment validation failed, session may not work correctly"sv;
    }

    state.valid = true;
    BOOST_LOG(info) << "session_manager: Session state saved (cage-as-window mode, no display config to save)"sv;
    return state;
  }

  static void dismiss_edit_mode() {
    run("dbus-send --session --type=method_call --dest=org.kde.plasmashell "
        "/PlasmaShell org.freedesktop.DBus.Properties.Set "
        "string:'org.kde.PlasmaShell' string:'editMode' variant:boolean:false 2>/dev/null");
  }

  static bool is_edit_mode_active() {
    auto result = exec(
      "dbus-send --session --print-reply --dest=org.kde.plasmashell "
      "/PlasmaShell org.freedesktop.DBus.Properties.Get "
      "string:'org.kde.PlasmaShell' string:'editMode' 2>/dev/null "
      "| grep -o 'true\\|false'");
    return result == "true";
  }

  void start_edit_mode_watchdog() {
    if (g_watchdog_running.exchange(true)) return;  // already running

    g_watchdog_thread = std::thread([]() {
      BOOST_LOG(info) << "session_manager: Edit mode watchdog started"sv;
      while (g_watchdog_running) {
        if (is_edit_mode_active()) {
          BOOST_LOG(info) << "session_manager: Watchdog dismissed KDE edit mode"sv;
          dismiss_edit_mode();
        }
        std::this_thread::sleep_for(500ms);
      }
      // Final cleanup after session ends — KDE may enter edit mode on labwc close
      for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(500ms);
        if (is_edit_mode_active()) {
          BOOST_LOG(info) << "session_manager: Post-session watchdog dismissed edit mode"sv;
          dismiss_edit_mode();
        }
      }
      BOOST_LOG(info) << "session_manager: Edit mode watchdog stopped"sv;
    });
  }

  void stop_edit_mode_watchdog() {
    g_watchdog_running = false;
    if (g_watchdog_thread.joinable()) {
      g_watchdog_thread.detach();  // let post-session cleanup finish in background
    }
  }

  void restore_state(const desktop_state_t &state) {
    if (!state.valid) {
      BOOST_LOG(warning) << "session_manager: No valid state to restore"sv;
      return;
    }

    // Release lock inhibitor if we held one
    if (state.lock_inhibited) {
      release_lock();
    }

    // Stop the edit mode watchdog (it will do post-session cleanup on its own)
    stop_edit_mode_watchdog();

    BOOST_LOG(info) << "session_manager: Session state restored"sv;
  }

  bool inhibit_lock() {
    // Use dbus-send to call org.freedesktop.ScreenSaver.Inhibit
    // This prevents the screen saver and lock screen from activating
    auto result = exec(
      "dbus-send --session --print-reply --dest=org.freedesktop.ScreenSaver "
      "/org/freedesktop/ScreenSaver org.freedesktop.ScreenSaver.Inhibit "
      "string:'polaris' string:'Game streaming active' 2>/dev/null "
      "| grep uint32 | awk '{print $2}'"
    );

    if (!result.empty()) {
      try {
        screensaver_cookie = static_cast<uint32_t>(std::stoul(result));
        BOOST_LOG(info) << "session_manager: Lock screen inhibited (cookie="sv
                        << screensaver_cookie << ")"sv;
        return true;
      } catch (...) {}
    }

    BOOST_LOG(warning) << "session_manager: Failed to inhibit lock screen via D-Bus"sv;

    // Fallback: use systemd-inhibit style via loginctl
    run("systemd-inhibit --what=idle:sleep --who=polaris "
        "--why='Game streaming active' --mode=block sleep infinity &");

    return false;
  }

  void release_lock() {
    if (screensaver_cookie > 0) {
      std::string cmd =
        "dbus-send --session --print-reply --dest=org.freedesktop.ScreenSaver "
        "/org/freedesktop/ScreenSaver org.freedesktop.ScreenSaver.UnInhibit "
        "uint32:" + std::to_string(screensaver_cookie) + " 2>/dev/null";
      run(cmd);
      BOOST_LOG(info) << "session_manager: Lock screen inhibitor released (cookie="sv
                      << screensaver_cookie << ")"sv;
      screensaver_cookie = 0;
    }
  }

  bool is_screen_locked() {
    auto result = exec(
      "dbus-send --session --print-reply --dest=org.freedesktop.ScreenSaver "
      "/org/freedesktop/ScreenSaver org.freedesktop.ScreenSaver.GetActive 2>/dev/null "
      "| grep boolean | awk '{print $2}'"
    );
    return result == "true";
  }

  bool unlock_screen() {
    if (!is_screen_locked()) {
      return true;  // Already unlocked
    }

    BOOST_LOG(info) << "session_manager: Attempting to dismiss lock screen"sv;
    bool ok = run(
      "dbus-send --session --print-reply --dest=org.freedesktop.ScreenSaver "
      "/org/freedesktop/ScreenSaver org.freedesktop.ScreenSaver.SetActive "
      "boolean:false 2>/dev/null"
    );

    if (ok) {
      BOOST_LOG(info) << "session_manager: Lock screen dismissed"sv;
    } else {
      BOOST_LOG(warning) << "session_manager: Failed to dismiss lock screen"sv;
    }
    return ok;
  }

}  // namespace session_manager
