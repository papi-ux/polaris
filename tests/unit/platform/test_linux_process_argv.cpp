/**
 * @file tests/unit/platform/test_linux_process_argv.cpp
 * @brief Regression coverage for shell-free Linux child process launches.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include "src/platform/linux/misc.h"
#include "src/platform/linux/stream_runtime.h"

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

TEST(GamescopeRuntime, ClosesInheritedDescriptorsBeforeExec) {
  EXPECT_TRUE(stream_runtime::gamescope_runtime_closes_inherited_descriptors_for_tests());
}

#endif
