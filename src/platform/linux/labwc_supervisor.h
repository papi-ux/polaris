/** @file src/platform/linux/labwc_supervisor.h
 * Fresh-process private compositor supervisor. No host initialization runs here.
 */
#pragma once

#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <linux/capability.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compositor_identity_channel.h"

namespace labwc_supervisor {
  using namespace std::chrono_literals;
  inline volatile sig_atomic_t supervised_runtime_pid = 0;
  inline volatile sig_atomic_t supervisor_stop_requested = 0;

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
    const std::string &startup_shell,
    bool identity_enabled = false
  ) {
    supervised_runtime_pid = 0;
    supervisor_stop_requested = 0;

    // Optional compositor identity handoff. A CLOEXEC pipe proves the child exec'd: exec closes it,
    // a failed exec writes errno into it. Any failure here only disables the handoff.
    int exec_status[2] = {-1, -1};
    if (identity_enabled && pipe2(exec_status, O_CLOEXEC) != 0) {
      identity_enabled = false;
      (void) close(compositor_identity::k_channel_fd);
    }

    // posix_spawn creates this exact process as the session/group leader.
    // Manual internal-mode invocation must also establish a private group.
    if (((getsid(0) != getpid() || getpgrp() != getpid()) && setsid() < 0) ||
        prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
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
      if (identity_enabled) {
        // The compositor and every client it starts must never hold the identity channel. CLOEXEC
        // would close it at exec; closing it here leaves no window at all.
        (void) close(compositor_identity::k_channel_fd);
        (void) close(exec_status[0]);
      }

      execl(labwc_path.c_str(), "labwc",
        "-C", config_dir.c_str(),
        "-s", startup_shell.c_str(),
        nullptr);
      const int exec_error = errno;
      if (identity_enabled) compositor_identity::report_exec_failure(exec_status[1], exec_error);
      _exit(exec_error == ENOENT ? 127 : 126);
    }

    if (runtime_pid < 0) {
      (void) sigprocmask(SIG_SETMASK, &previous_mask, nullptr);
      _exit(127);
    }

    supervised_runtime_pid = static_cast<sig_atomic_t>(runtime_pid);
    (void) sigprocmask(SIG_SETMASK, &previous_mask, nullptr);

    if (identity_enabled) {
      // Signals are deliverable again here, so a stop during the exec wait is honoured. The handoff
      // never blocks on the host and never changes the supervision below.
      (void) close(exec_status[1]);
      (void) compositor_identity::send_after_exec(
        compositor_identity::k_channel_fd, runtime_pid, exec_status[0], &supervisor_stop_requested
      );
      (void) close(exec_status[0]);
      (void) close(compositor_identity::k_channel_fd);
    }

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
   * Whether the inherited identity channel is usable. It is always made CLOEXEC first, so the
   * compositor can never inherit it even when it is refused here.
   */
  inline bool identity_channel_usable() {
    const int fd = compositor_identity::k_channel_fd;
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) return false;
    struct stat info {};
    return fstat(fd, &info) == 0 && S_ISSOCK(info.st_mode);
  }

  inline std::optional<int> dispatch(int argc, char **argv) {
    if (argc < 2 || !argv[1] || std::string_view(argv[1]) != "--internal-labwc-supervisor") return std::nullopt;
    if ((argc != 5 && argc != 6) || !argv[2] || argv[2][0] != '/' || !argv[3] || argv[3][0] != '/' || !argv[4]) return 126;
    // FD 5 is trusted only when the host asked for the channel explicitly.
    bool identity_enabled = false;
    if (argc == 6) {
      if (!argv[5] || std::string_view(argv[5]) != compositor_identity::k_supervisor_flag) return 126;
      identity_enabled = identity_channel_usable();
    }
    // Re-executing a file-capability-enabled host can regain its file caps.
    // The supervisor needs ordinary same-user process/device access only. Drop
    // that authority before creating descendants and keep /proc identity
    // readable by the host that previously dropped its portal capabilities.
    __user_cap_header_struct header { _LINUX_CAPABILITY_VERSION_3, 0 };
    __user_cap_data_struct capabilities[2] {};
    // Preserve inherited no_new_privs: Steam and bubblewrap may use their
    // installed setuid sandbox helpers, as they did below the fork supervisor.
    if (prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0 ||
        syscall(SYS_capset, &header, capabilities) != 0 ||
        prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0) return 126;
    supervise_labwc(argv[2], argv[3], argv[4], identity_enabled);
  }
}
