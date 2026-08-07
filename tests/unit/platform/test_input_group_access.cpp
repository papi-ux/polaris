/**
 * @file tests/unit/platform/test_input_group_access.cpp
 * @brief Test the input-group preflight that seat isolation depends on.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/input/input_group_access.h>

  #include <cstdlib>
  #include <string>

namespace {
  using platf::input_access::input_group_status_t;
  using platf::input_access::seat_isolation_access_warning;
  using platf::input_access::seat_isolation_options_t;

  constexpr seat_isolation_options_t gamepad_only {true, false};
  constexpr seat_isolation_options_t keyboard_mouse_only {false, true};
  constexpr seat_isolation_options_t both {true, true};
  constexpr seat_isolation_options_t neither {false, false};

  input_group_status_t member() {
    return {"papi", true, true};
  }

  input_group_status_t not_a_member() {
    return {"papi", true, false};
  }

  input_group_status_t no_such_group() {
    return {"papi", false, false};
  }

  bool contains(const std::string &text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
  }
}  // namespace

TEST(InputGroupAccessTests, NothingIsReportedWhenTheAccountCanOpenTheDevices) {
  EXPECT_TRUE(seat_isolation_access_warning(both, member()).empty());
  EXPECT_TRUE(seat_isolation_access_warning(gamepad_only, member()).empty());
  EXPECT_TRUE(seat_isolation_access_warning(keyboard_mouse_only, member()).empty());
}

TEST(InputGroupAccessTests, NothingIsReportedWhenSeatIsolationIsDisabled) {
  // Isolation off means the devices keep their uaccess ACL, so membership does
  // not matter and warning about it would be noise on every default install.
  EXPECT_TRUE(seat_isolation_access_warning(neither, not_a_member()).empty());
  EXPECT_TRUE(seat_isolation_access_warning(neither, no_such_group()).empty());
}

TEST(InputGroupAccessTests, MissingMembershipNamesTheAccountAndTheFix) {
  const auto warning = seat_isolation_access_warning(gamepad_only, not_a_member());
  ASSERT_FALSE(warning.empty());

  EXPECT_TRUE(contains(warning, "papi"));
  EXPECT_TRUE(contains(warning, "sudo usermod -aG input papi"));
  // A usermod that appears to do nothing until the next login is the second
  // support round-trip, so the message has to say so.
  EXPECT_TRUE(contains(warning, "log out and back in"));
  EXPECT_TRUE(contains(warning, "Disabling seat isolation"));
}

TEST(InputGroupAccessTests, TheWarningNamesOnlyTheDevicesTheEnabledOptionsCover) {
  EXPECT_TRUE(contains(seat_isolation_access_warning(gamepad_only, not_a_member()), "client gamepads"));
  EXPECT_FALSE(contains(seat_isolation_access_warning(gamepad_only, not_a_member()), "keyboard"));

  EXPECT_TRUE(contains(
    seat_isolation_access_warning(keyboard_mouse_only, not_a_member()),
    "the virtual keyboard and mouse"
  ));
  EXPECT_FALSE(contains(seat_isolation_access_warning(keyboard_mouse_only, not_a_member()), "gamepads"));

  const auto warning = seat_isolation_access_warning(both, not_a_member());
  EXPECT_TRUE(contains(warning, "client gamepads"));
  EXPECT_TRUE(contains(warning, "keyboard and mouse"));
}

TEST(InputGroupAccessTests, AHostWithoutTheGroupIsReportedAsADifferentProblem) {
  const auto warning = seat_isolation_access_warning(both, no_such_group());
  ASSERT_FALSE(warning.empty());

  EXPECT_TRUE(contains(warning, "no [input] group"));
  // usermod cannot add anybody to a group that does not exist, so offering the
  // command here would send the user down a dead end.
  EXPECT_FALSE(contains(warning, "usermod"));
  EXPECT_FALSE(contains(warning, "log out and back in"));
  EXPECT_TRUE(contains(warning, "Disabling seat isolation"));
}

TEST(InputGroupAccessTests, TheWarningSaysWhatActuallyBreaks) {
  // The reported symptom is a controller that never appears, which reads as a
  // broken feature. The message has to connect the two.
  const auto warning = seat_isolation_access_warning(gamepad_only, not_a_member());
  EXPECT_TRUE(contains(warning, "streamed game will not see them"));
}

TEST(InputGroupAccessTests, AnOstreeHostIsToldTheCommandThatActuallyWorks) {
  using platf::input_access::input_group_remedy_command;

  // On ostree images the input group is defined in /usr/lib/group, not
  // /etc/group, so `usermod -aG input` fails outright — the database it writes
  // has no such group. Reported from Bazzite in #274, where following our
  // advice accomplished nothing at all.
  const auto ublue = input_group_remedy_command("papi", true, true);
  EXPECT_EQ("ujust add-user-to-input-group", ublue);

  // Without ujust, the same two steps by hand: copy the group definition
  // across first, then the usermod that can now find it.
  const auto manual = input_group_remedy_command("papi", true, false);
  EXPECT_NE(manual.find("/usr/lib/group"), std::string::npos);
  EXPECT_NE(manual.find("/etc/group"), std::string::npos);
  EXPECT_NE(manual.find("usermod -aG input papi"), std::string::npos);
}

TEST(InputGroupAccessTests, AnOrdinaryHostStillGetsPlainUsermod) {
  using platf::input_access::input_group_remedy_command;

  const auto command = input_group_remedy_command("papi", false, false);
  EXPECT_EQ("sudo usermod -aG input papi", command);

  // ujust being present does not make a non-ostree host use it.
  EXPECT_EQ("sudo usermod -aG input papi", input_group_remedy_command("papi", false, true));
}

TEST(InputGroupAccessTests, SetupHostReportsOnTheAccountThatInvokedSudo) {
  using platf::input_access::setup_host_target_user;

  // A name no account on the machine running this can hold, so the assertions
  // cannot pass by coincidence on a host whose user happens to share it.
  constexpr auto sentinel = "polaris-setup-host-sentinel";

  const auto *original = std::getenv("SUDO_USER");
  const std::string restore = original ? original : "";

  // Host setup runs as root, so the process' own account is never the one that
  // will stream.
  ASSERT_EQ(0, ::setenv("SUDO_USER", sentinel, 1));
  EXPECT_EQ(sentinel, setup_host_target_user());

  ASSERT_EQ(0, ::unsetenv("SUDO_USER"));
  const auto fallback = setup_host_target_user();
  EXPECT_NE(sentinel, fallback);
  EXPECT_FALSE(fallback.empty());

  if (!restore.empty()) {
    ASSERT_EQ(0, ::setenv("SUDO_USER", restore.c_str(), 1));
  }
}

TEST(InputGroupAccessTests, TheProcessAndDatabaseLookupsAgreeOnThisHost) {
  // The startup warning reads the process' own supplementary groups and
  // --setup-host reads the passwd/group database, because the two answer
  // different questions: what the kernel will enforce now, and what the account
  // is configured for. On a host where nothing has changed mid-session they
  // have to agree, and disagreeing means one of the two lookups is wrong.
  const auto from_process = platf::input_access::current_input_group_status();
  const auto from_database = platf::input_access::input_group_status_for_user(from_process.user);

  EXPECT_EQ(from_process.group_exists, from_database.group_exists);
  EXPECT_EQ(from_process.member, from_database.member);
}

TEST(InputGroupAccessTests, SetupHostAdviceIsSilentForAnAccountThatIsAlreadyReady) {
  using platf::input_access::setup_host_input_group_advice;

  // Host setup cannot read the configuration, so its advice is conditional
  // rather than gated on the options; the one thing it can tell is whether the
  // account would be ready if seat isolation were turned on.
  const auto status = platf::input_access::input_group_status_for_user(
    platf::input_access::setup_host_target_user()
  );
  if (status.member) {
    EXPECT_TRUE(setup_host_input_group_advice().empty());
  } else {
    EXPECT_FALSE(setup_host_input_group_advice().empty());
  }
}

#endif
