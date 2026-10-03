#include "../tests_common.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <thread>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <src/process.h>

#if defined(__linux__)
  #include <fcntl.h>
  #include <poll.h>
  #include <set>
  #include <signal.h>
  #include <sys/prctl.h>
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
  const std::optional<std::string> expected_remote {
    "/srv/polaris-user/.steam/root/ubuntu12_32/steam-runtime/amd64/usr/bin/steam-runtime-steam-remote"
  };

  EXPECT_EQ(proc::steam_instance_pipe_path_for_tests(std::nullopt, account_home), expected);
  EXPECT_EQ(proc::steam_instance_pipe_path_for_tests(std::string {}, account_home), expected);
  EXPECT_EQ(proc::steam_remote_command_path_for_tests(std::nullopt, account_home), expected_remote);
  EXPECT_EQ(proc::steam_remote_command_path_for_tests(std::string {}, account_home), expected_remote);
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
    "bool ensure_steam_client_quiescent_for_doctor()"
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

TEST(SteamShutdownStateMachineTests, ExactGenerationUndoUsesNoBootstrapRemoteExecutable) {
  const auto source = read_source_file("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto forwarder = source_between(
    source,
    "boost::process::v1::child run_steam_remote_shutdown_without_launch(",
    "#endif\n\n    void normalize_steam_big_picture_app("
  );
  ASSERT_FALSE(forwarder.empty());
  EXPECT_NE(forwarder.find("boost::process::v1::child("), std::string::npos);
  EXPECT_NE(forwarder.find("remote_path"), std::string::npos);
  EXPECT_NE(forwarder.find("\"-shutdown\""), std::string::npos);
  EXPECT_EQ(forwarder.find("platf::run_command"), std::string::npos);
  EXPECT_EQ(forwarder.find("canonical_steam_shutdown_command"), std::string::npos);
}

TEST(SteamShutdownStateMachineTests, RetainedShutdownClearsOnlyAfterVerifiedCompletion) {
  using scenario = proc::retained_steam_shutdown_scenario_e;

  const auto absent = proc::run_retained_steam_shutdown_scenario_for_tests(
    scenario::listener_absent
  );
  EXPECT_TRUE(absent.complete);
  EXPECT_FALSE(absent.retained);
  EXPECT_EQ(absent.listener_checks, 1u);
  EXPECT_EQ(absent.dispatch_calls, 0u);

  const auto delivered = proc::run_retained_steam_shutdown_scenario_for_tests(
    scenario::delivered_and_released
  );
  EXPECT_TRUE(delivered.complete);
  EXPECT_FALSE(delivered.retained);
  EXPECT_EQ(delivered.dispatch_calls, 1u);
  EXPECT_EQ(delivered.release_waits, 1u);

  const auto vanished = proc::run_retained_steam_shutdown_scenario_for_tests(
    scenario::target_exited_during_delivery
  );
  EXPECT_TRUE(vanished.complete);
  EXPECT_FALSE(vanished.retained);
  EXPECT_EQ(vanished.listener_checks, 2u);
  EXPECT_EQ(vanished.dispatch_calls, 1u);
  EXPECT_EQ(vanished.release_waits, 0u);
}

TEST(SteamShutdownStateMachineTests, RetainedShutdownFailuresStayFailClosed) {
  using scenario = proc::retained_steam_shutdown_scenario_e;
  for (const auto value : {
         scenario::helper_missing,
         scenario::dispatch_error,
         scenario::dispatch_timeout,
         scenario::delivery_failed_listener_retained,
         scenario::delivered_release_timeout,
       }) {
    const auto result = proc::run_retained_steam_shutdown_scenario_for_tests(value);
    EXPECT_FALSE(result.complete);
    EXPECT_TRUE(result.retained);
    EXPECT_EQ(result.attempts, 1u);
  }
}

TEST(SteamShutdownStateMachineTests, RetainedShutdownRetriesOnTheNextLifecycleAttempt) {
  const auto result = proc::run_retained_steam_shutdown_scenario_for_tests(
    proc::retained_steam_shutdown_scenario_e::retry_after_helper_failure_then_listener_released
  );

  EXPECT_TRUE(result.complete);
  EXPECT_FALSE(result.retained);
  EXPECT_EQ(result.attempts, 2u);
  EXPECT_EQ(result.listener_checks, 2u);
  EXPECT_EQ(result.dispatch_calls, 0u);
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
    "      const boost::process::v1::environment &env,\n"
    "      const private_steam_app_close_request_t &request_close,\n"
    "      const private_steam_stop_budget_t &budget\n"
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
  const std::string appid = "4242" + std::to_string(getpid());
  const std::string launch_marker = "AppId=" + appid;
  proc::ctx_t steam_app {};
  steam_app.name = "Control";
  steam_app.source = "steam";
  steam_app.steam_appid = appid;

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
      launch_marker.c_str(),
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
    marker_visible = proc::steam_launch_cmdline_matches_appid_for_tests(cmdline, appid);
    if (!marker_visible) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  ASSERT_TRUE(marker_visible);

  bool single_marker_root = false;
  for (int attempt = 0; attempt < 40 && !single_marker_root; ++attempt) {
    std::size_t matching_roots = 0;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator("/proc", ec)) {
      const auto name = entry.path().filename().string();
      if (ec || name.empty() ||
          !std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        continue;
      }
      std::ifstream cmdline_file(entry.path() / "cmdline", std::ios::binary);
      const std::string cmdline(
        (std::istreambuf_iterator<char>(cmdline_file)),
        std::istreambuf_iterator<char>()
      );
      if (proc::steam_launch_cmdline_matches_appid_for_tests(cmdline, appid)) {
        ++matching_roots;
      }
    }
    single_marker_root = matching_roots == 1;
    if (!single_marker_root) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  ASSERT_TRUE(single_marker_root);

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

#if defined(__linux__)
namespace {
  /**
   * A stand in for a private Steam app: a reaper root carrying Steam's launch marker and the session
   * token, running script under bash with one background child, as the lineage stop finds a real one.
   */
  struct fake_steam_app_t {
    std::string appid;
    child_guard_t root;
    fd_guard_t root_pidfd;
  };

  std::unique_ptr<fake_steam_app_t> spawn_fake_steam_app(const std::string &token, const char *script) {
    static int serial = 0;
    auto app = std::make_unique<fake_steam_app_t>();
    app->appid = "5151" + std::to_string(getpid()) + std::to_string(++serial);
    const std::string marker = "AppId=" + app->appid;
    const auto root = fork();
    if (root == 0) {
      setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
      execl("/bin/bash", "reaper", "-c", script, "SteamLaunch", marker.c_str(), nullptr);
      _exit(127);
    }
    if (root < 0) {
      return nullptr;
    }
    app->root.pid = root;
    app->root_pidfd.fd = static_cast<int>(syscall(SYS_pidfd_open, root, 0));
    for (int attempt = 0; attempt < 80; ++attempt) {
      std::ifstream cmdline_file("/proc/" + std::to_string(root) + "/cmdline", std::ios::binary);
      const std::string cmdline((std::istreambuf_iterator<char>(cmdline_file)), std::istreambuf_iterator<char>());
      if (proc::steam_launch_cmdline_matches_appid_for_tests(cmdline, app->appid)) {
        // The background child exists once bash has run the script this far.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return app;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return nullptr;
  }

  proc::ctx_t fake_steam_context(const std::string &appid) {
    proc::ctx_t app {};
    app.name = "Control";
    app.source = "steam";
    app.steam_appid = appid;
    return app;
  }

  int exit_status_of(child_guard_t &root) {
    int status = 0;
    if (root.wait(&status, 0) != root.pid || !WIFEXITED(status)) {
      return -1;
    }
    return WEXITSTATUS(status);
  }
}  // namespace

TEST(SteamShutdownStateMachineTests, AnAppThatClosesWhenAskedIsNeverSignalled) {
  // The Control quit papi pasted: SIGTERM, two seconds, then SIGKILL for sixteen processes. A game
  // asked the way a player's close button asks gets to save and quit on its own.
  const std::string token = "private-steam-app-close-request";
  auto app = spawn_fake_steam_app(token, "trap 'kill $!; exit 0' USR1; trap 'kill $!; exit 3' TERM; sleep 60 & wait");
  ASSERT_TRUE(app);
  pid_t asked_root = -1;
  const auto result = proc::stop_session_owned_steam_app_lineage_for_tests(
    fake_steam_context(app->appid),
    token,
    [&](pid_t app_root) {
      asked_root = app_root;
      // What Wine does with WM_DELETE_WINDOW, stood in for by a signal the app treats as a close.
      (void) kill(app_root, SIGUSR1);
      return 1;
    },
    std::chrono::seconds(5),
    std::chrono::seconds(5),
    std::chrono::seconds(5)
  );
  EXPECT_TRUE(result.drained);
  EXPECT_EQ(result.path, "close_request");
  EXPECT_EQ(result.windows_asked, 1);
  EXPECT_EQ(asked_root, app->root.pid) << "the close request goes to the exact AppID root's lineage";
  EXPECT_EQ(exit_status_of(app->root), 0) << "the app quit on its own, with no SIGTERM";
}

TEST(SteamShutdownStateMachineTests, AnAppThatIgnoresTheCloseRequestIsAskedWithSigterm) {
  const std::string token = "private-steam-app-close-ignored";
  auto app = spawn_fake_steam_app(token, "trap 'kill $!; exit 3' TERM; sleep 60 & wait");
  ASSERT_TRUE(app);
  const auto result = proc::stop_session_owned_steam_app_lineage_for_tests(
    fake_steam_context(app->appid),
    token,
    [](pid_t) {
      return 1;
    },
    std::chrono::milliseconds(200),
    std::chrono::seconds(5),
    std::chrono::seconds(5)
  );
  EXPECT_TRUE(result.drained);
  EXPECT_EQ(result.path, "sigterm");
  EXPECT_EQ(result.windows_asked, 1);
  EXPECT_EQ(exit_status_of(app->root), 3) << "SIGTERM, not SIGKILL, ended it";
}

TEST(SteamShutdownStateMachineTests, AnAppWithNothingToAskGoesStraightToSigterm) {
  // A native Wayland window, or a session whose X display cannot be reached.
  const std::string token = "private-steam-app-no-window";
  auto app = spawn_fake_steam_app(token, "trap 'kill $!; exit 3' TERM; sleep 60 & wait");
  ASSERT_TRUE(app);
  const auto result = proc::stop_session_owned_steam_app_lineage_for_tests(
    fake_steam_context(app->appid),
    token,
    [](pid_t) {
      return 0;
    },
    std::chrono::seconds(30),
    std::chrono::seconds(5),
    std::chrono::seconds(5)
  );
  EXPECT_TRUE(result.drained);
  EXPECT_EQ(result.path, "sigterm");
  EXPECT_EQ(result.windows_asked, 0);
  EXPECT_EQ(exit_status_of(app->root), 3);
}

TEST(SteamShutdownStateMachineTests, OnlyAnAppThatOutlivesSigtermIsKilled) {
  const std::string token = "private-steam-app-sigkill";
  auto app = spawn_fake_steam_app(token, "trap '' TERM; sleep 60 & wait");
  ASSERT_TRUE(app);
  const auto result = proc::stop_session_owned_steam_app_lineage_for_tests(
    fake_steam_context(app->appid),
    token,
    {},
    std::chrono::milliseconds(200),
    std::chrono::milliseconds(300),
    std::chrono::milliseconds(300)
  );
  EXPECT_TRUE(result.drained);
  EXPECT_EQ(result.path, "sigkill");
  int status = 0;
  ASSERT_EQ(app->root.wait(&status, 0), app->root.pid);
  EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

TEST(SteamShutdownStateMachineTests, AnAppAlreadyGoneIsRecordedAsExitedBeforeTheStop) {
  const auto result = proc::stop_session_owned_steam_app_lineage_for_tests(
    fake_steam_context("51519999999"),
    "private-steam-app-already-gone",
    [](pid_t) {
      ADD_FAILURE() << "nothing is left to ask";
      return 0;
    },
    std::chrono::seconds(1),
    std::chrono::seconds(1),
    std::chrono::seconds(1)
  );
  EXPECT_TRUE(result.drained);
  EXPECT_EQ(result.path, "exited_before_stop");
}

namespace {
  std::vector<pid_t> children_of(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/children");
    std::vector<pid_t> children;
    pid_t child = 0;
    while (in >> child) {
      children.push_back(child);
    }
    return children;
  }

  std::string comm_of(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
    std::string comm;
    std::getline(in, comm);
    return comm;
  }

  /// Alive and not a zombie, which kill(pid, 0) alone cannot tell apart.
  bool running(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    std::getline(in, stat);
    const auto state = stat.rfind(')');
    return state != std::string::npos && state + 2 < stat.size() && stat[state + 2] != 'Z';
  }

  void collect_descendants(pid_t pid, std::vector<pid_t> &out) {
    for (const auto child : children_of(pid)) {
      out.push_back(child);
      collect_descendants(child, out);
    }
  }

  /**
   * A private session as the app stop finds it, with no compositor behind the names: a supervisor
   * that is a subreaper and carries the session token, labwc and Xwayland under it by name, and a
   * startup shell under labwc running the app's script. Everything here is this test's own, and it
   * reaps it all.
   */
  struct fake_private_session_t {
    child_guard_t supervisor;
    pid_t labwc = -1;
    pid_t xwayland = -1;
    pid_t shell = -1;
    pid_t anonymous_bwrap = -1;
    pid_t placeholder = -1;  ///< labwc's startup client when the app is Polaris's own child
    pid_t background = -1;  ///< swaybg, which the generated autostart starts
    pid_t user_helper = -1;  ///< what a user's own autostart runs
    std::filesystem::path runtime_dir;

    ~fake_private_session_t() {
      std::vector<pid_t> tree;
      if (supervisor.pid > 0) {
        collect_descendants(supervisor.pid, tree);
      }
      for (const auto pid : tree) {
        (void) kill(pid, SIGKILL);
      }
      std::error_code ignored;
      std::filesystem::remove_all(runtime_dir, ignored);
    }

    bool compositor_running() const {
      return running(supervisor.pid) && running(labwc) && running(xwayland);
    }
  };

  std::unique_ptr<fake_private_session_t> spawn_fake_private_session(const std::string &token, const char *script, bool with_helpers = false) {
    static int serial = 0;
    auto session = std::make_unique<fake_private_session_t>();
    session->runtime_dir = std::filesystem::temp_directory_path() /
                           ("polaris-private-app-stop-" + std::to_string(getpid()) + "-" + std::to_string(++serial));
    std::filesystem::remove_all(session->runtime_dir);
    std::filesystem::create_directories(session->runtime_dir / ".flatpak");
    // A process named bwrap with a zero-byte environ, as Flatpak's are, which no record names.
    const auto bwrap_link = session->runtime_dir / "bwrap";
    std::filesystem::create_symlink("/bin/sleep", bwrap_link);
    const auto bwrap_path = bwrap_link.string();

    const auto supervisor = fork();
    if (supervisor == 0) {
      setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
      (void) prctl(PR_SET_CHILD_SUBREAPER, 1);
      const auto labwc = fork();
      if (labwc == 0) {
        (void) prctl(PR_SET_NAME, "labwc");
        (void) signal(SIGCHLD, SIG_IGN);
        if (fork() == 0) {
          (void) signal(SIGCHLD, SIG_DFL);
          (void) prctl(PR_SET_NAME, "Xwayland");
          for (;;) {
            pause();
          }
        }
        if (fork() == 0) {
          (void) signal(SIGCHLD, SIG_DFL);
          char *empty[] = {nullptr};
          execle(bwrap_path.c_str(), "bwrap", "30", static_cast<char *>(nullptr), empty);
          _exit(127);
        }
        if (fork() == 0) {
          (void) signal(SIGCHLD, SIG_DFL);
          // A startup file a user's environment names would run first and start processes of its own.
          (void) unsetenv("BASH_ENV");
          (void) unsetenv("ENV");
          execl("/bin/bash", "bash", "-c", script, static_cast<char *>(nullptr));
          _exit(127);
        }
        if (with_helpers) {
          // What never exits on its own: labwc's startup client when the app is Polaris's own
          // child, the background the generated autostart paints, and a user's autostart helper.
          if (fork() == 0) {
            (void) signal(SIGCHLD, SIG_DFL);
            execl("/bin/sleep", "sleep", "infinity", static_cast<char *>(nullptr));
            _exit(127);
          }
          for (const char *name : {"swaybg", "waybar"}) {
            if (fork() == 0) {
              (void) signal(SIGCHLD, SIG_DFL);
              (void) prctl(PR_SET_NAME, name);
              for (;;) {
                pause();
              }
            }
          }
        }
        for (;;) {
          pause();
        }
      }
      for (;;) {
        if (wait(nullptr) < 0 && errno == ECHILD) {
          pause();
        }
      }
    }
    if (supervisor < 0) {
      return nullptr;
    }
    session->supervisor.pid = supervisor;
    for (int attempt = 0; attempt < 400; ++attempt) {
      for (const auto child : children_of(supervisor)) {
        if (comm_of(child) == "labwc") {
          session->labwc = child;
        }
      }
      if (session->labwc > 0) {
        for (const auto child : children_of(session->labwc)) {
          const auto comm = comm_of(child);
          if (comm == "Xwayland") {
            session->xwayland = child;
          } else if (comm == "bash") {
            session->shell = child;
          } else if (comm == "bwrap") {
            session->anonymous_bwrap = child;
          } else if (comm == "sleep") {
            session->placeholder = child;
          } else if (comm == "swaybg") {
            session->background = child;
          } else if (comm == "waybar") {
            session->user_helper = child;
          }
        }
      }
      // The script's own sleep is started once bash has run it this far.
      std::vector<pid_t> started;
      if (session->shell > 0) {
        collect_descendants(session->shell, started);
      }
      const bool script_running = std::any_of(started.begin(), started.end(), [](pid_t pid) {
        return comm_of(pid) == "sleep";
      });
      const bool helpers_running = !with_helpers || (session->placeholder > 0 && session->background > 0 && session->user_helper > 0);
      if (session->xwayland > 0 && session->shell > 0 && session->anonymous_bwrap > 0 && script_running && helpers_running) {
        return session;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return nullptr;
  }

  std::string private_app_stop_token() {
    static int serial = 0;
    return "private-app-stop-test-" + std::to_string(getpid()) + "-" + std::to_string(++serial);
  }

  bool contains(const std::vector<pid_t> &pids, pid_t pid) {
    return std::find(pids.begin(), pids.end(), pid) != pids.end();
  }

  void expect_compositor_untouched(const fake_private_session_t &session, const proc::private_app_stop_test_result_t &result) {
    EXPECT_TRUE(session.compositor_running()) << "the compositor is stopped after the phase, never by it";
    for (const auto pid : {session.supervisor.pid, session.labwc, session.xwayland, session.anonymous_bwrap}) {
      EXPECT_FALSE(contains(result.sigterm, pid)) << pid << " (" << comm_of(pid) << ") was sent SIGTERM";
      EXPECT_FALSE(contains(result.sigkill, pid)) << pid << " (" << comm_of(pid) << ") was sent SIGKILL";
    }
    EXPECT_TRUE(running(session.anonymous_bwrap)) << "a bwrap with a zero-byte environ is never signalled";
  }
}  // namespace

TEST(SteamShutdownStateMachineTests, PrivateAppStopAsksTheGameToCloseWhileTheCompositorIsUp) {
  const auto token = private_app_stop_token();
  auto session = spawn_fake_private_session(token, "trap 'kill $!; exit 0' USR1; trap 'kill $!; exit 3' TERM; sleep 30 & wait");
  ASSERT_TRUE(session);
  const auto shell = session->shell;
  const auto result = proc::stop_private_session_apps_for_tests(
    token,
    session->supervisor.pid,
    session->runtime_dir,
    false,
    5,
    [&](const std::function<bool(pid_t)> &asks) {
      EXPECT_FALSE(asks(session->labwc));
      EXPECT_FALSE(asks(session->xwayland));
      EXPECT_FALSE(asks(session->supervisor.pid));
      EXPECT_FALSE(asks(session->anonymous_bwrap));
      if (!asks(shell)) {
        return 0;
      }
      // What Wine does with WM_DELETE_WINDOW, stood in for by a signal the app treats as a close.
      (void) kill(shell, SIGUSR1);
      return 1;
    }
  );
  EXPECT_TRUE(result.acted);
  EXPECT_EQ(result.path, "close_request");
  EXPECT_EQ(result.windows_asked, 1);
  EXPECT_TRUE(result.drained);
  EXPECT_TRUE(result.sigterm.empty()) << "a game that closes when asked is never signalled";
  EXPECT_TRUE(result.sigkill.empty());
  EXPECT_FALSE(running(shell));
  expect_compositor_untouched(*session, result);
}

TEST(SteamShutdownStateMachineTests, PrivateAppStopWaitsForWhatItAskedNotForTheSessionsHelpers) {
  // A session also runs what never exits on its own: labwc's `sleep infinity` startup client when
  // the app is Polaris's own child, as every ROM import's is, swaybg from the generated autostart,
  // and whatever a user's own autostart starts. A game that closes when asked is done then. The
  // compositor's two go with the compositor; the user's helper is ended before it stops.
  const auto token = private_app_stop_token();
  auto session = spawn_fake_private_session(token, "trap 'kill $!; exit 0' USR1; trap 'kill $!; exit 3' TERM; sleep 30 & wait", true);
  ASSERT_TRUE(session);
  const auto shell = session->shell;
  const auto result = proc::stop_private_session_apps_for_tests(
    token,
    session->supervisor.pid,
    session->runtime_dir,
    false,
    5,
    [&](const std::function<bool(pid_t)> &asks) {
      EXPECT_FALSE(asks(session->placeholder)) << "the startup client is the compositor's";
      EXPECT_FALSE(asks(session->background)) << "the background is the compositor's";
      if (!asks(shell)) {
        return 0;
      }
      (void) kill(shell, SIGUSR1);
      return 1;
    }
  );
  EXPECT_EQ(result.path, "close_request");
  EXPECT_EQ(result.windows_asked, 1);
  EXPECT_TRUE(result.drained);
  EXPECT_LT(result.elapsed, std::chrono::milliseconds(1500)) << "the close wait is 2 s here, and the game closed at once";
  EXPECT_FALSE(contains(result.sigterm, shell)) << "the game closed when asked";
  EXPECT_TRUE(contains(result.sigterm, session->user_helper)) << "what the user's autostart ran goes before the compositor";
  EXPECT_FALSE(running(session->user_helper));
  for (const auto pid : {session->placeholder, session->background}) {
    EXPECT_FALSE(contains(result.sigterm, pid)) << comm_of(pid) << " is the compositor's";
    EXPECT_FALSE(contains(result.sigkill, pid)) << comm_of(pid) << " is the compositor's";
    EXPECT_TRUE(running(pid)) << comm_of(pid);
  }
  EXPECT_TRUE(result.sigkill.empty());
  expect_compositor_untouched(*session, result);
}

TEST(SteamShutdownStateMachineTests, CompositorSurvivesThePrivateAppStopEvenWhenEverythingTimesOut) {
  // The app ignores the close request and SIGTERM, and its child inherits both. The phase escalates
  // to SIGKILL within its budget, and returns with the compositor still up.
  const auto token = private_app_stop_token();
  auto session = spawn_fake_private_session(token, "trap '' TERM USR1; sleep 30 & wait");
  ASSERT_TRUE(session);
  const auto shell = session->shell;
  std::vector<pid_t> game;
  collect_descendants(shell, game);
  ASSERT_FALSE(game.empty());
  const auto started = std::chrono::steady_clock::now();
  const auto result = proc::stop_private_session_apps_for_tests(
    token,
    session->supervisor.pid,
    session->runtime_dir,
    false,
    10,
    [&](const std::function<bool(pid_t)> &asks) {
      if (asks(shell)) {
        (void) kill(shell, SIGUSR1);
        return 1;
      }
      return 0;
    }
  );
  const auto waited = std::chrono::steady_clock::now() - started;
  EXPECT_EQ(result.path, "sigkill");
  EXPECT_TRUE(result.drained);
  EXPECT_TRUE(contains(result.sigterm, shell));
  EXPECT_TRUE(contains(result.sigkill, shell));
  EXPECT_GE(result.sigkill.size(), 2u) << "the shell and the game it started";
  EXPECT_LE(result.elapsed, std::chrono::milliseconds(3000)) << "a tenth of the 30 s budget";
  EXPECT_LT(waited, std::chrono::seconds(5));
  EXPECT_FALSE(running(shell));
  for (const auto pid : game) {
    EXPECT_FALSE(running(pid)) << pid;
  }
  // Nothing of the session's is left but the compositor and the bwrap no record names.
  std::vector<pid_t> left;
  collect_descendants(session->supervisor.pid, left);
  for (const auto pid : left) {
    EXPECT_TRUE(pid == session->labwc || pid == session->xwayland || pid == session->anonymous_bwrap || !running(pid))
      << pid << " (" << comm_of(pid) << ") outlived the phase";
  }
  expect_compositor_untouched(*session, result);
}

TEST(SteamShutdownStateMachineTests, AnImmediatePrivateAppStopAsksNothing) {
  const auto token = private_app_stop_token();
  auto session = spawn_fake_private_session(token, "trap 'kill $!; exit 3' TERM; sleep 30 & wait");
  ASSERT_TRUE(session);
  const auto result = proc::stop_private_session_apps_for_tests(
    token,
    session->supervisor.pid,
    session->runtime_dir,
    true,
    2,
    [](const std::function<bool(pid_t)> &) {
      ADD_FAILURE() << "an immediate stop asks no window to close";
      return 0;
    }
  );
  EXPECT_EQ(result.path, "sigterm");
  EXPECT_EQ(result.windows_asked, 0);
  EXPECT_TRUE(result.sigkill.empty());
  expect_compositor_untouched(*session, result);
}

TEST(SteamShutdownStateMachineTests, PrivateAppStopFindsNothingOnceTheSessionsAppIsGone) {
  // What a Steam context leaves after the Steam lane: the compositor and nothing of the app.
  const auto token = private_app_stop_token();
  auto session = spawn_fake_private_session(token, "sleep 0.2 & wait");
  ASSERT_TRUE(session);
  for (int attempt = 0; attempt < 200 && running(session->shell); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_FALSE(running(session->shell));
  const auto result = proc::stop_private_session_apps_for_tests(
    token,
    session->supervisor.pid,
    session->runtime_dir,
    false,
    10,
    [](const std::function<bool(pid_t)> &) {
      ADD_FAILURE() << "there is nothing to ask";
      return 0;
    }
  );
  EXPECT_FALSE(result.acted);
  EXPECT_TRUE(result.sigterm.empty());
  EXPECT_TRUE(result.sigkill.empty());
  EXPECT_LT(result.elapsed, std::chrono::milliseconds(500));
  expect_compositor_untouched(*session, result);
}

TEST(SteamShutdownStateMachineTests, PrivateAppStopNeverSignalsAProcessOutsideTheSession) {
  // A process of the host's, not under the supervisor and without the session token, stands for
  // anything on the desktop: it is never signalled, whatever the phase does to the session.
  child_guard_t outsider;
  outsider.pid = fork();
  ASSERT_GE(outsider.pid, 0);
  if (outsider.pid == 0) {
    execl("/bin/sleep", "sleep", "30", static_cast<char *>(nullptr));
    _exit(127);
  }
  const auto token = private_app_stop_token();
  auto session = spawn_fake_private_session(token, "trap '' TERM; sleep 30 & wait");
  ASSERT_TRUE(session);
  const auto result = proc::stop_private_session_apps_for_tests(token, session->supervisor.pid, session->runtime_dir, false, 10, {});
  EXPECT_EQ(result.path, "sigkill");
  EXPECT_FALSE(contains(result.sigterm, outsider.pid));
  EXPECT_FALSE(contains(result.sigkill, outsider.pid));
  EXPECT_TRUE(running(outsider.pid));
  expect_compositor_untouched(*session, result);
}

TEST(SteamShutdownStateMachineTests, SteamContextRunsTheSteamLaneFirstThenTheAppsThenTheCompositor) {
  const auto source = read_source_file("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto terminate_start = source.find("void proc_t::terminate_impl(");
  const auto terminate_end = source.find("bool proc_t::reload_configuration_from_file", terminate_start);
  ASSERT_NE(terminate_start, std::string::npos);
  ASSERT_NE(terminate_end, std::string::npos);
  const auto terminate = source.substr(terminate_start, terminate_end - terminate_start);
  const auto steam_lane = terminate.find("terminate_session_owned_steam_before_cage_stop();");
  const auto apps = terminate.find("stop_private_session_apps_before_compositor(immediate);");
  const auto generation = terminate.find("terminate_isolated_session_generation();");
  ASSERT_NE(steam_lane, std::string::npos);
  ASSERT_NE(apps, std::string::npos);
  ASSERT_NE(generation, std::string::npos);
  EXPECT_LT(steam_lane, apps) << "a Steam context's game is stopped by the Steam lane first";
  EXPECT_LT(apps, generation) << "the generation cleanup, which stops the compositor, comes after the apps";

  // The phase itself never stops or resets the compositor.
  const auto phase_start = source.find("private_app_stop_report_t stop_private_session_apps(");
  const auto phase_end = source.find("bool terminate_session_owned_steam_app_lineage(", phase_start);
  ASSERT_NE(phase_start, std::string::npos);
  ASSERT_NE(phase_end, std::string::npos);
  const auto phase = source.substr(phase_start, phase_end - phase_start);
  for (const auto *compositor_stop : {"labwc::stop(", "reset_after_external_stop(", "finalize_isolated_session_runtime(", "cage_display_router::"}) {
    EXPECT_EQ(phase.find(compositor_stop), std::string::npos) << compositor_stop;
  }
  const auto member_start = source.find("void proc_t::stop_private_session_apps_before_compositor(");
  const auto member_end = source.find("void proc_t::finalize_isolated_session_runtime(", member_start);
  ASSERT_NE(member_start, std::string::npos);
  ASSERT_NE(member_end, std::string::npos);
  const auto member = source.substr(member_start, member_end - member_start);
  for (const auto *compositor_stop : {"labwc::stop(", "reset_after_external_stop(", "finalize_isolated_session_runtime(", "cage_display_router::"}) {
    EXPECT_EQ(member.find(compositor_stop), std::string::npos) << compositor_stop;
  }
}
#endif

TEST(SteamShutdownStateMachineTests, TheLongerWaitsComeOutOfTheFormerWorstCase) {
  using namespace std::chrono_literals;
  // Capture, the former two second SIGTERM grace, the SIGKILL wait, Steam's app-stopped event and
  // its settle, and the native shutdown: what the teardown could already hold the lifecycle lock for.
  EXPECT_EQ(proc::private_steam_stop_budget_for_tests(), 500ms + 2s + 1s + 5s + 5s + 10s);
  EXPECT_EQ(proc::private_steam_stop_budget_clamp_for_tests(3s, 10s), 3s);
  EXPECT_EQ(proc::private_steam_stop_budget_clamp_for_tests(20s, 10s), 10s);
  EXPECT_EQ(proc::private_steam_stop_budget_clamp_for_tests(-1s, 10s), 0ms);
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

TEST(SteamShutdownStateMachineTests, ProductionDoctorSteamVdfMutationCodeIsAbsent) {
  const auto source = read_source_file("src/doctor_actions.cpp");
  ASSERT_FALSE(source.empty());
  EXPECT_NE(source.find("if (action_id == \"disable_steam_input_xbox\")"), std::string::npos);
  EXPECT_NE(source.find("return steam_vdf_read_only_response()"), std::string::npos);
  EXPECT_EQ(source.find("ensure_steam_client_quiescent_for_doctor()"), std::string::npos);
  EXPECT_EQ(source.find("rewrite_steam_profile"), std::string::npos);
}

TEST(SteamShutdownStateMachineTests, DoctorSteamVdfUndoCodeIsAbsent) {
  const auto source = read_source_file("src/doctor_actions.cpp");
  ASSERT_FALSE(source.empty());
  EXPECT_EQ(source.find("action_kind_e::disable_steam_input_xbox"), std::string::npos);
  EXPECT_EQ(source.find("steam_profile_edit_t"), std::string::npos);
  EXPECT_EQ(source.find("rewrite_steam_profile"), std::string::npos);
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
  // /launch runs its desktop Steam step in admit_desktop_launch_policy().
  const auto launch_step = source_between(
    source,
    "bool admit_desktop_launch_policy(",
    "void put_optimization_launch_policy("
  );
  ASSERT_FALSE(launch_route.empty());
  ASSERT_FALSE(launch_step.empty());
  ASSERT_FALSE(api_route.empty());
  EXPECT_NE(launch_route.find("admit_desktop_launch_policy("), std::string::npos);

  const auto launch_refresh = launch_step.find(
    "launch_policy = proc::resolve_desktop_launch_safety_policy_after_shutdown("
  );
  const auto launch_gate = launch_step.find(
    "if (launch_policy.recommendedAction == \"refuse_private_stream\")",
    launch_refresh
  );
  ASSERT_NE(launch_refresh, std::string::npos);
  ASSERT_NE(launch_gate, std::string::npos);
  EXPECT_LT(launch_refresh, launch_gate);
  EXPECT_EQ(launch_step.find("refreshed_launch_policy"), std::string::npos);

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

TEST(SteamShutdownStateMachineTests, PerAppCloseDesktopSteamReachesBothNvhttpRoutes) {
  // The per-app "close-desktop-steam-for-private" flag is the only way a
  // standard Moonlight client (which cannot add launch parameters) can opt
  // into close-desktop-Steam-then-launch. It must enter the policy exactly
  // where the request-side closeDesktopSteamForPrivate parameter does, in
  // BOTH nvhttp wrappers, so the shutdown-and-re-resolve flow the routes
  // already implement applies to it unchanged. It must not be OR-ed into the
  // post-shutdown recheck, which deliberately re-resolves with the force flag
  // off.
  const auto nvhttp = read_source_file("src/nvhttp.cpp");
  ASSERT_FALSE(nvhttp.empty());
  EXPECT_NE(
    nvhttp.find("force_private_after_desktop_steam_shutdown_requested(args) || app.close_desktop_steam_for_private"),
    std::string::npos
  );
  EXPECT_NE(
    nvhttp.find("force_private_after_desktop_steam_shutdown_requested(body) || app.close_desktop_steam_for_private"),
    std::string::npos
  );

  // The flag only exists if apps.json parsing and payload validation both
  // know the key; losing either silently turns the toggle into a no-op.
  const auto process = read_source_file("src/process.cpp");
  ASSERT_FALSE(process.empty());
  EXPECT_NE(
    process.find("app_node.value(\"close-desktop-steam-for-private\", false)"),
    std::string::npos
  );
  const auto validation = read_source_file("src/confighttp_validation.cpp");
  ASSERT_FALSE(validation.empty());
  EXPECT_NE(validation.find("\"close-desktop-steam-for-private\"sv"), std::string::npos);
}

TEST(SteamShutdownStateMachineTests, DeviceCloseDesktopSteamReachesOnlyTheGameStreamLaunchRoute) {
  // A device's close_desktop_steam switch is the closeDesktopSteamForPrivate parameter for a
  // client that cannot send one, so it enters the policy beside the per-app switch, from the
  // GameStream /launch route; test_launch_desktop_steam.cpp covers what that route then does to
  // desktop Steam. Nova for Android asks its player what to do from the /polaris/v1/optimize
  // preview and sends the answer, so the preview leaves the switch out and that question is
  // still asked. The JSON launch route takes no device switch at all.
  const auto nvhttp = read_source_file("src/nvhttp.cpp");
  ASSERT_FALSE(nvhttp.empty());

  const auto wrapper = source_between(
    nvhttp,
    "bool device_closes_desktop_steam\n    ) {",
    "proc::desktop_launch_safety_policy_t resolve_streaming_launch_safety_policy("
  );
  ASSERT_FALSE(wrapper.empty());
  const auto per_app = wrapper.find("app.close_desktop_steam_for_private ||");
  ASSERT_NE(per_app, std::string::npos);
  EXPECT_NE(wrapper.find("device_closes_desktop_steam,", per_app), std::string::npos);

  const auto launch_route = source_between(
    nvhttp,
    "void launch(bool &host_audio",
    "void resume(bool &host_audio"
  );
  ASSERT_FALSE(launch_route.empty());
  const auto desktop_steam_step = launch_route.find("admit_desktop_launch_policy(");
  const auto device_switch = launch_route.find("named_cert_p->close_desktop_steam");
  ASSERT_NE(desktop_steam_step, std::string::npos);
  ASSERT_NE(device_switch, std::string::npos);
  EXPECT_LT(desktop_steam_step, device_switch);

  const auto preview = source_between(
    nvhttp,
    "void put_optimization_launch_policy(",
    "output[\"launchPolicy\"]"
  );
  ASSERT_FALSE(preview.empty());
  EXPECT_EQ(preview.find("named_cert"), std::string::npos);
  EXPECT_NE(preview.find("proc::input_only_app_id,\n        false\n      );"), std::string::npos);

  const auto api_route = source_between(
    nvhttp,
    "auto polarisLaunchGame =",
    "// Toggle MangoHud for a game"
  );
  ASSERT_FALSE(api_route.empty());
  EXPECT_EQ(api_route.find("close_desktop_steam"), std::string::npos);
}
#endif
