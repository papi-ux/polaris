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
