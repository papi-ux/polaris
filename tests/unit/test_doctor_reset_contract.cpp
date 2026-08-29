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
  EXPECT_NE(live_route.find("adaptive_bitrate::set_live_bitrate"), std::string::npos);
  EXPECT_EQ(live_route.find("adaptive_bitrate::set_base_bitrate"), std::string::npos);

  const auto video = source("src/video.cpp");
  const auto runtime_update = between(
    video,
    "// Check adaptive bitrate and update encoder if target has changed",
    "stream_stats::update_frame_delivery("
  );
  EXPECT_NE(runtime_update.find("adaptive_bitrate::is_active()"), std::string::npos);
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
  EXPECT_NE(handler.find("advertised_max_launch_refresh_rate_for_http"), std::string::npos);
  EXPECT_NE(handler.find("advertised_codec_support_for_http"), std::string::npos);
  EXPECT_NE(handler.find("host_hdr_capable"), std::string::npos);
}

TEST(DoctorResetContract, ResolvedLaunchFailsClosedWhenHostCapsChanged) {
  const auto launch_parser = between(
    source("src/nvhttp.cpp"),
    "if (launch_session->resolved_profile_from_client) {",
    "launch_session->watch_only ="
  );
  EXPECT_NE(launch_parser.find("above the current host refresh cap"), std::string::npos);
  EXPECT_NE(launch_parser.find("above the configured host bitrate cap"), std::string::npos);
  EXPECT_NE(launch_parser.find("current encoder lacks HDR support"), std::string::npos);
  EXPECT_NE(launch_parser.find("return nullptr"), std::string::npos);
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
