/**
 * @file tests/tests_main.cpp
 * @brief Entry point definition.
 */
#include "tests_common.h"
#include "tests_environment.h"
#include "tests_events.h"
#include "tests_paths.h"

int main(int argc, char **argv) {
  // Claim a log file named after this binary before anything opens one. Every
  // target logs into the same directory, so a shared name lets parallel ctest
  // runs truncate each other's log (issue #414).
  if (argc > 0 && argv[0]) {
    test_paths::set_log_owner(argv[0]);
  }
  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new PolarisEnvironment);
  testing::UnitTest::GetInstance()->listeners().Append(new PolarisEventListener);
  return RUN_ALL_TESTS();
}
