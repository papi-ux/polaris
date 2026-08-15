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
