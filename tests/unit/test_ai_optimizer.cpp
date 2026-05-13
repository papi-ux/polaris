/**
 * @file tests/unit/test_ai_optimizer.cpp
 * @brief Unit tests for AI optimizer session feedback classification.
 */

#include "src/ai_optimizer.h"

#include <gtest/gtest.h>

#include <optional>

namespace {
  ai_optimizer::session_history_t make_session(const std::string &grade,
                                               const std::string &end_reason,
                                               int duration_s,
                                               int samples) {
    ai_optimizer::session_history_t session;
    session.avg_fps = grade == "F" ? 24.0 : 58.0;
    session.last_fps = session.avg_fps;
    session.last_target_fps = 60.0;
    session.quality_grade = grade;
    session.last_quality_grade = grade;
    session.last_end_reason = end_reason;
    session.last_duration_s = duration_s;
    session.last_sample_count = samples;
    return session;
  }
}

TEST(AiOptimizerSessionFeedback, DisconnectIsAcceptedButCannotInvalidateCache) {
  auto policy = ai_optimizer::classify_session_feedback(
    "Black Myth: Wukong",
    make_session("F", "disconnect", 120, 120)
  );

  EXPECT_TRUE(policy.accepted);
  EXPECT_EQ("low", policy.confidence);
  EXPECT_FALSE(policy.counts_poor_outcome);
  EXPECT_FALSE(policy.can_invalidate_cache);
}

TEST(AiOptimizerSessionFeedback, ExplicitEndRequiresEnoughSamplesForHighConfidence) {
  auto low_policy = ai_optimizer::classify_session_feedback(
    "Black Myth: Wukong",
    make_session("F", "end", 12, 4)
  );
  auto high_policy = ai_optimizer::classify_session_feedback(
    "Black Myth: Wukong",
    make_session("F", "end", 60, 60)
  );

  EXPECT_TRUE(low_policy.accepted);
  EXPECT_EQ("low", low_policy.confidence);
  EXPECT_FALSE(low_policy.counts_poor_outcome);

  EXPECT_TRUE(high_policy.accepted);
  EXPECT_EQ("high", high_policy.confidence);
  EXPECT_TRUE(high_policy.counts_poor_outcome);
  EXPECT_TRUE(high_policy.can_invalidate_cache);
}

TEST(AiOptimizerSessionFeedback, MissingMetricsAreIgnored) {
  auto session = make_session("F", "end", 60, 60);
  session.avg_fps = 0.0;
  session.last_fps = 0.0;

  auto policy = ai_optimizer::classify_session_feedback("Black Myth: Wukong", session);

  EXPECT_FALSE(policy.accepted);
  EXPECT_EQ("ignored", policy.confidence);
  EXPECT_EQ("missing_quality_metrics", policy.ignored_reason);
}

TEST(AiOptimizerSessionGrading, TreatsIsolatedMinDipAsMildWhenSustainedPacingIsHealthy) {
  auto session = make_session("D", "disconnect", 362, 358);
  session.avg_fps = 56.4689;
  session.last_fps = session.avg_fps;
  session.last_target_fps = 60.0;
  session.avg_latency_ms = 3.41;
  session.packet_loss_pct = 0.0;
  session.last_low_1_percent_fps = 52.0;
  session.last_min_fps = 13.07;
  session.last_frame_pacing_bad_pct = 1.4;

  EXPECT_EQ("B", ai_optimizer::grade_session_quality(session));
}

TEST(AiOptimizerSessionGrading, KeepsCorroboratedPacingCollapsePoor) {
  auto session = make_session("D", "disconnect", 180, 120);
  session.avg_fps = 53.0;
  session.last_fps = session.avg_fps;
  session.last_target_fps = 60.0;
  session.avg_latency_ms = 5.0;
  session.packet_loss_pct = 0.0;
  session.last_low_1_percent_fps = 35.0;
  session.last_min_fps = 12.0;
  session.last_frame_pacing_bad_pct = 8.0;

  EXPECT_EQ("D", ai_optimizer::grade_session_quality(session));
}

TEST(AiOptimizerHistorySanitization, ResetsCorruptedCounters) {
  auto session = make_session("B", "end", 60, 60);
  session.session_count = 651167748;
  session.poor_outcome_count = 382205952;
  session.consecutive_poor_outcomes = 221184;

  auto sanitized = ai_optimizer::sanitize_session_history(session);

  EXPECT_EQ(1, sanitized.session_count);
  EXPECT_EQ(0, sanitized.poor_outcome_count);
  EXPECT_EQ(0, sanitized.consecutive_poor_outcomes);
  EXPECT_EQ("B", sanitized.last_quality_grade);
}

TEST(AiOptimizerHistorySanitization, ClearsLowConfidenceSoftEndRecoveryBias) {
  auto session = make_session("C", "host_pause", 20, 8);
  session.session_count = 3;
  session.last_sample_confidence = "low";
  session.last_health_grade = "degraded";
  session.last_primary_issue = "host_render_limited";
  session.last_safe_target_fps = 30.0;
  session.last_relaunch_recommended = true;
  session.poor_outcome_count = 0;
  session.consecutive_poor_outcomes = 0;

  auto sanitized = ai_optimizer::sanitize_session_history(session);

  EXPECT_EQ(3, sanitized.session_count);
  EXPECT_DOUBLE_EQ(0.0, sanitized.last_safe_target_fps);
  EXPECT_FALSE(sanitized.last_relaunch_recommended);
  EXPECT_EQ("watch", sanitized.last_health_grade);
  EXPECT_EQ("host_render_limited", sanitized.last_primary_issue);
}

TEST(AiOptimizerHistorySanitization, KeepsSampledSoftEndRecoveryBias) {
  auto session = make_session("C", "disconnect", 180, 180);
  session.session_count = 3;
  session.last_sample_confidence = "low";
  session.last_health_grade = "watch";
  session.last_primary_issue = "host_render_limited";
  session.last_issues = {"host_render_limited"};
  session.last_safe_target_fps = 60.0;
  session.last_relaunch_recommended = true;
  session.poor_outcome_count = 0;
  session.consecutive_poor_outcomes = 0;

  auto sanitized = ai_optimizer::sanitize_session_history(session);

  EXPECT_EQ(3, sanitized.session_count);
  EXPECT_DOUBLE_EQ(60.0, sanitized.last_safe_target_fps);
  EXPECT_TRUE(sanitized.last_relaunch_recommended);
  EXPECT_EQ("watch", sanitized.last_health_grade);
  EXPECT_EQ("host_render_limited", sanitized.last_primary_issue);
}

TEST(AiOptimizerHistorySanitization, KeepsHighConfidenceRecoveryBias) {
  auto session = make_session("C", "end", 180, 180);
  session.session_count = 3;
  session.last_sample_confidence = "high";
  session.last_primary_issue = "host_render_limited";
  session.last_safe_target_fps = 30.0;
  session.last_relaunch_recommended = true;
  session.poor_outcome_count = 1;
  session.consecutive_poor_outcomes = 1;

  auto sanitized = ai_optimizer::sanitize_session_history(session);

  EXPECT_EQ(3, sanitized.session_count);
  EXPECT_EQ(1, sanitized.poor_outcome_count);
  EXPECT_EQ(1, sanitized.consecutive_poor_outcomes);
  EXPECT_DOUBLE_EQ(30.0, sanitized.last_safe_target_fps);
  EXPECT_TRUE(sanitized.last_relaunch_recommended);
}

TEST(AiOptimizerHistorySafe, RelaxesCompletedThirtyFpsTrial) {
  auto session = make_session("B", "host_pause", 180, 180);
  session.last_target_fps = 30.0;
  session.last_fps = 28.5;
  session.last_safe_target_fps = 30.0;
  session.last_optimization_source = "ai_cached+history_safe";

  EXPECT_TRUE(ai_optimizer::should_relax_history_safe_target_fps(session));
}

TEST(AiOptimizerHistorySafe, KeepsCapAfterSevereSafeTargetMiss) {
  auto session = make_session("F", "host_pause", 180, 180);
  session.last_target_fps = 30.0;
  session.last_fps = 12.0;
  session.last_safe_target_fps = 30.0;
  session.last_optimization_source = "ai_cached+history_safe";

  EXPECT_FALSE(ai_optimizer::should_relax_history_safe_target_fps(session));
}

TEST(AiOptimizerHistorySafe, KeepsCapAfterShortLowConfidenceSafeTrial) {
  auto session = make_session("A", "host_pause", 50, 20);
  session.last_target_fps = 30.0;
  session.last_fps = 30.0;
  session.last_safe_target_fps = 30.0;
  session.last_sample_confidence = "low";
  session.last_optimization_source = "ai_cached+history_safe";

  EXPECT_FALSE(ai_optimizer::should_relax_history_safe_target_fps(session));
}

TEST(AiOptimizerHistorySafe, IgnoresLowConfidenceInterruptedRetry) {
  auto session = make_session("B", "host_pause", 20, 8);
  session.last_target_fps = 60.0;
  session.last_fps = 58.0;
  session.last_safe_target_fps = 30.0;
  session.last_sample_confidence = "low";
  session.last_optimization_source = "ai_cached";

  EXPECT_FALSE(ai_optimizer::should_relax_history_safe_target_fps(session));
}

TEST(AiOptimizerHistorySafe, KeepsCapAfterLowConfidenceFramePacingMiss) {
  auto session = make_session("F", "host_pause", 0, 0);
  session.last_target_fps = 40.0;
  session.last_fps = 10.0;
  session.last_safe_target_fps = 30.0;
  session.last_sample_confidence = "low";
  session.last_primary_issue = "frame_pacing";
  session.last_optimization_source = "ai_cached";

  EXPECT_FALSE(ai_optimizer::should_relax_history_safe_target_fps(session));
}

TEST(AiOptimizerHistorySafe, KeepsCapAfterLowConfidenceHostRenderMiss) {
  auto session = make_session("F", "host_pause", 0, 0);
  session.last_target_fps = 40.0;
  session.last_fps = 10.0;
  session.last_safe_target_fps = 30.0;
  session.last_sample_confidence = "low";
  session.last_primary_issue = "host_render_limited";
  session.last_optimization_source = "ai_cached";

  EXPECT_FALSE(ai_optimizer::should_relax_history_safe_target_fps(session));
}

TEST(AiOptimizerHistorySafe, IgnoresUnconfirmedPoorSoftEndForRecoveryFallback) {
  auto session = make_session("F", "host_pause", 0, 0);
  session.session_count = 3;
  session.last_target_fps = 60.0;
  session.last_fps = 9.9;
  session.avg_fps = 58.0;
  session.last_safe_bitrate_kbps = 3900;
  session.last_safe_codec = "hevc";
  session.last_safe_display_mode = "headless";
  session.last_safe_target_fps = 24.0;
  session.last_sample_confidence = "low";
  session.last_primary_issue = "host_render_limited";
  session.last_issues = {"frame_pacing", "host_render_limited"};
  session.last_relaunch_recommended = true;
  session.poor_outcome_count = 0;
  session.consecutive_poor_outcomes = 0;

  EXPECT_DOUBLE_EQ(
    0.0,
    ai_optimizer::effective_history_safe_target_fps("RetroidPocket6", session)
  );
  EXPECT_FALSE(
    ai_optimizer::get_history_safe_fallback(
      "RetroidPocket6",
      "Superposition Baseline",
      std::optional<ai_optimizer::session_history_t> {session}
    ).has_value()
  );
}

TEST(AiOptimizerHistorySafe, UsesSampledSoftEndForRecoveryFallback) {
  auto session = make_session("F", "host_pause", 180, 180);
  session.session_count = 3;
  session.last_target_fps = 120.0;
  session.last_fps = 38.0;
  session.avg_fps = 38.0;
  session.last_safe_bitrate_kbps = 13387;
  session.last_safe_codec = "hevc";
  session.last_safe_display_mode = "headless";
  session.last_safe_target_fps = 30.0;
  session.last_sample_confidence = "low";
  session.last_primary_issue = "host_render_limited";
  session.last_issues = {"host_render_limited"};
  session.last_relaunch_recommended = true;
  session.poor_outcome_count = 0;
  session.consecutive_poor_outcomes = 0;

  EXPECT_DOUBLE_EQ(
    30.0,
    ai_optimizer::effective_history_safe_target_fps("RetroidPocket6", session)
  );
  EXPECT_TRUE(
    ai_optimizer::get_history_safe_fallback(
      "RetroidPocket6",
      "Superposition Quality 120",
      std::optional<ai_optimizer::session_history_t> {session}
    ).has_value()
  );
}

TEST(AiOptimizerHistorySafe, UsesSampledHostRenderWatchForRecoveryFallback) {
  auto session = make_session("C", "host_pause", 180, 180);
  session.session_count = 3;
  session.last_target_fps = 120.0;
  session.last_fps = 94.0;
  session.avg_fps = 94.0;
  session.last_safe_bitrate_kbps = 16000;
  session.last_safe_codec = "hevc";
  session.last_safe_display_mode = "headless";
  session.last_safe_target_fps = 60.0;
  session.last_sample_confidence = "low";
  session.last_health_grade = "watch";
  session.last_primary_issue = "host_render_limited";
  session.last_issues = {"host_render_limited"};
  session.last_relaunch_recommended = true;
  session.poor_outcome_count = 0;
  session.consecutive_poor_outcomes = 0;

  EXPECT_TRUE(
    ai_optimizer::get_history_safe_fallback(
      "RetroidPocket6",
      "Superposition Quality 120",
      std::optional<ai_optimizer::session_history_t> {session}
    ).has_value()
  );
}

TEST(AiOptimizerHistorySafe, UsesHighConfidenceHostRenderRecoveryFallback) {
  auto session = make_session("C", "end", 180, 180);
  session.session_count = 3;
  session.last_target_fps = 120.0;
  session.last_fps = 94.0;
  session.avg_fps = 94.0;
  session.last_safe_bitrate_kbps = 16000;
  session.last_safe_codec = "hevc";
  session.last_safe_display_mode = "headless";
  session.last_safe_target_fps = 60.0;
  session.last_sample_confidence = "high";
  session.last_health_grade = "watch";
  session.last_primary_issue = "host_render_limited";
  session.last_issues = {"host_render_limited"};
  session.last_relaunch_recommended = true;
  session.poor_outcome_count = 0;
  session.consecutive_poor_outcomes = 0;

  EXPECT_TRUE(
    ai_optimizer::get_history_safe_fallback(
      "RetroidPocket6",
      "Superposition Quality 120",
      std::optional<ai_optimizer::session_history_t> {session}
    ).has_value()
  );
}

TEST(AiOptimizerHistorySafe, KeepsCapAfterLongSampledDisconnect) {
  auto session = make_session("D", "disconnect", 180, 180);
  session.last_target_fps = 60.0;
  session.last_fps = 53.0;
  session.last_safe_target_fps = 30.0;
  session.last_sample_confidence = "low";
  session.last_optimization_source = "ai_cached";

  EXPECT_FALSE(ai_optimizer::should_relax_history_safe_target_fps(session));
}

TEST(AiOptimizerHistorySafe, GraduatesStaleCapAfterSuccessfulHighFpsRetry) {
  auto session = make_session("D", "disconnect", 180, 180);
  session.avg_fps = 118.0;
  session.last_fps = 118.0;
  session.last_target_fps = 120.0;
  session.last_latency_ms = 3.5;
  session.last_packet_loss_pct = 0.0;
  session.last_low_1_percent_fps = 92.0;
  session.last_min_fps = 72.0;
  session.last_frame_pacing_bad_pct = 6.0;
  session.last_capture_path = "headless";
  session.last_network_risk = "normal";
  session.last_decoder_risk = "normal";
  session.last_hdr_risk = "normal";

  EXPECT_TRUE(
    ai_optimizer::should_graduate_history_safe_target_fps(session, 30.0)
  );
}

TEST(AiOptimizerHistorySafe, KeepsStaleCapWhenHighFpsRetryStillMissesTarget) {
  auto session = make_session("D", "disconnect", 180, 180);
  session.avg_fps = 82.0;
  session.last_fps = 82.0;
  session.last_target_fps = 120.0;
  session.last_latency_ms = 3.5;
  session.last_packet_loss_pct = 0.0;
  session.last_capture_path = "headless";

  EXPECT_FALSE(
    ai_optimizer::should_graduate_history_safe_target_fps(session, 30.0)
  );
}

TEST(AiOptimizerHistorySafe, RefreshesStaleRecoveryCacheAfterStableThirtyFpsTrial) {
  auto session = make_session("A", "disconnect", 414, 404);
  session.last_target_fps = 30.0;
  session.last_fps = 29.7;
  session.last_low_1_percent_fps = 29.0;
  session.last_min_fps = 29.1;
  session.last_latency_ms = 3.7;
  session.last_sample_confidence = "low";
  session.last_primary_issue = "nvenc_cuda_disabled";
  session.last_issues = {"headless_shm_fallback", "nvenc_cuda_disabled"};

  device_db::optimization_t optimization;
  optimization.display_mode = "1920x1080x30";
  optimization.confidence = "low";
  optimization.normalization_reason = "Lowered stream FPS from session pacing feedback.";

  EXPECT_TRUE(
    ai_optimizer::should_refresh_recovery_cache_after_stable_trial(session, optimization)
  );
}

TEST(AiOptimizerHistorySafe, KeepsIntentionalThirtyFpsQualityCache) {
  auto session = make_session("A", "disconnect", 414, 404);
  session.last_target_fps = 30.0;
  session.last_fps = 29.7;
  session.last_low_1_percent_fps = 29.0;
  session.last_min_fps = 29.1;
  session.last_latency_ms = 3.7;

  device_db::optimization_t optimization;
  optimization.display_mode = "1920x1080x30";
  optimization.confidence = "medium";
  optimization.reasoning = "Latest session earned A at 30 FPS, so keep the proven 1080p30 stream.";

  EXPECT_FALSE(
    ai_optimizer::should_refresh_recovery_cache_after_stable_trial(session, optimization)
  );
}

TEST(AiOptimizerSafeTargetFps, UsesStableFortyForModerateMobilePacingMiss) {
  EXPECT_DOUBLE_EQ(
    40.0,
    ai_optimizer::derive_safe_target_fps(
      60.0,
      52.0,
      0.0,
      0.0,
      8.0,
      true,
      false,
      true
    )
  );
}

TEST(AiOptimizerSafeTargetFps, KeepsThirtyForSevereMobilePacingMiss) {
  EXPECT_DOUBLE_EQ(
    30.0,
    ai_optimizer::derive_safe_target_fps(
      60.0,
      43.0,
      0.0,
      0.0,
      18.0,
      true,
      false,
      true
    )
  );
}

TEST(AiOptimizerSafeTargetFps, UsesSixtyForRecoverableHighRefreshMiss) {
  EXPECT_DOUBLE_EQ(
    60.0,
    ai_optimizer::derive_safe_target_fps(
      120.0,
      94.0,
      0.0,
      0.0,
      8.0,
      true,
      false,
      true
    )
  );
}

TEST(AiOptimizerSafeTargetFps, UsesThirtyForSevereHighRefreshMiss) {
  EXPECT_DOUBLE_EQ(
    30.0,
    ai_optimizer::derive_safe_target_fps(
      120.0,
      42.0,
      0.0,
      0.0,
      18.0,
      true,
      false,
      true
    )
  );
}

TEST(AiOptimizerSafeTargetFps, UpgradesLegacyThirtyCapWhenModerateMissCanHoldForty) {
  auto session = make_session("B", "end", 180, 180);
  session.avg_fps = 52.0;
  session.last_fps = 52.0;
  session.last_target_fps = 60.0;
  session.last_safe_target_fps = 30.0;
  session.last_frame_pacing_bad_pct = 8.0;
  session.last_primary_issue = "host_render_limited";

  EXPECT_DOUBLE_EQ(
    40.0,
    ai_optimizer::effective_history_safe_target_fps("RetroidPocket6", session)
  );
}
