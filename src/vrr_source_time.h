/**
 * @file src/vrr_source_time.h
 * @brief Map one new source frame's compositor presentation time onto this process's steady clock.
 *
 * Pure and injectable, like vrr_clock_proof.h. The result is a per-frame receipt: what was true when
 * the capture thread observed the frame, never live authority. A later reader must not treat a stored
 * `mapped` receipt as permission for anything; nothing in this slice paces, schedules or encodes by
 * it, and it never replaces a frame's pacing timestamp.
 *
 * Every gate fails closed and names itself. The epoch proof is historical evidence collected when the
 * display opened; on its own it no longer authorises a frame. The epoch's own retained lease must also
 * be current at the moment of observation, and a fresh bracketed clock read must agree with the
 * epoch's mapping, before a presentation time is converted.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <ratio>
#include <string_view>
#include <type_traits>
#include <utility>

#include "vrr_clock_proof.h"
#include "vrr_compositor_identity.h"
#include "vrr_source_time_receipt.h"

namespace vrr_clock {

  static_assert(k_source_time_unobserved_ns == k_failed_read,
                "a receipt without a fresh read uses the clock proof's failed-read sentinel");

  struct source_time_limits_t {
    std::uint64_t staleness_ns = 100'000'000;  ///< ready − presentation beyond this (plus uncertainty) is stale
    std::uint64_t max_live_bracket_ns = 20'000;
    std::uint64_t max_live_drift_ns = 50'000;  ///< |fresh steady−mono offset − epoch offset|
    std::uint64_t max_total_uncertainty_ns = 150'000;
  };

  /// One capture epoch's mutable mapping state. Owned by the capture thread's epoch only.
  struct epoch_source_state_t {
    /// The last presentation time this epoch mapped; later frames must be strictly after it.
    std::optional<std::int64_t> last_mapped_presentation_ns;
    std::uint64_t last_mapped_baseline = 0;
    /// Diagnostic: tracker sessions seen to change between mapped frames. Saturates, never wraps.
    std::uint64_t baseline_changes = 0;
  };

  /// What the producer knows about one newly captured frame.
  struct source_time_input_t {
    bool route_has_presentation_clock = false;
    std::uint64_t capture_epoch_id = 0;
    vrr_policy::presentation_status_e status = vrr_policy::presentation_status_e::missing;
    std::optional<std::int64_t> presentation_ns;
    std::uint64_t presentation_baseline = 0;
    bool prefetched_init = false;
    std::optional<std::chrono::steady_clock::time_point> ready_at;
  };

  /**
   * @brief value × factor for a positive factor, or nullopt when the product leaves int64.
   *
   * Bounds by division before multiplying, so no intermediate overflows and no compiler builtin is
   * needed. Integer division truncates toward zero, so max/factor and min/factor are exactly the
   * largest and smallest values whose product still fits.
   */
  inline std::optional<std::int64_t> checked_mul_positive(std::int64_t value, std::int64_t factor) {
    if (factor <= 0) return std::nullopt;
    if (value > k_int64_max / factor || value < k_int64_min / factor) return std::nullopt;
    return value * factor;
  }

  /// value ÷ divisor for a positive divisor, only when it divides exactly (no truncation).
  inline std::optional<std::int64_t> exact_div_positive(std::int64_t value, std::int64_t divisor) {
    if (divisor <= 0 || value % divisor != 0) return std::nullopt;
    return value / divisor;
  }

  /// value × num ÷ den for a reduced positive ratio, exact and within int64, else nullopt.
  template<class Ratio>
  std::optional<std::int64_t> exact_scale(std::int64_t value) {
    static_assert(Ratio::num > 0 && Ratio::den > 0, "clock periods are positive");
    static_assert(Ratio::num <= k_int64_max && Ratio::den <= k_int64_max, "ratio terms fit int64");
    const auto scaled = checked_mul_positive(value, static_cast<std::int64_t>(Ratio::num));
    if (!scaled) return std::nullopt;
    if constexpr (Ratio::den == 1) {
      return scaled;
    } else {
      return exact_div_positive(*scaled, static_cast<std::int64_t>(Ratio::den));
    }
  }

  /**
   * @brief A duration's exact count of nanoseconds, or nullopt when not exactly representable.
   *
   * Works for any integral tick type and period, signed or unsigned, coarse or fine (Linux and macOS
   * steady clocks differ in principle), so a clock never silently truncates, wraps or overflows.
   */
  template<class Rep, class Period>
  std::optional<std::int64_t> exact_ns(std::chrono::duration<Rep, Period> value) {
    static_assert(std::is_integral_v<Rep> && !std::is_same_v<Rep, bool>, "only integral clock ticks are mapped");
    if (!std::in_range<std::int64_t>(value.count())) return std::nullopt;
    return exact_scale<std::ratio_divide<Period, std::nano>>(static_cast<std::int64_t>(value.count()));
  }

  /// Whether ns is exactly a whole number of ticks of Duration, within its tick type's range.
  template<class Duration>
  bool representable_as(std::int64_t ns) {
    using rep = typename Duration::rep;
    static_assert(std::is_integral_v<rep> && !std::is_same_v<rep, bool>, "only integral clock ticks are mapped");
    const auto ticks = exact_scale<std::ratio_divide<std::nano, typename Duration::period>>(ns);
    return ticks && std::in_range<rep>(*ticks);
  }

  /**
   * @brief Map one newly captured frame, or name the first gate that refuses it.
   *
   * Order: route, prefetched, tracker syntax, value, historical proof gates (each separately, the
   * proof's own clock_domain_verified, then the full authorises_clock_comparison), the
   * epoch's lease current() exactly once, one fresh bracketed clock read and its bounds, ready time,
   * epoch/future/stale plausibility, epoch regression, checked conversion. state changes only when
   * the result is mapped.
   */
  inline source_time_receipt_t map_source_time(const source_time_input_t &input,
                                               const proof_t *proof,
                                               const compositor_lease_t *lease,
                                               system_t &clocks,
                                               epoch_source_state_t &state,
                                               const source_time_limits_t &limits) {
    using status_e = vrr_policy::presentation_status_e;
    source_time_receipt_t receipt;
    receipt.presentation_baseline = input.presentation_baseline;
    const auto refuse = [&receipt](source_time_e result) {
      receipt.result = result;
      receipt.source_steady_ns.reset();
      receipt.uncertainty_ns = 0;
      return receipt;
    };

    if (!input.route_has_presentation_clock) return refuse(source_time_e::no_presentation_clock);
    if (input.prefetched_init) return refuse(source_time_e::prefetched_init);
    switch (input.status) {
      case status_e::valid:
        break;
      case status_e::missing:
        return refuse(source_time_e::missing);
      case status_e::invalid_nsec:
        return refuse(source_time_e::invalid_nsec);
      case status_e::invalid_overflow:
        return refuse(source_time_e::invalid_overflow);
      case status_e::nonmonotonic:
        return refuse(source_time_e::nonmonotonic);
      default:
        return refuse(source_time_e::invalid_status);
    }
    if (!input.presentation_ns || *input.presentation_ns < 0) return refuse(source_time_e::invalid_status);
    const std::int64_t present = *input.presentation_ns;

    if (!proof || proof->capture_epoch_id == 0 || proof->capture_epoch_id != input.capture_epoch_id) {
      return refuse(source_time_e::proof_unavailable);
    }
    if (proof->time_namespace != namespace_e::same) return refuse(source_time_e::namespace_unverified);
    if (!proof->producer_identity_verified) return refuse(source_time_e::identity_unverified);
    if (!proof->mapping.valid || proof->epoch_start_mono_ns < 0) return refuse(source_time_e::mapping_invalid);
    // Contradictory flags never combine into success: the proof's own verdict must agree with its parts.
    if (!proof->clock_domain_verified) return refuse(source_time_e::clock_domain_unverified);
    if (!authorises_clock_comparison(*proof)) return refuse(source_time_e::proof_unavailable);

    // The historical proof is not live authority: the epoch's own lease must be current now.
    if (!lease || !lease->current()) return refuse(source_time_e::lease_not_current);

    const auto live = clocks.sample_clocks();
    if (live.mono_before_ns < 0 || live.steady_ns < 0 || live.mono_after_ns < 0) {
      return refuse(source_time_e::live_clock_unavailable);
    }
    receipt.observed_steady_ns = live.steady_ns;
    std::int64_t bracket = 0;
    if (!checked_sub(live.mono_after_ns, live.mono_before_ns, bracket) || bracket < 0) {
      return refuse(source_time_e::live_clock_unavailable);
    }
    // Both reads are nonnegative, so before + bracket / 2 cannot overflow.
    const std::int64_t mid = live.mono_before_ns + bracket / 2;
    std::int64_t live_offset = 0;
    std::int64_t drift = 0;
    if (!checked_sub(live.steady_ns, mid, live_offset) ||
        !checked_sub(live_offset, proof->mapping.steady_to_mono_offset_ns, drift)) {
      return refuse(source_time_e::mapping_drifted);
    }
    const auto drift_ns = magnitude(drift);
    const auto bracket_ns = static_cast<std::uint64_t>(bracket);
    std::uint64_t uncertainty = 0;
    if (bracket_ns > limits.max_live_bracket_ns || drift_ns > limits.max_live_drift_ns ||
        !checked_add_unsigned(proof->mapping.uncertainty_ns, drift_ns, uncertainty) ||
        !checked_add_unsigned(uncertainty, bracket_ns, uncertainty) ||
        uncertainty > limits.max_total_uncertainty_ns ||
        uncertainty > static_cast<std::uint64_t>(k_int64_max) ||
        limits.staleness_ns > static_cast<std::uint64_t>(k_int64_max)) {
      return refuse(source_time_e::mapping_drifted);
    }
    const auto u = static_cast<std::int64_t>(uncertainty);
    const auto offset = proof->mapping.steady_to_mono_offset_ns;

    if (!input.ready_at) return refuse(source_time_e::ready_unavailable);
    const auto ready_steady = exact_ns(input.ready_at->time_since_epoch());
    std::int64_t ready_mono = 0;
    if (!ready_steady || *ready_steady < 0 || !checked_sub(*ready_steady, offset, ready_mono) || ready_mono < 0) {
      return refuse(source_time_e::conversion_overflow);
    }

    // The epoch's original start, not the fresh bracket: a frame from before this epoch is never its own.
    if (present < proof->epoch_start_mono_ns) return refuse(source_time_e::before_epoch);
    std::int64_t latest = 0;
    if (!checked_add(ready_mono, u, latest)) return refuse(source_time_e::conversion_overflow);
    if (present > latest) return refuse(source_time_e::future);
    std::int64_t age = 0;
    std::int64_t allowed = 0;
    if (!checked_sub(ready_mono, present, age) ||
        !checked_add(static_cast<std::int64_t>(limits.staleness_ns), u, allowed)) {
      return refuse(source_time_e::conversion_overflow);
    }
    if (age > allowed) return refuse(source_time_e::stale);

    if (state.last_mapped_presentation_ns && present <= *state.last_mapped_presentation_ns) {
      return refuse(source_time_e::regressed_in_epoch);
    }

    // mono → steady is P + (steady − mono), never minus.
    std::int64_t source_steady = 0;
    std::int64_t observed_bound = 0;
    if (!checked_add(present, offset, source_steady) || source_steady < 0 ||
        !checked_add(live.steady_ns, u, observed_bound) || source_steady > observed_bound ||
        !representable_as<std::chrono::steady_clock::duration>(source_steady)) {
      return refuse(source_time_e::conversion_overflow);
    }

    receipt.result = source_time_e::mapped;
    receipt.source_steady_ns = source_steady;
    receipt.uncertainty_ns = uncertainty;
    if (state.last_mapped_presentation_ns && state.last_mapped_baseline != input.presentation_baseline &&
        state.baseline_changes != std::numeric_limits<std::uint64_t>::max()) {
      ++state.baseline_changes;
    }
    state.last_mapped_presentation_ns = present;
    state.last_mapped_baseline = input.presentation_baseline;
    return receipt;
  }

}  // namespace vrr_clock
