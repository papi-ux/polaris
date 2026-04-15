/**
 * @file src/platform/linux/cage_display_router.cpp
 * @brief Cage lifecycle manager — cage as a windowed Wayland compositor.
 *
 * Spawns cage as a regular Wayland window on the user's KDE desktop (DP-3).
 * Games render inside cage at the client's requested resolution. No display
 * switching, no HDMI-A-1, no kscreen-doctor, no KWin routing scripts.
 *
 * Key environment variables set for cage:
 *   WLR_BACKENDS=wayland    — run as a Wayland client window (not DRM)
 *   WLR_RENDERER=vulkan     — best NVIDIA support
 *   WAYLAND_DISPLAY=<sock>  — cage's own socket name for its children
 *
 * Resolution is set via wlr-randr after cage starts (--custom-mode).
 */

#ifdef __linux__

#include "cage_display_router.h"
#include "../../logging.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <functional>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <signal.h>
#include <sys/stat.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace std::literals;

namespace cage_display_router {

  static pid_t cage_pid = 0;
  static std::string cage_wayland_socket;  // e.g., "wayland-5"
  static std::atomic_bool headless_ram_capture_warning_logged {false};
  static std::atomic_bool windowed_ram_capture_warning_logged {false};
  static platf::runtime_state_t cage_runtime_state {
    .requested_headless = false,
    .effective_headless = false,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  // -----------------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------------

  static std::string exec_capture(const std::string &cmd) {
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

  static std::string runtime_dir() {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    return xdg ? xdg : "/run/user/1000";
  }

  static std::string socket_path(const std::string &socket_name) {
    return runtime_dir() + "/" + socket_name;
  }

  template<typename Predicate>
  static bool wait_for_condition(
    std::string_view label,
    std::chrono::milliseconds timeout,
    std::chrono::milliseconds poll_interval,
    Predicate &&predicate
  ) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) {
        return true;
      }

      std::this_thread::sleep_for(poll_interval);
    }

    BOOST_LOG(debug) << "labwc: Timed out waiting for "sv << label;
    return false;
  }

  /**
   * @brief Snapshot current Wayland sockets in XDG_RUNTIME_DIR.
   */
  static std::set<std::string> snapshot_wayland_sockets() {
    std::set<std::string> sockets;
    auto runtime = runtime_dir();
    for (int n = 0; n < 50; ++n) {
      std::string name = "wayland-" + std::to_string(n);
      std::string path = runtime + "/" + name;
      if (access(path.c_str(), F_OK) == 0) {
        sockets.insert(name);
      }
    }
    return sockets;
  }

  /**
   * @brief Find which new Wayland socket appeared after cage started.
   */
  static std::string find_new_socket(const std::set<std::string> &before, int max_wait_ms = 5000) {
    const auto poll_interval = 50ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (cage_pid > 0 && kill(cage_pid, 0) != 0) {
        return "";
      }

      auto current = snapshot_wayland_sockets();
      for (auto &s : current) {
        if (before.find(s) == before.end()) {
          return s;  // This socket is new
        }
      }
      std::this_thread::sleep_for(poll_interval);
    }
    return "";
  }

  /**
   * @brief Returns true once the expected output becomes visible on labwc's socket.
   */
  static bool wait_for_output(const std::string &socket_name, const std::string &output_name, int max_wait_ms = 3000) {
    return wait_for_condition(
      "labwc output",
      std::chrono::milliseconds(max_wait_ms),
      50ms,
      [&]() {
        auto outputs = exec_capture("WAYLAND_DISPLAY=" + socket_name + " wlr-randr 2>/dev/null");
        return outputs.find(output_name) != std::string::npos;
      }
    );
  }

  static bool output_reports_current_mode(
    std::string_view wlr_randr_output,
    std::string_view output_name,
    int width,
    int height
  ) {
    const std::string mode = std::to_string(width) + "x" + std::to_string(height);
    std::istringstream stream(std::string {wlr_randr_output});
    std::string line;
    bool in_target_output = false;

    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.empty()) {
        continue;
      }

      if (!std::isspace(static_cast<unsigned char>(line.front()))) {
        in_target_output = line.rfind(std::string {output_name}, 0) == 0;
        continue;
      }

      if (!in_target_output) {
        continue;
      }

      if (line.find(mode) != std::string::npos &&
          (line.find("current") != std::string::npos || line.find('*') != std::string::npos)) {
        return true;
      }
    }

    return false;
  }

  static bool wait_for_requested_mode(
    const std::string &socket_name,
    const std::string &output_name,
    int width,
    int height,
    int max_wait_ms = 5000
  ) {
    return wait_for_condition(
      "labwc output mode",
      std::chrono::milliseconds(max_wait_ms),
      50ms,
      [&]() {
        auto outputs = exec_capture("WAYLAND_DISPLAY=" + socket_name + " wlr-randr 2>/dev/null");
        return output_reports_current_mode(outputs, output_name, width, height);
      }
    );
  }

  bool windowed_gpu_native_capture_supported() {
    // wlgrab currently forces SHM for windowed labwc because nested DMA-BUF
    // screencopy is not reliable on this stack.
    return false;
  }

  bool should_force_windowed_for_gpu_native_capture(
    bool requested_headless,
    bool prefer_gpu_native_capture,
    bool encoder_requires_gpu_native_capture
  ) {
    return requested_headless &&
           prefer_gpu_native_capture &&
           encoder_requires_gpu_native_capture &&
           windowed_gpu_native_capture_supported();
  }

  bool should_report_headless_ram_capture_fallback(const platf::runtime_state_t &runtime_state) {
    return runtime_state.effective_headless ||
           (runtime_state.requested_headless && !runtime_state.gpu_native_override_active);
  }

  bool should_report_windowed_ram_capture_fallback(const platf::runtime_state_t &runtime_state) {
    return !should_report_headless_ram_capture_fallback(runtime_state);
  }

  bool should_log_headless_ram_capture_warning() {
    return !headless_ram_capture_warning_logged.exchange(true);
  }

  bool should_log_windowed_ram_capture_warning() {
    return !windowed_ram_capture_warning_logged.exchange(true);
  }

#ifdef POLARIS_TESTS
  void reset_windowed_ram_capture_warning_for_tests() {
    headless_ram_capture_warning_logged.store(false);
    windowed_ram_capture_warning_logged.store(false);
  }

  bool output_reports_current_mode_for_tests(
    std::string_view wlr_randr_output,
    std::string_view output_name,
    int width,
    int height
  ) {
    return output_reports_current_mode(wlr_randr_output, output_name, width, height);
  }
#endif

  // -----------------------------------------------------------------------
  // Public API
  // -----------------------------------------------------------------------

  bool start(int width, int height, const std::string &game_cmd, bool force_windowed) {
    const auto startup_begin = std::chrono::steady_clock::now();

    if (cage_pid > 0 && is_running()) {
      BOOST_LOG(warning) << "labwc: Already running (pid="sv << cage_pid << "), skipping start"sv;
      return true;
    }

    // Reset stale state
    cage_pid = 0;
    cage_wayland_socket.clear();

    bool requested_headless = config::video.linux_display.headless_mode;
    bool headless = requested_headless && !force_windowed;
    bool cage_enabled = config::video.linux_display.use_cage_compositor;
    cage_runtime_state.requested_headless = requested_headless;
    cage_runtime_state.effective_headless = headless;
    cage_runtime_state.gpu_native_override_active = requested_headless && force_windowed;
    cage_runtime_state.backend_name = "labwc";

    BOOST_LOG(info) << "labwc: requested_headless=" << requested_headless
                    << " effective_headless=" << headless
                    << " use_cage=" << cage_enabled;
    if (requested_headless && force_windowed) {
      BOOST_LOG(warning) << "labwc: Headless mode overridden to windowed mode to preserve GPU-native capture"sv;
    }
    BOOST_LOG(info) << "labwc: Starting in "sv << (headless ? "headless"sv : "windowed"sv)
                    << " mode — resolution="sv << width << "x"sv << height;

    // Config directory for kiosk-mode rc.xml (no decorations, maximize all)
    std::string config_dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.config/labwc-polaris";
    std::string mode = std::to_string(width) + "x" + std::to_string(height);

    // Build startup command: set resolution then run the game
    // In headless mode, the output name is HEADLESS-1 instead of WL-1
    std::string output_name = headless ? "HEADLESS-1" : "WL-1";
    std::string startup_cmd;
    // MangoHud env will be re-injected into the game command (not labwc itself)
    std::string mangohud_prefix;
    {
      const char *mh = getenv("MANGOHUD");
      if (mh && std::string(mh) == "1") {
        mangohud_prefix = "MANGOHUD=1 MANGOHUD_DLSYM=1 ";
      }
    }
    if (!game_cmd.empty()) {
      auto mode_retry_cmd =
        "for i in $(seq 1 50); do "
        "wlr-randr --output " + output_name + " --custom-mode " + mode + " >/dev/null 2>&1 && break; "
        "sleep 0.1; "
        "done; ";
      if (headless) {
        // In headless mode: set resolution, ensure XWayland is ready, then launch game.
        // labwc with xwaylandPersistence=yes starts XWayland eagerly, but we still
        // need to wait for DISPLAY to be available before launching Steam.
        startup_cmd = mode_retry_cmd +
          "for i in $(seq 1 50); do xdpyinfo >/dev/null 2>&1 && break; sleep 0.1; done; "
          "exec " + mangohud_prefix + game_cmd;
      } else {
        startup_cmd = mode_retry_cmd + "exec " + mangohud_prefix + game_cmd;
      }
    } else {
      startup_cmd =
        "for i in $(seq 1 50); do "
        "wlr-randr --output " + output_name + " --custom-mode " + mode + " >/dev/null 2>&1 && break; "
        "sleep 0.1; "
        "done; "
        "exec sleep infinity";
    }

    // Snapshot existing sockets to detect the new one labwc creates
    auto sockets_before = snapshot_wayland_sockets();

    // Save and clear MangoHud env vars before fork — MangoHud crashes labwc
    // if injected into the compositor. Will be re-set for the game via startup cmd.
    std::string saved_mangohud;
    std::string saved_mangohud_dlsym;
    const char *mh = getenv("MANGOHUD");
    const char *mhd = getenv("MANGOHUD_DLSYM");
    if (mh) { saved_mangohud = mh; unsetenv("MANGOHUD"); }
    if (mhd) { saved_mangohud_dlsym = mhd; unsetenv("MANGOHUD_DLSYM"); }

    pid_t pid = fork();
    if (pid == 0) {
      // Child: set wlroots environment, detach, exec labwc
      if (headless) {
        // Headless mode: no visible window on desktop, no parent display needed.
        // labwc creates virtual outputs that still support wlr-screencopy.
        //
        // Known limitations on NVIDIA + wlroots 0.19:
        // - Vulkan renderer crashes (vulkan_instance_destroy SEGV)
        // - GLES2/pixman renderers work but screencopy returns SHM/ARGB frames
        //   which CUDA/NVENC can't consume directly (needs NV12)
        // - Requires SHM→NV12 conversion in the capture pipeline (future work)
        //
        // For now: use headless with GLES2, the SHM capture path in wlgrab
        // handles the conversion via software scaler (slower but functional).
        setenv("WLR_BACKENDS", "headless", 1);
        setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
        setenv("WLR_RENDERER", "gles2", 1);
        // Clear inherited display vars so children ONLY talk to labwc.
        // labwc will set its own WAYLAND_DISPLAY and start XWayland (new DISPLAY).
        // Without this, Steam connects to KDE's :0 instead of labwc's XWayland.
        unsetenv("DISPLAY");
        unsetenv("WAYLAND_DISPLAY");
      } else {
        // Windowed mode: labwc runs as a Wayland window on the desktop
        setenv("WLR_BACKENDS", "wayland", 1);
        setenv("WLR_RENDERER", "vulkan", 1);
      }
      // Clear DISPLAY in ALL modes so games connect to labwc's XWayland,
      // not KDE's :0. labwc will set its own DISPLAY via XWayland.
      unsetenv("DISPLAY");
      setsid();

      // Redirect stdout/stderr
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }

      // Close inherited FDs (Polaris listening sockets, boost::asio, etc.)
      // Keep stdin(0), stdout(1), stderr(2) and close everything else up to a reasonable limit.
      int maxfd = (int)sysconf(_SC_OPEN_MAX);
      if (maxfd < 0) maxfd = 1024;
      for (int fd = 3; fd < maxfd; ++fd) {
        close(fd);
      }

      // labwc -C: config dir for rc.xml, -s: startup command (game)
      execl("/usr/bin/labwc", "labwc",
        "-C", config_dir.c_str(),
        "-s", ("bash -c '" + startup_cmd + "'").c_str(),
        nullptr);
      _exit(1);
    } else if (pid > 0) {
      cage_pid = pid;
      // Restore MangoHud in parent (was cleared to prevent labwc crash)
      if (!saved_mangohud.empty()) setenv("MANGOHUD", saved_mangohud.c_str(), 1);
      if (!saved_mangohud_dlsym.empty()) setenv("MANGOHUD_DLSYM", saved_mangohud_dlsym.c_str(), 1);
      BOOST_LOG(info) << "labwc: Spawned (pid="sv << pid << ")"sv;
    } else {
      BOOST_LOG(error) << "labwc: fork() failed"sv;
      return false;
    }

    // Discover which Wayland socket cage created (it auto-picks the next available)
    cage_wayland_socket = find_new_socket(sockets_before, 5000);
    if (cage_wayland_socket.empty()) {
      BOOST_LOG(error) << "labwc: No new Wayland socket appeared within 5s"sv;
      stop();
      return false;
    }
    BOOST_LOG(info) << "labwc: Wayland socket ready — "sv << cage_wayland_socket;

    if (!wait_for_output(cage_wayland_socket, output_name, 3000)) {
      BOOST_LOG(error) << "labwc: Output ["sv << output_name << "] did not become ready on "sv << cage_wayland_socket;
      stop();
      return false;
    }

    if (!wait_for_requested_mode(cage_wayland_socket, output_name, width, height, 5000)) {
      BOOST_LOG(warning) << "labwc: Output ["sv << output_name << "] did not settle to "
                         << width << "x"sv << height
                         << " before startup continued"sv;
    }

    BOOST_LOG(info) << "labwc: Ready — "sv
                    << (headless ? "headless compositor active, socket="sv
                                 : "window visible on desktop, socket="sv)
                    << cage_wayland_socket
                    << " startup_ms="sv
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - startup_begin
                       ).count();
    return true;
  }

  std::string wrap_cmd(const std::string &cmd) {
    if (cage_pid <= 0 || cage_wayland_socket.empty() || cmd.empty()) {
      return cmd;
    }

    // Set WAYLAND_DISPLAY so the game renders inside cage.
    // Also unset AT_SPI_BUS_ADDRESS to avoid at-spi2 interference (Fedora 43 Steam fix).
    std::string wrapped = "WAYLAND_DISPLAY=" + cage_wayland_socket +
                          " AT_SPI_BUS_ADDRESS= " + cmd;
    BOOST_LOG(info) << "labwc: Wrapping command with WAYLAND_DISPLAY="sv
                    << cage_wayland_socket;
    return wrapped;
  }

  void stop() {
    if (cage_pid <= 0) {
      return;
    }
    BOOST_LOG(info) << "labwc: Stopping (pid="sv << cage_pid << ")"sv;

    // Terminate process group so cage and its children all die
    kill(-cage_pid, SIGTERM);
    kill(cage_pid, SIGTERM);

    // Poll for exit up to 3 seconds
    for (int i = 0; i < 30; ++i) {
      pid_t ret = waitpid(cage_pid, nullptr, WNOHANG);
      if (ret > 0) break;
      if (kill(cage_pid, 0) != 0) break;  // process gone
      std::this_thread::sleep_for(100ms);
    }

    // Force kill if still alive
    if (kill(cage_pid, 0) == 0) {
      BOOST_LOG(warning) << "labwc: Did not exit gracefully, sending SIGKILL"sv;
      kill(-cage_pid, SIGKILL);
      kill(cage_pid, SIGKILL);
      waitpid(cage_pid, nullptr, WNOHANG);
    }

    BOOST_LOG(info) << "labwc: Stopped"sv;
    cage_pid = 0;
    cage_wayland_socket.clear();
    cage_runtime_state = {
      .requested_headless = false,
      .effective_headless = false,
      .gpu_native_override_active = false,
      .backend_name = "labwc",
    };
  }

  bool is_running() {
    if (cage_pid <= 0) return false;
    return kill(cage_pid, 0) == 0;
  }

  bool is_healthy() {
    if (!is_running()) return false;

    // Verify wayland socket still exists
    auto path = socket_path(cage_wayland_socket);
    return access(path.c_str(), F_OK) == 0;
  }

  pid_t get_pid() {
    return cage_pid;
  }

  std::string get_wayland_socket() {
    return cage_wayland_socket;
  }

  platf::runtime_state_t runtime_state() {
    return cage_runtime_state;
  }

}  // namespace cage_display_router

#endif  // __linux__
