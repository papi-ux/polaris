/**
 * @file tests/unit/test_entry_handler.cpp
 * @brief Test src/entry_handler.*.
 */
#include "../tests_common.h"
#include "../tests_log_checker.h"
#include "../tests_paths.h"

#include <src/entry_handler.h>

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

