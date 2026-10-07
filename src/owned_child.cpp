/**
 * @file src/owned_child.cpp
 * @brief One owned child process.
 */
#include "owned_child.h"

#include <algorithm>
#include <climits>
#include <filesystem>
#include <thread>

#ifndef _WIN32
  #include <cerrno>
  #include <csignal>
  #include <fcntl.h>
  #include <sys/resource.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #if defined(__linux__)
    #include <sys/syscall.h>
  #endif
#endif

namespace owned_child {
#ifdef _WIN32
  std::optional<close_plan_t> plan_descriptor_closure(const options_t &) {
    return std::nullopt;
  }

  child_t::~child_t() = default;

  bool child_t::start(const std::vector<std::string> &, const std::vector<std::string> &) {
    state_ = state_e::failed;
    return false;
  }

  state_e child_t::poll() {
    return state_;
  }

  void child_t::abort(std::chrono::milliseconds) {}
#else
  namespace {
    /// Exit statuses of a child that never reached execve.
    constexpr int k_exit_no_group = 121;
    constexpr int k_exit_no_null = 122;
    constexpr int k_exit_no_stdio = 123;
    constexpr int k_exit_no_closure = 124;

    /// Closes every descriptor above stderr, in the child after fork: async-signal-safe calls only.
    /// False when nothing proven could close them; the child then never executes.
    bool close_above_stderr(const close_plan_t &plan) {
  #if defined(__linux__) && defined(SYS_close_range)
      if (plan.close_range && ::syscall(SYS_close_range, 3U, ~0U, 0U) == 0) return true;
  #endif
      if (plan.bound < 0) return false;
      for (int fd = 3; fd < plan.bound; ++fd) ::close(fd);
      return true;
    }
  }  // namespace

  std::optional<close_plan_t> plan_descriptor_closure(const options_t &options) {
    close_plan_t plan;
  #if defined(__linux__) && defined(SYS_close_range)
    // Closing a range above any descriptor this process can hold closes nothing and says whether
    // the kernel (and any seccomp filter) allows the call.
    plan.close_range = options.allow_close_range && ::syscall(SYS_close_range, ~0U - 1, ~0U, 0U) == 0;
  #endif
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY &&
        limit.rlim_cur <= static_cast<rlim_t>(options.fallback_cap)) {
      int highest = -1;
      std::error_code ec;
      bool listed = false;
      for (const auto *dir : {"/proc/self/fd", "/dev/fd"}) {
        ec.clear();
        highest = -1;
        for (std::filesystem::directory_iterator it {dir, ec}, end; !ec && it != end; it.increment(ec)) {
          const auto name = it->path().filename().string();
          int fd = 0;
          bool digits = !name.empty();
          for (const char c : name) {
            if (c < '0' || c > '9' || fd > (INT_MAX - 9) / 10) {
              digits = false;
              break;
            }
            fd = fd * 10 + (c - '0');
          }
          if (digits && fd > highest) highest = fd;
        }
        if (!ec) {
          listed = true;
          break;
        }
      }
      if (listed && highest < options.fallback_cap) {
        plan.bound = std::max(static_cast<int>(limit.rlim_cur), highest + 1);
      }
    }
    if (!plan.close_range && plan.bound < 0) return std::nullopt;
    return plan;
  }

  child_t::~child_t() {
    abort();
  }

  bool child_t::start(const std::vector<std::string> &argv, const std::vector<std::string> &env) {
    if (state_ != state_e::idle || argv.empty() || argv.front().empty() || argv.front().front() != '/') {
      if (state_ == state_e::idle) state_ = state_e::failed;
      return false;
    }
    // Everything the child needs is built before fork: after it, only async-signal-safe calls.
    const auto plan = plan_descriptor_closure(options_);
    if (!plan) {
      // No closure bound can be proven: nothing is started rather than leaking a descriptor.
      state_ = state_e::failed;
      return false;
    }
    std::vector<char *> args;
    std::vector<char *> envp;
    for (const auto &arg : argv) args.push_back(const_cast<char *>(arg.c_str()));
    args.push_back(nullptr);
    for (const auto &entry : env) envp.push_back(const_cast<char *>(entry.c_str()));
    envp.push_back(nullptr);
    const char *path = argv.front().c_str();

    // Excludes the wildcard reaper from now until the pid is published.
    guard_ = std::make_unique<util::posix_children::protected_child_t>();
    const pid_t pid = ::fork();
    if (pid < 0) {
      guard_.reset();
      state_ = state_e::failed;
      return false;
    }
    if (pid == 0) {
      // Own process group, so a cancellation stops anything the command itself starts. A child that
      // cannot lead its own group, or set up its descriptors, never executes.
      if (::setpgid(0, 0) != 0) ::_exit(k_exit_no_group);
      const int null_fd = ::open("/dev/null", O_RDWR);
      if (null_fd < 0) ::_exit(k_exit_no_null);
      if (::dup2(null_fd, STDIN_FILENO) < 0 || ::dup2(null_fd, STDOUT_FILENO) < 0 || ::dup2(null_fd, STDERR_FILENO) < 0) {
        ::_exit(k_exit_no_stdio);
      }
      if (!close_above_stderr(*plan)) ::_exit(k_exit_no_closure);
      // Default signal dispositions and an empty mask: nothing the daemon set leaks into the child.
      sigset_t empty;
      sigemptyset(&empty);  // a macro on some libcs
      ::sigprocmask(SIG_SETMASK, &empty, nullptr);
      for (int sig = 1; sig < NSIG; ++sig) {
        if (sig != SIGKILL && sig != SIGSTOP) ::signal(sig, SIG_DFL);
      }
      ::execve(path, args.data(), envp.data());
      ::_exit(127);
    }
    // Also from the parent, so the group exists before any abort can target it.
    ::setpgid(pid, pid);
    pid_ = pid;
    guard_->publish(pid);
    state_ = state_e::running;
    return true;
  }

  void child_t::reap() {
    // The guard reaps exactly this pid (after confirming it is still our unreaped child) and clears
    // the group. Its result is the exit status, 128+signal, or -1 when the wait was lost.
    const int result = guard_ ? guard_->finish() : -1;
    guard_.reset();
    pid_ = -1;
    if (state_ == state_e::running && result >= 0 && result < 128) exit_code_ = result;
  }

  state_e child_t::poll() {
    if (state_ != state_e::running) return state_;
    siginfo_t info {};
    int observed;
    do {
      observed = ::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT);
    } while (observed < 0 && errno == EINTR);
    if (observed < 0) {
      // Lost to someone else: gone, and how it ended is unknown.
      guard_.reset();
      pid_ = -1;
      state_ = state_e::killed;
    } else if (info.si_pid == pid_) {
      reap();
      state_ = exit_code_ ? state_e::exited : state_e::killed;
    }
    return state_;
  }

  void child_t::abort(std::chrono::milliseconds grace) {
    if (poll() != state_e::running) return;
    // Only this child's own group: the pid is ours and unreaped, so it cannot have been reused.
    ::kill(-pid_, SIGTERM);
    const auto until = std::chrono::steady_clock::now() + grace;
    while (std::chrono::steady_clock::now() < until) {
      siginfo_t info {};
      if (::waitid(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid_) break;
      std::this_thread::sleep_for(std::chrono::milliseconds {5});
    }
    // Ended after we asked, or forced now: either way whatever it had already sent stands.
    state_ = state_e::killed;
    const int result = guard_ ? guard_->finish() : -1;
    (void) result;
    guard_.reset();
    pid_ = -1;
  }
#endif
}  // namespace owned_child
