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

TEST(KmsCaptureMetadata, PackedTenBitScanoutsReportAsTenBit) {
  using platf::kms_capture::frame_format_for_fourcc;
  EXPECT_EQ(frame_format_for_fourcc(DRM_FORMAT_XRGB2101010), platf::frame_format_e::p010);
  EXPECT_EQ(frame_format_for_fourcc(DRM_FORMAT_ARGB2101010), platf::frame_format_e::p010);
  EXPECT_EQ(frame_format_for_fourcc(DRM_FORMAT_XBGR2101010), platf::frame_format_e::p010);
  EXPECT_EQ(frame_format_for_fourcc(DRM_FORMAT_ABGR2101010), platf::frame_format_e::p010);
}

TEST(KmsCaptureMetadata, FloatScanoutsNeverReportAsEightBit) {
  // The regression. KWin composites HDR as ABGR16161616F, so a KDE host with capture = kms scans out
  // sixteen bit float. Reported as bgra8 it made capture_format say the format was ordinary while
  // PyroWave refused the very same buffer, leaving a stream carrying no video as the only evidence.
  using platf::kms_capture::frame_format_for_fourcc;
  for (const auto fourcc : {DRM_FORMAT_XRGB16161616F, DRM_FORMAT_ARGB16161616F,
                            DRM_FORMAT_XBGR16161616F, DRM_FORMAT_ABGR16161616F}) {
    EXPECT_EQ(frame_format_for_fourcc(fourcc), platf::frame_format_e::rgba16f)
      << "fourcc " << fourcc << " must not be reported as eight bit";
    EXPECT_NE(frame_format_for_fourcc(fourcc), platf::frame_format_e::bgra8);
  }
}

TEST(KmsCaptureMetadata, EightBitAndUnrecognisedScanoutsKeepTheEightBitDefault) {
  // Deliberately unchanged: consumers of this value expect eight bit BGRA for anything without its
  // own case, and narrowing that would change what every other capture path reports.
  using platf::kms_capture::frame_format_for_fourcc;
  EXPECT_EQ(frame_format_for_fourcc(DRM_FORMAT_XRGB8888), platf::frame_format_e::bgra8);
  EXPECT_EQ(frame_format_for_fourcc(DRM_FORMAT_ARGB8888), platf::frame_format_e::bgra8);
  EXPECT_EQ(frame_format_for_fourcc(DRM_FORMAT_XBGR8888), platf::frame_format_e::bgra8);
  EXPECT_EQ(frame_format_for_fourcc(DRM_FORMAT_ABGR8888), platf::frame_format_e::bgra8);
  EXPECT_EQ(frame_format_for_fourcc(DRM_FORMAT_NV12), platf::frame_format_e::bgra8);
  EXPECT_EQ(frame_format_for_fourcc(0u), platf::frame_format_e::bgra8);
}

TEST(KmsCaptureMetadata, AGpuFrameCarriesTheScanoutFormatItWasGiven) {
  // The path that matters: only a frame left on the GPU is in the scanout's format, and it is this
  // call that carries it into stream_stats and out into the doctor report as capture_format.
  const auto metadata = platf::kms_capture::frame_metadata(
    true, "/dev/dri/renderD128", platf::frame_format_e::rgba16f);
  EXPECT_EQ(metadata.format, platf::frame_format_e::rgba16f);
  EXPECT_EQ(metadata.residency, platf::frame_residency_e::gpu);
}

#include "src/platform/linux/kms_connector_selection.h"

TEST(KmsConnectorSelection, KernelConnectorNamesAndGpuIdentityDoNotReorderLegacyPositions) {
  using namespace platf::kms_selection;
  const std::vector<output_t> outputs {
    {"pci-0000:03:00.0", "DP-3"}, {"pci-0000:01:00.0", "HDMI-A-1"},
  };
  const auto names = display_names(outputs);
  ASSERT_EQ(names.size(), 2u);
  EXPECT_EQ(names[0], "kms:pci-0000:03:00.0/DP-3");
  EXPECT_EQ(names[1], "kms:pci-0000:01:00.0/HDMI-A-1");
  EXPECT_EQ(find_alias(names, "DP-3"), 0);
  EXPECT_EQ(find_alias(names, "pci-0000:01:00.0/HDMI-A-1"), 1);
  EXPECT_EQ(legacy_index("001"), 1);
  EXPECT_EQ(legacy_index(""), 0);
  EXPECT_FALSE(legacy_index("-1"));
  EXPECT_FALSE(legacy_index("1x"));
  EXPECT_FALSE(legacy_index("99999999999999999999"));
}

TEST(KmsConnectorSelection, DuplicateConnectorNamesRequireGpuQualification) {
  using namespace platf::kms_selection;
  const auto names = display_names({{"pci-0000:01:00.0", "DP-1"}, {"pci-0000:03:00.0", "DP-1"}});
  EXPECT_FALSE(find_alias(names, "DP-1"));
  EXPECT_EQ(find_alias(names, "kms:pci-0000:01:00.0/DP-1"), 0);
  EXPECT_EQ(find_alias(names, "pci-0000:03:00.0/DP-1"), 1);
  EXPECT_FALSE(find_alias(names, "pci-0000:02:00.0/DP-1"));
}

TEST(KmsConnectorSelection, MissingGpuIdentityAndAmbiguousPlanesRetainOnlyLegacySelection) {
  using namespace platf::kms_selection;
  const auto names = display_names({{"", "DP-1"}, {"pci-0000:03:00.0", "DP-1"},
                                    {"pci-0000:01:00.0", "HDMI-A-1"}, {"pci-0000:01:00.0", "HDMI-A-1"}});
  EXPECT_EQ(names[0], "0");
  EXPECT_EQ(names[2], "2");
  EXPECT_EQ(names[3], "3");
  EXPECT_FALSE(find_alias(names, "DP-1"));
  EXPECT_FALSE(find_alias(names, "HDMI-A-1"));
  EXPECT_FALSE(find_alias(names, "pci-0000:01:00.0/HDMI-A-1"));
  EXPECT_EQ(find_alias(names, "pci-0000:03:00.0/DP-1"), 1);
}

TEST(KmsConnectorSelection, UnplugAndEnumerationChangesCannotRedirectAQualifiedSelection) {
  using namespace platf::kms_selection;
  const auto before = display_names({{"pci-0000:01:00.0", "DP-1"}, {"pci-0000:03:00.0", "DP-1"}});
  const auto unplugged = display_names({{"pci-0000:03:00.0", "DP-1"}});
  const auto replugged = display_names({{"pci-0000:03:00.0", "DP-1"}, {"pci-0000:01:00.0", "DP-1"}});
  EXPECT_FALSE(find_alias(unplugged, before[0]));
  EXPECT_EQ(find_alias(replugged, before[0]), 1);
}

TEST(KmsConnectorSelection, DisconnectedOrUnboundCorrelationEntriesKeepTheirNumericPositions) {
  using namespace platf::kms_selection;
  // The first connector in a cloned-output correlation may retain a CRTC
  // after disconnect. Its numeric position must not become an unusable alias.
  const auto names = display_names({{"pci-0000:01:00.0", "DP-1", false, 9},
                                    {"pci-0000:01:00.0", "HDMI-A-1", true, 0},
                                    {"pci-0000:03:00.0", "DP-1", true, 10}});
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[0], "0");
  EXPECT_EQ(names[1], "1");
  EXPECT_EQ(names[2], "kms:pci-0000:03:00.0/DP-1");
  EXPECT_FALSE(find_alias(names, "pci-0000:01:00.0/DP-1"));
  EXPECT_EQ(find_alias(names, "pci-0000:03:00.0/DP-1"), 2);
}
