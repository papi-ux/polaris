/**
 * @file tests/unit/test_display_planner.cpp
 * @brief Pin the display planner to its JS reference and the Nova fixture.
 *
 * The planner exists three times: display-resolution-planner.js is the
 * reference, this port serves it, and Nova's PolarisGameContractTest fixture
 * decodes it. The 2560x1600x90 values asserted here are the ones that fixture
 * pins on the Nova side — if these move, that fixture is the other thing that
 * has to move, in the same breath.
 */
#include <limits>

#include <gtest/gtest.h>

#include "src/display_planner.h"

using display_planner::build_resolution_planner;
using display_planner::clamp_planner_scale;
using display_planner::format_display_mode;
using display_planner::limits_t;
using display_planner::mode_t;
using display_planner::parse_display_mode;
using display_planner::resolve_scaled_mode;

TEST(DisplayPlannerTests, ParsesModeStringsExactlyLikeTheWebPlanner) {
  const auto mode = parse_display_mode("2560x1600x90");
  ASSERT_TRUE(mode.has_value());
  EXPECT_EQ(2560, mode->width);
  EXPECT_EQ(1600, mode->height);
  EXPECT_EQ(90, mode->fps);

  const auto fractional = parse_display_mode("  1920x1080x59.94\t");
  ASSERT_TRUE(fractional.has_value());
  EXPECT_DOUBLE_EQ(59.94, fractional->fps);

  for (const auto *rejected : {"", "garbage", "1920x1080", "1920x1080x", "1920x1080x60x2", "1920 x1080x60", "-1920x1080x60", "1920x1080x60.", "1920x1080x.5"}) {
    EXPECT_FALSE(parse_display_mode(rejected).has_value()) << rejected;
  }
}

TEST(DisplayPlannerTests, FormatsIntegralFpsWithoutDecimals) {
  EXPECT_EQ("2560x1600x90", format_display_mode({2560, 1600, 90}));
  EXPECT_EQ("1920x1080x59.94", format_display_mode({1920, 1080, 59.94}));
}

TEST(DisplayPlannerTests, ClampsPlannerScaleIntoTheSupportedRange) {
  EXPECT_DOUBLE_EQ(0.5, clamp_planner_scale(0.1));
  EXPECT_DOUBLE_EQ(2, clamp_planner_scale(3));
  EXPECT_DOUBLE_EQ(1.25, clamp_planner_scale(1.25));
  EXPECT_DOUBLE_EQ(1, clamp_planner_scale(std::numeric_limits<double>::quiet_NaN()));
}

TEST(DisplayPlannerTests, RoundsScaledDimensionsUpToEvenAndNeverBelowTwo) {
  // 1366 * 0.75 = 1024.5 rounds to 1025, which is odd, so it becomes 1026.
  const auto scaled = resolve_scaled_mode({1366, 768, 60}, 0.75);
  EXPECT_EQ(1026, scaled.width);
  EXPECT_EQ(576, scaled.height);
  EXPECT_EQ(60, scaled.fps);

  const auto tiny = resolve_scaled_mode({2, 2, 60}, 0.5);
  EXPECT_EQ(2, tiny.width);
  EXPECT_EQ(2, tiny.height);
}

TEST(DisplayPlannerTests, BuildsTheSharedFixturePlanFor2560x1600x90) {
  const auto plan = build_resolution_planner({2560, 1600, 90});

  EXPECT_EQ("2560x1600x90", plan.source_mode);
  EXPECT_EQ("8:5", plan.source_aspect_ratio);
  EXPECT_EQ(90, plan.source.fps);

  ASSERT_EQ(5U, plan.choices.size());
  EXPECT_EQ("native", plan.choices[0].id);
  EXPECT_EQ("balanced", plan.choices[1].id);
  EXPECT_EQ("sharp", plan.choices[2].id);
  EXPECT_EQ("performance", plan.choices[3].id);
  EXPECT_EQ("custom", plan.choices[4].id);

  EXPECT_EQ("balanced", plan.recommended_id);
  EXPECT_EQ("Best for this device", plan.recommended_title);
  EXPECT_EQ("1920x1200x90", plan.recommended_mode);

  EXPECT_EQ("2560x1600x90", plan.choices[0].target_mode);
  EXPECT_EQ("2560×1600", plan.choices[0].badge);
  EXPECT_EQ("1920x1200x90", plan.choices[1].target_mode);
  EXPECT_EQ("Best for this device", plan.choices[1].badge);
  EXPECT_EQ("3840x2400x90", plan.choices[2].target_mode);
  EXPECT_EQ("1.5x supersample", plan.choices[2].badge);
  EXPECT_TRUE(plan.choices[2].advanced);
  EXPECT_EQ("1280x800x90", plan.choices[3].target_mode);
  EXPECT_EQ("0.5x downscale", plan.choices[3].badge);
  EXPECT_EQ("Advanced", plan.choices[4].badge);
  EXPECT_TRUE(plan.choices[4].custom);

  for (const auto &choice : plan.choices) {
    EXPECT_TRUE(choice.safe) << choice.id;
    EXPECT_FALSE(choice.hidden) << choice.id;
    EXPECT_EQ("8:5", choice.aspect_ratio) << choice.id;
    EXPECT_EQ(choice.intent, choice.reason) << choice.id;
  }

  ASSERT_EQ(6U, plan.advanced_scale_factors.size());
  EXPECT_EQ("0.5x", plan.advanced_scale_factors[0].label);
  EXPECT_EQ("0.75x", plan.advanced_scale_factors[1].label);
  EXPECT_EQ("1x", plan.advanced_scale_factors[2].label);
  EXPECT_EQ("1.25x", plan.advanced_scale_factors[3].label);
  EXPECT_EQ("1.5x", plan.advanced_scale_factors[4].label);
  EXPECT_EQ("2x", plan.advanced_scale_factors[5].label);
  EXPECT_EQ("5120x3200x90", plan.advanced_scale_factors[5].target_mode);
  EXPECT_TRUE(plan.advanced_scale_factors[5].safe);
}

TEST(DisplayPlannerTests, HidesUnsafeChoicesAndKeepsTheRecommendationSafe) {
  // At 7680x4320 the native pixel count sits exactly on the envelope, so
  // native stays safe while every upscale beyond it goes over.
  const auto plan = build_resolution_planner({7680, 4320, 60}, 2.0);

  EXPECT_TRUE(plan.choices[0].safe);
  EXPECT_TRUE(plan.choices[1].safe);
  EXPECT_FALSE(plan.choices[2].safe);
  EXPECT_TRUE(plan.choices[2].hidden);
  EXPECT_EQ("Hidden because this would exceed the safe display-mode envelope for normal users.", plan.choices[2].reason);
  EXPECT_FALSE(plan.choices[4].safe) << "custom at 2x doubles an already-maximal mode";

  EXPECT_EQ("balanced", plan.recommended_id);
  EXPECT_EQ("5760x3240x60", plan.recommended_mode);
}

TEST(DisplayPlannerTests, FallsBackToTheFirstChoiceWhenNothingIsSafe) {
  const auto plan = build_resolution_planner({20000, 20000, 60});

  for (const auto &choice : plan.choices) {
    EXPECT_FALSE(choice.safe) << choice.id;
    EXPECT_TRUE(choice.hidden) << choice.id;
  }
  EXPECT_EQ("native", plan.recommended_id);
}

TEST(DisplayPlannerTests, NormalizesDegenerateSourcesToTheDefaultDevice) {
  const auto plan = build_resolution_planner({0, -5, std::numeric_limits<double>::infinity()});
  EXPECT_EQ("1920x1080x60", plan.source_mode);
  EXPECT_EQ("16:9", plan.source_aspect_ratio);
}

TEST(DisplayPlannerTests, CustomChoiceUsesTheClampedCustomScale) {
  const auto plan = build_resolution_planner({2560, 1600, 90}, 0.3);

  const auto &custom = plan.choices[4];
  EXPECT_DOUBLE_EQ(0.5, custom.scale_factor);
  EXPECT_EQ("1280x800x90", custom.target_mode);
  EXPECT_TRUE(custom.advanced);
  EXPECT_TRUE(custom.custom);
}
