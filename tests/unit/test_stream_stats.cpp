/**
 * @file tests/unit/test_stream_stats.cpp
 * @brief Test src/stream_stats.*.
 */

#include <src/stream_stats.h>

#include <gtest/gtest.h>

TEST(StreamStatsCapturePathTests, UnknownWhenNoPathMetadataExists) {
  stream_stats::stats_t stats {};

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "unknown");
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, DetectsShmCpuCapture) {
  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.encode_target_residency = platf::frame_residency_e::cpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "shm_cpu_capture");
  EXPECT_TRUE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, DetectsCpuEncodeUpload) {
  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::cpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "cpu_encode_upload");
  EXPECT_TRUE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, DetectsFullyGpuNativePath) {
  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "gpu_native");
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_TRUE(stream_stats::capture_path_is_gpu_native(stats));
}
