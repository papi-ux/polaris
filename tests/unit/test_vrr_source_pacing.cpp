/**
 * @file tests/unit/test_vrr_source_pacing.cpp
 * @brief The pure source-time pacing rules: exact times, bounded work, ordered emission, no spending
 *        before commit. Supplements the production encode-loop cases; it does not replace them.
 */
#include <src/vrr_source_pacing.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

using vrr_pacing::candidate_t;
using vrr_pacing::event_e;
using vrr_pacing::kind_e;
using vrr_pacing::mode_e;

namespace {
  constexpr std::int64_t k_ms = 1'000'000;
  constexpr std::int64_t k_base = 10'000 * k_ms;  ///< 10 s of steady time
  constexpr vrr_pacing::rate_t k_60 {60, 1};

  vrr_pacing::source_frame_t mapped(std::uint64_t sequence, std::int64_t source_ns, std::uint64_t epoch = 7,
                                    std::uint64_t uncertainty = 1'000) {
    return {epoch, sequence, vrr_clock::source_time_e::mapped, source_ns, uncertainty};
  }

  /// Precheck and, when allowed, commit as the encode loop does after a successful encode.
  event_e emit(vrr_pacing::generation_t &generation, const vrr_pacing::source_frame_t &frame, std::int64_t now_ns) {
    const auto decision = generation.precheck_source(frame, now_ns);
    if (decision.candidate) generation.commit(*decision.candidate, now_ns);
    return decision.event;
  }
}  // namespace

TEST(VrrSourcePacingTests, AMappedFrameIsEmittedAtExactlyItsSourceTime) {
  vrr_pacing::generation_t generation {k_60};
  ASSERT_TRUE(generation.rate_valid());
  EXPECT_EQ(generation.mode(), mode_e::waiting);
  const auto decision = generation.precheck_source(mapped(1, k_base + 123'457), k_base + 2 * k_ms);
  ASSERT_TRUE(decision.candidate);
  EXPECT_EQ(decision.event, event_e::accepted_source);
  EXPECT_EQ(decision.candidate->kind, kind_e::source);
  EXPECT_EQ(decision.candidate->emit_ns, k_base + 123'457) << "never snapped to any grid";
  EXPECT_EQ(decision.candidate->capture_epoch, 7u);
  EXPECT_EQ(decision.candidate->source_sequence, 1u);
}

TEST(VrrSourcePacingTests, PrecheckSpendsNothingUntilCommit) {
  vrr_pacing::generation_t generation {k_60};
  const auto frame = mapped(1, k_base);
  // Prechecked repeatedly (a cancelled or failed encode): still the same verdict, nothing spent.
  for (int attempt = 0; attempt < 5; ++attempt) {
    ASSERT_TRUE(generation.precheck_source(frame, k_base + k_ms).candidate);
  }
  EXPECT_FALSE(generation.last_emitted_ns());
  EXPECT_EQ(generation.mode(), mode_e::waiting);
  ASSERT_EQ(emit(generation, frame, k_base + k_ms), event_e::accepted_source);
  EXPECT_EQ(generation.last_emitted_ns(), k_base);
  EXPECT_EQ(generation.mode(), mode_e::source_time);
  EXPECT_EQ(generation.precheck_source(frame, k_base + k_ms).event, event_e::stale_or_replayed)
    << "a committed sequence is never emitted twice";
}

TEST(VrrSourcePacingTests, UnevenIntervalsPassAndTheCeilingBoundsASustainedExcess) {
  vrr_pacing::generation_t generation {k_60};
  // Ordinary uneven intervals at or below 60 fps all pass, at their own times.
  std::int64_t source = k_base;
  std::uint64_t sequence = 0;
  for (const auto interval : {17, 25, 16'667'000 / 1'000'000 + 1, 33, 20}) {
    source += static_cast<std::int64_t>(interval) * k_ms;
    EXPECT_EQ(emit(generation, mapped(++sequence, source), source + k_ms), event_e::accepted_source) << interval;
  }
  // A short catch-up pair (1.5 T then 0.5 T) passes on the burst allowance.
  source += 25 * k_ms;
  EXPECT_EQ(emit(generation, mapped(++sequence, source), source + k_ms), event_e::accepted_source);
  source += 8 * k_ms;
  EXPECT_EQ(emit(generation, mapped(++sequence, source), source + k_ms), event_e::accepted_source);
  // A sustained 240 fps run: only about 60 fps of it is emitted, the rest is refused by name.
  int accepted = 0;
  int refused = 0;
  for (int index = 0; index < 240; ++index) {
    source += 4'166'667;
    const auto event = emit(generation, mapped(++sequence, source), source + k_ms);
    if (event == event_e::accepted_source) {
      ++accepted;
    } else {
      EXPECT_EQ(event, event_e::rate_ceiling);
      ++refused;
    }
  }
  EXPECT_LE(accepted, 60 + 2) << "one second at 60 fps plus the burst";
  EXPECT_GE(accepted, 59);
  EXPECT_EQ(accepted + refused, 240);
}

TEST(VrrSourcePacingTests, TheCeilingIsExactForRationalRatesAndSafeAtExtremes) {
  vrr_pacing::ceiling_t ntsc {{60'000, 1'001}};
  ASSERT_TRUE(ntsc.valid());
  // Empty the burst, then one frame every 1001/60000 s exactly is always allowed.
  std::int64_t at = k_base;
  ntsc.commit(at);
  ntsc.commit(at);
  EXPECT_FALSE(ntsc.allows(at + 16'683'332)) << "just under one interval";
  EXPECT_TRUE(ntsc.allows(at + 16'683'334)) << "one interval (16.683333 ms) rounded up";
  // A huge gap refills to the cap, never overflows.
  EXPECT_TRUE(ntsc.allows(std::numeric_limits<std::int64_t>::max()));
  // A time before the last commit never fits.
  EXPECT_FALSE(ntsc.allows(at - 1));
  for (const vrr_pacing::rate_t invalid : {vrr_pacing::rate_t {0, 1}, {60, 0}, {-60, 1}}) {
    vrr_pacing::ceiling_t ceiling {invalid};
    EXPECT_FALSE(ceiling.valid());
    EXPECT_FALSE(ceiling.allows(k_base));
  }
  vrr_pacing::ceiling_t overflowing {{1, std::numeric_limits<std::int64_t>::max()}};
  EXPECT_FALSE(overflowing.valid()) << "a cost that cannot be represented refuses, never wraps";
}

TEST(VrrSourcePacingTests, EmissionStaysStrictlyOrderedByAtLeastOneRtpTick) {
  vrr_pacing::generation_t generation {k_60};
  ASSERT_EQ(emit(generation, mapped(1, k_base), k_base + k_ms), event_e::accepted_source);
  EXPECT_EQ(generation.precheck_source(mapped(2, k_base), k_base + k_ms).event, event_e::regressed_vs_emitted);
  EXPECT_EQ(generation.precheck_source(mapped(2, k_base - 5 * k_ms), k_base + k_ms).event, event_e::regressed_vs_emitted);
  EXPECT_EQ(generation.precheck_source(mapped(2, k_base + vrr_pacing::k_tick_ns - 1), k_base + k_ms).event,
            event_e::tick_collision) << "two RTP timestamps could round equal";
  EXPECT_EQ(emit(generation, mapped(2, k_base + 17 * k_ms), k_base + 18 * k_ms), event_e::accepted_source);
  // A new epoch (reinit) restarts sequences but never the emission order.
  EXPECT_EQ(generation.precheck_source(mapped(1, k_base + 10 * k_ms, 8), k_base + 18 * k_ms).event,
            event_e::regressed_vs_emitted);
  EXPECT_EQ(emit(generation, mapped(1, k_base + 34 * k_ms, 8), k_base + 35 * k_ms), event_e::accepted_source);
  EXPECT_EQ(generation.precheck_source(mapped(9, k_base + 60 * k_ms, 7), k_base + 61 * k_ms).event,
            event_e::stale_or_replayed) << "an older epoch's frame after a newer one";
}

TEST(VrrSourcePacingTests, FreshnessIsJudgedAtEncodeWithTheStatedUncertainty) {
  vrr_pacing::generation_t generation {k_60};
  const std::int64_t now = k_base + 500 * k_ms;
  const std::uint64_t u = 20'000;
  EXPECT_TRUE(generation.precheck_source(mapped(1, now - vrr_pacing::k_max_source_age_ns - 20'000, 7, u), now).candidate);
  EXPECT_EQ(generation.precheck_source(mapped(1, now - vrr_pacing::k_max_source_age_ns - 20'001, 7, u), now).event,
            event_e::source_stale);
  EXPECT_TRUE(generation.precheck_source(mapped(1, now + 20'000, 7, u), now).candidate);
  EXPECT_EQ(generation.precheck_source(mapped(1, now + 20'001, 7, u), now).event, event_e::source_stale) << "future";
  EXPECT_EQ(generation.precheck_source(mapped(1, now, 7, std::numeric_limits<std::uint64_t>::max()), now).event,
            event_e::receipt_not_mapped) << "an uncertainty no mapper produces is not a receipt";
  auto unmapped = mapped(1, now);
  unmapped.receipt = vrr_clock::source_time_e::mapping_drifted;
  EXPECT_EQ(generation.precheck_source(unmapped, now).event, event_e::receipt_not_mapped);
  auto no_value = mapped(1, now);
  no_value.source_ns.reset();
  EXPECT_EQ(generation.precheck_source(no_value, now).event, event_e::receipt_not_mapped);
}

TEST(VrrSourcePacingTests, WaitsSuppressRoutineRepeatsAndLabelReadinessLivenessAndRecovery) {
  vrr_pacing::generation_t generation {k_60};
  // Nothing emitted yet: the first wait is labelled readiness, at the current time.
  auto wait = generation.on_wait(k_base, false);
  ASSERT_TRUE(wait.candidate);
  EXPECT_EQ(wait.event, event_e::readiness);
  EXPECT_EQ(wait.candidate->emit_ns, k_base);
  generation.commit(*wait.candidate, k_base);
  // Within the liveness interval: routine repeats are suppressed.
  for (const auto after : {100 * k_ms, 250 * k_ms, 499 * k_ms}) {
    const auto quiet = generation.on_wait(k_base + after, false);
    EXPECT_FALSE(quiet.candidate);
    EXPECT_EQ(quiet.event, event_e::routine_repeat_suppressed);
  }
  // At the bound: one labelled liveness repeat, at the CURRENT time (not last + tick).
  wait = generation.on_wait(k_base + 500 * k_ms, false);
  ASSERT_TRUE(wait.candidate);
  EXPECT_EQ(wait.event, event_e::liveness);
  EXPECT_EQ(wait.candidate->emit_ns, k_base + 500 * k_ms);
  generation.commit(*wait.candidate, k_base + 500 * k_ms);
  // An IDR request is a labelled recovery immediately, also at the current time.
  wait = generation.on_wait(k_base + 510 * k_ms, true);
  ASSERT_TRUE(wait.candidate);
  EXPECT_EQ(wait.event, event_e::idr_recovery);
  EXPECT_EQ(wait.candidate->emit_ns, k_base + 510 * k_ms);
  generation.commit(*wait.candidate, k_base + 510 * k_ms);
  // A valid mapped source older than the recovery is dropped by name, never moved.
  EXPECT_EQ(generation.precheck_source(mapped(1, k_base + 505 * k_ms), k_base + 512 * k_ms).event,
            event_e::regressed_vs_emitted);
}

TEST(VrrSourcePacingTests, AnIdrTheCeilingRefusesIsReportedAndRetriedNotLost) {
  vrr_pacing::generation_t generation {k_60};
  ASSERT_EQ(emit(generation, mapped(1, k_base), k_base), event_e::accepted_source);
  ASSERT_EQ(emit(generation, mapped(2, k_base + vrr_pacing::k_tick_ns), k_base + vrr_pacing::k_tick_ns),
            event_e::accepted_source);
  // Burst spent: a recovery right now does not fit; the caller keeps its IDR pending.
  const auto blocked = generation.on_wait(k_base + vrr_pacing::k_tick_ns, true);
  EXPECT_FALSE(blocked.candidate);
  EXPECT_EQ(blocked.event, event_e::rate_ceiling);
  const auto later = generation.on_wait(k_base + 17 * k_ms, true);
  ASSERT_TRUE(later.candidate);
  EXPECT_EQ(later.event, event_e::idr_recovery);
}

TEST(VrrSourcePacingTests, ALatchedGenerationNeverEmitsASourceTimeAgain) {
  vrr_pacing::generation_t generation {k_60};
  ASSERT_EQ(emit(generation, mapped(1, k_base), k_base), event_e::accepted_source);
  generation.latch();
  EXPECT_EQ(generation.mode(), mode_e::latched_fallback);
  EXPECT_EQ(generation.precheck_source(mapped(2, k_base + 20 * k_ms), k_base + 21 * k_ms).event, event_e::authority_lost);
  // Fallback emissions keep the order: a legacy time behind the last emission is moved to now.
  const auto behind = generation.fallback_emission(k_base - k_ms, k_base + 30 * k_ms);
  ASSERT_TRUE(behind.candidate);
  EXPECT_EQ(behind.candidate->emit_ns, k_base + 30 * k_ms);
  const auto ahead = generation.fallback_emission(k_base + 40 * k_ms, k_base + 30 * k_ms);
  ASSERT_TRUE(ahead.candidate);
  EXPECT_EQ(ahead.candidate->emit_ns, k_base + 40 * k_ms);
  generation.commit(*ahead.candidate, k_base + 30 * k_ms);
  EXPECT_EQ(generation.mode(), mode_e::latched_fallback) << "nothing in the core can unlatch";
}

TEST(VrrSourcePacingTests, DelayedEncoderOutputKeepsItsOwnSubmissionTime) {
  vrr_pacing::submission_times_t times;
  times.next_source_time_paced = true;
  times.put(1, 100);
  times.put(2, 200);
  times.next_source_time_paced = false;
  times.put(3, 300);
  // The encoder returns frame 2 while frame 3 is being submitted: frame 2 gets 200, not 300.
  const auto second = times.take(2);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->emit_ns, 200);
  EXPECT_TRUE(second->source_time_paced);
  EXPECT_FALSE(times.take(1)) << "older entries are discarded with it";
  const auto third = times.take(3);
  ASSERT_TRUE(third);
  EXPECT_EQ(third->emit_ns, 300);
  EXPECT_FALSE(third->source_time_paced);
  EXPECT_FALSE(times.take(4)) << "no time is invented for an index never submitted";
  for (std::int64_t index = 0; index < 1'000; ++index) times.put(index, index);
  EXPECT_EQ(times.size(), vrr_pacing::submission_times_t::k_capacity);
}

TEST(VrrSourcePacingTests, AValidBacklogJudgedAtOneActualTimeIsBoundedToTheBurst) {
  vrr_pacing::generation_t generation {k_60};
  // Eight queued 90 fps frames, all valid, monotonic and younger than 100 ms, all handed to the
  // encoder at the same actual time: the ceiling bounds work, so at most the burst of two goes.
  const std::int64_t now = k_base;
  int committed = 0;
  for (std::uint64_t index = 0; index < 8; ++index) {
    const auto source = now - 90 * k_ms + static_cast<std::int64_t>(index) * 11'111'111;
    const auto decision = generation.precheck_source(mapped(index + 1, source), now);
    if (decision.candidate) {
      EXPECT_EQ(decision.candidate->emit_ns, source) << "the source time is never rewritten";
      generation.commit(*decision.candidate, now);
      ++committed;
    } else {
      EXPECT_EQ(decision.event, event_e::rate_ceiling) << index;
    }
  }
  EXPECT_EQ(committed, 2);
  // One interval of real time later, one more fits.
  const auto later = generation.precheck_source(mapped(9, now + 5 * k_ms), now + 17 * k_ms);
  EXPECT_TRUE(later.candidate);
}

TEST(VrrSourcePacingTests, ExtremeTimesAreRefusedWithoutOverflow) {
  constexpr auto max = std::numeric_limits<std::int64_t>::max();
  constexpr auto min = std::numeric_limits<std::int64_t>::min();
  vrr_pacing::generation_t generation {k_60};
  for (const auto &[source, now] : {std::pair {min, max}, std::pair {max, std::int64_t {0}}, std::pair {std::int64_t {0}, min},
                                    std::pair {min, min}, std::pair {std::int64_t {-1}, std::int64_t {5}}}) {
    const auto decision = generation.precheck_source(mapped(1, source), now);
    EXPECT_FALSE(decision.candidate) << source << " @ " << now;
    EXPECT_EQ(decision.event, event_e::source_stale);
  }
  EXPECT_FALSE(generation.on_wait(min, false).candidate);
  EXPECT_FALSE(generation.on_wait(-1, true).candidate);
  EXPECT_FALSE(generation.fallback_emission(min, min).candidate);
  EXPECT_FALSE(generation.last_emitted_ns()) << "no refusal changed state";
  // A real time at the very end of the range still maps exactly; the next tick would not fit.
  const auto edge = generation.precheck_source(mapped(1, max - 50 * k_ms), max);
  ASSERT_TRUE(edge.candidate);
  generation.commit(*edge.candidate, max);
  // A recovery at the last representable time fits once; after it, last + tick would overflow.
  const auto recovery = generation.on_wait(max, true);
  ASSERT_TRUE(recovery.candidate);
  EXPECT_EQ(recovery.candidate->emit_ns, max);
  generation.commit(*recovery.candidate, max);
  const auto after = generation.on_wait(max, true);
  EXPECT_FALSE(after.candidate) << "last + tick overflows; refused, never wrapped";
}

TEST(VrrSourcePacingTests, AnUncertaintyNoMapperCanProduceIsNotAReceipt) {
  vrr_pacing::generation_t generation {k_60};
  const auto now = k_base;
  EXPECT_TRUE(generation.precheck_source(mapped(1, now, 7, vrr_pacing::k_max_receipt_uncertainty_ns), now).candidate);
  for (const auto uncertainty : {vrr_pacing::k_max_receipt_uncertainty_ns + 1, std::numeric_limits<std::uint64_t>::max()}) {
    const auto decision = generation.precheck_source(mapped(1, now, 7, uncertainty), now);
    EXPECT_FALSE(decision.candidate);
    EXPECT_EQ(decision.event, event_e::receipt_not_mapped);
  }
}

TEST(VrrSourcePacingTests, EveryRefusalLeavesTheGenerationUnchanged) {
  vrr_pacing::generation_t generation {k_60};
  ASSERT_EQ(emit(generation, mapped(1, k_base), k_base), event_e::accepted_source);
  const auto last = generation.last_emitted_ns();
  // Stale, future, replayed, regressed, tick collision, over-uncertain, not mapped, ceiling.
  auto unmapped = mapped(2, k_base + 20 * k_ms);
  unmapped.receipt = vrr_clock::source_time_e::stale;
  for (const auto &[frame, now] : {
         std::pair {mapped(2, k_base - 200 * k_ms), k_base + 20 * k_ms},
         std::pair {mapped(2, k_base + 30 * k_ms), k_base + 20 * k_ms},
         std::pair {mapped(1, k_base + 20 * k_ms), k_base + 21 * k_ms},
         std::pair {mapped(2, k_base - k_ms), k_base + 20 * k_ms},
         std::pair {mapped(2, k_base + 5'000), k_base + 20 * k_ms},
         std::pair {mapped(2, k_base + 20 * k_ms, 7, 200'000), k_base + 21 * k_ms},
         std::pair {unmapped, k_base + 21 * k_ms},
       }) {
    EXPECT_FALSE(generation.precheck_source(frame, now).candidate);
    EXPECT_EQ(generation.last_emitted_ns(), last);
    EXPECT_EQ(generation.mode(), mode_e::source_time);
  }
  EXPECT_EQ(emit(generation, mapped(2, k_base + 20 * k_ms), k_base + 21 * k_ms), event_e::accepted_source)
    << "the next valid frame is still accepted, as if no refusal happened";
}

TEST(VrrSourcePacingTests, SourcePacedHealthInputsAreNeutralForCadenceButKeepEncodeTime) {
  const auto unavailable = vrr_pacing::source_health_inputs(0.0, std::nullopt, 9.0, 12.5);
  EXPECT_EQ(unavailable.fps_ratio, 1.0) << "a lower game frame rate is not a delivery shortfall";
  EXPECT_EQ(unavailable.duplicate_frame_ratio, 0.0);
  EXPECT_TRUE(std::isnan(unavailable.frame_jitter_ms)) << "unavailable cadence is not a healthy zero";
  EXPECT_EQ(unavailable.encode_time_ms, 9.0);
  EXPECT_EQ(unavailable.avg_frame_age_ms, 12.5);
  const auto measured = vrr_pacing::source_health_inputs(0.01, 0.4, 12.0, 3.0);
  EXPECT_EQ(measured.frame_jitter_ms, 0.4);
  EXPECT_EQ(measured.dropped_frame_ratio, 0.01);
  EXPECT_EQ(measured.encode_time_ms, 12.0);
}
