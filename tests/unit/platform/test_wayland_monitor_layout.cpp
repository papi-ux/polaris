/**
 * @file tests/unit/platform/test_wayland_monitor_layout.cpp
 * @brief Test the desktop a Wayland capture measures, and where absolute input lands on it.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_WAYLAND)
  #include <cstdint>
  #include <memory>
  #include <optional>
  #include <utility>
  #include <vector>

  #include <src/input.h>
  #include <src/platform/linux/wayland.h>

namespace {
  using output_layout::rect_t;
  namespace transform = output_layout::transform;

  /**
   * @brief A monitor fed what a compositor sends when an output is bound: wl_output's geometry,
   *        current mode and scale, then xdg-output's position and, when it sends one, logical size.
   */
  std::unique_ptr<wl::monitor_t> monitor(int x, int y, int mode_width, int mode_height, std::int32_t output_transform, std::int32_t scale, std::optional<std::pair<int, int>> logical_size) {
    auto result = std::make_unique<wl::monitor_t>(nullptr);
    result->wl_geometry(nullptr, 0, 0, 0, 0, 0, "", "", output_transform);
    result->wl_mode(nullptr, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, mode_width, mode_height, 60000);
    result->wl_scale(nullptr, scale);
    result->xdg_position(nullptr, x, y);
    if (logical_size) {
      result->xdg_size(nullptr, logical_size->first, logical_size->second);
    }
    return result;
  }

  /**
   * @brief polaris#793: a 2560x1440 monitor at the origin, and a 1920x1080 monitor turned a
   *        quarter to its right.
   */
  std::vector<std::unique_ptr<wl::monitor_t>> reporters_desktop(bool with_logical_sizes) {
    std::vector<std::unique_ptr<wl::monitor_t>> monitors;
    monitors.emplace_back(monitor(0, 0, 2560, 1440, transform::normal, 1, with_logical_sizes ? std::optional {std::pair {2560, 1440}} : std::nullopt));
    monitors.emplace_back(monitor(2560, 0, 1920, 1080, transform::turned_90, 1, with_logical_sizes ? std::optional {std::pair {1080, 1920}} : std::nullopt));
    return monitors;
  }

  /**
   * @brief A display that holds nothing but the geometry a capture gives it.
   */
  class geometry_display_t final: public platf::display_t {
  public:
    explicit geometry_display_t(const wl::capture_geometry_t &geometry) {
      geometry.apply_to(*this);
    }

    platf::capture_e capture(const push_captured_image_cb_t &, const pull_free_image_cb_t &, bool *) override {
      return platf::capture_e::error;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      return {};
    }

    int dummy_img(platf::img_t *) override {
      return -1;
    }
  };

  /**
   * @brief Where a client's point lands as a fraction of the desktop, which is how an absolute
   *        pointer device places it: the path passthrough() and abs_mouse() take.
   */
  std::optional<std::pair<float, float>> desktop_fraction(const platf::display_t &display, int stream_width, int stream_height, float client_x, float client_y) {
    const auto port = input::make_touch_port(display.screen_on_desktop(), display.env_width, display.env_height, stream_width, stream_height);
    const auto on_screen = input::map_client_to_touchport(port, {client_x, client_y}, {static_cast<float>(stream_width), static_cast<float>(stream_height)});
    if (!on_screen) {
      return std::nullopt;
    }
    const platf::touch_port_t abs_port {port.offset_x, port.offset_y, port.env_width, port.env_height};
    const auto [x, y] = platf::point_on_desktop(abs_port, on_screen->first, on_screen->second);
    return std::pair {x / abs_port.width, y / abs_port.height};
  }
}  // namespace

// The reporter streams the main monitor. The turned monitor beside it is 1080 wide on the desktop,
// so the desktop is 3640 wide; measuring it by its 1920 wide mode made it 4480.
TEST(WaylandMonitorLayout, ReportersDesktopIsMeasuredInLogicalUnits) {
  const auto monitors = reporters_desktop(true);
  const auto geometry = wl::capture_geometry(monitors, 0);

  EXPECT_EQ(geometry.desktop, (rect_t {0, 0, 3640, 1920}));
  EXPECT_EQ(geometry.screen, (rect_t {0, 0, 2560, 1440}));
  EXPECT_EQ(geometry.frame_width, 2560);
  EXPECT_EQ(geometry.frame_height, 1440);
}

// Streaming the turned monitor itself: the capture still hands back its 1920x1080 mode, and input
// gets the 1080x1920 rectangle it covers on the desktop.
TEST(WaylandMonitorLayout, RotatedStreamedMonitorKeepsItsFrameInOutputPixels) {
  const auto monitors = reporters_desktop(true);
  const auto geometry = wl::capture_geometry(monitors, 1);

  EXPECT_EQ(geometry.frame_width, 1920);
  EXPECT_EQ(geometry.frame_height, 1080);
  EXPECT_EQ(geometry.screen, (rect_t {2560, 0, 1080, 1920}));
  EXPECT_EQ(geometry.desktop, (rect_t {0, 0, 3640, 1920}));

  const geometry_display_t display {geometry};
  EXPECT_EQ(display.width, 1920);
  EXPECT_EQ(display.height, 1080);
  const auto screen = display.screen_on_desktop();
  EXPECT_EQ(screen.offset_x, 2560);
  EXPECT_EQ(screen.offset_y, 0);
  EXPECT_EQ(screen.width, 1080);
  EXPECT_EQ(screen.height, 1920);
}

// A compositor that sends no logical size still says how the output is turned.
TEST(WaylandMonitorLayout, TransformPlacesAnOutputWithoutALogicalSize) {
  const auto monitors = reporters_desktop(false);

  EXPECT_EQ(monitors[1]->logical_rect(), (rect_t {2560, 0, 1080, 1920}));
  EXPECT_EQ(wl::measure_desktop(monitors).rect, (rect_t {0, 0, 3640, 1920}));
  EXPECT_EQ(monitors[1]->viewport.width, 1920);
  EXPECT_EQ(monitors[1]->viewport.height, 1080);
}

// A lone 3840x2160 monitor at scale 2 covers 1920x1080 logical units, and the desktop counts two
// pixels to each, so input gets exactly the numbers it had when the desktop was measured by the
// mode: a 3840x2160 screen on a 3840x2160 desktop, every physical pixel within the pointer's reach.
TEST(WaylandMonitorLayout, ALoneScaleTwoMonitorMeasuresWhatItsModeDoes) {
  for (const bool with_logical_sizes : {true, false}) {
    std::vector<std::unique_ptr<wl::monitor_t>> monitors;
    monitors.emplace_back(monitor(0, 0, 3840, 2160, transform::normal, 2, with_logical_sizes ? std::optional {std::pair {1920, 1080}} : std::nullopt));

    const geometry_display_t display {wl::capture_geometry(monitors, 0)};
    EXPECT_EQ(display.width, 3840) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(display.height, 2160) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(display.env_width, 3840) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(display.env_height, 2160) << "logical sizes sent: " << with_logical_sizes;

    const auto port = input::make_touch_port(display.screen_on_desktop(), display.env_width, display.env_height, 1920, 1080);
    const auto by_mode = input::make_touch_port(platf::touch_port_t {0, 0, 3840, 2160}, 3840, 2160, 1920, 1080);
    EXPECT_EQ(port.offset_x, by_mode.offset_x);
    EXPECT_EQ(port.offset_y, by_mode.offset_y);
    EXPECT_EQ(port.width, by_mode.width);
    EXPECT_EQ(port.height, by_mode.height);
    EXPECT_EQ(port.env_width, by_mode.env_width);
    EXPECT_EQ(port.env_height, by_mode.env_height);
    EXPECT_FLOAT_EQ(port.client_offsetX, by_mode.client_offsetX);
    EXPECT_FLOAT_EQ(port.client_offsetY, by_mode.client_offsetY);
    EXPECT_FLOAT_EQ(port.scalar_inv, by_mode.scalar_inv);
  }
}

// A HiDPI monitor at scale 2 beside a plain one. The desktop counts in the HiDPI monitor's
// pixels, so that monitor is placed at its whole mode, and the plain one covers twice its mode.
// That holds whether xdg-output says how big each is or only wl_output.scale does.
TEST(WaylandMonitorLayout, MixedScaleDesktopCountsInTheSharpestMonitorsPixels) {
  for (const bool with_logical_sizes : {true, false}) {
    std::vector<std::unique_ptr<wl::monitor_t>> monitors;
    monitors.emplace_back(monitor(0, 0, 3840, 2160, transform::normal, 2, with_logical_sizes ? std::optional {std::pair {1920, 1080}} : std::nullopt));
    monitors.emplace_back(monitor(1920, 0, 2560, 1440, transform::normal, 1, with_logical_sizes ? std::optional {std::pair {2560, 1440}} : std::nullopt));

    const auto hidpi = wl::capture_geometry(monitors, 0);
    EXPECT_EQ(hidpi.frame_width, 3840) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(hidpi.frame_height, 2160) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(hidpi.screen, (rect_t {0, 0, 3840, 2160})) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(hidpi.desktop, (rect_t {0, 0, 8960, 2880})) << "logical sizes sent: " << with_logical_sizes;

    const auto plain = wl::capture_geometry(monitors, 1);
    EXPECT_EQ(plain.frame_width, 2560) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(plain.frame_height, 1440) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(plain.screen, (rect_t {3840, 0, 5120, 2880})) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(plain.desktop, (rect_t {0, 0, 8960, 2880})) << "logical sizes sent: " << with_logical_sizes;
  }
}

// On that mixed desktop, the middle of either stream lands in the middle of its monitor. In
// logical units the HiDPI monitor spans 0 to 1920 and the plain one 1920 to 4480.
TEST(WaylandMonitorLayout, MixedScaleClientPointLandsOnEachMonitor) {
  std::vector<std::unique_ptr<wl::monitor_t>> monitors;
  monitors.emplace_back(monitor(0, 0, 3840, 2160, transform::normal, 2, std::pair {1920, 1080}));
  monitors.emplace_back(monitor(1920, 0, 2560, 1440, transform::normal, 1, std::pair {2560, 1440}));

  const geometry_display_t hidpi {wl::capture_geometry(monitors, 0)};
  const auto hidpi_middle = desktop_fraction(hidpi, 1920, 1080, 960.0f, 540.0f);
  ASSERT_TRUE(hidpi_middle.has_value());
  EXPECT_NEAR(hidpi_middle->first * 4480.0f, 960.0f, 1.0f);
  EXPECT_NEAR(hidpi_middle->second * 1440.0f, 540.0f, 1.0f);

  const geometry_display_t plain {wl::capture_geometry(monitors, 1)};
  const auto plain_middle = desktop_fraction(plain, 2560, 1440, 1280.0f, 720.0f);
  ASSERT_TRUE(plain_middle.has_value());
  EXPECT_NEAR(plain_middle->first * 4480.0f, 1920.0f + 1280.0f, 1.0f);
  EXPECT_NEAR(plain_middle->second * 1440.0f, 720.0f, 1.0f);
}

// At a fractional scale of 1.5, wl_output.scale rounds up to 2 and only xdg-output says the
// monitor covers 2560x1440. Its word wins, counted two desktop pixels to each logical unit.
TEST(WaylandMonitorLayout, FractionalScaleTakesXdgOutputsLogicalSize) {
  std::vector<std::unique_ptr<wl::monitor_t>> monitors;
  monitors.emplace_back(monitor(0, 0, 3840, 2160, transform::normal, 2, std::pair {2560, 1440}));

  const auto geometry = wl::capture_geometry(monitors, 0);
  EXPECT_EQ(geometry.frame_width, 3840);
  EXPECT_EQ(geometry.frame_height, 2160);
  EXPECT_EQ(geometry.screen, (rect_t {0, 0, 5120, 2880}));
  EXPECT_EQ(geometry.desktop, (rect_t {0, 0, 5120, 2880}));
}

// Plain monitors measure as they always did.
TEST(WaylandMonitorLayout, PlainMonitorsAreUnchanged) {
  std::vector<std::unique_ptr<wl::monitor_t>> monitors;
  monitors.emplace_back(monitor(0, 0, 2560, 1440, transform::normal, 1, std::pair {2560, 1440}));
  monitors.emplace_back(monitor(2560, 0, 1920, 1080, transform::normal, 1, std::pair {1920, 1080}));

  const auto geometry = wl::capture_geometry(monitors, 1);
  EXPECT_EQ(geometry.frame_width, 1920);
  EXPECT_EQ(geometry.frame_height, 1080);
  EXPECT_EQ(geometry.screen, (rect_t {2560, 0, 1920, 1080}));
  EXPECT_EQ(geometry.desktop, (rect_t {0, 0, 4480, 1440}));
}

// polaris#793 end to end: the reporter streams the main monitor at 1920x1080, and a client point
// lands where it was aimed. Measured by modes, the desktop came out 4480x1440 where it is
// 3640x1920, and the middle of the stream landed at 1040,960 instead of the monitor's middle.
TEST(WaylandMonitorLayout, ReportersClientPointLandsOnTheStreamedMonitor) {
  const auto monitors = reporters_desktop(true);
  const geometry_display_t display {wl::capture_geometry(monitors, 0)};

  const auto middle = desktop_fraction(display, 1920, 1080, 960.0f, 540.0f);
  ASSERT_TRUE(middle.has_value());
  EXPECT_NEAR(middle->first * 3640.0f, 1280.0f, 1.0f);
  EXPECT_NEAR(middle->second * 1920.0f, 720.0f, 1.0f);

  const auto corner = desktop_fraction(display, 1920, 1080, 1920.0f, 1080.0f);
  ASSERT_TRUE(corner.has_value());
  EXPECT_NEAR(corner->first * 3640.0f, 2560.0f, 1.0f);
  EXPECT_NEAR(corner->second * 1920.0f, 1440.0f, 1.0f);
}

// Streaming the turned monitor: its 1080x1920 rectangle is fitted into the stream, and a point
// lands on that monitor, right of the main one, rather than on the main one.
TEST(WaylandMonitorLayout, ClientPointLandsOnARotatedStreamedMonitor) {
  const auto monitors = reporters_desktop(true);
  const geometry_display_t display {wl::capture_geometry(monitors, 1)};

  const auto middle = desktop_fraction(display, 1920, 1080, 960.0f, 540.0f);
  ASSERT_TRUE(middle.has_value());
  EXPECT_NEAR(middle->first * 3640.0f, 2560.0f + 540.0f, 1.0f);
  EXPECT_NEAR(middle->second * 1920.0f, 960.0f, 1.0f);

  // The stream is wider than the monitor is, so the monitor sits between bars. The bottom right of
  // the picture is the monitor's own bottom right corner.
  const auto scale = 1080.0f / 1920.0f;
  const auto picture_right = 960.0f + 1080.0f * scale / 2.0f;
  const auto corner = desktop_fraction(display, 1920, 1080, picture_right, 1080.0f);
  ASSERT_TRUE(corner.has_value());
  EXPECT_NEAR(corner->first * 3640.0f, 3640.0f, 1.0f);
  EXPECT_NEAR(corner->second * 1920.0f, 1920.0f, 1.0f);
}
#endif
