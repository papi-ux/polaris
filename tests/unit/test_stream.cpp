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
  void record_network_stats(const std::string &client_ip, double latency_ms, double packet_loss, std::uint64_t bytes_sent);
}

namespace nvhttp {
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
    stats.capture_source_fps = target_fps;
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

TEST(NvhttpSessionHealthTests, DuplicateOnlyTargetRateDeliveryDoesNotInventPacing) {
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.duplicate_frame_ratio = 0.10;

  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "Nova Client",
    "Control"
  );

  EXPECT_EQ(health.at("grade"), "good");
  EXPECT_EQ(health.at("primary_issue"), "steady");
  EXPECT_EQ(health.at("limiting_factor"), "none");
  EXPECT_EQ(health.at("auto_action"), "none");
  EXPECT_FALSE(health.at("host_render_limited").get<bool>());
}

TEST(NvhttpSessionHealthTests, DroppedFramesAtTargetRemainHostRenderLimited) {
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.dropped_frame_ratio = 0.04;

  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "Nova Client",
    "Control"
  );

  EXPECT_EQ(health.at("grade"), "watch");
  EXPECT_EQ(health.at("primary_issue"), "host_render_limited");
  EXPECT_EQ(health.at("limiting_factor"), "host_render");
  EXPECT_EQ(health.at("auto_action"), "lower_render_profile");
  EXPECT_TRUE(health.at("host_render_limited").get<bool>());
}

TEST(StreamNetworkStatsTests, OneTransientReportDoesNotClassifyCleanStreamAsNetworkLimited) {
  constexpr auto client_ip = "203.0.113.120";
  stream_stats::update_stream_active(false);
  stream_stats::add_client(client_ip, "RetroidPocket6");

  for (int i = 0; i < 6; ++i) {
    stream::record_network_stats(client_ip, 3.0, 0.0, 0);
  }

  // One ENet control-channel estimate above the cut is noise, not sustained
  // pressure. The old handler submitted this same report twice and defeated
  // the tracker's two-report debounce.
  stream::record_network_stats(client_ip, 3.0, 3.0, 0);
  auto stats = stream_stats::get_current();
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.fps = 120.0;
  stats.encode_target_fps = 120.0;
  stats.codec = "hevc";

  EXPECT_FALSE(stats.network_risk);
  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "RetroidPocket6",
    "Synthetic Frame Counter"
  );
  EXPECT_EQ(health.at("grade"), "good");
  EXPECT_EQ(health.at("primary_issue"), "steady");
  EXPECT_EQ(health.at("limiting_factor"), "none");

  // More high control-channel estimates still cannot manufacture media loss.
  stream::record_network_stats(client_ip, 3.0, 3.0, 0);
  EXPECT_FALSE(stream_stats::get_current().network_risk);
  EXPECT_FALSE(stream_stats::get_current().packet_loss_available);
  EXPECT_DOUBLE_EQ(stream_stats::get_current().control_channel_packet_loss, 3.0);

  stream_stats::remove_client(client_ip);
  stream_stats::update_stream_active(false);
}

TEST(StreamNetworkStatsTests, SecondaryClientReportDoesNotReplacePrimaryTelemetry) {
  constexpr auto primary_ip = "203.0.113.121";
  constexpr auto secondary_ip = "203.0.113.122";
  stream_stats::update_stream_active(false);
  stream_stats::add_client(primary_ip, "Primary client");
  stream_stats::add_client(secondary_ip, "Secondary client");

  stream::record_network_stats(primary_ip, 3.0, 0.0, 1000);
  for (int i = 0; i < 50; ++i) {
    stream::record_network_stats(secondary_ip, 90.0, 8.0, 2000);
  }

  const auto stats = stream_stats::get_current();
  EXPECT_DOUBLE_EQ(stats.latency_ms, 3.0);
  EXPECT_DOUBLE_EQ(stats.packet_loss, 0.0);
  EXPECT_FALSE(stats.network_risk);
  EXPECT_EQ(stats.control_channel_samples, 1u);
  EXPECT_EQ(stats.bytes_sent, 1000u);
  EXPECT_EQ(stats.clients.size(), 2u);
  if (stats.clients.size() == 2u) {
    EXPECT_DOUBLE_EQ(stats.clients[0].latency_ms, 3.0);
    EXPECT_DOUBLE_EQ(stats.clients[0].packet_loss, 0.0);
    EXPECT_DOUBLE_EQ(stats.clients[0].control_channel_packet_loss, 0.0);
    EXPECT_EQ(stats.clients[0].bytes_sent, 1000u);
    EXPECT_DOUBLE_EQ(stats.clients[1].latency_ms, 90.0);
    EXPECT_DOUBLE_EQ(stats.clients[1].packet_loss, 0.0);
    EXPECT_DOUBLE_EQ(stats.clients[1].control_channel_packet_loss, 8.0);
    EXPECT_EQ(stats.clients[1].bytes_sent, 2000u);
  }

  stream_stats::remove_client(primary_ip);
  stream_stats::remove_client(secondary_ip);
  stream_stats::update_stream_active(false);
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

TEST(ProcHostPauseClassificationTests, DuplicateOnlyTargetRateDeliveryDoesNotInventPacing) {
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.duplicate_frame_ratio = 0.10;

  const auto classification = proc::classify_host_pause_session_for_tests(
    stats,
    60.0,
    false
  );

  EXPECT_EQ(classification.at("health_grade"), "good");
  EXPECT_EQ(classification.at("primary_issue"), "steady");
  EXPECT_FALSE(classification.at("host_render_limited").get<bool>());
}

TEST(ProcHostPauseClassificationTests, DroppedFramesAtTargetRemainHostRenderLimited) {
  auto stats = stable_gpu_native_stats(60.0, 60.0);
  stats.dropped_frame_ratio = 0.04;

  const auto classification = proc::classify_host_pause_session_for_tests(
    stats,
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
