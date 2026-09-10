#pragma once

#ifndef _WIN32
#include <cerrno>
#include <mutex>
#include <signal.h>
#include <sys/wait.h>

namespace util::posix_children {
  class protected_child_t;

  // Every process-wide wildcard reaper must use this registry. Exact-PID
  // owners may still reap their own children independently.
  inline std::mutex reaper_mutex;
  inline protected_child_t *protected_head = nullptr;

  class protected_child_t {
  public:
    // Register before spawn while excluding the wildcard reaper. The intrusive
    // list needs no allocation after a child has been created.
    protected_child_t() : lock(reaper_mutex), next(protected_head) {
      protected_head = this;
    }
    ~protected_child_t() {
      finish();
      auto **entry = &protected_head;
      while (*entry != this) entry = &(*entry)->next;
      *entry = next;
    }
    protected_child_t(const protected_child_t &) = delete;
    protected_child_t &operator=(const protected_child_t &) = delete;

    void publish(pid_t child) {
      pid = child;
      lock.unlock();
    }

    int finish() {
      if (!lock.owns_lock()) lock.lock();
      if (pid <= 0) return -1;
      const auto child = pid;
      pid = -1;
      siginfo_t info {};
      int observed;
      do {
        observed = waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT);
      } while (observed < 0 && errno == EINTR);
      // A foreign wait owner must never turn loss of ownership into a signal
      // to a potentially recycled process group.
      if (observed < 0) return -1;
      kill(-child, SIGKILL);
      int status = 0;
      pid_t waited;
      do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
      if (waited != child) return -1;
      if (WIFEXITED(status)) return WEXITSTATUS(status);
      return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
    }

  private:
    friend void reap_unowned_children();
    std::unique_lock<std::mutex> lock;
    protected_child_t *next;
    pid_t pid = -1;
  };

  inline void reap_unowned_children() {
    std::lock_guard guard(reaper_mutex);
    for (;;) {
      siginfo_t info {};
      if (waitid(P_ALL, 0, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
        if (errno == EINTR) continue;
        return;
      }
      if (info.si_pid == 0) return;
      for (auto *entry = protected_head; entry; entry = entry->next) {
        // Leave the leader unreaped for its owner. Try the generic drain again
        // on the next lifecycle tick; never block on provider inference.
        if (entry->pid == info.si_pid) return;
      }
      if (waitpid(info.si_pid, nullptr, WNOHANG) < 0 && errno != EINTR && errno != ECHILD) return;
    }
  }
}
#endif
