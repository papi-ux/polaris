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

  class child_t {
  public:
    child_t() = default;
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
