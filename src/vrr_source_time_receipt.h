/**
 * @file src/vrr_source_time_receipt.h
 * @brief The per-frame source-time receipt's types only: its result enum, fixed names and value.
 *
 * Kept free of the mapping logic, clock proof and compositor identity so that platform/common.h can
 * hold a receipt by value without making every capture-provenance user depend on how it is produced.
 * The mapping itself lives in vrr_source_time.h.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace vrr_clock {

  /// No fresh clock read was taken for this receipt. Equal to vrr_clock::k_failed_read (asserted there).
  inline constexpr std::int64_t k_source_time_unobserved_ns = -1;

  /// The first failing gate, in evaluation order. A default receipt is not_observed, never mapped.
  enum class source_time_e {
    not_observed,  ///< no receipt was produced for this image
    mapped,  ///< every gate passed; source_steady_ns and uncertainty_ns are set
    no_presentation_clock,  ///< the route carries no ext-image-copy presentation time
    prefetched_init,  ///< captured inside (re)initialization
    missing,
    invalid_nsec,
    invalid_overflow,
    nonmonotonic,  ///< the tracker's own backward/equal verdict
    invalid_status,  ///< a status outside the enum, or a "valid" status without a usable value
    proof_unavailable,  ///< no epoch proof, another epoch's, or one whose flags contradict each other
    namespace_unverified,
    identity_unverified,
    mapping_invalid,
    clock_domain_unverified,  ///< the proof's own clock_domain_verified is false although its parts look true
    lease_not_current,  ///< the epoch's retained lease was not current at observation
    live_clock_unavailable,  ///< the fresh bracket read failed or reversed
    mapping_drifted,  ///< fresh offset or bracket outside the bounds, or total uncertainty too large
    ready_unavailable,
    before_epoch,
    future,
    stale,
    regressed_in_epoch,  ///< not after the epoch's last mapped time, including across tracker resets
    conversion_overflow,  ///< not representable in signed nanoseconds or in steady_clock's own ticks
  };

  inline std::string_view name(source_time_e value) {
    switch (value) {
      case source_time_e::not_observed:
        return "not_observed";
      case source_time_e::mapped:
        return "mapped";
      case source_time_e::no_presentation_clock:
        return "no_presentation_clock";
      case source_time_e::prefetched_init:
        return "prefetched_init";
      case source_time_e::missing:
        return "missing";
      case source_time_e::invalid_nsec:
        return "invalid_nsec";
      case source_time_e::invalid_overflow:
        return "invalid_overflow";
      case source_time_e::nonmonotonic:
        return "nonmonotonic";
      case source_time_e::invalid_status:
        return "invalid_status";
      case source_time_e::proof_unavailable:
        return "proof_unavailable";
      case source_time_e::namespace_unverified:
        return "namespace_unverified";
      case source_time_e::identity_unverified:
        return "identity_unverified";
      case source_time_e::mapping_invalid:
        return "mapping_invalid";
      case source_time_e::clock_domain_unverified:
        return "clock_domain_unverified";
      case source_time_e::lease_not_current:
        return "lease_not_current";
      case source_time_e::live_clock_unavailable:
        return "live_clock_unavailable";
      case source_time_e::mapping_drifted:
        return "mapping_drifted";
      case source_time_e::ready_unavailable:
        return "ready_unavailable";
      case source_time_e::before_epoch:
        return "before_epoch";
      case source_time_e::future:
        return "future";
      case source_time_e::stale:
        return "stale";
      case source_time_e::regressed_in_epoch:
        return "regressed_in_epoch";
      case source_time_e::conversion_overflow:
        return "conversion_overflow";
    }
    return "invalid_result";  // a value outside the enum; never a runtime string
  }

  /**
   * @brief One new source frame's immutable mapping receipt: observation evidence, not live authority.
   *
   * source_steady_ns and uncertainty_ns are set only for mapped and are absent/zero on every refusal.
   * It carries no lease, descriptor or process fact.
   */
  struct source_time_receipt_t {
    source_time_e result = source_time_e::not_observed;
    /// The presentation tracker's session the sample came from (0 when the backend has none).
    std::uint64_t presentation_baseline = 0;
    /// mapped only: the presentation time on std::chrono::steady_clock, in nanoseconds since its epoch.
    std::optional<std::int64_t> source_steady_ns;
    /// mapped only: bound on |source_steady_ns − true steady time of the presentation|.
    std::uint64_t uncertainty_ns = 0;
    /// The steady read of the fresh bracket (the observation moment); k_source_time_unobserved_ns when
    /// none was taken.
    std::int64_t observed_steady_ns = k_source_time_unobserved_ns;

    bool operator==(const source_time_receipt_t &) const = default;
  };

}  // namespace vrr_clock
