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
#include "../../utility.h"
#include "misc.h"
#include "private_session_input.h"
#include "wlgrab_capture_policy.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <dirent.h>
#include <functional>
#include <fstream>
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
  static volatile sig_atomic_t supervised_runtime_pid = 0;
  static volatile sig_atomic_t supervisor_stop_requested = 0;

  static std::string cage_wayland_socket;  // e.g., "wayland-5"
  static std::string cage_x11_display;  // e.g., ":1"
  static std::string cage_session_instance_id;

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

  static void forward_supervisor_signal(int signal_number) {
    supervisor_stop_requested = 1;
    const auto runtime_pid = static_cast<pid_t>(supervised_runtime_pid);
    if (runtime_pid > 0) {
      (void) kill(runtime_pid, signal_number);
    }
  }

  static void install_signal_handler(int signal_number, void (*handler)(int)) {
    struct sigaction action {};
    sigemptyset(&action.sa_mask);
    action.sa_handler = handler;
    action.sa_flags = 0;
    (void) sigaction(signal_number, &action, nullptr);
  }

  static void restore_default_signal_handler(int signal_number) {
    struct sigaction action {};
    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_DFL;
    action.sa_flags = 0;
    (void) sigaction(signal_number, &action, nullptr);
  }

  static void sleep_without_losing_interrupt_time(std::chrono::milliseconds duration) {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
    timespec remaining {
      .tv_sec = static_cast<time_t>(seconds.count()),
      .tv_nsec = static_cast<long>(nanoseconds.count()),
    };
    while (nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
  }

  /**
   * Keep a stable direct child alive as the private session/process-group anchor.
   *
   * labwc is free to exit from its own menu while its startup client remains
   * alive. The supervisor subreaps that runtime tree, drains the anchored group
   * plus any adopted direct children that escaped it, and exits cleanly once no
   * private descendants remain. If a descendant ignores graceful teardown, the
   * final SIGKILL path covers both ownership classes before the group includes
   * the supervisor itself.
   */
  [[noreturn]] static void supervise_labwc(
    const std::string &labwc_path,
    const std::string &config_dir,
    const std::string &startup_shell
  ) {
    supervised_runtime_pid = 0;
    supervisor_stop_requested = 0;

    if (setsid() < 0 || prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
      _exit(126);
    }

    sigset_t forwarded_signals {};
    sigset_t previous_mask {};
    sigemptyset(&forwarded_signals);
    sigaddset(&forwarded_signals, SIGTERM);
    sigaddset(&forwarded_signals, SIGINT);
    sigaddset(&forwarded_signals, SIGHUP);
    (void) sigprocmask(SIG_BLOCK, &forwarded_signals, &previous_mask);

    install_signal_handler(SIGTERM, forward_supervisor_signal);
    install_signal_handler(SIGINT, forward_supervisor_signal);
    install_signal_handler(SIGHUP, forward_supervisor_signal);
    restore_default_signal_handler(SIGCHLD);

    const pid_t runtime_pid = fork();
    if (runtime_pid == 0) {
      supervised_runtime_pid = 0;
      restore_default_signal_handler(SIGTERM);
      restore_default_signal_handler(SIGINT);
      restore_default_signal_handler(SIGHUP);
      restore_default_signal_handler(SIGCHLD);
      (void) sigprocmask(SIG_SETMASK, &previous_mask, nullptr);

      execl(labwc_path.c_str(), "labwc",
        "-C", config_dir.c_str(),
        "-s", startup_shell.c_str(),
        nullptr);
      _exit(errno == ENOENT ? 127 : 126);
    }

    if (runtime_pid < 0) {
      (void) sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
      _exit(127);
    }

    supervised_runtime_pid = static_cast<sig_atomic_t>(runtime_pid);
    (void) sigprocmask(SIG_SETMASK, &previous_mask, nullptr);

    // A labwc startup client may call setsid() and leave the supervisor's
    // process group. PR_SET_CHILD_SUBREAPER makes that exact runtime descendant
    // a direct child when labwc exits. `/proc/.../children` is therefore a
    // generation-scoped ownership source: unlike a global process scan, it
    // cannot select an unrelated process that merely resembles the workload.
    auto signal_direct_children = [](int signal_number) {
      std::ifstream input {
        "/proc/self/task/" + std::to_string(getpid()) + "/children"
      };
      pid_t child = 0;
      while (input >> child) {
        if (child > 1 && child != getpid()) {
          (void) kill(child, signal_number);
        }
      }
    };

    siginfo_t exit_info {};
    bool observed_exit = false;
    if (!supervisor_stop_requested) {
      while (true) {
        if (waitid(P_PID, static_cast<id_t>(runtime_pid), &exit_info, WEXITED | WNOWAIT) == 0) {
          observed_exit = true;
          break;
        }
        if (errno != EINTR || supervisor_stop_requested) {
          break;
        }
      }
    }

    if (!observed_exit && !supervisor_stop_requested) {
      supervised_runtime_pid = 0;
      _exit(127);
    }

    // Ignore graceful signals while the supervisor broadcasts them to its own
    // private group. Every labwc descendant inherits this anchored PGID.
    install_signal_handler(SIGTERM, SIG_IGN);
    install_signal_handler(SIGINT, SIG_IGN);
    install_signal_handler(SIGHUP, SIG_IGN);
    (void) kill(-getpgrp(), SIGTERM);
    (void) kill(runtime_pid, SIGTERM);

    int runtime_status = 0;
    bool runtime_reaped = false;
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < deadline) {
      bool live_children = false;
      while (true) {
        int child_status = 0;
        const auto child_pid = waitpid(-1, &child_status, WNOHANG);
        if (child_pid > 0) {
          if (child_pid == runtime_pid) {
            runtime_status = child_status;
            runtime_reaped = true;
          }
          continue;
        }
        if (child_pid == 0) {
          live_children = true;
        } else if (errno == EINTR) {
          continue;
        } else if (errno != ECHILD) {
          live_children = true;
        }
        break;
      }

      if (live_children) {
        // Catch descendants as soon as the subreaper adopts them. Repeating
        // SIGTERM is harmless for the still-live compositor and closes the
        // adoption race without widening ownership beyond direct children.
        signal_direct_children(SIGTERM);
      }

      if (!live_children) {
        supervised_runtime_pid = 0;
        if (runtime_reaped && WIFEXITED(runtime_status)) {
          _exit(WEXITSTATUS(runtime_status));
        }
        if (runtime_reaped && WIFSIGNALED(runtime_status)) {
          _exit(128 + WTERMSIG(runtime_status));
        }
        _exit(127);
      }
      sleep_without_losing_interrupt_time(25ms);
    }

    // Separate-session descendants are outside the process group but remain
    // exact adopted children. Kill those before killing the immutable group
    // leader (which intentionally includes this supervisor).
    signal_direct_children(SIGKILL);
    (void) kill(-getpgrp(), SIGKILL);
    _exit(127);
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

  static void set_labwc_process_environment(bool headless) {
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
        setenv(std::string {key}.c_str(), value.c_str(), 1);
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
  }

  static bool executable_accessible(const std::string &path) {
    return !path.empty() && access(path.c_str(), X_OK) == 0;
  }

  static std::string shell_quote(std::string_view value) {
    const auto quote = std::string(1, static_cast<char>(39));
    const auto slash = std::string(1, static_cast<char>(92));
    std::string result = quote;

    for (char ch : value) {
      if (ch == static_cast<char>(39)) {
        result += quote;
        result += slash;
        result += quote;
        result += quote;
      } else {
        result += ch;
      }
    }

    result += quote;
    return result;
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

    // Reset stale state. The pidfd of a supervisor that died without going
    // through stop() is still open here; every session would otherwise leak one.
    close_cage_pidfd();
    cage_pid = 0;
    cage_wayland_socket.clear();
    cage_x11_display.clear();
    cage_session_instance_id.clear();
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

    // Snapshot existing sockets to detect the new one labwc creates
    auto sockets_before = snapshot_wayland_sockets();
    auto x11_displays_before = snapshot_x11_displays();
    BOOST_LOG(info) << "labwc: Watching runtime directory ["sv << runtime_dir()
                    << "] for a new Wayland socket"sv;

    // Save and clear MangoHud env vars before fork — MangoHud crashes labwc
    // if injected into the compositor. Will be re-set for the game via startup cmd.
    std::string saved_mangohud;
    std::string saved_mangohud_dlsym;
    std::string saved_mangohud_config;
    const char *mh = getenv("MANGOHUD");
    const char *mhd = getenv("MANGOHUD_DLSYM");
    const char *mhc = getenv("MANGOHUD_CONFIG");
    if (mh) { saved_mangohud = mh; unsetenv("MANGOHUD"); }
    if (mhd) { saved_mangohud_dlsym = mhd; unsetenv("MANGOHUD_DLSYM"); }
    if (mhc) { saved_mangohud_config = mhc; unsetenv("MANGOHUD_CONFIG"); }

    pid_t pid = fork();
    if (pid == 0) {
      // Child: become the stable session/process-group supervisor for one
      // private labwc generation. The compositor is forked beneath this anchor,
      // so menu-driven exit cannot leave its clients unowned.
      if (!session_instance_id.empty()) {
        setenv("POLARIS_SESSION_INSTANCE_ID", session_instance_id.c_str(), 1);
      } else {
        unsetenv("POLARIS_SESSION_INSTANCE_ID");
      }
      setenv("POLARIS_PRIVATE_SESSION", "1", 1);
      set_labwc_process_environment(headless);
      if (headless) {
        // Headless mode: no visible window on desktop, no parent display needed.
        // labwc creates virtual outputs that still support wlr-screencopy.
        //
        // Renderer is vulkan (see WLR_RENDERER above), same as the windowed cage.
        // Headless ran on gles2 until the wlroots-0.19 vulkan_instance_destroy SEGV
        // was confirmed gone on 0.20.2 (retested 2026-08-10). vulkan's ext-image-copy
        // DMA-BUF is GPU-native and CUDA/NVENC-importable, so headless no longer has
        // to fall back to a visible windowed cage to reach a GPU-native capture path.
        // Clear inherited display vars so children ONLY talk to labwc.
        // labwc will set its own WAYLAND_DISPLAY and start XWayland (new DISPLAY).
        // Without this, Steam connects to KDE's :0 instead of labwc's XWayland.
        unsetenv("DISPLAY");
        unsetenv("WAYLAND_DISPLAY");
      } else {
        // Windowed mode: labwc runs as a Wayland window on the desktop
      }
      // Clear DISPLAY in ALL modes so games connect to labwc's XWayland,
      // not KDE's :0. labwc will set its own DISPLAY via XWayland.
      unsetenv("DISPLAY");

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
      const auto startup_shell = std::string("bash -c " ) + shell_quote(startup_cmd);
      supervise_labwc(labwc_path, config_dir, startup_shell);
    } else if (pid > 0) {
      cage_pid = pid;
      open_cage_pidfd(pid);
      // Restore MangoHud in parent (was cleared to prevent labwc crash)
      if (!saved_mangohud.empty()) setenv("MANGOHUD", saved_mangohud.c_str(), 1);
      if (!saved_mangohud_dlsym.empty()) setenv("MANGOHUD_DLSYM", saved_mangohud_dlsym.c_str(), 1);
      if (!saved_mangohud_config.empty()) setenv("MANGOHUD_CONFIG", saved_mangohud_config.c_str(), 1);
      BOOST_LOG(info) << "labwc: Spawned (pid="sv << pid << ")"sv;
    } else {
      BOOST_LOG(error) << "labwc: fork() failed"sv;
      return false;
    }

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
    if (cage_pid <= 0) {
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
    cage_runtime_state = {
      .requested_headless = false,
      .effective_headless = false,
      .gpu_native_override_active = false,
      .backend_name = "labwc",
    };
  }

  void reset_after_external_stop() {
    if (cage_pid > 0) {
      (void) waitpid(cage_pid, nullptr, WNOHANG);
    }
    BOOST_LOG(info) << "labwc: Cleared router state after exact-generation pidfd cleanup"sv;
    close_cage_pidfd();
    cage_pid = 0;
    cage_wayland_socket.clear();
    cage_x11_display.clear();
    cage_session_instance_id.clear();
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

  std::string get_x11_display() {
    return cage_x11_display;
  }

  platf::runtime_state_t runtime_state() {
    return cage_runtime_state;
  }

}  // namespace cage_display_router

#endif  // __linux__
