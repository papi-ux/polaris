/**
 * @file src/cover_lookup_workers.h
 * @brief Bounded ownership of console cover lookups while the HTTP thread serves other pages.
 */
#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <mutex>
#include <vector>

namespace cover_lookup {
  enum class admission_e { accepted, full, stopping };

  class workers_t {
  public:
    static constexpr std::size_t capacity = 2;

    workers_t() { tasks_.reserve(capacity); }
    workers_t(const workers_t &) = delete;
    workers_t &operator=(const workers_t &) = delete;
    ~workers_t() { shutdown(); }

    admission_e submit(std::function<void()> task) {
      std::lock_guard lock(mutex_);
      if (!accepting_) return admission_e::stopping;
      for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->wait_for(std::chrono::seconds {0}) == std::future_status::ready) {
          consume(*it);
          it = tasks_.erase(it);
        } else {
          ++it;
        }
      }
      if (tasks_.size() == capacity) return admission_e::full;
      tasks_.emplace_back(std::async(std::launch::async, [task = std::move(task)]() mutable {
        // A future may retain its callable after completion. Release response
        // captures on the worker, so sending never waits for a later reap.
        auto owned = std::move(task);
        owned();
      }));
      return admission_e::accepted;
    }

    // Close admission before stopping the HTTP server. An in-flight handler can
    // then only receive a refusal, even while shutdown waits for accepted work.
    void stop_accepting() {
      std::lock_guard lock(mutex_);
      accepting_ = false;
    }

    void shutdown() {
      stop_accepting();
      // Concurrent/repeated callers all wait for the same completed drain.
      std::lock_guard drain_lock(drain_mutex_);
      std::vector<std::future<void>> tasks;
      {
        std::lock_guard lock(mutex_);
        tasks.swap(tasks_);
      }
      for (auto &task : tasks) consume(task);
    }

  private:
    static void consume(std::future<void> &task) noexcept {
      try {
        task.get();
      } catch (...) {
        // Route tasks format their own failures. Still join a throwing task.
      }
    }

    std::mutex mutex_;
    std::mutex drain_mutex_;
    bool accepting_ = true;
    std::vector<std::future<void>> tasks_;
  };
}  // namespace cover_lookup
