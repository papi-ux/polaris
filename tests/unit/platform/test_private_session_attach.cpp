/**
 * @file tests/unit/platform/test_private_session_attach.cpp
 * @brief Test the private-session attach verdict and the Flatpak portal warning.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/private_session_attach.h>

  #include <chrono>
  #include <string>

namespace {
  using namespace std::chrono_literals;
  using private_session_attach::evaluate;
  using private_session_attach::may_lose_display_to_flatpak_portal;
  using private_session_attach::probe_result_t;
  using private_session_attach::probe_status_e;
  using private_session_attach::verdict_e;

  constexpr auto k_grace = 60000ms;

  probe_result_t measured(int toplevels) {
    return probe_result_t {.status = probe_status_e::ok, .toplevel_count = toplevels};
  }
}  // namespace

TEST(PrivateSessionAttachTests, AWindowInTheSessionIsAnAttachRegardlessOfElapsedTime) {
  EXPECT_EQ(evaluate(measured(1), 0ms, k_grace), verdict_e::attached);
  EXPECT_EQ(evaluate(measured(3), k_grace * 2, k_grace), verdict_e::attached);
}

TEST(PrivateSessionAttachTests, AnEmptySessionInsideTheGracePeriodIsStillWaiting) {
  EXPECT_EQ(evaluate(measured(0), 0ms, k_grace), verdict_e::waiting);
  EXPECT_EQ(evaluate(measured(0), k_grace - 1ms, k_grace), verdict_e::waiting);
}

TEST(PrivateSessionAttachTests, AnEmptySessionPastTheGracePeriodIsTheReportedFailure) {
  // Issue #234: the app runs, the stream connects, and nothing ever appears in
  // the private compositor because the window went to the host session instead.
  EXPECT_EQ(evaluate(measured(0), k_grace, k_grace), verdict_e::never_attached);
  EXPECT_EQ(evaluate(measured(0), k_grace * 2, k_grace), verdict_e::never_attached);
}

TEST(PrivateSessionAttachTests, ACompositorWithoutTheProtocolIsSkippedNotAccused) {
  const probe_result_t unsupported {.status = probe_status_e::unsupported, .toplevel_count = 0};
  EXPECT_EQ(evaluate(unsupported, 0ms, k_grace), verdict_e::skipped);
  EXPECT_EQ(evaluate(unsupported, k_grace * 2, k_grace), verdict_e::skipped);
}

TEST(PrivateSessionAttachTests, AFailedMeasurementNeverBecomesAFailedLaunch) {
  // An unreachable compositor means Polaris learned nothing. Reporting that as
  // "the app never attached" would invent a defect out of a broken probe.
  const probe_result_t unavailable {.status = probe_status_e::unavailable, .toplevel_count = 0};
  EXPECT_EQ(evaluate(unavailable, 0ms, k_grace), verdict_e::waiting);
  EXPECT_EQ(evaluate(unavailable, k_grace * 2, k_grace), verdict_e::skipped);
}

TEST(PrivateSessionAttachTests, ProbingAnAbsentSocketReportsUnavailableRatherThanEmpty) {
  const auto result = private_session_attach::probe_toplevels("polaris-no-such-socket");
  EXPECT_EQ(result.status, probe_status_e::unavailable);
  EXPECT_EQ(result.toplevel_count, 0);
  EXPECT_EQ(evaluate(result, 0ms, k_grace), verdict_e::waiting);
}

TEST(PrivateSessionAttachTests, ProbingAnEmptySocketNameIsUnavailable) {
  EXPECT_EQ(private_session_attach::probe_toplevels("").status, probe_status_e::unavailable);
}

TEST(PrivateSessionAttachTests, FlatpakLaunchersAreFlaggedForThePortalDisplayHop) {
  EXPECT_TRUE(may_lose_display_to_flatpak_portal("flatpak run io.github.Faugus.faugus-launcher"));
  EXPECT_TRUE(may_lose_display_to_flatpak_portal("/usr/bin/flatpak run net.lutris.Lutris"));
  EXPECT_TRUE(may_lose_display_to_flatpak_portal("flatpak --user run com.heroicgameslauncher.hgl"));
  EXPECT_TRUE(may_lose_display_to_flatpak_portal("env DISPLAY=:2 flatpak run com.valvesoftware.Steam"));
  EXPECT_TRUE(may_lose_display_to_flatpak_portal("flatpak-spawn --host mygame"));
  EXPECT_TRUE(
    may_lose_display_to_flatpak_portal("~/.local/share/flatpak/exports/bin/net.lutris.Lutris")
  );
}

TEST(PrivateSessionAttachTests, NonPortalCommandsAreNotFlagged) {
  EXPECT_FALSE(may_lose_display_to_flatpak_portal(""));
  EXPECT_FALSE(may_lose_display_to_flatpak_portal("steam steam://rungameid/870780"));
  EXPECT_FALSE(may_lose_display_to_flatpak_portal("/usr/bin/lutris lutris:rungameid/1"));
  // A subcommand that launches nothing must not produce a launch warning.
  EXPECT_FALSE(may_lose_display_to_flatpak_portal("flatpak list --app"));
}

TEST(PrivateSessionAttachTests, ANativeRunnerStoredUnderAFlatpakDataDirIsNotFlagged) {
  // The reporter's non-Flatpak A/B in issue #234 runs umu-run straight from a
  // Flatpak app's data directory. It never touches the portal, so warning about
  // it would point the next reader at the wrong hop entirely.
  EXPECT_FALSE(may_lose_display_to_flatpak_portal(
    "~/.var/app/io.github.Faugus.faugus-launcher/data/faugus-launcher/umu-run game.exe"
  ));
}

#endif
