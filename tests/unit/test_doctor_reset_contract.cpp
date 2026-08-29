#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {
  std::string source(const char *relative) {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / relative);
    EXPECT_TRUE(input.is_open()) << relative;
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
  }

  std::string between(const std::string &text, const std::string &begin, const std::string &end) {
    const auto first = text.find(begin);
    EXPECT_NE(first, std::string::npos) << begin;
    const auto last = text.find(end, first);
    EXPECT_NE(last, std::string::npos) << end;
    if (first == std::string::npos || last == std::string::npos) return {};
    return text.substr(first, last - first);
  }
}

TEST(DoctorResetContract, OptimizeCannotReadHistoryAiSettingsOrRecoveryOverlay) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto handler = between(
    nvhttp,
    "auto polarisOptimize =",
    "auto polarisClientSupportReport ="
  );
  const auto serializer = between(
    nvhttp,
    "void append_deterministic_optimization_json(",
    "bool ai_auto_quality_enabled()"
  );
  for (const auto *forbidden : {
         "get_session_history", "get_cached", "request_sync",
         "prepare_for_optimizer", "overlay_optimization"
       }) {
    EXPECT_EQ(handler.find(forbidden), std::string::npos) << forbidden;
    EXPECT_EQ(serializer.find(forbidden), std::string::npos) << forbidden;
  }
  EXPECT_NE(handler.find("append_deterministic_optimization_json"), std::string::npos);
  EXPECT_NE(serializer.find("resolved_profile"), std::string::npos);
  EXPECT_NE(serializer.find("may_define_settings"), std::string::npos);
}

TEST(DoctorResetContract, FinalResolverCannotReadHistoryOrAiSettings) {
  const auto resolver = between(
    source("src/process.cpp"),
    "Resolve session overrides in a strict, evidence-independent order",
    "const bool mirror_desktop_session"
  );
  EXPECT_EQ(resolver.find("get_session_history"), std::string::npos);
  EXPECT_EQ(resolver.find("get_cached"), std::string::npos);
  EXPECT_EQ(resolver.find("history_safe"), std::string::npos);
  EXPECT_EQ(resolver.find("virtual_display = *resolved_optimization"), std::string::npos);
  EXPECT_NE(resolver.find("launch_profile::resolve"), std::string::npos);
}

TEST(DoctorResetContract, RunningSessionCannotRewriteTheConfiguredHostBitrateCap) {
  const auto process = source("src/process.cpp");
  EXPECT_EQ(process.find("config::video.max_bitrate ="), std::string::npos);
  EXPECT_EQ(
    process.find("config::video.adaptive_bitrate.max_bitrate_kbps ="),
    std::string::npos
  );

  const auto rtsp = source("src/rtsp.cpp");
  EXPECT_NE(
    rtsp.find("session.target_bitrate_kbps.value_or(\n        config::video.max_bitrate"),
    std::string::npos
  );
}

TEST(DoctorResetContract, ExplicitClientBitrateRoutesReplaceTheLiveTarget) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto live_route = between(
    nvhttp,
    "auto polarisSetBitrate =",
    "auto polarisSetAdaptiveBitrate ="
  );
  EXPECT_NE(live_route.find("doctor_actions::set_owner_live_bitrate"), std::string::npos);
  EXPECT_EQ(live_route.find("adaptive_bitrate::set_base_bitrate"), std::string::npos);

  const auto video = source("src/video.cpp");
  const auto runtime_update = between(
    video,
    "// Check adaptive bitrate and update encoder if target has changed",
    "stream_stats::update_frame_delivery("
  );
  EXPECT_NE(runtime_update.find("adaptive_bitrate::get_live_bitrate_request()"), std::string::npos);
  EXPECT_NE(runtime_update.find("acknowledge_live_bitrate_applied"), std::string::npos);
}

TEST(DoctorResetContract, HostNetworkEvidenceLinearizesBeforeAdaptiveFeedback) {
  const auto stream = source("src/stream.cpp");
  const auto periodic_ping = between(
    stream,
    "server->map(packetTypes[IDX_PERIODIC_PING]",
    "server->map(packetTypes[IDX_START_A]"
  );
  const auto loss_stats = between(
    stream,
    "server->map(packetTypes[IDX_LOSS_STATS]",
    "server->map(packetTypes[IDX_REQUEST_IDR_FRAME]"
  );
  for (const auto *handler : {&periodic_ping, &loss_stats}) {
    const auto evidence = handler->find("record_network_stats(");
    const auto feedback = handler->find("adaptive_bitrate::update_network_stats(");
    ASSERT_NE(evidence, std::string::npos);
    ASSERT_NE(feedback, std::string::npos);
    EXPECT_LT(evidence, feedback);
  }

  const auto stats = source("src/stream_stats.cpp");
  const auto publication = between(
    stats,
    "void record_primary_network_observation(",
    "}  // namespace"
  );
  const auto policy = publication.find("const bool suppresses_quality_restore");
  const auto epoch = publication.find("adaptive_bitrate::note_network_evidence_arrival(");
  const auto fields = publication.find("primary_network_state.received_at");
  const auto revision = publication.find("primary_network_state.revision");
  ASSERT_NE(policy, std::string::npos);
  ASSERT_NE(epoch, std::string::npos);
  ASSERT_NE(fields, std::string::npos);
  ASSERT_NE(revision, std::string::npos);
  EXPECT_LT(fields, policy);
  EXPECT_LT(policy, epoch);
  EXPECT_LT(epoch, revision);
}

TEST(DoctorResetContract, HostVideoEvidenceLinearizesBeforeAdaptiveFeedbackAndPublication) {
  const auto video = source("src/video.cpp");
  const auto sample = between(
    video,
    "const double fps_ratio =",
    "stream_stats::update_video_stats("
  );
  const auto epoch = sample.find("stream_stats::note_doctor_video_policy_sample(");
  const auto feedback = sample.find("adaptive_bitrate::update_stream_health(");
  const auto frame_publication = sample.find("stream_stats::update_frame_delivery(");
  ASSERT_NE(epoch, std::string::npos);
  ASSERT_NE(feedback, std::string::npos);
  ASSERT_NE(frame_publication, std::string::npos);
  EXPECT_LT(epoch, feedback);
  EXPECT_LT(epoch, frame_publication);

  const auto stats = source("src/stream_stats.cpp");
  const auto source_cadence = between(
    stats,
    "void update_capture_source_fps(",
    "void note_doctor_video_policy_sample("
  );
  const auto policy_lock = source_cadence.find("doctor_video_policy_mutex");
  const auto source_epoch = source_cadence.find("publish_doctor_video_policy_locked()");
  const auto source_field = source_cadence.find("hot_capture_source_fps.store(");
  const auto policy_unlock = source_cadence.find("\n    }", policy_lock);
  ASSERT_NE(policy_lock, std::string::npos);
  ASSERT_NE(source_epoch, std::string::npos);
  ASSERT_NE(source_field, std::string::npos);
  ASSERT_NE(policy_unlock, std::string::npos);
  EXPECT_LT(policy_lock, source_epoch);
  EXPECT_LT(source_epoch, source_field);
  EXPECT_LT(source_field, policy_unlock);
}

TEST(DoctorResetContract, PairedGlobalAdaptiveToggleRequiresTheSoleActiveOwner) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto adaptive_route = between(
    nvhttp,
    "auto polarisSetAdaptiveBitrate =",
    "auto polarisSetAiOptimizer ="
  );
  EXPECT_NE(
    adaptive_route.find("doctor_actions::acquire_paired_global_control"),
    std::string::npos
  );
  EXPECT_NE(adaptive_route.find("active_owner_required"), std::string::npos);
}

TEST(DoctorResetContract, ResolvedLaunchRequiresAnExactResolvedHdrValue) {
  const auto launch_parser = between(
    source("src/nvhttp.cpp"),
    "if (launch_session->resolved_profile_from_client) {",
    "launch_session->watch_only ="
  );
  EXPECT_NE(launch_parser.find("get_arg(args, \"resolvedHdr\", \"\")"), std::string::npos);
  EXPECT_NE(launch_parser.find("raw_hdr == \"1\""), std::string::npos);
  EXPECT_NE(launch_parser.find("raw_hdr == \"0\""), std::string::npos);
  EXPECT_NE(
    launch_parser.find("Rejecting resolved launch profile with missing or malformed HDR value"),
    std::string::npos
  );
}

TEST(DoctorResetContract, OptimizeOwnsHardRefreshAndHdrCapabilityValidation) {
  const auto handler = between(
    source("src/nvhttp.cpp"),
    "auto polarisOptimize =",
    "auto polarisClientSupportReport ="
  );
  EXPECT_NE(handler.find("client_max_fps"), std::string::npos);
  EXPECT_NE(handler.find("topology_max_launch_refresh_rate_for_http"), std::string::npos);
  EXPECT_NE(handler.find("launch_owned_display"), std::string::npos);
  EXPECT_NE(handler.find("selected_output_name"), std::string::npos);
  EXPECT_NE(handler.find("topology_locked"), std::string::npos);
  EXPECT_NE(handler.find("find_app_for_optimization_game"), std::string::npos);
  EXPECT_NE(handler.find("advertised_codec_support_for_http"), std::string::npos);
  EXPECT_NE(handler.find("host_hdr_capable"), std::string::npos);
}

TEST(DoctorResetContract, FutureLaunchOwnedDisplayIsNotCappedByCurrentPhysicalRefresh) {
  const auto helper = between(
    source("src/nvhttp.cpp"),
    "std::optional<int> topology_max_launch_refresh_rate_for_http(",
    "int advertised_max_launch_refresh_rate_for_http()"
  );
  EXPECT_NE(helper.find("if (launch_owned_display)"), std::string::npos);
  EXPECT_NE(helper.find("return 120"), std::string::npos);
  EXPECT_EQ(helper.find("return 60"), std::string::npos);
}

TEST(DoctorResetContract, CanonicalOptimizationIdentityPrecedesDisplayNameFallback) {
  const auto lookup = between(
    source("src/nvhttp.cpp"),
    "std::optional<proc::ctx_t> find_app_for_optimization_game(",
    "#if defined(__linux__)"
  );
  const auto uuid_match = lookup.find("boost::iequals(app.uuid, game)");
  const auto id_match = lookup.find("boost::iequals(app.id, game)");
  const auto name_match = lookup.find("boost::iequals(app.name, game)");
  ASSERT_NE(uuid_match, std::string::npos);
  ASSERT_NE(id_match, std::string::npos);
  ASSERT_NE(name_match, std::string::npos);
  EXPECT_LT(uuid_match, name_match);
  EXPECT_LT(id_match, name_match);
}

TEST(DoctorResetContract, FinalResolverRevalidatesPostProfileRefreshAndHdrCaps) {
  const auto capability_snapshot = between(
    source("src/process.cpp"),
    "int validate_resolved_launch_profile_for_app(",
    "int proc_t::execute("
  );
  EXPECT_NE(capability_snapshot.find("client_profile"), std::string::npos);
  EXPECT_NE(capability_snapshot.find("active_refresh_rate_hz_hint"), std::string::npos);
  EXPECT_NE(capability_snapshot.find("effective_session_selection_for_launch"), std::string::npos);
  EXPECT_NE(capability_snapshot.find("selection_owns_launch_refresh_rate"), std::string::npos);
  EXPECT_NE(capability_snapshot.find("advertised_codec_capability_state"), std::string::npos);
  EXPECT_NE(capability_snapshot.find("return 409"), std::string::npos);

  const auto launch_validation = between(
    source("src/process.cpp"),
    "int proc_t::execute_impl(",
    "Resolve session overrides in a strict, evidence-independent order"
  );
  EXPECT_NE(launch_validation.find("get_client_profile"), std::string::npos);
  EXPECT_NE(launch_validation.find("validate_resolved_launch_profile_for_app"), std::string::npos);

  const auto process = between(
    source("src/process.cpp"),
    "Resolve session overrides in a strict, evidence-independent order",
    "const bool mirror_desktop_session"
  );
  EXPECT_NE(process.find("preset_request.host_max_fps = launch_session->host_max_fps"), std::string::npos);
  EXPECT_NE(process.find("preset_request.host_hdr_capable = launch_session->host_hdr_capable"), std::string::npos);
}

TEST(DoctorResetContract, ResolvedLaunchFailsClosedWhenHostCapsChanged) {
  const auto final_capability_gate = between(
    source("src/process.cpp"),
    "int validate_resolved_launch_profile_for_app(",
    "int proc_t::execute("
  );
  EXPECT_NE(final_capability_gate.find("above final output refresh cap"), std::string::npos);
  EXPECT_NE(final_capability_gate.find("above configured bitrate cap"), std::string::npos);
  EXPECT_NE(final_capability_gate.find("final encoder lacks HDR support"), std::string::npos);
  EXPECT_NE(final_capability_gate.find("return 409"), std::string::npos);
}

TEST(DoctorResetContract, ResumeRevalidatesTheExactProfileBeforeRaisingAStream) {
  const auto process = between(
    source("src/process.cpp"),
    "int proc_t::validate_resolved_profile_for_running_app(",
    "bool proc_t::raise_session_for_admitted_launch("
  );
  EXPECT_NE(process.find("validate_resolved_launch_profile_for_app"), std::string::npos);
  EXPECT_NE(process.find("effective_session_selection_for_launch"), std::string::npos);
  EXPECT_NE(process.find("active app generation"), std::string::npos);

  const auto resume = between(
    source("src/nvhttp.cpp"),
    "void resume(",
    "void cancel("
  );
  const auto validation = resume.find("validate_resolved_profile_for_running_app");
  const auto raise = resume.find("raise_session_for_admitted_launch");
  EXPECT_NE(validation, std::string::npos);
  EXPECT_NE(raise, std::string::npos);
  EXPECT_LT(validation, raise);

  const auto launch = between(
    source("src/nvhttp.cpp"),
    "void launch(",
    "void resume("
  );
  const auto same_app = launch.find("We're basically resuming the same app");
  const auto launch_validation = launch.find(
    "validate_resolved_profile_for_running_app",
    same_app
  );
  ASSERT_NE(same_app, std::string::npos);
  ASSERT_NE(launch_validation, std::string::npos);
  const auto initial_validation = launch.find(
    "if (!active_profile_is_valid())",
    launch_validation
  );
  const auto display_prepare = launch.find("display_device::configure_display", same_app);
  const auto launch_capability_revalidation = launch.find(
    "if (!active_profile_is_valid())",
    display_prepare
  );
  ASSERT_NE(initial_validation, std::string::npos);
  ASSERT_NE(display_prepare, std::string::npos);
  ASSERT_NE(launch_capability_revalidation, std::string::npos);
  const auto launch_raise = launch.find(
    "raise_session_for_admitted_launch",
    launch_capability_revalidation
  );
  const auto launch_cage_refresh = launch.find(
    "stream_runtime::labwc::ensure_output_refresh(launch_session->fps, false)",
    launch_capability_revalidation
  );
  ASSERT_NE(launch_raise, std::string::npos);
  EXPECT_LT(initial_validation, display_prepare)
    << "the same-app /launch race is a resume and must reject a stale exact envelope before preparation";
  EXPECT_LT(display_prepare, launch_capability_revalidation);
  ASSERT_NE(launch_cage_refresh, std::string::npos);
  EXPECT_LT(launch_capability_revalidation, launch_cage_refresh);
  EXPECT_LT(launch_cage_refresh, launch_raise);
  EXPECT_NE(
    launch.find("if (!launch_cage_refresh_applied)", launch_cage_refresh),
    std::string::npos
  ) << "the same-app exact launch must not hide a failed private-output refresh";

  EXPECT_NE(
    process.find("resolved_reconnect_cadence_allowed"),
    std::string::npos
  );
  EXPECT_NE(
    process.find("active launch-owned output cannot apply and verify it"),
    std::string::npos
  ) << "Host Virtual and Gamescope reconnects must reject cadence changes they cannot apply";
}

TEST(DoctorResetContract, ExactReconnectRequiresTheActiveSessionToken) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto token_matcher = between(
    nvhttp,
    "bool session_token_matches_value(",
    "void append_current_game_session_fields("
  );
  EXPECT_NE(token_matcher.find("bool require_exact_token = false"), std::string::npos);
  EXPECT_NE(
    token_matcher.find("!expected_token.empty() && !active_token.empty()"),
    std::string::npos
  );

  const auto launch = between(nvhttp, "void launch(", "void resume(");
  const auto resume = between(nvhttp, "void resume(", "void cancel(");
  EXPECT_NE(
    launch.find(
      "session_token_matches_request(\n              args,\n              launch_session->resolved_profile_from_client"
    ),
    std::string::npos
  );
  EXPECT_NE(
    resume.find(
      "session_token_matches_request(\n          args,\n          launch_session->resolved_profile_from_client"
    ),
    std::string::npos
  );
}

TEST(DoctorResetContract, PairedDisplaySettingIsNotPromotedToAnExplicitLaunchLock) {
  const auto launch_parser = between(
    source("src/nvhttp.cpp"),
    "launch_session->resolved_profile_from_client =",
    "launch_session->scale_factor ="
  );
  EXPECT_NE(
    launch_parser.find(
      "launch_session->user_locked_display_mode = launch_session->resolved_profile_from_client;"
    ),
    std::string::npos
  );
  EXPECT_EQ(
    launch_parser.find(
      "launch_session->resolved_profile_from_client || !named_cert_p->display_mode.empty()"
    ),
    std::string::npos
  );
}

TEST(DoctorResetContract, ExplicitTopologyPrecedesPairedAlwaysVirtualDefault) {
  const auto launch_parser = between(
    source("src/nvhttp.cpp"),
    "const bool client_display_mode_explicit =",
    "launch_session->scale_factor ="
  );
  EXPECT_NE(
    launch_parser.find(
      "!client_display_mode_explicit &&\n        named_cert_p->always_use_virtual_display"
    ),
    std::string::npos
  );

  const auto optimize = between(
    source("src/nvhttp.cpp"),
    "auto polarisOptimize =",
    "auto polarisClientSupportReport ="
  );
  EXPECT_NE(
    optimize.find("named_cert_p->always_use_virtual_display && !topology_locked"),
    std::string::npos
  );
  EXPECT_NE(
    optimize.find("launch_profile::resolve_non_linux_topology("),
    std::string::npos
  );
  EXPECT_NE(optimize.find("topology_source"), std::string::npos);
  EXPECT_NE(optimize.find("topology_reason_code"), std::string::npos);

  const auto final_validation = between(
    source("src/process.cpp"),
    "int validate_resolved_launch_profile_for_app(",
    "int proc_t::execute("
  );
  EXPECT_NE(
    final_validation.find("launch_profile::resolve_non_linux_topology("),
    std::string::npos
  );
}

TEST(DoctorResetContract, OptimizeAndResolvedLaunchShareTheSessionTopologyGate) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto launch_parser = between(
    nvhttp,
    "const bool client_display_mode_explicit =",
    "launch_session->virtual_display ="
  );
  const auto launch_gate = launch_parser.find("accepted_session_stream_mode(");
  const auto exact_rejection = launch_parser.find(
    "launch_session->resolved_profile_from_client",
    launch_gate
  );
  const auto fail_closed = launch_parser.find("return nullptr;", exact_rejection);
  const auto expected_topology = launch_parser.find("expectedTopology");
  const auto fresh_parser_probe = launch_parser.find(
    "stream_display_policy::selection_valid_fresh("
  );
  ASSERT_NE(launch_gate, std::string::npos);
  ASSERT_NE(exact_rejection, std::string::npos);
  ASSERT_NE(expected_topology, std::string::npos);
  ASSERT_NE(fresh_parser_probe, std::string::npos);
  EXPECT_NE(fail_closed, std::string::npos);

  const auto optimize = between(
    nvhttp,
    "auto polarisOptimize =",
    "auto polarisClientSupportReport ="
  );
  EXPECT_NE(
    optimize.find("accepted_session_stream_mode("),
    std::string::npos
  );
  EXPECT_NE(
    optimize.find("invalid_or_unavailable_topology"),
    std::string::npos
  );
  const auto effective_topology = optimize.find(
    "effective_session_selection_for_launch("
  );
  const auto effective_availability = optimize.find(
    "stream_display_policy::selection_valid_fresh(",
    effective_topology
  );
  ASSERT_NE(effective_topology, std::string::npos);
  EXPECT_NE(effective_availability, std::string::npos)
    << "app-derived and configured host-default topology also require availability";

  const auto process = source("src/process.cpp");
  const auto final_apply = between(
    process,
    "Resolve and apply the exact topology before installing a new process",
    "++_session_generation;"
  );
  const auto final_validity = final_apply.find(
    "stream_display_policy::selection_valid_fresh("
  );
  const auto expected_binding = final_apply.find(
    "launch_session->expected_stream_mode != session_mode"
  );
  const auto companion_match = final_apply.find(
    "stream_display_policy::selection_companion_state_matches("
  );
  const auto apply = final_apply.find("stream_display_policy::apply_selection(");
  const auto resolved = final_apply.find(
    "launch_session->resolved_profile_from_client",
    apply
  );
  const auto conflict = final_apply.find("return 409;", resolved);
  ASSERT_NE(final_validity, std::string::npos);
  ASSERT_NE(expected_binding, std::string::npos);
  ASSERT_NE(companion_match, std::string::npos);
  ASSERT_NE(apply, std::string::npos);
  ASSERT_NE(resolved, std::string::npos);
  EXPECT_LT(final_validity, companion_match)
    << "availability must be re-probed even when the companion state already matches";
  EXPECT_NE(conflict, std::string::npos);
  EXPECT_LT(expected_binding, final_validity)
    << "the exact optimize result must match before the selected topology is applied";
  EXPECT_NE(
    final_apply.find("restore_prelaunch_display_policy();"),
    std::string::npos
  );
  for (const auto *field : {"streaming_output", "primary_output"}) {
    const auto snapshot = final_apply.find(
      std::string {"initial_"} + field + " = linux_display." + field
    );
    const auto restore = final_apply.find(
      std::string {"linux_display."} + field + " = initial_" + field,
      snapshot
    );
    ASSERT_NE(snapshot, std::string::npos) << field;
    EXPECT_NE(restore, std::string::npos) << field
      << " must be restored if dongle auto-detection partially mutates then fails";
  }
}

TEST(DoctorResetContract, TopologySettingsShareTheFinalLaunchLifecycleLock) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto client_settings = between(
    nvhttp,
    "auto polarisClientSettings =",
    "auto polarisClientSupportReport ="
  );
  const auto lifecycle_lock = client_settings.find(
    "proc::proc.acquire_session_lifecycle_lock()"
  );
  const auto standalone_gate = client_settings.find(
    "standalone_topology_required"
  );
  const auto typed_persistence_failure = client_settings.find(
    "stream_display_mode_persistence_failed"
  );
  const auto typed_topology_rejection = client_settings.find(
    "const auto reject_stream_display_mode ="
  );
  const auto controller_lock = client_settings.find(
    "doctor_actions::acquire_paired_global_control("
  );
  const auto active_generation_gate = client_settings.find(
    "proc::proc.running() != 0"
  );
  const auto topology_apply = client_settings.find(
    "apply_stream_display_mode_selection("
  );
  const auto lifecycle_unlock = client_settings.find(
    "topology_lifecycle_guard.unlock()"
  );
  ASSERT_NE(lifecycle_lock, std::string::npos);
  ASSERT_NE(standalone_gate, std::string::npos);
  ASSERT_NE(typed_persistence_failure, std::string::npos);
  ASSERT_NE(typed_topology_rejection, std::string::npos);
  ASSERT_NE(active_generation_gate, std::string::npos);
  ASSERT_NE(controller_lock, std::string::npos);
  ASSERT_NE(topology_apply, std::string::npos);
  ASSERT_NE(lifecycle_unlock, std::string::npos);
  EXPECT_LT(standalone_gate, lifecycle_lock)
    << "topology persistence must not share a request with a later fallible paired update";
  EXPECT_NE(
    client_settings.find(
      "SimpleWeb::StatusCode::server_error_internal_server_error",
      typed_persistence_failure
    ),
    std::string::npos
  ) << "a config write failure is a typed server error, never a rejected client value";
  EXPECT_NE(
    client_settings.find("invalid_or_unavailable_topology", typed_topology_rejection),
    std::string::npos
  ) << "malformed and unavailable topology values must share the typed rejection envelope";
  EXPECT_LT(lifecycle_lock, controller_lock)
    << "topology writers must preserve lifecycle-to-controller lock order";
  EXPECT_LT(lifecycle_lock, active_generation_gate);
  EXPECT_LT(active_generation_gate, controller_lock)
    << "an installed process generation must reject topology mutation";
  EXPECT_LT(controller_lock, topology_apply);
  EXPECT_LT(topology_apply, lifecycle_unlock)
    << "the lifecycle lock must cover the topology mutation";

  const auto process = source("src/process.cpp");
  EXPECT_NE(
    process.find(
      "std::unique_lock<std::recursive_mutex> proc_t::acquire_session_lifecycle_lock() const"
    ),
    std::string::npos
  );
}

TEST(DoctorResetContract, ClientSettingsPersistencePreservesExistingConfigAtomically) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto persistence = between(
    nvhttp,
    "bool persist_config_values(",
    "std::string configured_stream_display_mode_selection()"
  );

  EXPECT_EQ(persistence.find("file_handler::read_file"), std::string::npos);
  EXPECT_EQ(persistence.find("file_handler::write_file"), std::string::npos);
  EXPECT_NE(persistence.find("fs::symlink_status"), std::string::npos);
  EXPECT_NE(persistence.find("std::ifstream input"), std::string::npos);
  EXPECT_NE(persistence.find("input.bad()"), std::string::npos);
  EXPECT_NE(persistence.find("input.close()"), std::string::npos);
  EXPECT_NE(persistence.find("input.fail()"), std::string::npos);
  EXPECT_NE(persistence.find("private_state_file::write_atomic"), std::string::npos);
  EXPECT_NE(
    persistence.find("private_state_file::write_status_e::not_committed"),
    std::string::npos
  );
  EXPECT_NE(
    persistence.find("private_state_file::write_status_e::durability_uncertain"),
    std::string::npos
  );
}

TEST(DoctorResetContract, ExactTopologyAssertionHasItsOwnCapabilityVersion) {
  const auto nvhttp = source("src/nvhttp.cpp");
  EXPECT_NE(
    nvhttp.find("features[\"expected_topology_assertion_v1\"] = true"),
    std::string::npos
  );
}

TEST(DoctorResetContract, AppTopologyPrecedenceIsResolvedBeforeRequestAvailability) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto optimize_topology = between(
    nvhttp,
    "const bool paired_virtual_lock =",
    "resolved_topology = effective_selection;"
  );
  const auto effective = optimize_topology.find(
    "auto effective_selection = stream_display_policy::effective_session_selection_for_launch("
  );
  const auto requested_availability = optimize_topology.find(
    "accepted_session_stream_mode(",
    effective
  );

  ASSERT_NE(effective, std::string::npos);
  ASSERT_NE(requested_availability, std::string::npos);
  EXPECT_LT(effective, requested_availability)
    << "hard app mirror and unlocked app-virtual semantics must choose the winner before a losing request is probed";
  EXPECT_NE(
    optimize_topology.find("if (effective_selection == requested_selection)"),
    std::string::npos
  ) << "only the winning requested topology may make its live availability authoritative";
}

TEST(DoctorResetContract, HostVirtualRetiresDongleAuthorityUnlessKScreenNeedsIt) {
  const auto policy = source("src/platform/linux/stream_display_policy.cpp");
  const auto normalization = between(
    policy,
    "void normalize_host_virtual_display_state()",
    "bool selection_available_for_capabilities("
  );
  EXPECT_NE(
    normalization.find(
      "host_virtual_backend_creates_output(backend)"
    ),
    std::string::npos
  );
  EXPECT_NE(
    normalization.find("clear_connector_output_authority("),
    std::string::npos
  ) << "EVDI/wlroots Host Virtual must not inherit dongle capture or game-placement connectors";
}

TEST(DoctorResetContract, ExactHostVirtualAuthorityBypassesASynchronizedCache) {
  const auto virtual_display = source("src/platform/linux/virtual_display.cpp");
  EXPECT_NE(
    virtual_display.find("std::mutex backend_detection_mutex"),
    std::string::npos
  );
  const auto detector = between(
    virtual_display,
    "backend_e detect_backend_with_cache_policy(",
    "bool backend_has_required_configuration("
  );
  EXPECT_NE(detector.find("std::lock_guard cache_lock"), std::string::npos);
  EXPECT_NE(detector.find("!force_refresh && cached_backend.has_value()"), std::string::npos);
  EXPECT_NE(detector.find("detect_backend_with_cache_policy(true)"), std::string::npos);

  const auto unavailable_reason = between(
    virtual_display,
    "std::string unavailable_reason()",
    "static bool destroy_unlocked("
  );
  EXPECT_NE(
    unavailable_reason.find("detect_backend_with_cache_policy(false, &evdi_blocked)"),
    std::string::npos
  );
  EXPECT_EQ(unavailable_reason.find("evdi::load_library()"), std::string::npos)
    << "lazy EVDI loader state must only be touched under the detector mutex";

  const auto policy = source("src/platform/linux/stream_display_policy.cpp");
  const auto fresh_validator = between(
    policy,
    "bool selection_valid_fresh(",
    "bool selection_companion_state_matches("
  );
  EXPECT_NE(
    fresh_validator.find("virtual_display::is_available_fresh()"),
    std::string::npos
  );
}

TEST(DoctorResetContract, DisabledTrialsNeverExposeOrMutateRetainedReceipts) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto session_status = between(
    nvhttp,
    "const auto doctor_v2_status = doctor_v2::status(",
    "auto recovery_records ="
  );
  const auto disabled_status = session_status.find("if (!doctor_v2::trials_enabled())");
  const auto retained_status = session_status.find("doctor_trial::status(");
  ASSERT_NE(disabled_status, std::string::npos);
  ASSERT_NE(retained_status, std::string::npos);
  EXPECT_LT(disabled_status, retained_status);
  EXPECT_NE(session_status.find("{\"state\", \"disabled\"}"), std::string::npos);
  EXPECT_NE(session_status.find("{\"cancellable\", false}"), std::string::npos);

  const auto route = between(
    nvhttp,
    "auto polarisDoctorTrial =",
    "auto polarisDoctorAction ="
  );
  const auto disabled_gate = route.find("if (!doctor_v2::trials_enabled())");
  const auto get_route = route.find("if (request->method == \"GET\")");
  const auto cancel = route.find("doctor_trial::cancel(");
  ASSERT_NE(disabled_gate, std::string::npos);
  ASSERT_NE(get_route, std::string::npos);
  ASSERT_NE(cancel, std::string::npos);
  EXPECT_LT(disabled_gate, get_route);
  EXPECT_LT(disabled_gate, cancel);

  const auto session_report = between(
    nvhttp,
    "auto polarisSessionReport =",
    "auto polarisClearOptimizerProfile ="
  );
  const auto report_gate = session_report.find("doctor_v2::trials_enabled()");
  const auto crash_observation = session_report.find("doctor_trial::observe(");
  ASSERT_NE(report_gate, std::string::npos);
  ASSERT_NE(crash_observation, std::string::npos);
  EXPECT_LT(report_gate, crash_observation);
}

TEST(DoctorResetContract, LegacyLaunchOverlayHelpersAreRemoved) {
  const auto nvhttp = source("src/nvhttp.cpp");
  EXPECT_EQ(nvhttp.find("append_optimization_json"), std::string::npos);
  EXPECT_EQ(nvhttp.find("select_paired_client_launch_bitrate"), std::string::npos);
  EXPECT_EQ(nvhttp.find("build_optimizer_profile_state_json"), std::string::npos);
  EXPECT_EQ(nvhttp.find("build_stability_plan_json"), std::string::npos);

  const auto process = source("src/process.cpp");
  EXPECT_EQ(process.find("launch_bitrate_is_locked"), std::string::npos);
  EXPECT_EQ(process.find("apply_optimization_layer"), std::string::npos);
  EXPECT_EQ(process.find("parse_display_mode_string"), std::string::npos);
}

TEST(DoctorResetContract, GameLaunchDoesNotSynthesizeLimiterVariables) {
  const auto launch_environment = between(
    source("src/process.cpp"),
    "Apply per-app environment variables",
    "bool cage_started_with_detached_client"
  );
  EXPECT_EQ(launch_environment.find("set_session_env_var(_env, _session_env_keys, \"DXVK_FRAME_RATE\""), std::string::npos);
  EXPECT_EQ(launch_environment.find("set_session_env_var(_env, _session_env_keys, \"MANGOHUD\""), std::string::npos);
  EXPECT_EQ(launch_environment.find("fps_limit="), std::string::npos);
  EXPECT_NE(launch_environment.find("stream_fps_only"), std::string::npos);
}

TEST(DoctorResetContract, LegacyRecoveryApplyFailsDeprecated) {
  const auto actions = source("src/doctor_actions.cpp");
  EXPECT_NE(actions.find("unsupported_deprecated"), std::string::npos);
  EXPECT_NE(actions.find("Next-launch recovery profiles are observational legacy records"), std::string::npos);
}

TEST(DoctorResetContract, LegacyAiSurfacesAreExplanationOnly) {
  const auto config_http = source("src/confighttp.cpp");
  const auto ai_serializer = between(
    config_http,
    "void appendAiExplanationJson(",
    "void scrubAiSettingFields("
  );
  const auto device_suggestion = between(
    config_http,
    "void getDeviceSuggestion(",
    "// ---- AI Optimizer API ----"
  );
  const auto legacy_optimize = between(
    config_http,
    "void triggerAiOptimize(",
    "void explainDoctorWithAi("
  );
  for (const auto *forbidden : {
         "display_mode", "target_bitrate_kbps", "preferred_codec",
         "virtual_display", "nvenc_tune"
       }) {
    EXPECT_EQ(ai_serializer.find(forbidden), std::string::npos) << forbidden;
  }
  EXPECT_NE(ai_serializer.find("explanation_only"), std::string::npos);
  EXPECT_EQ(device_suggestion.find("ai_optimizer::get_cached"), std::string::npos);
  EXPECT_EQ(legacy_optimize.find("request_sync"), std::string::npos);
  EXPECT_NE(legacy_optimize.find("ai_launch_policy_removed"), std::string::npos);

  const auto ui = source("src_assets/common/assets/web/configs/tabs/AiOptimizer.vue");
  EXPECT_EQ(ui.find("AI Auto Quality"), std::string::npos);
  EXPECT_EQ(ui.find("testResult.payload.display_mode"), std::string::npos);
  EXPECT_EQ(ui.find("testResult.payload.target_bitrate_kbps"), std::string::npos);
  EXPECT_NE(ui.find("AI explanations"), std::string::npos);

  const auto audio_video = source("src_assets/common/assets/web/configs/tabs/AudioVideo.vue");
  EXPECT_EQ(audio_video.find("AI Auto Quality"), std::string::npos);
  EXPECT_NE(audio_video.find("Adaptive bitrate"), std::string::npos);
  EXPECT_EQ(
    audio_video.find("config.value.adaptive_bitrate_enabled === 'enabled' &&"),
    std::string::npos
  );
}
