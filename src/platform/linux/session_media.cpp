/**
 * @file src/platform/linux/session_media.cpp
 * @brief Linux session media teardown + coalescing post-HTTP worker.
 */

#include "session_media.h"

#ifdef __linux__

  #include "src/browser_stream.h"
  #include "src/logging.h"

  #include <atomic>
  #include <chrono>
  #include <condition_variable>
  #include <deque>
  #include <mutex>
  #include <thread>

using namespace std::literals;

namespace session_media {
  namespace {

    std::mutex g_worker_mu;
    std::condition_variable g_worker_cv;
    std::deque<std::function<void()>> g_queue;
    std::atomic<bool> g_worker_started {false};
    std::atomic<bool> g_prepare_inflight {false};

    void worker_main() {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock(g_worker_mu);
          g_worker_cv.wait(lock, [] {
            return !g_queue.empty();
          });
          job = std::move(g_queue.front());
          g_queue.pop_front();
          // Coalesce: drop identical no-op floods; keep only the latest pending
          // jobs after the one we take (stop storms from double-click UI).
          if (g_queue.size() > 2) {
            auto last = std::move(g_queue.back());
            g_queue.clear();
            g_queue.push_back(std::move(last));
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
      bool expected = false;
      if (g_worker_started.compare_exchange_strong(expected, true)) {
        std::thread {worker_main}.detach();
      }
    }

  }  // namespace

  void prepare_for_stop() {
    // Serialize concurrent prepare (HTTP stop + terminate_impl can race).
    bool expected = false;
    if (!g_prepare_inflight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      BOOST_LOG(info) << "session_media: prepare_for_stop already in flight; waiting on condition_variable"sv;
      // Slop #8: condition_variable + timeout (no spin-sleep). Waiters share g_worker_mu
      // with the in-flight prepare's release_flag so notify_all unblocks promptly.
      std::unique_lock lock(g_worker_mu);
      const bool done = g_worker_cv.wait_for(lock, std::chrono::seconds(2), [] {
        return !g_prepare_inflight.load(std::memory_order_acquire);
      });
      if (!done) {
        BOOST_LOG(warning) << "session_media: previous prepare still busy after 2s wait_for; skipping nested prepare"sv;
      }
      return;
    }

    struct release_flag {
      ~release_flag() {
        g_prepare_inflight.store(false, std::memory_order_release);
        g_worker_cv.notify_all();
      }
    } guard;

    // browser_stream owns Browser Stream capture threads; it also performs the
    // ordered portal release + bounded join. This is the single call site for
    // that sequence from process/confighttp/lifecycle.
    browser_stream::prepare_for_session_teardown();
  }

  void schedule(std::function<void()> work) {
    if (!work) {
      return;
    }
    ensure_worker();
    {
      std::lock_guard lock(g_worker_mu);
      g_queue.push_back(std::move(work));
    }
    g_worker_cv.notify_one();
  }

}  // namespace session_media

#endif  // __linux__
