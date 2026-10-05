/** @file tests/unit/test_vulkan_capture_admission.cpp */
#include "../tests_common.h"

#if defined(__linux__)
  #include <array>
  #include <climits>
  #include "src/platform/linux/vulkan_capture_admission.h"

namespace {
  struct layout_t {
    int width = 256;
    int height = 144;
    std::uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    std::array<int, 4> fds {10, -1, -1, -1};
    std::array<std::uint32_t, 4> pitches {1024, 0, 0, 0};

    std::string_view error(std::uint32_t bytes_per_pixel = 4) const {
      return vk::capture_admission::metadata_error(width, height, modifier, fds, pitches, bytes_per_pixel);
    }
  };
}

TEST(VulkanCaptureAdmissionTests, PackedRgbAndTenBitFormatsHaveExactMappings) {
  const auto rgb = vk::capture_admission::format_info(DRM_FORMAT_XRGB8888);
  ASSERT_TRUE(rgb);
  EXPECT_EQ(rgb->format, VK_FORMAT_B8G8R8A8_UNORM);
  EXPECT_EQ(rgb->bytes_per_pixel, 4);
  const auto ten = vk::capture_admission::format_info(DRM_FORMAT_ABGR2101010);
  ASSERT_TRUE(ten);
  EXPECT_EQ(ten->format, VK_FORMAT_A2B10G10R10_UNORM_PACK32);
  EXPECT_EQ(ten->bytes_per_pixel, 4);
}

TEST(VulkanCaptureAdmissionTests, SixteenBitFormatsKeepTheirPixelSizeAndChannelOrder) {
  const auto rgb = vk::capture_admission::format_info(DRM_FORMAT_XRGB16161616F);
  ASSERT_TRUE(rgb);
  EXPECT_EQ(rgb->format, VK_FORMAT_R16G16B16A16_SFLOAT);
  EXPECT_EQ(rgb->bytes_per_pixel, 8);
  EXPECT_EQ(rgb->swizzle.r, VK_COMPONENT_SWIZZLE_B);
  const auto bgr = vk::capture_admission::format_info(DRM_FORMAT_ABGR16161616);
  ASSERT_TRUE(bgr);
  EXPECT_EQ(bgr->swizzle.r, VK_COMPONENT_SWIZZLE_IDENTITY);
}

TEST(VulkanCaptureAdmissionTests, UnknownAndYuvFormatsAreNeverAssumedToBeBgra) {
  EXPECT_FALSE(vk::capture_admission::format_info(0));
  EXPECT_FALSE(vk::capture_admission::format_info(DRM_FORMAT_NV12));
  EXPECT_FALSE(vk::capture_admission::format_info(DRM_FORMAT_YUV420));
}

TEST(VulkanCaptureAdmissionTests, ExplicitLinearPackedPixelsHaveValidMetadata) {
  EXPECT_TRUE(layout_t {}.error().empty());
}

TEST(VulkanCaptureAdmissionTests, NonpositiveDimensionsAreRefused) {
  layout_t layout;
  layout.width = 0;
  EXPECT_FALSE(layout.error().empty());
  layout.width = 256;
  layout.height = -1;
  EXPECT_FALSE(layout.error().empty());
}

TEST(VulkanCaptureAdmissionTests, UnspecifiedTilingIsRefused) {
  layout_t layout;
  layout.modifier = DRM_FORMAT_MOD_INVALID;
  EXPECT_FALSE(layout.error().empty());
}

TEST(VulkanCaptureAdmissionTests, MissingOrGappedPlanesAreRefused) {
  layout_t layout;
  layout.fds[0] = -1;
  EXPECT_FALSE(layout.error().empty());
  layout.fds[0] = 10;
  layout.fds[2] = 12;
  layout.pitches[2] = 1024;
  EXPECT_FALSE(layout.error().empty());
}

TEST(VulkanCaptureAdmissionTests, EveryPresentPlaneNeedsItsOwnPitch) {
  layout_t layout;
  layout.fds[1] = 11;
  EXPECT_FALSE(layout.error().empty());
  layout.pitches[1] = 256;
  layout.modifier = 1;
  EXPECT_TRUE(layout.error().empty());
}

TEST(VulkanCaptureAdmissionTests, LinearPitchAccountsForTheActualPixelWidth) {
  layout_t layout;
  EXPECT_FALSE(layout.error(8).empty());
  layout.pitches[0] = 2048;
  EXPECT_TRUE(layout.error(8).empty());
  layout.width = INT_MAX;
  layout.pitches[0] = UINT32_MAX;
  EXPECT_FALSE(layout.error(8).empty());
}

TEST(VulkanCaptureAdmissionTests, ModifierSpecificPitchIsLeftToTheDriver) {
  layout_t layout;
  layout.modifier = 1;
  layout.pitches[0] = 64;
  EXPECT_TRUE(layout.error().empty());
}
#endif
