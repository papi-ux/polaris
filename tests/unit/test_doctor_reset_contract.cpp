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
