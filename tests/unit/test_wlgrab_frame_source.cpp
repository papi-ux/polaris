/**
 * @file tests/unit/test_wlgrab_frame_source.cpp
 * @brief Tests for ext-image-copy prefetched-frame handoff.
 */
#include <gtest/gtest.h>

#include "src/platform/linux/wlgrab_frame_source.h"

TEST(ExtcopyFrameSource, ConsumesFramePrefetchedDuringInitialization) {
  bool prefetched_frame_pending = true;

  EXPECT_EQ(
    wl::select_extcopy_frame_source(true, false, prefetched_frame_pending),
    wl::extcopy_frame_source_e::prefetched
  );
  EXPECT_FALSE(prefetched_frame_pending);
}

TEST(ExtcopyFrameSource, RequestsNewFrameAfterPrefetchedFrameWasConsumed) {
  bool prefetched_frame_pending = false;

  EXPECT_EQ(
    wl::select_extcopy_frame_source(true, false, prefetched_frame_pending),
    wl::extcopy_frame_source_e::capture
  );
}

TEST(ExtcopyFrameSource, ReinitializationSupersedesStalePrefetchedFrame) {
  bool prefetched_frame_pending = true;

  EXPECT_EQ(
    wl::select_extcopy_frame_source(false, false, prefetched_frame_pending),
    wl::extcopy_frame_source_e::initialize
  );
  EXPECT_FALSE(prefetched_frame_pending);

  prefetched_frame_pending = true;
  EXPECT_EQ(
    wl::select_extcopy_frame_source(true, true, prefetched_frame_pending),
    wl::extcopy_frame_source_e::initialize
  );
  EXPECT_FALSE(prefetched_frame_pending);
}
