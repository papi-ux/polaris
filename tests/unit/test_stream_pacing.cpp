/**
 * @file tests/unit/test_stream_pacing.cpp
 * @brief Tests for the video sender's wire pacing arithmetic, schedule and sleep.
 */
#include "../tests_common.h"

#include <limits>

#include <src/stream_pacing.h>

namespace {
  using namespace std::literals;
  using steady = std::chrono::steady_clock;

  // 1,400 byte payloads in a 1,416 byte block, as a typical Moonlight session sends.
  constexpr std::size_t k_blocksize = 1416;
}  // namespace

TEST(StreamPacingTests, LegacyRateIsTheSendersOwnExpression) {
  for (const int blocksize : {316, 1040, 1416, 1508}) {
    // The sender computed this with an int block size; the helper must agree for every real one.
    const std::size_t expected = std::giga::num * 80 / 100 / 1000 / blocksize / 8;
    EXPECT_EQ(stream_pacing::legacy_packets_in_1ms(blocksize), expected) << blocksize;
  }
  EXPECT_EQ(stream_pacing::legacy_packets_in_1ms(k_blocksize), 70u);
}

TEST(StreamPacingTests, OffKeepsTheLegacyRateWhateverTheSessionNegotiated) {
  const auto rate = stream_pacing::select_rate(false, 20'000, 200, k_blocksize);
  EXPECT_FALSE(rate.enabled);
  EXPECT_FALSE(rate.session_rate);
  EXPECT_EQ(rate.packets_in_1ms, 70u);
  EXPECT_EQ(rate.packets_per_second, 70'000u);
  EXPECT_EQ(stream_pacing::rate_name(rate), "off");
  EXPECT_EQ(stream_pacing::send_batch_limit(rate, 46), 46u);
}

TEST(StreamPacingTests, OffSchedulesEveryPacketWhereTheSenderAlwaysDid) {
  for (const int blocksize : {316, 1416, 1508}) {
    const auto rate = stream_pacing::select_rate(false, 20'000, 200, blocksize);
    const auto legacy = stream_pacing::legacy_packets_in_1ms(blocksize);
    for (const std::size_t sent : {0u, 1u, 46u, 69u, 70u, 71u, 500u, 1023u, 4096u}) {
      const std::chrono::nanoseconds expected = std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) * sent / legacy;
      EXPECT_EQ(stream_pacing::send_offset(rate, sent), expected) << blocksize << " " << sent;
    }
  }
}

TEST(StreamPacingTests, OnWithoutANegotiatedTotalKeepsTheLegacyRate) {
  // A client that sent no bitrate, or a watcher, has no split of its own to pace to.
  for (const std::int64_t negotiated : {0, -1}) {
    const auto rate = stream_pacing::select_rate(true, negotiated, 200, k_blocksize);
    EXPECT_TRUE(rate.enabled);
    EXPECT_FALSE(rate.session_rate);
    EXPECT_EQ(rate.packets_in_1ms, 70u);
    EXPECT_EQ(rate.packets_per_second, 70'000u);
    EXPECT_EQ(stream_pacing::rate_name(rate), "legacy");
    EXPECT_EQ(stream_pacing::send_offset(rate, 140), 2ms);
  }
}

TEST(StreamPacingTests, OnPacesAtTheNegotiatedTotalTimesTheHeadroom) {
  // 20 Mbps at 200% is 40,000,000 bits a second, 3,531.07 packets of 1,416 bytes.
  const auto rate = stream_pacing::select_rate(true, 20'000, 200, k_blocksize);
  EXPECT_TRUE(rate.enabled);
  EXPECT_TRUE(rate.session_rate);
  EXPECT_EQ(rate.bits_per_second, 40'000'000u);
  EXPECT_EQ(rate.packets_per_second, 3'531u);
  EXPECT_EQ(rate.packets_in_1ms, 3u);
  EXPECT_EQ(stream_pacing::rate_name(rate), "session");

  // The schedule follows the exact rate: 11,328 bits a packet is 283.2 microseconds, not a rounded rate's.
  EXPECT_EQ(stream_pacing::send_offset(rate, 0), 0ns);
  EXPECT_EQ(stream_pacing::send_offset(rate, 1), 283'200ns);
  EXPECT_EQ(stream_pacing::send_offset(rate, 64), 18'124'800ns);
  EXPECT_EQ(stream_pacing::send_offset(rate, 3'531), 999'979'200ns);
  EXPECT_EQ(stream_pacing::send_offset(rate, 125'000), 35'400ms);

  // At 100% the same session paces at half that rate.
  EXPECT_EQ(stream_pacing::select_rate(true, 20'000, 100, k_blocksize).bits_per_second, 20'000'000u);
}

TEST(StreamPacingTests, ALowRateIsPacedExactlyNotAtWholePacketsPerSecond) {
  // 2 Mbps at 100% is 176.55 packets a second. Each packet is due 5.664 ms after the last; 176 whole packets a
  // second would have spaced them 5.682 ms apart and fallen behind the rate.
  const auto rate = stream_pacing::select_rate(true, 2'000, 100, k_blocksize);
  ASSERT_TRUE(rate.session_rate);
  EXPECT_EQ(rate.packets_per_second, 176u);
  EXPECT_EQ(stream_pacing::send_offset(rate, 1), 5'664us);
  EXPECT_EQ(stream_pacing::send_offset(rate, 176), 996'864us);
  EXPECT_EQ(stream_pacing::send_offset(rate, 1'000), 5'664ms);

  // Smaller packets at the same rate are due sooner, in proportion.
  const auto small = stream_pacing::select_rate(true, 20'000, 200, 316);
  ASSERT_TRUE(small.session_rate);
  EXPECT_EQ(small.packets_in_1ms, 15u);
  EXPECT_EQ(stream_pacing::send_offset(small, 1), 63'200ns);
}

TEST(StreamPacingTests, LongSchedulesAndAbsurdInputsDoNotOverflow) {
  const auto rate = stream_pacing::select_rate(true, 20'000, 200, k_blocksize);
  EXPECT_EQ(stream_pacing::send_offset(rate, 1'000'000'000), 283'200s);

  // Inputs no session has keep the legacy rate rather than wrap the arithmetic.
  EXPECT_FALSE(stream_pacing::select_rate(true, std::numeric_limits<std::int64_t>::max(), 200, k_blocksize).session_rate);
  EXPECT_FALSE(stream_pacing::select_rate(true, 20'000, std::numeric_limits<int>::max(), k_blocksize).session_rate);
  EXPECT_FALSE(stream_pacing::select_rate(true, 20'000, 0, k_blocksize).session_rate);
}

TEST(StreamPacingTests, ASessionSendsAtMostItsMillisecondBudgetAtOnce) {
  const auto slow = stream_pacing::select_rate(true, 20'000, 200, k_blocksize);
  EXPECT_EQ(stream_pacing::send_batch_limit(slow, 46), 3u);

  // Below one packet a millisecond, it still sends one at a time rather than none.
  const auto slower = stream_pacing::select_rate(true, 2'000, 100, k_blocksize);
  EXPECT_EQ(slower.packets_in_1ms, 1u);
  EXPECT_EQ(stream_pacing::send_batch_limit(slower, 46), 1u);

  // A batch smaller than the rate's millisecond is left alone.
  const auto fast = stream_pacing::select_rate(true, 300'000, 200, k_blocksize);
  ASSERT_TRUE(fast.session_rate);
  EXPECT_EQ(fast.packets_in_1ms, 52u);
  EXPECT_EQ(stream_pacing::send_batch_limit(fast, 46), 46u);
}

TEST(StreamPacingTests, NeverPacesFasterThanTheLegacyRate) {
  // 350 Mbps at 200% is 700 Mbps, still under the legacy 793 Mbps of 1,416 byte packets.
  EXPECT_TRUE(stream_pacing::select_rate(true, 350'000, 200, k_blocksize).session_rate);

  // 500 Mbps at 200% would be 1 Gbps. Streams that fast are not this change's.
  const auto rate = stream_pacing::select_rate(true, 500'000, 200, k_blocksize);
  EXPECT_TRUE(rate.enabled);
  EXPECT_FALSE(rate.session_rate);
  EXPECT_EQ(rate.packets_per_second, 70'000u);

  // A rate too small to send even one packet a second is no usable pace either.
  EXPECT_FALSE(stream_pacing::select_rate(true, 1, 100, k_blocksize).session_rate);
}

TEST(StreamPacingTests, EachSessionKeepsItsOwnScheduleOnTheSharedSender) {
  stream_pacing::session_starts_t starts;
  const auto now = steady::time_point {} + 1h;

  // A session with no schedule starts now.
  EXPECT_EQ(starts.frame_start(1, now), now);
  EXPECT_FALSE(starts.next(1));

  // One still sending its last group starts after it, and another session does not wait for it.
  starts.frame_end(1, now + 40ms);
  EXPECT_EQ(starts.frame_start(1, now + 10ms), now + 40ms);
  EXPECT_EQ(starts.frame_start(2, now + 10ms), now + 10ms);

  starts.frame_end(2, now + 15ms);
  starts.frame_end(1, now + 60ms);
  EXPECT_EQ(starts.frame_start(1, now + 20ms), now + 60ms);

  // A schedule that has run out no longer delays the session, but still says where it ended, which is what
  // the session's duplicate frames are stamped with.
  EXPECT_EQ(starts.frame_start(2, now + 20ms), now + 20ms);
  EXPECT_EQ(starts.next(2), now + 15ms);
  EXPECT_EQ(starts.size(), 2u);

  // A session that has sent nothing for a while is forgotten.
  EXPECT_EQ(starts.frame_start(3, now + 60ms + 1s + 1ms), now + 60ms + 1s + 1ms);
  EXPECT_EQ(starts.size(), 0u);
  EXPECT_FALSE(starts.next(1));
}

TEST(StreamPacingTests, WaitingWakesAtLeastEveryCancellationStep) {
  const auto now = steady::time_point {} + 1h;
  EXPECT_EQ(stream_pacing::k_max_wait_step, 10ms);
  EXPECT_EQ(stream_pacing::next_wake(now + 3ms, now), now + 3ms);
  EXPECT_EQ(stream_pacing::next_wake(now + 10ms, now), now + 10ms);
  EXPECT_EQ(stream_pacing::next_wake(now + 250ms, now), now + 10ms);
}

TEST(StreamPacingTests, SleepsToAnAbsoluteDeadlineAndNeverWakesEarly) {
#ifdef __linux__
  // A deadline that has passed returns at once.
  const auto passed = steady::now() - 1ms;
  const auto before_passed = steady::now();
  EXPECT_TRUE(stream_pacing::sleep_until(passed));
  EXPECT_LT(steady::now() - before_passed, 100ms);

  // A future one sleeps until it, and is not merely a short nap.
  const auto start = steady::now();
  const auto until = start + 3ms;
  ASSERT_TRUE(stream_pacing::sleep_until(until));
  const auto woke = steady::now();
  EXPECT_GE(woke, until);
  EXPECT_LT(woke - start, 1s);
#else
  // Without an absolute sleep the sender keeps its relative timer.
  EXPECT_FALSE(stream_pacing::sleep_until(steady::now() + 1ms));
#endif
}
