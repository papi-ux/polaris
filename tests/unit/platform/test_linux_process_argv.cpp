/**
 * @file tests/unit/platform/test_linux_process_argv.cpp
 * @brief Regression coverage for shell-free Linux child process launches.
 */
#include <gtest/gtest.h>
#include <thread>

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

TEST(LinuxProcessArgv, CapturesOutputWithoutInterpretingArguments) {
  const auto result = platf::run_process_argv_capture(
    {"printf", "%s", "output.HDMI-A-1; exit 99"}
  );
  EXPECT_EQ(result.exit_status, 0);
  EXPECT_FALSE(result.timed_out);
  EXPECT_FALSE(result.truncated);
  EXPECT_EQ(result.output, "output.HDMI-A-1; exit 99");
}

TEST(LinuxProcessArgv, BoundsCapturedOutput) {
  const auto result = platf::run_process_argv_capture(
    {"printf", "123456789"},
    std::chrono::seconds {1},
    4
  );
  EXPECT_EQ(result.exit_status, 0);
  EXPECT_TRUE(result.truncated);
  EXPECT_EQ(result.output, "1234");
}

TEST(LinuxProcessArgv, TerminatesCapturedProcessAtDeadline) {
  const auto result = platf::run_process_argv_capture(
    {"sleep", "5"},
    std::chrono::milliseconds {20}
  );
  EXPECT_TRUE(result.timed_out);
  EXPECT_EQ(result.exit_status, 124);
}

TEST(GamescopeRuntime, ClosesInheritedDescriptorsBeforeExec) {
  EXPECT_TRUE(stream_runtime::gamescope_runtime_closes_inherited_descriptors_for_tests());
}

TEST(LinuxProcessArgv, AlreadyCancelledCommandsDoNotLaunch) {
  std::stop_source stop;
  stop.request_stop();
  const auto result = platf::run_process_argv_capture({"printf", "must not run"},
    std::chrono::seconds(5), 4096, stop.get_token());
  EXPECT_TRUE(result.cancelled);
  EXPECT_EQ(result.exit_status, 125);
  EXPECT_TRUE(result.output.empty());
}

TEST(LinuxProcessArgv, CancellationCannotBeStarvedByContinuousOutput) {
  std::stop_source stop;
  std::jthread cancel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.request_stop();
  });
  const auto started = std::chrono::steady_clock::now();
  const auto result = platf::run_process_argv_capture({"yes"},
    std::chrono::seconds(10), 64, stop.get_token());
  EXPECT_TRUE(result.cancelled);
  EXPECT_FALSE(result.timed_out);
  EXPECT_LE(result.output.size(), 64U);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
}

#endif
