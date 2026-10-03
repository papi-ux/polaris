/**
 * @file tests/unit/platform/test_private_app_stop.cpp
 * @brief The order a private session's apps are stopped in, its bounds, and which processes it may
 *        touch, with every action injected and a clock that only moves when a step waits.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/private_app_stop.h>

  #include <algorithm>
  #include <map>
  #include <optional>
  #include <set>
  #include <string>
  #include <vector>

namespace {
  namespace pas = private_app_stop;
  namespace fsi = flatpak_session_instances;
  using namespace std::chrono_literals;
  using namespace std::string_literals;
  using time_point = std::chrono::steady_clock::time_point;

  /// A session the steps act on: a game, launchers, and a clock that only moves when a step waits.
  struct fake_session_t {
    time_point clock {};
    std::vector<std::string> events;

    // How the game answers.
    bool game_running = true;
    std::optional<std::chrono::milliseconds> closes_after;  ///< after the close request
    std::optional<std::chrono::milliseconds> exits_after_sigterm;
    std::optional<std::chrono::milliseconds> dies_after_sigkill = 50ms;
    int windows = 1;
    std::chrono::milliseconds ask_takes = 5ms;
    std::optional<time_point> game_exits_at;

    // A helper of the session's own that exits only when signalled, as a user's autostart may run.
    // With one, the fake tells what the close request asked from everything of the session's.
    bool helper = false;
    std::optional<time_point> helper_exits_at;

    std::optional<time_point> app_exits_at() const {
      if (!helper) {
        return game_exits_at;
      }
      if (!game_exits_at || !helper_exits_at) {
        return std::nullopt;
      }
      return std::max(*game_exits_at, *helper_exits_at);
    }

    // How the launchers answer.
    std::vector<bool> launcher_running;
    std::optional<std::chrono::milliseconds> launcher_quits_after_sigterm = 400ms;
    std::optional<std::chrono::milliseconds> launcher_dies_after_sigkill = 20ms;
    std::chrono::milliseconds settle_takes = 800ms;
    std::vector<std::optional<time_point>> launcher_exits_at;

    std::map<std::string, std::vector<std::chrono::milliseconds>> waits;

    void exit_game_at(time_point when) {
      if (!game_exits_at || when < *game_exits_at) {
        game_exits_at = when;
      }
    }

    bool wait_until(std::optional<time_point> when, std::chrono::milliseconds timeout) {
      if (when && *when <= clock + timeout) {
        clock = std::max(clock, *when);
        return true;
      }
      clock += timeout;
      return false;
    }

    std::size_t index_of(const std::string &event) const {
      const auto found = std::find(events.begin(), events.end(), event);
      return found == events.end() ? events.size() + 1000 : static_cast<std::size_t>(found - events.begin());
    }

    std::size_t count_of(const std::string &event) const {
      return static_cast<std::size_t>(std::count(events.begin(), events.end(), event));
    }

    pas::actions_t actions() {
      pas::actions_t actions;
      launcher_exits_at.resize(launcher_running.size());
      if (!game_running) {
        game_exits_at = clock;
      }
      for (std::size_t i = 0; i < launcher_running.size(); ++i) {
        if (!launcher_running[i]) {
          launcher_exits_at[i] = clock;
        }
      }
      actions.now = [this]() {
        return clock;
      };
      actions.ask_app_to_close = [this](std::chrono::milliseconds timeout) {
        events.push_back("ask");
        waits["ask"].push_back(timeout);
        clock += std::min(ask_takes, timeout);
        if (ask_takes > timeout) {
          return 0;  // the display did not answer in time
        }
        if (closes_after) {
          exit_game_at(clock + *closes_after);
        }
        return windows;
      };
      actions.wait_app = [this](std::chrono::milliseconds timeout) {
        events.push_back("wait_app");
        waits["wait_app"].push_back(timeout);
        return wait_until(app_exits_at(), timeout);
      };
      if (helper) {
        actions.wait_asked = [this](std::chrono::milliseconds timeout) {
          events.push_back("wait_asked");
          waits["wait_asked"].push_back(timeout);
          return wait_until(game_exits_at, timeout);
        };
      }
      actions.sigterm_app = [this]() {
        events.push_back("sigterm_app");
        if (exits_after_sigterm) {
          exit_game_at(clock + *exits_after_sigterm);
        }
        if (helper && !helper_exits_at) {
          helper_exits_at = clock + 10ms;
        }
        return 3;
      };
      actions.sigkill_app = [this]() {
        events.push_back("sigkill_app");
        if (dies_after_sigkill) {
          exit_game_at(clock + *dies_after_sigkill);
        }
        if (helper && !helper_exits_at) {
          helper_exits_at = clock + 50ms;
        }
      };
      actions.launcher_alive = [this](std::size_t i) {
        return !launcher_exits_at[i] || *launcher_exits_at[i] > clock;
      };
      actions.settle_launchers = [this](std::chrono::milliseconds timeout) {
        events.push_back("settle");
        waits["settle"].push_back(timeout);
        return wait_until(clock + settle_takes, timeout);
      };
      actions.sigterm_launcher = [this](std::size_t i) {
        events.push_back("sigterm_launcher:" + std::to_string(i));
        if (launcher_quits_after_sigterm) {
          launcher_exits_at[i] = clock + *launcher_quits_after_sigterm;
        }
        return 4;
      };
      actions.wait_launcher = [this](std::size_t i, std::chrono::milliseconds timeout) {
        events.push_back("wait_launcher:" + std::to_string(i));
        waits["wait_launcher"].push_back(timeout);
        return wait_until(launcher_exits_at[i], timeout);
      };
      actions.sigkill_launcher_init = [this](std::size_t i) {
        events.push_back("sigkill_launcher_init:" + std::to_string(i));
        if (launcher_dies_after_sigkill) {
          launcher_exits_at[i] = clock + *launcher_dies_after_sigkill;
        }
      };
      actions.wait_scopes = [this](std::chrono::milliseconds timeout) {
        events.push_back("wait_scopes");
        waits["wait_scopes"].push_back(timeout);
        std::optional<time_point> last = clock;
        for (const auto &exits : launcher_exits_at) {
          if (!exits) {
            last.reset();
            break;
          }
          last = std::max(*last, *exits);
        }
        return wait_until(last, timeout);
      };
      return actions;
    }
  };

  pas::plan_t heroic_plan(bool immediate = false) {
    pas::plan_t plan;
    plan.immediate = immediate;
    plan.exit_timeout = 5s;
    plan.has_app = true;
    plan.launchers = {"com.heroicgameslauncher.hgl"};
    return plan;
  }

  fsi::member_t member(pid_t pid, pid_t parent, std::vector<pid_t> nspid, bool token, std::string comm) {
    fsi::member_t result;
    result.process = {pid, static_cast<std::uint64_t>(pid)};
    result.parent = parent;
    result.nspid = std::move(nspid);
    result.carries_token = token;
    result.comm = std::move(comm);
    return result;
  }

  pas::process_facts_t fact(
    pid_t pid,
    pid_t parent,
    std::uint64_t start,
    std::string comm,
    std::optional<std::string> environ,
    std::string cgroup = "/user.slice/user-1000.slice/user@1000.service/app.slice/app-polaris.scope",
    std::vector<pid_t> nspid = {},
    uid_t uid = 1000
  ) {
    pas::process_facts_t result;
    result.process = {pid, start};
    result.parent = parent;
    result.comm = std::move(comm);
    result.environ = std::move(environ);
    result.cgroup = std::move(cgroup);
    result.nspid = nspid.empty() ? std::vector<pid_t> {pid} : std::move(nspid);
    result.uid = uid;
    return result;
  }

  const std::string token = "022C50EB74B10CE9BD3E1AF593A98022";

  std::string with_token() {
    return "HOME=/var/empty\0POLARIS_SESSION_INSTANCE_ID="s + token + "\0"s;
  }

  std::set<pid_t> pids_of(const std::vector<fsi::process_t> &processes) {
    std::set<pid_t> pids;
    for (const auto &process : processes) {
      pids.insert(process.pid);
    }
    return pids;
  }
}  // namespace

TEST(PrivateAppStopTests, GameClosesBeforeLauncherBeforeCompositor) {
  fake_session_t session;
  session.closes_after = 4210ms;
  session.launcher_running = {true};
  const auto outcome = pas::run(heroic_plan(), session.actions());
  // The teardown stops the compositor once the phase returns, and not before.
  session.events.push_back("stop_compositor");

  EXPECT_EQ(outcome.path, "close_request");
  EXPECT_EQ(outcome.windows_asked, 1);
  EXPECT_EQ(outcome.waited, 4215ms);
  ASSERT_EQ(outcome.launchers.size(), 1u);
  EXPECT_EQ(outcome.launchers.front().path, "sigterm");
  EXPECT_TRUE(outcome.drained);
  EXPECT_EQ(session.count_of("sigterm_app"), 0u) << "a game that closes when asked is never signalled";
  EXPECT_EQ(session.count_of("sigkill_app"), 0u);
  EXPECT_EQ(session.count_of("sigkill_launcher_init:0"), 0u);
  EXPECT_LT(session.index_of("ask"), session.index_of("settle"));
  EXPECT_LT(session.index_of("settle"), session.index_of("sigterm_launcher:0"));
  EXPECT_LT(session.index_of("sigterm_launcher:0"), session.index_of("wait_launcher:0"));
  EXPECT_LT(session.index_of("wait_launcher:0"), session.index_of("wait_scopes"));
  EXPECT_EQ(session.events.back(), "stop_compositor");
  EXPECT_EQ(session.waits["wait_app"][1], 10000ms) << "an exit timeout of 5 s still gets the 10 s floor";
}

TEST(PrivateAppStopTests, CompositorNeverStopsBeforeAppPhaseReturnsEvenOnTimeouts) {
  // Nothing answers: the game ignores the close request, SIGTERM and even SIGKILL (a process stuck
  // in the kernel), and the launcher ignores SIGTERM and its backstop. The phase still returns,
  // inside its budget, having taken every step, and only then may the compositor stop.
  fake_session_t session;
  session.closes_after.reset();
  session.exits_after_sigterm.reset();
  session.dies_after_sigkill.reset();
  session.launcher_running = {true};
  session.launcher_quits_after_sigterm.reset();
  session.launcher_dies_after_sigkill.reset();
  session.settle_takes = 1h;
  const auto outcome = pas::run(heroic_plan(), session.actions());
  session.events.push_back("stop_compositor");

  EXPECT_FALSE(outcome.drained);
  EXPECT_EQ(outcome.path, "sigkill");
  EXPECT_LE(outcome.elapsed, 30s);
  EXPECT_EQ(session.count_of("sigkill_app"), 1u);
  EXPECT_EQ(session.count_of("sigkill_launcher_init:0"), 1u);
  EXPECT_EQ(session.events.back(), "stop_compositor");
  EXPECT_EQ(session.count_of("stop_compositor"), 1u);
  for (std::size_t i = 0; i + 1 < session.events.size(); ++i) {
    EXPECT_NE(session.events[i], "stop_compositor");
  }
}

TEST(PrivateAppStopTests, UnansweredCloseEscalatesSigtermThenSigkillWithinBudget) {
  fake_session_t session;
  session.closes_after.reset();
  session.exits_after_sigterm.reset();
  pas::plan_t plan;
  plan.exit_timeout = 5s;
  plan.has_app = true;
  const auto outcome = pas::run(plan, session.actions());
  EXPECT_EQ(outcome.path, "sigkill");
  EXPECT_TRUE(outcome.drained);
  EXPECT_EQ(
    session.events,
    (std::vector<std::string> {"wait_app", "ask", "wait_app", "sigterm_app", "wait_app", "sigkill_app", "wait_app"})
  );
  EXPECT_EQ(session.waits["wait_app"][1], 10000ms) << "the close request's wait";
  EXPECT_EQ(session.waits["wait_app"][2], 5000ms) << "SIGTERM's grace after an unanswered close";
  EXPECT_EQ(session.waits["wait_app"][3], 3000ms) << "the SIGKILL wait";
  EXPECT_LE(outcome.elapsed, 30s);

  // With nothing to ask, SIGTERM gets the longer grace.
  fake_session_t windowless;
  windowless.windows = 0;
  windowless.exits_after_sigterm = 7s;
  const auto sigterm = pas::run(plan, windowless.actions());
  EXPECT_EQ(sigterm.path, "sigterm");
  EXPECT_EQ(windowless.waits["wait_app"][1], 10000ms);
}

TEST(PrivateAppStopTests, ImmediateSkipsCloseRequestAndSettle) {
  fake_session_t session;
  session.closes_after = 1s;
  session.exits_after_sigterm = 1500ms;
  session.launcher_running = {true};
  session.launcher_quits_after_sigterm.reset();
  const auto outcome = pas::run(heroic_plan(true), session.actions());
  EXPECT_EQ(session.count_of("ask"), 0u);
  EXPECT_EQ(session.count_of("settle"), 0u);
  EXPECT_EQ(outcome.path, "sigterm");
  EXPECT_EQ(session.waits["wait_app"][1], 2000ms) << "SIGTERM gets 2 s on an immediate stop";
  EXPECT_EQ(session.waits["wait_launcher"][0], 2000ms) << "and so does the launcher";
  EXPECT_EQ(outcome.launchers.front().path, "sigkill");
}

TEST(PrivateAppStopTests, LauncherSettleWaitsForTokenCarryingHelpers) {
  fake_session_t session;
  session.closes_after = 2s;
  session.launcher_running = {true};
  session.settle_takes = 800ms;
  (void) pas::run(heroic_plan(), session.actions());
  ASSERT_EQ(session.waits["settle"].size(), 1u);
  EXPECT_EQ(session.waits["settle"][0], 3000ms);
  EXPECT_LT(session.index_of("settle"), session.index_of("sigterm_launcher:0"))
    << "legendary returns and Heroic records the playtime before Heroic is asked to quit";

  // Which of Heroic's processes the settle waits for: what it ran for the game, which carries the
  // token, and not heroic-run's shell above Electron's main process, which lives as long as Heroic.
  fsi::instance_t heroic;
  heroic.bwrap = {346422, 346422};
  heroic.init = {346430, 346430};
  const std::vector<fsi::member_t> members {
    member(346422, 346200, {346422}, false, "bwrap"),
    member(346500, 346422, {346500}, true, "bwrap"),
    member(346430, 346422, {346430, 1}, false, "bwrap"),
    member(346781, 346430, {346781, 2}, true, "sh"),
    member(346819, 346781, {346819, 13}, false, "heroic"),
    member(355000, 346819, {355000, 300}, true, "legendary"),
    member(355100, 355000, {355100, 301}, true, "gamemoderun"),
    member(355200, 355100, {355200, 302}, true, "python3"),
  };
  std::set<pid_t> settle;
  for (const auto &process : pas::settle_processes(heroic, members)) {
    settle.insert(process.process.pid);
  }
  EXPECT_EQ(settle, (std::set<pid_t> {355000, 355100, 355200}));
}

TEST(PrivateAppStopTests, BackstopTargetsSandboxInitNeverOuterBwrap) {
  fake_session_t session;
  session.closes_after = 1s;
  session.launcher_running = {true};
  session.launcher_quits_after_sigterm.reset();
  const auto outcome = pas::run(heroic_plan(), session.actions());
  EXPECT_EQ(outcome.launchers.front().path, "sigkill");
  EXPECT_EQ(session.count_of("sigkill_launcher_init:0"), 1u);
  EXPECT_LT(session.index_of("wait_launcher:0"), session.index_of("sigkill_launcher_init:0"))
    << "SIGTERM had its 5 s first";
  EXPECT_EQ(session.waits["wait_launcher"][0], 5000ms);
  EXPECT_TRUE(outcome.drained);

  // What SIGTERM and the game's SIGKILL may reach: never a bwrap. The outer bwrap and the proxy's
  // run in the host's namespace and the init is pid 1 of the sandbox's.
  fsi::instance_t heroic;
  heroic.bwrap = {346422, 346422};
  heroic.init = {346430, 346430};
  const std::vector<fsi::member_t> members {
    member(346422, 346200, {346422}, false, "bwrap"),
    member(346500, 346422, {346500}, true, "bwrap"),
    member(346430, 346422, {346430, 1}, false, "bwrap"),
    member(346819, 346430, {346819, 13}, false, "heroic"),
  };
  const auto apps = fsi::app_processes(heroic, members);
  ASSERT_EQ(apps.size(), 1u);
  EXPECT_EQ(apps.front().process.pid, 346819);
}

TEST(PrivateAppStopTests, WorstCaseStaysWithinThirtySeconds) {
  for (const bool immediate : {false, true}) {
    // An exit timeout of 30 s, a display that takes the whole second to answer and asks nothing,
    // a game and two launchers that answer nothing.
    fake_session_t session;
    session.ask_takes = 1s;
    session.closes_after.reset();
    session.exits_after_sigterm.reset();
    session.dies_after_sigkill.reset();
    session.launcher_running = {true, true};
    session.launcher_quits_after_sigterm.reset();
    session.launcher_dies_after_sigkill.reset();
    session.settle_takes = 1h;
    pas::plan_t plan = heroic_plan(immediate);
    plan.exit_timeout = 30s;
    plan.launchers.push_back("net.lutris.Lutris");
    const auto outcome = pas::run(plan, session.actions());
    EXPECT_LE(outcome.elapsed, 30s) << (immediate ? "immediate" : "normal");
    EXPECT_LE(session.clock - std::chrono::steady_clock::time_point {}, 30s);
    // The app's steps left the launchers their share: each still got SIGTERM and the backstop.
    EXPECT_EQ(session.count_of("sigterm_launcher:0"), 1u);
    EXPECT_EQ(session.count_of("sigterm_launcher:1"), 1u);
    EXPECT_EQ(session.count_of("sigkill_launcher_init:0"), 1u);
    EXPECT_EQ(session.count_of("sigkill_launcher_init:1"), 1u);
    EXPECT_GT(session.waits["wait_launcher"][0], 0ms);
  }
}

TEST(PrivateAppStopTests, SteamContextRunsAfterSteamLaneAndFindsNothing) {
  // After the Steam lane has stopped the game and Steam, nothing of the session's is left: the
  // phase takes no step and waits for nothing.
  fake_session_t session;
  pas::plan_t plan;
  plan.exit_timeout = 5s;
  const auto outcome = pas::run(plan, session.actions());
  EXPECT_TRUE(session.events.empty());
  EXPECT_TRUE(outcome.path.empty());
  EXPECT_TRUE(outcome.drained);
  EXPECT_EQ(outcome.elapsed, 0ms);

  // A game that exited on its own before the stop is recorded as such, and nothing is asked.
  fake_session_t exited;
  exited.game_running = false;
  plan.has_app = true;
  const auto gone = pas::run(plan, exited.actions());
  EXPECT_EQ(gone.path, "exited_before_stop");
  EXPECT_EQ(exited.events, (std::vector<std::string> {"wait_app"}));
}

TEST(PrivateAppStopTests, ALauncherThatQuitWithItsGameIsNotSignalled) {
  fake_session_t session;
  session.closes_after = 1s;
  session.launcher_running = {false};
  const auto outcome = pas::run(heroic_plan(), session.actions());
  EXPECT_EQ(outcome.launchers.front().path, "exited");
  EXPECT_EQ(session.count_of("settle"), 0u);
  EXPECT_EQ(session.count_of("sigterm_launcher:0"), 0u);
}

TEST(PrivateAppStopTests, CloseWaitFollowsTheAppsExitTimeout) {
  EXPECT_EQ(pas::close_wait(5s), 10000ms);
  EXPECT_EQ(pas::close_wait(0s), 10000ms);
  EXPECT_EQ(pas::close_wait(20s), 20000ms);
  EXPECT_EQ(pas::close_wait(90s), 30000ms);
}

TEST(PrivateAppStopTests, ClassifyKeepsTheCompositorLaunchChainAndSandboxesOutOfTheSession) {
  const fsi::process_t supervisor {345544, 21200000};
  const std::string scope = "/user.slice/user-1000.slice/user@1000.service/app.slice/app-flatpak-com.heroicgameslauncher.hgl-1424202439.scope";
  const std::vector<pas::process_facts_t> facts {
    fact(982284, 1, 1000, "systemd", "HOME=/var/empty\0"s),
    fact(3231794, 982284, 21100000, "polaris-kms", "HOME=/var/empty\0"s),
    fact(345544, 3231794, 21200000, "polaris-kms", with_token()),
    fact(345580, 345544, 21200010, "labwc", with_token()),
    fact(345590, 345580, 21200020, "Xwayland", with_token()),
    fact(346000, 345580, 21205000, "sh", with_token()),
    fact(346100, 346000, 21205010, "bwrap", with_token()),  // the gamepad isolation wrapper
    fact(346200, 346100, 21205020, "sh", with_token()),
    fact(346422, 346200, 21210000, "bwrap", ""s, scope),  // Heroic's outer bwrap
    fact(346430, 346422, 21210001, "bwrap", ""s, scope, {346430, 1}),
    fact(346819, 346430, 21210020, "heroic", std::string(32, '\0'), scope, {346819, 13}),
    fact(350000, 345580, 21212000, "sh", with_token()),  // a native game's shell
    fact(350010, 350000, 21212010, "game.x86_64", with_token()),
    fact(350020, 350010, 21212020, "chrome_crashpad", std::string(16, '\0')),  // no token, by lineage
    fact(350030, 350010, 21212030, "bwrap", ""s),  // a bwrap no record names, zero-byte environ
    fact(350040, 350010, 21212040, "chrome-sandbox", with_token(), "/user.slice/x.scope", {}, 0),  // setuid
    fact(350050, 350010, 21212050, "game-worker", with_token(), "/user.slice/x.scope", {350050, 7}),  // in a sandbox's namespace
    fact(2812294, 982284, 21016711, "bwrap", ""s, "/user.slice/user-1000.slice/user@1000.service/app.slice/app-flatpak-com.prusa3d.PrusaSlicer-1925812001.scope"),
    fact(2812310, 2812304, 21016720, "prusa-slicer", "HOME=/var/empty\0"s, "/user.slice/user-1000.slice/user@1000.service/app.slice/app-flatpak-com.prusa3d.PrusaSlicer-1925812001.scope", {2812310, 2}),
    fact(700000, 3231794, 21300000, "sh", "HOME=/var/empty\0"s),  // Polaris's own child, not the session's
    fact(351000, 345580, 21213000, "sh", with_token()),  // the shell of a `flatpak run` still starting
    fact(345599, 1, 21200030, "Xwayland", with_token()),  // the compositor's, with its lineage lost
    fact(351010, 351000, 21213010, "bwrap", ""s),
  };
  fsi::instances_t instances;
  fsi::instance_t heroic;
  heroic.id = "1424202439";
  heroic.app_id = "com.heroicgameslauncher.hgl";
  heroic.bwrap = {346422, 21210000};
  heroic.init = {346430, 21210001};
  heroic.pid_namespace = 4026540001;
  heroic.cgroup = scope;
  instances.live.push_back(heroic);
  fsi::instance_t starting;
  starting.id = "777777777";
  starting.app_id = "net.lutris.Lutris";
  starting.bwrap = {351010, 21213010};
  instances.unresolved.push_back(starting);
  const auto session = pas::classify(facts, supervisor, token, instances, 1000);
  EXPECT_EQ(pids_of(session.compositor), (std::set<pid_t> {345544, 345580, 345590}));
  EXPECT_EQ(pids_of(session.launch_chain), (std::set<pid_t> {346000, 346100, 346200, 351000}))
    << "signalling the shells that ran `flatpak run` would bring the sandbox down unordered";
  EXPECT_EQ(pids_of(session.session), (std::set<pid_t> {350000, 350010, 350020}));
}

TEST(PrivateAppStopTests, WindowOwnerTestNeverTakesALaunchersPid) {
  // Alan Wake 2 is 361425 on the host and 631 in Heroic's namespace; Heroic's own process is 13.
  const auto test = pas::window_owner_test({{361425, 350010}, {631}, {13, 2}});
  EXPECT_TRUE(test.host_pid(361425));
  EXPECT_TRUE(test.host_pid(350010));
  EXPECT_FALSE(test.host_pid(346819));
  EXPECT_FALSE(test.host_pid(631)) << "X-Resource gives a host pid, never a sandbox one";
  EXPECT_TRUE(test.net_wm_pid(631));
  EXPECT_TRUE(test.net_wm_pid(350010));
  EXPECT_FALSE(test.net_wm_pid(13));
  // A value both the game and Heroic have in their namespaces is never taken.
  const auto ambiguous = pas::window_owner_test({{361425}, {13}, {13}});
  EXPECT_FALSE(ambiguous.net_wm_pid(13));
}

TEST(PrivateAppStopTests, AnAppWithNoWindowToAskGetsItsExitTimeoutAfterSigterm) {
  // An emulator drawing with Wayland has no X11 window to ask, so SIGTERM is its only notice. It
  // gets its exit timeout when that is longer than SIGTERM's 10 s, as the sweep always gave it.
  fake_session_t session;
  session.windows = 0;
  session.exits_after_sigterm = 18s;
  pas::plan_t plan;
  plan.exit_timeout = 20s;
  plan.has_app = true;
  const auto outcome = pas::run(plan, session.actions());
  EXPECT_EQ(outcome.path, "sigterm");
  ASSERT_EQ(session.waits["wait_app"].size(), 2u);
  EXPECT_EQ(session.waits["wait_app"][1], 20000ms);
  EXPECT_EQ(session.count_of("sigkill_app"), 0u);

  // One asked first had its exit timeout to close, and SIGTERM after it keeps its 5 s.
  fake_session_t asked;
  asked.closes_after.reset();
  asked.exits_after_sigterm = 1s;
  const auto after_close = pas::run(plan, asked.actions());
  EXPECT_EQ(after_close.path, "sigterm");
  EXPECT_EQ(asked.waits["wait_app"][1], 20000ms) << "the close wait";
  EXPECT_EQ(asked.waits["wait_app"][2], 5000ms) << "SIGTERM's grace after it";
}

TEST(PrivateAppStopTests, ACloseRequestWaitsForWhatItAskedNotForTheSessionsHelpers) {
  // A helper of the session's own that never exits on its own, as a user's autostart may run, does
  // not hold the close wait: the game closed when asked, and the helper gets a moment, then SIGTERM.
  fake_session_t session;
  session.helper = true;
  session.closes_after = 1200ms;
  pas::plan_t plan;
  plan.exit_timeout = 5s;
  plan.has_app = true;
  const auto outcome = pas::run(plan, session.actions());
  EXPECT_EQ(outcome.path, "close_request");
  EXPECT_TRUE(outcome.drained);
  EXPECT_EQ(
    session.events,
    (std::vector<std::string> {"wait_app", "ask", "wait_asked", "wait_app", "sigterm_app", "wait_app"})
  );
  EXPECT_EQ(session.waits["wait_asked"][0], 10000ms) << "the close request's wait";
  EXPECT_EQ(session.waits["wait_app"][1], 1000ms) << "a moment for the rest to exit on its own";
  EXPECT_EQ(session.waits["wait_app"][2], 5000ms) << "SIGTERM's grace after a close";
  EXPECT_EQ(outcome.waited, 2215ms) << "not the whole close wait";
  EXPECT_EQ(session.count_of("sigkill_app"), 0u);
}

TEST(PrivateAppStopTests, ClassifyLeavesWhatTheCompositorRunsForItselfToTheCompositor) {
  // labwc's startup client when the app is Polaris's own child, and the background the generated
  // autostart paints, carry the token and never exit on their own: they are the compositor's. A
  // session's own `sleep 30`, and an app of Polaris's own that is `sleep infinity`, are not.
  const fsi::process_t supervisor {345544, 21200000};
  std::vector<pas::process_facts_t> facts {
    fact(982284, 1, 1000, "systemd", "HOME=/var/empty\0"s),
    fact(3231794, 982284, 21100000, "polaris-kms", "HOME=/var/empty\0"s),
    fact(345544, 3231794, 21200000, "polaris-kms", with_token()),
    fact(345580, 345544, 21200010, "labwc", with_token()),
    fact(345590, 345580, 21200020, "Xwayland", with_token()),
    fact(345600, 345544, 21200500, "sleep", with_token()),
    fact(345610, 345544, 21200600, "swaybg", with_token()),
    fact(345620, 345544, 21200700, "sleep", with_token()),
    fact(700100, 3231794, 21300100, "sleep", with_token()),
    fact(700110, 700100, 21300110, "game", with_token()),
  };
  facts[5].cmdline = "sleep\0infinity\0"s;
  facts[7].cmdline = "sleep\0" "30\0"s;
  facts[8].cmdline = "sleep\0infinity\0"s;
  const auto session = pas::classify(facts, supervisor, token, {}, 1000);
  EXPECT_EQ(pids_of(session.compositor), (std::set<pid_t> {345544, 345580, 345590, 345600, 345610}));
  EXPECT_EQ(pids_of(session.session), (std::set<pid_t> {345620, 700100, 700110}));
}

TEST(PrivateAppStopTests, WindowOwnerTestNotesTheHostPidsItAsked) {
  auto asked = std::make_shared<pas::asked_owners_t>();
  const auto test = pas::window_owner_test({{361425, 350010}, {631}, {13}}, asked);
  EXPECT_TRUE(test.host_pid(350010));
  EXPECT_FALSE(test.host_pid(346819));
  EXPECT_TRUE(test.net_wm_pid(361425));
  EXPECT_TRUE(test.net_wm_pid(631)) << "the game's, inside its sandbox, which the close wait waits for anyway";
  EXPECT_FALSE(test.net_wm_pid(13));
  EXPECT_EQ(asked->host, (std::set<pid_t> {350010, 361425}));
}

#endif
