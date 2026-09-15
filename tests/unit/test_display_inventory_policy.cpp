/**
 * @file tests/unit/test_display_inventory_policy.cpp
 * @brief The system-stats route enumerates displays rarely, and never repeatedly during a stream.
 */
#include <src/display_inventory_policy.h>

#include <gtest/gtest.h>

using namespace std::chrono_literals;
namespace di = display_inventory;

TEST(DisplayInventoryPolicy, FirstPollEnumeratesThenTheCacheServesFor30Seconds) {
  EXPECT_TRUE(di::should_enumerate(false, false, false, 0s));
  EXPECT_FALSE(di::should_enumerate(true, true, false, 3s));
  EXPECT_FALSE(di::should_enumerate(true, true, false, 29s));
  EXPECT_TRUE(di::should_enumerate(true, true, false, 30s));
  EXPECT_TRUE(di::should_enumerate(true, true, false, 5min));
}

TEST(DisplayInventoryPolicy, ADesktopStreamNeverReenumeratesTheDesktopCompositor) {
  // The idle inventory already describes the desktop's outputs; a stream of that desktop
  // is not a reason to open a new client against a compositor that is screencasting.
  EXPECT_EQ(di::cache_key(false, ""), "desktop");
  EXPECT_EQ(di::cache_key(false, "/run/user/1000/wayland-1"), "desktop");
  EXPECT_FALSE(di::should_enumerate(true, true, true, 3s));
  EXPECT_FALSE(di::should_enumerate(true, true, true, 10min));
  // Unless there is nothing cached at all.
  EXPECT_TRUE(di::should_enumerate(false, false, true, 0s));
}

TEST(DisplayInventoryPolicy, APrivateCompositorStreamEnumeratesOnceAtStartAndOnceAtEnd) {
  const auto cage = di::cache_key(true, "/run/user/1000/wayland-1");
  EXPECT_EQ(cage, "cage:/run/user/1000/wayland-1");
  // A private compositor that is not up yet keeps the desktop inventory.
  EXPECT_EQ(di::cache_key(true, ""), "desktop");

  // The key changed when the stream started: one enumeration against the private socket.
  EXPECT_TRUE(di::should_enumerate(true, false, true, 3s));
  // Then the stream is served from that inventory whatever its age.
  EXPECT_FALSE(di::should_enumerate(true, true, true, 20min));
  // The key changes back when the stream ends: one more, then the idle interval again.
  EXPECT_TRUE(di::should_enumerate(true, false, false, 0s));
  EXPECT_FALSE(di::should_enumerate(true, true, false, 1s));
}
