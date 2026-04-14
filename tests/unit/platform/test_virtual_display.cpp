/**
 * @file tests/unit/platform/test_virtual_display.cpp
 * @brief Test Linux virtual display backend detection helpers.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/virtual_display.h>

TEST(VirtualDisplayTests, BackendDetectionLogCacheOnlySignalsOnFirstObservationAndChanges) {
  virtual_display::backend_detection_log_cache_t cache;

  EXPECT_TRUE(cache.note(virtual_display::backend_e::KSCREEN_DOCTOR));
  EXPECT_FALSE(cache.note(virtual_display::backend_e::KSCREEN_DOCTOR));
  EXPECT_TRUE(cache.note(virtual_display::backend_e::WAYLAND_WLR));
  EXPECT_FALSE(cache.note(virtual_display::backend_e::WAYLAND_WLR));
}
#else
TEST(VirtualDisplayTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only virtual display tests";
}
#endif
