/**
 * @file tests/unit/test_entry_handler.cpp
 * @brief Test src/entry_handler.*.
 */
#include "../tests_common.h"
#include "../tests_log_checker.h"
#include "../tests_paths.h"

#include <src/entry_handler.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

TEST(EntryHandlerTests, TheTestLogIsScopedToThisBinaryNotSharedAcrossTargets) {
  // Issue #414: every test binary logged to one shared file, and test_logging.cpp
  // truncates it through logging::clear_log_file(). Under `ctest -j` that made the
  // assertions below fail at random, blaming the code under test for a collision
  // in the harness. The file name has to carry the binary.
  EXPECT_EQ(test_paths::log_file().parent_path(), test_paths::root());
  EXPECT_FALSE(test_paths::log_owner().empty());
  EXPECT_EQ(test_paths::log_file().filename().string(), test_paths::log_owner() + ".log");
  EXPECT_NE(test_paths::log_file().filename().string(), "test_polaris.log");
}

TEST(EntryHandlerTests, LogPublisherDataTest) {
  // call log_publisher_data
  log_publisher_data();

  // check if specific log messages exist
  ASSERT_TRUE(log_checker::line_starts_with(test_paths::log_file().string(), "Info: Package Publisher: "));
  ASSERT_TRUE(log_checker::line_starts_with(test_paths::log_file().string(), "Info: Publisher Website: "));
  ASSERT_TRUE(log_checker::line_starts_with(test_paths::log_file().string(), "Info: Get support: "));
}

TEST(EntryHandlerTests, HostSetupRepairsOnlyRootOwnedConfigDirectories) {
  // A single `sudo polaris` creates the per-user configuration directory as
  // root, and from then on running as the account fails its ownership check
  // with a failed credential save as the only symptom. Host setup is already
  // privileged, so it is the one place that can undo it.
  constexpr std::uint32_t root = 0;
  constexpr std::uint32_t account = 1000;
  constexpr std::uint32_t someone_else = 1001;

  constexpr std::uint32_t private_mode = 0700;
  constexpr std::uint32_t group_writable = 0775;

  EXPECT_EQ(
    config_ownership_action(true, true, false, root, account, private_mode),
    config_ownership_action_e::repair
  ) << "a root-owned directory is exactly the mistake this undoes";

  EXPECT_EQ(
    config_ownership_action(true, true, false, account, account, private_mode),
    config_ownership_action_e::nothing
  ) << "a directory already owned by the account needs no privileged rewrite";

  EXPECT_EQ(
    config_ownership_action(false, false, false, root, account, 0),
    config_ownership_action_e::nothing
  ) << "an absent directory is created later by the account itself";

  // The mode matters as much as the owner, and is the more common way in: a
  // umask of 002 makes directory creation produce 0775 unasked, which the
  // private-state guard refuses even though the account owns it.
  EXPECT_EQ(
    config_ownership_action(true, true, false, account, account, group_writable),
    config_ownership_action_e::repair
  ) << "the account's own directory still needs narrowing when it is group writable";

  EXPECT_EQ(
    config_ownership_action(true, true, false, account, account, 0707),
    config_ownership_action_e::repair
  ) << "other-writable is refused by the same guard and must be repaired too";

  EXPECT_EQ(
    config_ownership_action(true, true, false, account, account, 0755),
    config_ownership_action_e::nothing
  ) << "group and other readable is fine; only writable is refused";

  // The refusals matter more than the repair. A root process rewriting
  // ownership on a path it cannot explain is worse than the problem it fixes.
  EXPECT_EQ(
    config_ownership_action(true, true, false, someone_else, account, private_mode),
    config_ownership_action_e::refuse
  ) << "a third account's directory was not created by this mistake";

  EXPECT_EQ(
    config_ownership_action(true, false, true, root, account, private_mode),
    config_ownership_action_e::refuse
  ) << "a symlink must never be followed by a privileged chown";

  EXPECT_EQ(
    config_ownership_action(true, false, false, root, account, private_mode),
    config_ownership_action_e::refuse
  ) << "something that is not a directory is not this directory";
}


namespace {
  std::atomic<int> probe_sigint_count {0};

  void probe_sigint(int) {
    probe_sigint_count.fetch_add(1);
  }
}  // namespace

TEST(LifetimeTests, ServiceUnitComesFromTheCgroupLeaf) {
  EXPECT_EQ(lifetime::systemd_service_unit_from_cgroup(
              "0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n"),
    "polaris.service");
  EXPECT_EQ(lifetime::systemd_service_unit_from_cgroup(
              "0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
              "app-dev.polaris\\x2dstream.app.Polaris@766ee61ff79e46e1892f35d1866631f6.service\n"),
    "app-dev.polaris\\x2dstream.app.Polaris@766ee61ff79e46e1892f35d1866631f6.service");
  // A scope is not a service: nothing restarts it.
  EXPECT_EQ(lifetime::systemd_service_unit_from_cgroup(
              "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-org.kde.konsole@abc.scope\n"),
    "");
  // Hybrid hosts list v1 controllers first; the unified line decides.
  EXPECT_EQ(lifetime::systemd_service_unit_from_cgroup("12:memory:/user.slice\n0::/system.slice/polaris.service\n"),
    "polaris.service");
  EXPECT_EQ(lifetime::systemd_service_unit_from_cgroup(""), "");
  EXPECT_EQ(lifetime::systemd_service_unit_from_cgroup("0::/\n"), "");
}

TEST(LifetimeTests, OnlyThePackagedUnitRestartsThroughTheServiceManagerUnlessOverridden) {
  EXPECT_TRUE(lifetime::restart_via_service_manager("polaris.service", ""));
  EXPECT_FALSE(lifetime::restart_via_service_manager("app-dev.polaris\\x2dstream.app.Polaris@1.service", ""));
  EXPECT_FALSE(lifetime::restart_via_service_manager("polaris-spaces-ux-rp6-20260915.service", ""));
  EXPECT_FALSE(lifetime::restart_via_service_manager("", ""));
  // A custom unit with Restart= configured opts in; the packaged unit can opt out.
  EXPECT_TRUE(lifetime::restart_via_service_manager("polaris-headless.service", "1"));
  EXPECT_TRUE(lifetime::restart_via_service_manager("", "true"));
  EXPECT_FALSE(lifetime::restart_via_service_manager("polaris.service", "0"));
  EXPECT_FALSE(lifetime::restart_via_service_manager("polaris.service", "false"));
}

TEST(LifetimeTests, ThePackagedUnitRestartsOnTheRestartExitStatus) {
  std::ifstream in(std::filesystem::path(POLARIS_SOURCE_DIR) / "packaging" / "linux" / "polaris.service.in");
  ASSERT_TRUE(in.is_open());
  const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto status = std::to_string(lifetime::RESTART_EXIT_STATUS);
  EXPECT_NE(contents.find("RestartForceExitStatus=" + status), std::string::npos)
    << "the unit must restart Polaris on the status platf::restart() exits with";
  EXPECT_NE(contents.find("SuccessExitStatus=" + status), std::string::npos)
    << "that exit is a clean one, or every console restart leaves the unit failed";
}

#ifndef _WIN32
TEST(LifetimeTests, ARaisedSigintIsDiscardedWhileAnotherThreadIsInsideSystem) {
  // This is why exit_sunshine() cannot rely on std::raise(SIGINT): std::system()
  // ignores SIGINT process-wide until its command returns, and a signal raised
  // for an ignored disposition is dropped rather than queued. Polaris runs
  // shell commands from several threads (previews, session checks, CLI probes).
  probe_sigint_count.store(0);
  const auto previous = std::signal(SIGINT, probe_sigint);
  std::thread holder([]() {
    (void) std::system("sleep 1");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  std::raise(SIGINT);
  holder.join();
  std::signal(SIGINT, previous);
  EXPECT_EQ(probe_sigint_count.load(), 0);
}
#endif

TEST(LifetimeTests, ExitSunshineBeginsShutdownThroughTheRegisteredHandlerNotASignal) {
  lifetime::reset_for_tests();
  std::atomic<int> calls {0};
  lifetime::set_shutdown_request_handler([&calls]() {
    calls.fetch_add(1);
  });
#ifndef _WIN32
  // The same window that swallows a raised SIGINT must not swallow the request.
  std::thread holder([]() {
    (void) std::system("sleep 1");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
#endif
  lifetime::exit_sunshine(lifetime::RESTART_EXIT_STATUS, true, "restart requested");
#ifndef _WIN32
  holder.join();
#endif
  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(lifetime::desired_exit_code.load(), lifetime::RESTART_EXIT_STATUS);
  EXPECT_STREQ(lifetime::shutdown_reason(), "restart requested");
  lifetime::reset_for_tests();
}

TEST(LifetimeTests, RestartInPlaceIsOffUntilRequestedAndAnExternalStopClearsIt) {
  lifetime::reset_for_tests();
  EXPECT_FALSE(lifetime::restart_in_place_pending());
  lifetime::set_restart_in_place_pending(true);
  EXPECT_TRUE(lifetime::restart_in_place_pending());
  lifetime::set_restart_in_place_pending(false);
  EXPECT_FALSE(lifetime::restart_in_place_pending());
  lifetime::reset_for_tests();
}
