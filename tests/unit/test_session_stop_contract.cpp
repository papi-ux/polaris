/**
 * @file tests/unit/test_session_stop_contract.cpp
 * @brief Behavioral HTTP and RTSP-role contracts for the Polaris v1 session stop endpoint.
 */

#include <src/nvhttp.h>
#include <src/process.h>
#include <src/rtsp.h>
#include <src/platform/linux/stream_runtime.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <future>
#include <fcntl.h>
#include <poll.h>
#include <sstream>
#include <thread>
#include <signal.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

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

  std::string read_source_for_contract(const char *relative_path) {
    const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / relative_path;
    std::ifstream in(path);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  std::string read_rtsp_source_for_contract() {
    return read_source_for_contract("src/rtsp.cpp");
  }

  std::string read_nvhttp_source_for_contract() {
    return read_source_for_contract("src/nvhttp.cpp");
  }

  std::string read_portal_grab_source_for_contract() {
    return read_source_for_contract("src/platform/linux/portal_grab.cpp");
  }
}

TEST(SessionBitrateContract, HardCeilingAppliesAfterWarpExpansion) {
  EXPECT_EQ(rtsp_stream::bound_session_bitrate_for_tests(40000, 1, 40000), 40000);
  EXPECT_EQ(rtsp_stream::bound_session_bitrate_for_tests(40000, 2, 40000), 40000);
  EXPECT_EQ(rtsp_stream::bound_session_bitrate_for_tests(40000, 4, 40000), 40000);
  EXPECT_EQ(rtsp_stream::bound_session_bitrate_for_tests(10000, 4, 50000), 40000);
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

TEST(SessionStopContractTests, LiveControllerCanStopEvenIfLaunchOwnerUuidDrifted) {
  // SB-4: quit was 470 "another client" while stream still died. Controller of
  // the live RTSP session may stop even when launch-owner unique_id drifted.
  EXPECT_EQ(decide(true, true, 1, false, session_role_e::controller), session_stop_outcome_t::allowed);
}

TEST(SessionStopContractTests, TerminateSessionsUsesGracefulStopBeforeJoin) {
  // SB-2: hard session::stop without control terminate causes client RST.
  // clear()/terminate_sessions must graceful_stop every live slot first.
  const auto source = read_rtsp_source_for_contract();
  ASSERT_FALSE(source.empty());
  const auto clear_start = source.find("void clear(bool all = true)");
  ASSERT_NE(clear_start, std::string::npos);
  const auto clear_end = source.find("void remove(", clear_start);
  ASSERT_NE(clear_end, std::string::npos);
  const auto clear_body = source.substr(clear_start, clear_end - clear_start);
  const auto graceful = clear_body.find("stream::session::graceful_stop(");
  const auto join = clear_body.find("stream::session::join(");
  EXPECT_NE(graceful, std::string::npos);
  ASSERT_NE(join, std::string::npos);
  EXPECT_LT(graceful, join);
  // Hard stop alone is insufficient for host-initiated cancel/disconnect.
  EXPECT_EQ(clear_body.find("stream::session::stop("), std::string::npos);
}

TEST(SessionStopContractTests, StreamJoinCancelsOwnPendingMediaStartBeforeVideoJoin) {
  const auto source = read_source_for_contract("src/stream.cpp");
  const auto start = source.find("void join(session_t &session)");
  const auto end = source.find("int start(session_t &session", start);
  ASSERT_NE(start, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  const auto body = source.substr(start, end - start);
  const auto cancel = body.find("session_media::cancel_pending_starts(&session)");
  const auto join = body.find("session.videoThread.join()", cancel);
  ASSERT_NE(cancel, std::string::npos);
  ASSERT_NE(join, std::string::npos);
  EXPECT_LT(cancel, join);
  EXPECT_EQ(body.find("session_media::begin_teardown()"), std::string::npos);
  EXPECT_EQ(body.find("session_media::prepare_for_stop()"), std::string::npos);
}

TEST(SessionStopContractTests, VideoCaptureTagsPortalStartOwner) {
  const auto source = read_source_for_contract("src/video.cpp");
  const auto start = source.find("encode_e encode_run_sync(");
  const auto end = source.find("void captureThreadSync()", start);
  ASSERT_NE(start, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  const auto body = source.substr(start, end - start);
  const auto scope = body.find("pending_start_owner_scope_t");
  const auto reset = body.find("reset_display(", scope);
  ASSERT_NE(scope, std::string::npos);
  ASSERT_NE(reset, std::string::npos);
  EXPECT_LT(scope, reset);
}

TEST(SessionStopContractTests, PortalCancellationIsGuardedWhenBackendIsUnavailable) {
  const auto source = read_source_for_contract("src/platform/linux/session_media.cpp");
  ASSERT_FALSE(source.empty());

  const auto include = source.find("#include \"src/platform/linux/portal_session.h\"");
  ASSERT_NE(include, std::string::npos);
  const auto include_guard = source.rfind("#ifdef POLARIS_BUILD_PORTAL", include);
  const auto include_guard_end = source.find("#endif", include_guard);
  ASSERT_NE(include_guard, std::string::npos)
    << "portal_session.h requires GIO and must only be included when the portal backend is built";
  ASSERT_NE(include_guard_end, std::string::npos);
  EXPECT_LT(include_guard, include);
  EXPECT_LT(include, include_guard_end);

  const auto owner_cancel = source.find("portal::cancel_pending_requests(owner_tag)");
  ASSERT_NE(owner_cancel, std::string::npos);
  const auto owner_guard = source.rfind("#ifdef POLARIS_BUILD_PORTAL", owner_cancel);
  const auto owner_guard_end = source.find("#endif", owner_guard);
  ASSERT_NE(owner_guard, std::string::npos);
  ASSERT_NE(owner_guard_end, std::string::npos);
  EXPECT_LT(owner_guard, owner_cancel);
  EXPECT_LT(owner_cancel, owner_guard_end);

  const auto teardown_cancel = source.find("portal::cancel_pending_requests()", owner_cancel + 1);
  ASSERT_NE(teardown_cancel, std::string::npos);
  const auto teardown_guard = source.rfind("#ifdef POLARIS_BUILD_PORTAL", teardown_cancel);
  const auto teardown_guard_end = source.find("#endif", teardown_guard);
  ASSERT_NE(teardown_guard, std::string::npos);
  ASSERT_NE(teardown_guard_end, std::string::npos);
  EXPECT_LT(teardown_guard, teardown_cancel);
  EXPECT_LT(teardown_cancel, teardown_guard_end);
  EXPECT_EQ(source.find("portal::cancel_pending_requests", teardown_cancel + 1), std::string::npos)
    << "every optional portal cancellation call must be feature guarded";
}

TEST(SessionStopContractTests, CancelAnswersClientBeforeNestedTeardown) {
  // SB-2 residual: nested gamescope undo can SEGV mid-cancel; Moonlight must
  // already have cancel=1 or it maps the failed HTTP as "another device".
  const auto source = read_nvhttp_source_for_contract();
  ASSERT_FALSE(source.empty());
  const auto cancel_start = source.find("void cancel(resp_https_t response, req_https_t request)");
  ASSERT_NE(cancel_start, std::string::npos);
  const auto cancel_end = source.find("void appasset(", cancel_start);
  ASSERT_NE(cancel_end, std::string::npos);
  const auto body = source.substr(cancel_start, cancel_end - cancel_start);
  const auto preflight = body.find("get_session_stop_snapshot(");
  const auto respond = body.find("response_written = true");
  const auto shutdown = body.find("request_session_shutdown(");
  EXPECT_NE(preflight, std::string::npos);
  ASSERT_NE(respond, std::string::npos);
  ASSERT_NE(shutdown, std::string::npos);
  // Preflight + respond before the teardown shutdown call.
  EXPECT_LT(preflight, respond);
  EXPECT_LT(respond, shutdown);
  EXPECT_NE(body.find("is_session_owner("), std::string::npos);
}

TEST(SessionStopContractTests, PortalPipeWireTeardownDisconnectsUnderLoopLock) {
  // SB-2: destroy without disconnect under lock races state_changed → SEGV.
  // Dtor lives in pipewire_capture (extracted from portal_grab).
  const auto source = read_source_for_contract("src/platform/linux/pipewire_capture.cpp");
  ASSERT_FALSE(source.empty());
  const auto dtor = source.find("capture_t::~capture_t()");
  ASSERT_NE(dtor, std::string::npos);
  const auto dtor_end = source.find("bool capture_t::start()", dtor);
  ASSERT_NE(dtor_end, std::string::npos);
  const auto body = source.substr(dtor, dtor_end - dtor);
  const auto stop = body.find("pw_thread_loop_stop(");
  const auto lock = body.find("pw_thread_loop_lock(");
  const auto disconnect = body.find("pw_stream_disconnect(");
  const auto destroy = body.find("pw_stream_destroy(");
  EXPECT_NE(stop, std::string::npos);
  ASSERT_NE(lock, std::string::npos);
  ASSERT_NE(disconnect, std::string::npos);
  ASSERT_NE(destroy, std::string::npos);
  EXPECT_LT(lock, disconnect);
  EXPECT_LT(disconnect, destroy);
  EXPECT_LT(destroy, stop);
}

TEST(SessionStopContractTests, PipeWireReconnectRetiresOldGenerationBeforePublishingReplacement) {
  const auto portal = read_portal_grab_source_for_contract();
  const auto pipewire = read_source_for_contract("src/platform/linux/pipewire_capture.cpp");
  ASSERT_FALSE(portal.empty());
  ASSERT_FALSE(pipewire.empty());

  // Signature may be split across lines; match the declarator only.
  const auto ensure = portal.find("ensure_global_capture(");
  const auto ensure_end = portal.find("// -----------------------------------------------------------------------", ensure);
  ASSERT_NE(ensure, std::string::npos);
  ASSERT_NE(ensure_end, std::string::npos);
  const auto body = portal.substr(ensure, ensure_end - ensure);
  const auto retire = body.find("retired_capture = std::move(g_media.capture)");
  const auto shutdown = body.find("retired_capture->shutdown()", retire);
  const auto replacement = body.find("g_media.capture = std::move(local)", shutdown);
  EXPECT_NE(body.find("g_capture_transition_mu"), std::string::npos);
  ASSERT_NE(retire, std::string::npos);
  ASSERT_NE(shutdown, std::string::npos);
  ASSERT_NE(replacement, std::string::npos);
  EXPECT_LT(retire, shutdown);
  EXPECT_LT(shutdown, replacement);

  EXPECT_NE(pipewire.find("void capture_t::shutdown()"), std::string::npos);
  EXPECT_NE(pipewire.find("active_dmabuf_leases_ == 0"), std::string::npos);
}

TEST(SessionStopContractTests, PipeWireDmaBufIdentityTracksBufferAllocationLifecycle) {
  const auto source = read_source_for_contract("src/platform/linux/pipewire_capture.cpp");
  ASSERT_FALSE(source.empty());
  EXPECT_NE(source.find(".add_buffer = capture_t::on_add_buffer"), std::string::npos);
  EXPECT_NE(source.find(".remove_buffer = capture_t::on_remove_buffer"), std::string::npos);
  EXPECT_NE(source.find("allocate_buffer_key()"), std::string::npos);
  EXPECT_NE(source.find("descriptor->dmabuf_buffer_key = front_dmabuf_buffer_key_"), std::string::npos);
  EXPECT_EQ(source.find("reinterpret_cast<std::uintptr_t>(front_dmabuf_buffer_->buffer)"), std::string::npos);
  EXPECT_NE(source.find("front_info_.spa_format != raw_info.format"), std::string::npos);
  EXPECT_NE(source.find("front_info_.modifier != negotiated_modifier"), std::string::npos);
}

TEST(SessionStopContractTests, PortalReleaseRunsBeforeNestedCompositorKill) {
  // terminate_impl must run session_media (portal release inside) before Steam/cage kill.
  const auto source = read_source_for_contract("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto start = source.find("void proc_t::terminate_impl(");
  ASSERT_NE(start, std::string::npos);
  const auto body = source.substr(start, 5000);
  const auto prepare = body.find("session_media::prepare_for_stop(");
  const auto steam = body.find("terminate_session_owned_steam_before_cage_stop(");
  const auto gen = body.find("terminate_isolated_session_generation(");
  ASSERT_NE(prepare, std::string::npos);
  ASSERT_NE(steam, std::string::npos);
  ASSERT_NE(gen, std::string::npos);
  EXPECT_LT(prepare, steam);
  EXPECT_LT(prepare, gen);
  // Double portal release is forbidden — only session_media may release.
  EXPECT_EQ(body.find("portal::release_global_capture("), std::string::npos);
}

TEST(SessionStopContractTests, UnclassifiedNestedLaunchHasNoNumericGroupKillFallback) {
  const auto source = read_source_for_contract("nix/modules/polaris-gamescope-session.sh");
  ASSERT_FALSE(source.empty());
  EXPECT_EQ(source.find("kill -TERM \"-$nested_launch_pid\""), std::string::npos);
  const auto claim = source.find("      publish_nested_claim", source.find("start)"));
  const auto launch = source.find("setsid env -u WAYLAND_DISPLAY");
  ASSERT_NE(claim, std::string::npos);
  ASSERT_NE(launch, std::string::npos);
  EXPECT_LT(claim, launch);
  const auto recover = source.find("\"$0\" stop");
  ASSERT_NE(recover, std::string::npos);
}

TEST(SessionStopContractTests, WrappedGamescopeIsFrozenBeforeXwaylandGroupTeardown) {
  const auto source = read_source_for_contract("nix/modules/polaris-gamescope-runtime-lib.sh");
  ASSERT_FALSE(source.empty());
  const auto stop_start = source.find("polaris_stop_marked_gamescope() (");
  ASSERT_NE(stop_start, std::string::npos);
  const auto body = source.substr(stop_start);
  const auto freeze_group = body.find("\"$kill_bin\" -STOP \"-$pgid\"");
  const auto leader_stopped = body.find("polaris_wait_group_leader_stopped", freeze_group);
  const auto compositor_stopped = body.find("polaris_wait_gamescope_stopped", leader_stopped);
  const auto all_original_stopped = body.find("polaris_wait_process_group_stopped", compositor_stopped);
  const auto arm_destructive = body.find("destructive_signal_armed=1", all_original_stopped);
  const auto drain_siblings = body.find("polaris_kill_private_session_groups \"$pgid\"", arm_destructive);
  const auto final_sibling_scan = body.find("polaris_private_session_has_no_live_siblings", drain_siblings);
  const auto kill_group = body.find("\"$kill_bin\" -KILL \"-$pgid\"", drain_siblings);
  const auto clear_marker = body.rfind("rm -f \"$marker\"");
  ASSERT_NE(freeze_group, std::string::npos);
  ASSERT_NE(leader_stopped, std::string::npos);
  ASSERT_NE(compositor_stopped, std::string::npos);
  ASSERT_NE(all_original_stopped, std::string::npos);
  ASSERT_NE(arm_destructive, std::string::npos);
  ASSERT_NE(drain_siblings, std::string::npos);
  ASSERT_NE(final_sibling_scan, std::string::npos);
  ASSERT_NE(kill_group, std::string::npos);
  ASSERT_NE(clear_marker, std::string::npos);
  EXPECT_LT(freeze_group, leader_stopped);
  EXPECT_LT(leader_stopped, compositor_stopped);
  EXPECT_LT(compositor_stopped, all_original_stopped);
  EXPECT_LT(all_original_stopped, arm_destructive);
  EXPECT_LT(arm_destructive, drain_siblings);
  EXPECT_LT(drain_siblings, final_sibling_scan);
  EXPECT_LT(final_sibling_scan, kill_group);
  EXPECT_LT(drain_siblings, kill_group);
  EXPECT_LT(kill_group, clear_marker);
  EXPECT_EQ(body.find("-TERM \"-$pgid\""), std::string::npos);
  EXPECT_NE(body.find("[ \"$destructive_signal_armed\" = 0 ]"), std::string::npos);

  const auto sibling_start = source.find("polaris_kill_private_session_groups() {");
  ASSERT_NE(sibling_start, std::string::npos);
  const auto sibling_end = source.find("polaris_stop_marked_gamescope() (", sibling_start);
  ASSERT_NE(sibling_end, std::string::npos);
  const auto sibling_body = source.substr(sibling_start, sibling_end - sibling_start);
  const auto sibling_freeze = sibling_body.find("\"$kill_bin\" -STOP \"-$group\"");
  const auto sibling_stopped = sibling_body.find("polaris_wait_group_leader_stopped", sibling_freeze);
  const auto all_siblings_stopped = sibling_body.find("polaris_wait_process_group_stopped", sibling_stopped);
  const auto sibling_kill = sibling_body.find("\"$kill_bin\" -KILL \"-$group\"", sibling_freeze);
  ASSERT_NE(sibling_freeze, std::string::npos);
  ASSERT_NE(sibling_stopped, std::string::npos);
  ASSERT_NE(all_siblings_stopped, std::string::npos);
  ASSERT_NE(sibling_kill, std::string::npos);
  EXPECT_LT(sibling_freeze, sibling_stopped);
  EXPECT_LT(sibling_stopped, all_siblings_stopped);
  EXPECT_LT(all_siblings_stopped, sibling_kill);
  EXPECT_NE(sibling_body.find("[ \"$POLARIS_PROCESS_STATE\" = Z ] && continue"), std::string::npos);
  EXPECT_NE(sibling_body.find("if [ \"$POLARIS_PROCESS_STATE\" = Z ]; then"), std::string::npos);
  EXPECT_NE(source.find("[ -z \"$executable\" ]"), std::string::npos);
}

TEST(SessionStopContractTests, StartupRecoveryUsesCredentialedStopAndPortalRebind) {
  const auto recovery = read_source_for_contract("nix/modules/session-lib.nix");
  const auto session = read_source_for_contract("nix/modules/polaris-gamescope-session.sh");
  ASSERT_FALSE(recovery.empty());
  ASSERT_FALSE(session.empty());
  EXPECT_NE(recovery.find("${lib.getExe sessionBin} stop"), std::string::npos);
  EXPECT_NE(recovery.find("polaris-gamescope-session-id"), std::string::npos);
  EXPECT_NE(recovery.find("polaris-gamescope-session-state"), std::string::npos);
  EXPECT_NE(recovery.find("POLARIS_GAMESCOPE_LOCK_HELD=1"), std::string::npos);
  EXPECT_NE(recovery.find("claim_state=absent"), std::string::npos);
  EXPECT_NE(session.find("restart polaris-portal-gamescope.service"), std::string::npos);
  EXPECT_NE(session.find("publish_nested_claim transition absent"), std::string::npos);
  EXPECT_NE(session.find("polaris-gamescope-session-mode"), std::string::npos);
  EXPECT_NE(session.find("polaris-gamescope-session-state"), std::string::npos);
  EXPECT_NE(session.find("printf '%s %s %s\\n' \"$POLARIS_SESSION_INSTANCE_ID\" \"$mode\" \"$service_mode\""), std::string::npos);
  EXPECT_NE(session.find("mv -f -- \"$tmp\" \"$session_state_file\""), std::string::npos);
  EXPECT_NE(session.find("runtime services=$service_mode"), std::string::npos);
  EXPECT_NE(session.find("standalone package runtime restored with no idle gamescope"), std::string::npos);
  EXPECT_NE(session.find("prior session recovery failed; retaining its exact claim"), std::string::npos);
  EXPECT_EQ(session.find("forcing clean slate"), std::string::npos);
  EXPECT_NE(session.find("attach recovery could not terminate exact-session Steam"), std::string::npos);
  const auto idle = read_source_for_contract("scripts/install/lib/polaris-gamescope-idle.sh");
  ASSERT_FALSE(idle.empty());
  EXPECT_NE(idle.find("polaris-gamescope.lock"), std::string::npos);
  EXPECT_NE(idle.find("transition|nested|1"), std::string::npos);
  const auto non_nix = read_source_for_contract("scripts/install/lib/polaris-wait-gamescope.sh");
  ASSERT_FALSE(non_nix.empty());
  EXPECT_NE(non_nix.find("POLARIS_GAMESCOPE_SESSION_BIN"), std::string::npos);
  EXPECT_NE(non_nix.find("polaris-gamescope-session-id"), std::string::npos);
  EXPECT_NE(non_nix.find("polaris-gamescope-session-state"), std::string::npos);
}

TEST(SessionStopContractTests, OwnedRuntimeDrainsPrivateGroupBeforeClearingState) {
  const auto source = read_source_for_contract("src/platform/linux/stream_runtime_gamescope.cpp");
  ASSERT_FALSE(source.empty());
  const auto ownership_source = read_source_for_contract("src/platform/linux/gamescope_process.cpp");
  ASSERT_FALSE(ownership_source.empty());
  EXPECT_NE(ownership_source.find("O_PATH | O_CLOEXEC | O_NOFOLLOW"), std::string::npos);
  EXPECT_NE(ownership_source.find("socket_pin.still_names_node(socket_path)"), std::string::npos);
  EXPECT_NE(source.find("private_group_state"), std::string::npos);
  EXPECT_NE(source.find("private group did not drain"), std::string::npos);
  EXPECT_NE(source.find("SIGKILL"), std::string::npos);
  EXPECT_NE(source.find("SYS_pidfd_open"), std::string::npos);
  EXPECT_NE(source.find("pidfd_targets_pid(leader_pidfd, pgid)"), std::string::npos);
  EXPECT_NE(source.find("retained owned generation did not drain; refusing replacement launch"), std::string::npos);
  const auto pair_validation = source.find("revalidate_publication_pair()");
  ASSERT_NE(pair_validation, std::string::npos);
  EXPECT_NE(source.find("revalidate_publication_pair()", pair_validation + 1), std::string::npos);
  EXPECT_NE(source.find("Keep the leader unreaped until every negative-PGID operation finishes"), std::string::npos);
  const auto stop_start = source.find("void stop_unlocked()");
  const auto refresh_start = source.find("void refresh_runtime_state(", stop_start);
  ASSERT_NE(stop_start, std::string::npos);
  ASSERT_NE(refresh_start, std::string::npos);
  const auto stop_body = source.substr(stop_start, refresh_start - stop_start);
  EXPECT_NE(source.find("bool owned_group_drained_ = false"), std::string::npos);

  const auto owned_socket_reclaim_start = source.find(
    "bool reclaim_owned_gamescope_sockets_after_drain_unlocked()"
  );
  ASSERT_NE(owned_socket_reclaim_start, std::string::npos);
  ASSERT_LT(owned_socket_reclaim_start, stop_start);
  const auto owned_socket_reclaim = source.substr(
    owned_socket_reclaim_start,
    stop_start - owned_socket_reclaim_start
  );
  EXPECT_NE(
    owned_socket_reclaim.find("!owned_group_drained_ || socket_name_ != \"gamescope-0\""),
    std::string::npos
  );
  const auto reclaim_wayland = owned_socket_reclaim.find(
    "gp::remove_orphan_socket(socket_path(\"gamescope-0\"))"
  );
  const auto reclaim_ei = owned_socket_reclaim.find(
    "gp::remove_orphan_socket(socket_path(\"gamescope-0-ei\"))"
  );
  ASSERT_NE(reclaim_wayland, std::string::npos);
  ASSERT_NE(reclaim_ei, std::string::npos);
  EXPECT_LT(reclaim_wayland, reclaim_ei);
  EXPECT_EQ(owned_socket_reclaim.find("gamescope-1"), std::string::npos)
    << "post-drain cleanup may reclaim only the exact owned runtime socket pair";

  const auto missing_marker_guard = stop_body.find("if (owned_ && !marker_)");
  const auto missing_marker_return = stop_body.find("return;", missing_marker_guard);
  const auto owned_marker_branch = stop_body.find("if (owned_ && marker_)", missing_marker_return);
  const auto raw_marker = stop_body.find(
    "const auto marker_on_disk = gp::read_marker(marker_path())",
    owned_marker_branch
  );
  const auto retry_gate = stop_body.find("if (!owned_group_drained_)", raw_marker);
  const auto live_marker = stop_body.find("gp::validated_marker(marker_path(), \"runtime\")", retry_gate);
  const auto drain_group = stop_body.find("drain_private_process_group(", live_marker);
  const auto mark_drained = stop_body.find("owned_group_drained_ = true", drain_group);
  const auto reclaim_sockets = stop_body.find(
    "reclaim_owned_gamescope_sockets_after_drain_unlocked()",
    mark_drained
  );
  const auto clear_files = stop_body.find("remove_owned_files_if_current_unlocked()", reclaim_sockets);
  const auto clear_retry_state = stop_body.find("owned_group_drained_ = false", clear_files);
  ASSERT_NE(missing_marker_guard, std::string::npos);
  ASSERT_NE(missing_marker_return, std::string::npos);
  ASSERT_NE(owned_marker_branch, std::string::npos);
  ASSERT_NE(raw_marker, std::string::npos);
  ASSERT_NE(retry_gate, std::string::npos);
  ASSERT_NE(live_marker, std::string::npos);
  ASSERT_NE(drain_group, std::string::npos);
  ASSERT_NE(mark_drained, std::string::npos);
  ASSERT_NE(reclaim_sockets, std::string::npos);
  ASSERT_NE(clear_files, std::string::npos);
  ASSERT_NE(clear_retry_state, std::string::npos);
  EXPECT_LT(missing_marker_guard, missing_marker_return);
  EXPECT_LT(missing_marker_return, owned_marker_branch)
    << "an owned runtime without immutable marker authority must retain its state";
  EXPECT_LT(owned_marker_branch, raw_marker);
  EXPECT_LT(raw_marker, retry_gate)
    << "every retry must revalidate the raw immutable marker before cleanup";
  EXPECT_LT(retry_gate, live_marker);
  EXPECT_LT(live_marker, drain_group);
  EXPECT_LT(drain_group, mark_drained);
  EXPECT_LT(mark_drained, reclaim_sockets);
  EXPECT_LT(reclaim_sockets, clear_files)
    << "socket authority must be consumed before marker/environment authority";
  EXPECT_LT(clear_files, clear_retry_state);
  EXPECT_EQ(stop_body.find("validated_marker_for_socket"), std::string::npos);

  const auto reset_start = source.find("void reset_after_external_stop() override");
  const auto reset_end = source.find("bool is_running() const override", reset_start);
  ASSERT_NE(reset_start, std::string::npos);
  ASSERT_NE(reset_end, std::string::npos);
  const auto reset_body = source.substr(reset_start, reset_end - reset_start);
  EXPECT_NE(reset_body.find("owned_group_drained_ = false"), std::string::npos);

  const auto clear_state_start = source.find("void clear_runtime_state_unlocked()");
  const auto clear_state_end = source.find("bool revalidate_running_unlocked(", clear_state_start);
  ASSERT_NE(clear_state_start, std::string::npos);
  ASSERT_NE(clear_state_end, std::string::npos);
  const auto clear_state_body = source.substr(
    clear_state_start,
    clear_state_end - clear_state_start
  );
  EXPECT_NE(clear_state_body.find("owned_group_drained_ = false"), std::string::npos);

  const auto runtime_start = source.find("bool start(const start_params_t &params) override");
  const auto runtime_stop = source.find("void stop() override", runtime_start);
  ASSERT_NE(runtime_start, std::string::npos);
  ASSERT_NE(runtime_stop, std::string::npos);
  const auto start_body = source.substr(runtime_start, runtime_stop - runtime_start);
  const auto acquisition = start_body.find("owner_transition_lock_t acquisition_lock");
  const auto reclaim = start_body.find("reclaim_orphan_gamescope_sockets()");
  const auto spawn = start_body.find("const pid_t child = fork()");
  const auto marker_write = start_body.find("gp::write_marker(marker_path(), *marker_)");
  ASSERT_NE(acquisition, std::string::npos);
  ASSERT_NE(reclaim, std::string::npos);
  ASSERT_NE(spawn, std::string::npos);
  ASSERT_NE(marker_write, std::string::npos);
  EXPECT_LT(acquisition, reclaim);
  EXPECT_LT(reclaim, spawn);
  EXPECT_LT(spawn, marker_write);
  const auto owned_spawn = start_body.find("owned_ = true", spawn);
  const auto reset_spawn_retry_state = start_body.find("owned_group_drained_ = false", owned_spawn);
  ASSERT_NE(owned_spawn, std::string::npos);
  ASSERT_NE(reset_spawn_retry_state, std::string::npos);
  EXPECT_LT(owned_spawn, reset_spawn_retry_state);
  EXPECT_NE(start_body.find("runtime_acquisition_allowed_locked()"), std::string::npos);
  EXPECT_NE(source.find("try_attach_gamescope0(params, true)"), std::string::npos);

  const auto attach_start = source.find("bool try_attach_gamescope0(");
  const auto attach_end = source.find("static bool idle_hdr_flags_match_force()", attach_start);
  ASSERT_NE(attach_start, std::string::npos);
  ASSERT_NE(attach_end, std::string::npos);
  const auto attach_body = source.substr(attach_start, attach_end - attach_start);
  EXPECT_NE(attach_body.find("owned_group_drained_ = false"), std::string::npos);
  const auto drain_start = source.find("bool drain_private_process_group(");
  const auto rollback_start = source.find("bool rollback_spawned_private_group(", drain_start);
  ASSERT_NE(drain_start, std::string::npos);
  ASSERT_NE(rollback_start, std::string::npos);
  const auto drain = source.substr(drain_start, rollback_start - drain_start);
  EXPECT_EQ(drain.find("waitpid(pgid", drain.find("for (int i = 0")), drain.rfind("waitpid(pgid"));
}

TEST(SessionStopContractTests, RuntimeAcquisitionRejectsNestedOrIncompleteDurableClaims) {
#ifdef __linux__
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() /
    ("polaris-runtime-claim-test-" + std::to_string(getpid()));
  fs::remove_all(dir);
  ASSERT_TRUE(fs::create_directories(dir));
  const char *old_runtime = std::getenv("XDG_RUNTIME_DIR");
  const std::string saved_runtime = old_runtime ? old_runtime : "";
  ASSERT_EQ(setenv("XDG_RUNTIME_DIR", dir.c_str(), 1), 0);

  EXPECT_TRUE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  const int lock_fd = open((dir / "polaris-gamescope.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  ASSERT_GE(lock_fd, 0);
  ASSERT_EQ(flock(lock_fd, LOCK_EX), 0);
  auto blocked_acquisition = std::async(std::launch::async, []() {
    return stream_runtime::gamescope_runtime_acquisition_allowed_for_tests();
  });
  EXPECT_EQ(blocked_acquisition.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  {
    std::ofstream state(dir / "polaris-gamescope-session-state");
    state << "session-A nested\n";
  }
  ASSERT_EQ(flock(lock_fd, LOCK_UN), 0);
  close(lock_fd);
  EXPECT_FALSE(blocked_acquisition.get());
  fs::remove(dir / "polaris-gamescope-session-state");

  {
    std::ofstream state(dir / "polaris-gamescope-session-state");
    state << "session-A nested\n";
  }
  EXPECT_FALSE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  {
    std::ofstream state(dir / "polaris-gamescope-session-state", std::ios::trunc);
    state << "session-A attach\n";
  }
  EXPECT_TRUE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  {
    std::ofstream state(dir / "polaris-gamescope-session-state", std::ios::trunc);
    state << "session-A attach standalone\n";
  }
  EXPECT_TRUE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  {
    std::ofstream state(dir / "polaris-gamescope-session-state", std::ios::trunc);
    state << "session-A attach managed\n";
  }
  EXPECT_TRUE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  {
    std::ofstream state(dir / "polaris-gamescope-session-state", std::ios::trunc);
    state << "session-A attach unknown\n";
  }
  EXPECT_FALSE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  {
    std::ofstream claim(dir / "polaris-gamescope-wsi-nested");
    claim << "transition\n";
  }
  EXPECT_FALSE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  fs::remove(dir / "polaris-gamescope-wsi-nested");
  {
    std::ofstream state(dir / "polaris-gamescope-session-state", std::ios::trunc);
    state << "session-A\n";
  }
  EXPECT_FALSE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  fs::remove(dir / "polaris-gamescope-session-state");
  {
    std::ofstream id(dir / "polaris-gamescope-session-id");
    id << "session-A\n";
  }
  EXPECT_FALSE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  {
    std::ofstream mode(dir / "polaris-gamescope-session-mode");
    mode << "attach\n";
  }
  EXPECT_TRUE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());
  {
    std::ofstream mode(dir / "polaris-gamescope-session-mode", std::ios::trunc);
    mode << "nested\n";
  }
  EXPECT_FALSE(stream_runtime::gamescope_runtime_acquisition_allowed_for_tests());

  if (old_runtime) {
    ASSERT_EQ(setenv("XDG_RUNTIME_DIR", saved_runtime.c_str(), 1), 0);
  }
  else {
    ASSERT_EQ(unsetenv("XDG_RUNTIME_DIR"), 0);
  }
  fs::remove_all(dir);
#endif
}

TEST(SessionStopContractTests, OwnedRuntimeEscalatesTermResistantPrivateGroup) {
#ifdef __linux__
  int ready[2] {-1, -1};
  ASSERT_EQ(pipe(ready), 0);
  const pid_t leader = fork();
  ASSERT_GE(leader, 0);
  if (leader == 0) {
    close(ready[0]);
    if (setsid() < 0) _exit(125);
    signal(SIGTERM, SIG_IGN);
    const pid_t sibling = fork();
    if (sibling < 0) _exit(126);
    if (sibling == 0) {
      signal(SIGTERM, SIG_IGN);
      for (;;) pause();
    }
    (void) write(ready[1], &sibling, sizeof(sibling));
    close(ready[1]);
    for (;;) pause();
  }
  close(ready[1]);
  pid_t sibling = -1;
  ASSERT_EQ(read(ready[0], &sibling, sizeof(sibling)), sizeof(sibling));
  close(ready[0]);
  const bool drained = stream_runtime::drain_gamescope_private_group_for_tests(leader);
  if (!drained) (void) kill(-leader, SIGKILL);
  EXPECT_TRUE(drained);
  errno = 0;
  EXPECT_EQ(kill(sibling, 0), -1);
  EXPECT_EQ(errno, ESRCH);
#endif
}

TEST(SessionStopContractTests, OwnedRuntimeEscalatesAfterLeaderExitsOnTerm) {
#ifdef __linux__
  int ready[2] {-1, -1};
  ASSERT_EQ(pipe(ready), 0);
  const pid_t leader = fork();
  ASSERT_GE(leader, 0);
  if (leader == 0) {
    close(ready[0]);
    if (setsid() < 0) _exit(125);
    const pid_t sibling = fork();
    if (sibling < 0) _exit(126);
    if (sibling == 0) {
      signal(SIGTERM, SIG_IGN);
      for (;;) pause();
    }
    (void) write(ready[1], &sibling, sizeof(sibling));
    close(ready[1]);
    for (;;) pause();
  }
  close(ready[1]);
  pid_t sibling = -1;
  ASSERT_EQ(read(ready[0], &sibling, sizeof(sibling)), sizeof(sibling));
  close(ready[0]);
  const bool drained = stream_runtime::drain_gamescope_private_group_for_tests(leader);
  if (!drained) (void) kill(-leader, SIGKILL);
  EXPECT_TRUE(drained);
  errno = 0;
  EXPECT_EQ(kill(sibling, 0), -1);
  EXPECT_EQ(errno, ESRCH);
#endif
}

TEST(SessionStopContractTests, OwnedRuntimeDoesNotClearSameSessionEscapedGroup) {
#ifdef __linux__
  int ready[2] {-1, -1};
  ASSERT_EQ(pipe(ready), 0);
  const pid_t leader = fork();
  ASSERT_GE(leader, 0);
  if (leader == 0) {
    close(ready[0]);
    if (setsid() < 0) _exit(125);
    signal(SIGTERM, SIG_IGN);
    const pid_t escaped = fork();
    if (escaped < 0) _exit(126);
    if (escaped == 0) {
      if (setpgid(0, 0) < 0) _exit(127);
      signal(SIGTERM, SIG_IGN);
      const pid_t escaped_pid = getpid();
      (void) write(ready[1], &escaped_pid, sizeof(escaped_pid));
      close(ready[1]);
      for (;;) pause();
    }
    close(ready[1]);
    for (;;) pause();
  }
  close(ready[1]);
  pid_t escaped = -1;
  ASSERT_EQ(read(ready[0], &escaped, sizeof(escaped)), sizeof(escaped));
  close(ready[0]);
  const bool drained = stream_runtime::drain_gamescope_private_group_for_tests(leader);
  EXPECT_FALSE(drained);
  EXPECT_EQ(kill(escaped, 0), 0);
  (void) kill(escaped, SIGKILL);
  (void) kill(-leader, SIGKILL);
  (void) waitpid(leader, nullptr, 0);
#endif
}

TEST(SessionStopContractTests, SpawnRollbackDrainsSiblingAfterLeaderExit) {
#ifdef __linux__
  int ready[2] {-1, -1};
  ASSERT_EQ(pipe(ready), 0);
  const pid_t leader = fork();
  ASSERT_GE(leader, 0);
  if (leader == 0) {
    close(ready[0]);
    if (setsid() < 0) _exit(125);
    const pid_t sibling = fork();
    if (sibling < 0) _exit(126);
    if (sibling == 0) {
      signal(SIGTERM, SIG_IGN);
      for (;;) pause();
    }
    (void) write(ready[1], &sibling, sizeof(sibling));
    close(ready[1]);
    _exit(0);
  }
  close(ready[1]);
  pid_t sibling = -1;
  ASSERT_EQ(read(ready[0], &sibling, sizeof(sibling)), sizeof(sibling));
  close(ready[0]);
  const int leader_pidfd = static_cast<int>(syscall(SYS_pidfd_open, leader, 0));
  ASSERT_GE(leader_pidfd, 0);
  pollfd leader_exit {.fd = leader_pidfd, .events = POLLIN, .revents = 0};
  ASSERT_EQ(poll(&leader_exit, 1, 5000), 1);
  const bool drained = stream_runtime::rollback_gamescope_spawn_for_tests(leader, leader_pidfd);
  close(leader_pidfd);
  if (!drained) (void) kill(-leader, SIGKILL);
  EXPECT_TRUE(drained);
  errno = 0;
  EXPECT_EQ(kill(sibling, 0), -1);
  EXPECT_EQ(errno, ESRCH);
#endif
}

TEST(SessionStopContractTests, SpawnRollbackRejectsPidfdForDifferentLeader) {
#ifdef __linux__
  const pid_t first = fork();
  ASSERT_GE(first, 0);
  if (first == 0) {
    if (setsid() < 0) _exit(125);
    signal(SIGTERM, SIG_IGN);
    for (;;) pause();
  }
  const pid_t second = fork();
  ASSERT_GE(second, 0);
  if (second == 0) {
    if (setsid() < 0) _exit(126);
    signal(SIGTERM, SIG_IGN);
    for (;;) pause();
  }
  const auto became_private_group_leader = [](pid_t pid) {
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (getpgid(pid) == pid && getsid(pid) == pid) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  };
  ASSERT_TRUE(became_private_group_leader(first));
  ASSERT_TRUE(became_private_group_leader(second));
  const int second_pidfd = static_cast<int>(syscall(SYS_pidfd_open, second, 0));
  ASSERT_GE(second_pidfd, 0);
  EXPECT_FALSE(stream_runtime::rollback_gamescope_spawn_for_tests(first, second_pidfd));
  close(second_pidfd);
  EXPECT_EQ(kill(first, 0), 0);
  EXPECT_EQ(kill(second, 0), 0);
  (void) kill(-first, SIGKILL);
  (void) kill(-second, SIGKILL);
  EXPECT_EQ(waitpid(first, nullptr, 0), first);
  EXPECT_EQ(waitpid(second, nullptr, 0), second);
#endif
}

TEST(SessionStopContractTests, StreamingWillStopReleasesPortalCapture) {
  const auto source = read_source_for_contract("src/platform/linux/misc.cpp");
  ASSERT_FALSE(source.empty());
  const auto start = source.find("void streaming_will_stop()");
  ASSERT_NE(start, std::string::npos);
  const auto body = source.substr(start, 400);
  EXPECT_NE(body.find("portal::release_global_capture("), std::string::npos);
}

TEST(SessionStopContractTests, BrowserStreamStopJoinsCaptureBeforeAppTerminate) {
  // stop_session must sync-join capture before terminate when release_media is true.
  const auto source = read_source_for_contract("src/browser_stream.cpp");
  ASSERT_FALSE(source.empty());
  const auto stop_start = source.find("stop_session_result_t stop_session(");
  ASSERT_NE(stop_start, std::string::npos);
  const auto stop_end = source.find("nlohmann::json status_json()", stop_start);
  ASSERT_NE(stop_end, std::string::npos);
  const auto body = source.substr(stop_start, stop_end - stop_start);
  const auto prepare = body.find("prepare_for_session_teardown(");
  const auto terminate = body.find("proc::proc.terminate(");
  ASSERT_NE(prepare, std::string::npos);
  ASSERT_NE(terminate, std::string::npos);
  EXPECT_LT(prepare, terminate);
  EXPECT_EQ(body.find("stop_video_capture_async("), std::string::npos);
  EXPECT_NE(body.find("terminate_owned_app"), std::string::npos);
}

TEST(SessionStopContractTests, PrepareForSessionTeardownReleasesPortalBeforeJoin) {
  // Order: take state → release portal → bounded join.
  const auto source = read_source_for_contract("src/browser_stream.cpp");
  ASSERT_FALSE(source.empty());
  const auto start = source.find("void prepare_for_session_teardown()");
  ASSERT_NE(start, std::string::npos);
  const auto body = source.substr(start, 1200);
  const auto take = body.find("take_capture_state_for_stop(");
  const auto release = body.find("release_global_capture(");
  const auto join = body.find("finish_video_capture_stop(");
  ASSERT_NE(take, std::string::npos);
  ASSERT_NE(release, std::string::npos);
  ASSERT_NE(join, std::string::npos);
  EXPECT_LT(take, release);
  EXPECT_LT(release, join);
  EXPECT_EQ(body.find("stop_video_capture_async("), std::string::npos);
  EXPECT_NE(body.find("3s"), std::string::npos);
}

TEST(SessionStopContractTests, PortalGlobalCaptureUsesSharedOwnership) {
  // S4: single media_cache_t owns shared_ptr capture under one mutex so
  // release_global_capture cannot UAF negotiate/capture waiters.
  const auto source = read_portal_grab_source_for_contract();
  ASSERT_FALSE(source.empty());
  EXPECT_NE(source.find("struct media_cache_t"), std::string::npos);
  EXPECT_NE(source.find("std::shared_ptr<pipewire_capture::capture_t> capture"), std::string::npos);
  EXPECT_NE(source.find("g_media_mu"), std::string::npos);
  EXPECT_EQ(source.find("g_portal_mu"), std::string::npos);
  EXPECT_EQ(source.find("g_capture_mtx"), std::string::npos);
  EXPECT_NE(source.find("ensure_global_capture"), std::string::npos);
  const auto release = source.find("void release_global_capture()");
  ASSERT_NE(release, std::string::npos);
  const auto release_body = source.substr(release, 1200);
  // Public stop() wakes waiters; dtor may run async after a short budget.
  EXPECT_NE(release_body.find("capture->stop("), std::string::npos);
  EXPECT_NE(release_body.find("shared_ptr"), std::string::npos);
}

TEST(SessionStopContractTests, TerminateImplStopsBrowserCaptureBeforeIsolatedKill) {
  // Single media owner before pidfd kill — no second portal release.
  const auto source = read_source_for_contract("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto start = source.find("void proc_t::terminate_impl(");
  ASSERT_NE(start, std::string::npos);
  const auto body = source.substr(start, 3500);
  const auto prepare = body.find("session_media::prepare_for_stop(");
  const auto kill = body.find("terminate_isolated_session_generation(");
  ASSERT_NE(prepare, std::string::npos);
  ASSERT_NE(kill, std::string::npos);
  EXPECT_LT(prepare, kill);
  EXPECT_EQ(body.find("portal::release_global_capture("), std::string::npos);
}

TEST(SessionStopContractTests, WebUiDisconnectRespondsBeforeCaptureAndForceStop) {
  const auto source = read_source_for_contract("src/confighttp.cpp");
  ASSERT_FALSE(source.empty());
  const auto start = source.find("void disconnect(resp_https_t response, req_https_t request)");
  ASSERT_NE(start, std::string::npos);
  const auto end = source.find("void sendWol(", start);
  ASSERT_NE(end, std::string::npos);
  const auto body = source.substr(start, end - start);
  const auto schedule = body.find("session_media::schedule(");
  const auto prepare = body.find("session_media::prepare_for_stop(");
  const auto respond = body.find("send_response(");
  const auto force = body.find("proc::proc.terminate(");
  ASSERT_NE(schedule, std::string::npos);
  ASSERT_NE(prepare, std::string::npos);
  ASSERT_NE(respond, std::string::npos);
  ASSERT_NE(force, std::string::npos);
  EXPECT_LT(respond, schedule);
  EXPECT_LT(prepare, force);
}

TEST(SessionStopContractTests, BrowserStreamStopRespondsBeforeOwnedAppTerminate) {
  // confighttp must not join/terminate nested compositor before writing stop JSON.
  const auto source = read_source_for_contract("src/confighttp.cpp");
  ASSERT_FALSE(source.empty());
  const auto start = source.find("void postBrowserStreamStop(");
  ASSERT_NE(start, std::string::npos);
  const auto end = source.find("void getVDisplayBackends(", start);
  ASSERT_NE(end, std::string::npos);
  const auto body = source.substr(start, end - start);
  const auto schedule = body.find("session_media::schedule(");
  const auto prepare = body.find("session_media::prepare_for_stop(");
  const auto stop = body.find("browser_stream::stop_session(");
  const auto respond = body.find("send_response(");
  const auto terminate = body.find("proc::proc.terminate(");
  ASSERT_NE(schedule, std::string::npos);
  ASSERT_NE(prepare, std::string::npos);
  ASSERT_NE(stop, std::string::npos);
  ASSERT_NE(respond, std::string::npos);
  ASSERT_NE(terminate, std::string::npos);
  EXPECT_LT(stop, respond);
  EXPECT_LT(respond, schedule);
  EXPECT_LT(prepare, terminate);
  EXPECT_NE(body.find("terminate_owned_app"), std::string::npos);
  EXPECT_NE(body.find("release_media"), std::string::npos);
  EXPECT_NE(body.find("media_draining"), std::string::npos);
  EXPECT_NE(body.find("token_matched"), std::string::npos);
  // Linux leaves the HTTPS worker via session_media::schedule (not bare .detach).
}

TEST(SessionStopContractTests, PortalReleaseDestroysCaptureOffHttpCriticalPath) {
  // pw_thread_loop_stop can block forever; release must keep an owned worker and
  // a teardown owner instead of abandoning cleanup with a detached thread.
  const auto source = read_portal_grab_source_for_contract();
  ASSERT_FALSE(source.empty());
  const auto release = source.find("void release_global_capture()");
  ASSERT_NE(release, std::string::npos);
  const auto release_body = source.substr(release, 2200);
  EXPECT_NE(release_body.find("async capture/session destroy"), std::string::npos);
  EXPECT_NE(release_body.find("session_media::begin_teardown"), std::string::npos);
  EXPECT_NE(release_body.find("cleanup.worker"), std::string::npos);
  EXPECT_EQ(release_body.find("destroyer.detach"), std::string::npos);
}

TEST(SessionStopContractTests, TimedOutBrowserJoinRetainsTeardownOwnership) {
  const auto source = read_source_for_contract("src/browser_stream.cpp");
  ASSERT_FALSE(source.empty());
  const auto start = source.find("void finish_video_capture_stop(");
  ASSERT_NE(start, std::string::npos);
  const auto body = source.substr(start, 2200);
  EXPECT_NE(body.find("session_media::begin_teardown"), std::string::npos);
  EXPECT_NE(body.find("teardown = std::move(teardown)"), std::string::npos);
  EXPECT_NE(body.find("joiner.detach()"), std::string::npos);
}

TEST(SessionStopContractTests, CompositorTerminationRetainsRootMediaFenceUntilCleanupIsQuiescent) {
  const auto media = read_source_for_contract("src/platform/linux/session_media.cpp");
  ASSERT_FALSE(media.empty());
  const auto prepare = media.find("teardown_owner_t prepare_for_stop()");
  ASSERT_NE(prepare, std::string::npos);
  const auto prepare_body = media.substr(prepare, 2600);
  EXPECT_NE(prepare_body.find("media_gate().wait_for_other_teardowns(teardown)"), std::string::npos);
  EXPECT_NE(prepare_body.find("return teardown;"), std::string::npos);
  EXPECT_EQ(prepare_body.find("teardown.reset()"), std::string::npos);

  const auto process = read_source_for_contract("src/process.cpp");
  ASSERT_FALSE(process.empty());
  const auto terminate = process.find("void proc_t::terminate_impl(");
  ASSERT_NE(terminate, std::string::npos);
  const auto terminate_body = process.substr(terminate, 4200);
  // Function-scoped holder so the fence outlives early #ifdef blocks through undo.
  const auto fence = terminate_body.find("media_stop.fence = session_media::prepare_for_stop()");
  const auto compositor_stop = terminate_body.find("terminate_isolated_session_generation()");
  ASSERT_NE(fence, std::string::npos);
  ASSERT_NE(compositor_stop, std::string::npos);
  EXPECT_LT(fence, compositor_stop);
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

TEST(RtspLaunchHandoffTests, CancelledControlCommandRejectsWithoutInvokingHandler) {
  rtsp_stream::launch_session_t session {};
  session.cancel();
  bool handler_invoked = false;
  bool rejection_invoked = false;

  EXPECT_FALSE(rtsp_stream::dispatch_control_command_for_tests(
    session,
    [&handler_invoked]() {
      handler_invoked = true;
    },
    [&rejection_invoked]() {
      rejection_invoked = true;
    }
  ));
  EXPECT_FALSE(handler_invoked);
  EXPECT_TRUE(rejection_invoked);
}

TEST(RtspLaunchHandoffTests, AcceptAndDispatchUseSlotAwareCommandAdmission) {
  const auto source = read_rtsp_source_for_contract();
  ASSERT_FALSE(source.empty());

  const auto handle_start = source.find("void handle_msg(tcp::socket &sock, launch_session_t &session, msg_t &&req)");
  const auto handle_end = source.find("void handle_accept(const boost::system::error_code &ec)", handle_start);
  ASSERT_NE(handle_start, std::string::npos);
  ASSERT_NE(handle_end, std::string::npos);

  const auto handle_body = source.substr(handle_start, handle_end - handle_start);
  const auto dispatch_gate = handle_body.find("if (!dispatch_control_command(");
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
