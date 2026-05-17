/**
 * @file tests/unit/platform/test_session_manager.cpp
 * @brief Test Linux session unlock policy helpers.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/session_manager.h>

  #include <algorithm>
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
    std::vector<std::string> run_commands;

    SessionManagerCommandHarness() {
      session_manager::set_command_hooks_for_tests(
        [this](const std::string &cmd) {
          if (cmd.find("org.freedesktop.ScreenSaver.GetActive") != std::string::npos) {
            return locked ? std::string {"true"} : std::string {"false"};
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
            if (loginctl_run_ok && loginctl_clears_lock) {
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
    }

    bool ran(std::string_view needle) const {
      return std::any_of(run_commands.begin(), run_commands.end(), [needle](const std::string &cmd) {
        return cmd.find(needle) != std::string::npos;
      });
    }
  };
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

TEST(SessionManagerUnlockTests, MissingSessionIdUsesAllSessionsLoginctlFallback) {
  SessionManagerCommandHarness harness;
  harness.loginctl_clears_lock = true;
  unsetenv("XDG_SESSION_ID");

  EXPECT_TRUE(session_manager::unlock_screen());

  EXPECT_FALSE(harness.locked);
  EXPECT_TRUE(harness.ran("loginctl unlock-sessions"));
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
