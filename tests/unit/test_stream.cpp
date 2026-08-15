/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <cstdint>
#include <limits>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <src/stream_stats.h>
#include <string>
#include <string_view>
#include <vector>

namespace stream {
  void concat_and_insert_into(std::vector<uint8_t> &result, uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  bool control_packet_carries_type(std::size_t data_length);
  bool encrypted_control_header_present(std::size_t payload_size);
  bool encrypted_control_cipher_fits(std::size_t payload_size, std::uint16_t declared_length);
  bool input_control_cipher_fits(std::size_t payload_size, std::int32_t declared_cipher_length);
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

// Peer-supplied lengths on the control channel. Everything below arrives off
// the wire from a client that has established a session, so each of these
// bounds is the only thing standing between a declared size and a read.

TEST(ControlPacketBounds, APacketShorterThanItsTypeFieldIsRejected) {
  // dataLength - sizeof(type) wraps below 2, so the payload view would claim
  // the entire address space.
  EXPECT_FALSE(stream::control_packet_carries_type(0));
  EXPECT_FALSE(stream::control_packet_carries_type(1));
  EXPECT_TRUE(stream::control_packet_carries_type(2));
  EXPECT_TRUE(stream::control_packet_carries_type(1024));
}

TEST(ControlPacketBounds, EncryptedHeaderMustHaveArrived) {
  // The dispatcher consumed the 2-byte type, so 6 payload bytes complete the
  // 8-byte header the handler reads length and seq from.
  EXPECT_FALSE(stream::encrypted_control_header_present(0));
  EXPECT_FALSE(stream::encrypted_control_header_present(5));
  EXPECT_TRUE(stream::encrypted_control_header_present(6));
}

TEST(ControlPacketBounds, EncryptedCipherLengthIsBoundedByWhatArrived) {
  // 24 is the runt floor; the cipher is length - 4 bytes past the header.
  EXPECT_FALSE(stream::encrypted_control_cipher_fits(6, 23));  // below the floor
  EXPECT_FALSE(stream::encrypted_control_cipher_fits(6, 24));  // declares 20, none arrived
  EXPECT_TRUE(stream::encrypted_control_cipher_fits(26, 24));  // declares 20, 20 arrived
  EXPECT_TRUE(stream::encrypted_control_cipher_fits(1024, 24));

  // A uint16 maxes out at 65535, so the over-read was bounded but real.
  EXPECT_FALSE(stream::encrypted_control_cipher_fits(64, 65535));
}

TEST(ControlPacketBounds, InputCipherLengthRejectsNegativeAndOverlongClaims) {
  EXPECT_FALSE(stream::input_control_cipher_fits(3, 0));   // no room for the length itself
  EXPECT_TRUE(stream::input_control_cipher_fits(4, 0));
  EXPECT_TRUE(stream::input_control_cipher_fits(20, 16));
  EXPECT_FALSE(stream::input_control_cipher_fits(20, 17));  // one byte past the buffer

  // Signed off the wire: -1 would widen to SIZE_MAX as a view length.
  EXPECT_FALSE(stream::input_control_cipher_fits(1024, -1));
  EXPECT_FALSE(stream::input_control_cipher_fits(1024, std::numeric_limits<std::int32_t>::min()));
  EXPECT_FALSE(stream::input_control_cipher_fits(1024, std::numeric_limits<std::int32_t>::max()));
}
