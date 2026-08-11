/**
 * @file src/platform/linux/session_media.cpp
 * @brief Linux session media teardown + coalescing post-HTTP worker.
 */

#include "session_media.h"

#ifdef __linux__

  #include "src/browser_stream.h"
  #include "src/logging.h"
  #ifdef POLARIS_BUILD_PORTAL
    #include "src/platform/linux/portal_session.h"
  #endif

  #include <condition_variable>
  #include <deque>
  #include <mutex>
  #include <thread>
  #include <unordered_map>
  #include <utility>

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

    struct pending_cancel_state_t {
      std::mutex mutex;
      std::unordered_map<const void *, unsigned> owners;
    };

    pending_cancel_state_t &pending_cancel_state() {
      static auto *state = new pending_cancel_state_t;
      return *state;
    }

    thread_local const void *current_pending_start_owner = nullptr;

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

  pending_start_cancel_owner_t::pending_start_cancel_owner_t(
    pending_start_cancel_owner_t &&other
  ) noexcept:
      owner_tag_(std::exchange(other.owner_tag_, nullptr)) {}

  pending_start_cancel_owner_t &pending_start_cancel_owner_t::operator=(
    pending_start_cancel_owner_t &&other
  ) noexcept {
    if (this != &other) {
      reset();
      owner_tag_ = std::exchange(other.owner_tag_, nullptr);
    }
    return *this;
  }

  pending_start_cancel_owner_t::~pending_start_cancel_owner_t() {
    reset();
  }

  void pending_start_cancel_owner_t::reset() noexcept {
    const auto *owner_tag = std::exchange(owner_tag_, nullptr);
    if (!owner_tag) {
      return;
    }
    auto &state = pending_cancel_state();
    std::lock_guard lock(state.mutex);
    const auto it = state.owners.find(owner_tag);
    if (it != state.owners.end() && --it->second == 0) {
      state.owners.erase(it);
    }
  }

  pending_start_cancel_owner_t cancel_pending_starts(const void *owner_tag) {
    if (!owner_tag) {
      return {};
    }
    auto owner = pending_start_cancel_owner_t {owner_tag};
    auto &state = pending_cancel_state();
    {
      std::lock_guard lock(state.mutex);
      ++state.owners[owner_tag];
    }
  #ifdef POLARIS_BUILD_PORTAL
    portal::cancel_pending_requests(owner_tag);
  #endif
    return owner;
  }

  bool pending_start_cancelled(const void *owner_tag) {
    if (!owner_tag) {
      return false;
    }
    auto &state = pending_cancel_state();
    std::lock_guard lock(state.mutex);
    return state.owners.contains(owner_tag);
  }

  pending_start_owner_scope_t::pending_start_owner_scope_t(const void *owner_tag):
      previous_(current_pending_start_owner) {
    current_pending_start_owner = owner_tag;
  }

  pending_start_owner_scope_t::~pending_start_owner_scope_t() {
    current_pending_start_owner = previous_;
  }

  const void *pending_start_owner() {
    return current_pending_start_owner;
  }

  start_owner_t begin_start() {
    return media_gate().begin_start();
  }

  teardown_owner_t begin_teardown() {
    return media_gate().begin_teardown([] {
  #ifdef POLARIS_BUILD_PORTAL
      portal::cancel_pending_requests();
  #endif
    });
  }

  bool teardown_in_progress() {
    return media_gate().teardown_in_progress();
  }

  teardown_owner_t prepare_for_stop() {
    auto &state = worker_state();
    {
      std::unique_lock lock(state.mutex);
      if (state.prepare_inflight) {
        BOOST_LOG(info) << "session_media: prepare_for_stop already in flight; waiting for terminal media teardown"sv;
        state.changed.wait(lock, [&state] {
          return !state.prepare_inflight;
        });
        return begin_teardown();
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
    // Browser capture joins acquire additional owners when they outlive their
    // short synchronous response budgets.
    auto teardown = begin_teardown();
    browser_stream::prepare_for_session_teardown();

    // Wait for every subordinate cleanup while retaining the root owner.
    // Compositor/process termination occurs immediately after this function;
    // the returned fence keeps begin_start blocked until that kill completes.
    // Killing
    // gamescope while PipeWire still tears down can deadlock pw_thread_loop_stop,
    // and reopening capture admission would overlap two generations.
    media_gate().wait_for_other_teardowns(teardown);
    return teardown;
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
