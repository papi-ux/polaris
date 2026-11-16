/**
 * @file tests/unit/test_epic_provider_lock_order.cpp
 * @brief An Epic provider change saves under its session snapshot and reloads after it (#696).
 *
 * Before #696 the console's provider change reloaded apps.json while it still held the session
 * snapshot, and the reload ended any session first. Ending one waits in the lifecycle gate for every
 * snapshot to end, so the request waited on itself and nothing could launch or stop again. These
 * tests drive a real proc_t and its lifecycle gate through the order the endpoint uses now. A stop
 * the test blocks runs on its own thread and is timed out, so a regression fails instead of hanging.
 */
#include <src/confighttp.h>
#include <src/process.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
  using namespace std::chrono_literals;

  // A stop as proc::refresh() began one: proc_t::stop() first waits in the gate for any snapshot
  // to end. The returned future must outlive the snapshot, which is what lets a blocked stop finish.
  std::future<void> start_stop(proc::proc_t &subject) {
    return std::async(std::launch::async, [&subject] {
      if (subject.begin_session_stop_for_tests()) subject.finish_session_stop_for_tests(true);
    });
  }

  // Asked from another thread, because a thread may not try_lock a std::mutex it already holds.
  bool lock_is_free(std::mutex &mutex) {
    return std::async(std::launch::async, [&mutex] {
      if (!mutex.try_lock()) return false;
      mutex.unlock();
      return true;
    }).get();
  }

  // A snapshot taken here ends at once; it is only a probe of whether one could be taken.
  bool snapshot_is_free(proc::proc_t &subject) {
    return subject.try_session_status_guard().owns_snapshot();
  }
}  // namespace

TEST(EpicProviderLockOrderTests, AStopBegunFromTheReloadIsAdmitted) {
  // `change` runs where the old order reloaded: a stop begun there waits for the snapshot this
  // thread holds, and only the snapshot ending lets it go. Where the reload runs now, it goes.
  proc::proc_t subject;
  std::mutex apps_file;
  auto snapshot = subject.try_session_status_guard();
  ASSERT_TRUE(snapshot.owns_snapshot());
  std::optional<std::future<void>> from_change;
  std::optional<std::future<void>> from_reload;
  bool reloaded = false;
  const auto refused = confighttp::change_apps_file_under_snapshot(std::move(snapshot), apps_file, [&]() -> const char * {
    from_change = start_stop(subject);
    EXPECT_EQ(from_change->wait_for(100ms), std::future_status::timeout) << "a stop began under the provider change's own snapshot";
    return nullptr;
  }, [&] {
    reloaded = true;
    EXPECT_EQ(from_change->wait_for(5s), std::future_status::ready) << "the snapshot was still held when the reload ran";
    from_reload = start_stop(subject);
    EXPECT_EQ(from_reload->wait_for(5s), std::future_status::ready) << "a stop from the reload waited on the provider change";
  });
  EXPECT_EQ(refused, nullptr);
  EXPECT_TRUE(reloaded);
}

TEST(EpicProviderLockOrderTests, TheReloadKeepsTheAppsFileLockButNotTheSnapshot) {
  proc::proc_t subject;
  std::mutex apps_file;
  auto snapshot = subject.try_session_status_guard();
  ASSERT_TRUE(snapshot.owns_snapshot());
  bool reloaded = false;
  const auto refused = confighttp::change_apps_file_under_snapshot(std::move(snapshot), apps_file, [&]() -> const char * {
    EXPECT_FALSE(lock_is_free(apps_file));
    EXPECT_FALSE(snapshot_is_free(subject));
    return nullptr;
  }, [&] {
    reloaded = true;
    // The reload reads apps.json back, so no other writer may be halfway through rewriting it.
    EXPECT_FALSE(lock_is_free(apps_file));
    EXPECT_TRUE(snapshot_is_free(subject));
  });
  EXPECT_EQ(refused, nullptr);
  EXPECT_TRUE(reloaded);
  EXPECT_TRUE(lock_is_free(apps_file));
  EXPECT_TRUE(snapshot_is_free(subject));
}

TEST(EpicProviderLockOrderTests, ARefusalOrAFailureReloadsNothingAndReleasesBoth) {
  proc::proc_t subject;
  std::mutex apps_file;
  bool reloaded = false;
  const auto reload = [&] { reloaded = true; };

  auto snapshot = subject.try_session_status_guard();
  ASSERT_TRUE(snapshot.owns_snapshot());
  const auto refused = confighttp::change_apps_file_under_snapshot(std::move(snapshot), apps_file, []() -> const char * {
    return "That library entry no longer exists.";
  }, reload);
  EXPECT_STREQ(refused, "That library entry no longer exists.");
  EXPECT_FALSE(reloaded);
  EXPECT_TRUE(lock_is_free(apps_file));
  EXPECT_TRUE(snapshot_is_free(subject));

  auto again = subject.try_session_status_guard();
  ASSERT_TRUE(again.owns_snapshot());
  EXPECT_THROW(confighttp::change_apps_file_under_snapshot(std::move(again), apps_file, []() -> const char * {
    throw std::runtime_error("save failed");
  }, reload), std::runtime_error);
  EXPECT_FALSE(reloaded);
  EXPECT_TRUE(lock_is_free(apps_file));
  EXPECT_TRUE(snapshot_is_free(subject));
}

TEST(EpicProviderLockOrderTests, TheEndpointSavesThroughTheOrderAndEndsNoSession) {
  // The tests above prove the order; this pins the provider endpoint to it.
  std::ifstream stream(std::filesystem::path(POLARIS_SOURCE_DIR) / "src/confighttp.cpp");
  ASSERT_TRUE(stream.good());
  const std::string source {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  const auto start = source.find("void setEpicProvider(");
  ASSERT_NE(start, std::string::npos);
  const auto end = source.find("void epicSession(", start);
  ASSERT_NE(end, std::string::npos);
  const auto body = source.substr(start, end - start);

  const auto busy = body.find("A session is changing. Try the provider change when it is idle.");
  const auto running = body.find("End this game's session before changing its provider.");
  const auto save = body.find("change_apps_file_under_snapshot(std::move(guard), apps_file_mutex(),");
  const auto reload = body.find("proc::refresh(config::stream.file_apps, false);");
  ASSERT_NE(busy, std::string::npos);
  ASSERT_NE(running, std::string::npos);
  ASSERT_NE(save, std::string::npos);
  ASSERT_NE(reload, std::string::npos);
  EXPECT_LT(busy, save);
  EXPECT_LT(running, save);
  EXPECT_LT(save, reload);
  // One reload, and it ends no session; the order owns the apps-file lock.
  EXPECT_EQ(body.find("proc::refresh("), reload);
  EXPECT_EQ(body.find("proc::refresh(", reload + 1), std::string::npos);
  EXPECT_EQ(body.find("apps_lock"), std::string::npos);
}
