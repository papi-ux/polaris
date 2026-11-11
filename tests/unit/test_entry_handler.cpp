/**
 * @file tests/unit/test_entry_handler.cpp
 * @brief Test src/entry_handler.*.
 */
#include "../tests_common.h"
#include "../tests_log_checker.h"
#include "../tests_paths.h"

#include <src/entry_handler.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
  #include <fcntl.h>
  #include <poll.h>
  #include <unistd.h>
#endif

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

TEST(EntryHandlerTests, VersionConstellationIsSevenStarsWithPolarisMarked) {
  const auto art = args::version_constellation();
  EXPECT_EQ(std::count(art.begin(), art.end(), '*'), 7);
  EXPECT_NE(art.find("(*) Polaris"), std::string_view::npos);
  EXPECT_EQ(art.find("Polaris"), art.rfind("Polaris"));
  EXPECT_EQ(art.back(), '\n');
  EXPECT_TRUE(std::all_of(art.begin(), art.end(), [](unsigned char c) {
    return c == '\n' || (c >= 0x20 && c < 0x7f);
  }));
  // Small enough for a narrow terminal.
  std::size_t line_start = 0;
  while (line_start < art.size()) {
    const auto line_end = art.find('\n', line_start);
    EXPECT_LE(line_end - line_start, 40u);
    line_start = line_end + 1;
  }
}

TEST(EntryHandlerTests, VersionTextIsTheTwoLinesScriptsMatch) {
  // scripts/ci/run-steamos-build.sh, scripts/check-release-package-dependencies.py and the package
  // workflows grep for exactly this text; the build summary greps for cuda=<word>.
  const auto text = args::version_text();
  EXPECT_EQ(text.find(std::string {PROJECT_NAME} + " version: " + PROJECT_VERSION + " commit: " + PROJECT_VERSION_COMMIT + "\n"), 0u);
#ifdef __linux__
  EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 2);
  EXPECT_NE(text.find("\nBuild features: cuda="), std::string::npos);
  EXPECT_NE(text.find(" vulkan="), std::string::npos);
#else
  EXPECT_EQ(std::count(text.begin(), text.end(), '\n'), 1);
#endif
  EXPECT_EQ(text.find("config"), std::string::npos);
  EXPECT_EQ(text.back(), '\n');
}

TEST(EntryHandlerTests, AVersionRequestIsReadTheWayTheArgumentsAreParsed) {
  const auto asks = [](std::vector<const char *> arguments) {
    arguments.insert(arguments.begin(), "polaris");
    return args::is_version_request(static_cast<int>(arguments.size()), arguments.data());
  };
  EXPECT_TRUE(asks({"--version"}));
  // A configuration file, an option or a flag may come first; the first --word is the command.
  EXPECT_TRUE(asks({"/etc/polaris/polaris.conf", "--version"}));
  EXPECT_TRUE(asks({"min_log_level=1", "-p", "--version"}));
  EXPECT_TRUE(asks({"--version", "ignored"}));
  EXPECT_FALSE(asks({}));
  EXPECT_FALSE(asks({"/etc/polaris/polaris.conf"}));
  EXPECT_FALSE(asks({"version"}));
  EXPECT_FALSE(asks({"-version"}));
  EXPECT_FALSE(asks({"--versions"}));
  // Another command first, or --help before it, is not a version request.
  EXPECT_FALSE(asks({"--creds", "user", "--version"}));
  EXPECT_FALSE(asks({"--help", "--version"}));
  EXPECT_FALSE(asks({"--setup-host", "--version"}));
}

#ifndef _WIN32
TEST(EntryHandlerTests, VersionPrintsOnlyTheVersionWhenStdoutIsNotATerminal) {
  // CaptureStdout points fd 1 at a file, as a script's redirect does.
  testing::internal::CaptureStdout();
  EXPECT_EQ(args::version(), 0);
  std::fflush(stdout);
  const auto shown = testing::internal::GetCapturedStdout();
  // #647: the version itself, with no configuration dump, no log prefix and no drawing.
  EXPECT_EQ(shown, args::version_text());
  EXPECT_EQ(shown.find("Ursa Minor"), std::string::npos);
}

TEST(EntryHandlerTests, InteractiveTerminalIsOnlyATerminal) {
  int pipe_ends[2] {-1, -1};
  ASSERT_EQ(pipe(pipe_ends), 0);
  EXPECT_FALSE(args::is_interactive_terminal(pipe_ends[0]));
  EXPECT_FALSE(args::is_interactive_terminal(pipe_ends[1]));
  close(pipe_ends[0]);
  close(pipe_ends[1]);
  EXPECT_FALSE(args::is_interactive_terminal(pipe_ends[0]));

  const int controller = posix_openpt(O_RDWR | O_NOCTTY);
  ASSERT_GE(controller, 0) << "this test needs a pseudo-terminal";
  ASSERT_EQ(grantpt(controller), 0);
  ASSERT_EQ(unlockpt(controller), 0);
  const int terminal = open(ptsname(controller), O_RDWR | O_NOCTTY);
  ASSERT_GE(terminal, 0);
  EXPECT_TRUE(args::is_interactive_terminal(terminal));
  close(terminal);
  close(controller);
}

TEST(EntryHandlerTests, VersionDrawsTheConstellationOnATerminal) {
  const int controller = posix_openpt(O_RDWR | O_NOCTTY);
  ASSERT_GE(controller, 0) << "this test needs a pseudo-terminal";
  ASSERT_EQ(grantpt(controller), 0);
  ASSERT_EQ(unlockpt(controller), 0);
  const int terminal = open(ptsname(controller), O_RDWR | O_NOCTTY);
  ASSERT_GE(terminal, 0);

  std::fflush(stdout);
  const int saved_stdout = dup(STDOUT_FILENO);
  ASSERT_GE(saved_stdout, 0);
  ASSERT_EQ(dup2(terminal, STDOUT_FILENO), STDOUT_FILENO);
  const int result = args::version();
  std::fflush(stdout);
  dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
  close(terminal);

  EXPECT_EQ(result, 0);
  const int flags = fcntl(controller, F_GETFL);
  ASSERT_GE(flags, 0);
  ASSERT_EQ(fcntl(controller, F_SETFL, flags | O_NONBLOCK), 0);
  std::string shown(16 * 1024, '\0');
  std::size_t count = 0;
  bool drained = false;
  int read_error = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  // A PTY read may return only part of the output. With the slave closed,
  // drain through EOF (or Linux's EIO) before checking the complete text.
  while (count < shown.size() && std::chrono::steady_clock::now() < deadline) {
    const auto chunk = read(controller, shown.data() + count, shown.size() - count);
    if (chunk > 0) {
      count += static_cast<std::size_t>(chunk);
      continue;
    }
    if (chunk == 0 || errno == EIO) {
      drained = true;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      read_error = errno;
      break;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) {
      break;
    }
    pollfd pending {controller, POLLIN, 0};
    if (poll(&pending, 1, static_cast<int>(remaining)) < 0 && errno != EINTR) {
      read_error = errno;
      break;
    }
  }
  close(controller);
  ASSERT_EQ(read_error, 0);
  ASSERT_TRUE(drained) << "PTY output did not finish within 2 seconds and 16 KiB";
  ASSERT_GT(count, 0);
  shown.resize(static_cast<std::size_t>(count));
  EXPECT_NE(shown.find("(*) Polaris"), std::string::npos);
  EXPECT_NE(shown.find("Ursa Minor"), std::string::npos);
  // The version comes first; a terminal turns each newline into CR LF.
  const auto version_line = std::string {PROJECT_NAME} + " version: " + PROJECT_VERSION + " commit: " + PROJECT_VERSION_COMMIT;
  const auto version_at = shown.find(version_line);
  ASSERT_NE(version_at, std::string::npos);
  EXPECT_LT(version_at, shown.find("(*) Polaris"));
}
#endif

namespace {
  std::string main_source() {
    std::ifstream in {std::filesystem::path(POLARIS_SOURCE_DIR) / "src/main.cpp"};
    return {std::istreambuf_iterator<char> {in}, std::istreambuf_iterator<char> {}};
  }
}  // namespace

// main() is not linked into the tests, so its order is pinned from its source (#647).
TEST(EntryHandlerTests, MainAnswersTheVersionBeforeItLoadsAnyConfiguration) {
  const auto source = main_source();
  const auto version = source.find("if (args::is_version_request(argc, argv)) {\n    return args::version();");
  const auto parse = source.find("config::parse(argc, argv)");
  const auto logging_init = source.find("logging::init(config::sunshine.min_log_level");
  const auto begin_run = source.find("crash_report::begin_run(");
  ASSERT_NE(version, std::string::npos) << "--version is no longer answered on its own";
  ASSERT_NE(parse, std::string::npos);
  ASSERT_NE(logging_init, std::string::npos);
  ASSERT_NE(begin_run, std::string::npos);
  EXPECT_LT(version, parse);
  EXPECT_LT(version, logging_init);
  EXPECT_LT(version, begin_run);
}

TEST(EntryHandlerTests, MainGivesTheRunRecordOnlyToTheProcessThatServes) {
  const auto source = main_source();
  const auto role = source.find("crash_report::classify_run_role(\n    !config::sunshine.cmd.name.empty(),\n    logging::runtime_log_owned_elsewhere()\n  )");
  const auto begin_run = source.find("crash_report::begin_run(platf::appdata(), config::sunshine.log_file, PROJECT_VERSION, run_role);");
  const auto handlers = source.find("crash_report::install_fatal_handlers();");
  const auto dispatch = source.find("cmd_to_func.find(config::sunshine.cmd.name)");
  ASSERT_NE(role, std::string::npos) << "the run record's role is no longer decided from the command and the log owner";
  ASSERT_NE(begin_run, std::string::npos) << "begin_run() is called without the role";
  ASSERT_NE(handlers, std::string::npos);
  ASSERT_NE(dispatch, std::string::npos);
  EXPECT_LT(role, begin_run);
  EXPECT_LT(begin_run, handlers);
  EXPECT_LT(begin_run, dispatch) << "commands are dispatched after the role is known, and never before it";
  EXPECT_EQ(source.find("crash_report::begin_run(", begin_run + 1), std::string::npos) << "one call, with the role";
}

TEST(EntryHandlerTests, MainRecordsAnExitWhereverAStartFailsAndReturns) {
  // After the record says "running", a plain `return <number>;` leaves it that way and the next
  // start reports a kill. The one exception is an unknown command, which is not the run at all.
  const auto source = main_source();
  const auto begin_run = source.find("crash_report::begin_run(");
  const auto clean_exit = source.find("crash_report::note_clean_exit(lifetime::desired_exit_code");
  ASSERT_NE(begin_run, std::string::npos);
  ASSERT_NE(clean_exit, std::string::npos);
  ASSERT_LT(begin_run, clean_exit);
  const auto body = source.substr(begin_run, clean_exit - begin_run);
  std::vector<std::string> bare;
  const std::regex bare_return {R"(return\s+-?[0-9]+\s*;)"};
  for (std::sregex_iterator found {body.begin(), body.end(), bare_return}, end; found != end; ++found) {
    bare.push_back(found->str());
  }
  EXPECT_EQ(bare, std::vector<std::string> {"return 7;"});
  EXPECT_NE(body.find("return crash_report::note_failed_start(-1, \"start failed: HTTP interface failed to initialize\");"), std::string::npos);
}

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

TEST(SetupHostAssets, EveryShippedVersionIncludingTheCurrentOneIsRecognised) {
  for (const auto *name : {"60-polaris.rules", "60-polaris.conf"}) {
    std::ifstream in(std::filesystem::path(POLARIS_SOURCE_DIR) / "src_assets/linux/misc" / name, std::ios::binary);
    ASSERT_TRUE(in) << name;
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_TRUE(is_shipped_host_asset_version(name, contents))
      << name << " changed: add its sha256 to the shipped versions in is_shipped_host_asset_version";
    EXPECT_FALSE(is_shipped_host_asset_version(name, contents + "# local edit\n")) << name;
  }
  EXPECT_FALSE(is_shipped_host_asset_version("60-polaris.rules", ""));
  EXPECT_FALSE(is_shipped_host_asset_version("99-other.rules", "KERNEL==\"uinput\"\n"));
}
