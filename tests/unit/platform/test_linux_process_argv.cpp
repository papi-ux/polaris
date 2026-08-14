/**
 * @file tests/unit/platform/test_linux_process_argv.cpp
 * @brief Regression coverage for shell-free Linux child process launches.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include "src/platform/linux/misc.h"

TEST(LinuxProcessArgv, PreservesShellMetacharactersAsLiteralArguments) {
  constexpr auto value = "output.HDMI-A-1; exit 99";
  EXPECT_EQ(platf::run_process_argv({"test", value, "=", value}), 0);
}

TEST(LinuxProcessArgv, ReturnsChildExitStatus) {
  EXPECT_EQ(platf::run_process_argv({"sh", "-c", "exit 7"}), 7);
}

TEST(LinuxProcessArgv, ReportsMissingExecutable) {
  EXPECT_EQ(platf::run_process_argv({"polaris-command-that-does-not-exist"}), 127);
}

#endif
