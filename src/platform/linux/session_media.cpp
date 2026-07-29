/**
 * @file src/platform/linux/session_media.cpp
 * @brief Linux session media teardown + coalescing post-HTTP worker.
 */

#include "session_media.h"

#ifdef __linux__

  #include "src/browser_stream.h"
  #include "src/logging.h"

  #include <chrono>
  #include <condition_variable>
  #include <deque>
  #include <mutex>
  #include <thread>

using namespace std::literals;

namespace session_media {
  namespace {
    struct worker_state_t {
      std::mutex mutex;
      std::condition_variable changed;
      std::deque<std::function<void()>> queue;
      std::thread worker;
      bool worker_started = false;
      bool prepare_inflight = false;
    };

    // The worker is process-lifetime and owns its joinable thread. Deliberately
    // retain the state so neither the worker nor late teardown owners can touch
    // objects destroyed by static teardown.
    worker_state_t &worker_state() {
      static auto *state = new worker_state_t;
      return *state;
    }

    teardown_gate_t &media_gate() {
      static auto *gate = new teardown_gate_t;
      return *gate;
    }

    void worker_main(worker_state_t *state) {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock(state->mutex);
          state->changed.wait(lock, [state] {
            return !state->queue.empty();
          });
          job = std::move(state->queue.front());
          state->queue.pop_front();
          // Coalesce stop storms while retaining the current and latest jobs.
          if (state->queue.size() > 2) {
            auto last = std::move(state->queue.back());
            state->queue.clear();
            state->queue.push_back(std::move(last));
            BOOST_LOG(info) << "session_media: coalesced teardown queue to latest job"sv;
          }
        }
        if (!job) {
          continue;
        }
        try {
          job();
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "session_media: teardown job failed: "sv << e.what();
        } catch (...) {
          BOOST_LOG(warning) << "session_media: teardown job failed (unknown)"sv;
        }
      }
    }

    void ensure_worker() {
      auto &state = worker_state();
      std::lock_guard lock(state.mutex);
      if (state.worker_started) {
        return;
      }

      state.worker_started = true;
      try {
        state.worker = std::thread {worker_main, &state};
      } catch (...) {
        state.worker_started = false;
        throw;
      }
    }
  }  // namespace

  start_owner_t begin_start() {
    return media_gate().begin_start();
  }

  teardown_owner_t begin_teardown() {
    return media_gate().begin_teardown();
  }

  void prepare_for_stop() {
    auto &state = worker_state();
    {
      std::unique_lock lock(state.mutex);
      if (state.prepare_inflight) {
        BOOST_LOG(info) << "session_media: prepare_for_stop already in flight; waiting on condition_variable"sv;
        const bool done = state.changed.wait_for(lock, std::chrono::seconds(2), [&state] {
          return !state.prepare_inflight;
        });
        if (!done) {
          BOOST_LOG(warning) << "session_media: previous prepare still busy after 2s wait_for; skipping nested prepare"sv;
        }
        return;
      }
      state.prepare_inflight = true;
    }

    struct release_prepare_flag_t {
      worker_state_t &state;
      ~release_prepare_flag_t() {
        {
          std::lock_guard lock(state.mutex);
          state.prepare_inflight = false;
        }
        state.changed.notify_all();
      }
    } release_prepare_flag {state};

    // The root owner blocks reconnect immediately. Portal destruction and
    // Browser capture joins acquire additional owners when they outlive the
    // bounded prepare_for_stop() response budget.
    auto teardown = begin_teardown();
    browser_stream::prepare_for_session_teardown();
  }

  void schedule(std::function<void()> work) {
    if (!work) {
      return;
    }
    ensure_worker();
    auto &state = worker_state();
    {
      std::lock_guard lock(state.mutex);
      state.queue.push_back(std::move(work));
    }
    state.changed.notify_one();
  }

}  // namespace session_media

#endif  // __linux__
