/**
 * @file tests/unit/platform/test_session_manager.cpp
 * @brief Test Linux session unlock policy helpers.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/session_manager.h>

  #include <algorithm>
  #include <chrono>
  #include <cerrno>
  #include <csignal>
  #include <cstdlib>
  #include <string>
  #include <string_view>
  #include <vector>

namespace {
  struct SessionManagerCommandHarness {
    bool locked = true;
    bool dbus_run_ok = true;
    bool dbus_clears_lock = false;
    bool loginctl_run_ok = true;
    bool loginctl_clears_lock = false;
    std::string loginctl_command_that_clears;
    std::string loginctl_list_sessions_output;
    std::string systemd_environment_output;
    std::vector<std::string> run_commands;

    SessionManagerCommandHarness() {
      session_manager::set_command_hooks_for_tests(
        [this](const std::string &cmd) {
          if (cmd.find("org.freedesktop.ScreenSaver.GetActive") != std::string::npos) {
            return locked ? std::string {"true"} : std::string {"false"};
          }
          if (cmd.find("loginctl list-sessions") != std::string::npos) {
            return loginctl_list_sessions_output;
          }
          if (cmd.find("systemctl --user show-environment") != std::string::npos) {
            return systemd_environment_output;
          }
          return std::string {};
        },
        [this](const std::string &cmd) {
          run_commands.push_back(cmd);
          if (cmd.find("org.freedesktop.ScreenSaver.SetActive") != std::string::npos) {
            if (dbus_run_ok && dbus_clears_lock) {
              locked = false;
            }
            return dbus_run_ok;
          }

          if (cmd.find("loginctl unlock-session") != std::string::npos ||
              cmd.find("loginctl unlock-sessions") != std::string::npos) {
            const bool clears_expected_session =
              loginctl_command_that_clears.empty() ||
              cmd.find(loginctl_command_that_clears) != std::string::npos;
            if (loginctl_run_ok && loginctl_clears_lock && clears_expected_session) {
              locked = false;
            }
            return loginctl_run_ok;
          }

          return true;
        }
      );
    }

    ~SessionManagerCommandHarness() {
      session_manager::reset_command_hooks_for_tests();
      unsetenv("XDG_SESSION_ID");
      unsetenv("WAYLAND_DISPLAY");
      unsetenv("DISPLAY");
      unsetenv("XDG_CURRENT_DESKTOP");
      unsetenv("XDG_SESSION_TYPE");
      unsetenv("DBUS_SESSION_BUS_ADDRESS");
      unsetenv("XDG_RUNTIME_DIR");
      unsetenv("POLARIS_TEST_SENTINEL");
    }

    bool ran(std::string_view needle) const {
      return std::any_of(run_commands.begin(), run_commands.end(), [needle](const std::string &cmd) {
        return cmd.find(needle) != std::string::npos;
      });
    }
  };
}

TEST(SessionManagerEnvironmentRepairTests, ImportsMissingDesktopSessionVariablesFromUserSystemdEnvironment) {
  SessionManagerCommandHarness harness;
  unsetenv("WAYLAND_DISPLAY");
  unsetenv("DISPLAY");
  unsetenv("XDG_CURRENT_DESKTOP");
  setenv("XDG_SESSION_TYPE", "tty", 1);
  setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
  setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus", 1);
  harness.systemd_environment_output =
    "XDG_RUNTIME_DIR=/run/user/1000\n"
    "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus\n"
    "DISPLAY=:0\n"
    "WAYLAND_DISPLAY=wayland-0\n"
    "XDG_CURRENT_DESKTOP=KDE\n"
    "XDG_SESSION_TYPE=wayland\n"
    "POLARIS_TEST_SENTINEL=must-not-import";

  EXPECT_TRUE(session_manager::repair_desktop_session_environment());

  EXPECT_STREQ("wayland-0", getenv("WAYLAND_DISPLAY"));
  EXPECT_STREQ(":0", getenv("DISPLAY"));
  EXPECT_STREQ("KDE", getenv("XDG_CURRENT_DESKTOP"));
  EXPECT_STREQ("wayland", getenv("XDG_SESSION_TYPE"));
  EXPECT_EQ(nullptr, getenv("POLARIS_TEST_SENTINEL"));
  EXPECT_TRUE(session_manager::desktop_session_environment_was_repaired());
}

TEST(SessionManagerUnlockTests, AlreadyUnlockedSkipsUnlockCommands) {
  SessionManagerCommandHarness harness;
  harness.locked = false;
  setenv("XDG_SESSION_ID", "1", 1);

  EXPECT_TRUE(session_manager::unlock_screen());

  EXPECT_TRUE(harness.run_commands.empty());
}

TEST(SessionManagerUnlockTests, DbusUnlockClearsWithoutLoginctl) {
  SessionManagerCommandHarness harness;
  harness.dbus_clears_lock = true;
  setenv("XDG_SESSION_ID", "1", 1);

  EXPECT_TRUE(session_manager::unlock_screen());

  EXPECT_FALSE(harness.locked);
  EXPECT_TRUE(harness.ran("org.freedesktop.ScreenSaver.SetActive"));
  EXPECT_FALSE(harness.ran("loginctl"));
}

TEST(SessionManagerUnlockTests, DbusSuccessStillLockedUsesSessionLoginctlFallback) {
  SessionManagerCommandHarness harness;
  harness.loginctl_clears_lock = true;
  setenv("XDG_SESSION_ID", "alpha'beta", 1);

  EXPECT_TRUE(session_manager::unlock_screen());

  EXPECT_FALSE(harness.locked);
  EXPECT_TRUE(harness.ran("org.freedesktop.ScreenSaver.SetActive"));
  EXPECT_TRUE(harness.ran("loginctl unlock-session 'alpha'\\''beta'"));
}

TEST(SessionManagerUnlockTests, ManagerSessionIdFallsBackToGraphicalLoginctlSession) {
  SessionManagerCommandHarness harness;
  harness.loginctl_clears_lock = true;
  harness.loginctl_command_that_clears = "loginctl unlock-session '1'";
  const char *user = std::getenv("USER");
  ASSERT_NE(nullptr, user);
  harness.loginctl_list_sessions_output =
    "1 1000 " + std::string {user} + " seat0 2134 user tty1 no -\n"
    "2 1000 " + std::string {user} + " - 2197 manager - no -";
  setenv("XDG_SESSION_ID", "2", 1);

  EXPECT_TRUE(session_manager::unlock_screen());

  EXPECT_FALSE(harness.locked);
  EXPECT_TRUE(harness.ran("loginctl unlock-session '1'"));
  EXPECT_FALSE(harness.ran("loginctl unlock-session '2'"));
}

TEST(SessionManagerUnlockTests, FailedGraphicalUnlockTriesNextGraphicalSession) {
  SessionManagerCommandHarness harness;
  harness.loginctl_clears_lock = true;
  harness.loginctl_command_that_clears = "loginctl unlock-session '3'";
  const char *user = std::getenv("USER");
  ASSERT_NE(nullptr, user);
  harness.loginctl_list_sessions_output =
    "1 1000 " + std::string {user} + " seat0 2134 user tty1 no -\n"
    "2 1000 " + std::string {user} + " - 2197 manager - no -\n"
    "3 1000 " + std::string {user} + " seat0 2244 user tty2 no -";
  setenv("XDG_SESSION_ID", "2", 1);

  EXPECT_TRUE(session_manager::unlock_screen());

  EXPECT_FALSE(harness.locked);
  EXPECT_TRUE(harness.ran("loginctl unlock-session '1'"));
  EXPECT_TRUE(harness.ran("loginctl unlock-session '3'"));
  EXPECT_FALSE(harness.ran("loginctl unlock-session '2'"));
}

TEST(SessionManagerUnlockTests, MissingSessionIdUsesAllSessionsLoginctlFallback) {
  SessionManagerCommandHarness harness;
  harness.loginctl_clears_lock = true;
  unsetenv("XDG_SESSION_ID");

  EXPECT_TRUE(session_manager::unlock_screen());

  EXPECT_FALSE(harness.locked);
  EXPECT_TRUE(harness.ran("loginctl unlock-sessions"));
}

TEST(SessionManagerInhibitorTests, FallbackInhibitorIsHeldAndReleasedInsteadOfLeaking) {
  SessionManagerCommandHarness harness;

  // The harness answers ScreenSaver.Inhibit with an empty cookie, which is what
  // a headless or gamescope session looks like, so the fallback path runs.
  ASSERT_TRUE(session_manager::inhibit_lock());
  ASSERT_TRUE(session_manager::inhibitor_held_for_tests());

  const auto pid = session_manager::inhibitor_pid_for_tests();
  ASSERT_GT(pid, 0);
  EXPECT_EQ(0, kill(pid, 0)) << "inhibitor child should be running";

  session_manager::release_lock();

  EXPECT_FALSE(session_manager::inhibitor_held_for_tests());
  EXPECT_EQ(-1, session_manager::inhibitor_pid_for_tests());
  // Closing the anchor has to actually end the child. The previous
  // implementation left `systemd-inhibit ... sleep infinity` running for the
  // rest of the boot, on clean exits as well as crashes.
  EXPECT_EQ(-1, kill(pid, 0));
  EXPECT_EQ(ESRCH, errno);
}

TEST(SessionManagerInhibitorTests, ReleasingWithoutAnInhibitorIsHarmless) {
  SessionManagerCommandHarness harness;

  ASSERT_FALSE(session_manager::inhibitor_held_for_tests());
  session_manager::release_lock();
  EXPECT_FALSE(session_manager::inhibitor_held_for_tests());
}

namespace {
  struct HostSleepCommandHarness {
    // busctl prints a string reply as: s "yes"
    std::string can_suspend_answer = "s \"yes\"";
    // busctl prints nothing at all when a void method succeeds.
    std::string suspend_answer;
    std::vector<std::string> exec_commands;

    HostSleepCommandHarness() {
      // Every accepted suspend starts a watcher thread. Without a short window
      // here, one test's 30 second watch outlives it and the next test joins
      // that one instead of its own, which passes for the wrong reason.
      session_manager::set_suspend_watch_timings_for_tests(
        std::chrono::milliseconds {60},
        std::chrono::milliseconds {10}
      );
      session_manager::set_command_hooks_for_tests(
        [this](const std::string &cmd) {
          exec_commands.push_back(cmd);
          if (cmd.find("CanSuspend") != std::string::npos) {
            return can_suspend_answer;
          }
          if (cmd.find("Manager Suspend") != std::string::npos) {
            return suspend_answer;
          }
          return std::string {};
        },
        [](const std::string &) {
          return true;
        }
      );
    }

    ~HostSleepCommandHarness() {
      // Joins the watcher and clears the recorded outcome, so no test inherits
      // a verdict from the one before it.
      session_manager::reset_suspend_watch_timings_for_tests();
      session_manager::reset_command_hooks_for_tests();
    }

    bool asked(std::string_view needle) const {
      return std::any_of(exec_commands.begin(), exec_commands.end(), [needle](const std::string &cmd) {
        return cmd.find(needle) != std::string::npos;
      });
    }
  };
}

TEST(SessionManagerHostSleepTests, LogindYesIsSupported) {
  HostSleepCommandHarness harness;

  const auto readiness = session_manager::host_sleep_readiness();

  EXPECT_TRUE(readiness.supported);
  EXPECT_TRUE(readiness.reason.empty());
  EXPECT_TRUE(harness.asked("org.freedesktop.login1.Manager CanSuspend"));
}

TEST(SessionManagerHostSleepTests, LogindChallengeIsReportedAsPolkitDenied) {
  HostSleepCommandHarness harness;
  harness.can_suspend_answer = "s \"challenge\"";

  const auto readiness = session_manager::host_sleep_readiness();

  EXPECT_FALSE(readiness.supported);
  EXPECT_EQ("polkit_denied", readiness.reason);
  // The action id is the fix, so it has to survive into the message a client shows.
  EXPECT_NE(std::string::npos, readiness.message.find("org.freedesktop.login1.suspend"));
}

TEST(SessionManagerHostSleepTests, LogindNaIsReportedAsNotAvailable) {
  HostSleepCommandHarness harness;
  harness.can_suspend_answer = "s \"na\"";

  const auto readiness = session_manager::host_sleep_readiness();

  EXPECT_FALSE(readiness.supported);
  EXPECT_EQ("not_available", readiness.reason);
}

TEST(SessionManagerHostSleepTests, SilentLogindIsReportedAsUnavailable) {
  HostSleepCommandHarness harness;
  harness.can_suspend_answer = "";

  const auto readiness = session_manager::host_sleep_readiness();

  EXPECT_FALSE(readiness.supported);
  EXPECT_EQ("logind_unavailable", readiness.reason);
}

TEST(SessionManagerHostSleepTests, SuspendAsksLogindWithoutInteractiveAuthentication) {
  HostSleepCommandHarness harness;

  const auto result = session_manager::suspend_host();

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.reason.empty());
  // The false argument is the point: nobody is at the host to answer a polkit
  // prompt, and an interactive call would hang the request instead of failing.
  EXPECT_TRUE(harness.asked("org.freedesktop.login1.Manager Suspend b false"));
}

TEST(SessionManagerHostSleepTests, SuspendIsNotAttemptedWhenLogindCannotSuspend) {
  HostSleepCommandHarness harness;
  harness.can_suspend_answer = "s \"na\"";

  const auto result = session_manager::suspend_host();

  EXPECT_FALSE(result.ok);
  EXPECT_EQ("not_available", result.reason);
  EXPECT_FALSE(harness.asked("Manager Suspend"));
}

TEST(SessionManagerHostSleepTests, PolkitRefusalOfTheRequestIsNamed) {
  HostSleepCommandHarness harness;
  harness.suspend_answer = "Call failed: Interactive authentication required.";

  const auto result = session_manager::suspend_host();

  EXPECT_FALSE(result.ok);
  EXPECT_EQ("polkit_denied", result.reason);
  EXPECT_NE(std::string::npos, result.message.find("org.freedesktop.login1.suspend"));
}

TEST(SessionManagerHostSleepTests, AClockGapIsWhatCountsAsASuspend) {
  using namespace std::chrono_literals;

  // Both clocks advanced together: the host was awake the whole time.
  EXPECT_FALSE(session_manager::suspend_was_observed(30s, 30s));
  // Scheduling noise between the two reads must not read as a suspend.
  EXPECT_FALSE(session_manager::suspend_was_observed(30s + 200ms, 30s));
  // CLOCK_BOOTTIME counts the sleep and CLOCK_MONOTONIC does not, so the gap
  // between them is time the machine spent suspended.
  EXPECT_TRUE(session_manager::suspend_was_observed(90s, 30s));
}

TEST(SessionManagerHostSleepTests, AnAcceptedRequestThatNeverSleepsIsReportedAsFailed) {
  HostSleepCommandHarness harness;

  const auto result = session_manager::suspend_host();
  // logind took the request, which is all the caller can know at this point.
  EXPECT_TRUE(result.ok);

  session_manager::await_suspend_watcher_for_tests();
  const auto status = session_manager::host_sleep_status();

  // Nothing suspended, so the host never went down and the client has to be
  // able to take back "your host is going to sleep".
  EXPECT_EQ(session_manager::host_sleep_outcome_e::failed, status.outcome);
  EXPECT_EQ("suspend_failed", status.reason);
  EXPECT_FALSE(status.message.empty());
  EXPECT_GT(status.observed_at, 0);
}

TEST(SessionManagerHostSleepTests, ARefusedRequestStartsNoWatchAndLeavesNoOutcome) {
  HostSleepCommandHarness harness;
  harness.can_suspend_answer = "s \"na\"";

  const auto result = session_manager::suspend_host();

  EXPECT_FALSE(result.ok);
  // A request that was never accepted must not leave a pending outcome behind
  // for a client to read as "it might still be going to sleep".
  EXPECT_EQ(session_manager::host_sleep_outcome_e::none,
            session_manager::host_sleep_status().outcome);
}

TEST(SessionManagerHostSleepTests, UnknownFailureKeepsLogindsOwnWords) {
  HostSleepCommandHarness harness;
  harness.suspend_answer = "Call failed: Transport endpoint is not connected";

  const auto result = session_manager::suspend_host();

  EXPECT_FALSE(result.ok);
  EXPECT_EQ("request_failed", result.reason);
  EXPECT_NE(std::string::npos, result.message.find("Transport endpoint is not connected"));
}

TEST(SessionManagerUnlockTests, StillLockedAfterAllAttemptsReturnsFalse) {
  SessionManagerCommandHarness harness;
  harness.dbus_run_ok = false;
  harness.loginctl_run_ok = true;
  harness.loginctl_clears_lock = false;
  setenv("XDG_SESSION_ID", "1", 1);

  EXPECT_FALSE(session_manager::unlock_screen());

  EXPECT_TRUE(harness.locked);
  EXPECT_TRUE(harness.ran("org.freedesktop.ScreenSaver.SetActive"));
  EXPECT_TRUE(harness.ran("loginctl unlock-session '1'"));
}
#endif
