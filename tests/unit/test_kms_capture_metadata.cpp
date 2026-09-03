/**
 * @file tests/unit/test_kms_capture_metadata.cpp
 * @brief Unit tests for DRM/KMS capture metadata.
 */
#include <gtest/gtest.h>

#include "src/platform/linux/kms_capture_metadata.h"

TEST(KmsCaptureMetadata, DirectFramesDescribeGpuNativeDmabufCapture) {
  const auto metadata = platf::kms_capture::frame_metadata(true, "/dev/dri/renderD128");

  EXPECT_EQ(metadata.transport, platf::frame_transport_e::dmabuf);
  EXPECT_EQ(metadata.residency, platf::frame_residency_e::gpu);
  EXPECT_EQ(metadata.format, platf::frame_format_e::bgra8);
  EXPECT_EQ(metadata.device, "/dev/dri/renderD128");
}

TEST(KmsCaptureMetadata, ReadbackFramesRetainDmabufOriginAndReportCpuResidency) {
  const auto metadata = platf::kms_capture::frame_metadata(false, {});

  EXPECT_EQ(metadata.transport, platf::frame_transport_e::dmabuf);
  EXPECT_EQ(metadata.residency, platf::frame_residency_e::cpu);
  EXPECT_EQ(metadata.format, platf::frame_format_e::bgra8);
  EXPECT_TRUE(metadata.device.empty());
}
