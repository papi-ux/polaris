/**
 * @file tests/unit/test_prelaunch_transition.cpp
 * @brief The #204 leave-Game-Mode coordinator over a scripted host, adapter and clock.
 *
 * Nothing here touches the machine: the host, the session-switch adapter and the clock are stand-ins,
 * and the process probes read scratch directories laid out like /proc.
 */
#include <src/prelaunch_transition.h>

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace std::literals;
using prelaunch::direction_e;
using prelaunch::restore_e;
using prelaunch::session_identity_t;
using prelaunch::state_e;
using prelaunch::step_e;

namespace {
  const session_identity_t game_mode_leader {100, 5000, "session-1.scope"};
  const session_identity_t desktop_leader {200, 9000, "session-2.scope"};

  /// A host in Game Mode that leaves it when the adapter's switch completes.
  class scripted_host_t final: public prelaunch::host_t {
  public:
    bool live = true;
    bool title = false;
    bool independent = true;
    std::optional<session_identity_t> game_mode = game_mode_leader;
    std::optional<session_identity_t> desktop;
    int scans = 0;
    int probes = 0;
    std::function<void()> on_scan;
    std::vector<std::string> *log = nullptr;

    bool game_mode_live_fresh() override {
      ++scans;
      if (log) log->push_back("scan");
      if (on_scan) on_scan();
      return live;
    }

    std::optional<session_identity_t> game_mode_session() override {
      ++probes;
      return game_mode;
    }

    std::optional<session_identity_t> desktop_session() override {
      ++probes;
      return desktop;
    }

    bool local_title_running() override {
      ++probes;
      return title;
    }

    bool service_independent(std::string &reason) override {
      ++probes;
      if (!independent) reason = "stand-in says no";
      return independent;
    }
  };

  /// An adapter whose switch completes after `polls_to_finish` polls, or never, or fails.
  class scripted_adapter_t final: public prelaunch::adapter_t {
  public:
    scripted_host_t *host = nullptr;
    bool available = true;
    bool start_ok = true;
    int polls_to_finish = 2;
    bool fails = false;
    bool hangs = false;
    std::vector<direction_e> starts;
    int aborts = 0;
    int polls = 0;
    std::vector<std::string> *log = nullptr;

    bool implemented() const override {
      return available;
    }

    std::string unavailable_reason() const override {
      return "stand-in unavailable";
    }

    bool present = true;

    bool command_present() const override {
      return present;
    }

    bool start(direction_e direction) override {
      starts.push_back(direction);
      if (log) log->push_back(direction == direction_e::to_desktop ? "start_to_desktop" : "start_to_game_mode");
      remaining_ = polls_to_finish;
      direction_ = direction;
      return start_ok;
    }

    step_e poll() override {
      ++polls;
      if (fails) return step_e::failed;
      if (hangs || --remaining_ > 0) return step_e::running;
      if (host) {
        if (direction_ == direction_e::to_desktop) {
          host->live = false;
          host->game_mode.reset();
          host->desktop = desktop_leader;
        } else {
          host->live = true;
          host->desktop.reset();
          host->game_mode = session_identity_t {300, 12000, "session-3.scope"};
        }
      }
      return step_e::succeeded;
    }

    void abort() override {
      ++aborts;
    }

  private:
    int remaining_ = 0;
    direction_e direction_ = direction_e::to_desktop;
  };

  class fake_clock_t final: public prelaunch::clock_t {
  public:
    std::chrono::steady_clock::time_point at {};
    std::chrono::milliseconds slept {0};

    std::chrono::steady_clock::time_point now() override {
      return at;
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      at += duration;
      slept += duration;
    }
  };

  prelaunch::request_t launch_request(std::string client = "client-a") {
    prelaunch::request_t request;
    request.entry = prelaunch::entry_e::rtsp;
    request.client_uuid = std::move(client);
    request.lifecycle_generation = 7;
    request.policy_enabled = true;
    request.capture_ready = [](std::string &) {
      return true;
    };
    request.others_now = [](std::string &) {
      return false;
    };
    return request;
  }

  const prelaunch::coordinator_t::others_t nobody_else = [](std::string &) {
    return false;
  };

  struct fixture_t {
    scripted_host_t host;
    scripted_adapter_t adapter;
    fake_clock_t clock;
    prelaunch::coordinator_t coordinator {host, adapter, clock};

    fixture_t() {
      adapter.host = &host;
    }
  };
}  // namespace

TEST(PrelaunchTransitionTests, AnAppWithTheSettingOffNeverScansProbesOrSwitches) {
  fixture_t f;
  auto request = launch_request();
  request.policy_enabled = false;
  const auto outcome = f.coordinator.run(request);
  EXPECT_TRUE(outcome.proceed());
  EXPECT_EQ(outcome.state, state_e::not_needed);
  EXPECT_EQ(f.host.scans, 0);
  EXPECT_EQ(f.host.probes, 0);
  EXPECT_TRUE(f.adapter.starts.empty());
  EXPECT_EQ(f.coordinator.receipt().transition_generation, 0u) << "no receipt for a launch the setting does not cover";
}

TEST(PrelaunchTransitionTests, ResumeWatchAndInputOnlyBindTheirGenerationAndNeverSwitch) {
  for (int path = 0; path < 3; ++path) {
    fixture_t f;
    auto request = launch_request();
    request.resume = path == 0;
    request.watch_only = path == 1;
    request.input_only = path == 2;
    const auto outcome = f.coordinator.run(request);
    EXPECT_TRUE(outcome.proceed()) << path;
    EXPECT_EQ(f.host.scans, 0) << path;
    EXPECT_TRUE(f.adapter.starts.empty()) << path;
  }
}

TEST(PrelaunchTransitionTests, TheProductionAdapterIsRefusedBeforeAnyEffectEvenWithTheSettingOn) {
  scripted_host_t host;
  // A fixed path that cannot exist: discovery finds nothing, and nothing is ever run.
  prelaunch::unavailable_adapter_t adapter {"/nonexistent/polaris-test/steamosctl"};
  fake_clock_t clock;
  prelaunch::coordinator_t coordinator {host, adapter, clock};
  EXPECT_FALSE(adapter.implemented());
  EXPECT_FALSE(adapter.command_present());
  EXPECT_FALSE(adapter.start(direction_e::to_desktop));
  const auto outcome = coordinator.run(launch_request());
  EXPECT_EQ(outcome.status, 503);
  EXPECT_EQ(outcome.code, "prelaunch_transition_unavailable");
  EXPECT_EQ(outcome.state, state_e::refused);
  EXPECT_FALSE(outcome.changed_session);
  EXPECT_EQ(clock.slept, 0ms) << "no wait, no switch";
  EXPECT_EQ(host.probes, 0) << "refused before the title/service/identity probes";

  const auto off = coordinator.readiness(false);
  EXPECT_FALSE(off.enabled);
  EXPECT_FALSE(off.ready);
  const auto on = coordinator.readiness(true);
  EXPECT_TRUE(on.enabled) << "the setting is shown as on";
  EXPECT_FALSE(on.adapter_implemented) << "and distinguished from an adapter this build does not have";
  EXPECT_FALSE(on.ready);
  EXPECT_EQ(on.reason, adapter.unavailable_reason());
}

TEST(PrelaunchTransitionTests, AHostNotInGameModeNeedsNothing) {
  fixture_t f;
  f.host.live = false;
  const auto outcome = f.coordinator.run(launch_request());
  EXPECT_TRUE(outcome.proceed());
  EXPECT_EQ(outcome.state, state_e::not_needed);
  EXPECT_EQ(f.host.scans, 1) << "one fresh scan, never a kept answer";
  EXPECT_TRUE(f.adapter.starts.empty());
}

TEST(PrelaunchTransitionTests, ProfileSpaceAndOccupiedHostsAreRefusedBeforeTheScan) {
  struct case_t {
    std::function<void(prelaunch::request_t &)> set;
    std::string code;
  };
  const std::vector<case_t> cases {
    {[](auto &r) { r.profile_or_space = true; }, "prelaunch_transition_profile_unsupported"},
    {[](auto &r) { r.occupancy.app_running = true; }, "prelaunch_host_occupied"},
    {[](auto &r) { r.occupancy.streams_active = true; }, "prelaunch_host_occupied"},
    {[](auto &r) { r.occupancy.launch_pending = true; }, "prelaunch_host_occupied"},
    {[](auto &r) { r.occupancy.browser_capture = true; }, "prelaunch_host_occupied"},
    {[](auto &r) { r.occupancy.profile_service = true; }, "prelaunch_host_occupied"},
  };
  for (const auto &c : cases) {
    fixture_t f;
    auto request = launch_request();
    c.set(request);
    const auto outcome = f.coordinator.run(request);
    EXPECT_EQ(outcome.status, 409) << c.code;
    EXPECT_EQ(outcome.code, c.code);
    EXPECT_EQ(f.host.scans, 0) << c.code;
    EXPECT_TRUE(f.adapter.starts.empty()) << c.code;
  }
}

TEST(PrelaunchTransitionTests, AMirrorRouteIsRefusedRatherThanLeavingGameModeForNothing) {
  fixture_t f;
  auto request = launch_request();
  request.private_route = false;
  const auto outcome = f.coordinator.run(request);
  EXPECT_EQ(outcome.code, "prelaunch_private_route_required");
  EXPECT_TRUE(f.adapter.starts.empty());
  f.host.live = false;
  EXPECT_TRUE(f.coordinator.run(request).proceed()) << "off Game Mode the launch goes on as it always did";
}

TEST(PrelaunchTransitionTests, AnOpenTitleMissingServiceIndependenceOrUnidentifiedSessionRefusesWithoutSwitching) {
  for (int which = 0; which < 3; ++which) {
    fixture_t f;
    f.host.title = which == 0;
    f.host.independent = which != 1;
    if (which == 2) f.host.game_mode.reset();
    const auto outcome = f.coordinator.run(launch_request());
    EXPECT_FALSE(outcome.proceed()) << which;
    EXPECT_EQ(outcome.code, which == 0 ? "prelaunch_local_title_running" :
                            which == 1 ? "prelaunch_service_not_independent" :
                                         "prelaunch_session_identity_unavailable");
    EXPECT_TRUE(f.adapter.starts.empty()) << which;
  }
}

TEST(PrelaunchTransitionTests, AnAdmittedTransitionWaitsForTheOldSessionTheDesktopAndCaptureThenOwnsTheChange) {
  fixture_t f;
  int capture_checks = 0;
  auto request = launch_request();
  request.capture_ready = [&](std::string &reason) {
    // Ready only on the second check: the wait is real, and bounded.
    reason = "capture not yet";
    return ++capture_checks >= 2;
  };
  const auto outcome = f.coordinator.run(request);
  ASSERT_TRUE(outcome.proceed()) << outcome.code;
  EXPECT_EQ(outcome.state, state_e::ready);
  EXPECT_TRUE(outcome.changed_session);
  ASSERT_EQ(f.adapter.starts.size(), 1u);
  EXPECT_EQ(f.adapter.starts.front(), direction_e::to_desktop);
  EXPECT_EQ(capture_checks, 2);
  const auto receipt = f.coordinator.receipt();
  EXPECT_EQ(receipt.state, state_e::ready);
  EXPECT_EQ(receipt.restore, restore_e::owned);
  EXPECT_EQ(receipt.pre, game_mode_leader);
  EXPECT_EQ(receipt.post, desktop_leader);
  EXPECT_EQ(receipt.lifecycle_generation, 7u);
  EXPECT_FALSE(receipt.committed);
  EXPECT_LT(f.clock.slept, prelaunch::k_max_deadline);
}

TEST(PrelaunchTransitionTests, TheTransitionStagesAreNotedAroundTheSwitch) {
  fixture_t f;
  std::vector<std::string> stages;
  prelaunch::set_stage_sink_for_tests([&](std::string_view stage) {
    stages.emplace_back(stage);
  });
  ASSERT_TRUE(f.coordinator.run(launch_request()).proceed());
  prelaunch::set_stage_sink_for_tests({});
  EXPECT_EQ(stages, (std::vector<std::string> {"transition_start", "transition_ready"}));
}

TEST(PrelaunchTransitionTests, AHungSwitchIsAbortedAtTheDeadlineAndNothingUnprovenIsUndone) {
  fixture_t f;
  f.adapter.hangs = true;
  const auto outcome = f.coordinator.run(launch_request());
  EXPECT_EQ(outcome.status, 504);
  EXPECT_EQ(outcome.code, "prelaunch_switch_timeout");
  EXPECT_EQ(outcome.state, state_e::failed);
  EXPECT_EQ(f.adapter.aborts, 1);
  EXPECT_GE(f.clock.slept, prelaunch::k_max_deadline - prelaunch::k_poll_period);
  EXPECT_LE(f.clock.slept, prelaunch::k_max_deadline + prelaunch::k_poll_period);
  ASSERT_EQ(f.adapter.starts.size(), 1u) << "no restore without a desktop identity this launch owns";
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::refused);
}

TEST(PrelaunchTransitionTests, AFailedSwitchIsRefusedWithItsOwnCode) {
  fixture_t f;
  f.adapter.fails = true;
  const auto outcome = f.coordinator.run(launch_request());
  EXPECT_EQ(outcome.code, "prelaunch_switch_failed");
  EXPECT_EQ(outcome.state, state_e::failed);
  EXPECT_EQ(f.adapter.starts.size(), 1u);
}

TEST(PrelaunchTransitionTests, CaptureThatNeverBecomesReadyRollsBackOnlyTheDesktopThisLaunchOpened) {
  fixture_t f;
  auto request = launch_request();
  request.capture_ready = [](std::string &reason) {
    reason = "the private route is not ready";
    return false;
  };
  const auto outcome = f.coordinator.run(request);
  EXPECT_EQ(outcome.code, "prelaunch_capture_timeout");
  EXPECT_NE(outcome.message.find("the private route is not ready"), std::string::npos);
  ASSERT_EQ(f.adapter.starts.size(), 2u);
  EXPECT_EQ(f.adapter.starts.back(), direction_e::to_game_mode);
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::rolled_back);
}

TEST(PrelaunchTransitionTests, OnlyTheRequestingClientCanCancelItsTransition) {
  fixture_t f;
  bool tried = false;
  f.host.on_scan = [&]() {
    // While the coordinator waits for the desktop (no lock held), other clients and the owner cancel.
    if (f.host.live || tried) return;
    tried = true;
    EXPECT_FALSE(f.coordinator.cancel_for_client("client-b")) << "another client changes nothing";
    EXPECT_FALSE(f.coordinator.cancel_for_client("")) << "an empty identity is nobody";
    EXPECT_TRUE(f.coordinator.cancel_for_client("CLIENT-A")) << "the owner, whatever the case of its uuid";
  };
  // Capture never becomes ready, so the run is still waiting when the owner cancels.
  auto request = launch_request("client-a");
  request.capture_ready = [](std::string &) {
    return false;
  };
  const auto outcome = f.coordinator.run(request);
  EXPECT_TRUE(tried);
  EXPECT_EQ(outcome.code, "prelaunch_cancelled");
  EXPECT_EQ(outcome.state, state_e::cancelled);
  EXPECT_LT(f.clock.slept, prelaunch::k_max_deadline) << "cancelled promptly, not at the deadline";
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::rolled_back) << "the cancelled generation undoes its own desktop";
  EXPECT_FALSE(f.coordinator.cancel_for_client("client-a")) << "nothing is left to cancel";
}

TEST(PrelaunchTransitionTests, ASecondLaunchDuringATransitionIsRefusedAndHoldsNoLockAcrossTheWait) {
  fixture_t f;
  std::optional<prelaunch::outcome_t> nested;
  f.host.on_scan = [&]() {
    if (!nested && !f.host.live) nested = f.coordinator.run(launch_request("client-b"));
  };
  const auto outcome = f.coordinator.run(launch_request("client-a"));
  ASSERT_TRUE(nested.has_value()) << "the nested request ran while the first one waited";
  EXPECT_EQ(nested->code, "prelaunch_transition_busy");
  EXPECT_TRUE(outcome.proceed());
  EXPECT_EQ(f.adapter.starts.size(), 1u) << "exactly one transition";
}

TEST(PrelaunchTransitionTests, OnlyAPurposefulEndOfTheCommittedGenerationRestoresAndOnlyItsOwnDesktop) {
  struct case_t {
    std::string name;
    bool ended_on_request;
    bool daemon_shutdown;
    bool other_sessions;
    std::function<void(scripted_host_t &)> change;
    restore_e expected;
  };
  const std::vector<case_t> cases {
    {"purposeful end", true, false, false, {}, restore_e::restored},
    {"transient disconnect", false, false, false, {}, restore_e::refused},
    {"daemon shutdown", true, true, false, {}, restore_e::refused},
    {"another session", true, false, true, {}, restore_e::refused},
    {"a game opened on the host since", true, false, false, [](auto &h) { h.title = true; }, restore_e::refused},
    {"user went back to Game Mode", true, false, false, [](auto &h) { h.live = true; }, restore_e::refused},
    {"a different desktop session", true, false, false, [](auto &h) { h.desktop = session_identity_t {201, 9100, "session-4.scope"}; }, restore_e::refused},
    {"a restarted desktop leader with a reused pid", true, false, false, [](auto &h) { h.desktop = session_identity_t {200, 9999, "session-2.scope"}; }, restore_e::refused},
  };
  for (const auto &c : cases) {
    fixture_t f;
    const auto outcome = f.coordinator.run(launch_request());
    ASSERT_TRUE(outcome.proceed()) << c.name;
    f.coordinator.commit(outcome.transition_generation);
    ASSERT_TRUE(f.coordinator.receipt().committed) << c.name;
    if (c.change) c.change(f.host);
    const prelaunch::coordinator_t::others_t others = [&](std::string &reason) {
      if (c.other_sessions) reason = "a stream is active";
      return c.other_sessions;
    };
    const auto generation = outcome.transition_generation;
    EXPECT_EQ(f.coordinator.on_session_end(generation, c.ended_on_request, c.daemon_shutdown, others), c.expected) << c.name;
    EXPECT_EQ(f.adapter.starts.size(), c.expected == restore_e::restored ? 2u : 1u) << c.name;
    EXPECT_EQ(f.coordinator.on_session_end(generation, true, false, nobody_else), restore_e::none) << c.name << ": a receipt restores at most once";
  }
}

TEST(PrelaunchTransitionTests, AnUncommittedOrStaleGenerationIsNeverRestoredBySessionEnd) {
  fixture_t f;
  const auto first = f.coordinator.run(launch_request());
  ASSERT_TRUE(first.proceed());
  EXPECT_EQ(f.coordinator.on_session_end(first.transition_generation, true, false, nobody_else), restore_e::none) << "never committed";
  f.coordinator.commit(first.transition_generation + 1);
  EXPECT_FALSE(f.coordinator.receipt().committed) << "a commit for another generation is ignored";
  EXPECT_EQ(f.coordinator.rollback(first.transition_generation + 1, nobody_else), restore_e::none);
  f.coordinator.commit(first.transition_generation);
  EXPECT_EQ(f.coordinator.on_session_end(first.transition_generation - 1, true, false, nobody_else), restore_e::none)
    << "an older generation's teardown never restores the newer committed change";
  EXPECT_EQ(f.coordinator.on_session_end(0, true, false, nobody_else), restore_e::none) << "a session that committed nothing";
  EXPECT_EQ(f.adapter.starts.size(), 1u);
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::owned) << "still owned by its own session";
}

TEST(PrelaunchTransitionTests, ARestoreWithNoWayToCheckTheHostsOtherSessionsIsRefused) {
  fixture_t f;
  const auto outcome = f.coordinator.run(launch_request());
  ASSERT_TRUE(outcome.proceed());
  f.coordinator.commit(outcome.transition_generation);
  EXPECT_EQ(f.coordinator.on_session_end(outcome.transition_generation, true, false, {}), restore_e::refused);
  EXPECT_EQ(f.adapter.starts.size(), 1u);
}

TEST(PrelaunchTransitionTests, AnOwnedChangeCommittedOrNotBlocksAnotherTransitionUntilItSettles) {
  for (const bool commit : {false, true}) {
    fixture_t f;
    const auto first = f.coordinator.run(launch_request());
    ASSERT_TRUE(first.proceed());
    if (commit) f.coordinator.commit(first.transition_generation);
    f.host.live = true;  // even if Game Mode came back meanwhile
    const auto second = f.coordinator.run(launch_request("client-b"));
    EXPECT_EQ(second.code, "prelaunch_transition_owned") << commit;
    EXPECT_EQ(f.adapter.starts.size(), 1u) << commit;
    EXPECT_EQ(f.coordinator.receipt().transition_generation, first.transition_generation) << "the owned receipt is not overwritten";
    // Settled: rolled back before commit, or the committed session ended on request.
    f.host.live = false;
    const auto settled = commit ? f.coordinator.on_session_end(first.transition_generation, true, false, nobody_else) :
                                  f.coordinator.rollback(first.transition_generation, nobody_else);
    EXPECT_EQ(settled, commit ? restore_e::restored : restore_e::rolled_back);
    EXPECT_TRUE(f.coordinator.run(launch_request("client-b")).proceed()) << "a settled change no longer blocks";
  }
}

TEST(PrelaunchTransitionTests, AnImplementedAdapterWithoutItsInstalledCommandIsNeverStarted) {
  fixture_t f;
  f.adapter.present = false;
  const auto outcome = f.coordinator.run(launch_request());
  EXPECT_EQ(outcome.code, "prelaunch_command_unavailable");
  EXPECT_TRUE(f.adapter.starts.empty());
  const auto readiness = f.coordinator.readiness(true);
  EXPECT_TRUE(readiness.adapter_implemented);
  EXPECT_FALSE(readiness.command_present);
  EXPECT_FALSE(readiness.ready);
}

TEST(PrelaunchTransitionTests, ADesktopThatChangesWhileCaptureWaitsIsNeverPublishedAsOwned) {
  fixture_t f;
  int checks = 0;
  auto request = launch_request();
  request.capture_ready = [&](std::string &) {
    // The user's session changes after the first capture check.
    if (++checks == 1) f.host.desktop = session_identity_t {250, 9500, "session-5.scope"};
    return false;
  };
  const auto outcome = f.coordinator.run(request);
  EXPECT_EQ(outcome.code, "prelaunch_desktop_changed");
  EXPECT_EQ(outcome.state, state_e::failed);
  const auto receipt = f.coordinator.receipt();
  EXPECT_NE(receipt.restore, restore_e::owned);
  EXPECT_EQ(receipt.restore, restore_e::refused) << "the new desktop is not this launch's to undo";
  EXPECT_EQ(f.adapter.starts.size(), 1u);
}

TEST(PrelaunchTransitionTests, ALaunchScopeRollsBackAnUncommittedReadyTransitionOnce) {
  scripted_host_t host;
  scripted_adapter_t adapter;
  adapter.host = &host;
  fake_clock_t clock;
  prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
  {
    const auto outcome = prelaunch::coordinator().run(launch_request());
    ASSERT_TRUE(outcome.proceed());
    prelaunch::launch_scope_t scope {outcome};
    scope.set_others(nobody_else);
    // The launch fails after the transition: the scope leaves without commit.
  }
  EXPECT_EQ(prelaunch::coordinator().receipt().restore, restore_e::rolled_back);
  ASSERT_EQ(adapter.starts.size(), 2u);
  {
    const auto outcome = prelaunch::coordinator().run(launch_request());
    ASSERT_TRUE(outcome.proceed());
    prelaunch::launch_scope_t scope {outcome};
    EXPECT_EQ(scope.commit(), outcome.transition_generation) << "the generation the session must name at its end";
    EXPECT_EQ(scope.commit(), 0u) << "committed once";
  }
  EXPECT_EQ(prelaunch::coordinator().receipt().restore, restore_e::owned) << "a committed launch keeps its change";
  EXPECT_TRUE(prelaunch::coordinator().receipt().committed);
  prelaunch::set_dependencies_for_tests(nullptr, nullptr, nullptr);
}

TEST(PrelaunchTransitionTests, StagesReachTheTestSinkInOrderAndProductionHasNone) {
  std::vector<std::string> seen;
  prelaunch::note_stage("before");
  prelaunch::set_stage_sink_for_tests([&](std::string_view stage) {
    seen.emplace_back(stage);
  });
  prelaunch::note_stage("a");
  prelaunch::note_stage("b");
  prelaunch::set_stage_sink_for_tests({});
  prelaunch::note_stage("after");
  EXPECT_EQ(seen, (std::vector<std::string> {"a", "b"}));
}

namespace {
  struct scratch_proc_t {
    std::filesystem::path root;

    scratch_proc_t() {
      std::random_device device;
      root = std::filesystem::temp_directory_path() / ("polaris-prelaunch-proc-" + std::to_string(device()));
      std::filesystem::create_directories(root);
    }

    ~scratch_proc_t() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }

    void add(int pid, unsigned uid, std::vector<std::string> argv, std::uint64_t start, std::string cgroup, std::string comm = "x) (y") {
      const auto dir = root / std::to_string(pid);
      std::filesystem::create_directories(dir);
      std::ofstream {dir / "status"} << "Name:\tx\nUid:\t" << uid << "\t" << uid << "\t" << uid << "\t" << uid << "\n";
      std::string cmdline;
      for (const auto &arg : argv) cmdline += arg + '\0';
      std::ofstream {dir / "cmdline", std::ios::binary} << cmdline;
      std::ofstream stat {dir / "stat"};
      stat << pid << " (" << comm << ") S 1";
      for (int field = 5; field <= 21; ++field) stat << " 0";
      stat << ' ' << start << " 0 0\n";
      std::ofstream {dir / "cgroup"} << cgroup;
    }
  };
}  // namespace

TEST(PrelaunchTransitionTests, SessionIdentityIsTheLeadersPidStartTimeAndLogindScope) {
  scratch_proc_t proc;
  proc.add(400, 1000, {"/usr/bin/gamescope-session-plus", "steam"}, 777,
           "0::/user.slice/user-1000.slice/session-3.scope\n");
  proc.add(401, 1000, {"/usr/bin/gamescope-session-plus"}, 900, "0::/user.slice/user-1000.slice/session-3.scope\n");
  proc.add(402, 1001, {"/usr/bin/gamescope-session-plus"}, 10, "0::/user.slice/user-1001.slice/session-9.scope\n");
  proc.add(403, 1000, {"/usr/bin/startplasma-wayland"}, 5000, "0::/user.slice/user-1000.slice/session-4.scope\n");
  const auto identity = prelaunch::process_identity(proc.root, 400);
  ASSERT_TRUE(identity);
  EXPECT_EQ(identity->leader_pid, 400);
  EXPECT_EQ(identity->leader_start_ticks, 777u) << "start time read past a comm holding ') ('";
  EXPECT_EQ(identity->logind_scope, "session-3.scope");
  EXPECT_FALSE(prelaunch::find_session_leader(proc.root, 1000, {"gamescope-session-plus"}))
    << "two leaders for this account are ambiguous; neither is picked";
  EXPECT_FALSE(prelaunch::find_session_leader(proc.root, 1000, {"gamescope-session"})) << "basename match, not substring";
  EXPECT_FALSE(prelaunch::process_identity(proc.root, 999));
  const auto desktop = prelaunch::find_session_leader(proc.root, 1000, {"startplasma-wayland"});
  ASSERT_TRUE(desktop) << "one leader, another account's never counted";
  EXPECT_EQ(desktop->leader_pid, 403);
  EXPECT_EQ(desktop->logind_scope, "session-4.scope");
  std::filesystem::remove_all(proc.root / "401");
  ASSERT_TRUE(prelaunch::find_session_leader(proc.root, 1000, {"gamescope-session-plus"}));
  EXPECT_EQ(prelaunch::find_session_leader(proc.root, 1000, {"gamescope-session-plus"})->leader_pid, 400);
}

TEST(PrelaunchTransitionTests, ALeaderOutsideALogindSessionOrWithAnOutOfRangeUidIsNoSession) {
  scratch_proc_t proc;
  proc.add(600, 1000, {"/usr/bin/startplasma-wayland"}, 50, "0::/user.slice/user-1000.slice/user@1000.service/app.slice/x.service\n");
  EXPECT_FALSE(prelaunch::find_session_leader(proc.root, 1000, {"startplasma-wayland"})) << "no logind scope: not a graphical session";
  scratch_proc_t overflow;
  overflow.add(700, 1000, {"/usr/bin/startplasma-wayland"}, 50, "0::/user.slice/user-1000.slice/session-2.scope\n");
  // 2^32 + 1000 must not wrap to uid 1000.
  std::ofstream {overflow.root / "700" / "status"} << "Name:\tx\nUid:\t4294968296\t1000\t1000\t1000\n";
  EXPECT_FALSE(prelaunch::find_session_leader(overflow.root, 1000, {"startplasma-wayland"}));
  EXPECT_FALSE(prelaunch::steam_title_running(overflow.root, 1000));
}

TEST(PrelaunchTransitionTests, AnOpenSteamTitleIsSteamsOwnReaperForThisAccount) {
  scratch_proc_t proc;
  proc.add(500, 1000, {"/bin/sh", "-c", "echo SteamLaunch AppId=1"}, 1, "");
  EXPECT_FALSE(prelaunch::steam_title_running(proc.root, 1000)) << "a shell that mentions the words is not a title";
  proc.add(501, 1001, {"/fixture/other-account/steam/reaper", "SteamLaunch", "AppId=620", "--", "game"}, 1, "");
  EXPECT_FALSE(prelaunch::steam_title_running(proc.root, 1000)) << "another account's title";
  proc.add(502, 1000, {"/fixture/own-account/steam/reaper", "SteamLaunch", "AppId=620", "--", "game"}, 1, "");
  EXPECT_TRUE(prelaunch::steam_title_running(proc.root, 1000));
}

TEST(PrelaunchTransitionTests, OnlyThePolarisUserServiceOutsideALoginScopeIsIndependent) {
  EXPECT_TRUE(prelaunch::cgroup_in_independent_service(
    "0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n"));
  EXPECT_FALSE(prelaunch::cgroup_in_independent_service(
    "0::/user.slice/user-1000.slice/session-2.scope\n")) << "started from a terminal in the session";
  EXPECT_FALSE(prelaunch::cgroup_in_independent_service(
    "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-polaris@abc.service\n")) << "a desktop launcher's transient unit";
  EXPECT_FALSE(prelaunch::cgroup_in_independent_service("0::/system.slice/polaris.service\n")) << "not the account's own user manager";
  EXPECT_FALSE(prelaunch::cgroup_in_independent_service(""));
}
