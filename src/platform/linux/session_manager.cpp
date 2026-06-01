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
#include "../../config.h"
#include "../../logging.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#ifdef POLARIS_TESTS
  #include <functional>
#endif
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::literals;

namespace session_manager {

  // D-Bus inhibitor cookie (0 = not inhibited)
  static uint32_t screensaver_cookie = 0;

  // Edit mode watchdog
  static std::atomic<bool> g_watchdog_running{false};
  static std::thread g_watchdog_thread;

#ifdef POLARIS_TESTS
  static std::function<std::string(const std::string &)> g_exec_hook;
  static std::function<bool(const std::string &)> g_run_hook;
#endif

  // -----------------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------------

  static std::string exec(const std::string &cmd) {
#ifdef POLARIS_TESTS
    if (g_exec_hook) {
      return g_exec_hook(cmd);
    }
#endif
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
#ifdef POLARIS_TESTS
    if (g_run_hook) {
      return g_run_hook(cmd);
    }
#endif
    return std::system(cmd.c_str()) == 0;
  }

  static std::string shell_quote(std::string_view value) {
    std::string quoted = "'";
    for (const char c : value) {
      if (c == '\'') {
        quoted += "'\\''";
      } else {
        quoted += c;
      }
    }
    quoted += "'";
    return quoted;
  }

  static bool wait_for_unlock(std::chrono::milliseconds timeout) {
#ifdef POLARIS_TESTS
    if (g_exec_hook || g_run_hook) {
      return !is_screen_locked();
    }
#endif

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      if (!is_screen_locked()) {
        return true;
      }
      std::this_thread::sleep_for(100ms);
    } while (std::chrono::steady_clock::now() < deadline);

    return !is_screen_locked();
  }

  struct loginctl_session_t {
    std::string id;
    std::string user;
    std::string seat;
    std::string session_class;
    bool valid = false;

    bool is_graphical_user_session(std::string_view current_user) const {
      return valid &&
             session_class == "user" &&
             !seat.empty() &&
             seat != "-" &&
             (current_user.empty() || user == current_user);
    }
  };

  static loginctl_session_t parse_loginctl_session_line(const std::string &line) {
    loginctl_session_t session;
    std::string uid;
    std::string leader;
    std::istringstream input {line};
    if (input >> session.id >> uid >> session.user >> session.seat >> leader >> session.session_class) {
      session.valid = true;
    }
    return session;
  }

  static void append_unique_session_id(std::vector<std::string> &session_ids, const std::string &session_id) {
    if (session_id.empty()) {
      return;
    }

    for (const auto &candidate : session_ids) {
      if (candidate == session_id) {
        return;
      }
    }

    session_ids.push_back(session_id);
  }

  static std::vector<std::string> select_loginctl_sessions_for_unlock() {
    const char *env_session_id = std::getenv("XDG_SESSION_ID");
    const std::string xdg_session_id =
      (env_session_id && env_session_id[0] != '\0') ? env_session_id : "";
    const char *env_user = std::getenv("USER");
    const std::string current_user = (env_user && env_user[0] != '\0') ? env_user : "";

    auto sessions = exec("loginctl list-sessions --no-legend 2>/dev/null");
    std::istringstream lines {sessions};
    std::string line;
    std::vector<std::string> graphical_session_ids;
    bool xdg_session_is_graphical = false;

    while (std::getline(lines, line)) {
      const auto session = parse_loginctl_session_line(line);
      if (!session.is_graphical_user_session(current_user)) {
        continue;
      }

      append_unique_session_id(graphical_session_ids, session.id);
      if (!xdg_session_id.empty() && session.id == xdg_session_id) {
        xdg_session_is_graphical = true;
      }
    }

    std::vector<std::string> candidates;
    if (xdg_session_is_graphical) {
      append_unique_session_id(candidates, xdg_session_id);
    }
    for (const auto &session_id : graphical_session_ids) {
      append_unique_session_id(candidates, session_id);
    }
    if (candidates.empty()) {
      append_unique_session_id(candidates, xdg_session_id);
    }

    return candidates;
  }

  static bool run_loginctl_unlock_command(const std::string &cmd, std::string_view failure_message) {
    if (!run(cmd)) {
      BOOST_LOG(warning) << failure_message;
      return false;
    }

    return wait_for_unlock(2s);
  }

  static bool unlock_with_loginctl() {
    for (const auto &session_id : select_loginctl_sessions_for_unlock()) {
      BOOST_LOG(info) << "session_manager: Attempting loginctl unlock for session "sv << session_id;
      const std::string cmd = "loginctl unlock-session " + shell_quote(session_id) + " 2>/dev/null";
      if (run_loginctl_unlock_command(cmd, "session_manager: loginctl unlock command failed"sv)) {
        return true;
      }
    }

    BOOST_LOG(info) << "session_manager: Attempting loginctl unlock-sessions"sv;
    return run_loginctl_unlock_command(
      "loginctl unlock-sessions 2>/dev/null",
      "session_manager: loginctl unlock-sessions command failed"sv
    );
  }

  // -----------------------------------------------------------------------
  // Public API
  // -----------------------------------------------------------------------

  bool validate_environment() {
    bool ok = true;
    const bool private_headless_runtime =
      config::video.linux_display.headless_mode &&
      config::video.linux_display.use_cage_compositor;

    const char *vars[] = {"WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"};
    for (auto var : vars) {
      const char *val = getenv(var);
      if (!val || val[0] == '\0') {
        if (std::string_view {var} == "WAYLAND_DISPLAY"sv && private_headless_runtime) {
          BOOST_LOG(info) << "session_manager: WAYLAND_DISPLAY is not set; continuing because Headless Stream "
                          << "will start a private labwc socket. Desktop preview and portal capture may be "
                          << "unavailable until Polaris is launched from a desktop session."sv;
          continue;
        }

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
    const bool dbus_ok = run(
      "dbus-send --session --print-reply --dest=org.freedesktop.ScreenSaver "
      "/org/freedesktop/ScreenSaver org.freedesktop.ScreenSaver.SetActive "
      "boolean:false 2>/dev/null"
    );

    if (dbus_ok && wait_for_unlock(500ms)) {
      BOOST_LOG(info) << "session_manager: Lock screen dismissed"sv;
      return true;
    }

    if (dbus_ok) {
      BOOST_LOG(warning) << "session_manager: D-Bus unlock command completed, but lock screen is still active"sv;
    } else {
      BOOST_LOG(warning) << "session_manager: Failed to dismiss lock screen"sv;
    }

    if (unlock_with_loginctl()) {
      BOOST_LOG(info) << "session_manager: Lock screen dismissed via loginctl"sv;
      return true;
    }

    BOOST_LOG(warning) << "session_manager: Lock screen remains active after unlock attempts"sv;
    return false;
  }

#ifdef POLARIS_TESTS
  void set_command_hooks_for_tests(
    std::function<std::string(const std::string &)> exec_hook,
    std::function<bool(const std::string &)> run_hook
  ) {
    g_exec_hook = std::move(exec_hook);
    g_run_hook = std::move(run_hook);
  }

  void reset_command_hooks_for_tests() {
    g_exec_hook = nullptr;
    g_run_hook = nullptr;
  }
#endif

}  // namespace session_manager
