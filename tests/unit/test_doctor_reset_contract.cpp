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
    "bool host_prefers_headless()"
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

TEST(DoctorResetContract, WebLaunchModeCatalogUsesLaunchEquivalentVirtualDisplayCapability) {
  const auto get_config = between(
    source("src/confighttp.cpp"),
    "void getConfig(",
    "void getLocale("
  );
  EXPECT_NE(
    get_config.find("virtual_display::backend_has_required_configuration("),
    std::string::npos
  );
  EXPECT_NE(
    get_config.find("config::video.linux_display.streaming_output"),
    std::string::npos
  );
  EXPECT_EQ(
    get_config.find("vd_backend != virtual_display::backend_e::NONE"),
    std::string::npos
  );
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
  EXPECT_NE(
    runtime_update.find("begin_live_bitrate_session_recreation"),
    std::string::npos
  );
  EXPECT_NE(runtime_update.find("acknowledge_live_bitrate_applied"), std::string::npos);
  EXPECT_NE(runtime_update.find("bitrate_update_e::recreate_session"), std::string::npos);
  EXPECT_NE(runtime_update.find("config.bitrate = request->target_bitrate_kbps"), std::string::npos);
  EXPECT_EQ(runtime_update.find("pending_bitrate_confirmation"), std::string::npos);
  EXPECT_NE(video.find("codec_name == \"h264_nvenc\""), std::string::npos);
  EXPECT_NE(video.find("codec_name == \"hevc_nvenc\""), std::string::npos);
  EXPECT_NE(video.find("codec_name == \"av1_nvenc\""), std::string::npos);

  const auto ffmpeg_update = between(
    video,
    "bitrate_update_e update_bitrate(int new_bitrate_kbps) override",
    "avcodec_ctx_t avcodec_ctx"
  );
  EXPECT_NE(ffmpeg_update.find("return bitrate_update_e::recreate_session"), std::string::npos);
  EXPECT_EQ(ffmpeg_update.find("avcodec_ctx->bit_rate ="), std::string::npos);
  EXPECT_EQ(ffmpeg_update.find("acknowledge_live_bitrate_applied"), std::string::npos);

  const auto encode_success = between(
    video,
    "if (encode(frame_nr++, *session, packets, channel_data, frame_timestamp))",
    "auto encode_end = std::chrono::steady_clock::now()"
  );
  EXPECT_EQ(encode_success.find("acknowledge_live_bitrate_applied"), std::string::npos);
}

TEST(DoctorResetContract, PairedDoctorFailuresUseTypedNonSuccessHttpStatus) {
  const auto route = between(
    source("src/nvhttp.cpp"),
    "auto polarisDoctorAction =",
    "auto polarisSetBitrate ="
  );
  EXPECT_NE(
    route.find("doctor_actions::http_status_code(output)"),
    std::string::npos
  );
  EXPECT_NE(route.find("err[\"changed\"] = false"), std::string::npos);
  const auto owner_rejection = between(
    route,
    "if (!doctor_actions::paired_route_allowed(",
    "const auto stats = stream_stats::get_current()"
  );
  EXPECT_NE(owner_rejection.find("err[\"status\"] = false"), std::string::npos);
  EXPECT_NE(owner_rejection.find("err[\"changed\"] = false"), std::string::npos);
  EXPECT_NE(owner_rejection.find("err[\"state\"] = \"rejected\""), std::string::npos);
  EXPECT_NE(owner_rejection.find("err[\"code\"] = \"active_owner_required\""), std::string::npos);
}

TEST(DoctorResetContract, MediaCounterIngestAndStreamResetUseOneLockOrder) {
  const auto stats = source("src/stream_stats.cpp");
  const auto reset = between(
    stats,
    "void update_stream_active(bool active",
    "void add_client("
  );
  const auto ingest = between(
    stats,
    "client_media_ingest_result_t ingest_client_media_counters(",
    "#ifdef POLARIS_TESTS"
  );

  EXPECT_LT(reset.find("client_media_ingest_mutex"), reset.find("stats_mutex"));
  EXPECT_LT(ingest.find("frame_timing_mutex"), ingest.find("client_media_counter_mutex"));
  EXPECT_NE(ingest.find("active->session_token != sample.app_session_id"), std::string::npos);
  EXPECT_NE(ingest.find("client_media_ingest_state_e::scope_mismatch"), std::string::npos);
  EXPECT_LT(ingest.find("client_media_ingest_mutex"), ingest.find("client_media_counter_mutex"));
  EXPECT_LT(ingest.find("client_media_counter_mutex"), ingest.find("const auto host = get_current()"));
  EXPECT_NE(ingest.find("    }\n\n    // Serialize counter advancement"), std::string::npos)
    << "the counter mutex must be released before stats/network publication";
}

TEST(DoctorResetContract, ReconnectSerializesOldEvidenceResetWithNewDoctorScope) {
  const auto stream = source("src/stream.cpp");
  const auto join = between(
    stream,
    "void join(session_t &session)",
    "int start(session_t &session"
  );
  const auto start = between(
    stream,
    "int start(session_t &session",
    "std::shared_ptr<session_t> alloc("
  );

  const auto join_lock = join.find("stream_generation_boundary_mutex");
  const auto old_scope_retired = join.find("doctor_actions::session_ended");
  const auto old_count_retired = join.find("--running_sessions");
  const auto old_evidence_reset = join.find("stream_stats::update_stream_active(false)");
  ASSERT_NE(join_lock, std::string::npos);
  ASSERT_NE(old_scope_retired, std::string::npos);
  ASSERT_NE(old_count_retired, std::string::npos);
  ASSERT_NE(old_evidence_reset, std::string::npos);
  EXPECT_LT(join_lock, old_scope_retired);
  EXPECT_LT(old_scope_retired, old_count_retired);
  EXPECT_LT(old_count_retired, old_evidence_reset);

  const auto start_lock = start.find("stream_generation_boundary_mutex");
  const auto new_scope_started = start.find("doctor_actions::session_started");
  const auto new_count_started = start.find("++running_sessions");
  const auto start_unlock = start.find("generation_boundary_lock.unlock()");
  ASSERT_NE(start_lock, std::string::npos);
  ASSERT_NE(new_scope_started, std::string::npos);
  ASSERT_NE(new_count_started, std::string::npos);
  ASSERT_NE(start_unlock, std::string::npos);
  EXPECT_LT(start_lock, new_scope_started);
  EXPECT_LT(new_scope_started, new_count_started);
  EXPECT_LT(new_count_started, start_unlock);
}

TEST(DoctorResetContract, ResumeTimeoutCannotTerminateAcrossReconnectAdmission) {
  const auto stream = source("src/stream.cpp");
  const auto timeout = between(
    stream,
    "void schedule_disconnect_resume_timeout(std::string app_name)",
    "}  // namespace"
  );

  const auto conditional_stop = timeout.find("proc::proc.terminate_if(");
  const auto generation_check = timeout.find("disconnect_resume_timeout_generation.load");
  const auto active_check = timeout.find("session::running_sessions.load");
  ASSERT_NE(conditional_stop, std::string::npos);
  ASSERT_NE(generation_check, std::string::npos);
  ASSERT_NE(active_check, std::string::npos);
  EXPECT_EQ(timeout.find("session::stream_generation_boundary_mutex"), std::string::npos);
  EXPECT_EQ(timeout.find("proc::proc.terminate()"), std::string::npos);
  EXPECT_LT(generation_check, conditional_stop);
  EXPECT_LT(active_check, conditional_stop);

  const auto process = source("src/process.cpp");
  for (const auto &wrapper : {
         between(process,
           "bool proc_t::launch_input_only_and_raise(",
           "void proc_t::launch_input_only_impl("),
         between(process,
           "int proc_t::execute_and_raise(",
           "int proc_t::validate_resolved_profile_for_running_app("),
         between(process,
           "bool proc_t::raise_session_for_admitted_launch(",
           "std::optional<std::uint64_t> proc_t::capture_session_launch_generation(")
       }) {
    const auto raise = wrapper.find("rtsp_stream::launch_session_raise");
    const auto cancel = wrapper.find("cancel_disconnect_resume_timeout");
    ASSERT_NE(raise, std::string::npos);
    ASSERT_NE(cancel, std::string::npos);
    EXPECT_LT(raise, cancel);
  }

  const auto nvhttp = source("src/nvhttp.cpp");
  constexpr std::string_view lifecycle_binding =
    "launch_session->lifecycle_generation = *launch_generation;";
  const auto first_binding = nvhttp.find(lifecycle_binding);
  ASSERT_NE(first_binding, std::string::npos);
  const auto second_binding = nvhttp.find(lifecycle_binding, first_binding + 1);
  ASSERT_NE(second_binding, std::string::npos);
  EXPECT_EQ(nvhttp.find(lifecycle_binding, second_binding + 1), std::string::npos);

  const auto rtsp = source("src/rtsp.cpp");
  const auto setup = between(
    rtsp,
    "insert_start_result_e insert_and_start_if_not_cancelled(",
    "int run_setup_insert_for_tests("
  );
  const auto lifecycle_claim = setup.find("proc::proc.try_begin_rtsp_setup(");
  const auto slot_insert = setup.find("_session_slots->emplace(session)");
  const auto stream_start = setup.find("stream::session::start(");
  const auto lifecycle_release = setup.find("proc::proc.finish_rtsp_setup()");
  ASSERT_NE(lifecycle_claim, std::string::npos);
  ASSERT_NE(slot_insert, std::string::npos);
  ASSERT_NE(stream_start, std::string::npos);
  ASSERT_NE(lifecycle_release, std::string::npos);
  EXPECT_LT(lifecycle_claim, slot_insert);
  EXPECT_LT(lifecycle_claim, stream_start);
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

TEST(DoctorResetContract, LiveMediaTelemetryIsEvidenceOnlyAndGenerationBound) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto capabilities = between(
    nvhttp,
    "auto polarisCapabilities =",
    "auto polarisSessionTiming ="
  );
  EXPECT_NE(capabilities.find("live_media_telemetry_v1"), std::string::npos);

  const auto route = between(
    nvhttp,
    "auto polarisSessionTelemetry =",
    "// Paired, active-owner raw evidence ingress for Doctor v2 shadow mode."
  );
  EXPECT_NE(route.find("parse_request_stream_scope"), std::string::npos);
  EXPECT_NE(route.find("stop.owned_by_client"), std::string::npos);
  EXPECT_NE(route.find("requested_scope->app_session_id != stop.session_token"), std::string::npos);
  EXPECT_NE(route.find("requested_scope->session_generation != timing.session_generation"), std::string::npos);
  EXPECT_NE(route.find("stream_stats::ingest_client_media_counters"), std::string::npos);
  EXPECT_NE(route.find("frames_expected"), std::string::npos);
  EXPECT_NE(route.find("frames_received"), std::string::npos);
  EXPECT_NE(route.find("frames_lost"), std::string::npos);
  EXPECT_EQ(route.find("safe_settings"), route.rfind("safe_settings"))
    << "safe_settings may appear only in the explicit forbidden-field list";
  EXPECT_EQ(route.find("target_bitrate_kbps"), std::string::npos);
  EXPECT_EQ(route.find("rtt_ms"), std::string::npos)
    << "client RTT must not become live Doctor authority";

  EXPECT_NE(
    nvhttp.find("^/polaris/v1/session/telemetry$"),
    std::string::npos
  );
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

TEST(DoctorResetContract, SessionEncoderSelectionIsExactScopedAndRestored) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto optimize = between(
    nvhttp,
    "auto polarisOptimize =",
    "auto polarisClientSupportReport ="
  );
  EXPECT_NE(optimize.find("resolve_optimization_encoder("), std::string::npos);
  EXPECT_NE(optimize.find("invalid_or_unavailable_encoder"), std::string::npos);
  EXPECT_NE(optimize.find("*encoder_resolution"), std::string::npos);

  const auto resolved_output = between(
    nvhttp,
    "void append_deterministic_optimization_json(",
    "bool host_prefers_headless()"
  );
  EXPECT_NE(resolved_output.find("encoder_resolution"), std::string::npos);
  EXPECT_NE(resolved_output.find("encoder_options"), std::string::npos);
  EXPECT_NE(resolved_output.find("encoder_backend"), std::string::npos);

  const auto launch_parser = between(
    nvhttp,
    "launch_session->resolved_profile_from_client =",
    "std::stringstream mode;"
  );
  EXPECT_NE(launch_parser.find("encoderBackend"), std::string::npos);
  EXPECT_NE(launch_parser.find("expectedEncoder"), std::string::npos);
  EXPECT_NE(launch_parser.find("return nullptr;"), std::string::npos);

  const auto process = source("src/process.cpp");
  const auto execute = between(
    process,
    "int proc_t::execute_impl(",
    "int proc_t::running()"
  );
  const auto snapshot = execute.find("this->initial_encoder = config::video.encoder;");
  const auto apply = execute.find("config::video.encoder = session_encoder;");
  const auto reset = execute.find("video::reset_encoder_probe_state();", apply);
  const auto strict_probe = execute.find("video::probe_encoders(strict_session_encoder)");
  ASSERT_NE(snapshot, std::string::npos);
  ASSERT_NE(apply, std::string::npos);
  ASSERT_NE(reset, std::string::npos);
  ASSERT_NE(strict_probe, std::string::npos);
  EXPECT_LT(snapshot, apply);
  EXPECT_LT(apply, strict_probe);
  EXPECT_NE(
    execute.find("launch_session->enable_hdr &&\n          launch_session->host_hdr_capable == false"),
    std::string::npos
  ) << "an explicit encoder must be checked against HDR even for a legacy launch";

  const auto resume_validation = between(
    process,
    "int proc_t::validate_resolved_profile_for_running_app(",
    "int proc_t::running()"
  );
  EXPECT_NE(resume_validation.find("const auto live_active_encoder = video::active_encoder_name();"), std::string::npos);
  EXPECT_NE(resume_validation.find("_launch_session->effective_encoder_backend = active_effective_encoder;"), std::string::npos);

  const auto teardown = between(
    process,
    "void proc_t::terminate_impl(",
    "bool proc_t::reload_configuration_from_file("
  );
  EXPECT_NE(teardown.find("config::video.encoder = initial_encoder;"), std::string::npos);
  EXPECT_NE(teardown.find("video::reset_encoder_probe_state();"), std::string::npos);
}

TEST(DoctorResetContract, ClientSettingsPersistencePreservesExistingConfigAtomically) {
  const auto nvhttp = source("src/nvhttp.cpp");
  const auto persistence = between(
    nvhttp,
    "bool persist_config_values(",
    "using persist_config_values_fn_t"
  );

  EXPECT_EQ(persistence.find("file_handler::read_file"), std::string::npos);
  EXPECT_EQ(persistence.find("file_handler::write_file"), std::string::npos);
  EXPECT_NE(persistence.find("fs::symlink_status"), std::string::npos);
  EXPECT_NE(persistence.find("std::ifstream input"), std::string::npos);
  EXPECT_NE(persistence.find("input.bad()"), std::string::npos);
  EXPECT_NE(persistence.find("input.close()"), std::string::npos);
  EXPECT_NE(persistence.find("input.fail()"), std::string::npos);
  EXPECT_NE(persistence.find("const bool unchanged = std::all_of"), std::string::npos);
  EXPECT_NE(persistence.find("if (unchanged)"), std::string::npos);
  EXPECT_NE(persistence.find("config_file_update::apply"), std::string::npos);
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
    "void normalize_host_virtual_display_state_for_backend(",
    "bool host_virtual_backend_creates_output("
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

  const auto companion_match = between(
    policy,
    "bool selection_companion_state_matches(",
    "bool apply_selection("
  );
  EXPECT_NE(
    companion_match.find("host_virtual_connector_state_matches("),
    std::string::npos
  ) << "the final launch fast path must re-normalize if the live backend no longer owns a retained connector";

  const auto process = source("src/process.cpp");
  const auto actuator = between(
    process,
    "auto vdisplay = virtual_display::create(",
    "display_device::configure_display(config::video, *launch_session);"
  );
  const auto actual_backend_normalization = actuator.find(
    "normalize_host_virtual_display_state_for_backend("
  );
  const auto capture_identity = actuator.find(
    "config::video.output_name = stream_display_policy::capture_output_name_for_virtual_display("
  );
  ASSERT_NE(actual_backend_normalization, std::string::npos);
  ASSERT_NE(capture_identity, std::string::npos);
  EXPECT_LT(actual_backend_normalization, capture_identity)
    << "the created backend must retire stale connector authority before capture identity is published";
  EXPECT_NE(
    actuator.find("linux_vdisplay->backend", actual_backend_normalization),
    std::string::npos
  ) << "normalization must use the backend that actually created the display";
  EXPECT_NE(
    actuator.find("platf::reevaluate_capture_sources()", capture_identity),
    std::string::npos
  ) << "a backend-driven capture change must be applied before encoder probing";
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
  const auto doctor_explanation = between(
    config_http,
    "void explainDoctorWithAi(",
    "namespace {"
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
  EXPECT_NE(doctor_explanation.find("ai_optimizer::explain_doctor_json(evidence.dump())"), std::string::npos);
  EXPECT_EQ(doctor_explanation.find("body.value(\"provider\""), std::string::npos);
  EXPECT_EQ(doctor_explanation.find("ai_cfg.api_key"), std::string::npos);

  // The AI tab copy moved into the locale file too: the banned phrase is
  // checked in both places, and the "AI explanations" switch is pinned by its
  // locale key in the source and its English text in the locale.
  const auto ui = source("src_assets/common/assets/web/configs/tabs/AiOptimizer.vue");
  const auto ui_locale = source("src_assets/common/assets/web/public/assets/locale/en.json");
  EXPECT_EQ(ui.find("AI Auto Quality"), std::string::npos);
  EXPECT_EQ(ui_locale.find("AI Auto Quality"), std::string::npos);
  EXPECT_EQ(ui.find("testResult.payload.display_mode"), std::string::npos);
  EXPECT_EQ(ui.find("testResult.payload.target_bitrate_kbps"), std::string::npos);
  EXPECT_NE(ui.find("config.ai_explanations"), std::string::npos);
  EXPECT_NE(ui_locale.find("\"ai_explanations\": \"AI explanations\""), std::string::npos);

  // The Video/Audio page copy moved into the locale file, so the banned
  // legacy phrase is checked in both places and the required adaptive
  // explanation is pinned where it now lives, with its wiring in the source.
  const auto audio_video = source("src_assets/common/assets/web/configs/tabs/AudioVideo.vue");
  const auto av_locale = source("src_assets/common/assets/web/public/assets/locale/en.json");
  EXPECT_EQ(audio_video.find("AI Auto Quality"), std::string::npos);
  EXPECT_EQ(av_locale.find("AI Auto Quality"), std::string::npos);
  EXPECT_NE(av_locale.find("Adaptive bitrate"), std::string::npos);
  EXPECT_NE(audio_video.find("config.av_adaptive_range_title"), std::string::npos);
  EXPECT_EQ(
    audio_video.find("config.value.adaptive_bitrate_enabled === 'enabled' &&"),
    std::string::npos
  );
}
