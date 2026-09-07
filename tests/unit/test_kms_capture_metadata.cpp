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
