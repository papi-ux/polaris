/**
 * @file tests/unit/platform/test_linux_process_argv.cpp
 * @brief Regression coverage for shell-free Linux child process launches.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include "../../tests_paths.h"
#include "src/platform/common.h"
#include "src/platform/linux/misc.h"
#include "src/platform/linux/stream_runtime.h"

#include <boost/filesystem/path.hpp>
#include <boost/process/v1/environment.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

TEST(LinuxCommandLine, SingleQuotedTokensSplitLikeAShell) {
  const auto argv = platf::posix_command_argv("'/tmp/Eden (x86_64).AppImage' -f -g '/roms/it'\\''s here/Game (USA).nsp'");
  ASSERT_TRUE(argv.has_value());
  EXPECT_EQ(*argv, (std::vector<std::string> {"/tmp/Eden (x86_64).AppImage", "-f", "-g", "/roms/it's here/Game (USA).nsp"}));

  const auto escaped = platf::posix_command_argv("eden -g /roms/Game\\ One.nsp");
  ASSERT_TRUE(escaped.has_value());
  EXPECT_EQ(escaped->back(), "/roms/Game One.nsp");
}

TEST(LinuxCommandLine, PlainAndDoubleQuotedCommandsKeepBoostsSplitter) {
  EXPECT_FALSE(platf::posix_command_argv("eden -f -g /roms/Game.nsp").has_value());
  EXPECT_FALSE(platf::posix_command_argv("eden -f -g \"/roms/Game One.nsp\"").has_value());
  EXPECT_FALSE(platf::posix_command_argv("").has_value());
}

TEST(LinuxCommandLine, TheIsolationWrapperKeepsItsShellChildWhole) {
  const auto argv = platf::posix_command_argv("'/usr/bin/bwrap' --bind / / -- /bin/sh -lc 'eden -f -g '\\''/r/a b.nsp'\\'''");
  ASSERT_TRUE(argv.has_value());
  EXPECT_EQ(argv->front(), "/usr/bin/bwrap");
  EXPECT_EQ(argv->back(), "eden -f -g '/r/a b.nsp'");
}

TEST(LinuxRunCommand, QuotedArgumentsReachTheChildIntact) {
  namespace fs = std::filesystem;
  const auto root = test_paths::root() / "run_command_quotes" / "dir with space";
  fs::remove_all(root.parent_path());
  fs::create_directories(root);
  const auto script = root / "fake emu";
  const auto log = root / "argv.log";
  {
    std::ofstream out(script);
    out << "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"" << log.string() << "\"\n";
  }
  fs::permissions(script, fs::perms::owner_all, fs::perm_options::replace);
  const auto rom = root / "Game One (USA).nsp";
  const auto cmd = "'" + script.string() + "' -f -g '" + rom.string() + "'";

  boost::filesystem::path working_dir {root.string()};
  auto env = boost::this_process::environment();
  std::error_code ec;
  auto child = platf::run_command(false, true, cmd, working_dir, env, nullptr, ec, nullptr);
  ASSERT_FALSE(ec) << ec.message();
  child.wait();
  EXPECT_EQ(child.exit_code(), 0);

  std::ifstream in(log);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  EXPECT_EQ(lines, (std::vector<std::string> {"-f", "-g", rom.string()}));
  fs::remove_all(root.parent_path());
}

TEST(LinuxRunCommand, AMissingProgramReportsAnError) {
  boost::filesystem::path working_dir {"/tmp"};
  auto env = boost::this_process::environment();
  std::error_code ec;
  auto child = platf::run_command(false, true, "polaris-no-such-emulator -g 'x y'", working_dir, env, nullptr, ec, nullptr);
  EXPECT_TRUE(ec);
  EXPECT_FALSE(child.valid());
}

TEST(GamescopeRuntime, ClosesInheritedDescriptorsBeforeExec) {
  EXPECT_TRUE(stream_runtime::gamescope_runtime_closes_inherited_descriptors_for_tests());
}

#endif
