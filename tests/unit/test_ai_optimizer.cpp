/**
 * @file tests/unit/test_ai_optimizer.cpp
 * @brief Unit tests for AI optimizer session feedback classification.
 */

#include "src/ai_optimizer.h"
#include "src/owned_child.h"
#include "src/platform/common.h"
#include "src/stream_stats.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <unistd.h>
#include <nlohmann/json.hpp>

#ifdef __linux__
  #include <sys/stat.h>
extern char **environ;
#endif

namespace {
#ifndef _WIN32
  class scoped_fake_codex_t {
  public:
    scoped_fake_codex_t() {
      if (const char *home = std::getenv("HOME")) {
        previous_home_ = home;
      }
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      root_ = std::filesystem::temp_directory_path() /
        ("polaris-ai-doctor-test-" + std::to_string(stamp));
      const auto bin = root_ / ".local" / "bin";
      std::error_code ec;
      std::filesystem::create_directories(bin, ec);
      if (ec) return;

      const auto script = bin / "codex";
      std::ofstream output(script, std::ios::binary | std::ios::trunc);
      output << R"SH(#!/bin/sh
output_file=''
schema_file=''
while [ "$#" -gt 0 ]; do
  case "$1" in
    -o) output_file="$2"; shift 2 ;;
    --output-schema) schema_file="$2"; shift 2 ;;
    *) shift ;;
  esac
done
[ -n "$output_file" ] || exit 10
[ -f "$schema_file" ] || exit 11
prompt="$(cat)"
printf '%s' "$prompt" | grep -q 'destructive_action_allowed' || exit 12
cat > "$output_file" <<'JSON'
{"likely_cause":"Frame pacing pressure","evidence":["Doctor reported frame_pacing"],"try_first":["Use the deterministic Doctor fix"],"advanced_detail":"Explanation only; Doctor retains authority.","confidence":"high","destructive_action_allowed":false}
JSON
)SH";
      output.close();
      if (!output.good()) return;
      std::filesystem::permissions(
        script,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        ec);
      if (ec || setenv("HOME", root_.c_str(), 1) != 0) return;
      ready_ = true;
    }

    scoped_fake_codex_t(const scoped_fake_codex_t &) = delete;
    scoped_fake_codex_t &operator=(const scoped_fake_codex_t &) = delete;

    ~scoped_fake_codex_t() {
      if (previous_home_) {
        (void) setenv("HOME", previous_home_->c_str(), 1);
      } else {
        (void) unsetenv("HOME");
      }
      std::error_code ec;
      std::filesystem::remove_all(root_, ec);
    }

    bool ready() const {
      return ready_;
    }

  private:
    std::filesystem::path root_;
    std::optional<std::string> previous_home_;
    bool ready_ = false;
  };
#endif

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

// Never call ai_optimizer::clear_history() here: appdata() resolves once per
// process, so it would remove the real history file of whoever runs the tests.
TEST(AiOptimizerSessionHistory, ClearHistoryAtRemovesTheGivenFileAndForgetsEntries) {
  const auto history_file = std::filesystem::temp_directory_path() /
    ("polaris-ai-history-test-" + std::to_string(::getpid()) + ".json");
  {
    std::ofstream out(history_file);
    out << "{\"device:game\":{\"avg_fps\":60}}";
  }
  ASSERT_TRUE(std::filesystem::exists(history_file));
  // get_history_json() loads the persisted history once per process; trigger
  // that load first so the clear below is what leaves the list empty.
  (void) ai_optimizer::get_history_json();

  ai_optimizer::clear_history_at(history_file);

  EXPECT_FALSE(std::filesystem::exists(history_file));
  const auto history = nlohmann::json::parse(ai_optimizer::get_history_json());
  EXPECT_TRUE(history.is_array());
  EXPECT_TRUE(history.empty());
  // Idempotent: clearing an already-missing file is not an error.
  ai_optimizer::clear_history_at(history_file);
  EXPECT_FALSE(std::filesystem::exists(history_file));
}

TEST(AiOptimizerCodexHome, UsesExplicitCodexHomeWhenConfigured) {
  auto resolved = ai_optimizer::resolve_codex_home_for_subscription(
    "/tmp/polaris-profile-home",
    "/tmp/codex-home"
  );

  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ("/tmp/codex-home", *resolved);
}

TEST(AiOptimizerCodexHome, FallsBackToRuntimeHomeCodexDirectory) {
  auto resolved = ai_optimizer::resolve_codex_home_for_subscription(
    "/tmp/polaris-profile-home",
    ""
  );

  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ("/tmp/polaris-profile-home/.codex", *resolved);
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

TEST(AiOptimizerSessionGrading, UsesRelativeToleranceForMeaningfulFpsShortfall) {
  EXPECT_FALSE(stream_stats::is_meaningful_fps_shortfall(120.0, 115.6));
  EXPECT_FALSE(stream_stats::is_meaningful_fps_shortfall(120.0, 114.0));
  EXPECT_TRUE(stream_stats::is_meaningful_fps_shortfall(120.0, 113.9));
  EXPECT_TRUE(stream_stats::is_meaningful_fps_shortfall(60.0, 54.0));
  EXPECT_TRUE(stream_stats::is_meaningful_fps_shortfall(30.0, 28.0));
  EXPECT_FALSE(stream_stats::is_meaningful_fps_shortfall(0.0, 115.6));
  EXPECT_FALSE(stream_stats::is_meaningful_fps_shortfall(120.0, 0.0));
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

TEST(AiOptimizerHistorySanitization, RepairsHealthyNearTargetHostLimit) {
  auto session = make_session("A", "disconnect", 130, 11);
  session.avg_fps = 115.6;
  session.last_fps = session.avg_fps;
  session.last_target_fps = 120.0;
  session.avg_latency_ms = 0.0;
  session.packet_loss_pct = 0.0;
  session.last_low_1_percent_fps = 110.78;
  session.last_min_fps = 110.78;
  session.last_frame_pacing_bad_pct = 0.0;
  session.last_health_grade = "watch";
  session.last_primary_issue = "host_render_limited";
  session.last_issues = {"host_render_limited", "frame_pacing"};
  session.last_safe_target_fps = 60.0;
  session.last_relaunch_recommended = true;

  auto sanitized = ai_optimizer::sanitize_session_history(session);

  EXPECT_EQ("A", ai_optimizer::grade_session_quality(sanitized));
  EXPECT_EQ("good", sanitized.last_health_grade);
  EXPECT_EQ("steady", sanitized.last_primary_issue);
  EXPECT_TRUE(sanitized.last_issues.empty());
  EXPECT_DOUBLE_EQ(0.0, sanitized.last_safe_target_fps);
  EXPECT_FALSE(sanitized.last_relaunch_recommended);
  EXPECT_DOUBLE_EQ(
    0.0,
    ai_optimizer::effective_history_safe_target_fps("RetroidPocket6", sanitized)
  );
}

TEST(AiOptimizerHistorySanitization, KeepsHostLimitWhenTargetTelemetryIsMissing) {
  auto session = make_session("A", "end", 130, 11);
  session.avg_fps = 115.6;
  session.last_fps = 115.6;
  session.last_target_fps = 0.0;
  session.last_low_1_percent_fps = 110.8;
  session.last_min_fps = 110.8;
  session.last_frame_pacing_bad_pct = 0.0;
  session.last_health_grade = "watch";
  session.last_primary_issue = "host_render_limited";
  session.last_issues = {"frame_pacing", "host_render_limited"};
  session.last_safe_target_fps = 60.0;
  session.last_relaunch_recommended = true;

  const auto sanitized = ai_optimizer::sanitize_session_history(session);

  EXPECT_EQ("host_render_limited", sanitized.last_primary_issue);
  EXPECT_DOUBLE_EQ(60.0, sanitized.last_safe_target_fps);
  EXPECT_TRUE(sanitized.last_relaunch_recommended);
}

TEST(AiOptimizerHistorySanitization, KeepsCorroboratedPacingFailureAtHighRefresh) {
  auto session = make_session("C", "disconnect", 130, 100);
  session.avg_fps = 115.6;
  session.last_fps = session.avg_fps;
  session.last_target_fps = 120.0;
  session.avg_latency_ms = 8.0;
  session.packet_loss_pct = 0.0;
  session.last_low_1_percent_fps = 72.0;
  session.last_min_fps = 45.0;
  session.last_frame_pacing_bad_pct = 10.0;
  session.last_health_grade = "watch";
  session.last_primary_issue = "frame_pacing";
  session.last_issues = {"frame_pacing"};
  session.last_safe_target_fps = 60.0;
  session.last_relaunch_recommended = true;

  auto sanitized = ai_optimizer::sanitize_session_history(session);

  EXPECT_EQ("D", ai_optimizer::grade_session_quality(sanitized));
  EXPECT_EQ("watch", sanitized.last_health_grade);
  EXPECT_EQ("frame_pacing", sanitized.last_primary_issue);
  EXPECT_EQ(std::vector<std::string>({"frame_pacing"}), sanitized.last_issues);
  EXPECT_DOUBLE_EQ(60.0, sanitized.last_safe_target_fps);
  EXPECT_TRUE(sanitized.last_relaunch_recommended);
}

TEST(AiOptimizerSafeTargetFps, KeepsMeaningfulSixtyFpsShortfallRecoverable) {
  auto session = make_session("B", "disconnect", 130, 100);
  session.avg_fps = 54.0;
  session.last_fps = session.avg_fps;
  session.last_target_fps = 60.0;
  session.last_low_1_percent_fps = 45.0;
  session.last_min_fps = 42.0;
  session.last_frame_pacing_bad_pct = 5.0;

  EXPECT_DOUBLE_EQ(
    40.0,
    ai_optimizer::derive_safe_target_fps(
      session.last_target_fps,
      session.last_fps,
      session.last_low_1_percent_fps,
      session.last_min_fps,
      session.last_frame_pacing_bad_pct,
      true,
      false,
      true
    )
  );
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

TEST(AiOptimizerHistorySanitization, RepairsLegacyControlLossGradeAndNetworkLimit) {
  auto session = make_session("C", "host_pause", 0, 0);
  session.session_count = 3;
  session.avg_fps = 59.71;
  session.last_fps = 59.71;
  session.last_target_fps = 60.0;
  session.avg_latency_ms = 4.0;
  session.last_latency_ms = 4.0;
  session.packet_loss_pct = 3.2;
  session.last_packet_loss_pct = 8.72039794921875;
  session.last_sample_confidence = "low";
  session.last_health_grade = "watch";
  session.last_primary_issue = "network_jitter";
  session.last_issues = {"network_jitter"};
  session.last_network_risk = "elevated";
  session.last_bitrate_kbps = 16988;
  session.last_safe_bitrate_kbps = 12741;

  const auto sanitized = ai_optimizer::sanitize_session_history(session);

  EXPECT_EQ(sanitized.last_packet_loss_source, "legacy_control_channel");
  EXPECT_DOUBLE_EQ(sanitized.packet_loss_pct, 0.0);
  EXPECT_DOUBLE_EQ(sanitized.last_packet_loss_pct, 0.0);
  EXPECT_EQ(ai_optimizer::grade_session_quality(sanitized), "A");
  EXPECT_EQ(sanitized.quality_grade, "A");
  EXPECT_EQ(sanitized.last_quality_grade, "A");
  EXPECT_EQ(sanitized.last_network_risk, "normal");
  EXPECT_EQ(sanitized.last_primary_issue, "steady");
  EXPECT_TRUE(sanitized.last_issues.empty());
  EXPECT_EQ(sanitized.last_health_grade, "good");
  EXPECT_EQ(sanitized.last_safe_bitrate_kbps, 0);
  EXPECT_FALSE(
    ai_optimizer::get_history_safe_fallback(
      "RetroidPocket6",
      "Legacy Control Loss",
      std::optional<ai_optimizer::session_history_t> {sanitized}
    ).has_value()
  );
}

TEST(AiOptimizerHistorySanitization, KeepsRealRttPressureWhileDiscardingControlLoss) {
  auto session = make_session("D", "host_pause", 0, 0);
  session.avg_fps = 59.71;
  session.last_fps = 59.71;
  session.last_target_fps = 60.0;
  session.avg_latency_ms = 52.0;
  session.last_latency_ms = 52.0;
  session.packet_loss_pct = 8.7;
  session.last_packet_loss_pct = 8.7;
  session.last_health_grade = "watch";
  session.last_primary_issue = "network_jitter";
  session.last_issues = {"network_jitter"};
  session.last_network_risk = "elevated";

  const auto sanitized = ai_optimizer::sanitize_session_history(session);

  EXPECT_EQ(sanitized.last_packet_loss_source, "legacy_control_channel");
  EXPECT_DOUBLE_EQ(sanitized.packet_loss_pct, 0.0);
  EXPECT_EQ(sanitized.last_network_risk, "elevated");
  EXPECT_EQ(sanitized.last_primary_issue, "network_jitter");
  EXPECT_EQ(sanitized.last_health_grade, "watch");
  EXPECT_EQ(ai_optimizer::grade_session_quality(sanitized), "C");
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

TEST(AiOptimizerHistorySafe, RefreshesStaleRecoverySixtyFpsCacheAfterStableTrial) {
  // A 120-native host downgraded to a recovery-60 profile: the old 45 FPS gate
  // made this cache unretirable because no stable trial at its own 60 target
  // could ever qualify.
  auto session = make_session("A", "disconnect", 414, 404);
  session.last_target_fps = 60.0;
  session.last_fps = 59.5;
  session.last_low_1_percent_fps = 57.5;
  session.last_min_fps = 55.0;
  session.last_latency_ms = 3.7;

  device_db::optimization_t optimization;
  optimization.display_mode = "1920x1080x60";
  optimization.confidence = "low";
  optimization.normalization_reason = "Lowered stream FPS from session pacing feedback.";

  EXPECT_TRUE(
    ai_optimizer::should_refresh_recovery_cache_after_stable_trial(session, optimization)
  );
}

TEST(AiOptimizerHistorySafe, RefreshesRecoveryCacheWhenLegacyLossAverageIsModest) {
  // Legacy history without the tracker's debounced verdict: a 0.5% average on an
  // ENet reliable channel is ordinary, and the retired 0.35% hair-trigger must
  // not keep vetoing retirement forever.
  auto session = make_session("A", "disconnect", 414, 404);
  session.last_target_fps = 30.0;
  session.last_fps = 29.7;
  session.last_low_1_percent_fps = 29.0;
  session.last_min_fps = 29.1;
  session.last_latency_ms = 3.7;
  session.last_packet_loss_pct = 0.5;

  device_db::optimization_t optimization;
  optimization.display_mode = "1920x1080x30";
  optimization.confidence = "low";
  optimization.normalization_reason = "Lowered stream FPS from session pacing feedback.";

  EXPECT_TRUE(
    ai_optimizer::should_refresh_recovery_cache_after_stable_trial(session, optimization)
  );
}

TEST(AiOptimizerHistorySafe, KeepsRecoveryCacheWhenTrackerFlagsNetworkElevated) {
  // The debounced tracker verdict outranks clean-looking averages: when it says
  // elevated, the trial does not count as stable no matter the numbers.
  auto session = make_session("A", "disconnect", 414, 404);
  session.last_target_fps = 30.0;
  session.last_fps = 29.7;
  session.last_low_1_percent_fps = 29.0;
  session.last_min_fps = 29.1;
  session.last_latency_ms = 3.7;
  session.last_network_risk = "elevated";

  device_db::optimization_t optimization;
  optimization.display_mode = "1920x1080x30";
  optimization.confidence = "low";
  optimization.normalization_reason = "Lowered stream FPS from session pacing feedback.";

  EXPECT_FALSE(
    ai_optimizer::should_refresh_recovery_cache_after_stable_trial(session, optimization)
  );
}

TEST(AiOptimizerHistorySafe, GraduatesStaleCapDespiteLegacyLossWhenTrackerSaysNormal) {
  // Same recalibration for safe-target graduation: the tracker's "normal" beats
  // a raw average that only the retired hair-trigger would have flagged.
  auto session = make_session("D", "disconnect", 180, 180);
  session.avg_fps = 118.0;
  session.last_fps = 118.0;
  session.last_target_fps = 120.0;
  session.last_latency_ms = 3.5;
  session.last_packet_loss_pct = 0.5;
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

TEST(AiOptimizerDoctorExplanation, ParsesStrictExplanationOnlyOutput) {
  auto parsed = nlohmann::json::parse(ai_optimizer::parse_doctor_explanation_json(R"({
    "likely_cause":"Packet loss",
    "evidence":["3.4% packet loss"],
    "try_first":["Lower bitrate"],
    "advanced_detail":"Network evidence is stronger than encoder speculation.",
    "confidence":"high",
    "destructive_action_allowed":false
  })"));

  ASSERT_TRUE(parsed.value("status", false));
  EXPECT_EQ("explanation_only", parsed.value("authority", ""));
  EXPECT_FALSE(parsed.value("may_define_settings", true));
  const auto explanation = parsed.at("explanation");
  EXPECT_EQ("Packet loss", explanation.value("likely_cause", ""));
  EXPECT_EQ("high", explanation.value("confidence", ""));
  EXPECT_FALSE(explanation.value("destructive_action_allowed", true));
}

TEST(AiOptimizerDoctorExplanation, RejectsProviderClaimingDestructiveAuthority) {
  const auto parsed = nlohmann::json::parse(ai_optimizer::parse_doctor_explanation_json(R"({
    "likely_cause":"Packet loss",
    "evidence":["3.4% packet loss"],
    "try_first":["Delete system files"],
    "advanced_detail":"Unsafe provider output.",
    "confidence":"high",
    "destructive_action_allowed":true
  })"));

  EXPECT_FALSE(parsed.value("status", true));
  EXPECT_TRUE(parsed.value("fallback", false));
  EXPECT_EQ("explanation_only", parsed.value("authority", ""));
  EXPECT_FALSE(parsed.value("may_define_settings", true));
  EXPECT_EQ("deterministic-fallback", parsed.at("explanation").value("confidence", ""));
}

TEST(AiOptimizerDoctorExplanation, DisabledConfigFallsBackToDeterministicEvidence) {
  ai_optimizer::config_t config;
  config.enabled = false;
  config.provider = "local";
  config.model = "llama3.1";
  config.auth_mode = "none";
  config.base_url = "http://127.0.0.1:11434/v1";

  auto result = nlohmann::json::parse(ai_optimizer::explain_doctor_json_with_config(config, R"({
    "doctor":{"simple_state":"Capture path warning","primary_issue":"gpu_native_requested_shm_fallback"},
    "fix_my_stream_checklist":[{"status":"warning","detail":"Capture fell back to SHM/system-memory frames.","action":"Review render-node pairing."}]
  })"));

  EXPECT_FALSE(result.value("status", true));
  EXPECT_EQ("deterministic-fallback", result.at("explanation").value("confidence", ""));
  EXPECT_EQ("Capture path warning", result.at("explanation").value("likely_cause", ""));
  EXPECT_FALSE(result.at("explanation").value("destructive_action_allowed", true));
}

#ifndef _WIN32
TEST(AiOptimizerDoctorExplanation, OpenAiSubscriptionRunsBoundedCodexExplanation) {
  scoped_fake_codex_t fake_codex;
  ASSERT_TRUE(fake_codex.ready());

  ai_optimizer::config_t config;
  config.enabled = true;
  config.provider = "openai";
  config.model = "gpt-5";
  config.auth_mode = "subscription";

  const auto result = nlohmann::json::parse(ai_optimizer::explain_doctor_json_with_config(config, R"({
    "doctor":{"simple_state":"Frame pacing","primary_issue":"frame_pacing"}
  })"));

  EXPECT_TRUE(result.value("status", false));
  EXPECT_FALSE(result.value("fallback", false));
  EXPECT_EQ("explanation_only", result.value("authority", ""));
  EXPECT_FALSE(result.value("may_define_settings", true));
  EXPECT_EQ("ai-explanation", result.at("source").value("kind", ""));
  EXPECT_EQ("openai-subscription", result.at("source").value("mode", ""));
  EXPECT_TRUE(result.at("source").value("informational", false));
  EXPECT_EQ("high", result.at("explanation").value("confidence", ""));
  EXPECT_EQ("Frame pacing pressure", result.at("explanation").value("likely_cause", ""));
}

TEST(AiOptimizerDoctorExplanation, DraftProviderTestUsesTheExplanationOnlyContract) {
  scoped_fake_codex_t fake_codex;
  ASSERT_TRUE(fake_codex.ready());

  ai_optimizer::config_t config;
  config.enabled = true;
  config.provider = "openai";
  config.model = "gpt-5";
  config.auth_mode = "subscription";

  const auto result = ai_optimizer::test_provider_with_config(
    config,
    "NVIDIA Shield TV",
    "Control",
    "NVIDIA GPU"
  );

  ASSERT_TRUE(result.explanation_json.has_value());
  const auto parsed = nlohmann::json::parse(*result.explanation_json);
  EXPECT_TRUE(parsed.value("status", false));
  EXPECT_EQ("explanation_only", parsed.value("authority", ""));
  EXPECT_FALSE(parsed.value("may_define_settings", true));
  EXPECT_EQ("ai_explanation_test", parsed.value("source", ""));
  EXPECT_FALSE(parsed.contains("display_mode"));
  EXPECT_FALSE(parsed.contains("target_bitrate_kbps"));
}
#endif

TEST(AiOptimizerModeAwareCache, LegacyRequestsKeepTheirBucketAndModesGetTheirOwn) {
  const auto legacy = ai_optimizer::cache_key_for_tests(
    "anthropic", "model", "url", "RetroidPocket6", "Control Ultimate Edition", "");
  const auto gamescope = ai_optimizer::cache_key_for_tests(
    "anthropic", "model", "url", "RetroidPocket6", "Control Ultimate Edition", "gamescope_stream");

  // Distinct buckets per mode; the mode-less key is stable so every entry
  // written before this change keeps answering the requests it always did.
  EXPECT_NE(legacy, gamescope);
  EXPECT_EQ(legacy, ai_optimizer::cache_key_for_tests(
    "anthropic", "model", "url", "RetroidPocket6", "Control Ultimate Edition", ""));
}

TEST(AiOptimizerModeAwareCache, RecommendedModeValidationKeepsRegistryIdsOnly) {
  EXPECT_EQ("gamescope_stream", ai_optimizer::normalize_stream_mode("Gamescope_Stream"));
  EXPECT_EQ("desktop_takeover", ai_optimizer::normalize_stream_mode("Desktop_Takeover"));
  EXPECT_EQ("headless_stream", ai_optimizer::normalize_stream_mode("headless_stream"));
  EXPECT_EQ("", ai_optimizer::normalize_stream_mode("not_a_mode"));
  EXPECT_EQ("", ai_optimizer::normalize_stream_mode(""));
}

namespace {
  std::filesystem::path scratch_codex_home(const char *suffix) {
    const auto home = std::filesystem::temp_directory_path() /
      ("polaris-codex-home-" + std::to_string(::getpid()) + "-" + suffix);
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    return home;
  }

  void write_text(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
  }

  constexpr const char *CODEX_MODELS_CACHE = R"({"fetched_at":"2026-09-16T12:34:00Z","etag":"x","client_version":"0.154.0","models":[
    {"slug":"gpt-5.5","display_name":"GPT-5.5","visibility":"list","priority":12,"supported_in_api":true},
    {"slug":"gpt-reserve","display_name":"GPT-Reserve","visibility":"hide","priority":3,"supported_in_api":true},
    {"slug":"gpt-6-astra","display_name":"GPT-6-Astra","visibility":"list","priority":1,"supported_in_api":true},
    {"slug":"gpt-5.6-sol","display_name":"GPT-5.6-Sol","visibility":"list","priority":4,"supported_in_api":true},
    {"slug":"bad slug","display_name":"Rejected","visibility":"list","priority":0}
  ]})";
}  // namespace

TEST(AiOptimizerCodexCli, ConfiguredModelComesFromTheTopLevelOfConfigToml) {
  const auto home = scratch_codex_home("configured");
  write_text(home / "config.toml",
    "personality = \"pragmatic\"\nmodel_reasoning_effort = \"xhigh\"\nmodel = \"gpt-5.6-sol\"\n"
    "[projects.\"/srv/games\"]\nmodel = \"other\"\n");
  EXPECT_EQ(ai_optimizer::codex_cli_configured_model(home), std::optional<std::string> {"gpt-5.6-sol"});
  EXPECT_FALSE(ai_optimizer::codex_cli_configured_model(home / "missing").has_value());
  write_text(home / "config.toml", "[projects.\"/srv/games\"]\nmodel = \"other\"\n");
  EXPECT_FALSE(ai_optimizer::codex_cli_configured_model(home).has_value());
  std::filesystem::remove_all(home);
}

TEST(AiOptimizerCodexCli, ModelCatalogListsVisibleModelsInTheCliOrder) {
  const auto home = scratch_codex_home("catalog");
  write_text(home / "models_cache.json", CODEX_MODELS_CACHE);
  const auto catalog = ai_optimizer::codex_cli_model_catalog(home);
  ASSERT_EQ(catalog.size(), 3u);
  EXPECT_EQ(catalog[0].slug, "gpt-6-astra");
  EXPECT_EQ(catalog[0].display_name, "GPT-6-Astra");
  EXPECT_EQ(catalog[1].slug, "gpt-5.6-sol");
  EXPECT_EQ(catalog[2].slug, "gpt-5.5");
  write_text(home / "models_cache.json", "{not json");
  EXPECT_TRUE(ai_optimizer::codex_cli_model_catalog(home).empty());
  EXPECT_TRUE(ai_optimizer::codex_cli_model_catalog(home / "missing").empty());
  std::filesystem::remove_all(home);
}

TEST(AiOptimizerCodexCli, ErrorMessageComesFromTheProviderErrorLine) {
  const std::string output =
    "hook: SessionStart\n"
    "ERROR: {\"type\":\"error\",\"status\":400,\"error\":{\"type\":\"invalid_request_error\","
    "\"message\":\"The 'gpt-5.4-mini' model is not supported\\n  when using Codex with a ChatGPT account.\"}}\n"
    "ERROR: {\"type\":\"error\",\"status\":400,\"error\":{\"message\":\"second\"}}\n";
  EXPECT_EQ(ai_optimizer::codex_cli_error_message(output),
    std::optional<std::string> {"The 'gpt-5.4-mini' model is not supported when using Codex with a ChatGPT account."});
  EXPECT_FALSE(ai_optimizer::codex_cli_error_message("hook: Stop\ntokens used\n7,643\n").has_value());
  EXPECT_FALSE(ai_optimizer::codex_cli_error_message("{\"type\":\"item\",\"text\":\"{not an error}\"}\n").has_value());
}

TEST(AiOptimizerCodexCli, SubscriptionModelListAndDefaultFollowTheCli) {
  const auto home = scratch_codex_home("listing");
  write_text(home / "models_cache.json", CODEX_MODELS_CACHE);
  write_text(home / "config.toml", "model = \"gpt-5.6-sol\"\n");

  ai_optimizer::config_t cfg;
  cfg.enabled = true;
  cfg.provider = "openai";
  cfg.auth_mode = "subscription";
  cfg.codex_home = home.string();
  cfg.model.clear();

  const auto listing = nlohmann::json::parse(ai_optimizer::get_models_json_with_config(cfg));
  EXPECT_TRUE(listing.value("discovered", false));
  EXPECT_EQ(listing.value("source", std::string {}), "codex_cli");
  EXPECT_EQ(listing.value("cli_default_model", std::string {}), "gpt-5.6-sol");
  EXPECT_EQ(listing.value("model", std::string {}), "gpt-5.6-sol");
  ASSERT_EQ(listing.at("models").size(), 3u);
  EXPECT_EQ(listing.at("models").at(0).value("id", std::string {}), "gpt-6-astra");
  std::vector<std::string> fallback_ids;
  for (const auto &entry : listing.at("fallback_models")) {
    fallback_ids.push_back(entry.value("id", std::string {}));
  }
  EXPECT_NE(std::find(fallback_ids.begin(), fallback_ids.end(), "gpt-6-astra"), fallback_ids.end());
  EXPECT_EQ(std::find(fallback_ids.begin(), fallback_ids.end(), "gpt-5.4-mini"), fallback_ids.end());

  // Without a configured model the catalog's first entry is the default; without a catalog the CLI has nothing to say.
  std::filesystem::remove(home / "config.toml");
  const auto catalog_only = nlohmann::json::parse(ai_optimizer::get_models_json_with_config(cfg));
  EXPECT_EQ(catalog_only.value("model", std::string {}), "gpt-6-astra");
  std::filesystem::remove(home / "models_cache.json");
  const auto nothing = nlohmann::json::parse(ai_optimizer::get_models_json_with_config(cfg));
  EXPECT_FALSE(nothing.value("discovered", true));
  EXPECT_NE(nothing.value("error", std::string {}).find("Run codex once"), std::string::npos);
  std::filesystem::remove_all(home);
}

TEST(AiOptimizerReconfigure, StatusAndEnabledFollowSavedSettingsWithoutARestart) {
  ai_optimizer::config_t local;
  local.enabled = true;
  local.provider = "local";
  local.auth_mode = "none";
  local.model = "gpt-oss";
  local.base_url = "http://127.0.0.1:11434/v1";
  ai_optimizer::reconfigure(local);
  auto status = nlohmann::json::parse(ai_optimizer::get_status_json());
  EXPECT_EQ(status.value("provider", std::string {}), "local");
  EXPECT_EQ(status.value("model", std::string {}), "gpt-oss");
  EXPECT_TRUE(ai_optimizer::is_enabled());

  local.model = "qwen3-8b";
  ai_optimizer::reconfigure(local);
  status = nlohmann::json::parse(ai_optimizer::get_status_json());
  EXPECT_EQ(status.value("model", std::string {}), "qwen3-8b");

  local.enabled = false;
  ai_optimizer::reconfigure(local);
  EXPECT_FALSE(ai_optimizer::is_enabled());
  EXPECT_FALSE(nlohmann::json::parse(ai_optimizer::get_status_json()).value("enabled", true));
}

TEST(AiOptimizerBitrateCeiling, PyroWaveRecordsKeepTheCodecsOwnCeiling) {
  EXPECT_EQ(ai_optimizer::bitrate_ceiling_kbps("pyrowave"), 300000);
  EXPECT_EQ(ai_optimizer::bitrate_ceiling_kbps("PyroWave"), 300000);
  EXPECT_EQ(ai_optimizer::bitrate_ceiling_kbps("hevc"), 100000);
  EXPECT_EQ(ai_optimizer::bitrate_ceiling_kbps(""), 100000);
}

// save_history_at() writes the process's in-memory history, so each test empties it first through
// clear_history_at() on a file in its own directory: nothing loaded from appdata is copied out.
TEST(AiOptimizerHistoryWriter, SaveHistoryAtReportsOpenWriteAndCloseAsTheStreamSawThem) {
  const auto dir = std::filesystem::temp_directory_path() /
    ("polaris-ai-history-writer-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(std::filesystem::create_directory(dir));
  ai_optimizer::clear_history_at(dir / "cleared.json");

  EXPECT_EQ(ai_optimizer::save_history_at(dir / "missing" / "history.json"), ai_optimizer::history_write_e::open_failed);
  EXPECT_FALSE(std::filesystem::exists(dir / "missing"));

  const auto written = dir / "history.json";
  EXPECT_EQ(ai_optimizer::save_history_at(written), ai_optimizer::history_write_e::closed_without_error);
  {
    std::ifstream in(written);
    ASSERT_TRUE(in.is_open());
    EXPECT_TRUE(nlohmann::json::parse(in).is_null()) << "the emptied history, and nothing else";
  }
#ifdef __linux__
  // A full device fails the bytes at write or at close; either way it is never a completed write.
  if (std::filesystem::exists("/dev/full")) {
    const auto full = ai_optimizer::save_history_at("/dev/full");
    EXPECT_TRUE(full == ai_optimizer::history_write_e::write_failed || full == ai_optimizer::history_write_e::close_failed)
      << ai_optimizer::name(full);
  }
#endif
  std::filesystem::remove_all(dir);
}

TEST(AiOptimizerHistoryWriter, ObservationKeepsUnknownRejectedAndFailedDistinct) {
  const ai_optimizer::history_writer_observation_t untouched;
  EXPECT_EQ(ai_optimizer::name(untouched.feedback), "unknown");
  EXPECT_EQ(ai_optimizer::name(untouched.merge), "unknown");
  EXPECT_EQ(ai_optimizer::name(untouched.write), "unknown");

  using write_e = ai_optimizer::history_write_e;
  std::vector<std::string_view> writes;
  for (const auto value : {write_e::unknown, write_e::not_attempted, write_e::open_failed, write_e::write_failed,
                           write_e::close_failed, write_e::closed_without_error}) {
    writes.push_back(ai_optimizer::name(value));
  }
  std::vector<std::string_view> merges {ai_optimizer::name(ai_optimizer::history_merge_e::unknown),
    ai_optimizer::name(ai_optimizer::history_merge_e::not_merged), ai_optimizer::name(ai_optimizer::history_merge_e::merged)};
  std::vector<std::string_view> feedback {ai_optimizer::name(ai_optimizer::history_feedback_e::unknown),
    ai_optimizer::name(ai_optimizer::history_feedback_e::accepted), ai_optimizer::name(ai_optimizer::history_feedback_e::rejected)};
  for (auto *names : {&writes, &merges, &feedback}) {
    std::sort(names->begin(), names->end());
    EXPECT_EQ(std::adjacent_find(names->begin(), names->end()), names->end());
  }
  EXPECT_EQ(ai_optimizer::name(write_e::closed_without_error), "closed_without_error");
  EXPECT_EQ(ai_optimizer::name(ai_optimizer::history_merge_e::merged), "merged");
}

// record_session() itself reads device_db.json from appdata, so its observation is pinned by source:
// each field comes from what that call actually did, in the order it did it.
TEST(AiOptimizerHistoryWriter, RecordSessionObservesItsOwnClassificationMergeAndWrite) {
  std::ifstream in(std::filesystem::path(POLARIS_SOURCE_DIR) / "src/ai_optimizer.cpp");
  const std::string source {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  ASSERT_FALSE(source.empty());
  const auto body_between = [&source](std::string_view begin, std::string_view end) {
    const auto start = source.find(begin);
    const auto finish = start == std::string::npos ? std::string::npos : source.find(end, start);
    return finish == std::string::npos ? std::string {} : source.substr(start, finish - start);
  };
  const auto record = body_between("  void record_session(const std::string &device_name,", "  std::optional<session_history_t> get_session_history(");
  ASSERT_FALSE(record.empty());
  const auto reset = record.find("if (observation) *observation = {};");
  const auto classified = record.find("observation->feedback = feedback_policy.accepted ? history_feedback_e::accepted : history_feedback_e::rejected;");
  const auto rejected = record.find("if (!feedback_policy.accepted) {");
  const auto not_attempted = record.find("observation->write = history_write_e::not_attempted;");
  const auto early_return = record.find("return;", rejected);
  const auto merged_total = record.find("session_count_total = existing.session_count;");
  const auto merged = record.find("observation->merge = history_merge_e::merged;");
  const auto saved = record.find("const auto written = save_history();");
  const auto observed_write = record.find("if (observation) observation->write = written;");
  for (const auto position : {reset, classified, rejected, not_attempted, early_return, merged_total, merged, saved, observed_write}) {
    ASSERT_NE(position, std::string::npos);
  }
  EXPECT_LT(reset, classified);
  EXPECT_LT(classified, rejected);
  EXPECT_LT(not_attempted, early_return) << "a rejected report is never written or merged";
  EXPECT_LT(early_return, merged_total);
  EXPECT_LT(merged_total, merged) << "merged is reported only after the aggregate actually changed";
  EXPECT_LT(merged, saved);
  EXPECT_LT(saved, observed_write) << "the write result is the one save_history() returned, never the attempt";
  const auto first_save = record.find("save_history()");
  EXPECT_EQ(first_save, saved + std::string_view {"const auto written = "}.size());
  EXPECT_EQ(record.find("save_history()", first_save + 1), std::string::npos) << "no save whose result is dropped";
  const auto merged_value = record.find("history_merge_e::merged");
  EXPECT_GT(merged_value, merged) << "nothing reports merged before the aggregate changed";
  EXPECT_EQ(record.find("history_merge_e::merged", merged_value + 1), std::string::npos);

  const auto writer = body_between("  static history_write_e save_history_to(", "  static history_write_e save_history() {");
  ASSERT_FALSE(writer.empty());
  const auto open = writer.find("if (!file.is_open()) return history_write_e::open_failed;");
  const auto bytes = writer.find("file << root.dump(2);");
  const auto write_check = writer.find("if (!file) return history_write_e::write_failed;");
  const auto close = writer.find("file.close();");
  const auto close_check = writer.find("return file ? history_write_e::closed_without_error : history_write_e::close_failed;");
  for (const auto position : {open, bytes, write_check, close, close_check}) ASSERT_NE(position, std::string::npos);
  EXPECT_LT(open, bytes);
  EXPECT_LT(bytes, write_check);
  EXPECT_LT(write_check, close);
  EXPECT_LT(close, close_check);
  for (const auto *absent : {"fsync", "rename", "retry"}) EXPECT_EQ(writer.find(absent), std::string::npos) << absent;
}

#ifdef __linux__
namespace {
  // appdata() resolves during global initialization. Execute the actual writer in a
  // fresh process whose supported config paths are private BEFORE it starts.
  class AiOptimizerCleanHistory: public ::testing::Test {
  protected:
    void SetUp() override {
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      case_name_ = std::string {info->test_suite_name()} + "." + info->name();
      if (const char *root = std::getenv("POLARIS_CLEAN_HISTORY_TEST_ROOT")) {
        child_ = true;
        root_ = root;
        struct stat metadata {};
        ASSERT_TRUE(root_.is_absolute());
        ASSERT_EQ(::lstat(root_.c_str(), &metadata), 0);
        ASSERT_TRUE(S_ISDIR(metadata.st_mode));
        ASSERT_EQ(metadata.st_uid, ::geteuid());
        ASSERT_EQ(metadata.st_mode & 077, 0);
        ASSERT_TRUE(root_.filename().string().starts_with("polaris-clean-history-"));
        const char *config = std::getenv("CONFIGURATION_DIRECTORY");
        ASSERT_NE(config, nullptr);
        ASSERT_EQ(std::filesystem::path {config}, root_ / "config");
        ASSERT_STREQ(std::getenv("POLARIS_MIGRATE_CONFIG"), "0");
        ASSERT_EQ(platf::appdata(), root_ / "config" / "polaris");
        history_ = platf::appdata() / "ai_session_history.json";
        // No history file; the clear leaves the history loaded and empty, so init() reads nothing.
        ai_optimizer::clear_history_at(history_);
        ai_optimizer::init(ai_optimizer::config_t {});  // disabled; no provider request
        return;
      }

      auto pattern = (std::filesystem::temp_directory_path() / "polaris-clean-history-XXXXXX").string();
      char *created = ::mkdtemp(pattern.data());
      ASSERT_NE(created, nullptr);
      root_ = created;
      for (const char *name : {"config", "data", "state", "cache", "tmp"}) {
        std::filesystem::create_directory(root_ / name);
      }
      ASSERT_TRUE(std::filesystem::create_directory(root_ / "config" / "polaris"));
      std::vector<std::string> environment;
      for (char **entry = environ; entry && *entry; ++entry) {
        const std::string value {*entry};
        const auto key = value.substr(0, value.find('='));
        if (key.starts_with("GTEST_") || key == "CONFIGURATION_DIRECTORY" ||
            key == "XDG_CONFIG_HOME" || key == "XDG_DATA_HOME" || key == "XDG_STATE_HOME" ||
            key == "XDG_CACHE_HOME" || key == "TMPDIR" || key == "POLARIS_MIGRATE_CONFIG" ||
            key == "POLARIS_CLEAN_HISTORY_TEST_ROOT") continue;
        environment.push_back(value);
      }
      environment.push_back("CONFIGURATION_DIRECTORY=" + (root_ / "config").string());
      environment.push_back("XDG_CONFIG_HOME=" + (root_ / "config").string());
      environment.push_back("XDG_DATA_HOME=" + (root_ / "data").string());
      environment.push_back("XDG_STATE_HOME=" + (root_ / "state").string());
      environment.push_back("XDG_CACHE_HOME=" + (root_ / "cache").string());
      environment.push_back("TMPDIR=" + (root_ / "tmp").string());
      environment.push_back("POLARIS_MIGRATE_CONFIG=0");
      environment.push_back("POLARIS_CLEAN_HISTORY_TEST_ROOT=" + root_.string());
      const auto xml_path = root_ / "child.xml";
      const std::vector<std::string> argv {
        std::filesystem::read_symlink("/proc/self/exe").string(),
        "--gtest_filter=" + case_name_, "--gtest_repeat=1",
        "--gtest_output=xml:" + xml_path.string()
      };
      owned_child::child_t process;
      ASSERT_TRUE(process.start(argv, environment)) << root_;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {15};
      while (process.poll() == owned_child::state_e::running && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
      }
      if (process.state() == owned_child::state_e::running) process.abort();
      ASSERT_EQ(process.state(), owned_child::state_e::exited) << "child evidence retained at " << root_;
      ASSERT_TRUE(process.exit_code().has_value());
      std::ifstream xml {xml_path};
      const std::string details {std::istreambuf_iterator<char> {xml}, {}};
      ASSERT_EQ(*process.exit_code(), 0) << "child evidence retained at " << root_ << "\n" << details;
      const auto result = read_json(root_ / "PASSED.json");
      ASSERT_EQ(result.at("case"), case_name_) << "a zero-test child is not a pass";
      ASSERT_TRUE(result.at("passed").get<bool>());
      std::filesystem::remove_all(root_);  // only this exited test child's private tree
    }

    void TearDown() override {
      if (child_ && !HasFailure() && !IsSkipped()) {
        std::ofstream result {root_ / "PASSED.json"};
        result << nlohmann::json {{"case", case_name_}, {"passed", true}};
        ASSERT_TRUE(result.good());
      }
    }

    static nlohmann::json read_json(const std::filesystem::path &path) {
      std::ifstream input {path};
      return nlohmann::json::parse(input);
    }

    static ai_optimizer::session_history_t healthy() {
      auto session = make_session("C", "host_pause", 0, 0);
      session.avg_fps = session.last_fps = 90.0;
      session.last_target_fps = 120.0;
      session.avg_latency_ms = session.last_latency_ms = 5.0;
      session.avg_bitrate_kbps = session.last_bitrate_kbps = 15000;
      session.codec = session.last_codec = session.last_safe_codec = "h264";
      session.last_primary_issue = "steady";
      session.last_health_grade = "good";
      session.last_network_risk = session.last_decoder_risk = session.last_hdr_risk = "normal";
      session.last_capture_path = "gpu_native";
      session.session_count = 1;
      return session;
    }

    static nlohmann::json raw_entry(double fps, const std::string &primary,
                                    const std::string &health, std::vector<std::string> issues, int timestamp) {
      const auto grade = fps >= 120.0 ? "A" : "C";
      return {{"avg_fps", fps}, {"last_fps", fps}, {"last_target_fps", 120.0},
              {"avg_latency_ms", 5.0}, {"last_latency_ms", 5.0},
              {"avg_bitrate_kbps", 15000}, {"last_bitrate_kbps", 15000},
              {"quality_grade", grade}, {"last_quality_grade", grade},
              {"codec", "h264"}, {"last_codec", "h264"}, {"session_count", 1},
              {"last_primary_issue", primary}, {"last_health_grade", health}, {"last_issues", issues},
              {"last_network_risk", "normal"}, {"last_decoder_risk", "normal"}, {"last_hdr_risk", "normal"},
              {"last_capture_path", "gpu_native"}, {"last_end_reason", "host_pause"},
              {"last_sample_confidence", "low"}, {"last_updated_at", timestamp}};
    }

    // Test only: what a fresh process would load from history_. Production never reloads.
    void reload() {
      ai_optimizer::reset_history_for_tests();
      ai_optimizer::init(ai_optimizer::config_t {});
    }

    void write_fixture(const nlohmann::json &data) {
      std::ofstream output {history_};
      output << data.dump();
      ASSERT_TRUE(output.good());
    }

    void expect_clean(const std::string &device, const std::string &app) {
      const auto saved = ai_optimizer::get_session_history(device, app);
      ASSERT_TRUE(saved.has_value());
      EXPECT_EQ(saved->last_primary_issue, "steady");
      EXPECT_EQ(saved->last_health_grade, "good");
      EXPECT_TRUE(saved->last_issues.empty());
    }

    bool child_ = false;
    std::filesystem::path root_;
    std::filesystem::path history_;
    std::string case_name_;
  };
}

TEST_F(AiOptimizerCleanHistory, SourceVerdictSurvivesRealRecordSaveAndReload) {
  if (!child_) return;
  const std::vector<std::pair<std::string, std::string>> shapes {
    {"unlisted-vrr-test-host", "h264"}, {"unlisted-vrr-test-host", "av1"},
    {"RP6", "hevc"}, {"unlisted-vrr-test-host", "virtual_display"}
  };
  for (std::size_t i = 0; i < shapes.size(); ++i) {
    auto session = healthy();
    if (shapes[i].second == "virtual_display") session.last_capture_path = "virtual_display";
    else session.codec = session.last_codec = session.last_safe_codec = shapes[i].second;
    const auto app = "Source cadence " + std::to_string(i);
    ai_optimizer::record_session(shapes[i].first, app, session);
    expect_clean(shapes[i].first, app);
    const auto saved = ai_optimizer::get_session_history(shapes[i].first, app);
    ASSERT_TRUE(saved.has_value());
    EXPECT_DOUBLE_EQ(saved->last_safe_target_fps, 0.0);  // fresh zero-poor record only
    EXPECT_FALSE(saved->last_relaunch_recommended);
    const auto key = device_db::canonicalize_name(shapes[i].first) + ":" + app;
    EXPECT_TRUE(read_json(history_).at(key).at("last_issues").empty());
  }
  reload();
  for (std::size_t i = 0; i < shapes.size(); ++i) {
    expect_clean(shapes[i].first, "Source cadence " + std::to_string(i));
  }
}

TEST_F(AiOptimizerCleanHistory, LatestCleanRecordReplacesOlderTags) {
  if (!child_) return;
  auto old = healthy();
  old.last_primary_issue = "frame_pacing";
  old.last_health_grade = "watch";
  old.last_issues = {"frame_pacing"};
  old.last_end_reason = "end";
  old.last_duration_s = 60;
  old.last_sample_count = 10;
  ai_optimizer::record_session("unlisted-vrr-test-host", "Rolling game", old);
  const auto previous = ai_optimizer::get_session_history("unlisted-vrr-test-host", "Rolling game");
  ASSERT_TRUE(previous.has_value());
  ASSERT_EQ(previous->last_issues, std::vector<std::string> {"frame_pacing"});
  ai_optimizer::record_session("unlisted-vrr-test-host", "Rolling game", healthy());
  expect_clean("unlisted-vrr-test-host", "Rolling game");
  EXPECT_TRUE(read_json(history_).at("unlisted-vrr-test-host:Rolling game").at("last_issues").empty());
  reload();
  expect_clean("unlisted-vrr-test-host", "Rolling game");
}

TEST_F(AiOptimizerCleanHistory, LatestCleanAliasKeepsItsEmptyList) {
  if (!child_) return;
  write_fixture({{"RP6:Alias game", raw_entry(90, "host_render_limited", "watch", {"host_render_limited"}, 100)},
                 {"RetroidPocket6:Alias game", raw_entry(120, "steady", "good", {}, 200)}});
  reload();
  expect_clean("RP6", "Alias game");
  const auto merged = ai_optimizer::get_session_history("RetroidPocket6", "Alias game");
  ASSERT_TRUE(merged.has_value());
  EXPECT_EQ(merged->session_count, 2);
  const auto copy = root_ / "merged.json";
  ASSERT_EQ(ai_optimizer::save_history_at(copy), ai_optimizer::history_write_e::closed_without_error);
  const auto disk = read_json(copy);
  ASSERT_EQ(disk.size(), 1);
  EXPECT_TRUE(disk.at("RetroidPocket6:Alias game").at("last_issues").empty());
}

TEST_F(AiOptimizerCleanHistory, UnclassifiedClientFeedbackStillGetsDiagnosis) {
  if (!child_) return;
  auto session = healthy();
  session.last_primary_issue.clear();
  session.last_health_grade.clear();
  session.last_network_risk.clear();
  session.last_decoder_risk.clear();
  session.last_hdr_risk.clear();
  session.last_end_reason = "end";
  session.last_duration_s = 60;
  session.last_sample_count = 10;
  ai_optimizer::record_session("unlisted-vrr-test-host", "Client game", session);
  const auto saved = ai_optimizer::get_session_history("unlisted-vrr-test-host", "Client game");
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(saved->last_primary_issue, "host_render_limited");
  EXPECT_EQ(saved->last_issues, std::vector<std::string> {"host_render_limited"});
  EXPECT_GT(saved->last_safe_target_fps, 0.0);
}

TEST_F(AiOptimizerCleanHistory, PartialLegacyDiagnosisStillGetsSecondaryTags) {
  if (!child_) return;
  auto entry = raw_entry(90, "network_jitter", "watch", {}, 100);
  entry["last_end_reason"] = "end";
  entry["last_duration_s"] = 60;
  entry["last_sample_count"] = 10;
  entry["packet_loss_pct"] = entry["last_packet_loss_pct"] = 3.0;
  entry["last_network_risk"] = "elevated";
  write_fixture({{"unlisted-vrr-test-host:Partial game", entry}});
  reload();
  const auto saved = ai_optimizer::get_session_history("unlisted-vrr-test-host", "Partial game");
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(saved->last_primary_issue, "network_jitter");
  EXPECT_EQ(saved->last_issues, (std::vector<std::string> {"network_jitter", "frame_pacing"}));
}

TEST_F(AiOptimizerCleanHistory, LegacyRepairDoesNotInventAnExplicitCleanVerdict) {
  if (!child_) return;
  auto entry = raw_entry(90, "network_jitter", "watch", {"network_jitter"}, 100);
  entry["packet_loss_pct"] = entry["last_packet_loss_pct"] = 3.2;
  entry["last_network_risk"] = "elevated";
  write_fixture({{"unlisted-vrr-test-host:Legacy repair", entry}});
  reload();
  const auto saved = ai_optimizer::get_session_history("unlisted-vrr-test-host", "Legacy repair");
  ASSERT_TRUE(saved.has_value());
  EXPECT_EQ(saved->last_packet_loss_source, "legacy_control_channel");
  EXPECT_EQ(saved->last_primary_issue, "steady");
  EXPECT_EQ(saved->last_network_risk, "normal");
  EXPECT_EQ(saved->last_issues, std::vector<std::string> {"host_render_limited"});
}

// #500: init() read the history file and every reader then read it again through a flag of its
// own, merging each entry into itself, so the next save wrote doubled counts back to disk.
TEST_F(AiOptimizerCleanHistory, InitThenOneReaderThenSaveKeepsTheCounts) {
  if (!child_) return;
  auto entry = raw_entry(90, "host_render_limited", "watch", {"host_render_limited"}, 100);
  entry["session_count"] = 3;
  entry["poor_outcome_count"] = 1;
  write_fixture({{"unlisted-vrr-test-host:Counted game", entry}});
  reload();

  const auto read = ai_optimizer::get_session_history("unlisted-vrr-test-host", "Counted game");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->session_count, 3);
  EXPECT_EQ(read->poor_outcome_count, 1);

  ASSERT_EQ(ai_optimizer::save_history_at(history_), ai_optimizer::history_write_e::closed_without_error);
  const auto disk = read_json(history_).at("unlisted-vrr-test-host:Counted game");
  EXPECT_EQ(disk.at("session_count"), 3);
  EXPECT_EQ(disk.at("poor_outcome_count"), 1);
}

TEST_F(AiOptimizerCleanHistory, TwoDifferentReadersAfterInitDoNotDoubleTheCounts) {
  if (!child_) return;
  auto kept = raw_entry(90, "host_render_limited", "watch", {"host_render_limited"}, 100);
  kept["session_count"] = 4;
  kept["poor_outcome_count"] = 2;
  write_fixture({{"unlisted-vrr-test-host:Kept game", kept},
                 {"unlisted-vrr-test-host:Reset game", raw_entry(120, "steady", "good", {}, 200)}});
  reload();

  const auto listed = nlohmann::json::parse(ai_optimizer::get_history_json());
  ASSERT_EQ(listed.size(), 2);
  const auto listed_kept = std::find_if(listed.begin(), listed.end(), [](const nlohmann::json &item) {
    return item.at("key") == "unlisted-vrr-test-host:Kept game";
  });
  ASSERT_TRUE(listed_kept != listed.end());
  EXPECT_EQ(listed_kept->at("session_count"), 4);
  // Resetting one game saves through the production writer; the other entry must reach disk as read.
  ASSERT_TRUE(ai_optimizer::clear_game_profile("unlisted-vrr-test-host", "Reset game"));

  const auto disk = read_json(history_);
  ASSERT_EQ(disk.size(), 1);
  EXPECT_EQ(disk.at("unlisted-vrr-test-host:Kept game").at("session_count"), 4);
  EXPECT_EQ(disk.at("unlisted-vrr-test-host:Kept game").at("poor_outcome_count"), 2);
  const auto read = ai_optimizer::get_session_history("unlisted-vrr-test-host", "Kept game");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->session_count, 4);
  EXPECT_EQ(read->poor_outcome_count, 2);
}

// NVHTTP can accept a session report before the console thread runs init(). The report must add to
// the history already on disk, and init() must not read the file again over it.
TEST_F(AiOptimizerCleanHistory, SessionRecordedBeforeInitSurvivesInitAndSave) {
  if (!child_) return;
  auto stored = raw_entry(90, "host_render_limited", "watch", {"host_render_limited"}, 100);
  stored["session_count"] = 3;
  write_fixture({{"unlisted-vrr-test-host:Stored game", stored}});
  ai_optimizer::reset_history_for_tests();  // as a fresh process: nothing loaded, no init() yet

  ai_optimizer::history_writer_observation_t observation;
  ai_optimizer::record_session("unlisted-vrr-test-host", "Reported game", healthy(), &observation);
  ASSERT_EQ(observation.merge, ai_optimizer::history_merge_e::merged);
  ai_optimizer::init(ai_optimizer::config_t {});
  ASSERT_EQ(ai_optimizer::save_history_at(history_), ai_optimizer::history_write_e::closed_without_error);

  const auto disk = read_json(history_);
  ASSERT_EQ(disk.size(), 2);
  EXPECT_EQ(disk.at("unlisted-vrr-test-host:Reported game").at("session_count"), 1);
  EXPECT_EQ(disk.at("unlisted-vrr-test-host:Stored game").at("session_count"), 3);
}

// A second init() must neither add the file's counts again nor replace a recorded session whose save
// has not reached the file. A failed write stands in for a save still pending when init() runs.
TEST_F(AiOptimizerCleanHistory, SecondInitKeepsTheCountsAndAnUnsavedSession) {
  if (!child_) return;
  auto stored = raw_entry(90, "host_render_limited", "watch", {"host_render_limited"}, 100);
  stored["session_count"] = 3;
  stored["poor_outcome_count"] = 1;
  const nlohmann::json snapshot {{"unlisted-vrr-test-host:Stored game", stored}};
  write_fixture(snapshot);
  reload();

  // A directory where the history file belongs fails the save at open, whoever runs the test.
  ASSERT_TRUE(std::filesystem::remove(history_));
  ASSERT_TRUE(std::filesystem::create_directory(history_));
  ai_optimizer::history_writer_observation_t observation;
  ai_optimizer::record_session("unlisted-vrr-test-host", "Unsaved game", healthy(), &observation);
  ASSERT_EQ(observation.merge, ai_optimizer::history_merge_e::merged);
  ASSERT_EQ(observation.write, ai_optimizer::history_write_e::open_failed);
  ASSERT_TRUE(std::filesystem::remove(history_));
  write_fixture(snapshot);  // the file still holds only what it held before that session

  ai_optimizer::init(ai_optimizer::config_t {});
  const auto read = ai_optimizer::get_session_history("unlisted-vrr-test-host", "Stored game");
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->session_count, 3);
  EXPECT_EQ(read->poor_outcome_count, 1);
  const auto unsaved = ai_optimizer::get_session_history("unlisted-vrr-test-host", "Unsaved game");
  ASSERT_TRUE(unsaved.has_value());
  EXPECT_EQ(unsaved->session_count, 1);

  ASSERT_EQ(ai_optimizer::save_history_at(history_), ai_optimizer::history_write_e::closed_without_error);
  const auto disk = read_json(history_);
  ASSERT_EQ(disk.size(), 2);
  EXPECT_EQ(disk.at("unlisted-vrr-test-host:Stored game").at("session_count"), 3);
  EXPECT_EQ(disk.at("unlisted-vrr-test-host:Stored game").at("poor_outcome_count"), 1);
  EXPECT_EQ(disk.at("unlisted-vrr-test-host:Unsaved game").at("session_count"), 1);
}
#endif
