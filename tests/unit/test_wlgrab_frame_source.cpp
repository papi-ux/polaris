/**
 * @file tests/unit/test_wlgrab_frame_source.cpp
 * @brief Tests for ext-image-copy frame sourcing, pacing, and timing.
 */
#include <chrono>

#include <gtest/gtest.h>

#include "src/platform/linux/wlgrab_frame_source.h"
#include "src/platform/linux/wlgrab_pacing.h"
#include "src/platform/linux/wlgrab_timing.h"

using namespace std::chrono_literals;

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

TEST(WlgrabCapturePacing, FixedIntervalStartsImmediatelyThenWaits) {
  const wl::capture_pacer_t::time_point_t start {10s};
  wl::capture_pacer_t pacer {
    wl::capture_pacing_policy_e::fixed_interval,
    10ms,
    start,
  };

  EXPECT_EQ(pacer.wait_duration(start), 0ns);
  pacer.advance(start);
  EXPECT_EQ(pacer.wait_duration(start), 10ms);
  EXPECT_EQ(pacer.wait_duration(start + 4ms), 6ms);
}

TEST(WlgrabCapturePacing, FixedIntervalRecoversAfterFallingBehind) {
  const wl::capture_pacer_t::time_point_t start {10s};
  wl::capture_pacer_t pacer {
    wl::capture_pacing_policy_e::fixed_interval,
    10ms,
    start,
  };

  pacer.advance(start + 25ms);
  EXPECT_EQ(pacer.wait_duration(start + 25ms), 10ms);
}

TEST(WlgrabCapturePacing, SourceDrivenNeverAddsASecondClock) {
  const wl::capture_pacer_t::time_point_t start {10s};
  wl::capture_pacer_t pacer {
    wl::capture_pacing_policy_e::source_driven,
    10ms,
    start,
  };

  EXPECT_EQ(pacer.wait_duration(start), 0ns);
  pacer.advance(start);
  EXPECT_EQ(pacer.wait_duration(start + 1ms), 0ns);
  pacer.advance(start + 25ms);
  EXPECT_EQ(pacer.wait_duration(start + 25ms), 0ns);
}

TEST(ExtcopyTiming, RecordsRequestReadyAndPresentationIntervals) {
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};

  timing.mark_requested(start);
  EXPECT_TRUE(timing.mark_presentation(0, 1, 100'000'000));
  timing.mark_ready(start + 8ms);
  EXPECT_EQ(timing.last_sample().request_to_ready, 8ms);
  EXPECT_FALSE(timing.last_sample().presentation_interval.has_value());

  timing.mark_requested(start + 9ms);
  EXPECT_TRUE(timing.mark_presentation(0, 1, 108'333'333));
  timing.mark_ready(start + 17ms);
  EXPECT_EQ(timing.last_sample().request_to_ready, 8ms);
  EXPECT_EQ(timing.last_sample().presentation_interval, 8333us);
  EXPECT_EQ(timing.last_sample().ready_at, start + 17ms);
}

TEST(ExtcopyTiming, RejectsInvalidPresentationNanoseconds) {
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};

  timing.mark_requested(start);
  EXPECT_FALSE(timing.mark_presentation(0, 1, 1'000'000'000));
  timing.mark_ready(start + 8ms);

  EXPECT_EQ(timing.last_sample().request_to_ready, 8ms);
  EXPECT_FALSE(timing.last_sample().presentation_interval.has_value());
}

TEST(ExtcopyTiming, RejectsPresentationTimestampOverflow) {
  wl::extcopy_timing_tracker_t timing;

  EXPECT_FALSE(timing.mark_presentation(UINT32_MAX, UINT32_MAX, 999'999'999));
}

TEST(ExtcopyTiming, RecoversAfterPresentationClockMovesBackward) {
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};

  timing.mark_requested(start);
  ASSERT_TRUE(timing.mark_presentation(0, 2, 0));
  timing.mark_ready(start + 8ms);
  timing.mark_requested(start + 9ms);
  ASSERT_TRUE(timing.mark_presentation(0, 1, 0));
  timing.mark_ready(start + 17ms);
  EXPECT_FALSE(timing.last_sample().presentation_interval.has_value());

  timing.mark_requested(start + 18ms);
  ASSERT_TRUE(timing.mark_presentation(0, 1, 8'333'333));
  timing.mark_ready(start + 26ms);
  EXPECT_EQ(timing.last_sample().presentation_interval, 8333us);
}

TEST(ExtcopyTiming, ResetPreventsIntervalsAcrossCaptureSessions) {
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};

  timing.mark_requested(start);
  ASSERT_TRUE(timing.mark_presentation(0, 1, 100'000'000));
  timing.mark_ready(start + 8ms);
  timing.reset();
  timing.mark_requested(start + 20ms);
  ASSERT_TRUE(timing.mark_presentation(0, 5, 200'000'000));
  timing.mark_ready(start + 28ms);

  EXPECT_FALSE(timing.last_sample().presentation_interval.has_value());
}
