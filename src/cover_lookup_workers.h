/**
 * @file src/cover_lookup_workers.h
 * @brief Bounded ownership of console cover lookups while the HTTP thread serves other pages.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace cover_lookup {
  enum class admission_e { accepted, full, stopping };

  // A queued mutation owns no executor. Stop closes this gate before the HTTP
  // queue stops; an already-entered bounded commit finishes under the same gate.
  struct completion_gate_t { std::mutex mutex; bool open = true; };

  class completion_t : public std::enable_shared_from_this<completion_t> {
  public:
    explicit completion_t(std::shared_ptr<completion_gate_t> gate) : gate_(std::move(gate)) {}
    bool cancelled() const { return cancelled_.load(); }
    void cancel() {
      std::lock_guard lock(mutex_);
      cancelled_ = true;
      commit_ = {};
      done_ = true;
      finished_.notify_all();
    }
    void handoff(std::function<void()> commit,
                 const std::function<void(std::function<void()>)> &post) {
      {
        std::lock_guard lock(mutex_);
        if (cancelled_) return; // Destroy the caller's owned staged result here.
        commit_ = std::move(commit);
      }
      // Never retain an io_context from its own queued callback. Cancellation
      // clears the ticket's commit/artifacts even if this callback never runs.
      const std::weak_ptr weak = shared_from_this();
      try { post([weak] { if (auto ticket = weak.lock()) ticket->run(); }); }
      catch (...) { cancel(); }
      std::unique_lock lock(mutex_);
      finished_.wait(lock, [&] { return done_; });
    }
  private:
    void run() {
      std::lock_guard gate_lock(gate_->mutex);
      std::lock_guard lock(mutex_);
      if (done_) return;
      if (!gate_->open) { cancelled_ = true; commit_ = {}; }
      else {
        auto commit = std::move(commit_);
        try { if (commit) commit(); } catch (...) { }
      }
      done_ = true;
      finished_.notify_all();
    }
    std::shared_ptr<completion_gate_t> gate_;
    std::mutex mutex_;
    std::condition_variable finished_;
    std::function<void()> commit_;
    std::atomic<bool> cancelled_ {false};
    bool done_ = false;
  };

  class workers_t {
  public:
    static constexpr std::size_t capacity = 2;

    workers_t() { tasks_.reserve(capacity); }
    workers_t(const workers_t &) = delete;
    workers_t &operator=(const workers_t &) = delete;
    ~workers_t() { shutdown(); }

    admission_e submit(std::function<void()> task) {
      std::lock_guard lock(mutex_);
      return submit_locked(std::move(task));
    }

    admission_e submit_completion(std::function<void(std::shared_ptr<completion_t>)> task) {
      std::lock_guard lock(mutex_);
      auto ticket = std::make_shared<completion_t>(completion_gate_);
      const auto admitted = submit_locked([ticket, task = std::move(task)]() mutable { task(ticket); });
      if (admitted == admission_e::accepted) {
        std::erase_if(completions_, [](const auto &weak) { return weak.expired(); });
        completions_.push_back(ticket);
      }
      return admitted;
    }

    void stop_accepting() {
      std::vector<std::shared_ptr<completion_t>> pending;
      {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        for (const auto &weak : completions_) if (auto ticket = weak.lock()) pending.push_back(std::move(ticket));
      }
      {
        std::lock_guard gate_lock(completion_gate_->mutex);
        completion_gate_->open = false;
      }
      for (const auto &ticket : pending) ticket->cancel();
    }

#ifdef POLARIS_TESTS
    bool pending_for_tests() {
      std::lock_guard lock(mutex_);
      return std::any_of(tasks_.begin(), tasks_.end(), [](auto &task) {
        return task.wait_for(std::chrono::seconds {0}) != std::future_status::ready;
      });
    }
#endif

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
    admission_e submit_locked(std::function<void()> task) {
      if (!accepting_) return admission_e::stopping;
      for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->wait_for(std::chrono::seconds {0}) == std::future_status::ready) {
          consume(*it);
          it = tasks_.erase(it);
        } else { ++it; }
      }
      if (tasks_.size() == capacity) return admission_e::full;
      tasks_.emplace_back(std::async(std::launch::async, [task = std::move(task)]() mutable {
        auto owned = std::move(task); // Release captures before a later reap.
        owned();
      }));
      return admission_e::accepted;
    }
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
    std::shared_ptr<completion_gate_t> completion_gate_ = std::make_shared<completion_gate_t>();
    std::vector<std::weak_ptr<completion_t>> completions_;
    std::vector<std::future<void>> tasks_;
  };
}  // namespace cover_lookup
