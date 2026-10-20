/**
 * @file tests/unit/test_proc_env_locking.cpp
 * @brief proc_t::get_env() hands out a copy of _env, which execute_impl and
 *        terminate_impl rewrite under the session lifecycle lock. The callers
 *        that copy it -- the server-command and undo-command runners in
 *        stream.cpp -- are on other threads, so the copy has to take the same
 *        lock or it can read a native environment block mid-reallocation.
 */

#include <src/process.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(ProcEnvLocking, GetEnvWaitsForTheSessionLifecycleLock) {
  std::atomic<bool> reader_reached_call {false};
  std::atomic<bool> reader_finished {false};
  std::thread reader;

  proc::proc.with_session_lifecycle_lock_for_tests([&]() {
    reader = std::thread([&]() {
      reader_reached_call.store(true, std::memory_order_release);
      (void) proc::proc.get_env();
      reader_finished.store(true, std::memory_order_release);
    });

    while (!reader_reached_call.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(200ms);

    // Still inside the lock. An unlocked get_env() returns in microseconds, so
    // observing the copy complete here is the regression this guards against.
    // The reverse -- the reader not having reached the lock yet -- can only
    // make this pass spuriously, never fail spuriously.
    EXPECT_FALSE(reader_finished.load(std::memory_order_acquire));
  });

  reader.join();
  EXPECT_TRUE(reader_finished.load(std::memory_order_acquire));
}

TEST(ProcEnvLocking, GetEnvIsReentrantFromInsideTheLock) {
  // The lifecycle mutex is recursive, so a locked caller reaching get_env must
  // not deadlock against itself.
  proc::proc.with_session_lifecycle_lock_for_tests([]() {
    EXPECT_NO_THROW((void) proc::proc.get_env());
  });
}

TEST(ProcStreamStatsContext, CopiesApplicationMetadataUnderTheReentrantLock) {
  boost::process::v1::environment env;
  env["MANGOHUD"] = "1";
  proc::proc_t subject {std::move(env), {}};
  proc::ctx_t app {};
  app.id = "42";
  app.uuid = "telemetry-context-test";
  subject.set_running_app_for_tests(app);
  subject.virtual_display = true;

  std::optional<proc::stream_stats_context_t> context;
  subject.with_session_lifecycle_lock_for_tests([&]() {
    context = subject.try_stream_stats_context();
  });
  ASSERT_TRUE(context);
  EXPECT_TRUE(context->mangohud_configured);
  EXPECT_TRUE(context->virtual_display);
  EXPECT_EQ(context->game, subject.get_last_run_app_name());
  EXPECT_EQ(context->game_uuid, app.uuid);

  app.env_vars["MANGOHUD"] = "0";
  subject.set_running_app_for_tests(app);
  const auto overridden = subject.try_stream_stats_context();
  ASSERT_TRUE(overridden);
  EXPECT_FALSE(overridden->mangohud_configured);
  subject.clear_running_app_for_tests();
  const auto cleared = subject.try_stream_stats_context();
  ASSERT_TRUE(cleared);
  EXPECT_TRUE(cleared->game_uuid.empty());
  EXPECT_FALSE(cleared->mangohud_configured);
  EXPECT_EQ(context->game_uuid, "telemetry-context-test");
  EXPECT_TRUE(context->mangohud_configured);
}
