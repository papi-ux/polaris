/**
 * @file tests/unit/test_stream_fec.cpp
 * @brief Test bounded oversized-frame FEC diagnostics.
 */

#include <src/stream_fec.h>

#include <gtest/gtest.h>

#include <chrono>

namespace {
  stream::oversized_fec_frame_observation_t observation(
    std::size_t encoded_bytes,
    std::size_t packetized_bytes,
    std::size_t required_blocks,
    stream::fec_frame_type_e frame_type = stream::fec_frame_type_e::delta
  ) {
    return {
      .encoded_frame_bytes = encoded_bytes,
      .packetized_frame_bytes = packetized_bytes,
      .protected_payload_limit_bytes = 1'006'720,
      .required_blocks = required_blocks,
      .fec_percentage = 5,
      .packet_size = 1024,
      .frame_type = frame_type
    };
  }
}

TEST(StreamFecWarningTrackerTests, ReportsFirstOversizedFrameImmediately) {
  stream::fec_warning_tracker_t tracker;
  const auto now = stream::fec_warning_tracker_t::clock::time_point {};

  const auto report = tracker.observe(
    observation(1'080'000, 1'100'000, 5, stream::fec_frame_type_e::idr),
    now
  );

  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->frame_count, 1);
  EXPECT_EQ(report->idr_frame_count, 1);
  EXPECT_EQ(report->largest_encoded_frame_bytes, 1'080'000);
  EXPECT_EQ(report->largest_packetized_frame_bytes, 1'100'000);
  EXPECT_EQ(report->protected_payload_limit_bytes, 1'006'720);
  EXPECT_EQ(report->max_required_blocks, 5);
  EXPECT_EQ(report->largest_frame_type, stream::fec_frame_type_e::idr);
}

TEST(StreamFecWarningTrackerTests, CoalescesRepeatsIntoOnePeriodicSummary) {
  using namespace std::chrono_literals;

  stream::fec_warning_tracker_t tracker {20s};
  const auto start = stream::fec_warning_tracker_t::clock::time_point {};
  ASSERT_TRUE(tracker.observe(observation(1'020'000, 1'040'000, 5), start));

  EXPECT_FALSE(tracker.observe(
    observation(1'250'000, 1'280'000, 6), start + 1s
  ));
  EXPECT_FALSE(tracker.observe(
    observation(1'500'000, 1'540'000, 7, stream::fec_frame_type_e::reference_invalidation),
    start + 10s
  ));
  const auto report = tracker.observe(
    observation(2'900'000, 2'940'000, 12, stream::fec_frame_type_e::idr),
    start + 20s
  );

  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->frame_count, 3);
  EXPECT_EQ(report->idr_frame_count, 1);
  EXPECT_EQ(report->reference_invalidation_frame_count, 1);
  EXPECT_EQ(report->largest_encoded_frame_bytes, 2'900'000);
  EXPECT_EQ(report->largest_packetized_frame_bytes, 2'940'000);
  EXPECT_EQ(report->max_required_blocks, 12);
  EXPECT_EQ(report->largest_frame_type, stream::fec_frame_type_e::idr);
}

TEST(StreamFecWarningTrackerTests, StartsASecondAggregationWindowAfterReport) {
  using namespace std::chrono_literals;

  stream::fec_warning_tracker_t tracker {20s};
  const auto start = stream::fec_warning_tracker_t::clock::time_point {};
  ASSERT_TRUE(tracker.observe(observation(1'020'000, 1'040'000, 5), start));
  ASSERT_TRUE(tracker.observe(observation(1'100'000, 1'120'000, 5), start + 20s));

  EXPECT_FALSE(tracker.observe(observation(1'200'000, 1'220'000, 5), start + 21s));
  const auto report = tracker.observe(
    observation(1'300'000, 1'320'000, 6), start + 40s
  );

  ASSERT_TRUE(report.has_value());
  EXPECT_EQ(report->frame_count, 2);
  EXPECT_EQ(report->max_required_blocks, 6);
}
