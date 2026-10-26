/**
 * @file tests/unit/test_stream_fec.cpp
 * @brief Test bounded oversized-frame FEC diagnostics.
 */

#include <src/stream_fec.h>
#include <src/stream_fec_status.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>

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

namespace {
  using fec_wire_t = std::array<unsigned char, stream::fec_status::payload_size>;

  void fec_put16(fec_wire_t &wire, std::size_t offset, std::uint16_t value) {
    wire[offset] = static_cast<unsigned char>(value >> 8);
    wire[offset + 1] = static_cast<unsigned char>(value);
  }

  fec_wire_t fec_wire(std::uint32_t frame = 0x89abcdefu, unsigned char block = 2, unsigned char count = 4) {
    // Literal current common-c wire contract: D=100, P=10, Rd=89, Rp=10.
    // The eleven absent data shards leave a one-shard rank deficit.
    fec_wire_t wire {
      0x89, 0xab, 0xcd, 0xef, 0xff, 0xfe, 0x01, 0x23, 0x00, 0x0b,
      0x00, 0x64, 0x00, 0x0a, 0x00, 0x59, 0x00, 0x0a, 0x0a, 0x02, 0x04
    };
    wire[0] = static_cast<unsigned char>(frame >> 24);
    wire[1] = static_cast<unsigned char>(frame >> 16);
    wire[2] = static_cast<unsigned char>(frame >> 8);
    wire[3] = static_cast<unsigned char>(frame);
    wire[19] = block;
    wire[20] = count;
    return wire;
  }

  std::string_view fec_payload(const fec_wire_t &wire) {
    return {reinterpret_cast<const char *>(wire.data()), wire.size()};
  }
}

TEST(StreamFecStatusParserTests, DecodesUnalignedBigEndianPayloadAndReportedBlockFacts) {
  namespace fs = stream::fec_status;
  const auto wire = fec_wire();
  std::array<unsigned char, fs::payload_size + 1> unaligned {};
  std::copy(wire.begin(), wire.end(), unaligned.begin() + 1);
  const auto report = fs::parse(
    {reinterpret_cast<const char *>(unaligned.data() + 1), fs::payload_size}, fs::client_feature_flag
  );

  ASSERT_EQ(report.refusal, fs::refusal_e::none);
  ASSERT_TRUE(report.block);
  EXPECT_EQ(report.block->frame_index, 0x89abcdefu);
  EXPECT_EQ(report.block->highest_received_sequence_number, 0xfffe);
  EXPECT_EQ(report.block->next_contiguous_sequence_number, 0x0123);
  EXPECT_EQ(report.block->missing_packets_before_highest_received, 11);
  EXPECT_EQ(report.block->total_data_packets, 100);
  EXPECT_EQ(report.block->total_parity_packets, 10);
  EXPECT_EQ(report.block->received_data_packets, 89);
  EXPECT_EQ(report.block->received_parity_packets, 10);
  EXPECT_EQ(report.block->fec_percentage, 10);
  EXPECT_EQ(report.block->block_index, 2);
  EXPECT_EQ(report.block->block_count, 4);
  EXPECT_EQ(report.block->absent_data_shards(), 11);
  EXPECT_EQ(report.block->rank_deficit_shards(), 1);
}

TEST(StreamFecStatusParserTests, AcceptsEffectivePercentageAbovePolicyCapAndClientRounding) {
  namespace fs = stream::fec_status;
  struct parity_case_t {
    std::uint16_t data;
    std::uint16_t parity;
    unsigned char percentage;
  };
  // Small-block minimum parity can yield 200%, or a truncated ratio such as
  // floor(100 * 2 / 3) = 66; the client reconstructs P with a ceiling.
  for (const auto &input : {parity_case_t {1, 2, 200}, parity_case_t {3, 2, 66}}) {
    SCOPED_TRACE(input.percentage);
    auto wire = fec_wire();
    fec_put16(wire, 8, 0);
    fec_put16(wire, 10, input.data);
    fec_put16(wire, 12, input.parity);
    fec_put16(wire, 14, static_cast<std::uint16_t>(input.data - 1));
    fec_put16(wire, 16, 1);
    wire[18] = input.percentage;
    const auto report = fs::parse(fec_payload(wire), fs::client_feature_flag);

    ASSERT_EQ(report.refusal, fs::refusal_e::none);
    ASSERT_TRUE(report.block);
    EXPECT_EQ(report.block->fec_percentage, input.percentage);
    // Having enough shards is not an actual RS/decode success flag.
    EXPECT_EQ(report.block->absent_data_shards(), 1);
    EXPECT_EQ(report.block->rank_deficit_shards(), 0);
  }
}

TEST(StreamFecStatusParserTests, AcceptsParityFreeOversizedDataAndWideCounters) {
  namespace fs = stream::fec_status;
  auto wire = fec_wire();
  fec_put16(wire, 8, 0x02fd);
  fec_put16(wire, 10, 1023);
  fec_put16(wire, 12, 0);
  fec_put16(wire, 14, 0x0102);
  fec_put16(wire, 16, 0);
  wire[18] = 0;
  const auto report = fs::parse(fec_payload(wire), fs::client_feature_flag);

  ASSERT_EQ(report.refusal, fs::refusal_e::none);
  ASSERT_TRUE(report.block);
  EXPECT_EQ(report.block->total_data_packets, 1023);
  EXPECT_EQ(report.block->received_data_packets, 258);
  EXPECT_EQ(report.block->missing_packets_before_highest_received, 765);
  EXPECT_EQ(report.block->absent_data_shards(), 765);
  EXPECT_EQ(report.block->rank_deficit_shards(), 765);
}

TEST(StreamFecStatusParserTests, RequiresAdvertisedCapabilityAndExactCurrentLength) {
  namespace fs = stream::fec_status;
  const auto wire = fec_wire();
  for (const auto flags : {0u, 2u}) {
    const auto report = fs::parse(fec_payload(wire), flags);
    EXPECT_EQ(report.refusal, fs::refusal_e::capability_unavailable);
    EXPECT_FALSE(report.block);
  }
  EXPECT_EQ(fs::parse(fec_payload(wire), 3).refusal, fs::refusal_e::none);
  for (const std::size_t length : {0u, 16u, 20u}) {
    SCOPED_TRACE(length);
    const auto report = fs::parse(fec_payload(wire).substr(0, length), fs::client_feature_flag);
    EXPECT_EQ(report.refusal, fs::refusal_e::invalid_size);
    EXPECT_FALSE(report.block);
  }
  const std::array<char, fs::payload_size + 1> extra {};
  const auto report = fs::parse({extra.data(), extra.size()}, fs::client_feature_flag);
  EXPECT_EQ(report.refusal, fs::refusal_e::invalid_size);
  EXPECT_FALSE(report.block);
}

TEST(StreamFecStatusParserTests, RefusesMalformedDataReceiveCountsAndBlockIdentity) {
  namespace fs = stream::fec_status;
  struct invalid_count_t {
    std::size_t offset;
    std::uint16_t value;
    fs::refusal_e refusal;
  };
  for (const auto &input : {
         invalid_count_t {10, 0, fs::refusal_e::invalid_data_count},
         invalid_count_t {10, 1024, fs::refusal_e::invalid_data_count},
         invalid_count_t {14, 101, fs::refusal_e::invalid_received_count},
         invalid_count_t {16, 11, fs::refusal_e::invalid_received_count}
       }) {
    SCOPED_TRACE(input.offset);
    SCOPED_TRACE(input.value);
    auto wire = fec_wire();
    fec_put16(wire, input.offset, input.value);
    const auto report = fs::parse(fec_payload(wire), fs::client_feature_flag);
    EXPECT_EQ(report.refusal, input.refusal);
    EXPECT_FALSE(report.block);
  }
  const std::array<std::array<unsigned char, 2>, 3> invalid_blocks {{{19, 4}, {20, 0}, {20, 5}}};
  for (const auto &input : invalid_blocks) {
    auto wire = fec_wire();
    wire[input[0]] = input[1];
    const auto report = fs::parse(fec_payload(wire), fs::client_feature_flag);
    EXPECT_EQ(report.refusal, fs::refusal_e::invalid_block_identity);
    EXPECT_FALSE(report.block);
  }
}

TEST(StreamFecStatusParserTests, ChecksClientParityRelationProtectedLimitAndMissingBounds) {
  namespace fs = stream::fec_status;
  auto wire = fec_wire();
  fec_put16(wire, 12, 11);
  EXPECT_EQ(fs::parse(fec_payload(wire), 1).refusal, fs::refusal_e::invalid_parity_count);
  wire = fec_wire();
  wire[18] = 0;
  EXPECT_EQ(fs::parse(fec_payload(wire), 1).refusal, fs::refusal_e::invalid_parity_count);

  wire = fec_wire();
  fec_put16(wire, 10, 240);
  fec_put16(wire, 12, 24);
  EXPECT_EQ(fs::parse(fec_payload(wire), 1).refusal, fs::refusal_e::protected_block_too_large);
  // Exactly 255 total protected shards is allowed.
  fec_put16(wire, 8, 0);
  fec_put16(wire, 10, 250);
  fec_put16(wire, 12, 5);
  fec_put16(wire, 14, 245);
  fec_put16(wire, 16, 5);
  wire[18] = 2;
  EXPECT_EQ(fs::parse(fec_payload(wire), 1).refusal, fs::refusal_e::none);

  wire = fec_wire();
  fec_put16(wire, 8, 12);  // Only eleven of 110 shards are not received.
  const auto report = fs::parse(fec_payload(wire), 1);
  EXPECT_EQ(report.refusal, fs::refusal_e::invalid_missing_count);
  EXPECT_FALSE(report.block);
}

TEST(StreamFecStatusTrackerTests, RequiresBoundGenerationCapabilityAndFreshAcceptedReport) {
  using namespace std::chrono_literals;
  namespace fs = stream::fec_status;
  fs::tracker_t tracker;
  const auto now = fs::clock::time_point {} + 1s;
  const auto wire = fec_wire();
  EXPECT_EQ(tracker.snapshot(11, now, 1s).availability, fs::availability_e::generation_unavailable);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::generation_unavailable);

  tracker.reset(11, 0);
  EXPECT_EQ(tracker.snapshot(11, now, 1s).availability, fs::availability_e::capability_unavailable);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::capability_unavailable);
  tracker.reset(11, fs::client_feature_flag);
  EXPECT_EQ(tracker.snapshot(11, now, 1s).availability, fs::availability_e::no_report);
  EXPECT_EQ(tracker.observe(12, fec_payload(wire), now), fs::refusal_e::generation_mismatch);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire).substr(0, 20), now), fs::refusal_e::invalid_size);
  EXPECT_EQ(tracker.snapshot(11, now, 1s).availability, fs::availability_e::no_report);

  ASSERT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::none);
  const auto fresh = tracker.snapshot(11, now + 500ms, 500ms);
  ASSERT_EQ(fresh.availability, fs::availability_e::fresh_report);
  ASSERT_TRUE(fresh.sample);
  EXPECT_EQ(fresh.sample->received_at, now);
  const auto stale = tracker.snapshot(11, now + 501ms, 500ms);
  EXPECT_EQ(stale.availability, fs::availability_e::stale_report);
  EXPECT_FALSE(stale.sample);
  EXPECT_EQ(tracker.snapshot(12, now, 1s).availability, fs::availability_e::generation_mismatch);
  EXPECT_EQ(tracker.snapshot(11, now - 1ms, 1s).availability, fs::availability_e::reversed_clock);
  EXPECT_EQ(tracker.snapshot(11, now, -1ms).availability, fs::availability_e::invalid_max_age);

  tracker.reset(12, fs::client_feature_flag);
  EXPECT_EQ(tracker.snapshot(12, now, 1s).availability, fs::availability_e::no_report);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::generation_mismatch);
  tracker.reset(0, fs::client_feature_flag);
  EXPECT_EQ(tracker.snapshot(0, now, 1s).availability, fs::availability_e::generation_unavailable);
}

TEST(StreamFecStatusTrackerTests, RefusalsPreserveAcceptedTimestampAndFrame) {
  using namespace std::chrono_literals;
  namespace fs = stream::fec_status;
  fs::tracker_t tracker;
  tracker.reset(11, fs::client_feature_flag);
  const auto now = fs::clock::time_point {} + 1s;
  auto wire = fec_wire(10, 1, 3);
  ASSERT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::none);

  fec_put16(wire, 14, 88);  // Changed counters cannot refresh the same identity.
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now + 1s), fs::refusal_e::duplicate_block);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire).substr(0, 20), now + 2s), fs::refusal_e::invalid_size);
  wire = fec_wire(9, 0, 3);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now + 3s), fs::refusal_e::older_frame);
  wire = fec_wire(10, 0, 4);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now + 4s), fs::refusal_e::block_count_changed);
  wire = fec_wire(11, 0, 3);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now - 1ms), fs::refusal_e::reversed_arrival_time);

  const auto retained = tracker.snapshot(11, now + 4s, 10s);
  ASSERT_TRUE(retained.sample);
  EXPECT_EQ(retained.sample->received_at, now);
  EXPECT_EQ(retained.sample->block.frame_index, 10);
  EXPECT_EQ(retained.sample->block.received_data_packets, 89);
  EXPECT_EQ(tracker.snapshot(11, now + 11s, 10s).availability, fs::availability_e::stale_report);
}

TEST(StreamFecStatusTrackerTests, AcceptsDistinctSameFrameBlocksInAnyOrderAndRemembersDuplicates) {
  using namespace std::chrono_literals;
  namespace fs = stream::fec_status;
  fs::tracker_t tracker;
  tracker.reset(11, fs::client_feature_flag);
  auto now = fs::clock::time_point {};
  for (const unsigned char index : {3, 1, 2, 0}) {
    const auto wire = fec_wire(10, index, 4);
    EXPECT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::none);
    now += 1s;
  }
  auto wire = fec_wire(10, 3, 4);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::duplicate_block);
  wire = fec_wire(11, 0, 1);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::none);
  const auto report = tracker.snapshot(11, now, 0s);
  ASSERT_TRUE(report.sample);
  EXPECT_EQ(report.sample->block.frame_index, 11);
  EXPECT_EQ(report.sample->block.block_count, 1);
}

TEST(StreamFecStatusTrackerTests, OrdersFrameWrapWithoutSignedArithmeticAndResetsGeneration) {
  using namespace std::chrono_literals;
  namespace fs = stream::fec_status;
  fs::tracker_t tracker;
  tracker.reset(11, fs::client_feature_flag);
  auto now = fs::clock::time_point {};
  for (const std::uint32_t frame : {0xfffffffeu, 0xffffffffu, 0u, 1u}) {
    const auto wire = fec_wire(frame, 0, 1);
    ASSERT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::none);
    now += 1s;
  }
  auto wire = fec_wire(0xffffffffu, 0, 1);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::older_frame);
  wire = fec_wire(0x80000001u, 0, 1);
  EXPECT_EQ(tracker.observe(11, fec_payload(wire), now), fs::refusal_e::ambiguous_frame);
  const auto retained = tracker.snapshot(11, now, 1s);
  ASSERT_TRUE(retained.sample);
  EXPECT_EQ(retained.sample->block.frame_index, 1);
  EXPECT_EQ(retained.sample->received_at, now - 1s);

  tracker.reset(12, fs::client_feature_flag);
  wire = fec_wire(0xffffffffu, 0, 1);
  // The new generation has no ordering or timestamp inherited from generation 11.
  EXPECT_EQ(tracker.observe(12, fec_payload(wire), now - 10s), fs::refusal_e::none);
}

TEST(StreamFecStatusTrackerTests, BoundsFreshnessAtExtremeClockValuesWithoutSignedSubtraction) {
  namespace fs = stream::fec_status;
  fs::tracker_t tracker;
  tracker.reset(11, fs::client_feature_flag);
  const auto wire = fec_wire();
  ASSERT_EQ(tracker.observe(11, fec_payload(wire), fs::clock::time_point::min()), fs::refusal_e::none);
  EXPECT_EQ(
    tracker.snapshot(11, fs::clock::time_point::min(), fs::clock::duration::zero()).availability,
    fs::availability_e::fresh_report
  );
  EXPECT_EQ(
    tracker.snapshot(11, fs::clock::time_point::max(), fs::clock::duration::max()).availability,
    fs::availability_e::stale_report
  );
}

TEST(StreamFecFrameFitTests, KeepsTheLargestTargetAtTheFourBlockBoundary) {
  constexpr std::size_t block = 1024;
  const auto exact = stream::fit_frame_fec(4 * 170 * block, block, 10, 50, 2);
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->fec_percentage, 50);
  EXPECT_EQ(exact->required_blocks, 4);
  EXPECT_EQ(exact->block_count, 4);
  const auto next_byte = stream::fit_frame_fec(4 * 170 * block + 1, block, 10, 50, 2);
  ASSERT_TRUE(next_byte);
  EXPECT_EQ(next_byte->fec_percentage, 49);
  EXPECT_EQ(next_byte->block_count, 4);
}

TEST(StreamFecFrameFitTests, StepsDownRaisedParityWithoutRemovingConfiguredProtection) {
  const auto fit = stream::fit_frame_fec(800 * 1024, 1024, 10, 50, 2);
  ASSERT_TRUE(fit);
  EXPECT_EQ(fit->fec_percentage, 27);  // 200 data + 54 parity per block.
  EXPECT_EQ(fit->envelope_percentage, 27);
  EXPECT_EQ(fit->required_blocks, 4);
  const auto at_base_limit = stream::fit_frame_fec(924 * 1024, 1024, 10, 50, 2);
  ASSERT_TRUE(at_base_limit);
  EXPECT_EQ(at_base_limit->fec_percentage, 10);
}

TEST(StreamFecFrameFitTests, KeepsOriginalOversizedWarningEnvelopeAndUnprotectedFallback) {
  const auto fit = stream::fit_frame_fec(925 * 1024, 1024, 10, 50, 2);
  ASSERT_TRUE(fit);
  EXPECT_EQ(fit->fec_percentage, 0);
  EXPECT_EQ(fit->envelope_percentage, 10);
  EXPECT_EQ(fit->required_blocks, 5);
  EXPECT_EQ(fit->block_count, 4);
  EXPECT_EQ(fit->protected_payload_limit_bytes, 924 * 1024);
  const auto fixed = stream::fit_frame_fec(925 * 1024, 1024, 10, 10, 2);
  ASSERT_TRUE(fixed);
  EXPECT_EQ(fixed->fec_percentage, fit->fec_percentage);
  EXPECT_EQ(fixed->required_blocks, fit->required_blocks);
  EXPECT_EQ(fixed->protected_payload_limit_bytes, fit->protected_payload_limit_bytes);
}

TEST(StreamFecFrameFitTests, ChecksActualMinimumParityAndEffectiveHeaderRange) {
  const auto small = stream::fit_frame_fec(100, 1024, 10, 50, 2);
  ASSERT_TRUE(small);
  // This remains the policy target. The existing encoder raises wire F to
  // 200 for one data plus two parity shards; neither number is capped at 50.
  EXPECT_EQ(small->fec_percentage, 50);
  EXPECT_FALSE(stream::fit_frame_fec(100, 1024, 10, 50, 3));  // Wire F=300 does not fit eight bits.
  EXPECT_FALSE(stream::fit_frame_fec(231 * 1024, 1024, 10, 10, 25));  // 231+25 exceeds RS8.
  const auto split = stream::fit_frame_fec(231 * 1024, 1024, 10, 11, 25);
  ASSERT_TRUE(split);
  EXPECT_EQ(split->fec_percentage, 11);
  EXPECT_EQ(split->block_count, 2);  // Both aligned blocks fit the minimum parity.
}

TEST(StreamFecFrameFitTests, KeepsFixedHighAndZeroSettingsAndBoundsRequestedGrowth) {
  for (const int configured : {50, 60, 255}) {
    const auto fixed = stream::fit_frame_fec(100, 1024, configured, 70, 2);
    ASSERT_TRUE(fixed);
    EXPECT_EQ(fixed->fec_percentage, configured);
  }
  const auto zero = stream::fit_frame_fec(100, 1024, 0, 0, 255);
  ASSERT_TRUE(zero);
  EXPECT_EQ(zero->fec_percentage, 0);  // A minimum does not turn disabled parity on.
  const auto lower = stream::fit_frame_fec(100, 1024, 20, 10, 2);
  ASSERT_TRUE(lower);
  EXPECT_EQ(lower->fec_percentage, 20);
  const auto capped = stream::fit_frame_fec(100, 1024, 10, 80, 2);
  ASSERT_TRUE(capped);
  EXPECT_EQ(capped->fec_percentage, 50);
}

TEST(StreamFecFrameFitTests, RejectsInvalidDimensionsAndAvoidsSizeOverflow) {
  EXPECT_FALSE(stream::fit_frame_fec(0, 1024, 10, 20, 2));
  EXPECT_FALSE(stream::fit_frame_fec(100, 0, 10, 20, 2));
  EXPECT_FALSE(stream::fit_frame_fec(100, 1024, -1, 20, 2));
  EXPECT_FALSE(stream::fit_frame_fec(100, 1024, 10, 256, 2));
  EXPECT_FALSE(stream::fit_frame_fec(100, 1024, 10, 20, -1));
  EXPECT_FALSE(stream::fit_frame_fec(100, 1024, 10, 20, 256));
  constexpr auto largest = std::numeric_limits<std::size_t>::max();
  EXPECT_FALSE(stream::fit_frame_fec(100, largest, 10, 20, 2));
  const auto huge = stream::fit_frame_fec(largest, 1, 10, 50, 2);
  ASSERT_TRUE(huge);
  EXPECT_EQ(huge->fec_percentage, 0);
  EXPECT_EQ(huge->required_blocks, largest / 231 + (largest % 231 != 0));
  // This models the existing FEC fallback only, not a valid 10-bit wire frame.
}
