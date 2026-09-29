/**
 * @file tests/unit/test_output_layout.cpp
 * @brief Test where each output sits on the desktop, in the desktop's own units.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>

#include "src/platform/linux/output_layout.h"

namespace {
  using output_layout::bounds;
  using output_layout::logical_rect;
  using output_layout::on_desktop;
  using output_layout::output_t;
  using output_layout::rect_t;
  namespace transform = output_layout::transform;

  /**
   * @brief An output as a compositor describes it in full: xdg-output's position and logical size,
   *        and wl_output's mode, transform and scale.
   */
  output_t described(int x, int y, int logical_width, int logical_height, int mode_width, int mode_height, std::int32_t output_transform = transform::normal, std::int32_t scale = 1) {
    return {
      .x = x,
      .y = y,
      .logical_width = logical_width,
      .logical_height = logical_height,
      .mode_width = mode_width,
      .mode_height = mode_height,
      .transform = output_transform,
      .scale = scale,
    };
  }

  /**
   * @brief The same output from a compositor that sent a position but no logical size.
   */
  output_t mode_only(int x, int y, int mode_width, int mode_height, std::int32_t output_transform = transform::normal, std::int32_t scale = 1) {
    return described(x, y, 0, 0, mode_width, mode_height, output_transform, scale);
  }
}  // namespace

// polaris#793: a 2560x1440 monitor, and a 1920x1080 monitor turned a quarter to its right. The
// turned monitor is 1080 wide on the desktop, so the desktop is 3640 wide, not 2560 + 1920.
TEST(OutputLayout, ReportersRotatedMonitorIsMeasuredByItsHeight) {
  const std::array outputs {
    logical_rect(described(0, 0, 2560, 1440, 2560, 1440)),
    logical_rect(described(2560, 0, 1080, 1920, 1920, 1080, transform::turned_90)),
  };

  EXPECT_EQ(outputs[0], (rect_t {0, 0, 2560, 1440}));
  EXPECT_EQ(outputs[1], (rect_t {2560, 0, 1080, 1920}));
  EXPECT_EQ(bounds(outputs), (rect_t {0, 0, 3640, 1920}));
}

// The same desktop from a compositor that sends no logical size: the transform turns the mode.
TEST(OutputLayout, ReportersLayoutWithoutLogicalSizesTurnsTheMode) {
  const std::array outputs {
    logical_rect(mode_only(0, 0, 2560, 1440)),
    logical_rect(mode_only(2560, 0, 1920, 1080, transform::turned_90)),
  };

  EXPECT_EQ(outputs[1], (rect_t {2560, 0, 1080, 1920}));
  EXPECT_EQ(bounds(outputs), (rect_t {0, 0, 3640, 1920}));
}

// Four transforms turn an output a quarter, flipped ones included. The other four keep its shape.
TEST(OutputLayout, EveryQuarterTurnSwapsTheModeAndNoOtherTransformDoes) {
  for (const auto turned : {transform::turned_90, transform::turned_270, transform::flipped_90, transform::flipped_270}) {
    EXPECT_TRUE(output_layout::turns_a_quarter(turned)) << "transform " << turned;
    EXPECT_EQ(logical_rect(mode_only(0, 0, 1920, 1080, turned)), (rect_t {0, 0, 1080, 1920})) << "transform " << turned;
  }
  for (const auto kept : {transform::normal, transform::turned_180, transform::flipped, transform::flipped_180}) {
    EXPECT_FALSE(output_layout::turns_a_quarter(kept)) << "transform " << kept;
    EXPECT_EQ(logical_rect(mode_only(0, 0, 1920, 1080, kept)), (rect_t {0, 0, 1920, 1080})) << "transform " << kept;
  }
}

// A HiDPI monitor at scale 2 covers half its mode each way, beside a plain one.
TEST(OutputLayout, ScaleTwoCoversHalfTheModeEachWay) {
  EXPECT_EQ(logical_rect(described(0, 0, 1920, 1080, 3840, 2160, transform::normal, 2)), (rect_t {0, 0, 1920, 1080}));
  EXPECT_EQ(logical_rect(mode_only(0, 0, 3840, 2160, transform::normal, 2)), (rect_t {0, 0, 1920, 1080}));

  const std::array outputs {
    logical_rect(mode_only(0, 0, 3840, 2160, transform::normal, 2)),
    logical_rect(mode_only(1920, 0, 2560, 1440)),
  };
  EXPECT_EQ(bounds(outputs), (rect_t {0, 0, 4480, 1440}));
}

// wl_output.scale is a whole number, so a compositor at 1.5 sends 2. xdg-output's logical size is
// the real one and wins.
TEST(OutputLayout, FractionalScaleTakesXdgOutputsSize) {
  EXPECT_EQ(logical_rect(described(0, 0, 2560, 1440, 3840, 2160, transform::normal, 2)), (rect_t {0, 0, 2560, 1440}));
}

TEST(OutputLayout, TurnedAndScaledTogether) {
  EXPECT_EQ(logical_rect(mode_only(0, 0, 3840, 2160, transform::flipped_270, 2)), (rect_t {0, 0, 1080, 1920}));
}

// A scale the compositor never sent, or sent as nonsense, divides by nothing.
TEST(OutputLayout, MissingScaleCountsAsOne) {
  EXPECT_EQ(logical_rect(mode_only(0, 0, 1920, 1080, transform::normal, 0)), (rect_t {0, 0, 1920, 1080}));
}

// Plain monitors come out as they always did: their modes, side by side from the origin.
TEST(OutputLayout, UnrotatedUnscaledOutputsAreUnchanged) {
  const std::array outputs {
    logical_rect(described(0, 0, 2560, 1440, 2560, 1440)),
    logical_rect(described(2560, 0, 1920, 1080, 1920, 1080)),
  };

  EXPECT_EQ(outputs[0], (rect_t {0, 0, 2560, 1440}));
  EXPECT_EQ(outputs[1], (rect_t {2560, 0, 1920, 1080}));
  const auto desktop = bounds(outputs);
  EXPECT_EQ(desktop, (rect_t {0, 0, 4480, 1440}));
  EXPECT_EQ(on_desktop(outputs[1], desktop), outputs[1]);
}

// A monitor left of the origin moves the desktop's corner there, and input counts from it.
TEST(OutputLayout, DesktopCornerIsTheTopLeftMostOutput) {
  const std::array outputs {
    rect_t {-1920, 0, 1920, 1080},
    rect_t {0, -200, 2560, 1440},
  };

  const auto desktop = bounds(outputs);
  EXPECT_EQ(desktop, (rect_t {-1920, -200, 4480, 1440}));
  EXPECT_EQ(on_desktop(outputs[0], desktop), (rect_t {0, 200, 1920, 1080}));
  EXPECT_EQ(on_desktop(outputs[1], desktop), (rect_t {1920, 0, 2560, 1440}));
}

// An output that has not been sized yet covers nothing, wherever it says it is.
TEST(OutputLayout, AnOutputWithNoAreaHoldsNothing) {
  const std::array outputs {
    rect_t {0, 0, 2560, 1440},
    rect_t {5000, 0, 0, 0},
  };

  EXPECT_EQ(bounds(outputs), (rect_t {0, 0, 2560, 1440}));
  EXPECT_EQ(bounds(std::span<const rect_t> {}), (rect_t {}));
}
