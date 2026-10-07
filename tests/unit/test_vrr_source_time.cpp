/**
 * @file tests/unit/test_vrr_source_time.cpp
 * @brief Per-frame source-time mapping: every gate fails closed, names itself and writes nothing.
 */
#include <src/vrr_source_time.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

using vrr_clock::source_time_e;
using status_e = vrr_policy::presentation_status_e;

namespace {
  constexpr std::int64_t k_offset = 1'000;  ///< steady − mono for these fixtures
  constexpr std::int64_t k_epoch_start = 5'000'000'000;
  constexpr std::int64_t k_now_mono = 6'000'000'000;
  constexpr std::uint64_t k_epoch_uncertainty = 1'000;

  /// Clocks whose next bracket the test sets; counts reads.
  class fake_clocks_t final: public vrr_clock::system_t {
  public:
    vrr_clock::probe_sample_t next {k_now_mono, k_now_mono + 50 + k_offset, k_now_mono + 100};  ///< bracket 100, no drift
    std::size_t samples = 0;

    std::optional<peer_cred_t> peer_credentials(int) override {
      return std::nullopt;
    }

    std::optional<vrr_clock::ns_id_t> time_namespace(std::int64_t) override {
      return std::nullopt;
    }

    std::optional<std::uint64_t> process_starttime(std::int64_t) override {
      return std::nullopt;
    }

    std::uint32_t current_uid() override {
      return 0;
    }

    vrr_clock::probe_sample_t sample_clocks() override {
      ++samples;
      return next;
    }

    std::int64_t monotonic_now_ns() override {
      return next.mono_before_ns;
    }
  };

  class fake_lease_t final: public vrr_clock::compositor_lease_t {
  public:
    fake_lease_t(): compositor_lease_t({4242, 1000, 77}) {}

    mutable bool is_current = true;
    mutable std::size_t checks = 0;

    bool current() const override {
      ++checks;
      return is_current;
    }
  };

  vrr_clock::proof_t verified_proof(std::uint64_t epoch = 7) {
    vrr_clock::proof_t proof;
    proof.capture_epoch_id = epoch;
    proof.time_namespace = vrr_clock::namespace_e::same;
    proof.namespace_reason = "same_dev_and_inode";
    proof.producer_identity_verified = true;
    proof.producer_identity_reason = "authoritative_compositor_peer_matched";
    proof.mapping.valid = true;
    proof.mapping.reason = "bounded";
    proof.mapping.steady_to_mono_offset_ns = k_offset;
    proof.mapping.uncertainty_ns = k_epoch_uncertainty;
    proof.epoch_start_mono_ns = k_epoch_start;
    proof.clock_domain_verified = true;
    return proof;
  }

  std::chrono::steady_clock::time_point steady_at_ns(std::int64_t ns) {
    return std::chrono::steady_clock::time_point {
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds {ns})
    };
  }

  /// A new frame presented at present_mono, ready at k_now_mono (mono), in epoch 7.
  vrr_clock::source_time_input_t frame(std::int64_t present_mono = k_now_mono - 1'000'000, std::uint64_t baseline = 1) {
    vrr_clock::source_time_input_t input;
    input.route_has_presentation_clock = true;
    input.capture_epoch_id = 7;
    input.status = status_e::valid;
    input.presentation_ns = present_mono;
    input.presentation_baseline = baseline;
    input.ready_at = steady_at_ns(k_now_mono + k_offset);
    return input;
  }

  struct harness_t {
    vrr_clock::proof_t proof = verified_proof();
    fake_lease_t lease;
    fake_clocks_t clocks;
    vrr_clock::epoch_source_state_t state;
    vrr_clock::source_time_limits_t limits;

    vrr_clock::source_time_receipt_t map(const vrr_clock::source_time_input_t &input) {
      return vrr_clock::map_source_time(input, &proof, &lease, clocks, state, limits);
    }
  };

  void expect_refused(const vrr_clock::source_time_receipt_t &receipt, source_time_e result) {
    EXPECT_EQ(receipt.result, result) << vrr_clock::name(receipt.result);
    EXPECT_FALSE(receipt.source_steady_ns) << "a refusal never carries a mapped value";
    EXPECT_EQ(receipt.uncertainty_ns, 0u) << "a refusal never carries an uncertainty";
  }
}  // namespace

TEST(VrrSourceTimeTests, ADefaultReceiptIsNeverAMappedObservation) {
  const vrr_clock::source_time_receipt_t receipt;
  EXPECT_EQ(receipt.result, source_time_e::not_observed);
  EXPECT_EQ(vrr_clock::name(receipt.result), "not_observed");
  EXPECT_FALSE(receipt.source_steady_ns);
  EXPECT_EQ(receipt.uncertainty_ns, 0u);
  EXPECT_EQ(receipt.observed_steady_ns, vrr_clock::k_failed_read);
  EXPECT_EQ(vrr_clock::name(static_cast<source_time_e>(250)), "invalid_result");
}

TEST(VrrSourceTimeTests, AVerifiedCurrentFrameMapsAsPresentationPlusOffset) {
  harness_t h;
  const auto input = frame();
  const auto receipt = h.map(input);
  ASSERT_EQ(receipt.result, source_time_e::mapped) << vrr_clock::name(receipt.result);
  ASSERT_TRUE(receipt.source_steady_ns);
  EXPECT_EQ(*receipt.source_steady_ns, *input.presentation_ns + k_offset) << "mono → steady is P + offset, never minus";
  EXPECT_EQ(receipt.uncertainty_ns, k_epoch_uncertainty + 0 + 100) << "epoch U + drift + fresh bracket";
  EXPECT_EQ(receipt.observed_steady_ns, h.clocks.next.steady_ns);
  EXPECT_EQ(receipt.presentation_baseline, 1u);
  EXPECT_EQ(h.lease.checks, 1u) << "exactly one live lease check at observation";
  EXPECT_EQ(h.clocks.samples, 1u) << "exactly one fresh bracket";
  EXPECT_EQ(h.state.last_mapped_presentation_ns, input.presentation_ns);
}

TEST(VrrSourceTimeTests, GatesBeforeTheProofNeverTouchTheLeaseOrClocks) {
  struct case_t {
    vrr_clock::source_time_input_t input;
    source_time_e expected;
  };
  std::vector<case_t> cases;
  auto add = [&](source_time_e expected, auto &&edit) {
    auto input = frame();
    edit(input);
    cases.push_back({input, expected});
  };
  add(source_time_e::no_presentation_clock, [](auto &input) { input.route_has_presentation_clock = false; });
  add(source_time_e::prefetched_init, [](auto &input) { input.prefetched_init = true; });
  add(source_time_e::missing, [](auto &input) { input.status = status_e::missing; });
  add(source_time_e::invalid_nsec, [](auto &input) { input.status = status_e::invalid_nsec; });
  add(source_time_e::invalid_overflow, [](auto &input) { input.status = status_e::invalid_overflow; });
  add(source_time_e::nonmonotonic, [](auto &input) { input.status = status_e::nonmonotonic; });
  add(source_time_e::invalid_status, [](auto &input) { input.status = static_cast<status_e>(99); });
  add(source_time_e::invalid_status, [](auto &input) { input.presentation_ns.reset(); });
  add(source_time_e::invalid_status, [](auto &input) { input.presentation_ns = -1; });
  add(source_time_e::proof_unavailable, [](auto &input) { input.capture_epoch_id = 8; });
  for (const auto &item : cases) {
    harness_t h;
    expect_refused(h.map(item.input), item.expected);
    EXPECT_EQ(h.lease.checks, 0u) << vrr_clock::name(item.expected);
    EXPECT_EQ(h.clocks.samples, 0u) << vrr_clock::name(item.expected);
    EXPECT_FALSE(h.state.last_mapped_presentation_ns);
  }
}

TEST(VrrSourceTimeTests, EachHistoricalProofGateIsSeparateAndContradictionsNeverSucceed) {
  const auto refused_by = [](auto &&edit) {
    harness_t h;
    edit(h.proof);
    const auto receipt = h.map(frame());
    EXPECT_EQ(h.lease.checks, 0u) << "no live check for a proof that already refuses";
    EXPECT_FALSE(receipt.source_steady_ns);
    return receipt.result;
  };
  EXPECT_EQ(refused_by([](auto &proof) { proof.capture_epoch_id = 0; }), source_time_e::proof_unavailable);
  EXPECT_EQ(refused_by([](auto &proof) { proof.time_namespace = vrr_clock::namespace_e::different; }),
            source_time_e::namespace_unverified);
  EXPECT_EQ(refused_by([](auto &proof) { proof.time_namespace = vrr_clock::namespace_e::unverifiable; }),
            source_time_e::namespace_unverified);
  EXPECT_EQ(refused_by([](auto &proof) { proof.producer_identity_verified = false; }), source_time_e::identity_unverified);
  EXPECT_EQ(refused_by([](auto &proof) { proof.mapping.valid = false; }), source_time_e::mapping_invalid);
  EXPECT_EQ(refused_by([](auto &proof) { proof.epoch_start_mono_ns = vrr_clock::k_failed_read; }),
            source_time_e::mapping_invalid);
  // Every part looks true, but the proof's own verdict says no: never combined into success.
  EXPECT_EQ(refused_by([](auto &proof) { proof.clock_domain_verified = false; }), source_time_e::clock_domain_unverified);
  // The opposite contradiction: a verdict claiming yes over a failing part is refused by that part.
  EXPECT_EQ(refused_by([](auto &proof) { proof.producer_identity_verified = false; proof.clock_domain_verified = true; }),
            source_time_e::identity_unverified);

  harness_t h;
  expect_refused(vrr_clock::map_source_time(frame(), nullptr, &h.lease, h.clocks, h.state, h.limits),
                 source_time_e::proof_unavailable);
}

TEST(VrrSourceTimeTests, AHistoricalProofIsNotLiveAuthorityOnceTheLeaseRetires) {
  harness_t h;
  ASSERT_EQ(h.map(frame(k_now_mono - 3'000'000)).result, source_time_e::mapped);
  // The same epoch, the same immutable proof: its generation is retired before the next frame.
  h.lease.is_current = false;
  const auto later = frame(k_now_mono - 2'000'000);
  const auto receipt = h.map(later);
  expect_refused(receipt, source_time_e::lease_not_current);
  EXPECT_EQ(h.clocks.samples, 1u) << "no clock read once the lease refuses";
  // The previous path judged this frame from the historical proof alone, and still calls it valid.
  EXPECT_TRUE(vrr_clock::authorises_clock_comparison(h.proof));
  EXPECT_EQ(vrr_clock::classify(later.status, later.presentation_ns, false, later.ready_at, &h.proof, {}),
            vrr_clock::sample_e::valid);
  EXPECT_EQ(h.state.last_mapped_presentation_ns, k_now_mono - 3'000'000) << "a refusal changes no epoch state";

  harness_t missing;
  expect_refused(vrr_clock::map_source_time(frame(), &missing.proof, nullptr, missing.clocks, missing.state, missing.limits),
                 source_time_e::lease_not_current);
}

TEST(VrrSourceTimeTests, FreshClockReadsMustBeReadableOrderedAndAgreeWithTheEpochMapping) {
  const auto with = [](vrr_clock::probe_sample_t sample) {
    harness_t h;
    h.clocks.next = sample;
    return h.map(frame());
  };
  expect_refused(with({vrr_clock::k_failed_read, k_now_mono, k_now_mono}), source_time_e::live_clock_unavailable);
  expect_refused(with({k_now_mono, vrr_clock::k_failed_read, k_now_mono}), source_time_e::live_clock_unavailable);
  expect_refused(with({k_now_mono + 100, k_now_mono + k_offset, k_now_mono}), source_time_e::live_clock_unavailable);
  // Drift exactly at the bound maps; one nanosecond more does not.
  EXPECT_EQ(with({k_now_mono, k_now_mono + k_offset + 50'000, k_now_mono}).result, source_time_e::mapped);
  expect_refused(with({k_now_mono, k_now_mono + k_offset + 50'001, k_now_mono}), source_time_e::mapping_drifted);
  expect_refused(with({k_now_mono, k_now_mono + k_offset - 50'001, k_now_mono}), source_time_e::mapping_drifted);
  // A bracket wider than the bound.
  expect_refused(with({k_now_mono, k_now_mono + 10'000 + k_offset, k_now_mono + 20'002}), source_time_e::mapping_drifted);
  // Parts within their bounds, total beyond it.
  harness_t h;
  h.proof.mapping.uncertainty_ns = 100'000;
  h.clocks.next = {k_now_mono, k_now_mono + k_offset + 50'000, k_now_mono + 1};
  expect_refused(h.map(frame()), source_time_e::mapping_drifted);
  harness_t huge;
  huge.proof.mapping.uncertainty_ns = std::numeric_limits<std::uint64_t>::max();
  expect_refused(huge.map(frame()), source_time_e::mapping_drifted);
  EXPECT_FALSE(huge.state.last_mapped_presentation_ns);
}

TEST(VrrSourceTimeTests, PlausibilityUsesTheEpochStartAndReadyTimeWithTheFrameUncertainty) {
  constexpr std::int64_t u = static_cast<std::int64_t>(k_epoch_uncertainty) + 100;
  const auto at = [](std::int64_t present) {
    harness_t h;
    return h.map(frame(present));
  };
  {
    harness_t h;
    auto input = frame();
    input.ready_at.reset();
    expect_refused(h.map(input), source_time_e::ready_unavailable);
  }
  expect_refused(at(k_epoch_start - 1), source_time_e::before_epoch);
  // Exactly the epoch start is inside the epoch, but far older than the staleness bound here.
  expect_refused(at(k_epoch_start), source_time_e::stale);
  EXPECT_EQ(at(k_now_mono + u).result, source_time_e::mapped);
  expect_refused(at(k_now_mono + u + 1), source_time_e::future);
  EXPECT_EQ(at(k_now_mono - 100'000'000 - u).result, source_time_e::mapped);
  expect_refused(at(k_now_mono - 100'000'000 - u - 1), source_time_e::stale);
}

TEST(VrrSourceTimeTests, AnEpochNeverMapsAnEqualOrEarlierTimeEvenAcrossTrackerResets) {
  harness_t h;
  ASSERT_EQ(h.map(frame(k_now_mono - 2'000'000, 1)).result, source_time_e::mapped);
  expect_refused(h.map(frame(k_now_mono - 2'000'000, 1)), source_time_e::regressed_in_epoch);
  // The tracker was reset (new session) and so reports a backward time as valid. The epoch does not.
  const auto across = frame(k_now_mono - 2'500'000, 2);
  expect_refused(h.map(across), source_time_e::regressed_in_epoch);
  EXPECT_EQ(vrr_clock::classify(across.status, across.presentation_ns, false, across.ready_at, &h.proof, {}),
            vrr_clock::sample_e::valid)
    << "the previous path accepts it";
  EXPECT_EQ(h.state.last_mapped_presentation_ns, k_now_mono - 2'000'000);
  EXPECT_EQ(h.state.baseline_changes, 0u) << "refusals never count";
  ASSERT_EQ(h.map(frame(k_now_mono - 1'000'000, 2)).result, source_time_e::mapped);
  EXPECT_EQ(h.state.baseline_changes, 1u);
  EXPECT_EQ(h.state.last_mapped_baseline, 2u);

  vrr_clock::epoch_source_state_t saturated;
  saturated.last_mapped_presentation_ns = 1;
  saturated.last_mapped_baseline = 1;
  saturated.baseline_changes = std::numeric_limits<std::uint64_t>::max();
  harness_t s;
  s.state = saturated;
  ASSERT_EQ(s.map(frame(k_now_mono - 1'000'000, 3)).result, source_time_e::mapped);
  EXPECT_EQ(s.state.baseline_changes, std::numeric_limits<std::uint64_t>::max()) << "saturates, never wraps";
}

TEST(VrrSourceTimeTests, ConversionIsCheckedAndExact) {
  {
    // ready − offset overflows: refused, not wrapped.
    harness_t h;
    h.proof.mapping.steady_to_mono_offset_ns = -100;
    h.clocks.next = {1'000, 900, 1'000};
    auto input = frame(k_now_mono);
    input.ready_at = steady_at_ns(std::numeric_limits<std::int64_t>::max() - 10);
    expect_refused(h.map(input), source_time_e::conversion_overflow);
  }
  // Exact nanosecond counts from any integral tick period.
  using pico = std::chrono::duration<std::int64_t, std::pico>;
  using micro = std::chrono::duration<std::int64_t, std::micro>;
  EXPECT_EQ(vrr_clock::exact_ns(pico {2'000}), 2);
  EXPECT_FALSE(vrr_clock::exact_ns(pico {1'500})) << "a fraction of a nanosecond is not exact";
  EXPECT_EQ(vrr_clock::exact_ns(micro {3}), 3'000);
  EXPECT_FALSE(vrr_clock::exact_ns(micro {std::numeric_limits<std::int64_t>::max() / 100}));
  EXPECT_EQ(vrr_clock::exact_ns(std::chrono::nanoseconds {-5}), -5);
  EXPECT_TRUE(vrr_clock::representable_as<micro>(2'000));
  EXPECT_FALSE(vrr_clock::representable_as<micro>(1'500));
  EXPECT_TRUE(vrr_clock::representable_as<pico>(1'000));
  EXPECT_FALSE(vrr_clock::representable_as<pico>(std::numeric_limits<std::int64_t>::max() / 10));
  EXPECT_FALSE((vrr_clock::representable_as<std::chrono::duration<std::int32_t, std::nano>>(std::int64_t {1} << 40)));
  EXPECT_TRUE(vrr_clock::representable_as<std::chrono::steady_clock::duration>(123'456'789));
}

TEST(VrrSourceTimeTests, CheckedScalingBoundsByDivisionAndNeverTruncates) {
  constexpr auto max = std::numeric_limits<std::int64_t>::max();
  constexpr auto min = std::numeric_limits<std::int64_t>::min();
  EXPECT_EQ(vrr_clock::checked_mul_positive(max / 3, 3), (max / 3) * 3);
  EXPECT_FALSE(vrr_clock::checked_mul_positive(max / 3 + 1, 3));
  EXPECT_EQ(vrr_clock::checked_mul_positive(min / 3, 3), (min / 3) * 3);
  EXPECT_FALSE(vrr_clock::checked_mul_positive(min / 3 - 1, 3));
  EXPECT_EQ(vrr_clock::checked_mul_positive(min, 1), min);
  EXPECT_FALSE(vrr_clock::checked_mul_positive(min, 2));
  EXPECT_FALSE(vrr_clock::checked_mul_positive(5, 0)) << "only positive factors";
  EXPECT_FALSE(vrr_clock::checked_mul_positive(5, -1));
  EXPECT_EQ(vrr_clock::exact_div_positive(-6, 3), -2);
  EXPECT_FALSE(vrr_clock::exact_div_positive(-7, 3)) << "no truncation toward zero";
  EXPECT_FALSE(vrr_clock::exact_div_positive(6, 0));
}

TEST(VrrSourceTimeTests, ConversionCoversUnsignedCoarseFineAndNegativeTicks) {
  using unsigned_ns = std::chrono::duration<std::uint64_t, std::nano>;
  using unsigned32_ns = std::chrono::duration<std::uint32_t, std::nano>;
  using coarse = std::chrono::duration<std::int64_t>;  // seconds
  using fine = std::chrono::duration<std::int64_t, std::pico>;
  using micro = std::chrono::duration<std::int64_t, std::micro>;
  constexpr auto max = std::numeric_limits<std::int64_t>::max();

  // Unsigned ticks: in range converts exactly; above int64 is refused, never wrapped negative.
  EXPECT_EQ(vrr_clock::exact_ns(unsigned_ns {42}), 42);
  EXPECT_EQ(vrr_clock::exact_ns(unsigned_ns {static_cast<std::uint64_t>(max)}), max);
  EXPECT_FALSE(vrr_clock::exact_ns(unsigned_ns {static_cast<std::uint64_t>(max) + 1}));
  EXPECT_FALSE(vrr_clock::exact_ns(unsigned_ns {std::numeric_limits<std::uint64_t>::max()}));
  EXPECT_TRUE(vrr_clock::representable_as<unsigned_ns>(0));
  EXPECT_FALSE(vrr_clock::representable_as<unsigned_ns>(-1)) << "a negative time has no unsigned tick";
  EXPECT_TRUE(vrr_clock::representable_as<unsigned32_ns>(std::numeric_limits<std::uint32_t>::max()));
  EXPECT_FALSE(vrr_clock::representable_as<unsigned32_ns>(std::int64_t {std::numeric_limits<std::uint32_t>::max()} + 1));

  // Coarse ticks: multiply with an overflow bound; nanoseconds must be whole ticks.
  EXPECT_EQ(vrr_clock::exact_ns(coarse {3}), 3'000'000'000);
  EXPECT_EQ(vrr_clock::exact_ns(coarse {-3}), -3'000'000'000);
  EXPECT_EQ(vrr_clock::exact_ns(coarse {max / 1'000'000'000}), (max / 1'000'000'000) * 1'000'000'000);
  EXPECT_FALSE(vrr_clock::exact_ns(coarse {max / 1'000'000'000 + 1}));
  EXPECT_TRUE(vrr_clock::representable_as<coarse>(2'000'000'000));
  EXPECT_FALSE(vrr_clock::representable_as<coarse>(2'500'000'000)) << "half a second is not a whole tick";
  EXPECT_TRUE(vrr_clock::representable_as<coarse>(-2'000'000'000));

  // Fine ticks: divide only when exact.
  EXPECT_EQ(vrr_clock::exact_ns(fine {-2'000}), -2);
  EXPECT_FALSE(vrr_clock::exact_ns(fine {-1'500}));
  EXPECT_TRUE(vrr_clock::representable_as<fine>(-1'000));
  EXPECT_FALSE(vrr_clock::representable_as<fine>(max / 1'000 + 1));

  // Negative counts on a coarser-than-ns signed clock.
  EXPECT_EQ(vrr_clock::exact_ns(micro {-3}), -3'000);
  EXPECT_TRUE(vrr_clock::representable_as<micro>(-2'000));
  EXPECT_FALSE(vrr_clock::representable_as<micro>(-1'500));
}

TEST(VrrSourceTimeTests, EveryRefusalLeavesEpochStateUntouched) {
  harness_t h;
  ASSERT_EQ(h.map(frame(k_now_mono - 3'000'000)).result, source_time_e::mapped);
  const auto before = h.state.last_mapped_presentation_ns;
  auto prefetched = frame(k_now_mono - 1'000'000);
  prefetched.prefetched_init = true;
  expect_refused(h.map(prefetched), source_time_e::prefetched_init);
  expect_refused(h.map(frame(k_now_mono + 1'000'000)), source_time_e::future);
  h.lease.is_current = false;
  expect_refused(h.map(frame(k_now_mono - 1'000'000)), source_time_e::lease_not_current);
  EXPECT_EQ(h.state.last_mapped_presentation_ns, before);
  h.lease.is_current = true;
  EXPECT_EQ(h.map(frame(k_now_mono - 1'000'000)).result, source_time_e::mapped)
    << "a prefetched or refused time never became the baseline";
}
