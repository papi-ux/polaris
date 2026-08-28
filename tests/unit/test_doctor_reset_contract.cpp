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
