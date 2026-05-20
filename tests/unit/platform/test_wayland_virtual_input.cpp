/**
 * @file tests/unit/platform/test_wayland_virtual_input.cpp
 * @brief Test Linux labwc virtual keyboard modifier-state helpers.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT)
  #include <linux/input-event-codes.h>

  #include "src/platform/linux/input/inputtino_wayland_virtual_input.h"

TEST(WaylandVirtualInputTests, ShiftPressSerializesDepressedModifierState) {
  const auto state = platf::wayland_virtual_input::modifier_state_after_key_events_for_tests(
    {
      {KEY_LEFTSHIFT, false},
    },
    "it"
  );

  ASSERT_TRUE(state.has_value());
  EXPECT_NE(0u, state->depressed);
  EXPECT_EQ(0u, state->latched);
  EXPECT_EQ(0u, state->locked);
}

TEST(WaylandVirtualInputTests, ShiftReleaseClearsDepressedModifierState) {
  const auto state = platf::wayland_virtual_input::modifier_state_after_key_events_for_tests(
    {
      {KEY_LEFTSHIFT, false},
      {KEY_LEFTSHIFT, true},
    },
    "it"
  );

  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(0u, state->depressed);
  EXPECT_EQ(0u, state->latched);
  EXPECT_EQ(0u, state->locked);
}

TEST(WaylandVirtualInputTests, AltGrPressSerializesDepressedModifierStateForItalianLayout) {
  const auto state = platf::wayland_virtual_input::modifier_state_after_key_events_for_tests(
    {
      {KEY_RIGHTALT, false},
    },
    "it"
  );

  ASSERT_TRUE(state.has_value());
  EXPECT_NE(0u, state->depressed);
}
#endif
