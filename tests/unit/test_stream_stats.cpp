/**
 * @file tests/unit/test_stream_stats.cpp
 * @brief Test src/stream_stats.*.
 */

#include <src/stream_stats.h>
#include <src/config.h>

#include <gtest/gtest.h>

namespace {
  struct LinuxDisplayConfigGuard {
    LinuxDisplayConfigGuard():
        use_cage_compositor {config::video.linux_display.use_cage_compositor},
        prefer_gpu_native_capture {config::video.linux_display.prefer_gpu_native_capture} {
    }

    ~LinuxDisplayConfigGuard() {
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
    }

    bool use_cage_compositor;
    bool prefer_gpu_native_capture;
  };
}  // namespace

TEST(StreamStatsCapturePathTests, UnknownWhenNoPathMetadataExists) {
  stream_stats::stats_t stats {};

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "unknown");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "no_capture_metadata");
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, DetectsShmCpuCapture) {
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;

  stream_stats::stats_t stats {};
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.encode_target_residency = platf::frame_residency_e::cpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "shm_cpu_capture");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "headless_shm_fallback");
  EXPECT_TRUE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, ExplainsGpuNativeShmFallback) {
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = true;

  stream_stats::stats_t stats {};
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.encode_target_residency = platf::frame_residency_e::cpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "shm_cpu_capture");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "headless_shm_fallback");
  EXPECT_TRUE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, DetectsCpuEncodeUpload) {
  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::cpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "cpu_encode_upload");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "encoder_upload_cpu");
  EXPECT_TRUE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, DetectsFullyGpuNativePath) {
  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "gpu_native");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "gpu_native");
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_TRUE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, LabelsHeadlessExtcopyDmabufPath) {
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "gpu_native");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "headless_extcopy_dmabuf");
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_TRUE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, LabelsWindowedDmabufOverridePath) {
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.runtime_gpu_native_override_active = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "gpu_native");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "windowed_dmabuf_override");
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_TRUE(stream_stats::capture_path_is_gpu_native(stats));
}
