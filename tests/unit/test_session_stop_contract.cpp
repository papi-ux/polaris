/**
 * @file tests/unit/test_session_stop_contract.cpp
 * @brief Behavioral HTTP and RTSP-role contracts for the Polaris v1 session stop endpoint.
 */

#include <src/nvhttp.h>
#include <src/process.h>
#include <src/rtsp.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

namespace {
  using proc::session_stop_outcome_t;
  using rtsp_stream::session_role_e;

  session_stop_outcome_t decide(
    bool can_launch,
    bool has_running_app,
    int active_sessions,
    bool owned_by_client,
    session_role_e requester_role,
    bool stop_in_progress = false,
    bool token_matches = true
  ) {
    return proc::evaluate_session_stop_request(
      can_launch,
      has_running_app,
      active_sessions,
      owned_by_client,
      requester_role,
      stop_in_progress,
      token_matches
    );
  }

  std::string read_rtsp_source_for_contract() {
    const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/rtsp.cpp";
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }
}

TEST(ProcessRefreshContractTests, ParsedConfigurationPreservesLifecycleIdentityAndGeneration) {
  auto current_env = boost::this_process::environment();
  proc::proc_t subject {std::move(current_env), {}};
  const auto identity_before = subject.session_lifecycle_identity_for_tests();
  const auto generation_before = subject.capture_session_launch_generation();
  ASSERT_TRUE(generation_before.has_value());

  auto refreshed_env = boost::this_process::environment();
  std::vector<proc::ctx_t> refreshed_apps;
  proc::ctx_t refreshed_app;
  refreshed_app.name = "refreshed-app";
  refreshed_apps.emplace_back(std::move(refreshed_app));
  proc::proc_t parsed {std::move(refreshed_env), std::move(refreshed_apps)};

  subject.reload_configuration(std::move(parsed));

  EXPECT_EQ(subject.session_lifecycle_identity_for_tests(), identity_before);
  EXPECT_EQ(subject.capture_session_launch_generation(), generation_before);
  ASSERT_EQ(subject.get_apps().size(), 1);
  EXPECT_EQ(subject.get_apps().front().name, "refreshed-app");
}

TEST(SessionStopContractTests, ActiveOwnerCanStopRunningSession) {
  EXPECT_EQ(decide(true, true, 1, true, session_role_e::controller), session_stop_outcome_t::allowed);
}

TEST(SessionStopContractTests, RevokedLaunchPermissionIsDeniedForStatusAndPostDecision) {
  EXPECT_EQ(decide(false, true, 1, true, session_role_e::controller), session_stop_outcome_t::permission_denied);
}

TEST(SessionStopContractTests, ActiveControllerCanCleanUpRtspOnlySession) {
  EXPECT_EQ(decide(true, false, 1, false, session_role_e::controller), session_stop_outcome_t::allowed);
}

TEST(SessionStopContractTests, UnrelatedPairedClientCannotStopRtspOnlySession) {
  EXPECT_EQ(decide(true, false, 1, false, session_role_e::none), session_stop_outcome_t::uncontrolled_stream);
}

TEST(SessionStopContractTests, ViewerCannotStopRtspOnlySession) {
  EXPECT_EQ(decide(true, false, 1, false, session_role_e::viewer), session_stop_outcome_t::viewer_forbidden);
}

TEST(SessionStopContractTests, DifferentPairedClientCannotStopOwnedSession) {
  EXPECT_EQ(decide(true, true, 1, false, session_role_e::none), session_stop_outcome_t::other_owner);
}

TEST(SessionStopContractTests, DuplicateStopIsRejectedWhileStopIsInProgress) {
  EXPECT_EQ(
    decide(true, true, 1, true, session_role_e::controller, true),
    session_stop_outcome_t::stop_in_progress
  );
}

TEST(SessionStopContractTests, RtspOnlyDuplicateStopBeatsZeroSessionIdempotence) {
  EXPECT_EQ(
    decide(true, false, 0, false, session_role_e::none, true),
    session_stop_outcome_t::stop_in_progress
  );
}

TEST(SessionStopContractTests, LegacyCancelOwnerWithoutTokenKeepsOwnerAuthorizedSemantics) {
  EXPECT_EQ(
    decide(true, true, 1, true, session_role_e::controller, false, true),
    session_stop_outcome_t::allowed
  );
}

TEST(SessionStopContractTests, LegacyOwnerAuthorizationDoesNotRequireAnActiveToken) {
  EXPECT_TRUE(proc::session_stop_token_matches(false, "", "", {}));
  EXPECT_TRUE(proc::session_stop_token_matches(false, "", "active-token", {}));
  EXPECT_FALSE(proc::session_stop_token_matches(true, "", "active-token", {}));
  EXPECT_TRUE(proc::session_stop_token_matches(true, "active-token", "active-token", {}));
}

TEST(SessionStopContractTests, ExactTokenIsRequiredForRtspOnlyPendingSession) {
  rtsp_stream::terminate_sessions();
  auto pending = std::make_shared<rtsp_stream::launch_session_t>();
  pending->id = 4242;
  pending->unique_id = "rtsp-only-controller";
  pending->session_token = "rtsp-only-token";
  ASSERT_TRUE(rtsp_stream::launch_session_raise(pending));

  auto env = boost::this_process::environment();
  proc::proc_t subject {std::move(env), {}};

  const auto omitted = subject.request_session_shutdown(
    pending->unique_id,
    "",
    true,
    true
  );
  EXPECT_EQ(omitted.snapshot.outcome, session_stop_outcome_t::token_mismatch);
  EXPECT_FALSE(omitted.stopped);
  EXPECT_FALSE(pending->is_cancelled());

  const auto mismatched = subject.request_session_shutdown(
    pending->unique_id,
    "wrong-token",
    true,
    true
  );
  EXPECT_EQ(mismatched.snapshot.outcome, session_stop_outcome_t::token_mismatch);
  EXPECT_FALSE(mismatched.stopped);
  EXPECT_FALSE(pending->is_cancelled());

  const auto matched = subject.request_session_shutdown(
    pending->unique_id,
    "rtsp-only-token",
    true,
    true
  );
  EXPECT_EQ(matched.snapshot.outcome, session_stop_outcome_t::allowed);
  EXPECT_TRUE(matched.stopped);
  EXPECT_TRUE(pending->is_cancelled());
  rtsp_stream::terminate_sessions();
}

TEST(SessionStopContractTests, MismatchedTokenIsRejectedForRunningApp) {
  EXPECT_EQ(
    decide(true, true, 1, true, session_role_e::controller, false, false),
    session_stop_outcome_t::token_mismatch
  );
}

TEST(SessionStopContractTests, AuthorizedZeroSessionRequestIsIdempotentNoOp) {
  EXPECT_EQ(decide(true, false, 0, false, session_role_e::none), session_stop_outcome_t::no_active_session);
}

TEST(SessionStopContractTests, ZeroSessionRequestStillRequiresLaunchPermission) {
  EXPECT_EQ(decide(false, false, 0, false, session_role_e::none), session_stop_outcome_t::permission_denied);
}

TEST(SessionStopContractTests, OmittedTokenCannotMatchRunningSession) {
  EXPECT_FALSE(proc::session_token_matches_active("", "active-token"));
}

TEST(SessionStopContractTests, MissingActiveTokenCannotAuthorizeRunningSession) {
  EXPECT_FALSE(proc::session_token_matches_active("expected-token", ""));
}

TEST(SessionStopContractTests, ExactNonEmptyTokenMatchesRunningSession) {
  EXPECT_TRUE(proc::session_token_matches_active("session-token", "session-token"));
}

TEST(SessionStopContractTests, DifferentNonEmptyTokenCannotMatchRunningSession) {
  EXPECT_FALSE(proc::session_token_matches_active("stale-token", "active-token"));
}

TEST(SessionRoleContractTests, StoppingSlotIsExcludedAndConcurrentCleanupClaimsItOnce) {
  rtsp_stream::terminate_sessions();
  std::atomic_int cleaned {0};
  rtsp_stream::set_cleanup_session_probe_for_tests([&cleaned]() {
    ++cleaned;
  });

  rtsp_stream::launch_session_t launch {};
  launch.id = 5201;
  launch.unique_id = "stopping-slot";
  launch.session_token = "stopping-token";
  rtsp_stream::add_session_for_tests(launch, true);

  const auto stopping_snapshot = rtsp_stream::session_snapshot(launch.unique_id);
  EXPECT_EQ(stopping_snapshot.active_sessions, 0);
  EXPECT_TRUE(stopping_snapshot.requester_session_tokens.empty());

  auto first = std::async(std::launch::async, []() {
    rtsp_stream::run_cleanup_for_tests();
  });
  auto second = std::async(std::launch::async, []() {
    rtsp_stream::run_cleanup_for_tests();
  });
  first.get();
  second.get();

  EXPECT_EQ(cleaned.load(), 1);
  EXPECT_EQ(rtsp_stream::session_snapshot(launch.unique_id).active_sessions, 0);
  rtsp_stream::set_cleanup_session_probe_for_tests({});
}

TEST(SessionStopContractTests, ExactTokenIsRequiredForRtspOnlyActiveSession) {
  rtsp_stream::terminate_sessions();
  std::atomic_int cleaned {0};
  rtsp_stream::set_cleanup_session_probe_for_tests([&cleaned]() {
    ++cleaned;
  });

  rtsp_stream::launch_session_t launch {};
  launch.id = 5202;
  launch.unique_id = "active-rtsp-controller";
  launch.session_token = "active-rtsp-token";
  rtsp_stream::add_session_for_tests(launch, false);

  auto env = boost::this_process::environment();
  proc::proc_t subject {std::move(env), {}};
  const auto omitted = subject.request_session_shutdown(launch.unique_id, "", true, true);
  EXPECT_EQ(omitted.snapshot.outcome, session_stop_outcome_t::token_mismatch);
  EXPECT_FALSE(omitted.stopped);
  const auto mismatched = subject.request_session_shutdown(launch.unique_id, "wrong", true, true);
  EXPECT_EQ(mismatched.snapshot.outcome, session_stop_outcome_t::token_mismatch);
  EXPECT_FALSE(mismatched.stopped);
  EXPECT_EQ(rtsp_stream::session_snapshot(launch.unique_id).active_sessions, 1);

  const auto matched = subject.request_session_shutdown(
    launch.unique_id,
    "active-rtsp-token",
    true,
    true
  );
  EXPECT_EQ(matched.snapshot.outcome, session_stop_outcome_t::allowed);
  EXPECT_TRUE(matched.stopped);
  EXPECT_EQ(cleaned.load(), 1);
  EXPECT_EQ(rtsp_stream::session_snapshot(launch.unique_id).active_sessions, 0);
  rtsp_stream::terminate_sessions();
  rtsp_stream::set_cleanup_session_probe_for_tests({});
}

TEST(SessionRoleContractTests, RtspCleanupReleasesSlotsBeforeJoiningSessionThreads) {
  auto completed = std::make_shared<std::promise<void>>();
  auto completed_future = completed->get_future();

  rtsp_stream::set_cleanup_unlocked_probe_for_tests([completed, &completed_future]() {
    std::thread([completed]() {
      static_cast<void>(rtsp_stream::session_snapshot("cleanup-probe"));
      completed->set_value();
    }).detach();
    EXPECT_EQ(completed_future.wait_for(100ms), std::future_status::ready);
  });

  rtsp_stream::run_cleanup_for_tests();
  EXPECT_EQ(completed_future.wait_for(1s), std::future_status::ready);
  rtsp_stream::set_cleanup_unlocked_probe_for_tests({});
}

TEST(SessionRoleContractTests, StatusSnapshotDoesNotRunRtspCleanupOrJoin) {
  rtsp_stream::reset_cleanup_call_count_for_tests();
  static_cast<void>(rtsp_stream::session_snapshot("snapshot-client"));
  EXPECT_EQ(rtsp_stream::cleanup_call_count_for_tests(), 0);
}

TEST(SessionRoleContractTests, ControllerBeforeViewerStillCountsEveryViewer) {
  rtsp_stream::session_snapshot_t snapshot;
  rtsp_stream::accumulate_session_snapshot_for_tests(snapshot, true, false);
  rtsp_stream::accumulate_session_snapshot_for_tests(snapshot, false, true);
  EXPECT_EQ(snapshot.requester_role, session_role_e::controller);
  EXPECT_EQ(snapshot.viewer_count, 1);
}

TEST(RtspLaunchHandoffTests, CancellationAfterRealSlotInsertionRollsBackBeforeCommit) {
  rtsp_stream::terminate_sessions();
  rtsp_stream::launch_session_t launch {};
  launch.id = 5101;
  launch.unique_id = "insert-cancel";
  launch.session_token = "insert-cancel-token";

  const auto result = rtsp_stream::run_setup_insert_for_tests(launch, true, 0);
  EXPECT_EQ(result, rtsp_stream::setup_insert_result_e::cancelled);
  EXPECT_TRUE(launch.is_cancelled());
  EXPECT_EQ(rtsp_stream::session_snapshot(launch.unique_id).active_sessions, 0);
  rtsp_stream::set_cleanup_session_probe_for_tests([]() {});
  rtsp_stream::terminate_sessions();
  rtsp_stream::set_cleanup_session_probe_for_tests({});
}

TEST(RtspLaunchHandoffTests, StartFailureAfterCommitCancelsAndRollsBackRealSlot) {
  rtsp_stream::terminate_sessions();
  rtsp_stream::launch_session_t launch {};
  launch.id = 5102;
  launch.unique_id = "insert-start-failure";
  launch.session_token = "insert-start-failure-token";

  const auto result = rtsp_stream::run_setup_insert_for_tests(launch, false, 1);
  EXPECT_EQ(result, rtsp_stream::setup_insert_result_e::failed);
  EXPECT_TRUE(launch.is_cancelled());
  EXPECT_EQ(rtsp_stream::session_snapshot(launch.unique_id).active_sessions, 0);
  rtsp_stream::set_cleanup_session_probe_for_tests([]() {});
  rtsp_stream::terminate_sessions();
  rtsp_stream::set_cleanup_session_probe_for_tests({});
}

TEST(RtspLaunchHandoffTests, TimeoutAndSetupHandoffHaveOneAtomicWinner) {
  rtsp_stream::launch_session_t timeout_wins {};
  EXPECT_TRUE(timeout_wins.cancel_for_timeout());
  EXPECT_FALSE(timeout_wins.try_begin_setup_handoff());
  EXPECT_TRUE(timeout_wins.is_cancelled());

  rtsp_stream::launch_session_t timeout_beats_handoff {};
  EXPECT_TRUE(timeout_beats_handoff.try_begin_setup_handoff());
  EXPECT_TRUE(timeout_beats_handoff.cancel_for_timeout());
  EXPECT_FALSE(timeout_beats_handoff.commit_setup_start());

  rtsp_stream::launch_session_t setup_wins {};
  EXPECT_TRUE(setup_wins.try_begin_setup_handoff());
  EXPECT_TRUE(setup_wins.commit_setup_start());
  EXPECT_FALSE(setup_wins.cancel_for_timeout());
  EXPECT_FALSE(setup_wins.is_cancelled());
}

TEST(RtspLaunchHandoffTests, FollowupControlConnectionsRemainAdmissibleAfterSetupStarts) {
  rtsp_stream::launch_session_t session {};
  EXPECT_TRUE(session.accepts_control_connection());

  ASSERT_TRUE(session.try_begin_setup_handoff());
  EXPECT_TRUE(session.accepts_control_connection());

  ASSERT_TRUE(session.commit_setup_start());
  EXPECT_TRUE(session.accepts_control_connection());

  session.cancel();
  EXPECT_FALSE(session.accepts_control_connection());
}

TEST(RtspLaunchHandoffTests, StartedControlCommandsRequireMatchingLiveSessionSlot) {
  rtsp_stream::terminate_sessions();
  rtsp_stream::launch_session_t session {};
  session.id = 5103;
  session.unique_id = "started-command-admission";
  session.session_token = "started-command-admission-token";

  ASSERT_TRUE(session.try_begin_setup_handoff());
  ASSERT_TRUE(session.commit_setup_start());
  EXPECT_FALSE(rtsp_stream::control_command_admissible_for_tests(session));

  rtsp_stream::launch_session_t reconnect {};
  reconnect.id = 5104;
  reconnect.unique_id = session.unique_id;
  reconnect.session_token = session.session_token;
  ASSERT_TRUE(reconnect.try_begin_setup_handoff());
  ASSERT_TRUE(reconnect.commit_setup_start());
  rtsp_stream::add_session_for_tests(reconnect, false);
  EXPECT_FALSE(rtsp_stream::control_command_admissible_for_tests(session));
  rtsp_stream::set_cleanup_session_probe_for_tests([]() {});
  rtsp_stream::terminate_sessions();
  rtsp_stream::set_cleanup_session_probe_for_tests({});

  rtsp_stream::add_session_for_tests(session, true);
  EXPECT_FALSE(rtsp_stream::control_command_admissible_for_tests(session));
  rtsp_stream::set_cleanup_session_probe_for_tests([]() {});
  rtsp_stream::terminate_sessions();
  rtsp_stream::set_cleanup_session_probe_for_tests({});

  rtsp_stream::add_session_for_tests(session, false);
  EXPECT_TRUE(rtsp_stream::control_command_admissible_for_tests(session));

  rtsp_stream::set_cleanup_session_probe_for_tests([]() {});
  rtsp_stream::terminate_sessions();
  rtsp_stream::set_cleanup_session_probe_for_tests({});
  EXPECT_FALSE(rtsp_stream::control_command_admissible_for_tests(session));
}

TEST(RtspLaunchHandoffTests, AcceptAndDispatchUseSlotAwareCommandAdmission) {
  const auto source = read_rtsp_source_for_contract();
  ASSERT_FALSE(source.empty());

  const auto handle_start = source.find("void handle_msg(tcp::socket &sock, launch_session_t &session, msg_t &&req)");
  const auto handle_end = source.find("void handle_accept(const boost::system::error_code &ec)", handle_start);
  ASSERT_NE(handle_start, std::string::npos);
  ASSERT_NE(handle_end, std::string::npos);

  const auto handle_body = source.substr(handle_start, handle_end - handle_start);
  const auto dispatch_gate = handle_body.find("if (!control_command_admissible(session))");
  const auto command_dispatch = handle_body.find("_map_cmd_cb.find");
  EXPECT_NE(dispatch_gate, std::string::npos);
  ASSERT_NE(command_dispatch, std::string::npos);
  if (dispatch_gate != std::string::npos) {
    EXPECT_LT(dispatch_gate, command_dispatch);
  }

  const auto accept_start = handle_end;
  const auto accept_end = source.find("void map(const std::string_view &type, cmd_func_t cb)", accept_start);
  ASSERT_NE(accept_end, std::string::npos);
  const auto accept_body = source.substr(accept_start, accept_end - accept_start);
  EXPECT_NE(
    accept_body.find("if (launch_session && control_command_admissible(*launch_session))"),
    std::string::npos
  );
}

TEST(RtspLaunchHandoffTests, SnapshotAndTeardownRetainClaimedHandoffBeforeSlotInsertion) {
  rtsp_stream::terminate_sessions();
  auto session = std::make_shared<rtsp_stream::launch_session_t>();
  session->id = 3131;
  session->unique_id = "handoff-controller";
  ASSERT_TRUE(rtsp_stream::launch_session_raise(session));
  ASSERT_TRUE(session->try_begin_setup_handoff());

  const auto during_handoff = rtsp_stream::session_snapshot(session->unique_id);
  EXPECT_EQ(during_handoff.pending_sessions, 1);
  EXPECT_EQ(during_handoff.requester_role, session_role_e::controller);

  rtsp_stream::terminate_sessions();
  EXPECT_TRUE(session->is_cancelled());
  EXPECT_FALSE(session->commit_setup_start());
}

TEST(RtspLaunchHandoffTests, TimerExpiryRemovesStartedAndCancelledLaunchEvents) {
  rtsp_stream::terminate_sessions();
  auto started = std::make_shared<rtsp_stream::launch_session_t>();
  started->id = 3130;
  started->unique_id = "started-expiry";
  ASSERT_TRUE(rtsp_stream::launch_session_raise(started));
  const auto started_generation = rtsp_stream::launch_timer_generation_for_tests();
  ASSERT_TRUE(started->try_begin_setup_handoff());
  ASSERT_TRUE(started->commit_setup_start());
  EXPECT_FALSE(rtsp_stream::expire_pending_launch_for_tests(started->id, started_generation));

  auto after_started = std::make_shared<rtsp_stream::launch_session_t>();
  after_started->id = 3129;
  ASSERT_TRUE(rtsp_stream::launch_session_raise(after_started));
  rtsp_stream::terminate_sessions();

  auto cancelled = std::make_shared<rtsp_stream::launch_session_t>();
  cancelled->id = 3128;
  cancelled->unique_id = "cancelled-expiry";
  ASSERT_TRUE(rtsp_stream::launch_session_raise(cancelled));
  const auto cancelled_generation = rtsp_stream::launch_timer_generation_for_tests();
  cancelled->cancel();
  EXPECT_FALSE(rtsp_stream::expire_pending_launch_for_tests(cancelled->id, cancelled_generation));

  auto after_cancelled = std::make_shared<rtsp_stream::launch_session_t>();
  after_cancelled->id = 3127;
  EXPECT_TRUE(rtsp_stream::launch_session_raise(after_cancelled));
  rtsp_stream::terminate_sessions();
}

TEST(RtspLaunchHandoffTests, ExplicitCancellationCanBeatClaimedSetupBeforeStart) {
  rtsp_stream::launch_session_t session {};
  EXPECT_TRUE(session.try_begin_setup_handoff());
  session.cancel();
  EXPECT_FALSE(session.commit_setup_start());
  EXPECT_TRUE(session.is_cancelled());
}

TEST(SessionRoleContractTests, MixedViewerThenControllerAggregatesToController) {
  auto role = rtsp_stream::merge_session_role_for_tests(session_role_e::none, true);
  role = rtsp_stream::merge_session_role_for_tests(role, false);
  EXPECT_EQ(role, session_role_e::controller);
}

TEST(SessionRoleContractTests, MixedControllerThenViewerRemainsController) {
  auto role = rtsp_stream::merge_session_role_for_tests(session_role_e::none, false);
  role = rtsp_stream::merge_session_role_for_tests(role, true);
  EXPECT_EQ(role, session_role_e::controller);
}

TEST(SessionRoleContractTests, ViewerOnlySessionsAggregateToViewer) {
  auto role = rtsp_stream::merge_session_role_for_tests(session_role_e::none, true);
  role = rtsp_stream::merge_session_role_for_tests(role, true);
  EXPECT_EQ(role, session_role_e::viewer);
}


TEST(SessionLifecycleGateTests, StopWaitsForAdmittedLaunchAndBlocksPendingReplacement) {
  using namespace std::chrono_literals;

  proc::session_lifecycle_gate_t gate;
  gate.begin_launch();

  std::promise<void> stop_started;
  auto stop_acquired = std::async(std::launch::async, [&]() {
    stop_started.set_value();
    return gate.begin_stop();
  });
  stop_started.get_future().wait();

  EXPECT_EQ(stop_acquired.wait_for(20ms), std::future_status::timeout);
  gate.finish_launch();
  ASSERT_EQ(stop_acquired.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(stop_acquired.get());

  EXPECT_TRUE(gate.stop_in_progress());
  EXPECT_FALSE(gate.begin_stop());
  EXPECT_FALSE(gate.try_begin_rtsp_launch());

  gate.finish_stop();
  EXPECT_FALSE(gate.stop_in_progress());
  EXPECT_TRUE(gate.try_begin_rtsp_launch());
  gate.finish_launch();
}

TEST(SessionLifecycleGateTests, CompletedStopInvalidatesLaunchGenerationCapturedBeforeTeardown) {
  proc::session_lifecycle_gate_t gate;
  const auto stale_generation = gate.capture_launch_generation();
  ASSERT_TRUE(stale_generation.has_value());

  ASSERT_TRUE(gate.begin_stop());
  gate.finish_stop();

  EXPECT_FALSE(gate.try_begin_rtsp_launch(*stale_generation));

  const auto fresh_generation = gate.capture_launch_generation();
  ASSERT_TRUE(fresh_generation.has_value());
  EXPECT_NE(*fresh_generation, *stale_generation);
  EXPECT_TRUE(gate.try_begin_rtsp_launch(*fresh_generation));
  gate.finish_launch();
}

TEST(SessionLifecycleGateTests, RequestCapturedDuringStopCannotBecomeLaunchableAfterStopCompletes) {
  proc::session_lifecycle_gate_t gate;
  ASSERT_TRUE(gate.begin_stop());

  const auto during_stop = gate.capture_launch_generation();
  EXPECT_FALSE(during_stop.has_value());

  gate.finish_stop();
  EXPECT_FALSE(gate.try_begin_rtsp_launch(during_stop.value_or(0)));
}

TEST(SessionLifecycleGateTests, SnapshotStateBlocksInsteadOfRejectingConcurrentLaunchCapture) {
  proc::session_lifecycle_gate_t gate;
  gate.begin_snapshot();
  auto capture = std::async(std::launch::async, [&]() {
    return gate.capture_launch_generation();
  });

  EXPECT_EQ(capture.wait_for(20ms), std::future_status::timeout);
  EXPECT_FALSE(gate.stop_in_progress());
  gate.finish_snapshot();
  const auto generation = capture.get();
  ASSERT_TRUE(generation.has_value());
  EXPECT_TRUE(gate.try_begin_rtsp_launch(*generation));
  gate.finish_launch();
}
TEST(SessionLifecycleGateTests, StatusLatchesObservedStopAcrossProcessMutexDelay) {
  auto env = boost::this_process::environment();
  proc::proc_t subject {std::move(env), {}};
  std::promise<void> mutex_acquired;
  std::promise<void> release_mutex;
  const auto release_future = release_mutex.get_future().share();
  auto holder = std::async(std::launch::async, [&]() {
    subject.with_session_lifecycle_lock_for_tests([&]() {
      mutex_acquired.set_value();
      release_future.wait();
    });
  });
  mutex_acquired.get_future().wait();

  ASSERT_TRUE(subject.begin_session_stop_for_tests());
  auto status = std::async(std::launch::async, [&]() {
    return subject.get_session_status_view("status-client", true).snapshot.stop.stop_in_progress;
  });
  EXPECT_EQ(status.wait_for(20ms), std::future_status::timeout);

  subject.finish_session_stop_for_tests(true);
  release_mutex.set_value();
  holder.get();

  ASSERT_EQ(status.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(status.get());
}

TEST(SessionLifecycleGateTests, StatusSnapshotCanObserveActiveTeardownWithoutWaiting) {
  proc::session_lifecycle_gate_t gate;
  ASSERT_TRUE(gate.begin_stop());
  auto guard = proc::session_snapshot_guard_t::try_acquire(gate);
  EXPECT_FALSE(guard.owns_snapshot());
  EXPECT_TRUE(gate.stop_in_progress());
  gate.finish_stop(false);
}

TEST(SessionLifecycleGateTests, StatusSnapshotObservesStopQueuedBehindAdmittedLaunch) {
  proc::session_lifecycle_gate_t gate;
  gate.begin_launch();
  auto status = std::async(std::launch::async, [&]() {
    return proc::session_snapshot_guard_t::try_acquire(gate);
  });
  EXPECT_EQ(status.wait_for(20ms), std::future_status::timeout);

  auto stop = std::async(std::launch::async, [&]() {
    const bool admitted = gate.begin_stop();
    if (admitted) {
      gate.finish_stop(false);
    }
    return admitted;
  });
  while (!gate.stop_in_progress()) {
    std::this_thread::yield();
  }

  EXPECT_EQ(status.wait_for(20ms), std::future_status::ready);
  if (status.wait_for(0ms) == std::future_status::ready) {
    EXPECT_FALSE(status.get().owns_snapshot());
  }

  gate.finish_launch();
  EXPECT_TRUE(stop.get());
}

TEST(SessionLifecycleGateTests, StopWaitsForSnapshotStateBeforeTeardown) {
  proc::session_lifecycle_gate_t gate;
  gate.begin_snapshot();
  std::promise<void> stop_started;

  auto stop = std::async(std::launch::async, [&]() {
    stop_started.set_value();
    return gate.begin_stop();
  });
  stop_started.get_future().wait();
  EXPECT_EQ(stop.wait_for(20ms), std::future_status::timeout);
  EXPECT_FALSE(gate.stop_in_progress());
  gate.finish_snapshot();
  EXPECT_TRUE(stop.get());
  gate.finish_stop();
}

TEST(SessionLifecycleGateTests, ProcessStopSnapshotDoesNotReenterLifecycleGate) {
  rtsp_stream::terminate_sessions();
  auto snapshot = std::async(std::launch::async, []() {
    return proc::proc.get_session_stop_snapshot("snapshot-reader", true);
  });
  ASSERT_EQ(snapshot.wait_for(1s), std::future_status::ready);
  EXPECT_FALSE(snapshot.get().stop_in_progress);
}

TEST(SessionLifecycleGateTests, ReturnedStatusViewKeepsSnapshotAdmissionUntilDestroyed) {
  rtsp_stream::terminate_sessions();
  std::optional<proc::session_status_view_t> status_view;
  status_view.emplace(proc::proc.get_session_status_view("status-view-client", true));

  auto launch_capture = std::async(std::launch::async, []() {
    return proc::proc.capture_session_launch_generation();
  });
  EXPECT_EQ(launch_capture.wait_for(20ms), std::future_status::timeout);

  status_view.reset();
  ASSERT_EQ(launch_capture.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(launch_capture.get().has_value());
}

TEST(SessionLifecycleGateTests, AdmittedTerminateTransitionsDirectlyToStopWithoutIdleWindow) {
  proc::session_lifecycle_gate_t gate;
  const auto generation = gate.capture_launch_generation();
  ASSERT_TRUE(generation.has_value());
  ASSERT_TRUE(gate.try_begin_rtsp_launch(*generation));

  EXPECT_TRUE(gate.transition_launch_to_stop());
  EXPECT_TRUE(gate.stop_in_progress());
  EXPECT_FALSE(gate.try_begin_rtsp_launch());

  gate.finish_stop();
  EXPECT_FALSE(gate.stop_in_progress());
}

TEST(SessionLifecycleGateTests, AdmittedTerminateDefersToAlreadyWaitingStopWithoutAnIdleAdmissionWindow) {
  proc::session_lifecycle_gate_t gate;
  ASSERT_TRUE(gate.try_begin_rtsp_launch());

  auto stop = std::async(std::launch::async, [&]() {
    const bool admitted = gate.begin_stop();
    if (admitted) {
      gate.finish_stop();
    }
    return admitted;
  });
  while (!gate.stop_in_progress()) {
    std::this_thread::yield();
  }

  EXPECT_FALSE(gate.transition_launch_to_stop());
  EXPECT_TRUE(stop.get());
  EXPECT_FALSE(gate.stop_in_progress());
  const auto next = gate.capture_launch_generation();
  ASSERT_TRUE(next.has_value());
  EXPECT_TRUE(gate.try_begin_rtsp_launch(*next));
  gate.finish_launch();
}

TEST(SessionLifecycleGateTests, AdmittedTerminateTakesOverWhenWaitingStopIsRejected) {
  proc::session_lifecycle_gate_t gate;
  ASSERT_TRUE(gate.try_begin_rtsp_launch());

  std::promise<void> stop_admitted;
  std::promise<void> allow_rejected_stop_to_finish;
  auto stop = std::async(std::launch::async, [&]() {
    const bool admitted = gate.begin_stop();
    if (admitted) {
      stop_admitted.set_value();
      allow_rejected_stop_to_finish.get_future().wait();
      gate.finish_stop(false);
    }
    return admitted;
  });
  while (!gate.stop_in_progress()) {
    std::this_thread::yield();
  }

  auto terminate_handoff = std::async(std::launch::async, [&]() {
    return gate.transition_launch_to_stop();
  });
  stop_admitted.get_future().wait();

  auto competing_launch = std::async(std::launch::async, [&]() {
    gate.begin_launch();
    return true;
  });
  EXPECT_EQ(competing_launch.wait_for(20ms), std::future_status::timeout);

  allow_rejected_stop_to_finish.set_value();
  EXPECT_EQ(competing_launch.wait_for(20ms), std::future_status::timeout);
  ASSERT_EQ(terminate_handoff.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(terminate_handoff.get());
  EXPECT_TRUE(stop.get());
  EXPECT_TRUE(gate.stop_in_progress());

  gate.finish_stop(true);
  ASSERT_EQ(competing_launch.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(competing_launch.get());
  gate.finish_launch();
}

TEST(SessionLifecycleGateTests, TeardownCountsAndClearsPendingRtspLaunch) {
  rtsp_stream::terminate_sessions();

  auto pending = std::make_shared<rtsp_stream::launch_session_t>();
  pending->id = 3132;
  pending->unique_id = "pending-controller";
  pending->watch_only = false;
  rtsp_stream::launch_session_raise(pending);

  const auto before = rtsp_stream::session_snapshot(pending->unique_id);
  EXPECT_EQ(before.active_sessions, 0);
  EXPECT_EQ(before.pending_sessions, 1);
  EXPECT_EQ(before.requester_role, session_role_e::controller);

  rtsp_stream::terminate_sessions();

  const auto after = rtsp_stream::session_snapshot(pending->unique_id);
  EXPECT_EQ(after.active_sessions, 0);
  EXPECT_EQ(after.pending_sessions, 0);
  EXPECT_EQ(after.requester_role, session_role_e::none);
}

TEST(SessionLifecycleGateTests, PendingRtspLaunchBlocksReplacementAdmission) {
  rtsp_stream::terminate_sessions();

  auto pending = std::make_shared<rtsp_stream::launch_session_t>();
  pending->id = 3133;
  pending->unique_id = "first-pending-controller";
  rtsp_stream::launch_session_raise(pending);

  const auto generation = proc::proc.capture_session_launch_generation();
  ASSERT_TRUE(generation.has_value());
  EXPECT_FALSE(proc::proc.try_begin_session_launch(*generation));

  rtsp_stream::terminate_sessions();
}

TEST(SessionLifecycleGateTests, ConditionalEventPopCannotConsumeReplacementLaunch) {
  safe::event_t<std::shared_ptr<rtsp_stream::launch_session_t>> event;
  auto original = std::make_shared<rtsp_stream::launch_session_t>();
  original->id = 3134;
  auto replacement = std::make_shared<rtsp_stream::launch_session_t>();
  replacement->id = 3135;

  EXPECT_TRUE(event.raise_if_empty(replacement));
  EXPECT_FALSE(event.raise_if_empty(original));
  EXPECT_FALSE(event.pop_if([&](const auto &pending) {
    return pending && pending->id == original->id;
  }));

  const auto preserved = event.pop_if([&](const auto &pending) {
    return pending && pending->id == replacement->id;
  });
  ASSERT_TRUE(preserved);
  EXPECT_EQ(preserved->id, replacement->id);
}

TEST(SessionLifecycleGateTests, StaleTimerGenerationCannotCancelReplacementWithReusedId) {
  rtsp_stream::terminate_sessions();

  auto original = std::make_shared<rtsp_stream::launch_session_t>();
  original->id = 3136;
  original->unique_id = "stale-generation-original";
  ASSERT_TRUE(rtsp_stream::launch_session_raise(original));
  const auto stale_generation = rtsp_stream::launch_timer_generation_for_tests();

  rtsp_stream::terminate_sessions();
  auto replacement = std::make_shared<rtsp_stream::launch_session_t>();
  replacement->id = original->id;
  replacement->unique_id = "same-id-replacement";
  ASSERT_TRUE(rtsp_stream::launch_session_raise(replacement));
  ASSERT_NE(rtsp_stream::launch_timer_generation_for_tests(), stale_generation);

  EXPECT_FALSE(rtsp_stream::expire_pending_launch_for_tests(original->id, stale_generation));
  const auto preserved = rtsp_stream::session_snapshot(replacement->unique_id);
  EXPECT_EQ(preserved.pending_sessions, 1);
  EXPECT_EQ(preserved.requester_role, session_role_e::controller);

  rtsp_stream::terminate_sessions();
}

TEST(SessionLifecycleGateTests, DuplicatePendingRaiseIsRejectedWithoutReplacingOriginal) {
  rtsp_stream::terminate_sessions();
  auto original = std::make_shared<rtsp_stream::launch_session_t>();
  original->id = 3136;
  original->unique_id = "original-pending";
  auto replacement = std::make_shared<rtsp_stream::launch_session_t>();
  replacement->id = 3137;
  replacement->unique_id = "replacement-pending";

  EXPECT_TRUE(rtsp_stream::launch_session_raise(original));
  EXPECT_FALSE(rtsp_stream::launch_session_raise(replacement));
  const auto original_snapshot = rtsp_stream::session_snapshot(original->unique_id);
  const auto replacement_snapshot = rtsp_stream::session_snapshot(replacement->unique_id);
  EXPECT_EQ(original_snapshot.pending_sessions, 1);
  EXPECT_EQ(original_snapshot.requester_role, rtsp_stream::session_role_e::controller);
  EXPECT_EQ(replacement_snapshot.requester_role, rtsp_stream::session_role_e::none);
  rtsp_stream::terminate_sessions();
}
