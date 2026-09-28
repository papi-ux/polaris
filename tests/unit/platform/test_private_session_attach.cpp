/**
 * @file tests/unit/platform/test_private_session_attach.cpp
 * @brief Test the private-session attach verdict and the Flatpak portal warning.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/private_app_stop.h>
  #include <src/platform/linux/private_session_attach.h>

  #include <atomic>
  #include <chrono>
  #include <future>
  #include <memory>
  #include <string>
  #include <thread>

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

TEST(PrivateSessionAttachTests, EitherSignalSeeingAWindowIsAnAttach) {
  // Issue #415: a fullscreen game can map an override-redirect window, which the
  // Wayland toplevel list never reports but the X root does. Either signal alone
  // is enough to prove the app arrived.
  using private_session_attach::combine;
  const probe_result_t none {.status = probe_status_e::ok, .toplevel_count = 0};

  EXPECT_EQ(combine(measured(1), none).toplevel_count, 1);
  EXPECT_EQ(combine(none, measured(1)).toplevel_count, 1);
  // Overlapping signals: one managed window shows up in both, so the count is a
  // lower bound rather than a sum. Reporting 5 here would mean logging one window
  // as two on every ordinary launch.
  EXPECT_EQ(combine(measured(2), measured(3)).toplevel_count, 3);
  EXPECT_EQ(combine(measured(1), measured(1)).toplevel_count, 1);
  EXPECT_EQ(combine(none, none).status, probe_status_e::ok);
  EXPECT_EQ(evaluate(combine(none, measured(1)), 0ms, k_grace), verdict_e::attached);
}

TEST(PrivateSessionAttachTests, OneWorkingSignalIsNotSuppressedByTheOtherFailing) {
  // A session with no Xwayland at all must not lose the Wayland verdict, and an
  // unreachable compositor must not erase a window the X root already reported.
  using private_session_attach::combine;
  const probe_result_t dead {.status = probe_status_e::unavailable, .toplevel_count = 0};
  const probe_result_t absent {.status = probe_status_e::unsupported, .toplevel_count = 0};

  EXPECT_EQ(combine(measured(1), dead).status, probe_status_e::ok);
  EXPECT_EQ(combine(measured(1), dead).toplevel_count, 1);
  EXPECT_EQ(combine(absent, measured(2)).toplevel_count, 2);
  EXPECT_EQ(evaluate(combine(measured(0), dead), k_grace, k_grace), verdict_e::never_attached);
}

TEST(PrivateSessionAttachTests, TwoFailedMeasurementsStayFailedMeasurements) {
  using private_session_attach::combine;
  const probe_result_t dead {.status = probe_status_e::unavailable, .toplevel_count = 0};
  const probe_result_t absent {.status = probe_status_e::unsupported, .toplevel_count = 0};

  EXPECT_EQ(combine(dead, dead).status, probe_status_e::unavailable);
  EXPECT_EQ(combine(absent, dead).status, probe_status_e::unsupported);
  EXPECT_EQ(combine(dead, absent).status, probe_status_e::unsupported);
  // Neither combination may ever read as a launch that failed to attach.
  EXPECT_EQ(evaluate(combine(dead, dead), k_grace * 2, k_grace), verdict_e::skipped);
  EXPECT_EQ(evaluate(combine(absent, dead), k_grace * 2, k_grace), verdict_e::skipped);
}

TEST(PrivateSessionAttachTests, ProbingAnAbsentXDisplayReportsUnavailable) {
  EXPECT_EQ(private_session_attach::probe_x11_windows("").status, probe_status_e::unavailable);
  EXPECT_EQ(
    private_session_attach::probe_x11_windows(":no-such-polaris-display").status,
    probe_status_e::unavailable
  );
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

TEST(PrivateSessionAttachTests, OnlyTheAppsOwnManagedWindowsThatAskForACloseAreAskedToClose) {
  using private_session_attach::x11_window_t;
  const auto app = [](std::uint32_t pid) {
    return pid == 4200 || pid == 4201;
  };
  const std::vector<x11_window_t> windows {
    {1, true, false, 4200u, true},  // the game's window
    {2, true, false, 4201u, true},  // a second window of the game's lineage
    {3, true, false, 999u, true},  // Steam's own window in the same session
    {4, true, false, std::nullopt, true},  // names no process, so cannot be told apart from Steam's
    {5, true, false, 4200u, false},  // never asked for WM_DELETE_WINDOW; closing it is a kill
    {6, false, false, 4200u, true},  // not on screen
    {7, true, true, 4200u, true},  // a menu or tooltip no window manager closes
  };
  EXPECT_EQ(private_session_attach::close_targets(windows, app), (std::vector<std::uint32_t> {1, 2}));
  EXPECT_TRUE(private_session_attach::close_targets(windows, std::function<bool(std::uint32_t)> {}).empty());
}

TEST(PrivateSessionAttachTests, CloseTargetsPreferXResClientPid) {
  // A Proton game in a Flatpak sandbox names itself 631 in _NET_WM_PID, its pid in the sandbox's
  // pid namespace. The X server knows it as 361425, from its socket, and says so through X-Resource.
  using private_session_attach::x11_window_t;
  const auto window = [](std::uint32_t id, std::optional<std::uint32_t> net_wm_pid, std::optional<std::uint32_t> client_pid) {
    x11_window_t result {id, true, false, net_wm_pid, true};
    result.client_pid = client_pid;
    return result;
  };
  const std::vector<x11_window_t> windows {
    window(1, 631u, 361425u),  // the game: its own word is a sandbox pid, the server's is the host pid
    window(2, 361425u, 4242u),  // its own word matches, the server's does not: the server decides
    window(3, std::nullopt, 361425u),  // names nothing itself, and X-Resource still names it
    window(4, 631u, std::nullopt),  // no X-Resource: only the sandbox pid, asked as such
  };
  private_session_attach::window_owner_test_t game {
    [](std::uint32_t pid) {
      return pid == 361425;
    },
    [](std::uint32_t pid) {
      return pid == 631;
    },
  };
  EXPECT_EQ(private_session_attach::close_targets(windows, game), (std::vector<std::uint32_t> {1, 3, 4}));

  // Without a _NET_WM_PID test, a window's own word is taken as a host pid, as for a game outside
  // any sandbox.
  game.net_wm_pid = {};
  EXPECT_EQ(private_session_attach::close_targets(windows, game), (std::vector<std::uint32_t> {1, 3}));
}

TEST(PrivateSessionAttachTests, CloseTargetsSkipLauncherWindows) {
  // Heroic runs in its sandbox as 13 and its game as 631, 346819 and 361425 on the host. Only the
  // game's windows are asked: asking Heroic's own to close ends Heroic before its game.
  using private_session_attach::x11_window_t;
  const auto window = [](std::uint32_t id, std::optional<std::uint32_t> net_wm_pid, std::optional<std::uint32_t> client_pid) {
    x11_window_t result {id, true, false, net_wm_pid, true};
    result.client_pid = client_pid;
    return result;
  };
  const auto owners = private_app_stop::window_owner_test({{361425}, {631}, {13}});
  const std::vector<x11_window_t> with_x_resource {
    window(1, 631u, 361425u),  // the game
    window(2, 13u, 346819u),  // Heroic, were it an X11 client
  };
  EXPECT_EQ(private_session_attach::close_targets(with_x_resource, owners), (std::vector<std::uint32_t> {1}));
  const std::vector<x11_window_t> without_x_resource {
    window(1, 631u, std::nullopt),
    window(2, 13u, std::nullopt),
  };
  EXPECT_EQ(private_session_attach::close_targets(without_x_resource, owners), (std::vector<std::uint32_t> {1}));
}

TEST(PrivateSessionAttachTests, FlatpakRunNamesTheAppItStarts) {
  using private_session_attach::flatpak_run_app_id;
  EXPECT_EQ(flatpak_run_app_id("setsid flatpak run com.heroicgameslauncher.hgl 'heroic://launch?appName=x&runner=legendary'"), "com.heroicgameslauncher.hgl");
  EXPECT_EQ(flatpak_run_app_id("flatpak run --branch=stable --command=lutris net.lutris.Lutris lutris:rungame/x"), "net.lutris.Lutris");
  EXPECT_EQ(flatpak_run_app_id("/usr/bin/flatpak run app/org.libretro.RetroArch/x86_64/stable"), "org.libretro.RetroArch");
  EXPECT_EQ(flatpak_run_app_id("/var/lib/flatpak/exports/bin/io.github.ryubing.Ryujinx game.nsp"), "io.github.ryubing.Ryujinx");
  EXPECT_FALSE(flatpak_run_app_id("flatpak list"));
  EXPECT_FALSE(flatpak_run_app_id("setsid steam steam://rungameid/1234"));
  EXPECT_FALSE(flatpak_run_app_id(""));
}

TEST(PrivateSessionAttachTests, AskingAnAbsentXDisplayToCloseAsksNothing) {
  const auto result = private_session_attach::request_x11_window_close(
    ":polaris-test-no-such-display",
    {[](std::uint32_t) {
       return true;
     },
     {}}
  );
  EXPECT_EQ(result.status, private_session_attach::probe_status_e::unavailable);
  EXPECT_EQ(result.windows_asked, 0);
  EXPECT_EQ(private_session_attach::request_x11_window_close("", {}).windows_asked, 0);
}

TEST(PrivateSessionAttachTests, AnXDisplayThatStopsAnsweringCannotHoldTheStop) {
  // The stop asks under the session lifecycle lock, and xcb waits for a reply without a timeout. A
  // wedged Xwayland used to be able to hold a launch and a stop request's answer with it.
  auto release = std::make_shared<std::promise<void>>();
  auto released = release->get_future().share();
  auto finished = std::make_shared<std::atomic<bool>>(false);
  const auto started = std::chrono::steady_clock::now();
  const auto result = private_session_attach::request_x11_window_close_within(
    ":wedged",
    {[](std::uint32_t) {
       return true;
     },
     {}},
    100ms,
    [released, finished](const std::string &, const private_session_attach::window_owner_test_t &) {
      released.wait();
      finished->store(true);
      private_session_attach::close_request_result_t late {};
      late.status = probe_status_e::ok;
      late.windows_asked = 3;
      return late;
    }
  );
  const auto waited = std::chrono::steady_clock::now() - started;
  EXPECT_TRUE(result.timed_out);
  EXPECT_EQ(result.windows_asked, 0) << "nothing is known to have been asked";
  EXPECT_EQ(result.status, probe_status_e::unavailable);
  EXPECT_LT(waited, 5s) << "the stop went on without the display";
  EXPECT_FALSE(finished->load());

  // Let the exchange end, as it does once the display goes away with the private session.
  release->set_value();
  for (int i = 0; i < 200 && !finished->load(); ++i) {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_TRUE(finished->load());
}

TEST(PrivateSessionAttachTests, AnXDisplayThatAnswersInTimeGivesItsAnswer) {
  // Shared, because a thread that timed out would outlive this test.
  auto asked_display = std::make_shared<std::string>();
  const auto result = private_session_attach::request_x11_window_close_within(
    ":1",
    {[](std::uint32_t pid) {
       return pid == 4200;
     },
     {}},
    5s,
    [asked_display](const std::string &display, const private_session_attach::window_owner_test_t &belongs) {
      *asked_display = display;
      private_session_attach::close_request_result_t answered {};
      answered.status = probe_status_e::ok;
      answered.windows_asked = belongs.host_pid(4200) && !belongs.host_pid(999) ? 2 : 0;
      return answered;
    }
  );
  EXPECT_FALSE(result.timed_out);
  EXPECT_EQ(result.status, probe_status_e::ok);
  EXPECT_EQ(result.windows_asked, 2);
  EXPECT_EQ(*asked_display, ":1");

  // No display, or nothing to ask with, asks nothing and starts nothing.
  EXPECT_EQ(private_session_attach::request_x11_window_close_within("", {}, 1s).windows_asked, 0);
  EXPECT_FALSE(private_session_attach::request_x11_window_close_within("", {}, 1s).timed_out);
}

#endif
