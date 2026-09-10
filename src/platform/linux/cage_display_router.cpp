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

#include "process_environment.h"

#include "cage_display_router.h"
#include "../../logging.h"
#include "../../utility.h"
#include "labwc_startup_diagnostics.h"
#include "misc.h"
#include "encoder_probe_identity.h"
#include "private_session_input.h"
#include "wlgrab_capture_policy.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <fstream>
#include <memory>
#include <map>
#include <spawn.h>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <signal.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace std::literals;

namespace cage_display_router {

  static pid_t cage_pid = 0;

  // Probe readers never borrow the mutable router strings or descriptor. Each
  // successful launch publishes immutable ownership of this process generation.
  struct probe_topology_t {
    int pidfd = -1;
    int socketfd = -1;
    std::string socket_path;
    std::string socket_identity;
    std::string generation;
    ~probe_topology_t() {
      if (pidfd >= 0) close(pidfd);
      if (socketfd >= 0) close(socketfd);
    }
  };
  static std::atomic<std::shared_ptr<const probe_topology_t>> probe_topology;
  static std::atomic_uint probe_topology_writers {0};
  static std::atomic_uint64_t probe_topology_revision {0};

  struct probe_topology_mutation_t {
    probe_topology_mutation_t() {
      probe_topology_writers.fetch_add(1);
      probe_topology_revision.fetch_add(1);
    }
    ~probe_topology_mutation_t() {
      probe_topology_revision.fetch_add(1);
      probe_topology_writers.fetch_sub(1);
    }
  };

  /**
   * A pidfd for the owned labwc supervisor, opened at spawn.
   *
   * `kill(pid, 0)` cannot answer "is this runtime generation still running": it reports
   * success for a zombie, and after the pid has been reaped and recycled it
   * reports on whatever process inherited the number. A pidfd refers to the
   * supervisor generation itself, so it stays correct across both.
   */
  static int cage_pidfd = -1;

#if defined(POLARIS_TESTS)
  static bool force_cage_pidfd_open_failure = false;
#endif

  // The direct child tracked by cage_pid is a tiny supervisor and the immutable
  // leader of the private labwc session/process group. Normal descendants stay
  // inside that anchored group. The supervisor is also a child subreaper so a
  // startup client that creates its own session is adopted and drained without
  // any global process scan or recycled-PID guesswork.

  static std::string cage_wayland_socket;  // e.g., "wayland-5"
  static std::string cage_x11_display;  // e.g., ":1"
  static std::string cage_session_instance_id;
  static std::mutex startup_diagnostics_mutex;
  static std::shared_ptr<labwc_startup_diagnostics::collector_t> startup_diagnostics;

  // The output mode most recently requested of the running compositor. A
  // resume can carry a different refresh than the launch that started the
  // cage; without re-applying it the output stays at the old rate for the
  // whole session (issue #367: a 120 FPS client resuming a 60 Hz session
  // could never be served more than 60). Written by the launch thread,
  // read by the nvhttp resume handler — atomics, and zeroed until startup
  // has settled so a resume racing a launch reports "not applied" instead
  // of silently succeeding.
  static std::atomic<int> cage_mode_width {0};
  static std::atomic<int> cage_mode_height {0};
  static std::atomic<int> cage_mode_refresh_hz {0};
  // Non-zero when the launch deliberately ran below the client's request
  // (optimizer/runtime policy clamp): a resume must not out-vote that
  // decision by re-applying the raw request.
  static std::atomic<int> cage_mode_refresh_ceiling_hz {0};
  static std::atomic_bool headless_ram_capture_warning_logged {false};
  static std::atomic_bool windowed_ram_capture_warning_logged {false};

  // 0 = unknown, 1 = false, 2 = true (shared by windowed + headless probe caches).
  struct probe_cache_t {
    std::atomic<int> value {0};

    std::optional<bool> get() const {
      switch (value.load()) {
        case 1:
          return false;
        case 2:
          return true;
        default:
          return std::nullopt;
      }
    }

    void set(bool supported) {
      value.store(supported ? 2 : 1);
    }

    void reset() {
      value.store(0);
    }
  };

  static probe_cache_t windowed_gpu_native_probe;
  static probe_cache_t headless_extcopy_dmabuf_probe;
  static platf::runtime_state_t cage_runtime_state {
    .requested_headless = false,
    .effective_headless = false,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  // -----------------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------------

  static void close_descriptor(int &fd) {
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }

  static void publish_startup_diagnostics(
    std::shared_ptr<labwc_startup_diagnostics::collector_t> collector
  ) {
    const std::lock_guard lock {startup_diagnostics_mutex};
    startup_diagnostics = std::move(collector);
  }

  static void clear_startup_diagnostics() {
    publish_startup_diagnostics(nullptr);
  }

  /// Open a pidfd for the freshly spawned compositor, if the kernel allows it.
  static void open_cage_pidfd(pid_t pid) {
    if (cage_pidfd >= 0) {
      close(cage_pidfd);
      cage_pidfd = -1;
    }

#if defined(POLARIS_TESTS)
    if (force_cage_pidfd_open_failure) {
      return;
    }
#endif

    cage_pidfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    if (cage_pidfd < 0) {
      BOOST_LOG(warning) << "labwc: pidfd_open failed for pid ["sv << pid << "]: "sv << strerror(errno)
                         << "; falling back to pid-based liveness checks"sv;
    }
  }

  static void close_cage_pidfd() {
    if (cage_pidfd >= 0) {
      close(cage_pidfd);
      cage_pidfd = -1;
    }
  }

  /**
   * @brief Whether the compositor process is still alive.
   *
   * A pidfd becomes readable when the process it refers to exits, so POLLIN here
   * means "already gone" and is immune to pid reuse. Without a pidfd this falls
   * back to signal 0, which is all the older path ever had.
   */
  static bool cage_process_alive() {
    if (cage_pid <= 0) {
      return false;
    }

    if (cage_pidfd < 0) {
      return kill(cage_pid, 0) == 0;
    }

    pollfd descriptor {cage_pidfd, POLLIN, 0};
    while (true) {
      const auto ready = poll(&descriptor, 1, 0);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        // An unusable pidfd should not be read as "the supervisor is gone":
        // teardown must keep waiting rather than discard a live generation.
        return kill(cage_pid, 0) == 0;
      }
      return ready == 0;
    }
  }

  static bool signal_cage_supervisor(int signal_number) {
    if (cage_pid <= 0) {
      return false;
    }

    if (cage_pidfd >= 0) {
      if (syscall(SYS_pidfd_send_signal, cage_pidfd, signal_number, nullptr, 0) == 0 ||
          errno == ESRCH) {
        return true;
      }
      BOOST_LOG(warning) << "labwc: pidfd signal failed for supervisor pid ["sv
                         << cage_pid << "]: "sv << strerror(errno);
      return false;
    }

    return kill(cage_pid, signal_number) == 0 || errno == ESRCH;
  }

  static bool force_kill_cage_group() {
    // Freeze the pidfd-bound group leader before addressing -cage_pid. While the
    // supervisor is stopped and unreaped, its PGID cannot be recycled between
    // the ownership check and the group SIGKILL.
    if (!signal_cage_supervisor(SIGSTOP)) {
      return false;
    }
    if (!cage_process_alive()) {
      return true;
    }

    const bool group_signalled = kill(-cage_pid, SIGKILL) == 0 || errno == ESRCH;
    const bool supervisor_signalled = signal_cage_supervisor(SIGKILL);
    return group_signalled && supervisor_signalled;
  }

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
    return (xdg && *xdg) ? xdg : "/run/user/" + std::to_string(getuid());
  }

  static std::string socket_path(const std::string &socket_name) {
    return runtime_dir() + "/" + socket_name;
  }

  std::string trimmed(std::string_view value) {
    auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
      return {};
    }

    auto end = value.find_last_not_of(" \t\r\n");
    return std::string {value.substr(begin, end - begin + 1)};
  }

  std::string lowercase_trimmed(std::string_view value) {
    std::string result = trimmed(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return result;
  }

  bool command_targets_steam_big_picture(std::string_view cmd) {
    const auto normalized = lowercase_trimmed(cmd);
    if (normalized.empty()) {
      return false;
    }

    if (normalized.find("steam://open/bigpicture") != std::string::npos ||
        normalized.find("steam://close/bigpicture") != std::string::npos) {
      return true;
    }

    return normalized.find("steam -gamepadui") != std::string::npos &&
           normalized.find("-applaunch") == std::string::npos &&
           normalized.find("steam://rungameid/") == std::string::npos;
  }

  std::string mangohud_prefix_for_command(std::string_view game_cmd,
                                          bool allow_mangohud,
                                          std::string_view mangohud_value,
                                          std::string_view mangohud_config) {
    if (!allow_mangohud || command_targets_steam_big_picture(game_cmd)) {
      return {};
    }

    if (lowercase_trimmed(mangohud_value) != "1") {
      return {};
    }

    std::string prefix = "MANGOHUD=1 MANGOHUD_DLSYM=1 ";
    const auto config = trimmed(mangohud_config);
    if (!config.empty()) {
      prefix += "MANGOHUD_CONFIG=" + config + " ";
    }
    return prefix;
  }

  static std::string labwc_process_environment_value(bool headless, std::string_view key) {
    if (key == "WLR_NO_HARDWARE_CURSORS") {
      // Polaris captures the private labwc compositor, not the host desktop. On
      // some wlroots/labwc stacks hardware cursors remain interactive but never
      // appear in captured frames, so force wlroots to paint software cursors.
      return "1";
    }

    if (key == "WLR_BACKENDS") {
      return headless ? "headless" : "wayland";
    }

    if (key == "WLR_RENDERER") {
      // Both cages use the vulkan renderer. Headless was formerly pinned to gles2 to
      // dodge a wlroots-0.19 vulkan_instance_destroy teardown SEGV; that crash no longer
      // reproduces on 0.20.2 (retested 2026-08-10). vulkan is also the renderer whose
      // ext-image-copy DMA-BUF the CUDA/NVENC import path can consume, which unblocks
      // true-headless GPU-native capture -- the gles2 headless frame could not be
      // imported, which is what forced the visible windowed-cage fallback.
      return "vulkan";
    }

    if (headless && key == "WLR_HEADLESS_OUTPUTS") {
      return "1";
    }

    if (headless && key == "WLR_RENDER_DRM_DEVICE") {
      // Headless-only, deliberately. On the headless backend wlroots owns DRM
      // device selection outright and, left to itself, picks the first render
      // node it enumerates — which on a multi-GPU host is not necessarily the
      // one the operator configured via adapter_name, so the private compositor
      // renders on (and the stream is captured from) the wrong card (issue #354).
      // Pin it to adapter_name, the same render node used for capture/encode.
      //
      // The windowed (wayland-backend) path is left alone on purpose: there labwc
      // is a client of the host compositor and negotiates its device from the
      // parent's dmabuf feedback, so forcing a device could mismatch the parent
      // and break buffer sharing on a multi-GPU host.
      //
      // Only a real, accessible /dev/dri device is meaningful:
      // WLR_RENDER_DRM_DEVICE makes wlroots fail rather than fall back if the
      // path is bogus — and a node the Polaris user cannot open (no render
      // group membership) is exactly as fatal as a missing one, so the check
      // is R_OK|W_OK, not mere existence.
      const auto adapter = trimmed(config::video.adapter_name);
      if (adapter.rfind("/dev/dri/", 0) == 0 && access(adapter.c_str(), R_OK | W_OK) == 0) {
        return adapter;
      }
      // No usable adapter_name: pin to the shared default-device choice instead
      // of letting wlroots auto-pick. The encoder's VAAPI fallback resolves the
      // same default, so headless capture and encode agree on multi-GPU hosts
      // where "first node enumerated" and "first node numbered" differ (issue
      // #367: compositor on the APU, encoder on the dGPU, every frame through
      // a CPU copy).
      const auto fallback = platf::default_render_device();
      if (!fallback.empty() && access(fallback.c_str(), R_OK | W_OK) == 0) {
        return fallback;
      }
      return {};
    }

    return {};
  }

  static std::vector<std::string> labwc_process_environment(bool headless, const std::string &session_instance_id) {
    auto environment = process_environment::snapshot();
    for (const auto key : {"MANGOHUD", "MANGOHUD_DLSYM", "MANGOHUD_CONFIG", "DISPLAY"}) environment.erase(key);
    if (headless) environment.erase("WAYLAND_DISPLAY");
    if (session_instance_id.empty()) environment.erase("POLARIS_SESSION_INSTANCE_ID");
    else environment["POLARIS_SESSION_INSTANCE_ID"] = session_instance_id;
    environment["POLARIS_PRIVATE_SESSION"] = "1";
    const auto adapter = trimmed(config::video.adapter_name);
    for (std::string_view key : {
           "WLR_NO_HARDWARE_CURSORS"sv,
           "WLR_BACKENDS"sv,
           "WLR_RENDERER"sv,
           "WLR_HEADLESS_OUTPUTS"sv,
           "WLR_RENDER_DRM_DEVICE"sv,
         }) {
      const auto value = labwc_process_environment_value(headless, key);
      if (!value.empty()) {
        environment[std::string {key}] = value;
        if (key == "WLR_RENDER_DRM_DEVICE") {
          BOOST_LOG(info) << "labwc: pinning wlroots render device to ["sv << value
                          << "] ("sv
                          << (value == adapter ? "configured adapter_name"sv : "auto-selected default render device"sv)
                          << ')';
        }
      }
    }

    // If a headless session has adapter_name set but it could not be used as a
    // render device, say so — the failure mode this fixes (issue #354) is
    // otherwise invisible: the user sets adapter_name and the compositor silently
    // lands on a different card. Only meaningful for headless, since the windowed
    // path intentionally leaves device selection to the parent compositor.
    if (headless && !adapter.empty()) {
      const auto pinned = labwc_process_environment_value(headless, "WLR_RENDER_DRM_DEVICE");
      if (pinned != adapter) {
        BOOST_LOG(warning) << "labwc: adapter_name ["sv << adapter
                           << "] is not a present /dev/dri render-device path; "sv
                           << (pinned.empty() ?
                                 "wlroots will auto-select a GPU for the private compositor"s :
                                 "pinning the private compositor to the default render device ["s + pinned + "] instead"s);
      }
    }
    std::vector<std::string> result;
    result.reserve(environment.size());
    for (const auto &[key, value] : environment) result.push_back(key + "=" + value);
    return result;
  }

  static bool executable_accessible(const std::string &path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
  }

  static std::string resolve_executable(const std::string &name) {
    if (name.find('/') != std::string::npos) {
      return executable_accessible(name) ? name : "";
    }

    const char *path_env = getenv("PATH");
    std::string path = (path_env && *path_env) ? path_env : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;

    while (start <= path.size()) {
      const auto end = path.find(':', start);
      auto dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (dir.empty()) {
        dir = ".";
      }

      const auto candidate = dir + "/" + name;
      if (executable_accessible(candidate)) {
        return candidate;
      }

      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }

    return "";
  }

  static bool is_wayland_socket_name(std::string_view name) {
    constexpr std::string_view prefix = "wayland-";
    if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix) {
      return false;
    }

    for (char ch : name.substr(prefix.size())) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        return false;
      }
    }

    return true;
  }

  static bool is_x11_socket_name(std::string_view name) {
    if (name.size() <= 1 || name.front() != 'X') {
      return false;
    }

    for (char ch : name.substr(1)) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        return false;
      }
    }

    return true;
  }

  static std::string describe_child_status(int status) {
    if (WIFEXITED(status)) {
      return "exited with status " + std::to_string(WEXITSTATUS(status));
    }

    if (WIFSIGNALED(status)) {
      return "terminated by signal " + std::to_string(WTERMSIG(status));
    }

    return "changed state before startup completed";
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

    DIR *dir = opendir(runtime.c_str());
    if (!dir) {
      BOOST_LOG(debug) << "labwc: Could not inspect runtime directory ["sv << runtime
                       << "]: "sv << std::strerror(errno);
      return sockets;
    }

    while (auto *entry = readdir(dir)) {
      std::string name = entry->d_name;
      if (!is_wayland_socket_name(name)) {
        continue;
      }

      const auto path = runtime + "/" + name;
      struct stat statbuf {};
      if (stat(path.c_str(), &statbuf) == 0 && S_ISSOCK(statbuf.st_mode)) {
        sockets.insert(std::move(name));
      }
    }

    closedir(dir);
    return sockets;
  }

  static std::set<std::string> snapshot_x11_displays() {
    std::set<std::string> displays;

    DIR *dir = opendir("/tmp/.X11-unix");
    if (!dir) {
      BOOST_LOG(debug) << "labwc: Could not inspect X11 socket directory: "sv << std::strerror(errno);
      return displays;
    }

    while (auto *entry = readdir(dir)) {
      std::string name = entry->d_name;
      if (!is_x11_socket_name(name)) {
        continue;
      }

      const auto path = "/tmp/.X11-unix/"s + name;
      struct stat statbuf {};
      if (stat(path.c_str(), &statbuf) == 0 && S_ISSOCK(statbuf.st_mode)) {
        displays.insert(":" + name.substr(1));
      }
    }

    closedir(dir);
    return displays;
  }

  /**
   * @brief Find which new Wayland socket appeared after cage started.
   */
  static std::string find_new_socket(
    const std::set<std::string> &before,
    int max_wait_ms = 10000,
    std::optional<int> *exit_status = nullptr
  ) {
    const auto poll_interval = 50ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      if (cage_pid > 0) {
        int status = 0;
        const auto ret = waitpid(cage_pid, &status, WNOHANG);
        if (ret == cage_pid) {
          if (exit_status) {
            *exit_status = status;
          }
          return "";
        }
        if (ret < 0 && errno == ECHILD) {
          return "";
        }
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

  static std::string find_new_x11_display(
    const std::set<std::string> &before,
    int max_wait_ms = 3000
  ) {
    const auto poll_interval = 50ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
      auto current = snapshot_x11_displays();
      for (auto &display : current) {
        if (before.find(display) == before.end()) {
          return display;
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

  static std::optional<double> output_current_refresh_hz(
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
        const auto hz_pos = line.find(" Hz");
        const auto separator_pos = line.find(" px,");
        if (hz_pos == std::string::npos || separator_pos == std::string::npos || separator_pos + 4 >= hz_pos) {
          continue;
        }

        auto refresh = trimmed(
          std::string_view {line}.substr(separator_pos + 4, hz_pos - separator_pos - 4)
        );
        // wlr-randr may honor a comma-decimal locale even though its field
        // separator is also a comma. Normalize only the isolated Hz field.
        std::replace(refresh.begin(), refresh.end(), ',', '.');
        return util::parse_decimal<double>(refresh);
      }
    }

    return std::nullopt;
  }

  static bool output_reports_current_mode(
    std::string_view wlr_randr_output,
    std::string_view output_name,
    int width,
    int height,
    int refresh_hz = 0
  ) {
    const auto current_refresh = output_current_refresh_hz(
      wlr_randr_output,
      output_name,
      width,
      height
    );

    if (!current_refresh) {
      return false;
    }
    return refresh_hz <= 0 || std::abs(*current_refresh - static_cast<double>(refresh_hz)) < 0.5;
  }

  static bool wait_for_requested_mode(
    const std::string &socket_name,
    const std::string &output_name,
    int width,
    int height,
    int refresh_hz,
    int max_wait_ms = 5000
  ) {
    return wait_for_condition(
      "labwc output mode",
      std::chrono::milliseconds(max_wait_ms),
      50ms,
      [&]() {
        auto outputs = exec_capture("WAYLAND_DISPLAY=" + socket_name + " wlr-randr 2>/dev/null");
        return output_reports_current_mode(outputs, output_name, width, height, refresh_hz);
      }
    );
  }

  static std::string format_wlr_custom_mode(int width, int height, int refresh_hz) {
    auto mode = std::to_string(width) + "x" + std::to_string(height);
    if (refresh_hz > 0) {
      mode += "@" + std::to_string(refresh_hz) + "Hz";
    }
    return mode;
  }

  bool gpu_native_dmabuf_is_safe(
    platf::mem_type_e hwdevice_type,
    wlgrab_capture_policy::gpu_native_capture_route_e route,
    std::optional<std::uint64_t> modifier
  ) {
    return wlgrab_capture_policy::gpu_native_dmabuf_is_safe(hwdevice_type, route, modifier);
  }

  bool should_attempt_headless_extcopy_dmabuf(
    const platf::runtime_state_t &runtime_state,
    platf::mem_type_e hwdevice_type
  ) {
    if (!runtime_state.effective_headless || runtime_state.gpu_native_override_active) {
      return false;
    }

    if (auto cached_result = headless_extcopy_dmabuf_probe.get();
        cached_result && !*cached_result) {
      return false;
    }

    return wlgrab_capture_policy::gpu_native_dmabuf_probe_is_allowed(
      hwdevice_type,
      wlgrab_capture_policy::gpu_native_capture_route_e::headless_extcopy
    );
  }

  bool should_attempt_gpu_native_cage_capture(
    const platf::runtime_state_t &runtime_state,
    platf::mem_type_e hwdevice_type
  ) {
    if (!runtime_state.gpu_native_override_active) {
      return false;
    }

    // Two independent refusals, and both are wanted. The probe result retires a
    // path that failed for this host in this process, whatever the encoder — a
    // conversion that already failed once is not worth retrying every session.
    // The memory-type policy refuses an import Polaris does not consider safe at
    // all, which is a statement about the encoder rather than about this host.
    if (auto cached_result = cached_windowed_gpu_native_probe_result();
        cached_result && !*cached_result) {
      return false;
    }

    return cage_display_router::gpu_native_dmabuf_is_safe(
      hwdevice_type,
      wlgrab_capture_policy::gpu_native_capture_route_e::windowed_nested,
      std::nullopt
    );
  }

  bool should_disable_headless_extcopy_after_conversion_failure(
    const platf::runtime_state_t &runtime_state,
    const platf::frame_metadata_t &source_metadata
  ) {
    return runtime_state.effective_headless &&
           !runtime_state.gpu_native_override_active &&
           source_metadata.transport == platf::frame_transport_e::dmabuf &&
           source_metadata.residency == platf::frame_residency_e::gpu;
  }

  bool should_disable_windowed_gpu_native_after_conversion_failure(
    const platf::runtime_state_t &runtime_state,
    const platf::frame_metadata_t &source_metadata
  ) {
    return runtime_state.gpu_native_override_active &&
           source_metadata.transport == platf::frame_transport_e::dmabuf &&
           source_metadata.residency == platf::frame_residency_e::gpu;
  }

  bool should_disable_headless_extcopy_after_initial_conversion_failure(
    const platf::runtime_state_t &runtime_state,
    std::optional<bool> cached_extcopy_dmabuf_probe_result
  ) {
    return runtime_state.effective_headless &&
           !runtime_state.gpu_native_override_active &&
           cached_extcopy_dmabuf_probe_result == std::optional<bool> {true};
  }

  std::optional<bool> cached_windowed_gpu_native_probe_result() {
    return windowed_gpu_native_probe.get();
  }

  std::optional<bool> cached_headless_extcopy_dmabuf_probe_result() {
    return headless_extcopy_dmabuf_probe.get();
  }

  void update_windowed_gpu_native_probe_result(bool supported) {
    windowed_gpu_native_probe.set(supported);
  }

  void update_headless_extcopy_dmabuf_probe_result(bool supported) {
    headless_extcopy_dmabuf_probe.set(supported);
  }

  bool should_report_headless_ram_capture_fallback(const platf::runtime_state_t &runtime_state) {
    return runtime_state.effective_headless ||
           (runtime_state.requested_headless && !runtime_state.gpu_native_override_active);
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
    windowed_gpu_native_probe.reset();
    headless_extcopy_dmabuf_probe.reset();
  }

  bool output_reports_current_mode_for_tests(
    std::string_view wlr_randr_output,
    std::string_view output_name,
    int width,
    int height,
    int refresh_hz
  ) {
    return output_reports_current_mode(wlr_randr_output, output_name, width, height, refresh_hz);
  }

  std::optional<double> output_current_refresh_hz_for_tests(
    std::string_view wlr_randr_output,
    std::string_view output_name,
    int width,
    int height
  ) {
    return output_current_refresh_hz(wlr_randr_output, output_name, width, height);
  }

  std::string format_wlr_custom_mode_for_tests(int width, int height, int refresh_hz) {
    return format_wlr_custom_mode(width, height, refresh_hz);
  }

  bool is_wayland_socket_name_for_tests(std::string_view name) {
    return is_wayland_socket_name(name);
  }

  std::string mangohud_prefix_for_command_for_tests(
    std::string_view game_cmd,
    bool allow_mangohud,
    std::string_view mangohud_value,
    std::string_view mangohud_config
  ) {
    return mangohud_prefix_for_command(game_cmd, allow_mangohud, mangohud_value, mangohud_config);
  }

  std::string labwc_process_environment_value_for_tests(bool headless, std::string_view key) {
    return labwc_process_environment_value(headless, key);
  }

  void force_cage_pidfd_open_failure_for_tests(bool force_failure) {
    force_cage_pidfd_open_failure = force_failure;
  }

  bool cage_pidfd_available_for_tests() {
    return cage_pidfd >= 0;
  }
#endif

  // -----------------------------------------------------------------------
  // Public API
  // -----------------------------------------------------------------------

  bool start(
    int width,
    int height,
    int refresh_hz,
    const std::string &game_cmd,
    bool force_windowed,
    bool allow_mangohud,
    const std::string &session_instance_id,
    int requested_refresh_hz
  ) {
    const auto startup_begin = std::chrono::steady_clock::now();

    if (cage_pid > 0 && is_running()) {
      BOOST_LOG(warning) << "labwc: Already running (pid="sv << cage_pid << "), skipping start"sv;
      return true;
    }

    const probe_topology_mutation_t probe_mutation;
    probe_topology.store(nullptr);

    // Reset stale state. The pidfd of a supervisor that died without going
    // through stop() is still open here; every session would otherwise leak one.
    close_cage_pidfd();
    cage_pid = 0;
    cage_wayland_socket.clear();
    cage_x11_display.clear();
    cage_session_instance_id.clear();
    clear_startup_diagnostics();
    // The mode is unrecorded until this startup settles: a resume racing the
    // launch must see "no recorded mode" and report failure, not silently
    // claim the refresh was applied.
    cage_mode_width = 0;
    cage_mode_height = 0;
    cage_mode_refresh_hz = 0;
    cage_mode_refresh_ceiling_hz = 0;

    bool requested_headless = config::video.linux_display.headless_mode;
    bool headless = requested_headless && !force_windowed;
    bool cage_enabled = config::video.linux_display.use_cage_compositor;
    cage_runtime_state.requested_headless = requested_headless;
    cage_runtime_state.effective_headless = headless;
    cage_runtime_state.gpu_native_override_active = requested_headless && force_windowed;
    cage_runtime_state.backend_name = "labwc";
    cage_runtime_state.path_id = headless ? "headless_stream" : "windowed_stream";
    cage_runtime_state.reported_output_refresh_hz = 0.0;

    BOOST_LOG(info) << "labwc: requested_headless=" << requested_headless
                    << " effective_headless=" << headless
                    << " use_cage=" << cage_enabled;
    if (requested_headless && force_windowed) {
      BOOST_LOG(warning) << "labwc: Headless mode overridden to windowed mode to preserve GPU-native capture"sv;
    }
    BOOST_LOG(info) << "labwc: Starting in "sv << (headless ? "headless"sv : "windowed"sv)
                    << " mode — resolution="sv << width << "x"sv << height
                    << "@"sv << refresh_hz << "Hz"sv;

    const auto labwc_path = resolve_executable("labwc");
    if (labwc_path.empty()) {
      BOOST_LOG(error) << "labwc: Required executable [labwc] was not found in PATH; install labwc and restart Polaris"sv;
      return false;
    }

    const auto wlr_randr_path = resolve_executable("wlr-randr");
    if (wlr_randr_path.empty()) {
      BOOST_LOG(error) << "labwc: Required executable [wlr-randr] was not found in PATH; install wlr-randr and restart Polaris"sv;
      return false;
    }

    if (headless && !game_cmd.empty()) {
      if (resolve_executable("Xwayland").empty()) {
        BOOST_LOG(warning) << "labwc: Xwayland was not found in PATH; X11 games and Steam may fail inside the headless runtime"sv;
      }
      if (resolve_executable("xdpyinfo").empty()) {
        BOOST_LOG(warning) << "labwc: xdpyinfo was not found in PATH; Polaris cannot wait for labwc XWayland readiness"sv;
      }
    }

    BOOST_LOG(info) << "labwc: Using executable ["sv << labwc_path << ']';

    // Config directory for kiosk-mode rc.xml (no decorations, maximize all)
    std::string config_dir = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.config/labwc-polaris";

    // Generate that rc.xml before launching. Besides the kiosk presentation, it
    // tells labwc to ignore the host's physical input devices, so a private
    // session running alongside a live desktop session does not consume the
    // keyboard and mouse the user at the machine is typing on.
    {
      std::string rc_status;
      const bool generated = platf::private_session_input::ensure_generated_rc_xml(
        config_dir,
        platf::private_session_input::enumerate_host_input_devices(),
        rc_status
      );
      BOOST_LOG(generated ? info : warning) << "labwc: "sv << rc_status;
    }

    // Companion files with the same ownership contract: the root menu replaces
    // labwc's built-in Terminal + Exit fallback (the only UI a user sees on an
    // empty private session) with one that explains what this screen is, and
    // autostart paints a background when swaybg is present.
    {
      std::string menu_status;
      const bool generated_menu = platf::private_session_input::ensure_generated_menu_xml(config_dir, menu_status);
      BOOST_LOG(generated_menu ? info : warning) << "labwc: "sv << menu_status;

      std::string autostart_status;
      const bool generated_autostart = platf::private_session_input::ensure_generated_autostart(config_dir, autostart_status);
      BOOST_LOG(generated_autostart ? info : warning) << "labwc: "sv << autostart_status;
    }

    const std::string mode = format_wlr_custom_mode(width, height, refresh_hz);

    // Build startup command: set resolution then run the game
    // In headless mode, the output name is HEADLESS-1 instead of WL-1
    std::string output_name = headless ? "HEADLESS-1" : "WL-1";
    std::string startup_cmd;
    // MangoHud env will be re-injected into the game command (not labwc itself).
    // Steam Big Picture is deliberately excluded because MangoHud is unstable
    // in Steam helper processes during headless sessions.
    std::string mangohud_prefix;
    {
      const char *mh = getenv("MANGOHUD");
      const char *mhc = getenv("MANGOHUD_CONFIG");
      if (mh) {
        mangohud_prefix = mangohud_prefix_for_command(game_cmd, allow_mangohud, mh, mhc ? mhc : "");
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
          + mangohud_prefix + "exec " + game_cmd;
      } else {
        startup_cmd = mode_retry_cmd + mangohud_prefix + "exec " + game_cmd;
      }
    } else {
      startup_cmd =
        "for i in $(seq 1 50); do "
        "wlr-randr --output " + output_name + " --custom-mode " + mode + " >/dev/null 2>&1 && break; "
        "sleep 0.1; "
        "done; "
        "exec sleep infinity";
    }

    // Capture only a real startup client's stderr and eventual shell status.
    // The pipes are memory-only and continuously drained, so a noisy launcher
    // cannot block on a full pipe or leave an unbounded diagnostic file behind.
    std::array<int, 2> startup_stderr_pipe {-1, -1};
    std::array<int, 2> startup_status_pipe {-1, -1};
    std::shared_ptr<labwc_startup_diagnostics::collector_t> startup_collector;
    std::thread startup_diagnostics_reader;
    bool startup_diagnostics_enabled = false;
    if (!game_cmd.empty() && !session_instance_id.empty()) {
      int diagnostics_error = 0;
      if (pipe2(startup_stderr_pipe.data(), O_CLOEXEC) != 0) {
        diagnostics_error = errno;
      } else if (pipe2(startup_status_pipe.data(), O_CLOEXEC) != 0) {
        diagnostics_error = errno;
      } else {
        try {
          startup_collector =
            std::make_shared<labwc_startup_diagnostics::collector_t>(session_instance_id);
          startup_diagnostics_reader = std::thread {
            labwc_startup_diagnostics::drain_pipes,
            startup_stderr_pipe[0],
            startup_status_pipe[0],
            startup_collector
          };
          startup_diagnostics_enabled = true;
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "labwc: Startup-client diagnostics unavailable: "sv << e.what();
        }
      }

      if (!startup_diagnostics_enabled) {
        close_descriptor(startup_stderr_pipe[0]);
        close_descriptor(startup_stderr_pipe[1]);
        close_descriptor(startup_status_pipe[0]);
        close_descriptor(startup_status_pipe[1]);
        if (diagnostics_error != 0) {
          BOOST_LOG(warning) << "labwc: Startup-client diagnostics unavailable: "sv
                             << strerror(diagnostics_error);
        }
      }
    }

    // Snapshot existing sockets to detect the new one labwc creates
    auto sockets_before = snapshot_wayland_sockets();
    auto x11_displays_before = snapshot_x11_displays();
    BOOST_LOG(info) << "labwc: Watching runtime directory ["sv << runtime_dir()
                    << "] for a new Wayland socket"sv;

    // Prepare every allocation, GPU lookup, environment value and diagnostic
    // command in the host. The spawned image enters the supervisor before any
    // host logging, config parsing or worker threads are initialized.
    auto environment_storage = labwc_process_environment(headless, session_instance_id);
    std::vector<char *> environment;
    for (auto &entry : environment_storage) environment.push_back(entry.data());
    environment.push_back(nullptr);

    std::array<int, 2> diagnostic_copies {-1, -1};
    bool child_diagnostics_enabled = startup_diagnostics_enabled;
    if (child_diagnostics_enabled) {
      // Reserve sources above the fixed child destinations to avoid dup2 cycles.
      diagnostic_copies[0] = fcntl(startup_stderr_pipe[1], F_DUPFD_CLOEXEC, 5);
      diagnostic_copies[1] = fcntl(startup_status_pipe[1], F_DUPFD_CLOEXEC, 5);
      child_diagnostics_enabled = diagnostic_copies[0] >= 0 && diagnostic_copies[1] >= 0;
    }
    auto close_copies = util::fail_guard([&] {
      for (auto &fd : diagnostic_copies) close_descriptor(fd);
    });
    const auto plain_startup_shell = labwc_startup_diagnostics::make_labwc_startup_command(startup_cmd);
    const auto diagnostic_startup_shell = startup_diagnostics_enabled ?
      labwc_startup_diagnostics::make_labwc_startup_command(
        labwc_startup_diagnostics::instrument_shell_command(startup_cmd, 3, 4)) : plain_startup_shell;
    std::array<std::string, 5> arguments {
      "polaris", "--internal-labwc-supervisor", std::filesystem::absolute(labwc_path).string(),
      std::filesystem::absolute(config_dir).string(), child_diagnostics_enabled ? diagnostic_startup_shell : plain_startup_shell
    };
    std::array<char *, 6> argv {};
    for (std::size_t i = 0; i < arguments.size(); ++i) argv[i] = arguments[i].data();

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int spawn_error = posix_spawn_file_actions_init(&actions);
    bool actions_ready = spawn_error == 0;
    if (!spawn_error) spawn_error = posix_spawnattr_init(&attributes);
    bool attributes_ready = spawn_error == 0;
    auto release_spawn = util::fail_guard([&] {
      if (actions_ready) posix_spawn_file_actions_destroy(&actions);
      if (attributes_ready) posix_spawnattr_destroy(&attributes);
    });
    auto record_error = [&](int result) { if (!spawn_error) spawn_error = result; };
    if (!spawn_error) {
      for (int fd = 0; fd < 3; ++fd) record_error(posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0));
      if (child_diagnostics_enabled) {
        record_error(posix_spawn_file_actions_adddup2(&actions, diagnostic_copies[0], 3));
        record_error(posix_spawn_file_actions_adddup2(&actions, diagnostic_copies[1], 4));
      }
      // closefrom runs inside spawn, so concurrent host opens cannot leak a
      // descriptor between a parent-side /proc enumeration and process creation.
      record_error(posix_spawn_file_actions_addclosefrom_np(&actions, child_diagnostics_enabled ? 5 : 3));
      sigset_t empty, defaults;
      sigemptyset(&empty);
      sigemptyset(&defaults);
      for (const auto signal_number : {SIGTERM, SIGINT, SIGHUP, SIGCHLD}) sigaddset(&defaults, signal_number);
      record_error(posix_spawnattr_setsigmask(&attributes, &empty));
      record_error(posix_spawnattr_setsigdefault(&attributes, &defaults));
      record_error(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
    }
    pid_t pid = -1;
    if (!spawn_error) spawn_error = posix_spawn(&pid, "/proc/self/exe", &actions, &attributes, argv.data(), environment.data());
    for (auto &fd : diagnostic_copies) close_descriptor(fd);
    if (startup_diagnostics_enabled) {
      close_descriptor(startup_stderr_pipe[1]);
      close_descriptor(startup_status_pipe[1]);
      startup_diagnostics_reader.detach();
      if (!spawn_error) publish_startup_diagnostics(std::move(startup_collector));
    }
    if (spawn_error) {
      BOOST_LOG(error) << "labwc: supervisor spawn failed: "sv << strerror(spawn_error);
      return false;
    }
    cage_pid = pid;
    open_cage_pidfd(pid);
    BOOST_LOG(info) << "labwc: Spawned fresh supervisor (pid="sv << pid << ")"sv;

    // Discover which Wayland socket cage created (it auto-picks the next available)
    std::optional<int> labwc_exit_status;
    cage_wayland_socket = find_new_socket(sockets_before, 10000, &labwc_exit_status);
    if (cage_wayland_socket.empty()) {
      if (labwc_exit_status) {
        BOOST_LOG(error) << "labwc: Exited before creating a Wayland socket ("sv
                         << describe_child_status(*labwc_exit_status)
                         << "); check labwc, wlroots, and headless runtime dependencies"sv;
      } else {
        BOOST_LOG(error) << "labwc: No new Wayland socket appeared within 10s in ["sv
                         << runtime_dir() << ']';
      }
      stop();
      return false;
    }
    BOOST_LOG(info) << "labwc: Wayland socket ready — "sv << cage_wayland_socket;

    if (!wait_for_output(cage_wayland_socket, output_name, 3000)) {
      BOOST_LOG(error) << "labwc: Output ["sv << output_name << "] did not become ready on "sv << cage_wayland_socket;
      stop();
      return false;
    }

    if (!wait_for_requested_mode(cage_wayland_socket, output_name, width, height, refresh_hz, 5000)) {
      BOOST_LOG(warning) << "labwc: Output ["sv << output_name << "] did not settle to "
                         << width << "x"sv << height << "@"sv << refresh_hz << "Hz"sv
                         << " before startup continued"sv;
    }
    cage_mode_width = width;
    cage_mode_height = height;
    cage_mode_refresh_hz = refresh_hz;
    const auto reported_refresh = output_current_refresh_hz(
      exec_capture("WAYLAND_DISPLAY=" + cage_wayland_socket + " wlr-randr 2>/dev/null"),
      output_name,
      width,
      height
    );
    cage_runtime_state.reported_output_refresh_hz = reported_refresh.value_or(0.0);
    BOOST_LOG(reported_refresh ? info : warning) << "labwc: Output ["sv << output_name
      << "] reported_refresh_hz="sv << cage_runtime_state.reported_output_refresh_hz;
    // If the launch deliberately ran below the client's request (optimizer or
    // runtime policy clamp), record the effective rate as a ceiling so a
    // resume cannot re-apply the raw request over that decision.
    const int requested_norm = normalize_session_refresh_hz(requested_refresh_hz);
    cage_mode_refresh_ceiling_hz = (requested_norm > 0 && refresh_hz < requested_norm) ? refresh_hz : 0;

    cage_x11_display = find_new_x11_display(x11_displays_before, 3000);
    if (!cage_x11_display.empty()) {
      BOOST_LOG(info) << "labwc: XWayland display ready — "sv << cage_x11_display;
    } else {
      BOOST_LOG(debug) << "labwc: XWayland display was not observed during startup; X11 follow-up commands will rely on Wayland only"sv;
    }

    BOOST_LOG(info) << "labwc: Ready — "sv
                    << (headless ? "headless compositor active, socket="sv
                                 : "window visible on desktop, socket="sv)
                    << cage_wayland_socket
                    << " startup_ms="sv
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - startup_begin
                       ).count();
    cage_session_instance_id = session_instance_id;
    if (!session_instance_id.empty() && cage_pidfd >= 0) {
      auto topology = std::make_shared<probe_topology_t>();
      topology->pidfd = fcntl(cage_pidfd, F_DUPFD_CLOEXEC, 0);
      topology->socket_path = socket_path(cage_wayland_socket);
      topology->socketfd = open(topology->socket_path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
      const auto identity = platf::encoder_probe_identity::file_identity(topology->socket_path, S_IFSOCK);
      struct stat pinned {}, named {};
      if (topology->pidfd >= 0 && topology->socketfd >= 0 && identity &&
          fstat(topology->socketfd, &pinned) == 0 &&
          lstat(topology->socket_path.c_str(), &named) == 0 &&
          (pinned.st_mode & S_IFMT) == S_IFSOCK &&
          pinned.st_dev == named.st_dev && pinned.st_ino == named.st_ino) {
        topology->socket_identity = *identity;
        std::ostringstream generation;
        generation << std::quoted(session_instance_id) << ':' << cage_pid << ':'
                   << width << ':' << height << ':' << headless << ':' << force_windowed;
        topology->generation = generation.str();
        probe_topology.store(std::move(topology));
      }
    }
    return true;
  }

  int normalize_session_refresh_hz(int session_fps) {
    // Session FPS arrives in whole hertz from most clients, but in millihertz
    // from clients that request fractional rates (matching the launch-time
    // handling in process.cpp).
    if (session_fps >= 1000) {
      return static_cast<int>(std::lround(static_cast<double>(session_fps) / 1000.0));
    }
    return session_fps;
  }

  int resolve_resume_refresh_hz(
      int session_fps,
      int recorded_ceiling_hz,
      bool respect_recorded_ceiling) {
    const int refresh_hz = normalize_session_refresh_hz(session_fps);
    if (respect_recorded_ceiling &&
        refresh_hz > 0 && recorded_ceiling_hz > 0 &&
        refresh_hz > recorded_ceiling_hz) {
      // The launch deliberately ran below the client's request; a resume
      // carrying the legacy raw request must not out-vote that decision. An
      // exact resolved profile has already made that decision and bypasses it.
      return recorded_ceiling_hz;
    }
    return refresh_hz;
  }

  int current_output_refresh_hz() {
    return cage_mode_refresh_hz.load(std::memory_order_acquire);
  }

  bool ensure_output_refresh(
      int session_fps,
      bool respect_recorded_ceiling) {
    if (cage_pid <= 0 || cage_wayland_socket.empty() || !is_running()) {
      return false;
    }
    const int mode_width = cage_mode_width;
    const int mode_height = cage_mode_height;
    const int current_refresh_hz = cage_mode_refresh_hz;
    if (mode_width <= 0 || mode_height <= 0 || current_refresh_hz <= 0) {
      // Startup has not recorded a settled mode yet (or a start is racing this
      // resume); claiming success here would silently drop the re-apply.
      return false;
    }
    const int refresh_hz = resolve_resume_refresh_hz(
      session_fps,
      cage_mode_refresh_ceiling_hz,
      respect_recorded_ceiling
    );
    if (refresh_hz <= 0) {
      return false;
    }
    const std::string output_name = cage_runtime_state.effective_headless ? "HEADLESS-1" : "WL-1";
    if (refresh_hz == current_refresh_hz) {
      const auto reported_refresh = output_current_refresh_hz(
        exec_capture("WAYLAND_DISPLAY=" + cage_wayland_socket + " wlr-randr 2>/dev/null"),
        output_name,
        mode_width,
        mode_height
      );
      cage_runtime_state.reported_output_refresh_hz = reported_refresh.value_or(0.0);
      if (reported_refresh && std::abs(*reported_refresh - static_cast<double>(refresh_hz)) < 0.5) {
        return true;
      }
      BOOST_LOG(warning) << "labwc: Cached output refresh was "sv << current_refresh_hz
                         << "Hz but wlr-randr no longer reports that mode; re-applying"sv;
    }

    // An unchanged live read-back does not mutate capture topology. Retire
    // reuse only when an output operation is actually attempted (even if it
    // fails or returns to the prior mode).
    const probe_topology_mutation_t probe_mutation;
    // The cage outlives the launch that started it, and the mode is otherwise
    // only ever set from the startup command — a resume carrying a different
    // refresh must re-apply it or the output stays at the old rate for the
    // whole session (issue #367).
    const std::string mode = format_wlr_custom_mode(mode_width, mode_height, refresh_hz);
    exec_capture(
      "WAYLAND_DISPLAY=" + cage_wayland_socket +
      " wlr-randr --output " + output_name + " --custom-mode " + mode + " 2>&1"
    );
    if (!wait_for_requested_mode(cage_wayland_socket, output_name, mode_width, mode_height, refresh_hz, 5000)) {
      BOOST_LOG(warning) << "labwc: Output ["sv << output_name << "] did not settle to resumed mode "sv
                         << mode << "; keeping "sv << current_refresh_hz << "Hz"sv;
      return false;
    }
    BOOST_LOG(info) << "labwc: Output ["sv << output_name << "] refresh re-applied for resume — "sv
                    << mode << " (was "sv << current_refresh_hz << "Hz)"sv;
    cage_mode_refresh_hz = refresh_hz;
    cage_runtime_state.reported_output_refresh_hz = output_current_refresh_hz(
      exec_capture("WAYLAND_DISPLAY=" + cage_wayland_socket + " wlr-randr 2>/dev/null"),
      output_name,
      mode_width,
      mode_height
    ).value_or(static_cast<double>(refresh_hz));
    return true;
  }

  std::string wrap_cmd(const std::string &cmd) {
    if (cage_pid <= 0 || cage_wayland_socket.empty() || cmd.empty()) {
      return cmd;
    }

    // Set WAYLAND_DISPLAY so the game renders inside cage.
    // Also unset AT_SPI_BUS_ADDRESS to avoid at-spi2 interference (Fedora 43 Steam fix).
    std::string wrapped = "WAYLAND_DISPLAY=" + cage_wayland_socket + " ";
    if (!cage_x11_display.empty()) {
      wrapped += "DISPLAY=" + cage_x11_display + " ";
    }
    wrapped += "AT_SPI_BUS_ADDRESS= " + cmd;
    BOOST_LOG(info) << "labwc: Wrapping command with WAYLAND_DISPLAY="sv
                    << cage_wayland_socket
                    << (cage_x11_display.empty() ? ""sv : " DISPLAY="sv)
                    << cage_x11_display;
    return wrapped;
  }

  void stop() {
    const probe_topology_mutation_t probe_mutation;
    probe_topology.store(nullptr);
    if (cage_pid <= 0) {
      clear_startup_diagnostics();
      return;
    }

    // The supervisor may already have exited after observing a menu-driven
    // labwc shutdown. Its private runtime group has been drained before the
    // supervisor becomes reapable, so only cached router state remains here.
    if (!cage_process_alive()) {
      BOOST_LOG(info) << "labwc: Already exited (pid="sv << cage_pid << "); reaping session state"sv;
      (void) waitpid(cage_pid, nullptr, WNOHANG);
      close_cage_pidfd();
      cage_pid = 0;
      cage_wayland_socket.clear();
      cage_x11_display.clear();
      cage_session_instance_id.clear();
      clear_startup_diagnostics();
      cage_runtime_state = {
        .requested_headless = false,
        .effective_headless = false,
        .gpu_native_override_active = false,
        .backend_name = "labwc",
      };
      return;
    }

    BOOST_LOG(info) << "labwc: Stopping (pid="sv << cage_pid << ")"sv;

    // Signal only the pidfd-bound supervisor. It owns the private compositor
    // group and forwards graceful teardown without any raw PGID guesswork.
    (void) signal_cage_supervisor(SIGTERM);

    // Poll for exit up to 3 seconds. Once waitpid() reaps this generation (or
    // reports ECHILD because another synchronized reaper already did), never
    // consult the numeric PID again: without pidfd support it is recyclable.
    bool supervisor_reaped = false;
    for (int i = 0; i < 30; ++i) {
      const pid_t ret = waitpid(cage_pid, nullptr, WNOHANG);
      if (ret == cage_pid || (ret < 0 && errno == ECHILD)) {
        supervisor_reaped = true;
        break;
      }
      if (!cage_process_alive()) break;  // process gone
      std::this_thread::sleep_for(100ms);
    }

    // Force kill only while the original supervisor remains unreaped.
    if (!supervisor_reaped && cage_process_alive()) {
      BOOST_LOG(warning) << "labwc: Supervisor did not exit gracefully, killing its private group"sv;
      if (force_kill_cage_group()) {
        while (waitpid(cage_pid, nullptr, 0) < 0 && errno == EINTR) {
        }
      } else {
        BOOST_LOG(error) << "labwc: Could not prove ownership for forced private-group cleanup; retaining router state"sv;
        return;
      }
    }

    BOOST_LOG(info) << "labwc: Stopped"sv;
    close_cage_pidfd();
    cage_pid = 0;
    cage_wayland_socket.clear();
    cage_x11_display.clear();
    cage_session_instance_id.clear();
    clear_startup_diagnostics();
    cage_runtime_state = {
      .requested_headless = false,
      .effective_headless = false,
      .gpu_native_override_active = false,
      .backend_name = "labwc",
    };
  }

  void reset_after_external_stop() {
    const probe_topology_mutation_t probe_mutation;
    probe_topology.store(nullptr);
    if (cage_pid > 0) {
      (void) waitpid(cage_pid, nullptr, WNOHANG);
    }
    BOOST_LOG(info) << "labwc: Cleared router state after exact-generation pidfd cleanup"sv;
    close_cage_pidfd();
    cage_pid = 0;
    cage_wayland_socket.clear();
    cage_x11_display.clear();
    cage_session_instance_id.clear();
    clear_startup_diagnostics();
    cage_runtime_state = {
      .requested_headless = false,
      .effective_headless = false,
      .gpu_native_override_active = false,
      .backend_name = "labwc",
    };
  }

#if defined(POLARIS_TESTS)
  void reset_after_external_stop_for_tests(pid_t pid) {
    cage_pid = pid;
    reset_after_external_stop();
  }
#endif

  bool is_running() {
    return cage_process_alive();
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

  std::string get_session_instance_id() {
    return cage_session_instance_id;
  }

  std::optional<labwc_startup_diagnostics::snapshot_t> get_startup_client_diagnostics(
    std::string_view expected_session_instance_id
  ) {
    std::shared_ptr<labwc_startup_diagnostics::collector_t> collector;
    {
      const std::lock_guard lock {startup_diagnostics_mutex};
      collector = startup_diagnostics;
    }
    return collector ? collector->snapshot(expected_session_instance_id) : std::nullopt;
  }

  static std::optional<std::string> encoder_probe_topology_with_query(
    const std::function<std::optional<std::string>(const std::string &)> &query
  ) {
    const auto revision = probe_topology_revision.load();
    const auto snapshot = probe_topology.load();
    if (!snapshot || probe_topology_writers.load() != 0) return std::nullopt;
    struct pollfd process {snapshot->pidfd, POLLIN, 0};
    if (poll(&process, 1, 0) != 0) return std::nullopt;
    const auto socket_identity = platf::encoder_probe_identity::file_identity(snapshot->socket_path, S_IFSOCK);
    if (!socket_identity || *socket_identity != snapshot->socket_identity) return std::nullopt;

    const auto outputs = query(snapshot->socket_path);
    if (!outputs || outputs->empty() || outputs->size() > 65536) return std::nullopt;
    // An empty/disabled/ambiguous output report cannot establish capture.
    nlohmann::json parsed;
    try {
      parsed = nlohmann::json::parse(*outputs);
      if (!parsed.is_array() || parsed.empty() || parsed.size() > 16) return std::nullopt;
      unsigned enabled = 0;
      std::set<std::string> names;
      for (const auto &output : parsed) {
        const auto name = output.at("name").get<std::string>();
        if (name.empty() || !names.insert(name).second) return std::nullopt;
        if (!output.at("enabled").get<bool>()) continue;
        ++enabled;
        unsigned current = 0;
        const auto &modes = output.at("modes");
        if (!modes.is_array()) return std::nullopt;
        for (const auto &mode : modes) {
          if (!mode.at("current").get<bool>()) continue;
          ++current;
          const auto refresh = mode.at("refresh").get<double>();
          if (mode.at("width").get<int>() <= 0 || mode.at("height").get<int>() <= 0 ||
              !std::isfinite(refresh) || refresh <= 0) return std::nullopt;
        }
        if (current != 1) return std::nullopt;
      }
      if (enabled != 1) return std::nullopt;
    } catch (...) { return std::nullopt; }
    std::ostringstream key;
    key << std::quoted(snapshot->generation) << std::quoted(snapshot->socket_identity)
        << std::quoted(parsed.dump()) << ':' << revision << ':' << windowed_gpu_native_probe.value.load()
        << ':' << headless_extcopy_dmabuf_probe.value.load();
    const auto final_socket_identity = platf::encoder_probe_identity::file_identity(snapshot->socket_path, S_IFSOCK);
    if (!final_socket_identity || *final_socket_identity != snapshot->socket_identity) return std::nullopt;
    process.revents = 0;
    if (poll(&process, 1, 0) != 0 || probe_topology.load() != snapshot ||
        probe_topology_writers.load() != 0 || probe_topology_revision.load() != revision) return std::nullopt;
    return key.str();
  }

  std::optional<std::string> encoder_probe_topology() {
    return encoder_probe_topology_with_query([](const std::string &socket) -> std::optional<std::string> {
      // Query the actual mode, including changes made outside the router.
      const auto output = platf::run_process_argv_capture(
        {"/usr/bin/env", "WAYLAND_DISPLAY=" + socket, "/usr/bin/wlr-randr", "--json"},
        500ms, 65536
      );
      if (output.exit_status != 0 || output.timed_out || output.truncated) return std::nullopt;
      return output.output;
    });
  }

#ifdef POLARIS_TESTS
  std::optional<std::string> encoder_probe_topology_for_tests(
    const std::function<std::optional<std::string>(const std::string &)> &query
  ) {
    return encoder_probe_topology_with_query(query);
  }
#endif

  std::string get_x11_display() {
    return cage_x11_display;
  }

  platf::runtime_state_t runtime_state() {
    return cage_runtime_state;
  }

}  // namespace cage_display_router

#endif  // __linux__
