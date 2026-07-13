/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <src/stream_stats.h>
#include <string>
#include <string_view>
#include <vector>

namespace stream {
  void concat_and_insert_into(std::vector<uint8_t> &result, uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
}

namespace nvhttp {
  std::optional<int> select_paired_client_launch_bitrate_for_tests(
    const std::optional<int> &target_bitrate_kbps,
    int paired_bitrate_kbps,
    bool applied_history_safe
  );

  nlohmann::json build_session_health_json_for_tests(
    const stream_stats::stats_t &stats,
    bool current_virtual_display,
    const std::string &device_name,
    const std::string &app_name
  );
}

namespace proc {
  bool should_publish_stream_ended_after_terminate_for_tests(bool had_running_app, int active_sessions, std::string_view session_state);

  nlohmann::json classify_host_pause_session_for_tests(
    const stream_stats::stats_t &stats,
    double target_fps,
    bool current_virtual_display
  );
}

namespace {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2) {
    std::vector<uint8_t> result;
    stream::concat_and_insert_into(result, insert_size, slice_size, data1, data2);
    return result;
  }

  stream_stats::stats_t stable_gpu_native_stats(double delivered_fps, double target_fps) {
    stream_stats::stats_t stats;
    stats.streaming = true;
    stats.runtime_effective_headless = true;
    stats.capture_transport = platf::frame_transport_e::dmabuf;
    stats.capture_residency = platf::frame_residency_e::gpu;
    stats.encode_target_residency = platf::frame_residency_e::gpu;
    stats.fps = delivered_fps;
    stats.encode_target_fps = target_fps;
    stats.codec = "hevc";
    return stats;
  }
}

#include "../tests_common.h"

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(NvhttpOptimizerTests, PairedClientLaunchBitrateOverridesCachedLowerOptimizerTarget) {
  const auto selected = nvhttp::select_paired_client_launch_bitrate_for_tests(
    25000,
    80000,
    false
  );

  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(*selected, 80000);
}

TEST(NvhttpOptimizerTests, PairedClientLaunchBitrateKeepsHistorySafeRecoveryCap) {
  const auto selected = nvhttp::select_paired_client_launch_bitrate_for_tests(
    25000,
    80000,
    true
  );

  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(*selected, 25000);
}

TEST(NvhttpSessionHealthTests, HighRefreshNearTargetDeliveryRemainsSteady) {
  const auto health = nvhttp::build_session_health_json_for_tests(
    stable_gpu_native_stats(115.6, 120.0),
    false,
    "Nova Client",
    "Mouse"
  );

  EXPECT_EQ(health.at("grade"), "good");
  EXPECT_EQ(health.at("primary_issue"), "steady");
  EXPECT_EQ(health.at("limiting_factor"), "none");
  EXPECT_FALSE(health.at("host_render_limited").get<bool>());
}

TEST(NvhttpSessionHealthTests, MeaningfulTargetMissRemainsHostRenderLimited) {
  const auto health = nvhttp::build_session_health_json_for_tests(
    stable_gpu_native_stats(54.0, 60.0),
    false,
    "Nova Client",
    "Mouse"
  );

  EXPECT_EQ(health.at("grade"), "watch");
  EXPECT_EQ(health.at("primary_issue"), "host_render_limited");
  EXPECT_EQ(health.at("limiting_factor"), "host_render");
  EXPECT_TRUE(health.at("host_render_limited").get<bool>());
}

TEST(ProcHostPauseClassificationTests, HighRefreshNearTargetDeliveryRemainsSteady) {
  const auto classification = proc::classify_host_pause_session_for_tests(
    stable_gpu_native_stats(115.6, 120.0),
    120.0,
    false
  );

  EXPECT_EQ(classification.at("health_grade"), "good");
  EXPECT_EQ(classification.at("primary_issue"), "steady");
  EXPECT_FALSE(classification.at("host_render_limited").get<bool>());
}

TEST(ProcHostPauseClassificationTests, MeaningfulTargetMissRemainsHostRenderLimited) {
  const auto classification = proc::classify_host_pause_session_for_tests(
    stable_gpu_native_stats(54.0, 60.0),
    60.0,
    false
  );

  EXPECT_EQ(classification.at("health_grade"), "watch");
  EXPECT_EQ(classification.at("primary_issue"), "host_render_limited");
  EXPECT_TRUE(classification.at("host_render_limited").get<bool>());
}

TEST(ProcSessionLifecycleTests, TerminatedPausedAppPublishesStreamEndedWhenNoSessionsRemain) {
  EXPECT_TRUE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "paused"));
}

TEST(ProcSessionLifecycleTests, TerminatedAppDoesNotPublishStreamEndedWhileClientIsConnected) {
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 1, "streaming"));
}

TEST(ProcSessionLifecycleTests, AlreadyIdleTerminateDoesNotPublishDuplicateStreamEnded) {
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "idle"));
}

TEST(ProcSessionLifecycleTests, StreamingTerminateWaitsForStreamJoinToPublishStreamEnded) {
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "streaming"));
}
