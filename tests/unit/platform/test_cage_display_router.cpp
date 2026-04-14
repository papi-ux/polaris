/**
 * @file tests/unit/platform/test_cage_display_router.cpp
 * @brief Test Linux labwc runtime policy helpers.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/cage_display_router.h>

TEST(CageDisplayRouterPolicyTests, HeadlessNvencDoesNotForceWindowedWhenNestedGpuNativeCaptureIsUnavailable) {
  EXPECT_FALSE(cage_display_router::windowed_gpu_native_capture_supported());
  EXPECT_FALSE(cage_display_router::should_force_windowed_for_gpu_native_capture(
    true,
    true,
    true
  ));
}

TEST(CageDisplayRouterPolicyTests, OverrideRequiresHeadlessGpuPreferenceAndEncoderRequirement) {
  EXPECT_FALSE(cage_display_router::should_force_windowed_for_gpu_native_capture(
    false,
    true,
    true
  ));
  EXPECT_FALSE(cage_display_router::should_force_windowed_for_gpu_native_capture(
    true,
    false,
    true
  ));
  EXPECT_FALSE(cage_display_router::should_force_windowed_for_gpu_native_capture(
    true,
    true,
    false
  ));
}

#ifdef POLARIS_TESTS
TEST(CageDisplayRouterPolicyTests, WindowedRamCaptureFallbackWarningLogsOnlyOnce) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_TRUE(cage_display_router::should_log_windowed_ram_capture_warning());
  EXPECT_FALSE(cage_display_router::should_log_windowed_ram_capture_warning());
}
#endif
#else
TEST(CageDisplayRouterPolicyTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only runtime policy tests";
}
#endif
