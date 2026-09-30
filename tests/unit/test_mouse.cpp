/**
 * @file tests/unit/test_mouse.cpp
 * @brief Test src/input.*.
 */
#include "../tests_common.h"

#include <array>
#include <optional>
#include <utility>

#include <src/input.h>
#include <src/video.h>

TEST(InputTouchPortMapping, RejectsNonPositiveClientSurfaceDimensions) {
  input::touch_port_t touch_port {
    {0, 0, 1920, 1080},
    1920,
    1080,
    0.0f,
    0.0f,
    1.0f
  };

  EXPECT_EQ(std::nullopt, input::map_client_to_touchport(touch_port, {50.0f, 50.0f}, {0.0f, 100.0f}));
  EXPECT_EQ(std::nullopt, input::map_client_to_touchport(touch_port, {50.0f, 50.0f}, {100.0f, -1.0f}));
}

// A Wayland capture reads its own viewport from wl_output and the desktop
// extents from the compositor's globals, so it can come back sized 0x0 while the
// desktop is known. That used to make the scalar infinite and refuse every
// absolute packet for the whole session, which is nova#302.
TEST(InputTouchPortMapping, CaptureWithoutAViewportStillMapsAgainstTheDesktop) {
  const auto port = input::make_touch_port(platf::touch_port_t {0, 0, 0, 0}, 3840, 2160, 1920, 1080);

  EXPECT_TRUE(static_cast<bool>(port));
  EXPECT_GT(port.scalar_inv, 0.0f);
  EXPECT_EQ(port.env_width, 3840);
  EXPECT_EQ(port.env_height, 2160);

  input::touchport_reject_e reason = input::touchport_reject_e::none;
  const auto middle = input::map_client_to_touchport(port, {960.0f, 540.0f}, {1920.0f, 1080.0f}, &reason);
  ASSERT_TRUE(middle.has_value());
  EXPECT_EQ(reason, input::touchport_reject_e::none);
  EXPECT_NEAR(middle->first, 1920.0f, 1.0f);
  EXPECT_NEAR(middle->second, 1080.0f, 1.0f);

  const auto corner = input::map_client_to_touchport(port, {0.0f, 0.0f}, {1920.0f, 1080.0f}, &reason);
  ASSERT_TRUE(corner.has_value());
  EXPECT_NEAR(corner->first, 0.0f, 1.0f);
  EXPECT_NEAR(corner->second, 0.0f, 1.0f);
}

// A capture that does report its own size keeps mapping onto that size, letterbox
// and all, so the fallback above cannot change a working host.
TEST(InputTouchPortMapping, CaptureWithAViewportIsUnchanged) {
  const auto port = input::make_touch_port(platf::touch_port_t {0, 0, 2560, 1440}, 2560, 1440, 1920, 1080);

  EXPECT_FLOAT_EQ(port.scalar_inv, 2560.0f / 1920.0f);
  EXPECT_FLOAT_EQ(port.client_offsetX, 0.0f);
  EXPECT_FLOAT_EQ(port.client_offsetY, 0.0f);

  const auto middle = input::map_client_to_touchport(port, {960.0f, 540.0f}, {1920.0f, 1080.0f});
  ASSERT_TRUE(middle.has_value());
  EXPECT_NEAR(middle->first, 1280.0f, 1.0f);
  EXPECT_NEAR(middle->second, 720.0f, 1.0f);
}

// An aspect mismatch still letterboxes, and the offsets still bracket the frame.
TEST(InputTouchPortMapping, LetterboxesAnAspectMismatch) {
  const auto port = input::make_touch_port(platf::touch_port_t {0, 0, 1280, 1024}, 1280, 1024, 1920, 1080);

  EXPECT_GT(port.client_offsetX, 0.0f);
  EXPECT_FLOAT_EQ(port.client_offsetY, 0.0f);

  input::touchport_reject_e reason = input::touchport_reject_e::none;
  const auto mapped = input::map_client_to_touchport(port, {960.0f, 540.0f}, {1920.0f, 1080.0f}, &reason);
  ASSERT_TRUE(mapped.has_value());
  EXPECT_EQ(reason, input::touchport_reject_e::none);
  EXPECT_NEAR(mapped->first, 640.0f, 1.0f);
  EXPECT_NEAR(mapped->second, 512.0f, 1.0f);
}

// A Steam Deck in Game Mode: gamescope fits its 1280x800 screen into the 1920x1080 frame it
// exports, with a 96 pixel bar down each side. Mapped across the whole frame, as it was before the
// capture named the screen, a tap on the Nova Deck library's Recent tab at x=355 landed at 237 on
// the Deck instead of 192, and missed the tab.
TEST(InputTouchPortMapping, AGameModeScreenFittedIntoTheFrameMapsEdgeToEdge) {
  const auto port = input::make_touch_port(platf::touch_port_t {0, 0, 1280, 800}, 1280, 800, 1920, 1080);

  EXPECT_NEAR(port.client_offsetX, 96.0f, 0.5f);
  EXPECT_FLOAT_EQ(port.client_offsetY, 0.0f);

  const auto left = input::map_client_to_touchport(port, {96.0f, 540.0f}, {1920.0f, 1080.0f});
  const auto right = input::map_client_to_touchport(port, {1824.0f, 540.0f}, {1920.0f, 1080.0f});
  const auto tab = input::map_client_to_touchport(port, {355.0f, 237.0f}, {1920.0f, 1080.0f});
  ASSERT_TRUE(left && right && tab);
  EXPECT_NEAR(left->first, 0.0f, 1.0f);
  EXPECT_NEAR(left->second, 400.0f, 1.0f);
  EXPECT_NEAR(right->first, 1280.0f, 1.0f);
  EXPECT_NEAR(tab->first, 192.0f, 1.0f);
  EXPECT_NEAR(tab->second, 176.0f, 1.0f);
}

// The same screen in a frame of a third shape: gamescope fits 1280x800 into a 1920x1080 frame as
// 1728x1080, and that frame goes into a 1280x800 stream as 1280x720 with bars above and below. The
// screen is 1152x720 in the stream, not the 1280x800 a single fit of screen into stream says.
TEST(InputTouchPortMapping, AScreenInAFrameOfAnotherShapeTakesBothFits) {
  const auto port = input::make_touch_port_in_frame(1280, 800, 1920, 1080, 1280, 800);

  EXPECT_NEAR(port.client_offsetX, 64.0f, 0.5f);
  EXPECT_NEAR(port.client_offsetY, 40.0f, 0.5f);
  const auto left = input::map_client_to_touchport(port, {64.0f, 400.0f}, {1280.0f, 800.0f});
  const auto bottom_right = input::map_client_to_touchport(port, {1216.0f, 760.0f}, {1280.0f, 800.0f});
  ASSERT_TRUE(left && bottom_right);
  EXPECT_NEAR(left->first, 0.0f, 1.0f);
  EXPECT_NEAR(left->second, 400.0f, 1.0f);
  EXPECT_NEAR(bottom_right->first, 1280.0f, 1.0f);
  EXPECT_NEAR(bottom_right->second, 800.0f, 1.0f);

  // A frame the stream's shape is one fit, as the Deck's 1920x1080 frame in a 1920x1080 stream is.
  const auto same_shape = input::make_touch_port_in_frame(1280, 800, 1920, 1080, 1920, 1080);
  const auto direct = input::make_touch_port(platf::touch_port_t {0, 0, 1280, 800}, 1280, 800, 1920, 1080);
  EXPECT_NEAR(same_shape.client_offsetX, direct.client_offsetX, 0.01f);
  EXPECT_NEAR(same_shape.client_offsetY, direct.client_offsetY, 0.01f);
  EXPECT_NEAR(same_shape.scalar_inv, direct.scalar_inv, 0.0001f);
}

namespace {
  // gamescope's apply_touchscreen_orientation, as 3.16.23 writes it.
  std::pair<float, float> gamescope_turns(int degrees, float x, float y) {
    switch (degrees) {
      case 90:
        return {1.0f - y, x};
      case 180:
        return {1.0f - x, 1.0f - y};
      case 270:
        return {y, 1.0f - x};
      default:
        return {x, y};
    }
  }
}  // namespace

// gamescope turns a touch from a virtual touchscreen by the internal panel's orientation. Turned
// back first, every touch lands where it was aimed, whichever way the panel is mounted.
TEST(InputTouchPortMapping, ATouchTurnedBackLandsWhereItWasAimedAfterGamescopeTurnsIt) {
  const std::array<std::pair<float, float>, 5> aims {{{0.0f, 0.0f}, {1.0f, 0.0f}, {0.15f, 0.22f}, {0.5f, 0.5f}, {0.9f, 1.0f}}};
  for (const int degrees : {0, 90, 180, 270}) {
    for (const auto &[x, y] : aims) {
      const auto sent = input::turn_back_touch(degrees, x, y);
      const auto landed = gamescope_turns(degrees, sent.first, sent.second);
      EXPECT_NEAR(landed.first, x, 1e-6f) << degrees << " degrees";
      EXPECT_NEAR(landed.second, y, 1e-6f) << degrees << " degrees";
    }
  }
}

// Found on a Steam Deck: its panel is right side up, so gamescope turns touch 270 degrees, and a
// tap on the Nova app's Recent tab near the top left had to be sent from the top right to land.
TEST(InputTouchPortMapping, ASteamDeckTapIsSentFromWhereGamescopeTurnsItOntoTheTarget) {
  const auto sent = input::turn_back_touch(270, 0.15f, 0.22f);
  EXPECT_NEAR(sent.first, 0.78f, 1e-6f);
  EXPECT_NEAR(sent.second, 0.15f, 1e-6f);

  // A turn gamescope has no name for is not guessed at.
  const auto unturned = input::turn_back_touch(45, 0.15f, 0.22f);
  EXPECT_FLOAT_EQ(unturned.first, 0.15f);
  EXPECT_FLOAT_EQ(unturned.second, 0.22f);
}

// The refusals have to be told apart, because one warning covering all of them is
// what left nova#302 with several hundred identical lines a second and no cause.
TEST(InputTouchPortMapping, NamesWhyACoordinateWasRefused) {
  const auto port = input::make_touch_port(platf::touch_port_t {0, 0, 2560, 1440}, 2560, 1440, 1920, 1080);

  input::touchport_reject_e reason = input::touchport_reject_e::none;
  EXPECT_EQ(std::nullopt, input::map_client_to_touchport(port, {50.0f, 50.0f}, {0.0f, 1080.0f}, &reason));
  EXPECT_EQ(reason, input::touchport_reject_e::client_surface_empty);

  auto empty_capture = port;
  empty_capture.scalar_inv = 0.0f;
  EXPECT_EQ(std::nullopt, input::map_client_to_touchport(empty_capture, {50.0f, 50.0f}, {1920.0f, 1080.0f}, &reason));
  EXPECT_EQ(reason, input::touchport_reject_e::capture_viewport_empty);

  auto inverted = port;
  inverted.client_offsetX = static_cast<float>(inverted.width) + 1.0f;
  EXPECT_EQ(std::nullopt, input::map_client_to_touchport(inverted, {50.0f, 50.0f}, {1920.0f, 1080.0f}, &reason));
  EXPECT_EQ(reason, input::touchport_reject_e::letterbox_bounds_inverted);

  EXPECT_EQ(input::touchport_reject_name(input::touchport_reject_e::capture_viewport_empty),
            "the capture never reported a size of its own");
}

namespace {
  /**
   * @brief A display that holds nothing but where its screen sits.
   */
  class placed_display_t final: public platf::display_t {
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
}  // namespace

// Absolute input maps onto the captured screen in the desktop's units. A capture that sets no
// input size, which is every one but wlroots and KMS capture on Wayland, maps onto its frame as
// before. One that sets it maps onto that: here a 1920x1080 monitor at scale 1 right of a scale 2
// monitor, on a desktop counted in the scale 2 monitor's pixels.
TEST(InputTouchPortMapping, ScreenOnDesktopTakesTheInputSizeOnlyWhenSet) {
  placed_display_t display;
  display.offset_x = 3840;
  display.offset_y = 0;
  display.width = 1920;
  display.height = 1080;

  auto screen = display.screen_on_desktop();
  EXPECT_EQ(screen.offset_x, 3840);
  EXPECT_EQ(screen.offset_y, 0);
  EXPECT_EQ(screen.width, 1920);
  EXPECT_EQ(screen.height, 1080);

  // Half a size is no size: the frame's shape is kept whole rather than mixed with it.
  display.input_width = 3840;
  screen = display.screen_on_desktop();
  EXPECT_EQ(screen.width, 1920);
  EXPECT_EQ(screen.height, 1080);

  display.input_height = 2160;
  screen = display.screen_on_desktop();
  EXPECT_EQ(screen.offset_x, 3840);
  EXPECT_EQ(screen.width, 3840);
  EXPECT_EQ(screen.height, 2160);
}

// A point on the captured screen is counted from that screen's corner, and absolute input spans
// the whole desktop, so the screen's own place on the desktop goes in first. Linux left it out, and
// a screen right of another took its pointer on the one at the origin.
TEST(InputTouchPortMapping, PointOnDesktopCountsFromTheScreensCorner) {
  const platf::touch_port_t beside_the_main_monitor {2560, 0, 3640, 1920};
  const auto [x, y] = platf::point_on_desktop(beside_the_main_monitor, 540.0f, 960.0f);
  EXPECT_FLOAT_EQ(x, 3100.0f);
  EXPECT_FLOAT_EQ(y, 960.0f);

  const platf::touch_port_t below_it {0, 1440, 2560, 2520};
  const auto [below_x, below_y] = platf::point_on_desktop(below_it, 1280.0f, 540.0f);
  EXPECT_FLOAT_EQ(below_x, 1280.0f);
  EXPECT_FLOAT_EQ(below_y, 1980.0f);

  const platf::touch_port_t at_the_origin {0, 0, 2560, 1440};
  const auto [origin_x, origin_y] = platf::point_on_desktop(at_the_origin, 1280.0f, 720.0f);
  EXPECT_FLOAT_EQ(origin_x, 1280.0f);
  EXPECT_FLOAT_EQ(origin_y, 720.0f);
}

namespace {
  video::config_t streaming_at(int width, int height) {
    video::config_t config {};
    config.width = width;
    config.height = height;
    return config;
  }

  void expect_same_port(const input::touch_port_t &port, const input::touch_port_t &expected) {
    EXPECT_EQ(port.offset_x, expected.offset_x);
    EXPECT_EQ(port.offset_y, expected.offset_y);
    EXPECT_EQ(port.width, expected.width);
    EXPECT_EQ(port.height, expected.height);
    EXPECT_EQ(port.env_width, expected.env_width);
    EXPECT_EQ(port.env_height, expected.env_height);
    EXPECT_FLOAT_EQ(port.client_offsetX, expected.client_offsetX);
    EXPECT_FLOAT_EQ(port.client_offsetY, expected.client_offsetY);
    EXPECT_FLOAT_EQ(port.scalar_inv, expected.scalar_inv);
    EXPECT_EQ(port.compositor_touch_turn, expected.compositor_touch_turn);
  }
}  // namespace

// make_port() builds every session's touch port. A capture that sets no input size, which is
// every one but wlroots and KMS capture on Wayland, has input mapped onto its frame as it always
// did: here X11 capture of a monitor right of another.
TEST(InputTouchPortMapping, MakePortMapsOntoTheFrameWithoutAnInputSize) {
  placed_display_t display;
  display.offset_x = 2560;
  display.offset_y = 0;
  display.width = 1920;
  display.height = 1080;
  display.env_width = 4480;
  display.env_height = 1440;

  expect_same_port(
    video::make_port(&display, streaming_at(1920, 1080)),
    input::make_touch_port(platf::touch_port_t {2560, 0, 1920, 1080}, 4480, 1440, 1920, 1080)
  );
}

// With an input size, make_port() maps onto that rectangle rather than the frame. A 1920x1080
// monitor at scale 1 right of a scale 2 monitor covers 3840x2160 desktop pixels, so a point in
// the middle of its 1920x1080 stream lands in the middle of that rectangle, two pixels on for
// each pixel of the stream.
TEST(InputTouchPortMapping, MakePortMapsOntoTheScreensRectangleOnTheDesktop) {
  placed_display_t display;
  display.offset_x = 3840;
  display.offset_y = 0;
  display.width = 1920;
  display.height = 1080;
  display.input_width = 3840;
  display.input_height = 2160;
  display.env_width = 8960;
  display.env_height = 2880;

  const auto port = video::make_port(&display, streaming_at(1920, 1080));
  EXPECT_EQ(port.offset_x, 3840);
  EXPECT_EQ(port.offset_y, 0);
  EXPECT_EQ(port.env_width, 8960);
  EXPECT_EQ(port.env_height, 2880);
  EXPECT_FLOAT_EQ(port.scalar_inv, 2.0f);

  const auto middle = input::map_client_to_touchport(port, {960.0f, 540.0f}, {1920.0f, 1080.0f});
  ASSERT_TRUE(middle.has_value());
  EXPECT_FLOAT_EQ(middle->first, 1920.0f);
  EXPECT_FLOAT_EQ(middle->second, 1080.0f);
}

// wlroots and KMS capture of a monitor turned a quarter keep the input 1.4.13 gave it: the
// display keeps the monitor's place, here 2560,0 on 4480x1440 extents, but every point counts from
// the desktop's corner, so make_port() leaves the place out and nothing else changes. On Linux only
// abs_mouse() reads a touch port's place, and 1.4.13's never added it.
TEST(InputTouchPortMapping, MakePortLeavesThePlaceOutWhenPointsCountFromTheDesktopsCorner) {
  placed_display_t display;
  display.offset_x = 2560;
  display.offset_y = 0;
  display.width = 1920;
  display.height = 1080;
  display.env_width = 4480;
  display.env_height = 1440;
  display.input_counts_from_screen = false;

  const auto screen = display.screen_on_desktop();
  EXPECT_EQ(screen.offset_x, 0);
  EXPECT_EQ(screen.offset_y, 0);
  EXPECT_EQ(screen.width, 1920);
  EXPECT_EQ(screen.height, 1080);

  const auto port = video::make_port(&display, streaming_at(1920, 1080));
  expect_same_port(port, input::make_touch_port(platf::touch_port_t {0, 0, 1920, 1080}, 4480, 1440, 1920, 1080));

  const auto far_corner = input::map_client_to_touchport(port, {1920.0f, 1080.0f}, {1920.0f, 1080.0f});
  ASSERT_TRUE(far_corner.has_value());
  const auto [x, y] = platf::point_on_desktop(platf::touch_port_t {port.offset_x, port.offset_y, port.env_width, port.env_height}, far_corner->first, far_corner->second);
  EXPECT_FLOAT_EQ(x, 1920.0f);
  EXPECT_FLOAT_EQ(y, 1080.0f);

  // Every other capture counts from its screen's corner.
  display.input_counts_from_screen = true;
  EXPECT_EQ(video::make_port(&display, streaming_at(1920, 1080)).offset_x, 2560);
}

// Game Mode names a screen fitted inside its frame, and the turn gamescope gives a touch.
// make_port() places input inside the picture and passes the turn on, with or without an input
// size, which Game Mode never sets anyway.
TEST(InputTouchPortMapping, MakePortPlacesAFittedScreenInsideThePicture) {
  placed_display_t display;
  display.width = 1920;
  display.height = 1080;
  display.scaled_screen_width = 1280;
  display.scaled_screen_height = 800;
  display.compositor_touch_turn = 90;

  auto expected = input::make_touch_port_in_frame(1280, 800, 1920, 1080, 1920, 1080);
  expected.compositor_touch_turn = 90;
  expect_same_port(video::make_port(&display, streaming_at(1920, 1080)), expected);

  display.input_width = 3840;
  display.input_height = 2160;
  expect_same_port(video::make_port(&display, streaming_at(1920, 1080)), expected);
}

TEST(InputTouchPortMapping, RejectsInvertedLetterboxBounds) {
  input::touch_port_t touch_port {
    {0, 0, 100, 100},
    100,
    100,
    80.0f,
    0.0f,
    1.0f
  };

  EXPECT_EQ(std::nullopt, input::map_client_to_touchport(touch_port, {50.0f, 50.0f}, {100.0f, 100.0f}));
}

struct MouseHIDTest: PlatformTestSuite, testing::WithParamInterface<util::point_t> {
  void SetUp() override {
#ifdef _WIN32
    // TODO: Windows tests are failing, `get_mouse_loc` seems broken and `platf::abs_mouse` too
    //       the alternative `platf::abs_mouse` method seem to work better during tests,
    //       but I'm not sure about real work
    GTEST_SKIP() << "TODO Windows";
#elif __linux__
    // TODO: Inputtino waiting https://github.com/games-on-whales/inputtino/issues/6 is resolved.
    GTEST_SKIP() << "TODO Inputtino";
#endif
  }

  void TearDown() override {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
};

INSTANTIATE_TEST_SUITE_P(
  MouseInputs,
  MouseHIDTest,
  testing::Values(
    util::point_t {40, 40},
    util::point_t {70, 150}
  )
);

// todo: add tests for hitting screen edges

TEST_P(MouseHIDTest, MoveInputTest) {
  util::point_t mouse_delta = GetParam();

  BOOST_LOG(tests) << "MoveInputTest:: got param: " << mouse_delta;
  platf::input_t input = platf::input();
  BOOST_LOG(tests) << "MoveInputTest:: init input";

  BOOST_LOG(tests) << "MoveInputTest:: get current mouse loc";
  auto old_loc = platf::get_mouse_loc(input);
  BOOST_LOG(tests) << "MoveInputTest:: got current mouse loc: " << old_loc;

  BOOST_LOG(tests) << "MoveInputTest:: move: " << mouse_delta;
  platf::move_mouse(input, mouse_delta.x, mouse_delta.y);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  BOOST_LOG(tests) << "MoveInputTest:: moved: " << mouse_delta;

  BOOST_LOG(tests) << "MoveInputTest:: get updated mouse loc";
  auto new_loc = platf::get_mouse_loc(input);
  BOOST_LOG(tests) << "MoveInputTest:: got updated mouse loc: " << new_loc;

  bool has_input_moved = old_loc.x != new_loc.x && old_loc.y != new_loc.y;

  if (!has_input_moved) {
    BOOST_LOG(tests) << "MoveInputTest:: haven't moved";
  } else {
    BOOST_LOG(tests) << "MoveInputTest:: moved";
  }

  EXPECT_TRUE(has_input_moved);

  // Verify we moved as much as we requested
  EXPECT_EQ(new_loc.x - old_loc.x, mouse_delta.x);
  EXPECT_EQ(new_loc.y - old_loc.y, mouse_delta.y);
}

TEST_P(MouseHIDTest, AbsMoveInputTest) {
  util::point_t mouse_pos = GetParam();
  BOOST_LOG(tests) << "AbsMoveInputTest:: got param: " << mouse_pos;

  platf::input_t input = platf::input();
  BOOST_LOG(tests) << "AbsMoveInputTest:: init input";

  BOOST_LOG(tests) << "AbsMoveInputTest:: get current mouse loc";
  auto old_loc = platf::get_mouse_loc(input);
  BOOST_LOG(tests) << "AbsMoveInputTest:: got current mouse loc: " << old_loc;

#ifdef _WIN32
  platf::touch_port_t abs_port {
    0,
    0,
    65535,
    65535
  };
#elif __linux__
  platf::touch_port_t abs_port {
    0,
    0,
    19200,
    12000
  };
#else
  platf::touch_port_t abs_port {};
#endif
  BOOST_LOG(tests) << "AbsMoveInputTest:: move: " << mouse_pos;
  platf::abs_mouse(input, abs_port, mouse_pos.x, mouse_pos.y);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  BOOST_LOG(tests) << "AbsMoveInputTest:: moved: " << mouse_pos;

  BOOST_LOG(tests) << "AbsMoveInputTest:: get updated mouse loc";
  auto new_loc = platf::get_mouse_loc(input);
  BOOST_LOG(tests) << "AbsMoveInputTest:: got updated mouse loc: " << new_loc;

  bool has_input_moved = old_loc.x != new_loc.x || old_loc.y != new_loc.y;

  if (!has_input_moved) {
    BOOST_LOG(tests) << "AbsMoveInputTest:: haven't moved";
  } else {
    BOOST_LOG(tests) << "AbsMoveInputTest:: moved";
  }

  EXPECT_TRUE(has_input_moved);

  // Verify we moved to the absolute coordinate
  EXPECT_EQ(new_loc.x, mouse_pos.x);
  EXPECT_EQ(new_loc.y, mouse_pos.y);
}
