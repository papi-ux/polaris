/**
 * @file tests/unit/platform/test_wayland_monitor_layout.cpp
 * @brief Test the desktop a Wayland capture measures, and where absolute input lands on it.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_WAYLAND)
  #include <algorithm>
  #include <array>
  #include <cmath>
  #include <cstdint>
  #include <memory>
  #include <optional>
  #include <span>
  #include <sstream>
  #include <string>
  #include <string_view>
  #include <utility>
  #include <vector>

  #include <src/input.h>
  #include <src/platform/linux/wayland.h>
  #include <src/video.h>

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
   * @brief The touch port a session streaming this capture at this size gets.
   */
  input::touch_port_t session_port(const platf::display_t &display, int stream_width, int stream_height) {
    video::config_t config {};
    config.width = stream_width;
    config.height = stream_height;
    return video::make_port(&display, config);
  }
}  // namespace

namespace {
  using output_layout::crtc_output_t;
  using output_layout::output_t;

  constexpr std::array quarter_turns {transform::turned_90, transform::turned_270, transform::flipped_90, transform::flipped_270};

  std::optional<std::pair<int, int>> logical_size_if(bool sent, int width, int height) {
    return sent ? std::optional {std::pair {width, height}} : std::nullopt;
  }

  std::string label(std::string_view layout, std::int32_t output_transform, bool logical_sizes_sent) {
    std::ostringstream text;
    text << layout << ", transform " << output_transform << ", logical sizes sent: " << logical_sizes_sent;
    return text.str();
  }

  /**
   * @brief What local/staging 4f2ee4dc handed absolute input for one capture, copied from it and not
   *        computed by the code under test.
   *
   * Its wl_display_names() and kms_display_names() both measured the desktop from zero to the
   * furthest right and bottom edge of any monitor's viewport: the monitor's position on the desktop
   * and its mode, in output pixels. Each capture's init() gave its display that viewport's position
   * and those extents, and no input size, so input mapped onto the frame.
   */
  struct staging_display_t {
    int offset_x = 0;
    int offset_y = 0;
    int width = 0;
    int height = 0;
    int env_width = 0;
    int env_height = 0;
  };

  staging_display_t staging_display(std::span<const rect_t> viewports, std::size_t streamed, int frame_width, int frame_height) {
    staging_display_t display;
    for (const auto &viewport : viewports) {
      display.env_width = std::max(display.env_width, viewport.x + viewport.width);
      display.env_height = std::max(display.env_height, viewport.y + viewport.height);
    }
    display.offset_x = viewports[streamed].x;
    display.offset_y = viewports[streamed].y;
    display.width = frame_width;
    display.height = frame_height;
    return display;
  }

  /**
   * @brief local/staging's wlgrab: every monitor's viewport as the Wayland listeners fill it, which
   *        this branch leaves as it was, and the streamed monitor's viewport for the frame.
   */
  staging_display_t staging_wlgrab(const std::vector<std::unique_ptr<wl::monitor_t>> &monitors, std::size_t streamed) {
    std::vector<rect_t> viewports;
    for (const auto &monitor : monitors) {
      viewports.push_back({monitor->viewport.offset_x, monitor->viewport.offset_y, monitor->viewport.width, monitor->viewport.height});
    }
    return staging_display(viewports, streamed, monitors[streamed]->viewport.width, monitors[streamed]->viewport.height);
  }

  /**
   * @brief local/staging's make_port() for a capture with no screen fitted into its frame.
   */
  input::touch_port_t staging_port(const staging_display_t &display, int stream_width, int stream_height) {
    return input::make_touch_port(
      platf::touch_port_t {display.offset_x, display.offset_y, display.width, display.height},
      display.env_width,
      display.env_height,
      stream_width,
      stream_height
    );
  }

  /**
   * @brief Where the wlr virtual pointer puts an absolute point, as a fraction of the extents. It
   *        rounds the point and clamps it to them (inputtino_wayland_virtual_input.cpp), so a point
   *        past the desktop's edge is pinned to it.
   */
  std::pair<float, float> delivered(float x, float y, int extent_width, int extent_height) {
    const auto px = std::clamp(static_cast<int>(std::lround(x)), 0, extent_width);
    const auto py = std::clamp(static_cast<int>(std::lround(y)), 0, extent_height);
    return {static_cast<float>(px) / static_cast<float>(extent_width), static_cast<float>(py) / static_cast<float>(extent_height)};
  }

  /**
   * @brief Where a client's point on the stream lands, as a fraction of the extents, by the path
   *        passthrough(), abs_mouse() and the wlr virtual pointer take.
   */
  std::optional<std::pair<float, float>> pointer_fraction(const input::touch_port_t &port, int stream_width, int stream_height, float client_x, float client_y) {
    const auto on_screen = input::map_client_to_touchport(port, {client_x, client_y}, {static_cast<float>(stream_width), static_cast<float>(stream_height)});
    if (!on_screen) {
      return std::nullopt;
    }
    const platf::touch_port_t abs_port {port.offset_x, port.offset_y, port.env_width, port.env_height};
    const auto [x, y] = platf::point_on_desktop(abs_port, on_screen->first, on_screen->second);
    return delivered(x, y, abs_port.width, abs_port.height);
  }

  /**
   * @brief The same on local/staging, whose Linux abs_mouse() handed the backends the point on the
   *        captured screen as it was, the screen's place left out.
   */
  std::optional<std::pair<float, float>> staging_pointer_fraction(const input::touch_port_t &port, int stream_width, int stream_height, float client_x, float client_y) {
    const auto on_screen = input::map_client_to_touchport(port, {client_x, client_y}, {static_cast<float>(stream_width), static_cast<float>(stream_height)});
    if (!on_screen) {
      return std::nullopt;
    }
    return delivered(on_screen->first, on_screen->second, port.env_width, port.env_height);
  }

  /**
   * @brief Where a client's point on a session streaming this capture lands, as a fraction of the
   *        extents: make_port(), then the path pointer_fraction() takes.
   */
  std::optional<std::pair<float, float>> desktop_fraction(const platf::display_t &display, int stream_width, int stream_height, float client_x, float client_y) {
    return pointer_fraction(session_port(display, stream_width, stream_height), stream_width, stream_height, client_x, client_y);
  }

  /**
   * @brief A touch or pen point as passthrough() hands it on: the touch port's point over the
   *        extents, and a touch turned back by the compositor's turn. Both read nothing else.
   */
  std::optional<std::pair<float, float>> touch_fraction(const input::touch_port_t &port, float across, float down, bool is_touch) {
    const auto coords = input::map_client_to_touchport(port, {across * 65535.f, down * 65535.f}, {65535.f, 65535.f});
    if (!coords) {
      return std::nullopt;
    }
    const float x = coords->first / static_cast<float>(port.env_width);
    const float y = coords->second / static_cast<float>(port.env_height);
    return is_touch ? input::turn_back_touch(port.compositor_touch_turn, x, y) : std::pair {x, y};
  }

  /// Points across the whole picture, edges included, as fractions of the stream.
  constexpr std::array picture_fractions {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f};

  /// A landscape stream, one of another shape, and a portrait one.
  constexpr std::array<std::pair<int, int>, 3> stream_sizes {{{1920, 1080}, {1280, 800}, {1080, 1920}}};

  std::string describe(const std::optional<std::pair<float, float>> &point) {
    std::ostringstream text;
    if (point) {
      text << point->first << ',' << point->second;
    } else {
      text << "refused";
    }
    return text.str();
  }

  /**
   * @brief A monitor's rectangle in the logical units the compositor lays the desktop out in,
   *        counted from the desktop's corner.
   */
  struct area_t {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
  };

  /**
   * @brief Expect every point of the picture, edges included, to land at the same point of the
   *        monitor, within a logical unit, on a desktop of this many logical units. A point pinned
   *        to the desktop's edge, or one off the monitor, fails it.
   */
  void expect_picture_spans(const platf::display_t &display, int stream_width, int stream_height, std::pair<float, float> desktop, area_t monitor, const std::string &layout) {
    int misses = 0;
    std::string first;
    for (const auto across : picture_fractions) {
      for (const auto down : picture_fractions) {
        const auto point = desktop_fraction(display, stream_width, stream_height, across * static_cast<float>(stream_width), down * static_cast<float>(stream_height));
        const float want_x = monitor.x + across * monitor.width;
        const float want_y = monitor.y + down * monitor.height;
        if (point && std::abs(point->first * desktop.first - want_x) <= 1.0f && std::abs(point->second * desktop.second - want_y) <= 1.0f) {
          continue;
        }
        if (misses++ == 0) {
          std::ostringstream text;
          text << across << ',' << down << " of the picture landed at ";
          if (point) {
            text << point->first * desktop.first << ',' << point->second * desktop.second;
          } else {
            text << "nothing";
          }
          text << ", aimed at " << want_x << ',' << want_y;
          first = text.str();
        }
      }
    }
    EXPECT_EQ(misses, 0) << layout << ": first miss: " << first;
  }

  /**
   * @brief Expect a capture to hand absolute input exactly what local/staging did: the display's
   *        numbers, and across the whole picture at three stream shapes, where the mouse, a touch
   *        and a pen land.
   */
  void expect_stagings_input(const platf::display_t &display, const staging_display_t &staging, const std::string &layout) {
    EXPECT_EQ(display.offset_x, staging.offset_x) << layout;
    EXPECT_EQ(display.offset_y, staging.offset_y) << layout;
    EXPECT_EQ(display.width, staging.width) << layout;
    EXPECT_EQ(display.height, staging.height) << layout;
    EXPECT_EQ(display.input_width, 0) << layout << ": local/staging set no input size, so input mapped onto the frame";
    EXPECT_EQ(display.input_height, 0) << layout;
    EXPECT_EQ(display.env_width, staging.env_width) << layout;
    EXPECT_EQ(display.env_height, staging.env_height) << layout;
    EXPECT_FALSE(display.input_counts_from_screen) << layout << ": local/staging's Linux abs_mouse() left the offset out of every point";

    for (const auto &[stream_width, stream_height] : stream_sizes) {
      const auto port = session_port(display, stream_width, stream_height);
      const auto was = staging_port(staging, stream_width, stream_height);
      const auto at = layout + ", streamed at " + std::to_string(stream_width) + 'x' + std::to_string(stream_height);

      EXPECT_EQ(port.width, was.width) << at;
      EXPECT_EQ(port.height, was.height) << at;
      EXPECT_EQ(port.env_width, was.env_width) << at;
      EXPECT_EQ(port.env_height, was.env_height) << at;
      EXPECT_FLOAT_EQ(port.client_offsetX, was.client_offsetX) << at;
      EXPECT_FLOAT_EQ(port.client_offsetY, was.client_offsetY) << at;
      EXPECT_FLOAT_EQ(port.scalar_inv, was.scalar_inv) << at;
      EXPECT_EQ(port.compositor_touch_turn, was.compositor_touch_turn) << at;

      int differences = 0;
      std::string first;
      const auto compare = [&](std::string_view what, float across, float down, const std::optional<std::pair<float, float>> &got, const std::optional<std::pair<float, float>> &want) {
        if (got.has_value() == want.has_value() && (!got || *got == *want)) {
          return;
        }
        if (differences++ == 0) {
          std::ostringstream text;
          text << what << " at " << across << ',' << down << " of the picture: " << describe(got)
               << " where local/staging gave " << describe(want);
          first = text.str();
        }
      };
      for (const auto across : picture_fractions) {
        for (const auto down : picture_fractions) {
          const auto client_x = across * static_cast<float>(stream_width);
          const auto client_y = down * static_cast<float>(stream_height);
          compare("mouse", across, down, pointer_fraction(port, stream_width, stream_height, client_x, client_y), staging_pointer_fraction(was, stream_width, stream_height, client_x, client_y));
          compare("touch", across, down, touch_fraction(port, across, down, true), touch_fraction(was, across, down, true));
          compare("pen", across, down, touch_fraction(port, across, down, false), touch_fraction(was, across, down, false));
        }
      }
      EXPECT_EQ(differences, 0) << at << ": first difference: " << first;
    }
  }

  void expect_wlgrab_keeps_stagings_input(const std::vector<std::unique_ptr<wl::monitor_t>> &monitors, std::size_t streamed, const std::string &layout) {
    const geometry_display_t display {wl::capture_geometry(monitors, streamed)};
    expect_stagings_input(display, staging_wlgrab(monitors, streamed), layout);
  }

  /**
   * @brief A display that holds only what a capture's init gave it.
   */
  class plain_display_t final: public platf::display_t {
  public:
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

  output_t laid_out(int x, int y, int logical_width, int logical_height, int mode_width, int mode_height, std::int32_t output_transform = transform::normal, std::int32_t scale = 1) {
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
   * @brief A KMS capture's enumeration: the active CRTCs the desktop is measured from, and every
   *        connector's viewport, its CRTC's mode at the position Wayland gave it, with or without a
   *        plane, which is what local/staging measured by.
   */
  struct kms_layout_t {
    std::vector<crtc_output_t> active;
    std::vector<rect_t> enumerated;
  };

  /**
   * @brief A display given what KMS capture's init gives it for one CRTC, wired as kmsgrab.cpp wires
   *        it, which AbsoluteInputSource guards.
   */
  std::unique_ptr<plain_display_t> kms_display(const kms_layout_t &layout, const crtc_output_t &streamed, int frame_width, int frame_height) {
    auto display = std::make_unique<plain_display_t>();
    display->width = frame_width;
    display->height = frame_height;
    auto desktop = output_layout::measure_crtc_desktop(layout.active);
    desktop.mode_extents = output_layout::mode_extents(layout.enumerated);
    const auto placement = output_layout::crtc_input_placement(streamed, desktop);
    display->offset_x = placement.screen.x;
    display->offset_y = placement.screen.y;
    display->input_width = placement.screen.width;
    display->input_height = placement.screen.height;
    display->input_counts_from_screen = placement.counts_from_screen;
    display->env_width = placement.extents.width;
    display->env_height = placement.extents.height;
    return display;
  }

  void expect_kms_keeps_stagings_input(const kms_layout_t &layout, std::size_t streamed, int frame_width, int frame_height, const std::string &name) {
    const auto display = kms_display(layout, layout.active[streamed], frame_width, frame_height);
    expect_stagings_input(*display, staging_display(layout.enumerated, streamed, frame_width, frame_height), name);
  }
}  // namespace

// The reporter streams the main monitor. The turned monitor beside it is 1080 wide on the desktop,
// so the desktop is 3640 wide; measuring it by its 1920 wide mode made it 4480.
TEST(WaylandMonitorLayout, ReportersDesktopIsMeasuredInLogicalUnits) {
  const auto monitors = reporters_desktop(true);
  const auto geometry = wl::capture_geometry(monitors, 0);

  EXPECT_EQ(geometry.input.extents, (rect_t {0, 0, 3640, 1920}));
  EXPECT_EQ(geometry.input.screen, (rect_t {0, 0, 2560, 1440}));
  EXPECT_TRUE(geometry.input.counts_from_screen);
  EXPECT_EQ(geometry.frame_width, 2560);
  EXPECT_EQ(geometry.frame_height, 1440);
}

// Streaming the turned monitor itself: the capture hands back its 1920x1080 mode unturned, so the
// picture is sideways, and input keeps local/staging's numbers: the monitor's 2560,0 left out of
// each point, no size of its own, and the 4480x1440 its mode made of the desktop.
TEST(WaylandMonitorLayout, RotatedStreamedMonitorKeepsStagingsNumbers) {
  for (const bool with_logical_sizes : {true, false}) {
    const auto monitors = reporters_desktop(with_logical_sizes);
    const auto geometry = wl::capture_geometry(monitors, 1);

    EXPECT_EQ(geometry.frame_width, 1920) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(geometry.frame_height, 1080) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(geometry.input.screen, (rect_t {2560, 0, 0, 0})) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(geometry.input.extents, (rect_t {0, 0, 4480, 1440})) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_FALSE(geometry.input.counts_from_screen) << "logical sizes sent: " << with_logical_sizes;

    const geometry_display_t display {geometry};
    EXPECT_EQ(display.offset_x, 2560);
    EXPECT_EQ(display.offset_y, 0);
    EXPECT_EQ(display.width, 1920);
    EXPECT_EQ(display.height, 1080);
    EXPECT_FALSE(display.input_counts_from_screen);
    const auto screen = display.screen_on_desktop();
    EXPECT_EQ(screen.offset_x, 0);
    EXPECT_EQ(screen.offset_y, 0);
    EXPECT_EQ(screen.width, 1920);
    EXPECT_EQ(screen.height, 1080);
  }
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

    const auto port = session_port(display, 1920, 1080);
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
    EXPECT_EQ(hidpi.input.screen, (rect_t {0, 0, 3840, 2160})) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(hidpi.input.extents, (rect_t {0, 0, 8960, 2880})) << "logical sizes sent: " << with_logical_sizes;

    const auto plain = wl::capture_geometry(monitors, 1);
    EXPECT_EQ(plain.frame_width, 2560) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(plain.frame_height, 1440) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(plain.input.screen, (rect_t {3840, 0, 5120, 2880})) << "logical sizes sent: " << with_logical_sizes;
    EXPECT_EQ(plain.input.extents, (rect_t {0, 0, 8960, 2880})) << "logical sizes sent: " << with_logical_sizes;
  }
}

// On that mixed desktop, every point of either stream, edges included, lands at the same point of
// its monitor. In logical units the HiDPI monitor spans 0 to 1920 and the plain one 1920 to 4480.
TEST(WaylandMonitorLayout, MixedScaleClientPointLandsOnEachMonitor) {
  std::vector<std::unique_ptr<wl::monitor_t>> monitors;
  monitors.emplace_back(monitor(0, 0, 3840, 2160, transform::normal, 2, std::pair {1920, 1080}));
  monitors.emplace_back(monitor(1920, 0, 2560, 1440, transform::normal, 1, std::pair {2560, 1440}));

  const geometry_display_t hidpi {wl::capture_geometry(monitors, 0)};
  expect_picture_spans(hidpi, 1920, 1080, {4480.0f, 1440.0f}, {0.0f, 0.0f, 1920.0f, 1080.0f}, "the HiDPI monitor");

  const geometry_display_t plain {wl::capture_geometry(monitors, 1)};
  expect_picture_spans(plain, 2560, 1440, {4480.0f, 1440.0f}, {1920.0f, 0.0f, 2560.0f, 1440.0f}, "the plain monitor");
}

// At a fractional scale of 1.5, wl_output.scale rounds up to 2 and only xdg-output says the
// monitor covers 2560x1440. Its word wins, counted two desktop pixels to each logical unit.
TEST(WaylandMonitorLayout, FractionalScaleTakesXdgOutputsLogicalSize) {
  std::vector<std::unique_ptr<wl::monitor_t>> monitors;
  monitors.emplace_back(monitor(0, 0, 3840, 2160, transform::normal, 2, std::pair {2560, 1440}));

  const auto geometry = wl::capture_geometry(monitors, 0);
  EXPECT_EQ(geometry.frame_width, 3840);
  EXPECT_EQ(geometry.frame_height, 2160);
  EXPECT_EQ(geometry.input.screen, (rect_t {0, 0, 5120, 2880}));
  EXPECT_EQ(geometry.input.extents, (rect_t {0, 0, 5120, 2880}));
}

// Plain monitors measure as they always did.
TEST(WaylandMonitorLayout, PlainMonitorsAreUnchanged) {
  std::vector<std::unique_ptr<wl::monitor_t>> monitors;
  monitors.emplace_back(monitor(0, 0, 2560, 1440, transform::normal, 1, std::pair {2560, 1440}));
  monitors.emplace_back(monitor(2560, 0, 1920, 1080, transform::normal, 1, std::pair {1920, 1080}));

  const auto geometry = wl::capture_geometry(monitors, 1);
  EXPECT_EQ(geometry.frame_width, 1920);
  EXPECT_EQ(geometry.frame_height, 1080);
  EXPECT_EQ(geometry.input.screen, (rect_t {2560, 0, 1920, 1080}));
  EXPECT_EQ(geometry.input.extents, (rect_t {0, 0, 4480, 1440}));
  EXPECT_TRUE(geometry.input.counts_from_screen);
}

// polaris#793 end to end: the reporter streams the main monitor at 1920x1080, and every point of
// the stream, edges included, lands where it was aimed. Measured by modes, the desktop came out
// 4480x1440 where it is 3640x1920, and the middle of the stream landed at 1040,960 instead of the
// monitor's middle.
TEST(WaylandMonitorLayout, ReportersClientPointLandsOnTheStreamedMonitor) {
  const auto monitors = reporters_desktop(true);
  const geometry_display_t display {wl::capture_geometry(monitors, 0)};

  const auto middle = desktop_fraction(display, 1920, 1080, 960.0f, 540.0f);
  ASSERT_TRUE(middle.has_value());
  EXPECT_NEAR(middle->first * 3640.0f, 1280.0f, 1.0f);
  EXPECT_NEAR(middle->second * 1920.0f, 720.0f, 1.0f);

  expect_picture_spans(display, 1920, 1080, {3640.0f, 1920.0f}, {0.0f, 0.0f, 2560.0f, 1440.0f}, "the reporter's main monitor");
}

// A plain monitor on a desktop with a turned one keeps the layout's placement, wherever the turned
// monitor sits: below it or left of the origin.
TEST(WaylandMonitorLayout, APlainMonitorBesideATurnedOneLandsWhereAimed) {
  for (const auto turned : quarter_turns) {
    std::vector<std::unique_ptr<wl::monitor_t>> below;
    below.emplace_back(monitor(0, 0, 2560, 1440, transform::normal, 1, std::pair {2560, 1440}));
    below.emplace_back(monitor(0, 1440, 1920, 1080, turned, 1, std::pair {1080, 1920}));
    const geometry_display_t above {wl::capture_geometry(below, 0)};
    EXPECT_TRUE(above.input_counts_from_screen);
    expect_picture_spans(above, 2560, 1440, {2560.0f, 3360.0f}, {0.0f, 0.0f, 2560.0f, 1440.0f}, label("above a turned monitor", turned, true));

    std::vector<std::unique_ptr<wl::monitor_t>> left;
    left.emplace_back(monitor(-1080, 0, 1920, 1080, turned, 1, std::pair {1080, 1920}));
    left.emplace_back(monitor(0, 0, 2560, 1440, transform::normal, 1, std::pair {2560, 1440}));
    const geometry_display_t right {wl::capture_geometry(left, 1)};
    expect_picture_spans(right, 2560, 1440, {3640.0f, 1920.0f}, {1080.0f, 0.0f, 2560.0f, 1440.0f}, label("right of a turned monitor left of the origin", turned, true));
  }
}

// A monitor turned a quarter streams sideways, since wlroots capture hands its mode back unturned,
// and nothing measured by layout fits that picture. Until capture turns it upright, its stream
// keeps what local/staging gave absolute input. Alone, that is its mode on a desktop of its mode:
// every point of the picture reaches the monitor, on the wrong axes.
TEST(WaylandMonitorLayout, ALoneTurnedMonitorKeepsStagingsInput) {
  for (const auto turned : quarter_turns) {
    for (const bool sent : {true, false}) {
      std::vector<std::unique_ptr<wl::monitor_t>> monitors;
      monitors.emplace_back(monitor(0, 0, 1920, 1080, turned, 1, logical_size_if(sent, 1080, 1920)));

      const auto staging = staging_wlgrab(monitors, 0);
      EXPECT_EQ(staging.env_width, 1920);
      EXPECT_EQ(staging.env_height, 1080);
      expect_wlgrab_keeps_stagings_input(monitors, 0, label("a lone turned monitor", turned, sent));
    }
  }
}

// The reporter's desktop, streaming the turned monitor right of the main one: local/staging
// measured a 4480x1440 desktop, gave the stream the monitor's 2560,0 and left it out of each point.
TEST(WaylandMonitorLayout, ReportersTurnedMonitorKeepsStagingsInput) {
  for (const auto turned : quarter_turns) {
    for (const bool sent : {true, false}) {
      std::vector<std::unique_ptr<wl::monitor_t>> monitors;
      monitors.emplace_back(monitor(0, 0, 2560, 1440, transform::normal, 1, logical_size_if(sent, 2560, 1440)));
      monitors.emplace_back(monitor(2560, 0, 1920, 1080, turned, 1, logical_size_if(sent, 1080, 1920)));

      const auto staging = staging_wlgrab(monitors, 1);
      EXPECT_EQ(staging.offset_x, 2560);
      EXPECT_EQ(staging.env_width, 4480);
      EXPECT_EQ(staging.env_height, 1440);
      expect_wlgrab_keeps_stagings_input(monitors, 1, label("the reporter's turned monitor", turned, sent));

      // Which puts that stream's pointer where local/staging did: its far corner at 1920,1080 of
      // the 4480x1440 extents, which the compositor lays over its 3640x1920 desktop at 1560,1440,
      // on the main monitor. Turning the picture upright is what fixes this.
      const geometry_display_t display {wl::capture_geometry(monitors, 1)};
      const auto corner = desktop_fraction(display, 1920, 1080, 1920.0f, 1080.0f);
      ASSERT_TRUE(corner.has_value());
      EXPECT_NEAR(corner->first * 3640.0f, 1560.0f, 1.0f);
      EXPECT_NEAR(corner->second * 1920.0f, 1440.0f, 1.0f);
    }
  }
}

// A turned monitor at the desktop's right edge, taller than the monitor beside it, and a turned
// HiDPI monitor there at scale 2.
TEST(WaylandMonitorLayout, ATurnedMonitorAtTheRightEdgeKeepsStagingsInput) {
  for (const auto turned : quarter_turns) {
    for (const bool sent : {true, false}) {
      std::vector<std::unique_ptr<wl::monitor_t>> monitors;
      monitors.emplace_back(monitor(0, 0, 1920, 1080, transform::normal, 1, logical_size_if(sent, 1920, 1080)));
      monitors.emplace_back(monitor(1920, 0, 2560, 1440, turned, 1, logical_size_if(sent, 1440, 2560)));
      expect_wlgrab_keeps_stagings_input(monitors, 1, label("a tall turned monitor at the right edge", turned, sent));

      std::vector<std::unique_ptr<wl::monitor_t>> scaled;
      scaled.emplace_back(monitor(0, 0, 2560, 1440, transform::normal, 1, logical_size_if(sent, 2560, 1440)));
      scaled.emplace_back(monitor(2560, 0, 3840, 2160, turned, 2, logical_size_if(sent, 1080, 1920)));
      expect_wlgrab_keeps_stagings_input(scaled, 1, label("a turned scale 2 monitor at the right edge", turned, sent));
    }
  }
}

// A turned monitor below the main one, at the desktop's bottom edge.
TEST(WaylandMonitorLayout, ATurnedMonitorAtTheBottomKeepsStagingsInput) {
  for (const auto turned : quarter_turns) {
    for (const bool sent : {true, false}) {
      std::vector<std::unique_ptr<wl::monitor_t>> monitors;
      monitors.emplace_back(monitor(0, 0, 2560, 1440, transform::normal, 1, logical_size_if(sent, 2560, 1440)));
      monitors.emplace_back(monitor(0, 1440, 1920, 1080, turned, 1, logical_size_if(sent, 1080, 1920)));

      const auto staging = staging_wlgrab(monitors, 1);
      EXPECT_EQ(staging.offset_y, 1440);
      EXPECT_EQ(staging.env_width, 2560);
      EXPECT_EQ(staging.env_height, 2520);
      expect_wlgrab_keeps_stagings_input(monitors, 1, label("a turned monitor at the bottom", turned, sent));
    }
  }
}

// A turned monitor left of the origin. local/staging measured from zero, so its extents ignore
// what lies left of it, and its offset is negative.
TEST(WaylandMonitorLayout, ATurnedMonitorLeftOfTheOriginKeepsStagingsInput) {
  for (const auto turned : quarter_turns) {
    for (const bool sent : {true, false}) {
      std::vector<std::unique_ptr<wl::monitor_t>> monitors;
      monitors.emplace_back(monitor(-1080, 0, 1920, 1080, turned, 1, logical_size_if(sent, 1080, 1920)));
      monitors.emplace_back(monitor(0, 0, 2560, 1440, transform::normal, 1, logical_size_if(sent, 2560, 1440)));

      const auto staging = staging_wlgrab(monitors, 0);
      EXPECT_EQ(staging.offset_x, -1080);
      EXPECT_EQ(staging.env_width, 2560);
      expect_wlgrab_keeps_stagings_input(monitors, 0, label("a turned monitor left of the origin", turned, sent));
    }
  }
}

// KMS capture of a monitor Wayland turns a quarter keeps what local/staging gave it too: the CRTC's
// mode at Wayland's position, on the extents of every enumerated connector, whether or not Wayland
// named every active CRTC.
TEST(WaylandMonitorLayout, KmsTurnedMonitorKeepsStagingsInput) {
  for (const auto turned : quarter_turns) {
    const std::string at = " at transform " + std::to_string(turned);

    const kms_layout_t lone {
      {{{0, 0, 1920, 1080}, laid_out(0, 0, 1080, 1920, 1920, 1080, turned)}},
      {{0, 0, 1920, 1080}},
    };
    expect_kms_keeps_stagings_input(lone, 0, 1920, 1080, "KMS, a lone turned monitor" + at);

    const kms_layout_t reporters {
      {
        {{0, 0, 2560, 1440}, laid_out(0, 0, 2560, 1440, 2560, 1440)},
        {{2560, 0, 1920, 1080}, laid_out(2560, 0, 1080, 1920, 1920, 1080, turned)},
      },
      {{0, 0, 2560, 1440}, {2560, 0, 1920, 1080}},
    };
    expect_kms_keeps_stagings_input(reporters, 1, 1920, 1080, "KMS, the reporter's turned monitor" + at);

    const kms_layout_t right_edge {
      {
        {{0, 0, 1920, 1080}, laid_out(0, 0, 1920, 1080, 1920, 1080)},
        {{1920, 0, 2560, 1440}, laid_out(1920, 0, 1440, 2560, 2560, 1440, turned)},
      },
      {{0, 0, 1920, 1080}, {1920, 0, 2560, 1440}},
    };
    expect_kms_keeps_stagings_input(right_edge, 1, 2560, 1440, "KMS, a tall turned monitor at the right edge" + at);

    const kms_layout_t bottom {
      {
        {{0, 0, 2560, 1440}, laid_out(0, 0, 2560, 1440, 2560, 1440)},
        {{0, 1440, 1920, 1080}, laid_out(0, 1440, 1080, 1920, 1920, 1080, turned)},
      },
      {{0, 0, 2560, 1440}, {0, 1440, 1920, 1080}},
    };
    expect_kms_keeps_stagings_input(bottom, 1, 1920, 1080, "KMS, a turned monitor at the bottom" + at);

    // A CRTC without a Wayland output keeps the desktop in CRTC rectangles, and a connector Wayland
    // placed at 5000,0 whose CRTC scans nothing out still reached local/staging's extents.
    const kms_layout_t unmatched {
      {
        {{0, 0, 2560, 1440}, std::nullopt},
        {{2560, 0, 1920, 1080}, laid_out(2560, 0, 1080, 1920, 1920, 1080, turned)},
      },
      {{0, 0, 2560, 1440}, {2560, 0, 1920, 1080}, {5000, 0, 0, 0}},
    };
    expect_kms_keeps_stagings_input(unmatched, 1, 1920, 1080, "KMS, a turned monitor beside an unmatched CRTC" + at);

    // A portrait panel the compositor turns to landscape, whose frame KMS capture swaps for a plane
    // rotated 270, as a Steam Deck's is: local/staging mapped that 1280x800 frame onto extents of
    // the 800x1280 mode.
    const kms_layout_t panel {
      {{{0, 0, 800, 1280}, laid_out(0, 0, 1280, 800, 800, 1280, turned)}},
      {{0, 0, 800, 1280}},
    };
    expect_kms_keeps_stagings_input(panel, 0, 1280, 800, "KMS, a turned panel whose frame capture swapped" + at);
  }
}
#endif
