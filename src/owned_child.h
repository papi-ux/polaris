/**
 * @file src/owned_child.h
 * @brief One owned child process: fixed argv and environment, no shell, polled, killed and reaped.
 *
 * The child gets exactly the argv and environment given, stdin/stdout/stderr on /dev/null and no
 * other inherited descriptor. It leads its own process group so a cancellation reaches anything it
 * started. Nothing waits under a lock and nothing is detached: the owner polls, and abort() or the
 * destructor stops and reaps the child before returning.
 */
#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
  #include <sys/types.h>

  #include "posix_child_reaper.h"
#endif

namespace owned_child {
  enum class state_e {
    idle,  ///< never started
    running,
    exited,  ///< exited by itself; see exit_code()
    killed,  ///< stopped by abort(); its effect, if any, is unknown
    failed,  ///< could not be started
  };

  /// How a child closes every descriptor above stderr, decided before fork.
  struct close_plan_t {
    bool close_range = false;  ///< the kernel's close_range(3, ~0) answered here, so the child uses it
    int bound = -1;  ///< else (or if it fails) close [3, bound); -1: no bound could be proven
  };

  struct options_t {
    /// Tests turn this off to exercise the bounded fallback on a kernel that has close_range.
    bool allow_close_range = true;
    /// The largest descriptor bound the fallback loop accepts; above it the start is refused.
    int fallback_cap = 1 << 20;
  };

  /**
   * @brief The closure plan for this process now, or nullopt when no closure can be proven.
   *
   * The fallback bound is the larger of a finite RLIMIT_NOFILE soft limit and one past the highest
   * descriptor listed in /proc/self/fd (or /dev/fd). A descriptor opened later by another thread is
   * the lowest free number, so it is below the limit; one above the limit (opened before the limit
   * was lowered) is in the listing. An infinite or oversized limit, or an unreadable listing, is no
   * bound.
   */
  std::optional<close_plan_t> plan_descriptor_closure(const options_t &options = {});

  class child_t {
  public:
    explicit child_t(options_t options = {}):
        options_ {options} {}
    ~child_t();
    child_t(const child_t &) = delete;
    child_t &operator=(const child_t &) = delete;

    /// Starts `argv[0]` (an absolute path, executed directly) with exactly `env`. False if it could
    /// not be started; then nothing runs.
    bool start(const std::vector<std::string> &argv, const std::vector<std::string> &env);
    /// Non-blocking: reaps the child if it has exited.
    state_e poll();
    /// Stops the child's process group (TERM, then KILL after `grace`) and reaps it. Idempotent.
    void abort(std::chrono::milliseconds grace = std::chrono::milliseconds {200});
    state_e state() const {
      return state_;
    }
    /// The exit status when it exited normally, else nullopt (signalled, killed, running).
    std::optional<int> exit_code() const {
      return exit_code_;
    }
#ifndef _WIN32
    pid_t pid() const {
      return pid_;
    }
#endif

  private:
    options_t options_;
    state_e state_ = state_e::idle;
    std::optional<int> exit_code_;
#ifndef _WIN32
    pid_t pid_ = -1;
    /// Registered before fork, so the process-wide wildcard reaper never takes this child's status.
    std::unique_ptr<util::posix_children::protected_child_t> guard_;
    void reap();
#endif
  };
}  // namespace owned_child
