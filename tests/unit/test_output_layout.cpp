/**
 * @file tests/unit/test_output_layout.cpp
 * @brief Test where each output sits on the desktop, in the desktop's own units.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/platform/linux/output_layout.h"

namespace {
  using output_layout::bounds;
  using output_layout::crtc_input_placement;
  using output_layout::crtc_output_t;
  using output_layout::desktop_scale;
  using output_layout::input_placement_t;
  using output_layout::input_rect;
  using output_layout::keeps_mode_placement;
  using output_layout::logical_rect;
  using output_layout::measure_crtc_desktop;
  using output_layout::measure_desktop;
  using output_layout::mode_extents;
  using output_layout::on_desktop;
  using output_layout::output_t;
  using output_layout::place_input;
  using output_layout::rect_t;
  namespace transform = output_layout::transform;

  constexpr std::array quarter_turns {transform::turned_90, transform::turned_270, transform::flipped_90, transform::flipped_270};

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

  /**
   * @brief What local/staging 4f2ee4dc handed absolute input, copied from it and not computed by
   *        the code under test.
   *
   * wlgrab's wl_display_names() and kmsgrab's kms_display_names() each measured the desktop from
   * zero to the furthest right and bottom edge of any monitor's viewport, its mode at its position
   * on the desktop. init() gave the streamed monitor's display that viewport's position, those
   * extents and no input size, so input mapped onto the frame, and the Linux abs_mouse() left the
   * position out of every point.
   */
  struct staging_input_t {
    int offset_x = 0;
    int offset_y = 0;
    int env_width = 0;
    int env_height = 0;
  };

  staging_input_t staging_input(std::span<const rect_t> viewports, std::size_t streamed) {
    staging_input_t input;
    for (const auto &viewport : viewports) {
      input.env_width = std::max(input.env_width, viewport.x + viewport.width);
      input.env_height = std::max(input.env_height, viewport.y + viewport.height);
    }
    input.offset_x = viewports[streamed].x;
    input.offset_y = viewports[streamed].y;
    return input;
  }

  /**
   * @brief Each output's viewport as wlgrab has it: its position and its mode.
   */
  std::vector<rect_t> viewports(std::span<const output_t> outputs) {
    std::vector<rect_t> result;
    for (const auto &output : outputs) {
      result.push_back({output.x, output.y, output.mode_width, output.mode_height});
    }
    return result;
  }

  staging_input_t staging_of(std::span<const output_t> outputs, std::size_t streamed) {
    return staging_input(viewports(outputs), streamed);
  }

  /**
   * @brief Where wlroots capture places input for outputs[streamed], wired as wl::capture_geometry()
   *        wires it.
   */
  input_placement_t placed(std::span<const output_t> outputs, std::size_t streamed) {
    const auto modes = viewports(outputs);
    return place_input(outputs[streamed], modes[streamed], measure_desktop(outputs), mode_extents(modes));
  }

  /**
   * @brief Where KMS capture places input for active[streamed], wired as kmsgrab.cpp wires it: the
   *        desktop from the active CRTCs, and 1.4.13's extents from every enumerated connector.
   */
  input_placement_t kms_placed(std::span<const crtc_output_t> active, std::span<const rect_t> enumerated, std::size_t streamed) {
    auto desktop = measure_crtc_desktop(active);
    desktop.mode_extents = mode_extents(enumerated);
    return crtc_input_placement(active[streamed], desktop);
  }

  /**
   * @brief Expect a placement to give input exactly local/staging's numbers: its offset, no input
   *        size, its extents, and each point counted from the desktop's corner.
   */
  void expect_stagings(const input_placement_t &placement, const staging_input_t &staging, const std::string &layout) {
    EXPECT_EQ(placement.screen.x, staging.offset_x) << layout;
    EXPECT_EQ(placement.screen.y, staging.offset_y) << layout;
    EXPECT_EQ(placement.screen.width, 0) << layout;
    EXPECT_EQ(placement.screen.height, 0) << layout;
    EXPECT_EQ(placement.extents.width, staging.env_width) << layout;
    EXPECT_EQ(placement.extents.height, staging.env_height) << layout;
    EXPECT_FALSE(placement.counts_from_screen) << layout << ": local/staging left the offset out of every point";
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
  EXPECT_EQ(input_rect(outputs[0], desktop), (rect_t {0, 0, 3840, 2160}));
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
  EXPECT_EQ(input_rect(outputs[0], desktop), (rect_t {0, 0, 3840, 2160}));
  EXPECT_EQ(input_rect(outputs[1], desktop), (rect_t {3840, 0, 5120, 2880}));
}

// The reporter's desktop has no scaled monitor, so desktop pixels are logical units there, and the
// main monitor is placed by the layout: its own rectangle, counted from its own corner.
TEST(OutputLayout, ReportersDesktopCountsOnePixelToEachLogicalUnit) {
  const std::array outputs {
    described(0, 0, 2560, 1440, 2560, 1440),
    described(2560, 0, 1080, 1920, 1920, 1080, transform::turned_90),
  };

  const auto desktop = measure_desktop(outputs);
  EXPECT_EQ(desktop.scale, 1);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 3640, 1920}));
  EXPECT_EQ(input_rect(outputs[0], desktop), (rect_t {0, 0, 2560, 1440}));

  const auto main = placed(outputs, 0);
  EXPECT_EQ(main.screen, (rect_t {0, 0, 2560, 1440}));
  EXPECT_EQ(main.extents, (rect_t {0, 0, 3640, 1920}));
  EXPECT_TRUE(main.counts_from_screen);
}

// local/staging measured from zero to the furthest edge of any mode, so nothing left of or above
// the origin counts, and a connector with no area still reaches as far as its corner.
TEST(OutputLayout, ModeExtentsCountFromZeroToTheFurthestEdge) {
  const std::array left_of_the_origin {rect_t {-1920, 0, 1920, 1080}, rect_t {0, 0, 2560, 1440}};
  EXPECT_EQ(mode_extents(left_of_the_origin), (rect_t {0, 0, 2560, 1440}));

  const std::array reporters {rect_t {0, 0, 2560, 1440}, rect_t {2560, 0, 1920, 1080}};
  EXPECT_EQ(mode_extents(reporters), (rect_t {0, 0, 4480, 1440}));

  const std::array with_an_empty_connector {rect_t {0, 0, 2560, 1440}, rect_t {5000, 0, 0, 0}};
  EXPECT_EQ(mode_extents(with_an_empty_connector), (rect_t {0, 0, 5000, 1440}));

  EXPECT_EQ(mode_extents(std::span<const rect_t> {}), (rect_t {}));
}

// Four transforms turn an output a quarter, and only a capture of one of those keeps 1.4.13's
// input. Half a turn and the flips keep the output's shape, so the layout places it: here a
// portrait monitor right of a landscape one.
TEST(OutputLayout, OnlyAQuarterTurnKeepsStagingsInput) {
  for (const auto turned : quarter_turns) {
    EXPECT_TRUE(keeps_mode_placement(mode_only(0, 0, 1920, 1080, turned))) << "transform " << turned;
  }
  for (const auto kept : {transform::normal, transform::turned_180, transform::flipped, transform::flipped_180}) {
    EXPECT_FALSE(keeps_mode_placement(mode_only(0, 0, 1920, 1080, kept))) << "transform " << kept;

    const std::array outputs {
      described(0, 0, 1920, 1080, 1920, 1080),
      described(1920, 0, 1080, 1920, 1080, 1920, kept),
    };
    const auto portrait = placed(outputs, 1);
    EXPECT_EQ(portrait.screen, (rect_t {1920, 0, 1080, 1920})) << "transform " << kept;
    EXPECT_EQ(portrait.extents, (rect_t {0, 0, 3000, 1920})) << "transform " << kept;
    EXPECT_TRUE(portrait.counts_from_screen) << "transform " << kept;
  }
}

// polaris#793's other half: streaming a monitor turned a quarter. wlroots capture hands its picture
// back sideways, and so does KMS capture unless the panel's plane turns it, and no rectangle on a
// desktop measured by layout fits a sideways picture: the monitor's portrait rectangle left part of
// the picture out of the pointer's reach, and one in the picture's shape ran past the desktop's
// edge. So that stream keeps every number local/staging gave it: its
// mode's corner, left out of each point, no input size, and the extents of every mode. That holds
// alone, beside the reporter's main monitor, at the desktop's right edge, at its bottom and left of
// the origin, for each quarter turn, with logical sizes or without.
TEST(OutputLayout, ATurnedStreamedMonitorKeepsStagingsInput) {
  for (const auto turned : quarter_turns) {
    for (const bool sent : {true, false}) {
      const auto at = [&](std::string_view layout) {
        return std::string {layout} + ", transform " + std::to_string(turned) + ", logical sizes sent: " + (sent ? "yes" : "no");
      };
      const auto output = [&](int x, int y, int logical_width, int logical_height, int mode_width, int mode_height, std::int32_t output_transform) {
        return sent ? described(x, y, logical_width, logical_height, mode_width, mode_height, output_transform) : mode_only(x, y, mode_width, mode_height, output_transform);
      };

      const std::array lone {output(0, 0, 1080, 1920, 1920, 1080, turned)};
      expect_stagings(placed(lone, 0), staging_of(lone, 0), at("alone"));

      const std::array reporters {
        output(0, 0, 2560, 1440, 2560, 1440, transform::normal),
        output(2560, 0, 1080, 1920, 1920, 1080, turned),
      };
      expect_stagings(placed(reporters, 1), staging_of(reporters, 1), at("the reporter's"));
      EXPECT_EQ(placed(reporters, 1).screen, (rect_t {2560, 0, 0, 0})) << at("the reporter's");
      EXPECT_EQ(placed(reporters, 1).extents, (rect_t {0, 0, 4480, 1440})) << at("the reporter's");

      const std::array right_edge {
        output(0, 0, 1920, 1080, 1920, 1080, transform::normal),
        output(1920, 0, 1440, 2560, 2560, 1440, turned),
      };
      expect_stagings(placed(right_edge, 1), staging_of(right_edge, 1), at("a tall one at the right edge"));

      const std::array bottom {
        output(0, 0, 2560, 1440, 2560, 1440, transform::normal),
        output(0, 1440, 1080, 1920, 1920, 1080, turned),
      };
      expect_stagings(placed(bottom, 1), staging_of(bottom, 1), at("at the bottom"));
      EXPECT_EQ(placed(bottom, 1).extents, (rect_t {0, 0, 2560, 2520})) << at("at the bottom");

      const std::array left {
        output(-1080, 0, 1080, 1920, 1920, 1080, turned),
        output(0, 0, 2560, 1440, 2560, 1440, transform::normal),
      };
      expect_stagings(placed(left, 0), staging_of(left, 0), at("left of the origin"));
      EXPECT_EQ(placed(left, 0).screen, (rect_t {-1080, 0, 0, 0})) << at("left of the origin");
    }
  }
}

// Turned and scaled together, the stream keeps local/staging's numbers too: the 3840x2160 mode at
// 2560,0 on extents of 6400x2160. The main monitor beside it is placed by the layout, in desktop
// pixels at scale 2.
TEST(OutputLayout, ATurnedScaledStreamedMonitorKeepsStagingsInput) {
  const std::array outputs {
    described(0, 0, 2560, 1440, 2560, 1440),
    described(2560, 0, 1080, 1920, 3840, 2160, transform::turned_90, 2),
  };

  const auto turned = placed(outputs, 1);
  expect_stagings(turned, staging_of(outputs, 1), "turned and scaled");
  EXPECT_EQ(turned.extents, (rect_t {0, 0, 6400, 2160}));

  const auto main = placed(outputs, 0);
  EXPECT_EQ(main.screen, (rect_t {0, 0, 5120, 2880}));
  EXPECT_EQ(main.extents, (rect_t {0, 0, 7280, 3840}));
  EXPECT_TRUE(main.counts_from_screen);
}

// A portrait panel the compositor turns to landscape, as a Steam Deck's is under KDE. KMS capture
// swaps its frame to 1280x800 for a plane rotated 270; local/staging mapped that frame onto extents
// of the 800x1280 mode, and so does this.
TEST(OutputLayout, APanelTurnedByPlaneRotationKeepsStagingsInput) {
  const std::array crtcs {crtc_output_t {{0, 0, 800, 1280}, described(0, 0, 1280, 800, 800, 1280, transform::turned_270)}};
  const std::array enumerated {rect_t {0, 0, 800, 1280}};

  const auto panel = kms_placed(crtcs, enumerated, 0);
  expect_stagings(panel, staging_input(enumerated, 0), "a turned panel");
  EXPECT_EQ(panel.extents, (rect_t {0, 0, 800, 1280}));
}

// KMS capture on the reporter's desktop: Wayland matched an output to both CRTCs, so the desktop
// is measured the way Wayland lays it out, and the main monitor sits at its corner. The turned
// monitor streams sideways and keeps local/staging's numbers.
TEST(OutputLayout, KmsDesktopIsWaylandsWhenEveryCrtcHasAnOutput) {
  const std::array crtcs {
    crtc_output_t {{0, 0, 2560, 1440}, described(0, 0, 2560, 1440, 2560, 1440)},
    crtc_output_t {{2560, 0, 1920, 1080}, described(2560, 0, 1080, 1920, 1920, 1080, transform::turned_90)},
  };
  const std::array enumerated {rect_t {0, 0, 2560, 1440}, rect_t {2560, 0, 1920, 1080}};

  const auto desktop = measure_crtc_desktop(crtcs);
  EXPECT_TRUE(desktop.by_wayland);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 3640, 1920}));

  const auto main = kms_placed(crtcs, enumerated, 0);
  EXPECT_EQ(main.screen, (rect_t {0, 0, 2560, 1440}));
  EXPECT_EQ(main.extents, (rect_t {0, 0, 3640, 1920}));
  EXPECT_TRUE(main.counts_from_screen);

  const auto turned = kms_placed(crtcs, enumerated, 1);
  expect_stagings(turned, staging_input(enumerated, 1), "KMS, the reporter's turned monitor");
  EXPECT_EQ(turned.extents, (rect_t {0, 0, 4480, 1440}));
}

// KMS capture keeps local/staging's numbers for a turned CRTC whichever way it measures the
// desktop: alone, at the right edge or the bottom, beside a CRTC Wayland named no output for, or
// one no connector names, and with a connector Wayland placed at 5000,0 that no plane scans out
// to, which local/staging's extents still reached.
TEST(OutputLayout, KmsTurnedCrtcKeepsStagingsInputOnEitherDesktop) {
  for (const auto turned : quarter_turns) {
    const auto at = [&](std::string_view layout) {
      return std::string {layout} + ", transform " + std::to_string(turned);
    };

    const std::array lone {crtc_output_t {{0, 0, 1920, 1080}, described(0, 0, 1080, 1920, 1920, 1080, turned)}};
    const std::array lone_enumerated {rect_t {0, 0, 1920, 1080}};
    expect_stagings(kms_placed(lone, lone_enumerated, 0), staging_input(lone_enumerated, 0), at("alone"));
    EXPECT_EQ(kms_placed(lone, lone_enumerated, 0).extents, (rect_t {0, 0, 1920, 1080})) << at("alone");

    const std::array right_edge {
      crtc_output_t {{0, 0, 1920, 1080}, described(0, 0, 1920, 1080, 1920, 1080)},
      crtc_output_t {{1920, 0, 2560, 1440}, described(1920, 0, 1440, 2560, 2560, 1440, turned)},
    };
    const std::array right_edge_enumerated {rect_t {0, 0, 1920, 1080}, rect_t {1920, 0, 2560, 1440}};
    expect_stagings(kms_placed(right_edge, right_edge_enumerated, 1), staging_input(right_edge_enumerated, 1), at("at the right edge"));

    const std::array bottom {
      crtc_output_t {{0, 0, 2560, 1440}, described(0, 0, 2560, 1440, 2560, 1440)},
      crtc_output_t {{0, 1440, 1920, 1080}, described(0, 1440, 1080, 1920, 1920, 1080, turned)},
    };
    const std::array bottom_enumerated {rect_t {0, 0, 2560, 1440}, rect_t {0, 1440, 1920, 1080}};
    expect_stagings(kms_placed(bottom, bottom_enumerated, 1), staging_input(bottom_enumerated, 1), at("at the bottom"));

    const std::array unmatched {
      crtc_output_t {{0, 0, 2560, 1440}, std::nullopt},
      crtc_output_t {{2560, 0, 1920, 1080}, described(2560, 0, 1080, 1920, 1920, 1080, turned)},
    };
    const std::array unmatched_enumerated {rect_t {0, 0, 2560, 1440}, rect_t {2560, 0, 1920, 1080}, rect_t {5000, 0, 0, 0}};
    EXPECT_FALSE(measure_crtc_desktop(unmatched).by_wayland);
    expect_stagings(kms_placed(unmatched, unmatched_enumerated, 1), staging_input(unmatched_enumerated, 1), at("beside an unmatched CRTC"));
    EXPECT_EQ(kms_placed(unmatched, unmatched_enumerated, 1).extents, (rect_t {0, 0, 5000, 1440})) << at("beside an unmatched CRTC");

    const std::array unnamed {
      crtc_output_t {{2560, 0, 1920, 1080}, described(2560, 0, 1080, 1920, 1920, 1080, turned)},
      crtc_output_t {},
    };
    const std::array unnamed_enumerated {rect_t {2560, 0, 1920, 1080}};
    expect_stagings(kms_placed(unnamed, unnamed_enumerated, 0), staging_input(unnamed_enumerated, 0), at("beside an unnamed CRTC"));
  }
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
  const auto placement = crtc_input_placement(crtcs[0], desktop);
  EXPECT_EQ(placement.screen, (rect_t {0, 0, 0, 0}));
  EXPECT_EQ(placement.extents, (rect_t {0, 0, 3840, 2160}));
  EXPECT_TRUE(placement.counts_from_screen);
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
  const auto placement = crtc_input_placement(crtcs[1], desktop);
  EXPECT_EQ(placement.screen, (rect_t {2560, 0, 0, 0}));
  EXPECT_EQ(placement.extents, (rect_t {0, 0, 4480, 1440}));
  EXPECT_TRUE(placement.counts_from_screen);
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
  EXPECT_EQ(crtc_input_placement(hidpi, lone_desktop).screen, (rect_t {0, 0, 3840, 2160}));

  const std::array both {hidpi, plain};
  const auto desktop = measure_crtc_desktop(both);
  EXPECT_EQ(desktop.scale, 2);
  EXPECT_EQ(desktop.rect, (rect_t {0, 0, 8960, 2880}));
  EXPECT_EQ(crtc_input_placement(hidpi, desktop).screen, (rect_t {0, 0, 3840, 2160}));
  EXPECT_EQ(crtc_input_placement(plain, desktop).screen, (rect_t {3840, 0, 5120, 2880}));
  EXPECT_EQ(crtc_input_placement(plain, desktop).extents, (rect_t {0, 0, 8960, 2880}));
}

// A CRTC that appeared after KMS capture measured a desktop Wayland named, so that init could not
// tie it to a monitor, has only its own position and mode, in output pixels, where the desktop
// counts desktop pixels, here two to each logical unit. Placed on that desktop, its offset and the
// extents came out in different units. It keeps local/staging's numbers instead: its own corner,
// left out of each point, input mapped onto its frame, and the extents of every enumerated
// connector.
TEST(OutputLayout, AnUntiedCrtcOnAWaylandDesktopKeepsStagingsNumbers) {
  const std::array crtcs {
    crtc_output_t {{0, 0, 3840, 2160}, described(0, 0, 1920, 1080, 3840, 2160, transform::normal, 2)},
    crtc_output_t {{1920, 0, 2560, 1440}, described(1920, 0, 2560, 1440, 2560, 1440)},
  };
  const std::array enumerated {rect_t {0, 0, 3840, 2160}, rect_t {1920, 0, 2560, 1440}};
  auto desktop = measure_crtc_desktop(crtcs);
  desktop.mode_extents = mode_extents(enumerated);
  ASSERT_TRUE(desktop.by_wayland);
  ASSERT_EQ(desktop.rect, (rect_t {0, 0, 8960, 2880}));

  // local/staging's branch for a CRTC it could not find took the CRTC's own position, and the
  // extents its enumeration measured.
  const crtc_output_t hotplugged {{4480, 0, 1920, 1080}, std::nullopt};
  auto staging = staging_input(enumerated, 0);
  staging.offset_x = 4480;
  staging.offset_y = 0;
  expect_stagings(crtc_input_placement(hotplugged, desktop), staging, "a CRTC found after the desktop was measured");
  EXPECT_EQ(crtc_input_placement(hotplugged, desktop).extents, (rect_t {0, 0, 4480, 2160}));
}

// A CRTC capture could not tie to a monitor has only its own place, and on a desktop of CRTC
// rectangles counts from the desktop's corner like every other CRTC. Here a monitor left of the
// origin puts the corner at -1920.
TEST(OutputLayout, AnUntiedCrtcCountsFromTheDesktopsCorner) {
  const std::array crtcs {
    crtc_output_t {{-1920, 0, 1920, 1080}, described(-1920, 0, 1920, 1080, 1920, 1080)},
    crtc_output_t {{0, 0, 2560, 1440}, std::nullopt},
  };

  const auto desktop = measure_crtc_desktop(crtcs);
  EXPECT_EQ(desktop.rect, (rect_t {-1920, 0, 4480, 1440}));

  const crtc_output_t untied {{0, 0, 2560, 1440}, std::nullopt};
  const auto placement = crtc_input_placement(untied, desktop);
  EXPECT_EQ(placement.screen, (rect_t {1920, 0, 0, 0}));
  EXPECT_TRUE(placement.counts_from_screen);
}
