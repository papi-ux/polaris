/**
 * @file tests/unit/platform/test_cage_display_router.cpp
 * @brief Test Linux labwc runtime policy helpers.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/cage_display_router.h>

TEST(CageDisplayRouterPolicyTests, HeadlessNvencRequestsWindowedGpuNativeProbeWhenAllConditionsMatch) {
  EXPECT_TRUE(cage_display_router::should_attempt_windowed_gpu_native_probe(
    true,
    true,
    true
  ));
}

TEST(CageDisplayRouterPolicyTests, OverrideRequiresHeadlessGpuPreferenceAndEncoderRequirement) {
  EXPECT_FALSE(cage_display_router::should_attempt_windowed_gpu_native_probe(
    false,
    true,
    true
  ));
  EXPECT_FALSE(cage_display_router::should_attempt_windowed_gpu_native_probe(
    true,
    false,
    true
  ));
  EXPECT_FALSE(cage_display_router::should_attempt_windowed_gpu_native_probe(
    true,
    true,
    false
  ));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideAttemptsGpuNativeCapture) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state));
}

TEST(CageDisplayRouterPolicyTests, HeadlessRuntimeDoesNotAttemptGpuNativeCapture) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state));
}

TEST(CageDisplayRouterPolicyTests, RequestedHeadlessWithoutOverrideStillReportsHeadlessFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_report_headless_ram_capture_fallback(runtime_state));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideDoesNotReportHeadlessFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_report_headless_ram_capture_fallback(runtime_state));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideReportsWindowedFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_report_windowed_ram_capture_fallback(runtime_state));
}

TEST(CageDisplayRouterPolicyTests, EffectiveHeadlessDoesNotReportWindowedFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_report_windowed_ram_capture_fallback(runtime_state));
}

#ifdef POLARIS_TESTS
TEST(CageDisplayRouterPolicyTests, WindowedRamCaptureFallbackWarningLogsOnlyOnce) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_TRUE(cage_display_router::should_log_windowed_ram_capture_warning());
  EXPECT_FALSE(cage_display_router::should_log_windowed_ram_capture_warning());
}

TEST(CageDisplayRouterPolicyTests, HeadlessRamCaptureFallbackWarningLogsOnlyOnce) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_TRUE(cage_display_router::should_log_headless_ram_capture_warning());
  EXPECT_FALSE(cage_display_router::should_log_headless_ram_capture_warning());
}

TEST(CageDisplayRouterPolicyTests, WindowedGpuNativeProbeResultDefaultsToUnknown) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_FALSE(cage_display_router::cached_windowed_gpu_native_probe_result().has_value());
}

TEST(CageDisplayRouterPolicyTests, WindowedGpuNativeProbeResultCachesFailure) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_windowed_gpu_native_probe_result(false);

  EXPECT_EQ(cage_display_router::cached_windowed_gpu_native_probe_result(), std::optional<bool> {false});
}

TEST(CageDisplayRouterPolicyTests, WindowedGpuNativeProbeResultCachesSuccess) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_windowed_gpu_native_probe_result(true);

  EXPECT_EQ(cage_display_router::cached_windowed_gpu_native_probe_result(), std::optional<bool> {true});
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRecognizesRequestedCurrentMode) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1280x720 px, 60.000000 Hz
    1920x1080 px, 60.000000 Hz (current)
)";

  EXPECT_TRUE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080
  ));
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRejectsNonCurrentRequestedMode) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1280x720 px, 60.000000 Hz (current)
    1920x1080 px, 60.000000 Hz
)";

  EXPECT_FALSE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080
  ));
}
#endif
#else
TEST(CageDisplayRouterPolicyTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only runtime policy tests";
}
#endif
