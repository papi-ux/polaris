/**
 * @file tests/unit/platform/test_portal_grab_policy.cpp
 * @brief Test XDG Desktop Portal and PipeWire capture policy.
 */

#include "../../tests_common.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <drm_fourcc.h>
#include <spa/param/video/raw.h>

#include "src/platform/common.h"
#include "src/platform/linux/pipewire_capture.h"

namespace portal {
  std::uint32_t capture_type_for_stream_display_for_tests(bool headless_mode, bool use_cage_compositor);
}

TEST(PortalGrabPolicyTests, DesktopDisplayRequestsMonitorSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display_for_tests(false, false), 1u);
}

TEST(PortalGrabPolicyTests, PrivateAndWindowedCagePathsRequestWindowSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display_for_tests(true, true), 2u);
  EXPECT_EQ(portal::capture_type_for_stream_display_for_tests(false, true), 2u);
}

TEST(PipeWireCapturePolicyTests, MapsSupportedSpaFormatsToDrmFormats) {
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_BGRx), DRM_FORMAT_XRGB8888);
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_BGRA), DRM_FORMAT_ARGB8888);
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_RGBx), DRM_FORMAT_XBGR8888);
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_RGBA), DRM_FORMAT_ABGR8888);
}

TEST(PipeWireCapturePolicyTests, RejectsUnsupportedSpaFormats) {
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_NV12), std::nullopt);
}

TEST(PipeWireCapturePolicyTests, SafeRowCopyBytesClampsToEveryLimit) {
  EXPECT_EQ(pipewire_capture::safe_row_copy_bytes(16, 24, 32), 16u);
  EXPECT_EQ(pipewire_capture::safe_row_copy_bytes(24, 16, 32), 16u);
  EXPECT_EQ(pipewire_capture::safe_row_copy_bytes(32, 24, 16), 16u);
}

TEST(PipeWireCapturePolicyTests, SafeRowCopyBytesRejectsNonPositiveSourceStride) {
  EXPECT_EQ(pipewire_capture::safe_row_copy_bytes(16, 0, 32), 0u);
  EXPECT_EQ(pipewire_capture::safe_row_copy_bytes(16, -1, 32), 0u);
}

TEST(PipeWireCapturePolicyTests, CopiesPaddedRowsAndLeavesDestinationPaddingUntouched) {
  std::array<std::uint8_t, 20> source {
    10, 20, 30, 40,
    50, 60, 70, 80,
    0xAA, 0xAA, 0xAA, 0xAA,
    90, 100, 110, 120,
    130, 140, 150, 160,
  };
  std::vector<std::uint8_t> destination(24, 0xEE);

  const auto result = pipewire_capture::copy_memptr_frame_for_tests(
    source.data(), source.size(), 0, source.size(), 2, 2, 12, SPA_VIDEO_FORMAT_BGRx, destination.data(), 12);

  ASSERT_TRUE(result);
  EXPECT_EQ(destination, (std::vector<std::uint8_t> {
    10, 20, 30, 40,
    50, 60, 70, 80,
    0xEE, 0xEE, 0xEE, 0xEE,
    90, 100, 110, 120,
    130, 140, 150, 160,
    0xEE, 0xEE, 0xEE, 0xEE,
  }));
}

TEST(PipeWireCapturePolicyTests, RejectsInsufficientSourcePayload) {
  std::array<std::uint8_t, 15> source {};
  std::array<std::uint8_t, 16> destination {};

  EXPECT_FALSE(pipewire_capture::copy_memptr_frame_for_tests(
    source.data(), source.size(), 0, source.size(), 2, 2, 8, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8));
}

TEST(PipeWireCapturePolicyTests, AppliesChunkOffsetBeforeCopying) {
  std::array<std::uint8_t, 12> source {
    0xCC, 0xCC, 0xCC, 0xCC,
    1, 2, 3, 4,
    5, 6, 7, 8,
  };
  std::array<std::uint8_t, 8> destination {};

  const auto result = pipewire_capture::copy_memptr_frame_for_tests(
    source.data(), source.size(), 4, 8, 2, 1, 8, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8);

  ASSERT_TRUE(result);
  EXPECT_EQ(destination, (std::array<std::uint8_t, 8> {1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(PipeWireCapturePolicyTests, BgrFormatsPreserveByteOrder) {
  const std::array<std::uint8_t, 8> source {1, 2, 3, 4, 5, 6, 7, 8};
  for (const auto format : {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA}) {
    std::array<std::uint8_t, 8> destination {};
    ASSERT_TRUE(pipewire_capture::copy_memptr_frame_for_tests(
      source.data(), source.size(), 0, source.size(), 2, 1, 8, format, destination.data(), 8));
    EXPECT_EQ(destination, source);
  }
}

TEST(PipeWireCapturePolicyTests, RgbFormatsSwapRedAndBlueToBgraByteOrder) {
  std::array<std::uint8_t, 8> source {
    10, 20, 30, 40,
    50, 60, 70, 80,
  };
  for (const auto format : {SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA}) {
    std::array<std::uint8_t, 8> destination {};
    ASSERT_TRUE(pipewire_capture::copy_memptr_frame_for_tests(
      source.data(), source.size(), 0, source.size(), 2, 1, 8, format, destination.data(), 8));
    EXPECT_EQ(destination, (std::array<std::uint8_t, 8> {
      30, 20, 10, 40,
      70, 60, 50, 80,
    }));
  }
}

TEST(PipeWireCapturePolicyTests, RejectsNonPositiveStride) {
  std::array<std::uint8_t, 8> source {};
  std::array<std::uint8_t, 8> destination {};

  EXPECT_FALSE(pipewire_capture::copy_memptr_frame_for_tests(
    source.data(), source.size(), 0, source.size(), 2, 1, 0, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8));
  EXPECT_FALSE(pipewire_capture::copy_memptr_frame_for_tests(
    source.data(), source.size(), 0, source.size(), 2, 1, -8, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8));
}

TEST(PipeWireCapturePolicyTests, StreamTransitionRulesOnlyTreatTerminalStatesAsErrors) {
  using pipewire_capture::stream_state_e;

  EXPECT_FALSE(pipewire_capture::is_terminal_transition_for_tests(
    stream_state_e::connecting, stream_state_e::paused));
  EXPECT_TRUE(pipewire_capture::is_terminal_transition_for_tests(
    stream_state_e::streaming, stream_state_e::paused));
  EXPECT_TRUE(pipewire_capture::is_terminal_transition_for_tests(
    stream_state_e::paused, stream_state_e::error));
  EXPECT_TRUE(pipewire_capture::is_terminal_transition_for_tests(
    stream_state_e::streaming, stream_state_e::unconnected));
}

TEST(PipeWireCapturePolicyTests, CpuFramesReportSharedMemoryMetadata) {
  const auto metadata = pipewire_capture::cpu_frame_metadata();

  EXPECT_EQ(metadata.transport, platf::frame_transport_e::shm);
  EXPECT_EQ(metadata.residency, platf::frame_residency_e::cpu);
  EXPECT_EQ(metadata.format, platf::frame_format_e::bgra8);
  EXPECT_TRUE(metadata.device.empty());
}

TEST(PipeWireCapturePolicyTests, DmaBufFramesReportGpuAndRenderNodeMetadata) {
  const auto metadata = pipewire_capture::dmabuf_frame_metadata("/dev/dri/renderD128");

  EXPECT_EQ(metadata.transport, platf::frame_transport_e::dmabuf);
  EXPECT_EQ(metadata.residency, platf::frame_residency_e::gpu);
  EXPECT_EQ(metadata.format, platf::frame_format_e::bgra8);
  EXPECT_EQ(metadata.device, "/dev/dri/renderD128");
}
