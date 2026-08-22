#include "../tests_common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <src/process.h>

#if defined(__linux__)
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace {
  std::string read_source_file(const char *relative_path) {
    const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / relative_path;
    std::ifstream in(path);
    if (!in) {
      return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  std::string source_between(
    const std::string &source,
    std::string_view begin_marker,
    std::string_view end_marker
  ) {
    const auto begin = source.find(begin_marker);
    if (begin == std::string::npos) {
      return {};
    }
    const auto end = source.find(end_marker, begin + begin_marker.size());
    if (end == std::string::npos) {
      return {};
    }
    return source.substr(begin, end - begin);
  }
}

#if defined(__linux__)
TEST(SteamShutdownStateMachineTests, PersistentProcessActivityUsesOneBoundedDeadline) {
  const auto result = proc::run_steam_shutdown_quiescence_scenario_for_tests(
    {true},
    {false},
    std::chrono::milliseconds(500),
    std::chrono::milliseconds(100)
  );

  EXPECT_FALSE(result.quiescent);
  EXPECT_EQ(result.elapsed, std::chrono::milliseconds(500));
  EXPECT_GE(result.process_checks, 5u);
  EXPECT_EQ(result.fifo_checks, 0u);
}

TEST(SteamShutdownStateMachineTests, PersistentFifoListenerUsesTheSameBoundedDeadline) {
  const auto result = proc::run_steam_shutdown_quiescence_scenario_for_tests(
    {false},
    {true},
    std::chrono::milliseconds(500),
    std::chrono::milliseconds(100)
  );

  EXPECT_FALSE(result.quiescent);
  EXPECT_EQ(result.elapsed, std::chrono::milliseconds(500));
  EXPECT_GE(result.process_checks, 5u);
  EXPECT_GE(result.fifo_checks, 5u);
}

TEST(SteamShutdownStateMachineTests, ProcessExitThenReleasedFifoSucceeds) {
  const auto result = proc::run_steam_shutdown_quiescence_scenario_for_tests(
    {true, false, false},
    {false},
    std::chrono::milliseconds(500),
    std::chrono::milliseconds(100)
  );

  EXPECT_TRUE(result.quiescent);
  EXPECT_EQ(result.elapsed, std::chrono::milliseconds(100));
  EXPECT_EQ(result.process_checks, 3u);
  EXPECT_EQ(result.fifo_checks, 1u);
}

TEST(SteamShutdownStateMachineTests, ProcessReappearanceAfterFifoProbeIsRevalidated) {
  const auto result = proc::run_steam_shutdown_quiescence_scenario_for_tests(
    {false, true, false, false},
    {false},
    std::chrono::milliseconds(500),
    std::chrono::milliseconds(100)
  );

  EXPECT_TRUE(result.quiescent);
  EXPECT_EQ(result.elapsed, std::chrono::milliseconds(100));
  EXPECT_EQ(result.process_checks, 4u);
  EXPECT_EQ(result.fifo_checks, 2u);
}

TEST(SteamShutdownStateMachineTests, ProcessScanErrorsRemainFailClosed) {
  EXPECT_TRUE(proc::desktop_steam_proc_open_error_fails_closed_for_tests());
}

TEST(SteamShutdownStateMachineTests, EmptyOrUnsetHomeFallsBackToAccountHome) {
  const std::optional<std::string> account_home {"/srv/polaris-user"};
  const std::optional<std::string> expected {"/srv/polaris-user/.steam/steam.pipe"};

  EXPECT_EQ(proc::steam_instance_pipe_path_for_tests(std::nullopt, account_home), expected);
  EXPECT_EQ(proc::steam_instance_pipe_path_for_tests(std::string {}, account_home), expected);
}

TEST(SteamShutdownStateMachineTests, EnvironmentHomeTakesPrecedence) {
  EXPECT_EQ(
    proc::steam_instance_pipe_path_for_tests(
      std::string {"/srv/environment-home"},
      std::string {"/srv/account-home"}
    ),
    std::optional<std::string> {"/srv/environment-home/.steam/steam.pipe"}
  );
}

TEST(SteamShutdownStateMachineTests, MissingHomeAndAccountHomeFailsClosed) {
  EXPECT_EQ(proc::steam_instance_pipe_path_for_tests(std::nullopt, std::nullopt), std::nullopt);
  EXPECT_EQ(
    proc::steam_instance_pipe_path_for_tests(std::string {}, std::string {}),
    std::nullopt
  );
}

TEST(SteamShutdownStateMachineTests, FifoProbeTracksReaderLifetime) {
  namespace fs = std::filesystem;
  const auto unique = std::to_string(::getpid()) + "-" + std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count()
  );
  const auto test_dir = fs::temp_directory_path() / ("polaris-steam-shutdown-" + unique);
  ASSERT_TRUE(fs::create_directories(test_dir));
  auto cleanup = util::fail_guard([&test_dir]() {
    std::error_code ec;
    fs::remove_all(test_dir, ec);
  });

  const auto pipe_path = test_dir / "steam.pipe";
  ASSERT_EQ(mkfifo(pipe_path.c_str(), 0600), 0);
  EXPECT_FALSE(proc::steam_instance_pipe_listener_active_for_tests(pipe_path.string()));

  const int reader = open(pipe_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  ASSERT_GE(reader, 0);
  auto close_reader = util::fail_guard([reader]() {
    close(reader);
  });
  EXPECT_TRUE(proc::steam_instance_pipe_listener_active_for_tests(pipe_path.string()));

  close_reader.disable();
  close(reader);
  EXPECT_FALSE(proc::steam_instance_pipe_listener_active_for_tests(pipe_path.string()));
  EXPECT_FALSE(proc::steam_instance_pipe_listener_active_for_tests(
    (test_dir / "missing-steam.pipe").string()
  ));
}

TEST(SteamShutdownStateMachineTests, ProductionShutdownUsesSingleMonotonicQuiescenceLoop) {
  const auto source = read_source_file("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto shutdown = source_between(
    source,
    "bool request_desktop_steam_shutdown_for_private_stream()",
    "#endif"
  );
  ASSERT_FALSE(shutdown.empty());

  const auto path_resolution = shutdown.find("const auto pipe_path = steam_instance_pipe_path()");
  const auto fail_closed = shutdown.find("if (!pipe_path)", path_resolution);
  const auto shutdown_command = shutdown.find("canonical_steam_shutdown_command", fail_closed);
  ASSERT_NE(path_resolution, std::string::npos);
  ASSERT_NE(fail_closed, std::string::npos);
  ASSERT_NE(shutdown_command, std::string::npos);
  EXPECT_LT(path_resolution, fail_closed);
  EXPECT_LT(fail_closed, shutdown_command);
  EXPECT_NE(shutdown.find("wait_for_desktop_steam_quiescence("), std::string::npos);
  EXPECT_EQ(shutdown.find("for (int i = 0; i < 50; ++i)"), std::string::npos);
}

TEST(SteamShutdownStateMachineTests, PrivateCageGracefulShutdownRequiresCompleteSoloExactOwnership) {
  EXPECT_TRUE(proc::should_request_session_owned_steam_graceful_shutdown_for_tests(
    true, 1, 0, 0, true
  ));
  EXPECT_FALSE(proc::should_request_session_owned_steam_graceful_shutdown_for_tests(
    false, 1, 0, 0, true
  ));
  EXPECT_FALSE(proc::should_request_session_owned_steam_graceful_shutdown_for_tests(
    true, 0, 0, 0, true
  ));
  EXPECT_FALSE(proc::should_request_session_owned_steam_graceful_shutdown_for_tests(
    true, 2, 0, 0, true
  ));
  EXPECT_FALSE(proc::should_request_session_owned_steam_graceful_shutdown_for_tests(
    true, 1, 1, 0, true
  ));
  EXPECT_FALSE(proc::should_request_session_owned_steam_graceful_shutdown_for_tests(
    true, 1, 0, 1, true
  ));
  EXPECT_FALSE(proc::should_request_session_owned_steam_graceful_shutdown_for_tests(
    true, 1, 0, 0, false
  ));
}

TEST(SteamShutdownStateMachineTests, GracefulAttemptDoesNotWaitWhenDispatchFails) {
  const auto result = proc::run_private_steam_graceful_shutdown_scenario_for_tests(false, true);
  EXPECT_FALSE(result.root_exited);
  EXPECT_EQ(result.request_calls, 1U);
  EXPECT_EQ(result.wait_calls, 0U);
}

TEST(SteamShutdownStateMachineTests, GracefulAttemptFallsBackWhenExactRootDoesNotExit) {
  const auto result = proc::run_private_steam_graceful_shutdown_scenario_for_tests(true, false);
  EXPECT_FALSE(result.root_exited);
  EXPECT_EQ(result.request_calls, 1U);
  EXPECT_EQ(result.wait_calls, 1U);
}

TEST(SteamShutdownStateMachineTests, GracefulAttemptSucceedsOnlyAfterExactRootExit) {
  const auto result = proc::run_private_steam_graceful_shutdown_scenario_for_tests(true, true);
  EXPECT_TRUE(result.root_exited);
  EXPECT_EQ(result.request_calls, 1U);
  EXPECT_EQ(result.wait_calls, 1U);
}

TEST(SteamShutdownStateMachineTests, ProductionPrivateCageAttemptsScopedGracefulBeforePidfdFallback) {
  const auto source = read_source_file("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto cleanup = source_between(
    source,
    "bool terminate_session_owned_steam_before_cage_stop_impl(",
    "bool terminate_gamescope_attached_session_clients("
  );
  ASSERT_FALSE(cleanup.empty());

  const auto root_cardinality_guard = cleanup.find("if (ownership.roots.size() != 1)");
  const auto root_front_access = cleanup.find("ownership.roots.front()");
  const auto no_unowned_guard = cleanup.find("ownership.unowned.empty()");
  const auto graceful_request = cleanup.find("attempt_session_owned_steam_graceful_shutdown(");
  const auto exact_pidfd_fallback = cleanup.find("terminate_pidfds(ownership.roots");
  ASSERT_NE(root_cardinality_guard, std::string::npos);
  ASSERT_NE(root_front_access, std::string::npos);
  ASSERT_NE(no_unowned_guard, std::string::npos);
  ASSERT_NE(graceful_request, std::string::npos);
  ASSERT_NE(exact_pidfd_fallback, std::string::npos);
  EXPECT_LT(root_cardinality_guard, root_front_access);
  EXPECT_LT(no_unowned_guard, graceful_request);
  EXPECT_LT(graceful_request, exact_pidfd_fallback);
  EXPECT_NE(
    cleanup.find("wait_for_pidfds_exit(", graceful_request),
    std::string::npos
  );
  EXPECT_NE(
    cleanup.find("private_steam_native_shutdown_timeout", graceful_request),
    std::string::npos
  );
}

TEST(SteamShutdownStateMachineTests, ProductionPrivateSteamRequestUsesSessionEnvironmentAndLiveFifo) {
  const auto source = read_source_file("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto request = source_between(
    source,
    "bool proc_t::request_session_owned_steam_graceful_shutdown_before_cage_stop()",
    "bool proc_t::terminate_session_owned_steam_before_cage_stop()"
  );
  ASSERT_FALSE(request.empty());
  EXPECT_NE(request.find("_env.find(\"HOME\")"), std::string::npos);
  EXPECT_NE(request.find("steam_instance_pipe_listener_active"), std::string::npos);
  EXPECT_NE(request.find("canonical_steam_shutdown_command"), std::string::npos);
  EXPECT_NE(request.find("platf::run_command"), std::string::npos);
  EXPECT_NE(request.find(", _env, nullptr"), std::string::npos);
  EXPECT_NE(request.find("child.detach()"), std::string::npos);
}

TEST(SteamShutdownStateMachineTests, PostShutdownPolicyRechecksLiveProcessState) {
  const auto source = read_source_file("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto policy = source_between(
    source,
    "desktop_launch_safety_policy_t resolve_desktop_launch_safety_policy_after_shutdown(",
    "nlohmann::json desktop_launch_safety_policy_to_json("
  );
  ASSERT_FALSE(policy.empty());
  EXPECT_NE(policy.find("desktop_steam_client_active_impl()"), std::string::npos);
}

TEST(SteamShutdownStateMachineTests, BothNvhttpRoutesGateOnLiveRefreshedPolicy) {
  const auto source = read_source_file("src/nvhttp.cpp");
  ASSERT_FALSE(source.empty());

  const auto launch_route = source_between(
    source,
    "void launch(bool &host_audio",
    "void resume(bool &host_audio"
  );
  const auto api_route = source_between(
    source,
    "auto polarisLaunchGame =",
    "// Toggle MangoHud for a game"
  );
  ASSERT_FALSE(launch_route.empty());
  ASSERT_FALSE(api_route.empty());

  const auto launch_refresh = launch_route.find(
    "launch_policy = proc::resolve_desktop_launch_safety_policy_after_shutdown("
  );
  const auto launch_gate = launch_route.find(
    "if (launch_policy.recommendedAction == \"refuse_private_stream\")",
    launch_refresh
  );
  ASSERT_NE(launch_refresh, std::string::npos);
  ASSERT_NE(launch_gate, std::string::npos);
  EXPECT_LT(launch_refresh, launch_gate);
  EXPECT_EQ(launch_route.find("refreshed_launch_policy"), std::string::npos);

  const auto api_refresh = api_route.find(
    "launch_policy = proc::resolve_desktop_launch_safety_policy_after_shutdown("
  );
  const auto api_gate = api_route.find(
    "if (launch_policy.recommendedAction == \"refuse_private_stream\")",
    api_refresh
  );
  ASSERT_NE(api_refresh, std::string::npos);
  ASSERT_NE(api_gate, std::string::npos);
  EXPECT_LT(api_refresh, api_gate);
}
#endif
