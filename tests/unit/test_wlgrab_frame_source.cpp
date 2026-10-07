/**
 * @file tests/unit/test_wlgrab_frame_source.cpp
 * @brief Tests for ext-image-copy frame sourcing, pacing, and timing.
 */
#include <chrono>
#include <cstdint>
#include <limits>

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

TEST(ExtcopySession, IsRecreatedWhenTheCursorSettingChanges) {
  // Displays start without cursor painting and the stream turns it on at the first frame,
  // since mouse_cursor_visible defaults to true. Reusing the session there kept the cursor
  // out of the stream and made an idle output rebuild the pipeline about once a second.
  EXPECT_FALSE(wl::extcopy_session_reusable(true, false, true));
  EXPECT_FALSE(wl::extcopy_session_reusable(true, true, false));
  EXPECT_TRUE(wl::extcopy_session_reusable(true, true, true));
  EXPECT_TRUE(wl::extcopy_session_reusable(true, false, false));
  EXPECT_FALSE(wl::extcopy_session_reusable(false, false, false));
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

TEST(WlgrabCapturePacing, PrivateScreencopyUsesOnlyTheCompositorClock) {
  EXPECT_EQ(
    wl::screencopy_pacing_policy(true),
    wl::capture_pacing_policy_e::source_driven
  );
  EXPECT_EQ(
    wl::screencopy_pacing_policy(false),
    wl::capture_pacing_policy_e::fixed_interval
  );
}

TEST(WlgrabCapturePacing, MeasuresSuccessfulSourceFrameRateFromIntervals) {
  wl::capture_source_rate_tracker_t tracker;
  const wl::capture_source_rate_tracker_t::time_point_t start {10s};

  EXPECT_FALSE(tracker.note_frame(start));
  for (int frame = 1; frame < 120; ++frame) {
    EXPECT_FALSE(tracker.note_frame(start + frame * (1s / 120)));
  }
  const auto fps = tracker.note_frame(start + 1s);
  ASSERT_TRUE(fps);
  EXPECT_NEAR(*fps, 120.0, 0.01);
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

TEST(ExtcopyTiming, BackwardPresentationNeverLowersTheValidBaseline) {
  using vrr_policy::presentation_status_e;
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};

  timing.mark_requested(start);
  ASSERT_TRUE(timing.mark_presentation(0, 2, 0));
  timing.mark_ready(start + 8ms);
  EXPECT_EQ(timing.last_sample().presentation_status, presentation_status_e::valid);

  // The clock goes back a second. The value is kept as delivered, marked, and given no interval.
  timing.mark_requested(start + 9ms);
  ASSERT_TRUE(timing.mark_presentation(0, 1, 0));
  timing.mark_ready(start + 17ms);
  EXPECT_EQ(timing.last_sample().presentation_status, presentation_status_e::nonmonotonic);
  EXPECT_EQ(timing.last_sample().presentation_ns, 1'000'000'000);
  EXPECT_FALSE(timing.last_sample().presentation_interval.has_value());

  // Still behind the last VALID time (2 s), so still nonmonotonic: the backward sample did not
  // become a baseline a later frame could pass against.
  timing.mark_requested(start + 18ms);
  ASSERT_TRUE(timing.mark_presentation(0, 1, 8'333'333));
  timing.mark_ready(start + 26ms);
  EXPECT_EQ(timing.last_sample().presentation_status, presentation_status_e::nonmonotonic);
  EXPECT_FALSE(timing.last_sample().presentation_interval.has_value());

  // Past the valid baseline again: valid, and the interval runs from the last valid frame.
  timing.mark_requested(start + 27ms);
  ASSERT_TRUE(timing.mark_presentation(0, 2, 8'333'333));
  timing.mark_ready(start + 35ms);
  EXPECT_EQ(timing.last_sample().presentation_status, presentation_status_e::valid);
  EXPECT_EQ(timing.last_sample().presentation_interval, 8333us);
}

TEST(ExtcopyTiming, EqualPresentationIsNotAFreshFrame) {
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};
  timing.mark_requested(start);
  ASSERT_TRUE(timing.mark_presentation(0, 3, 500));
  timing.mark_ready(start + 8ms);
  timing.mark_requested(start + 9ms);
  ASSERT_TRUE(timing.mark_presentation(0, 3, 500));
  timing.mark_ready(start + 17ms);
  EXPECT_EQ(timing.last_sample().presentation_status, vrr_policy::presentation_status_e::nonmonotonic);
  EXPECT_FALSE(timing.last_sample().presentation_interval.has_value());
}

TEST(ExtcopyTiming, KeepsTheRawAbsolutePresentationTime) {
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};
  timing.mark_requested(start);
  // tv_sec_hi carries the high 32 bits of seconds (ext-image-copy-capture-v1 presentation_time).
  ASSERT_TRUE(timing.mark_presentation(1, 5, 123'456'789));
  timing.mark_ready(start + 8ms);
  const std::int64_t seconds = (std::int64_t {1} << 32) + 5;
  EXPECT_EQ(timing.last_sample().presentation_ns, seconds * 1'000'000'000 + 123'456'789);
  EXPECT_EQ(timing.last_sample().presentation_status, vrr_policy::presentation_status_e::valid);
}

TEST(ExtcopyTiming, NamesMissingAndInvalidPresentationTimes) {
  using vrr_policy::presentation_status_e;
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};

  // No presentation_time event before ready.
  timing.mark_requested(start);
  timing.mark_ready(start + 8ms);
  EXPECT_EQ(timing.last_sample().presentation_status, presentation_status_e::missing);
  EXPECT_FALSE(timing.last_sample().presentation_ns.has_value());

  timing.mark_requested(start + 9ms);
  EXPECT_FALSE(timing.mark_presentation(0, 1, 1'000'000'000));
  timing.mark_ready(start + 17ms);
  EXPECT_EQ(timing.last_sample().presentation_status, presentation_status_e::invalid_nsec);
  EXPECT_FALSE(timing.last_sample().presentation_ns.has_value());

  // One past the largest second count signed nanoseconds can carry.
  constexpr std::uint64_t max_seconds = static_cast<std::uint64_t>(INT64_MAX) / 1'000'000'000ULL;
  timing.mark_requested(start + 18ms);
  EXPECT_FALSE(timing.mark_presentation(
    static_cast<std::uint32_t>((max_seconds + 1) >> 32U), static_cast<std::uint32_t>(max_seconds + 1), 0));
  timing.mark_ready(start + 26ms);
  EXPECT_EQ(timing.last_sample().presentation_status, presentation_status_e::invalid_overflow);

  // The largest representable value is still valid.
  timing.mark_requested(start + 27ms);
  EXPECT_TRUE(timing.mark_presentation(
    static_cast<std::uint32_t>(max_seconds >> 32U), static_cast<std::uint32_t>(max_seconds), 0));
  timing.mark_ready(start + 35ms);
  EXPECT_EQ(timing.last_sample().presentation_status, presentation_status_e::valid);
}

TEST(ExtcopyTiming, InvalidSampleBetweenValidOnesYieldsNoHealthyInterval) {
  using vrr_policy::presentation_status_e;
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};
  timing.mark_requested(start);
  ASSERT_TRUE(timing.mark_presentation(0, 1, 0));
  timing.mark_ready(start + 8ms);
  timing.mark_requested(start + 9ms);
  EXPECT_FALSE(timing.mark_presentation(0, 1, 2'000'000'000));
  timing.mark_ready(start + 17ms);
  EXPECT_EQ(timing.last_sample().presentation_status, presentation_status_e::invalid_nsec);
  EXPECT_FALSE(timing.last_sample().presentation_interval.has_value());
  timing.mark_requested(start + 18ms);
  ASSERT_TRUE(timing.mark_presentation(0, 1, 16'666'667));
  timing.mark_ready(start + 26ms);
  // Spans the invalid frame: measured from the last valid time, never from the invalid one.
  EXPECT_EQ(timing.last_sample().presentation_interval, 16666us);
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

TEST(ExtcopyTiming, EverySampleNamesItsTrackerSessionAndResetStartsANewOne) {
  wl::extcopy_timing_tracker_t timing;
  const wl::extcopy_timing_tracker_t::time_point_t start {10s};
  EXPECT_EQ(timing.baseline(), 1u);
  timing.mark_requested(start);
  ASSERT_TRUE(timing.mark_presentation(0, 5, 0));
  timing.mark_ready(start + 8ms);
  EXPECT_EQ(timing.last_sample().baseline, 1u);
  timing.mark_ready(start + 16ms);
  EXPECT_EQ(timing.last_sample().baseline, 1u) << "frames do not change the session";
  timing.reset();
  EXPECT_EQ(timing.last_sample().baseline, 0u) << "no sample yet in the new session";
  // The new session has no baseline: a time before the previous session's is valid at this level.
  // Only the capture epoch's mapping guard can refuse it (regressed_in_epoch).
  timing.mark_requested(start + 20ms);
  ASSERT_TRUE(timing.mark_presentation(0, 4, 0));
  timing.mark_ready(start + 28ms);
  EXPECT_EQ(timing.last_sample().baseline, 2u);
  EXPECT_EQ(timing.last_sample().presentation_status, vrr_policy::presentation_status_e::valid);
}

TEST(ExtcopyTiming, TheSessionNumberSaturatesInsteadOfWrapping) {
  constexpr auto max = std::numeric_limits<std::uint64_t>::max();
  wl::extcopy_timing_tracker_t timing {max - 1};
  timing.reset();
  EXPECT_EQ(timing.baseline(), max);
  timing.reset();
  EXPECT_EQ(timing.baseline(), max) << "never wraps back to an earlier session's number";
  timing.mark_ready(wl::extcopy_timing_tracker_t::time_point_t {1s});
  EXPECT_EQ(timing.last_sample().baseline, max);
}
