/**
 * @file tests/unit/test_output_layout.cpp
 * @brief Test where each output sits on the desktop, in the desktop's own units.
 */
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "src/platform/linux/output_layout.h"

namespace {
  using output_layout::bounds;
  using output_layout::crtc_input_rect;
  using output_layout::crtc_output_t;
  using output_layout::desktop_scale;
  using output_layout::input_rect;
  using output_layout::logical_rect;
  using output_layout::measure_crtc_desktop;
  using output_layout::measure_desktop;
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

// The desktop counts in the pixels of its most detailed monitor, and never in fewer than one to
// a logical unit.
TEST(OutputLayout, DesktopScaleIsTheLargestWholeScale) {
  const std::array mixed {
    mode_only(0, 0, 1920, 1080),
    mode_only(1920, 0, 3840, 2160, transform::normal, 2),
    mode_only(3840, 0, 5120, 2880, transform::normal, 3),
  };
  EXPECT_EQ(desktop_scale(mixed), 3);
  EXPECT_EQ(desktop_scale(std::span<const output_t> {}), 1);

  const std::array unsent {mode_only(0, 0, 1920, 1080, transform::normal, 0)};
  EXPECT_EQ(desktop_scale(unsent), 1);
}

// A lone 3840x2160 monitor at scale 2 measures what its mode does, so input on it has the numbers
// it always had.
TEST(OutputLayout, ALoneScaleTwoMonitorMeasuresWhatItsModeDoes) {
  const std::array outputs {described(0, 0, 1920, 1080, 3840, 2160, transform::normal, 2)};

  const auto desktop = measure_desktop(outputs);
  EXPECT_EQ(desktop.scale, 2);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 3840, 2160}));
  EXPECT_EQ(input_rect(outputs[0], desktop, 3840, 2160), (rect_t {0, 0, 3840, 2160}));
}

// A scale 2 monitor beside a plain one: two desktop pixels to each logical unit on both.
TEST(OutputLayout, MixedScaleDesktopCountsInTheSharpestMonitorsPixels) {
  const std::array outputs {
    described(0, 0, 1920, 1080, 3840, 2160, transform::normal, 2),
    described(1920, 0, 2560, 1440, 2560, 1440),
  };

  const auto desktop = measure_desktop(outputs);
  EXPECT_EQ(desktop.scale, 2);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 8960, 2880}));
  EXPECT_EQ(input_rect(outputs[0], desktop, 3840, 2160), (rect_t {0, 0, 3840, 2160}));
  EXPECT_EQ(input_rect(outputs[1], desktop, 2560, 1440), (rect_t {3840, 0, 5120, 2880}));
}

// The reporter's desktop has no scaled monitor, so desktop pixels are logical units there.
TEST(OutputLayout, ReportersDesktopCountsOnePixelToEachLogicalUnit) {
  const std::array outputs {
    described(0, 0, 2560, 1440, 2560, 1440),
    described(2560, 0, 1080, 1920, 1920, 1080, transform::turned_90),
  };

  const auto desktop = measure_desktop(outputs);
  EXPECT_EQ(desktop.scale, 1);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 3640, 1920}));
  EXPECT_EQ(input_rect(outputs[0], desktop, 2560, 1440), (rect_t {0, 0, 2560, 1440}));
}

// Streaming the reporter's turned monitor: the capture hands back its 1920x1080 frame sideways,
// so input keeps that frame's shape at the monitor's place rather than fitting its 1080x1920
// rectangle into the picture as a band. The same holds for each quarter turn, flipped or not.
TEST(OutputLayout, ATurnedStreamedMonitorKeepsItsFramesShapeForInput) {
  for (const auto turned : {transform::turned_90, transform::turned_270, transform::flipped_90, transform::flipped_270}) {
    const std::array outputs {
      described(0, 0, 2560, 1440, 2560, 1440),
      described(2560, 0, 1080, 1920, 1920, 1080, turned),
    };

    const auto desktop = measure_desktop(outputs);
    EXPECT_EQ(desktop.rect, (rect_t {0, 0, 3640, 1920})) << "transform " << turned;
    EXPECT_EQ(input_rect(outputs[1], desktop, 1920, 1080), (rect_t {2560, 0, 1920, 1080})) << "transform " << turned;
  }
}

// Turned and scaled together, the frame's shape is kept in desktop pixels: the 3840x2160 monitor
// at scale 2 covers 1080x1920 logical units, 2160x3840 desktop pixels, and input covers 3840x2160.
TEST(OutputLayout, ATurnedScaledStreamedMonitorKeepsItsFramesShapeInDesktopPixels) {
  const std::array outputs {
    described(0, 0, 2560, 1440, 2560, 1440),
    described(2560, 0, 1080, 1920, 3840, 2160, transform::turned_90, 2),
  };

  const auto desktop = measure_desktop(outputs);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 7280, 3840}));
  EXPECT_EQ(input_rect(outputs[1], desktop, 3840, 2160), (rect_t {5120, 0, 3840, 2160}));
}

// A frame the capture has already turned to the monitor's shape keeps that shape, as KMS capture
// does for a panel turned by plane rotation. A frame that has not, from a portrait panel shown as
// landscape, keeps the portrait frame's shape.
TEST(OutputLayout, AFrameInTheMonitorsShapeKeepsIt) {
  const std::array outputs {described(0, 0, 1280, 800, 800, 1280, transform::turned_270)};
  const auto desktop = measure_desktop(outputs);

  EXPECT_EQ(input_rect(outputs[0], desktop, 1280, 800), (rect_t {0, 0, 1280, 800}));
  EXPECT_EQ(input_rect(outputs[0], desktop, 800, 1280), (rect_t {0, 0, 800, 1280}));
}

// Half a turn and the flips keep an output's shape, so input takes its rectangle as it is.
TEST(OutputLayout, OnlyAQuarterTurnKeepsTheFramesShape) {
  for (const auto kept : {transform::normal, transform::turned_180, transform::flipped, transform::flipped_180}) {
    const std::array outputs {described(0, 0, 1080, 1920, 1080, 1920, kept)};
    const auto desktop = measure_desktop(outputs);
    // A frame of another shape than the output's own mode never comes back for these, and even
    // one that did would not turn the rectangle.
    EXPECT_EQ(input_rect(outputs[0], desktop, 1920, 1080), (rect_t {0, 0, 1080, 1920})) << "transform " << kept;
  }
}

// KMS capture on the reporter's desktop: Wayland matched an output to both CRTCs, so the desktop
// is measured the way Wayland lays it out, and the main monitor sits at its corner.
TEST(OutputLayout, KmsDesktopIsWaylandsWhenEveryCrtcHasAnOutput) {
  const std::array crtcs {
    crtc_output_t {{0, 0, 2560, 1440}, described(0, 0, 2560, 1440, 2560, 1440)},
    crtc_output_t {{2560, 0, 1920, 1080}, described(2560, 0, 1080, 1920, 1920, 1080, transform::turned_90)},
  };

  const auto desktop = measure_crtc_desktop(crtcs);
  EXPECT_TRUE(desktop.by_wayland);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 3640, 1920}));
  EXPECT_EQ(crtc_input_rect(crtcs[0], desktop, 2560, 1440), (rect_t {0, 0, 2560, 1440}));
  // The turned monitor streams sideways, so input keeps its frame's shape at its place.
  EXPECT_EQ(crtc_input_rect(crtcs[1], desktop, 1920, 1080), (rect_t {2560, 0, 1920, 1080}));
}

// One CRTC without a Wayland output keeps the whole desktop in CRTC rectangles, as KMS capture has
// always measured it. A scale 2 monitor's logical rectangle beside another CRTC's mode made a
// desktop of two units, 2560x1440, and the pointer reached three quarters of the monitor each way.
TEST(OutputLayout, OneCrtcWithoutAnOutputKeepsTheDesktopInCrtcRectangles) {
  const std::array crtcs {
    crtc_output_t {{0, 0, 3840, 2160}, described(0, 0, 1920, 1080, 3840, 2160, transform::normal, 2)},
    crtc_output_t {{0, 0, 2560, 1440}, std::nullopt},
  };

  const auto desktop = measure_crtc_desktop(crtcs);
  EXPECT_FALSE(desktop.by_wayland);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 3840, 2160}));
  // No size of its own: input maps onto the 3840x2160 frame across a 3840x2160 desktop.
  EXPECT_EQ(crtc_input_rect(crtcs[0], desktop, 3840, 2160), (rect_t {0, 0, 0, 0}));
}

// A CRTC a plane scans out of that no connector names has no Wayland output either, and no place
// on the desktop.
TEST(OutputLayout, AnUnnamedCrtcKeepsCrtcRectanglesAndTakesNoRoom) {
  const std::array crtcs {
    crtc_output_t {{0, 0, 3840, 2160}, described(0, 0, 1920, 1080, 3840, 2160, transform::normal, 2)},
    crtc_output_t {},
  };

  const auto desktop = measure_crtc_desktop(crtcs);
  EXPECT_FALSE(desktop.by_wayland);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 3840, 2160}));
}

// Without Wayland, on X11, every CRTC keeps its own place and mode, as it always did.
TEST(OutputLayout, KmsDesktopWithoutWaylandIsTheCrtcs) {
  const std::array crtcs {
    crtc_output_t {{0, 0, 2560, 1440}, std::nullopt},
    crtc_output_t {{2560, 0, 1920, 1080}, std::nullopt},
  };

  const auto desktop = measure_crtc_desktop(crtcs);
  EXPECT_FALSE(desktop.by_wayland);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 4480, 1440}));
  EXPECT_EQ(crtc_input_rect(crtcs[1], desktop, 1920, 1080), (rect_t {2560, 0, 0, 0}));
  EXPECT_FALSE(measure_crtc_desktop(std::span<const crtc_output_t> {}).by_wayland);
}

// KMS capture counts a Wayland desktop in desktop pixels too: a lone scale 2 monitor measures what
// its CRTC does, and beside a plain one both count two pixels to each logical unit.
TEST(OutputLayout, KmsDesktopCountsInDesktopPixels) {
  const crtc_output_t hidpi {{0, 0, 3840, 2160}, described(0, 0, 1920, 1080, 3840, 2160, transform::normal, 2)};
  const crtc_output_t plain {{1920, 0, 2560, 1440}, described(1920, 0, 2560, 1440, 2560, 1440)};

  const std::array alone {hidpi};
  const auto lone_desktop = measure_crtc_desktop(alone);
  EXPECT_TRUE(lone_desktop.by_wayland);
  EXPECT_EQ(lone_desktop.rect, (rect_t {0, 0, 3840, 2160}));
  EXPECT_EQ(crtc_input_rect(hidpi, lone_desktop, 3840, 2160), (rect_t {0, 0, 3840, 2160}));

  const std::array both {hidpi, plain};
  const auto desktop = measure_crtc_desktop(both);
  EXPECT_EQ(desktop.scale, 2);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 8960, 2880}));
  EXPECT_EQ(crtc_input_rect(hidpi, desktop, 3840, 2160), (rect_t {0, 0, 3840, 2160}));
  EXPECT_EQ(crtc_input_rect(plain, desktop, 2560, 1440), (rect_t {3840, 0, 5120, 2880}));
}

// A CRTC capture could not tie to a monitor has only its own place, and counts from the desktop's
// corner like every other CRTC. Here a monitor left of the origin puts the corner at -1920.
TEST(OutputLayout, AnUntiedCrtcCountsFromTheDesktopsCorner) {
  const std::array crtcs {
    crtc_output_t {{-1920, 0, 1920, 1080}, described(-1920, 0, 1920, 1080, 1920, 1080)},
    crtc_output_t {{0, 0, 2560, 1440}, std::nullopt},
  };

  const auto desktop = measure_crtc_desktop(crtcs);
  EXPECT_EQ(desktop.rect, (rect_t {-1920, 0, 4480, 1440}));

  const crtc_output_t untied {{0, 0, 2560, 1440}, std::nullopt};
  EXPECT_EQ(crtc_input_rect(untied, desktop, 2560, 1440), (rect_t {1920, 0, 0, 0}));
}
