#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <stdexcept>
#include <thread>

#include "src/cover_lookup_workers.h"

using namespace std::chrono_literals;

TEST(CoverLookupWorkers, BoundsAcceptedWorkAndRejectsAfterAdmissionCloses) {
  cover_lookup::workers_t workers;
  std::promise<void> release;
  auto released = release.get_future().share();
  std::atomic<int> calls {0};
  auto task = [&] { ++calls; released.wait(); };
  EXPECT_EQ(workers.submit(task), cover_lookup::admission_e::accepted);
  EXPECT_EQ(workers.submit(task), cover_lookup::admission_e::accepted);
  EXPECT_EQ(workers.submit(task), cover_lookup::admission_e::full);
  workers.stop_accepting();
  EXPECT_EQ(workers.submit(task), cover_lookup::admission_e::stopping);
  release.set_value();
  workers.shutdown();
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(workers.submit(task), cover_lookup::admission_e::stopping);
}

TEST(CoverLookupWorkers, EveryShutdownCallerWaitsForAcceptedWorkToExit) {
  cover_lookup::workers_t workers;
  std::promise<void> release, entered;
  auto released = release.get_future().share();
  std::atomic<bool> finished {false};
  EXPECT_EQ(workers.submit([&] {
    entered.set_value();
    released.wait();
    finished = true;
  }), cover_lookup::admission_e::accepted);
  entered.get_future().wait();
  auto first = std::async(std::launch::async, [&] { workers.shutdown(); });
  auto second = std::async(std::launch::async, [&] { workers.shutdown(); });
  EXPECT_EQ(first.wait_for(30ms), std::future_status::timeout);
  EXPECT_EQ(second.wait_for(30ms), std::future_status::timeout);
  EXPECT_FALSE(finished);
  release.set_value();
  first.get();
  second.get();
  EXPECT_TRUE(finished);
  EXPECT_EQ(workers.submit([] {}), cover_lookup::admission_e::stopping);
}

TEST(CoverLookupWorkers, ExceptionsDoNotLoseOwnershipOrPreventDraining) {
  cover_lookup::workers_t workers;
  EXPECT_EQ(workers.submit([] { throw std::runtime_error("mock provider failure"); }),
            cover_lookup::admission_e::accepted);
  std::atomic<bool> completed {false};
  EXPECT_EQ(workers.submit([&] { completed = true; }), cover_lookup::admission_e::accepted);
  EXPECT_NO_THROW(workers.shutdown());
  EXPECT_TRUE(completed);
}

TEST(CoverLookupWorkers, CompletedTaskReleasesItsCapturesBeforeOwnerReapsIt) {
  cover_lookup::workers_t workers;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> observed = capture;
  EXPECT_EQ(workers.submit([owned = std::move(capture)] {}), cover_lookup::admission_e::accepted);
  for (int i = 0; i < 200 && !observed.expired(); ++i) std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(observed.expired());
  workers.shutdown();
}

TEST(CoverLookupWorkers, CancellationClearsQueuedCapturesWithoutExecutingTheQueue) {
  cover_lookup::workers_t workers;
  std::promise<void> queued;
  std::function<void()> callback;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> observed = capture;
  std::atomic<int> commits {0};
  ASSERT_EQ(workers.submit_completion([owned = std::move(capture), &queued, &callback, &commits](const auto &ticket) {
    ticket->handoff([owned, &commits] { ++commits; }, [&](std::function<void()> fn) {
      callback = std::move(fn); queued.set_value();
    });
  }), cover_lookup::admission_e::accepted);
  queued.get_future().wait();
  workers.stop_accepting(); workers.shutdown();
  EXPECT_TRUE(observed.expired()); EXPECT_EQ(commits, 0);
  callback(); EXPECT_EQ(commits, 0);
}

TEST(CoverLookupWorkers, AnEnteredCommitCompletesBeforeTheGateCloses) {
  cover_lookup::workers_t workers;
  std::promise<void> queued, entered, release;
  auto released = release.get_future().share();
  std::function<void()> callback;
  std::atomic<int> commits {0};
  ASSERT_EQ(workers.submit_completion([&](const auto &ticket) {
    ticket->handoff([&] { entered.set_value(); released.wait(); ++commits; },
      [&](std::function<void()> fn) { callback = std::move(fn); queued.set_value(); });
  }), cover_lookup::admission_e::accepted);
  queued.get_future().wait();
  std::jthread executor([&] { callback(); });
  entered.get_future().wait();
  auto closing = std::async(std::launch::async, [&] { workers.stop_accepting(); });
  EXPECT_EQ(closing.wait_for(30ms), std::future_status::timeout);
  release.set_value(); closing.get(); workers.shutdown();
  EXPECT_EQ(commits, 1);
}

TEST(CoverLookupWorkers, FailedPostCancelsAndDrainsOwnedCompletion) {
  cover_lookup::workers_t workers;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> observed = capture;
  std::promise<void> posted;
  ASSERT_EQ(workers.submit_completion([owned = std::move(capture), &posted](const auto &ticket) {
    ticket->handoff([owned] {}, [&](std::function<void()>) {
      posted.set_value(); throw std::runtime_error("owned executor unavailable");
    });
  }), cover_lookup::admission_e::accepted);
  posted.get_future().wait(); workers.shutdown(); EXPECT_TRUE(observed.expired());
}

TEST(CoverLookupWorkers, QueuedCompletionKeepsItsSlotUntilCommitReturns) {
  cover_lookup::workers_t workers;
  std::promise<void> queued, release;
  auto released = release.get_future().share();
  std::function<void()> callback;
  ASSERT_EQ(workers.submit_completion([&](const auto &ticket) {
    ticket->handoff([] {}, [&](std::function<void()> fn) { callback = std::move(fn); queued.set_value(); });
  }), cover_lookup::admission_e::accepted);
  queued.get_future().wait();
  ASSERT_EQ(workers.submit([&] { released.wait(); }), cover_lookup::admission_e::accepted);
  EXPECT_EQ(workers.submit([] {}), cover_lookup::admission_e::full);
  callback(); release.set_value(); workers.shutdown();
}
