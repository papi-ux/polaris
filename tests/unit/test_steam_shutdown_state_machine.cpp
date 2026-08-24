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
  #include <poll.h>
  #include <signal.h>
  #include <sys/stat.h>
  #include <sys/syscall.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace {
#if defined(__linux__)
  struct child_guard_t {
    pid_t pid = -1;
    bool reaped = false;

    ~child_guard_t() {
      if (pid <= 0 || reaped) {
        return;
      }
      (void) kill(pid, SIGKILL);
      int status = 0;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    pid_t wait(int *status, int options) {
      const auto result = waitpid(pid, status, options);
      if (result == pid) {
        reaped = true;
      }
      return result;
    }
  };

  struct fd_guard_t {
    int fd = -1;
    ~fd_guard_t() {
      if (fd >= 0) {
        close(fd);
      }
    }
  };

  bool wait_pidfd_exit(int fd, std::chrono::milliseconds timeout) {
    pollfd descriptor {fd, POLLIN, 0};
    int result = -1;
    do {
      result = poll(&descriptor, 1, static_cast<int>(timeout.count()));
    } while (result < 0 && errno == EINTR);
    return result == 1 && (descriptor.revents & POLLIN) != 0;
  }
#endif

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

TEST(SteamShutdownStateMachineTests, GracefulAttemptDoesNotRequestShutdownWhenAppStopFails) {
  const auto result = proc::run_private_steam_graceful_shutdown_scenario_for_tests(
    false, true, true, true
  );
  EXPECT_FALSE(result.root_exited);
  EXPECT_EQ(result.app_stop_calls, 1U);
  EXPECT_EQ(result.app_wait_calls, 0U);
  EXPECT_EQ(result.request_calls, 0U);
  EXPECT_EQ(result.wait_calls, 0U);
}

TEST(SteamShutdownStateMachineTests, GracefulAttemptDoesNotRequestShutdownBeforeAppQuiesces) {
  const auto result = proc::run_private_steam_graceful_shutdown_scenario_for_tests(
    true, false, true, true
  );
  EXPECT_FALSE(result.root_exited);
  EXPECT_EQ(result.app_stop_calls, 1U);
  EXPECT_EQ(result.app_wait_calls, 1U);
  EXPECT_EQ(result.request_calls, 0U);
  EXPECT_EQ(result.wait_calls, 0U);
}

TEST(SteamShutdownStateMachineTests, GracefulAttemptDoesNotWaitWhenDispatchFails) {
  const auto result = proc::run_private_steam_graceful_shutdown_scenario_for_tests(
    true, true, false, true
  );
  EXPECT_FALSE(result.root_exited);
  EXPECT_EQ(result.app_stop_calls, 1U);
  EXPECT_EQ(result.app_wait_calls, 1U);
  EXPECT_EQ(result.request_calls, 1U);
  EXPECT_EQ(result.wait_calls, 0U);
}

TEST(SteamShutdownStateMachineTests, GracefulAttemptFallsBackWhenExactRootDoesNotExit) {
  const auto result = proc::run_private_steam_graceful_shutdown_scenario_for_tests(
    true, true, true, false
  );
  EXPECT_FALSE(result.root_exited);
  EXPECT_EQ(result.app_stop_calls, 1U);
  EXPECT_EQ(result.app_wait_calls, 1U);
  EXPECT_EQ(result.request_calls, 1U);
  EXPECT_EQ(result.wait_calls, 1U);
}

TEST(SteamShutdownStateMachineTests, GracefulAttemptSucceedsOnlyAfterAppQuiescenceAndExactRootExit) {
  const auto result = proc::run_private_steam_graceful_shutdown_scenario_for_tests(
    true, true, true, true
  );
  EXPECT_TRUE(result.root_exited);
  EXPECT_EQ(result.app_stop_calls, 1U);
  EXPECT_EQ(result.app_wait_calls, 1U);
  EXPECT_EQ(result.request_calls, 1U);
  EXPECT_EQ(result.wait_calls, 1U);
}

TEST(SteamShutdownStateMachineTests, SteamLaunchMarkerMatchesSplitArgvWithExactBoundaries) {
  static constexpr char split_cmdline[] =
    "reaper\0SteamLaunch\0AppId=4242\0--\0";
  const std::string_view split_view {split_cmdline, sizeof(split_cmdline) - 1};

  EXPECT_TRUE(proc::steam_launch_cmdline_matches_appid_for_tests(split_view, "4242"));
  EXPECT_TRUE(proc::steam_launch_cmdline_matches_appid_for_tests(
    "reaper SteamLaunch AppId=4242 --",
    "4242"
  ));
  EXPECT_FALSE(proc::steam_launch_cmdline_matches_appid_for_tests(
    "reaper NotSteamLaunch AppId=4242 --",
    "4242"
  ));
  EXPECT_FALSE(proc::steam_launch_cmdline_matches_appid_for_tests(
    "reaper SteamLaunch AppId=42420 --",
    "4242"
  ));
  EXPECT_FALSE(proc::steam_launch_cmdline_matches_appid_for_tests(split_view, ""));
  EXPECT_FALSE(proc::steam_launch_cmdline_matches_appid_for_tests(split_view, "42x2"));
}

TEST(SteamShutdownStateMachineTests, EmptySteamAppRootStillUsesPinnedStopBarrier) {
  const auto source = read_source_file("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto quiescence = source_between(
    source,
    "bool quiesce_session_owned_steam_app_before_native_shutdown(\n"
    "      const proc::ctx_t &app,\n"
    "      std::string_view session_instance_id,\n"
    "      const boost::process::v1::environment &env\n"
    "    ) {\n"
    "      const auto appid",
    "bool dispatch_steam_big_picture_action("
  );
  ASSERT_FALSE(quiescence.empty());

  const auto log_snapshot = quiescence.find("snapshot_steam_game_process_log(");
  const auto roots_snapshot = quiescence.find("private_steam_app_root_snapshot(");
  const auto stopped_barrier = quiescence.find("wait_for_steam_app_stopped_event(");
  ASSERT_NE(log_snapshot, std::string::npos);
  ASSERT_NE(roots_snapshot, std::string::npos);
  ASSERT_NE(stopped_barrier, std::string::npos);
  EXPECT_LT(log_snapshot, roots_snapshot);
  EXPECT_LT(roots_snapshot, stopped_barrier);
  EXPECT_EQ(quiescence.find("if (roots_before.roots.empty())"), std::string::npos);
}

TEST(SteamShutdownStateMachineTests, PrivateSteamAppQuiescenceTerminatesOnlyExactAppLineage) {
  const std::string token = "private-steam-app-quiescence";
  proc::ctx_t steam_app {};
  steam_app.name = "Control";
  steam_app.source = "steam";
  steam_app.steam_appid = "4242";

  const auto spawn_owned = [&](const char *argv0) {
    const auto child = fork();
    EXPECT_GE(child, 0);
    if (child == 0) {
      setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
      execl("/bin/sleep", argv0, "60", nullptr);
      _exit(127);
    }
    return child;
  };

  const auto cage = spawn_owned("labwc");
  child_guard_t cage_guard {cage};
  const auto steam = spawn_owned("/tmp/ubuntu12_32/steam");
  child_guard_t steam_guard {steam};
  ASSERT_GT(cage, 0);
  ASSERT_GT(steam, 0);

  int lineage_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(lineage_pipe), 0);
  const auto app_root = fork();
  ASSERT_GE(app_root, 0);
  child_guard_t app_root_guard {app_root};
  if (app_root == 0) {
    close(lineage_pipe[0]);
    setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
    const auto descendant = fork();
    if (descendant < 0) _exit(124);
    if (descendant == 0) {
      close(lineage_pipe[1]);
      unsetenv("POLARIS_SESSION_INSTANCE_ID");
      execl("/bin/sleep", "Control.exe", "60", nullptr);
      _exit(127);
    }
    if (write(lineage_pipe[1], &descendant, sizeof(descendant)) != sizeof(descendant)) _exit(125);
    close(lineage_pipe[1]);
    execl(
      "/bin/bash",
      "reaper",
      "-c",
      "trap 'exit 0' TERM; sleep 60 & wait",
      "SteamLaunch",
      "AppId=4242",
      nullptr
    );
    _exit(127);
  }

  close(lineage_pipe[1]);
  fd_guard_t lineage_read {lineage_pipe[0]};
  pid_t descendant = -1;
  ASSERT_EQ(read(lineage_read.fd, &descendant, sizeof(descendant)), sizeof(descendant));
  ASSERT_GT(descendant, 0);
  const int app_root_pidfd = static_cast<int>(syscall(SYS_pidfd_open, app_root, 0));
  ASSERT_GE(app_root_pidfd, 0);
  fd_guard_t app_root_pidfd_guard {app_root_pidfd};
  const int descendant_pidfd = static_cast<int>(syscall(SYS_pidfd_open, descendant, 0));
  ASSERT_GE(descendant_pidfd, 0);
  fd_guard_t descendant_guard {descendant_pidfd};
  auto terminate_descendant = util::fail_guard([descendant_pidfd]() {
    (void) syscall(SYS_pidfd_send_signal, descendant_pidfd, SIGKILL, nullptr, 0);
  });

  bool marker_visible = false;
  for (int attempt = 0; attempt < 40 && !marker_visible; ++attempt) {
    std::ifstream cmdline_file("/proc/" + std::to_string(app_root) + "/cmdline", std::ios::binary);
    const std::string cmdline(
      (std::istreambuf_iterator<char>(cmdline_file)),
      std::istreambuf_iterator<char>()
    );
    marker_visible = proc::steam_launch_cmdline_matches_appid_for_tests(cmdline, "4242");
    if (!marker_visible) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  ASSERT_TRUE(marker_visible);

  ASSERT_TRUE(proc::terminate_session_owned_steam_app_lineage_for_tests(steam_app, token));
  ASSERT_TRUE(wait_pidfd_exit(app_root_pidfd, std::chrono::seconds(2)))
    << "the split-argv Steam app root must be terminated";

  int app_status = 0;
  EXPECT_EQ(app_root_guard.wait(&app_status, 0), app_root);
  EXPECT_TRUE(WIFEXITED(app_status) || WIFSIGNALED(app_status));
  EXPECT_TRUE(wait_pidfd_exit(descendant_pidfd, std::chrono::seconds(2)))
    << "a token-stripped descendant of the exact Steam app root must be terminated";

  int survivor_status = 0;
  EXPECT_EQ(cage_guard.wait(&survivor_status, WNOHANG), 0)
    << "the cage compositor must remain alive until cage teardown";
  EXPECT_EQ(steam_guard.wait(&survivor_status, WNOHANG), 0)
    << "the Steam root must remain alive until native shutdown is dispatched";
}

TEST(SteamShutdownStateMachineTests, ProductionPrivateCageQuiescesSteamAppBeforeNativeShutdown) {
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
  const auto app_quiescence = cleanup.find("quiesce_session_owned_steam_app_before_native_shutdown(");
  const auto graceful_request = cleanup.find("attempt_session_owned_steam_graceful_shutdown(");
  const auto exact_pidfd_fallback = cleanup.find("kill_private_steam_pidfds_immediately(ownership");
  ASSERT_NE(root_cardinality_guard, std::string::npos);
  ASSERT_NE(root_front_access, std::string::npos);
  ASSERT_NE(no_unowned_guard, std::string::npos);
  ASSERT_NE(app_quiescence, std::string::npos);
  ASSERT_NE(graceful_request, std::string::npos);
  ASSERT_NE(exact_pidfd_fallback, std::string::npos);
  EXPECT_LT(root_cardinality_guard, root_front_access);
  EXPECT_LT(no_unowned_guard, app_quiescence);
  EXPECT_LT(app_quiescence, graceful_request);
  EXPECT_LT(graceful_request, exact_pidfd_fallback);
  EXPECT_EQ(cleanup.find("terminate_pidfds(ownership.roots"), std::string::npos);
  EXPECT_EQ(cleanup.find("wait_for_pidfds_exit(ownership.helpers"), std::string::npos);
  EXPECT_NE(
    cleanup.find("wait_for_pidfds_exit(", graceful_request),
    std::string::npos
  );
  EXPECT_NE(
    cleanup.find("private_steam_native_shutdown_timeout", graceful_request),
    std::string::npos
  );
}

TEST(SteamShutdownStateMachineTests, ProductionAppLineageCaptureIsBoundedAndCannotLeakStoppedProcesses) {
  const auto source = read_source_file("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto resume_helper = source_between(
    source,
    "bool resume_or_kill_frozen_pidfds(",
    "bool terminate_session_owned_steam_app_lineage("
  );
  const auto app_termination = source_between(
    source,
    "bool terminate_session_owned_steam_app_lineage(",
    "bool terminate_session_owned_steam_before_cage_stop_impl("
  );
  ASSERT_FALSE(resume_helper.empty());
  ASSERT_FALSE(app_termination.empty());
  EXPECT_NE(resume_helper.find("SIGCONT"), std::string::npos);
  EXPECT_NE(resume_helper.find("SIGKILL"), std::string::npos);
  EXPECT_NE(app_termination.find("resume_or_kill_frozen_pidfds("), std::string::npos);
  EXPECT_NE(app_termination.find("private_steam_app_capture_timeout"), std::string::npos);
  EXPECT_EQ(app_termination.find("max_capture_passes"), std::string::npos);
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

TEST(SteamShutdownStateMachineTests, DoctorShutdownIncludesPrivateSteamSingleton) {
  EXPECT_FALSE(proc::doctor_steam_shutdown_required_for_tests(false, false));
  EXPECT_TRUE(proc::doctor_steam_shutdown_required_for_tests(true, false));
  EXPECT_TRUE(proc::doctor_steam_shutdown_required_for_tests(false, true));
  EXPECT_TRUE(proc::doctor_steam_shutdown_required_for_tests(true, true));
}

TEST(SteamShutdownStateMachineTests, ProductionDoctorQuiescesSteamBeforeVdfMutation) {
  const auto source = read_source_file("src/doctor_actions.cpp");
  ASSERT_FALSE(source.empty());
  const auto action = source_between(
    source,
    "if (action_id == \"disable_steam_input_xbox\" ||",
    "const auto stats = stream_stats::get_current();"
  );
  ASSERT_FALSE(action.empty());

  const auto quiesce = action.find("ensure_steam_client_quiescent_for_doctor()");
  const auto rewrite = action.find("rewrite_steam_profile(path, false)");
  ASSERT_NE(quiesce, std::string::npos);
  ASSERT_NE(rewrite, std::string::npos);
  EXPECT_LT(quiesce, rewrite);
  EXPECT_EQ(action.find("if (proc::desktop_steam_client_active())"), std::string::npos);
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
