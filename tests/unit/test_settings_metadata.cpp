/**
 * @file tests/unit/test_settings_metadata.cpp
 * @brief Test shared settings metadata builders.
 */

#include <src/adaptive_bitrate.h>
#include <src/settings_metadata.h>
#include <src/stream_stats.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <string>

namespace {
  adaptive_bitrate::state_t make_adaptive_state() {
    adaptive_bitrate::state_t state;
    state.enabled = true;
    state.active = true;
    state.runtime_update_supported = true;
    state.base_bitrate_kbps = 9000;
    state.target_bitrate_kbps = 7000;
    state.min_bitrate_kbps = 2000;
    state.max_bitrate_kbps = 100000;
    state.ewma_packet_loss = 0.25;
    state.ewma_rtt_ms = 18.5;
    state.state = "recovering";
    state.reason = "packet loss cleared";
    return state;
  }
}  // namespace

TEST(SettingsMetadataTests, AutoQualityBlockedReasonMapsLimitingFactors) {
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("host_render"), "host_render_limited");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("network"), "network");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("encoder"), "encoder");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("decoder"), "decoder");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("pacing"), "insufficient_signal");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("capture"), "insufficient_signal");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("hdr"), "insufficient_signal");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("none"), "none");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason(""), "none");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("something_else"), "none");
  // The mapping normalizes case before matching.
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("HOST_RENDER"), "host_render_limited");
  EXPECT_EQ(settings_metadata::auto_quality_blocked_reason("Network"), "network");
}

TEST(SettingsMetadataTests, AutoQualityPolicyReportsOffStateEvenUnderPressure) {
  // ai_auto_quality_enabled() is a hardcoded legacy false on this master, so
  // every reachable policy lands in the "off" state regardless of health
  // pressure or adaptive recovery signals. Pin that contract.
  const nlohmann::json health {
    {"limiting_factor", "network"},
    {"relaunch_recommended", true},
    {"summary", "Network pressure detected."},
    {"safe_bitrate_kbps", 5000},
    {"safe_display_mode", "headless_stream"},
    {"safe_target_fps", 60},
  };
  const auto adaptive_state = make_adaptive_state();
  const auto policy = settings_metadata::build_auto_quality_policy_json(health, adaptive_state, 20000);

  EXPECT_FALSE(policy.at("enabled").get<bool>());
  EXPECT_EQ(policy.at("state").get<std::string>(), "off");
  EXPECT_EQ(policy.at("blocked_reason").get<std::string>(), "none");
  EXPECT_EQ(policy.at("summary").get<std::string>(), "Manual stream tuning is active.");
  EXPECT_EQ(policy.at("detail").get<std::string>(), "Network pressure detected.");
  EXPECT_EQ(policy.at("live_bitrate_kbps").get<int>(), 7000);
  EXPECT_EQ(policy.at("quality_cap_kbps").get<int>(), 9000);
  EXPECT_FALSE(policy.at("relaunch_required").get<bool>());
  EXPECT_FALSE(policy.at("can_recover_live").get<bool>());
  EXPECT_FALSE(policy.contains("suggested_profile"));

  const auto &components = policy.at("components");
  EXPECT_FALSE(components.at("optimizer_active").get<bool>());
  EXPECT_TRUE(components.at("adaptive_bitrate_active").is_boolean());
  EXPECT_EQ(components.at("adaptive_state").get<std::string>(), "recovering");
  EXPECT_EQ(components.at("adaptive_reason").get<std::string>(), "packet loss cleared");
  EXPECT_EQ(components.at("target_bitrate_kbps").get<int>(), 7000);
}

TEST(SettingsMetadataTests, AutoQualityPolicyFallsBackToEncoderBitrate) {
  const adaptive_bitrate::state_t adaptive_state;
  const auto policy = settings_metadata::build_auto_quality_policy_json(
    nlohmann::json::object(),
    adaptive_state,
    15000
  );

  EXPECT_EQ(policy.at("state").get<std::string>(), "off");
  EXPECT_EQ(policy.at("live_bitrate_kbps").get<int>(), 15000);
  EXPECT_EQ(policy.at("quality_cap_kbps").get<int>(), 15000);
  // With no health summary the detail falls back to the state summary.
  EXPECT_EQ(policy.at("detail").get<std::string>(), "Manual stream tuning is active.");
  EXPECT_FALSE(policy.contains("suggested_profile"));
}

TEST(SettingsMetadataTests, BuildTuningJsonMirrorsAdaptiveStateAndSnapshotFlags) {
  const auto adaptive_state = make_adaptive_state();
  stream_stats::stats_t stats;
  stats.adaptive_target_bitrate_kbps = 7500;

  const auto tuning = settings_metadata::build_tuning_json(adaptive_state, stats, true);

  EXPECT_EQ(tuning.size(), 14u);
  EXPECT_EQ(tuning.at("adaptive_bitrate_enabled").get<bool>(), adaptive_bitrate::is_enabled());
  EXPECT_TRUE(tuning.at("adaptive_bitrate_active").get<bool>());
  EXPECT_TRUE(tuning.at("adaptive_runtime_update_supported").get<bool>());
  EXPECT_EQ(tuning.at("adaptive_target_bitrate_kbps").get<int>(), 7500);
  EXPECT_EQ(tuning.at("adaptive_base_bitrate_kbps").get<int>(), 9000);
  EXPECT_EQ(tuning.at("adaptive_min_bitrate_kbps").get<int>(), 2000);
  EXPECT_EQ(tuning.at("adaptive_max_bitrate_kbps").get<int>(), 100000);
  EXPECT_EQ(tuning.at("adaptive_bitrate_state").get<std::string>(), "recovering");
  EXPECT_EQ(tuning.at("adaptive_bitrate_reason").get<std::string>(), "packet loss cleared");
  EXPECT_DOUBLE_EQ(tuning.at("adaptive_packet_loss_ewma").get<double>(), 0.25);
  EXPECT_DOUBLE_EQ(tuning.at("adaptive_rtt_ewma_ms").get<double>(), 18.5);
  EXPECT_FALSE(tuning.at("ai_auto_quality_enabled").get<bool>());
  EXPECT_FALSE(tuning.at("ai_optimizer_enabled").get<bool>());
  EXPECT_TRUE(tuning.at("mangohud_configured").get<bool>());

  const auto without_mangohud = settings_metadata::build_tuning_json(adaptive_state, stats, false);
  EXPECT_FALSE(without_mangohud.at("mangohud_configured").get<bool>());
}

TEST(SettingsMetadataTests, StreamDisplayModeOptionsCarryBadges) {
  const auto modes = settings_metadata::stream_display_mode_options_json();
  ASSERT_TRUE(modes.is_array());
  ASSERT_FALSE(modes.empty());
  bool saw_headless = false;
  for (const auto &mode : modes) {
    ASSERT_TRUE(mode.contains("badge")) << mode.value("value", std::string {});
    EXPECT_TRUE(mode.at("badge").is_string());
    if (mode.value("value", std::string {}) == "headless_stream") {
      saw_headless = true;
      EXPECT_EQ(mode.at("badge").get<std::string>(), "Recommended");
    }
  }
  EXPECT_TRUE(saw_headless);
}
