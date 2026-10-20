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
#include <future>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std::literals;
using prelaunch::direction_e;
using prelaunch::restore_e;
using prelaunch::session_identity_t;
using prelaunch::state_e;
using prelaunch::step_e;
namespace steamos = prelaunch::steamos;

namespace {
  session_identity_t identity(std::string id, prelaunch::mode_e mode, char generation, std::uint32_t leader, std::uint64_t ticks) {
    session_identity_t out;
    out.boot_id = "6c1b5f3e-2a4d-4c8e-9f10-0123456789ab";
    out.logind_id = std::move(id);
    out.logind_started = 1000 + ticks;
    out.uid = 1000;
    out.leader_pid = leader;
    out.leader_start_ticks = ticks;
    out.target_invocation = std::string(32, generation);
    out.mode = mode;
    return out;
  }

  const session_identity_t game_mode_leader = identity("2", prelaunch::mode_e::game, 'a', 100, 5000);
  const session_identity_t desktop_leader = identity("3", prelaunch::mode_e::desktop, 'b', 200, 9000);

  /// A host in Game Mode that leaves it when the adapter's switch completes. Its session records are
  /// what observe() reports: one bound session, or a read or bind refusal.
  class scripted_host_t final: public prelaunch::host_t {
  public:
    bool live = true;
    bool title = false;
    bool independent = true;
    std::optional<session_identity_t> session = game_mode_leader;
    prelaunch::verdict_t read_failure;
    prelaunch::verdict_t bind_failure;
    prelaunch::verdict_t service;
    int scans = 0;
    int probes = 0;
    std::function<void()> on_scan;
    std::vector<std::string> *log = nullptr;

    /// Opt in to the production observer's failed-read contract when its wait ends.
    std::function<std::chrono::steady_clock::time_point()> read_clock;
    int failed_reads = 0;
    int failed_read_scan = 0;
    prelaunch::verdict_t last_failed_read;
    std::optional<std::chrono::steady_clock::time_point> failed_read_deadline;

    /// Overrides the Game Mode unit's state; otherwise it follows `live`.
    std::optional<steamos::game_mode_e> mode_state;
    int service_proofs = 0;

    prelaunch::observation_t observe(const session_identity_t *since, const prelaunch::wait_t &wait, bool prove_service) override {
      ++scans;
      if (log) log->push_back("scan");
      if (on_scan) on_scan();
      prelaunch::observation_t out;
      out.read = read_failure;
      if (read_clock) {
        if (read_clock() >= wait.deadline) {
          out.read = prelaunch::verdict_t::fail("prelaunch_session_read_timeout", "The scripted session read exhausted its wait.");
        } else if (wait.cancelled && wait.cancelled()) {
          out.read = prelaunch::verdict_t::fail("prelaunch_cancelled", "The scripted session read was cancelled.");
        }
      }
      if (!out.read.ok()) {
        ++failed_reads;
        failed_read_scan = scans;
        last_failed_read = out.read;
        failed_read_deadline = wait.deadline;
        return out;
      }
      out.game_mode = mode_state.value_or(live ? steamos::game_mode_e::active : steamos::game_mode_e::inactive);
      if (prove_service) {
        ++service_proofs;
        out.service = service;
      }
      if (!bind_failure.ok()) {
        out.bind = bind_failure;
      } else if (session) {
        out.session = session;
      } else {
        out.bind = prelaunch::verdict_t::fail("prelaunch_session_identity_unavailable", "stand-in has no session");
      }
      // Retired: the session and its target generation are both gone.
      out.retired = since && (!session || (session->logind_id != since->logind_id && session->target_invocation != since->target_invocation));
      return out;
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
    bool present = true;
    bool supported = true;
    bool start_ok = true;
    int polls_to_finish = 2;
    bool fails = false;
    bool hangs = false;
    bool hangs_returning = false;
    std::vector<direction_e> starts;
    int aborts = 0;
    int polls = 0;
    int supports = 0;
    std::vector<std::string> *log = nullptr;

    bool implemented() const override {
      return available;
    }

    bool command_present() const override {
      return present;
    }

    prelaunch::verdict_t support(const prelaunch::wait_t &) override {
      ++supports;
      if (!available) return prelaunch::verdict_t::fail("prelaunch_transition_unavailable", "stand-in unavailable");
      if (!present) return prelaunch::verdict_t::fail("prelaunch_command_unavailable", "stand-in has no command");
      if (!supported) return prelaunch::verdict_t::fail("prelaunch_adapter_version_unaudited", "stand-in is unaudited");
      return prelaunch::verdict_t::pass();
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
      if (hangs || (hangs_returning && direction_ == direction_e::to_game_mode) || --remaining_ > 0) return step_e::running;
      if (host) {
        if (direction_ == direction_e::to_desktop) {
          host->live = false;
          host->session = desktop_leader;
        } else {
          host->live = true;
          host->session = identity("4", prelaunch::mode_e::game, 'c', 300, 12000);
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

  const prelaunch::budgets_t budgets {};

  struct fixture_t {
    scripted_host_t host;
    scripted_adapter_t adapter;
    fake_clock_t clock;
    prelaunch::coordinator_t coordinator {host, adapter, clock};

    fixture_t() {
      adapter.host = &host;
    }

    /// A session end, with its handed-over restore waited for.
    restore_e end_session(std::uint64_t generation, bool on_request, bool shutdown, const prelaunch::coordinator_t::others_t &others,
                          const prelaunch::admission_fence_t &fence = {}) {
      const auto handed = coordinator.on_session_end(generation, on_request, shutdown, others, fence);
      if (handed != restore_e::owned) return handed;
      coordinator.wait_for_restore();
      return coordinator.receipt().restore;
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
  EXPECT_EQ(adapter.support({}).code, "prelaunch_transition_unavailable");
  EXPECT_FALSE(adapter.start(direction_e::to_desktop));
  const auto outcome = coordinator.run(launch_request());
  EXPECT_EQ(outcome.status, 503);
  EXPECT_EQ(outcome.code, "prelaunch_transition_unavailable");
  EXPECT_EQ(outcome.state, state_e::refused);
  EXPECT_FALSE(outcome.changed_session);
  EXPECT_EQ(clock.slept, 0ms) << "no wait, no switch";
  EXPECT_EQ(host.probes, 0) << "refused before the title and service probes";
  EXPECT_EQ(host.scans, 1) << "one typed observation said Game Mode is running";

  const auto off = coordinator.readiness(false);
  EXPECT_FALSE(off.enabled);
  EXPECT_FALSE(off.ready);
  const auto on = coordinator.readiness(true);
  EXPECT_TRUE(on.enabled) << "the setting is shown as on";
  EXPECT_FALSE(on.adapter_implemented) << "and distinguished from an adapter this build does not have";
  EXPECT_FALSE(on.ready);
  EXPECT_EQ(on.code, "prelaunch_transition_unavailable");
  EXPECT_EQ(on.reason, adapter.support({}).reason);
}

TEST(PrelaunchTransitionTests, AHostNotInGameModeNeedsNothing) {
  for (const auto state : {steamos::game_mode_e::inactive, steamos::game_mode_e::absent}) {
    fixture_t f;
    f.host.mode_state = state;
    const auto outcome = f.coordinator.run(launch_request());
    EXPECT_TRUE(outcome.proceed());
    EXPECT_EQ(outcome.state, state_e::not_needed);
    EXPECT_EQ(f.host.scans, 1) << "one fresh typed observation, never a kept answer";
    EXPECT_TRUE(f.adapter.starts.empty());
  }
}

TEST(PrelaunchTransitionTests, AnUnreadHostIsRefusedNeverTreatedAsNotInGameMode) {
  fixture_t f;
  f.host.live = false;  // whatever any other detector would say
  f.host.read_failure = prelaunch::verdict_t::fail("prelaunch_session_identity_unavailable", "no user manager");
  const auto outcome = f.coordinator.run(launch_request());
  EXPECT_FALSE(outcome.proceed());
  EXPECT_EQ(outcome.code, "prelaunch_session_identity_unavailable");
  EXPECT_EQ(outcome.state, state_e::refused);
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

TEST(PrelaunchTransitionTests, AnOpenTitleMissingServiceIndependenceOrUnprovenSessionRefusesWithoutSwitching) {
  struct case_t {
    std::string name;
    std::function<void(fixture_t &)> change;
    int status;
    std::string code;
  };
  const std::vector<case_t> cases {
    {"a title open", [](auto &f) { f.host.title = true; }, 409, "prelaunch_local_title_running"},
    {"not the independent service", [](auto &f) { f.host.independent = false; }, 503, "prelaunch_service_not_independent"},
    {"the unit stops with the session", [](auto &f) {
       f.host.service = prelaunch::verdict_t::fail("prelaunch_service_not_independent", "PartOf graphical-session.target");
     }, 503, "prelaunch_service_not_independent"},
    {"no session identified", [](auto &f) { f.host.session.reset(); }, 503, "prelaunch_session_identity_unavailable"},
    {"two graphical sessions", [](auto &f) {
       f.host.bind_failure = prelaunch::verdict_t::fail("prelaunch_session_ambiguous", "two");
     }, 503, "prelaunch_session_ambiguous"},
    {"not an autologin session", [](auto &f) {
       f.host.bind_failure = prelaunch::verdict_t::fail("prelaunch_session_not_autologin", "sddm");
     }, 503, "prelaunch_session_not_autologin"},
    {"a stalled read", [](auto &f) {
       f.host.read_failure = prelaunch::verdict_t::fail("prelaunch_session_read_timeout", "stalled");
     }, 504, "prelaunch_session_read_timeout"},
    {"a malformed record", [](auto &f) {
       f.host.read_failure = prelaunch::verdict_t::fail("prelaunch_session_record_malformed", "Leader as s");
     }, 503, "prelaunch_session_record_malformed"},
    {"the detector and the records disagree", [](auto &f) { f.host.session = desktop_leader; }, 409, "prelaunch_session_mode_unknown"},
    {"an incomplete identity", [](auto &f) { f.host.session->target_invocation.clear(); }, 503, "prelaunch_session_identity_unavailable"},
    {"an unaudited manager", [](auto &f) { f.adapter.supported = false; }, 503, "prelaunch_adapter_version_unaudited"},
    {"Game Mode starting or stopping", [](auto &f) { f.host.mode_state = steamos::game_mode_e::transitional; }, 409, "prelaunch_session_mode_unknown"},
    {"a read that says nothing about Game Mode", [](auto &f) { f.host.mode_state = steamos::game_mode_e::unknown; }, 409, "prelaunch_session_mode_unknown"},
  };
  for (const auto &c : cases) {
    fixture_t f;
    c.change(f);
    const auto outcome = f.coordinator.run(launch_request());
    EXPECT_FALSE(outcome.proceed()) << c.name;
    EXPECT_EQ(outcome.status, c.status) << c.name;
    EXPECT_EQ(outcome.code, c.code) << c.name;
    EXPECT_EQ(outcome.state, state_e::refused) << c.name;
    EXPECT_FALSE(outcome.changed_session) << c.name;
    EXPECT_TRUE(f.adapter.starts.empty()) << c.name;
    EXPECT_FALSE(f.coordinator.receipt().outcome_unknown) << c.name;
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
  EXPECT_FALSE(receipt.outcome_unknown);
  EXPECT_LT(f.clock.slept, budgets.forward);
  EXPECT_GE(f.adapter.supports, 1) << "support proved for this launch";
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

TEST(PrelaunchTransitionTests, AHungCommandIsStoppedAtItsBudgetAsOutcomeUnknownAndNothingUnprovenIsUndone) {
  fixture_t f;
  f.adapter.hangs = true;
  const auto outcome = f.coordinator.run(launch_request());
  EXPECT_EQ(outcome.status, 504);
  EXPECT_EQ(outcome.code, "prelaunch_switch_outcome_unknown");
  EXPECT_EQ(outcome.state, state_e::failed);
  EXPECT_EQ(f.adapter.aborts, 1);
  EXPECT_GE(f.clock.slept, budgets.child - prelaunch::k_poll_period);
  EXPECT_LE(f.clock.slept, budgets.child + prelaunch::k_poll_period);
  ASSERT_EQ(f.adapter.starts.size(), 1u) << "no restore without a desktop identity this launch owns";
  const auto receipt = f.coordinator.receipt();
  EXPECT_EQ(receipt.restore, restore_e::refused);
  EXPECT_TRUE(receipt.outcome_unknown) << "it may have sent the switch before it was stopped";
}

TEST(PrelaunchTransitionTests, AFailedSwitchIsRefusedWithItsOwnCode) {
  fixture_t f;
  f.adapter.fails = true;
  const auto outcome = f.coordinator.run(launch_request());
  EXPECT_EQ(outcome.code, "prelaunch_switch_failed");
  EXPECT_EQ(outcome.state, state_e::failed);
  EXPECT_EQ(f.adapter.starts.size(), 1u);
  EXPECT_TRUE(f.coordinator.receipt().outcome_unknown) << "it ran; what it did before failing is not known";
}

TEST(PrelaunchTransitionTests, ACommandThatCouldNotStartChangedNothing) {
  fixture_t f;
  f.adapter.start_ok = false;
  const auto outcome = f.coordinator.run(launch_request());
  EXPECT_EQ(outcome.code, "prelaunch_switch_failed");
  EXPECT_FALSE(f.coordinator.receipt().outcome_unknown);
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::none);
}

TEST(PrelaunchTransitionTests, ExitZeroIsNotEnoughTheOldSessionMustBeSeenToEnd) {
  fixture_t f;
  // The command reports success, and the host never leaves its Game Mode session.
  f.adapter.host = nullptr;
  const auto outcome = f.coordinator.run(launch_request());
  EXPECT_EQ(outcome.code, "prelaunch_old_session_timeout");
  EXPECT_EQ(outcome.state, state_e::failed);
  const auto receipt = f.coordinator.receipt();
  EXPECT_EQ(receipt.restore, restore_e::refused);
  EXPECT_TRUE(receipt.outcome_unknown);
  EXPECT_FALSE(receipt.post);
  EXPECT_EQ(f.adapter.starts.size(), 1u) << "never rolled back";
}

TEST(PrelaunchTransitionTests, AFailedLaunchSpendsAtMostTheForwardAndRollbackBudgets) {
  fixture_t f;
  f.adapter.hangs_returning = true;
  auto request = launch_request();
  request.capture_ready = [](std::string &reason) {
    reason = "never";
    return false;
  };
  const auto outcome = f.coordinator.run(request);
  EXPECT_EQ(outcome.code, "prelaunch_capture_timeout");
  const auto receipt = f.coordinator.receipt();
  EXPECT_EQ(receipt.restore, restore_e::failed);
  EXPECT_TRUE(receipt.outcome_unknown) << "the return was stopped after it started";
  EXPECT_LE(f.clock.slept, budgets.forward + budgets.rollback + 2 * prelaunch::k_poll_period);
  EXPECT_LE(budgets.forward + budgets.rollback, std::chrono::seconds {120}) << "inside the /launch reply";
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
  EXPECT_LT(f.clock.slept, budgets.forward) << "cancelled promptly, not at the deadline";
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
    {"Game Mode starting", true, false, false, [](auto &h) { h.mode_state = steamos::game_mode_e::transitional; }, restore_e::refused},
    {"a different desktop session", true, false, false, [](auto &h) { h.session = identity("5", prelaunch::mode_e::desktop, 'd', 201, 9100); }, restore_e::refused},
    {"a restarted desktop leader with a reused pid", true, false, false, [](auto &h) { h.session->leader_start_ticks = 9999; }, restore_e::refused},
    {"a restarted graphical target", true, false, false, [](auto &h) { h.session->target_invocation = std::string(32, 'e'); }, restore_e::refused},
    {"the current stop would now reach Polaris", true, false, false, [](auto &h) {
       h.service = prelaunch::verdict_t::fail("prelaunch_service_not_independent", "new stop dependency reaches Polaris");
     }, restore_e::refused},
    {"headless boot no longer ready", true, false, false, [](auto &h) { h.independent = false; }, restore_e::refused},
    {"the host's sessions cannot be read", true, false, false, [](auto &h) {
       h.read_failure = prelaunch::verdict_t::fail("prelaunch_session_read_timeout", "stalled");
     }, restore_e::refused},
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
    EXPECT_EQ(f.end_session(generation, c.ended_on_request, c.daemon_shutdown, others), c.expected) << c.name;
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
  EXPECT_EQ(f.end_session(outcome.transition_generation, true, false, {}), restore_e::refused);
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
    const auto settled = commit ? f.end_session(first.transition_generation, true, false, nobody_else) :
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
    if (++checks == 1) f.host.session = identity("6", prelaunch::mode_e::desktop, 'f', 250, 9500);
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

TEST(PrelaunchTransitionTests, AReadyAnswerCannotExtendTheOriginalForwardDeadline) {
  for (const bool slow_final_observation : {false, true}) {
    fixture_t f;
    bool capture_checked = false;
    bool delayed = false;
    f.host.on_scan = [&]() {
      if (slow_final_observation && capture_checked && !delayed) {
        f.clock.at += budgets.forward;
        delayed = true;
      }
    };
    auto request = launch_request();
    request.capture_ready = [&](std::string &) {
      capture_checked = true;
      if (!slow_final_observation) f.clock.at += budgets.forward;
      return true;
    };
    const auto outcome = f.coordinator.run(request);
    EXPECT_FALSE(outcome.proceed()) << slow_final_observation;
    EXPECT_EQ(outcome.code, "prelaunch_capture_timeout") << slow_final_observation;
    EXPECT_EQ(f.coordinator.receipt().restore, restore_e::rolled_back);
    EXPECT_FALSE(f.coordinator.receipt().committed);
  }
}

TEST(PrelaunchTransitionTests, CancellationDuringAReadyCheckWinsBeforeHandOff) {
  fixture_t f;
  bool cancelled = false;
  auto request = launch_request();
  request.cancelled = [&]() { return cancelled; };
  request.capture_ready = [&](std::string &) {
    cancelled = true;
    return true;
  };
  const auto outcome = f.coordinator.run(request);
  EXPECT_FALSE(outcome.proceed());
  EXPECT_EQ(outcome.code, "prelaunch_cancelled");
  EXPECT_EQ(outcome.state, state_e::cancelled);
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::rolled_back);
  EXPECT_LT(f.clock.slept, budgets.forward);
}

TEST(PrelaunchTransitionTests, CancellationDuringTheFinalTypedReadWinsBeforeHandOff) {
  fixture_t f;
  const auto deadline = f.clock.at + budgets.forward;
  bool cancelled = false;
  int capture_scan = 0;
  f.host.read_clock = [&]() { return f.clock.now(); };
  f.host.on_scan = [&]() {
    if (capture_scan != 0 && f.host.scans == capture_scan + 1) cancelled = true;
  };
  auto request = launch_request();
  request.cancelled = [&]() { return cancelled; };
  request.capture_ready = [&](std::string &) {
    EXPECT_EQ(f.host.session, desktop_leader);
    EXPECT_FALSE(f.host.live);
    capture_scan = f.host.scans;
    return true;
  };
  const auto outcome = f.coordinator.run(request);
  ASSERT_NE(capture_scan, 0);
  ASSERT_EQ(f.host.failed_reads, 1) << "the final observer returned a failed read, not a same-desktop success";
  EXPECT_EQ(f.host.failed_read_scan, capture_scan + 1);
  EXPECT_EQ(f.host.last_failed_read.code, "prelaunch_cancelled");
  EXPECT_EQ(f.host.failed_read_deadline, deadline);
  EXPECT_FALSE(outcome.proceed());
  EXPECT_EQ(outcome.code, "prelaunch_cancelled");
  EXPECT_EQ(outcome.state, state_e::cancelled);
  const auto receipt = f.coordinator.receipt();
  EXPECT_EQ(receipt.pre, game_mode_leader);
  EXPECT_EQ(receipt.post, desktop_leader);
  EXPECT_EQ(receipt.restore, restore_e::rolled_back);
  EXPECT_FALSE(receipt.committed);
  EXPECT_EQ(f.adapter.starts, (std::vector<direction_e> {direction_e::to_desktop, direction_e::to_game_mode}));
  EXPECT_LT(f.clock.slept, budgets.forward);
}

TEST(PrelaunchTransitionTests, ExpiryDuringTheFinalTypedReadUsesTheOriginalForwardDeadline) {
  fixture_t f;
  const auto deadline = f.clock.at + budgets.forward;
  int capture_scan = 0;
  f.host.read_clock = [&]() { return f.clock.now(); };
  f.host.on_scan = [&]() {
    if (capture_scan != 0 && f.host.scans == capture_scan + 1) f.clock.at = deadline;
  };
  auto request = launch_request();
  request.capture_ready = [&](std::string &) {
    EXPECT_EQ(f.host.session, desktop_leader);
    EXPECT_FALSE(f.host.live);
    capture_scan = f.host.scans;
    return true;
  };
  const auto outcome = f.coordinator.run(request);
  ASSERT_NE(capture_scan, 0);
  ASSERT_EQ(f.host.failed_reads, 1) << "the final observer returned timeout before the coordinator's guard";
  EXPECT_EQ(f.host.failed_read_scan, capture_scan + 1);
  EXPECT_EQ(f.host.last_failed_read.code, "prelaunch_session_read_timeout");
  EXPECT_EQ(f.host.failed_read_deadline, deadline);
  EXPECT_FALSE(outcome.proceed());
  EXPECT_EQ(outcome.code, "prelaunch_capture_timeout");
  EXPECT_EQ(outcome.state, state_e::failed);
  const auto receipt = f.coordinator.receipt();
  EXPECT_EQ(receipt.pre, game_mode_leader);
  EXPECT_EQ(receipt.post, desktop_leader);
  EXPECT_EQ(receipt.restore, restore_e::rolled_back);
  EXPECT_FALSE(receipt.committed);
  EXPECT_EQ(f.adapter.starts, (std::vector<direction_e> {direction_e::to_desktop, direction_e::to_game_mode}));
  EXPECT_LT(f.clock.now(), deadline + budgets.rollback);
}

TEST(PrelaunchTransitionTests, AnUnreadFinalDesktopWaitsForFreshProofInsteadOfClaimingReplacement) {
  fixture_t f;
  const auto deadline = f.clock.at + budgets.forward;
  int capture_checks = 0;
  bool unread_delivered = false;
  f.host.on_scan = [&]() {
    if (capture_checks == 1 && !unread_delivered) {
      // An unavailable read is not a replacement even if it has no diagnostic text.
      f.host.read_failure = prelaunch::verdict_t::fail("prelaunch_session_read_failed", "");
      unread_delivered = true;
    } else {
      f.host.read_failure = prelaunch::verdict_t::pass();
    }
  };
  auto request = launch_request();
  request.capture_ready = [&](std::string &) {
    ++capture_checks;
    EXPECT_EQ(f.host.session, desktop_leader);
    EXPECT_FALSE(f.host.live);
    return true;
  };
  const auto outcome = f.coordinator.run(request);
  ASSERT_TRUE(unread_delivered);
  EXPECT_EQ(f.host.failed_reads, 1);
  EXPECT_EQ(f.host.last_failed_read.code, "prelaunch_session_read_failed");
  EXPECT_EQ(f.host.failed_read_deadline, deadline);
  EXPECT_EQ(capture_checks, 2) << "readiness follows a new successful typed read and capture check";
  EXPECT_TRUE(outcome.proceed());
  EXPECT_EQ(outcome.state, state_e::ready);
  EXPECT_EQ(f.coordinator.receipt().pre, game_mode_leader);
  EXPECT_EQ(f.coordinator.receipt().post, desktop_leader);
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::owned);
  EXPECT_LT(f.clock.now(), deadline);
  EXPECT_EQ(f.adapter.starts.size(), 1u);
  EXPECT_EQ(f.coordinator.rollback(outcome.transition_generation, nobody_else), restore_e::rolled_back);
}

TEST(PrelaunchTransitionTests, AReadyCaptureCannotAdoptAReplacementTypedDesktop) {
  fixture_t f;
  auto request = launch_request();
  request.capture_ready = [&](std::string &) {
    f.host.session = identity("6", prelaunch::mode_e::desktop, 'f', 250, 9500);
    return true;
  };
  const auto outcome = f.coordinator.run(request);
  EXPECT_FALSE(outcome.proceed());
  EXPECT_EQ(outcome.code, "prelaunch_desktop_changed");
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::refused);
  EXPECT_EQ(f.adapter.starts.size(), 1u) << "a replacement desktop is not ours to roll back";
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

TEST(PrelaunchTransitionTests, AnOutOfRangeUidNeverMatchesThisAccount) {
  scratch_proc_t overflow;
  overflow.add(700, 1000, {"/fixture/own-account/steam/reaper", "SteamLaunch", "AppId=620"}, 50, "");
  // 2^32 + 1000 must not wrap to uid 1000.
  std::ofstream {overflow.root / "700" / "status"} << "Name:\tx\nUid:\t4294968296\t1000\t1000\t1000\n";
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

TEST(PrelaunchTransitionTests, StoppingRefusesNewTransitionsAndEveryRestore) {
  fixture_t f;
  const auto outcome = f.coordinator.run(launch_request());
  ASSERT_TRUE(outcome.proceed());
  f.coordinator.commit(outcome.transition_generation);
  f.coordinator.request_stop();
  EXPECT_EQ(f.coordinator.run(launch_request("client-b")).code, "prelaunch_shutting_down");
  EXPECT_EQ(f.coordinator.on_session_end(outcome.transition_generation, true, false, nobody_else), restore_e::refused)
    << "the host stays where the session left it";
  EXPECT_EQ(f.coordinator.receipt().restore_reason, "Polaris is stopping");
  EXPECT_EQ(f.adapter.starts.size(), 1u);
  f.coordinator.shutdown();
  f.coordinator.shutdown();
}

TEST(PrelaunchTransitionTests, ATransitionRunningWhenPolarisStopsEndsAtItsNextCheck) {
  fixture_t f;
  auto request = launch_request();
  request.capture_ready = [&](std::string &) {
    f.coordinator.request_stop();
    return false;
  };
  const auto outcome = f.coordinator.run(request);
  EXPECT_EQ(outcome.state, state_e::cancelled);
  EXPECT_LT(f.clock.slept, budgets.forward);
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::refused) << "no rollback while stopping";
  EXPECT_EQ(f.adapter.starts.size(), 1u);
}

TEST(PrelaunchTransitionTests, ASessionEndOnlyClaimsTheRestoreTheWorkerActsAfterTheCallerReturns) {
  fixture_t f;
  const auto outcome = f.coordinator.run(launch_request());
  ASSERT_TRUE(outcome.proceed());
  f.coordinator.commit(outcome.transition_generation);
  std::atomic<bool> others_ran_on_caller {false};
  const auto caller = std::this_thread::get_id();
  std::promise<void> release;
  auto released = release.get_future().share();
  const prelaunch::coordinator_t::others_t others = [&, released](std::string &) {
    others_ran_on_caller = std::this_thread::get_id() == caller;
    released.wait();
    return false;
  };
  EXPECT_EQ(f.coordinator.on_session_end(outcome.transition_generation, true, false, others), restore_e::owned);
  // The caller is back while the restore waits: nothing ran under its locks, and the fence holds.
  EXPECT_TRUE(f.coordinator.restore_pending());
  EXPECT_EQ(f.coordinator.run(launch_request("client-b")).code, "prelaunch_transition_busy");
  release.set_value();
  f.coordinator.wait_for_restore();
  EXPECT_FALSE(others_ran_on_caller);
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::restored);
  EXPECT_FALSE(f.coordinator.restore_pending());
}

TEST(PrelaunchTransitionTests, TheEntrysOwnCancellationTransportIsRecordedHonestly) {
  for (const bool has_cancel : {false, true}) {
    fixture_t f;
    auto request = launch_request();
    if (has_cancel) {
      request.cancelled = []() {
        return false;
      };
    }
    ASSERT_TRUE(f.coordinator.run(request).proceed());
    EXPECT_EQ(f.coordinator.receipt().cancellation_transport, has_cancel ? "launch" : "unsupported");
  }
}

TEST(PrelaunchTransitionTests, TheSurvivalProofIsAskedForOnceBeforeTheSwitch) {
  fixture_t f;
  ASSERT_TRUE(f.coordinator.run(launch_request()).proceed());
  EXPECT_EQ(f.host.service_proofs, 1) << "proved on the admission observation, not left to the waits";
}

/// A fence standing in for the lifecycle gate: it records what the coordinator did with it.
struct recording_fence_t {
  std::atomic<bool> launch_admitted {false};
  std::atomic<bool> fenced {false};
  std::atomic<int> releases {0};
  std::function<void()> on_release;

  prelaunch::admission_fence_t fence() {
    return {
      [this]() {
        if (launch_admitted || fenced) return false;
        fenced = true;
        return true;
      },
      [this]() {
        if (on_release) on_release();
        fenced = false;
        ++releases;
      },
    };
  }
};

TEST(PrelaunchTransitionTests, ARestoreIsNeverClaimedWhileALaunchIsAdmitted) {
  fixture_t f;
  const auto outcome = f.coordinator.run(launch_request());
  ASSERT_TRUE(outcome.proceed());
  f.coordinator.commit(outcome.transition_generation);
  recording_fence_t gate;
  gate.launch_admitted = true;  // e.g. this teardown runs inside a new launch's admission
  EXPECT_EQ(f.coordinator.on_session_end(outcome.transition_generation, true, false, nobody_else, gate.fence()), restore_e::refused);
  EXPECT_EQ(f.coordinator.receipt().restore_reason, "a launch was being admitted when the session ended");
  EXPECT_FALSE(f.coordinator.restore_pending());
  EXPECT_EQ(gate.releases.load(), 0) << "nothing was acquired";
  EXPECT_EQ(f.adapter.starts.size(), 1u);
}

TEST(PrelaunchTransitionTests, TheAdmissionFenceIsHeldFromTheClaimUntilTheOutcomeIsRecorded) {
  fixture_t f;
  const auto outcome = f.coordinator.run(launch_request());
  ASSERT_TRUE(outcome.proceed());
  f.coordinator.commit(outcome.transition_generation);
  recording_fence_t gate;
  restore_e recorded_at_release = restore_e::none;
  gate.on_release = [&]() {
    recorded_at_release = f.coordinator.receipt().restore;
  };
  std::promise<void> unstall;
  auto unstalled = unstall.get_future().share();
  const prelaunch::coordinator_t::others_t stalled = [unstalled](std::string &) {
    unstalled.wait();  // the restore stalls in its proofs
    return false;
  };
  EXPECT_EQ(f.coordinator.on_session_end(outcome.transition_generation, true, false, stalled, gate.fence()), restore_e::owned);
  EXPECT_TRUE(gate.fenced) << "taken in the same step as the claim";
  // While it stalls, a policy-off launch cannot get past the fence, and the gate's claim stays.
  EXPECT_FALSE(gate.fence().acquire()) << "already fenced";
  EXPECT_EQ(f.coordinator.run(launch_request("client-b")).code, "prelaunch_transition_busy");
  unstall.set_value();
  f.coordinator.wait_for_restore();
  EXPECT_FALSE(gate.fenced);
  EXPECT_EQ(gate.releases.load(), 1);
  EXPECT_EQ(recorded_at_release, restore_e::restored) << "released only after the outcome was recorded";
}

TEST(PrelaunchTransitionTests, OneSerialWorkerServesSuccessiveRestores) {
  fixture_t f;
  recording_fence_t gate;
  for (int round = 0; round < 3; ++round) {
    f.host.live = true;
    f.host.session = game_mode_leader;
    const auto outcome = f.coordinator.run(launch_request());
    ASSERT_TRUE(outcome.proceed()) << round << outcome.code;
    f.coordinator.commit(outcome.transition_generation);
    EXPECT_EQ(f.end_session(outcome.transition_generation, true, false, nobody_else, gate.fence()), restore_e::restored) << round;
  }
  EXPECT_EQ(gate.releases.load(), 3);
}

TEST(PrelaunchTransitionTests, AStopDuringTheRestoreProofsWinsOverTheEffect) {
  fixture_t f;
  const auto outcome = f.coordinator.run(launch_request());
  ASSERT_TRUE(outcome.proceed());
  f.coordinator.commit(outcome.transition_generation);
  recording_fence_t gate;
  std::promise<void> in_proofs;
  auto reached = in_proofs.get_future();
  const prelaunch::coordinator_t::others_t stop_now = [&](std::string &) {
    f.coordinator.request_stop();  // Polaris starts stopping after the claim, during the proofs
    in_proofs.set_value();
    return false;
  };
  EXPECT_EQ(f.coordinator.on_session_end(outcome.transition_generation, true, false, stop_now, gate.fence()), restore_e::owned);
  // The worker really is past its first stop check: the stop lands inside the proofs.
  ASSERT_EQ(reached.wait_for(std::chrono::seconds {5}), std::future_status::ready);
  f.coordinator.wait_for_restore();
  EXPECT_EQ(f.coordinator.receipt().restore_reason, "Polaris is stopping");
  f.coordinator.shutdown();
  EXPECT_EQ(f.coordinator.receipt().restore, restore_e::refused);
  EXPECT_EQ(f.adapter.starts.size(), 1u) << "the return to Game Mode never started";
  EXPECT_EQ(gate.releases.load(), 1) << "shutdown still releases the fence";
}

TEST(PrelaunchTransitionTests, AProcessStopLatchedBeforeTheCoordinatorExistsStopsItFromBirth) {
  scripted_host_t host;
  scripted_adapter_t adapter;
  adapter.host = &host;
  fake_clock_t clock;
  prelaunch::request_stop();  // before the registry has made this coordinator
  prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
  const auto outcome = prelaunch::coordinator().run(launch_request());
  prelaunch::set_dependencies_for_tests(nullptr, nullptr, nullptr);
  prelaunch::reset_process_stop_for_tests();
  EXPECT_EQ(outcome.code, "prelaunch_shutting_down");
  EXPECT_TRUE(adapter.starts.empty());
  EXPECT_EQ(host.scans, 0);
}

TEST(PrelaunchTransitionTests, RegisteredCasesAreFrozen) {
  const std::set<std::string> expected {
    "AnAppWithTheSettingOffNeverScansProbesOrSwitches",
    "ResumeWatchAndInputOnlyBindTheirGenerationAndNeverSwitch",
    "TheProductionAdapterIsRefusedBeforeAnyEffectEvenWithTheSettingOn",
    "AHostNotInGameModeNeedsNothing",
    "AnUnreadHostIsRefusedNeverTreatedAsNotInGameMode",
    "ProfileSpaceAndOccupiedHostsAreRefusedBeforeTheScan",
    "AMirrorRouteIsRefusedRatherThanLeavingGameModeForNothing",
    "AnOpenTitleMissingServiceIndependenceOrUnprovenSessionRefusesWithoutSwitching",
    "AnAdmittedTransitionWaitsForTheOldSessionTheDesktopAndCaptureThenOwnsTheChange",
    "TheTransitionStagesAreNotedAroundTheSwitch",
    "AHungCommandIsStoppedAtItsBudgetAsOutcomeUnknownAndNothingUnprovenIsUndone",
    "AFailedSwitchIsRefusedWithItsOwnCode",
    "ACommandThatCouldNotStartChangedNothing",
    "ExitZeroIsNotEnoughTheOldSessionMustBeSeenToEnd",
    "AFailedLaunchSpendsAtMostTheForwardAndRollbackBudgets",
    "CaptureThatNeverBecomesReadyRollsBackOnlyTheDesktopThisLaunchOpened",
    "OnlyTheRequestingClientCanCancelItsTransition",
    "ASecondLaunchDuringATransitionIsRefusedAndHoldsNoLockAcrossTheWait",
    "OnlyAPurposefulEndOfTheCommittedGenerationRestoresAndOnlyItsOwnDesktop",
    "AnUncommittedOrStaleGenerationIsNeverRestoredBySessionEnd",
    "ARestoreWithNoWayToCheckTheHostsOtherSessionsIsRefused",
    "AnOwnedChangeCommittedOrNotBlocksAnotherTransitionUntilItSettles",
    "AnImplementedAdapterWithoutItsInstalledCommandIsNeverStarted",
    "ADesktopThatChangesWhileCaptureWaitsIsNeverPublishedAsOwned",
    "AReadyAnswerCannotExtendTheOriginalForwardDeadline",
    "CancellationDuringAReadyCheckWinsBeforeHandOff",
    "CancellationDuringTheFinalTypedReadWinsBeforeHandOff",
    "ExpiryDuringTheFinalTypedReadUsesTheOriginalForwardDeadline",
    "AnUnreadFinalDesktopWaitsForFreshProofInsteadOfClaimingReplacement",
    "AReadyCaptureCannotAdoptAReplacementTypedDesktop",
    "ALaunchScopeRollsBackAnUncommittedReadyTransitionOnce",
    "StagesReachTheTestSinkInOrderAndProductionHasNone",
    "AnOutOfRangeUidNeverMatchesThisAccount",
    "AnOpenSteamTitleIsSteamsOwnReaperForThisAccount",
    "OnlyThePolarisUserServiceOutsideALoginScopeIsIndependent",
    "StoppingRefusesNewTransitionsAndEveryRestore",
    "ATransitionRunningWhenPolarisStopsEndsAtItsNextCheck",
    "ASessionEndOnlyClaimsTheRestoreTheWorkerActsAfterTheCallerReturns",
    "TheEntrysOwnCancellationTransportIsRecordedHonestly",
    "TheSurvivalProofIsAskedForOnceBeforeTheSwitch",
    "ARestoreIsNeverClaimedWhileALaunchIsAdmitted",
    "TheAdmissionFenceIsHeldFromTheClaimUntilTheOutcomeIsRecorded",
    "OneSerialWorkerServesSuccessiveRestores",
    "AStopDuringTheRestoreProofsWinsOverTheEffect",
    "AProcessStopLatchedBeforeTheCoordinatorExistsStopsItFromBirth",
    "RegisteredCasesAreFrozen",
  };
  std::set<std::string> registered;
  const auto *unit = ::testing::UnitTest::GetInstance();
  for (int suite = 0; suite < unit->total_test_suite_count(); ++suite) {
    const auto *info = unit->GetTestSuite(suite);
    if (std::string {info->name()} != "PrelaunchTransitionTests") continue;
    for (int test = 0; test < info->total_test_count(); ++test) registered.insert(info->GetTestInfo(test)->name());
  }
  EXPECT_EQ(registered, expected);
}
