/**
 * @file src/owned_child.cpp
 * @brief One owned child process.
 */
#include "owned_child.h"

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
    /// Closes every descriptor from `first` up, in the child after fork: async-signal-safe only.
    void close_from(int first) {
  #if defined(__linux__) && defined(SYS_close_range)
      if (::syscall(SYS_close_range, first, ~0U, 0) == 0) return;
  #endif
      struct rlimit limit {};
      int max = 1024;
      if (::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY && limit.rlim_cur < 1048576) {
        max = static_cast<int>(limit.rlim_cur);
      }
      for (int fd = first; fd < max; ++fd) ::close(fd);
    }
  }  // namespace

  child_t::~child_t() {
    abort();
  }

  bool child_t::start(const std::vector<std::string> &argv, const std::vector<std::string> &env) {
    if (state_ != state_e::idle || argv.empty() || argv.front().empty() || argv.front().front() != '/') {
      if (state_ == state_e::idle) state_ = state_e::failed;
      return false;
    }
    // Everything the child needs is built before fork: after it, only async-signal-safe calls.
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
      // Own process group, so a cancellation stops anything the command itself starts.
      ::setpgid(0, 0);
      const int null_fd = ::open("/dev/null", O_RDWR);
      if (null_fd < 0) ::_exit(127);
      ::dup2(null_fd, STDIN_FILENO);
      ::dup2(null_fd, STDOUT_FILENO);
      ::dup2(null_fd, STDERR_FILENO);
      close_from(3);
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
