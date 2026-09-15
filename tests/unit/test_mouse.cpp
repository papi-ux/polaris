/**
 * @file tests/unit/test_mouse.cpp
 * @brief Test src/input.*.
 */
#include "../tests_common.h"

#include <optional>

#include <src/input.h>

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
