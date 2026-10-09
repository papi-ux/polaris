#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <new>

#include "src/cover_lookup_workers.h"

using namespace std::chrono_literals;

TEST(CoverWorkerAdmissionTests, PrelaunchExceptionNeverInvokesOrRetainsWork) {
  cover_lookup::workers_t workers;
  std::atomic<int> calls {0};
  auto payload = std::make_shared<int>(1);
  std::weak_ptr<int> observed = payload;
  workers.set_before_launch_for_tests([] { throw std::bad_alloc {}; });
  EXPECT_THROW(workers.submit([owned = std::move(payload), &calls] { ++calls; }), std::bad_alloc);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(observed.expired());
  EXPECT_FALSE(workers.pending_for_tests());
  workers.set_before_launch_for_tests({});
  EXPECT_EQ(workers.submit([&] { ++calls; }), cover_lookup::admission_e::accepted);
  workers.shutdown();
  EXPECT_EQ(calls, 1);
}

TEST(CoverWorkerAdmissionTests, CompletionPrelaunchExceptionLeavesNoCommitOrDrainDebt) {
  cover_lookup::workers_t workers;
  std::atomic<int> tasks {0}, commits {0};
  auto payload = std::make_shared<int>(1);
  std::weak_ptr<int> observed = payload;
  workers.set_before_launch_for_tests([] { throw std::bad_alloc {}; });
  EXPECT_THROW(workers.submit_completion([owned = std::move(payload), &tasks, &commits](const auto &ticket) {
    ++tasks;
    ticket->handoff([&] { ++commits; }, [](auto callback) { callback(); });
  }), std::bad_alloc);
  EXPECT_EQ(tasks, 0);
  EXPECT_EQ(commits, 0);
  EXPECT_TRUE(observed.expired());
  EXPECT_FALSE(workers.pending_for_tests());
  workers.set_before_launch_for_tests({});
  EXPECT_EQ(workers.submit_completion([&](const auto &ticket) {
    ++tasks;
    ticket->handoff([&] { ++commits; }, [](auto callback) { callback(); });
  }), cover_lookup::admission_e::accepted);
  // The commit may be cancelled by stop; accepted storage still runs exactly once.
  workers.shutdown();
  EXPECT_EQ(tasks, 1);
  EXPECT_LE(commits, 1);
}

TEST(CoverWorkerAdmissionTests, FullAndStoppedAdmissionPrecedeLaunchBookkeeping) {
  cover_lookup::workers_t workers;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> hooks {0}, refused_calls {0};
  EXPECT_EQ(workers.submit([&] { released.wait(); }), cover_lookup::admission_e::accepted);
  EXPECT_EQ(workers.submit([&] { released.wait(); }), cover_lookup::admission_e::accepted);
  workers.set_before_launch_for_tests([&] { ++hooks; throw std::bad_alloc {}; });
  EXPECT_EQ(workers.submit_completion([&](const auto &) { ++refused_calls; }), cover_lookup::admission_e::full);
  workers.stop_accepting();
  EXPECT_EQ(workers.submit_completion([&](const auto &) { ++refused_calls; }), cover_lookup::admission_e::stopping);
  EXPECT_EQ(workers.submit([&] { ++refused_calls; }), cover_lookup::admission_e::stopping);
  release.set_value();
  workers.shutdown();
  EXPECT_EQ(hooks, 0);
  EXPECT_EQ(refused_calls, 0);
}

TEST(CoverWorkerAdmissionTests, StopCancelsEveryTrackedCompletionAndLateQueueIsInert) {
  cover_lookup::workers_t workers;
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::function<void()>> queued;
  std::atomic<int> commits {0};
  for (std::size_t i = 0; i < cover_lookup::workers_t::capacity; ++i) {
    EXPECT_EQ(workers.submit_completion([&](const auto &ticket) {
      ticket->handoff([&] { ++commits; }, [&](auto callback) {
        std::lock_guard lock(mutex);
        queued.push_back(std::move(callback));
        changed.notify_all();
      });
    }), cover_lookup::admission_e::accepted);
  }
  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(changed.wait_for(lock, 10s, [&] { return queued.size() == cover_lookup::workers_t::capacity; }));
  }
  workers.stop_accepting();
  workers.shutdown();
  for (auto &callback : queued) callback();
  EXPECT_EQ(commits, 0);
  EXPECT_FALSE(workers.pending_for_tests());
}
