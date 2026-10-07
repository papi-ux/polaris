/**
 * @file src/vrr_clock_proof.h
 * @brief Evidence about whether a compositor's presentation times are in this process's clock.
 *
 * Pure, portable and injectable: every fact comes through system_t, so the rules are testable without
 * a compositor. The proof is diagnostic evidence only. Nothing here changes how a frame is stamped,
 * paced or encoded, and a proof never makes a session effective on its own.
 *
 * ext-image-copy-capture-v1 (wayland-protocols 0091197f) sends presentation_time "in system monotonic
 * time" before ready. A process in another time namespace sees a different CLOCK_MONOTONIC, so the
 * value is only comparable when the compositor shares our time namespace, and only then when we know
 * the peer really is the compositor we started.
 *
 * Limits, stated rather than guessed:
 * - SO_PEERCRED on the connected Wayland socket gives socket-peer facts only: the process that created
 *   the listening socket, not necessarily the compositor Polaris supervises. Only a retained lease of
 *   the owned private runtime generation makes those same peer facts authoritative; without one, or
 *   when it retires during collection, producer_identity_verified stays false, and so does
 *   clock_domain_verified, even when the namespaces match.
 * - A shared namespace says the clocks agree, not that the compositor reports truthful times; the
 *   per-frame plausibility in classify() is still required.
 * - The steady/CLOCK_MONOTONIC mapping is measured once per capture epoch and bounds only that moment.
 * - A failed clock read is reported as a negative sentinel and is never treated as time zero.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "vrr_compositor_identity.h"
#include "vrr_session_policy.h"

namespace vrr_clock {

  inline constexpr std::int64_t k_int64_max = std::numeric_limits<std::int64_t>::max();
  inline constexpr std::int64_t k_int64_min = std::numeric_limits<std::int64_t>::min();

  /** a + b without overflow; false when it would overflow. */
  inline bool checked_add(std::int64_t a, std::int64_t b, std::int64_t &out) {
    if ((b > 0 && a > k_int64_max - b) || (b < 0 && a < k_int64_min - b)) return false;
    out = a + b;
    return true;
  }

  /** a − b without overflow; false when it would overflow. */
  inline bool checked_sub(std::int64_t a, std::int64_t b, std::int64_t &out) {
    if ((b < 0 && a > k_int64_max + b) || (b > 0 && a < k_int64_min + b)) return false;
    out = a - b;
    return true;
  }

  /** |value| as unsigned, defined for every int64 including the minimum. */
  inline std::uint64_t magnitude(std::int64_t value) {
    return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1 : static_cast<std::uint64_t>(value);
  }

  /** a + b for unsigned values, false when it would exceed the maximum. */
  inline bool checked_add_unsigned(std::uint64_t a, std::uint64_t b, std::uint64_t &out) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
    out = a + b;
    return true;
  }

  /** A failed clock read, distinct from any real CLOCK_MONOTONIC or steady_clock value. */
  inline constexpr std::int64_t k_failed_read = -1;

  /** One bracketed read: CLOCK_MONOTONIC, then std::chrono::steady_clock, then CLOCK_MONOTONIC. */
  struct probe_sample_t {
    std::int64_t mono_before_ns = k_failed_read;
    std::int64_t steady_ns = k_failed_read;
    std::int64_t mono_after_ns = k_failed_read;
  };

  inline constexpr std::uint64_t k_min_probe_samples = 2;
  inline constexpr std::uint64_t k_max_probe_samples = 64;

  struct mapping_limits_t {
    std::uint64_t min_samples = 8;  ///< clamped to [k_min_probe_samples, k_max_probe_samples] by collect()
    std::uint64_t max_uncertainty_ns = 50'000;  ///< worst delta + worst bracket
    std::uint64_t max_abs_offset_ns = 1'000'000;  ///< |steady − monotonic| per sample; one kernel clock is expected
  };

  /** steady_ns − steady_to_mono_offset_ns is the CLOCK_MONOTONIC time, within uncertainty_ns. */
  struct mapping_t {
    bool valid = false;
    std::uint64_t samples = 0;
    std::uint64_t max_delta_ns = 0;
    std::uint64_t max_bracket_ns = 0;
    std::uint64_t uncertainty_ns = 0;
    std::int64_t steady_to_mono_offset_ns = 0;
    std::string_view reason = "not_probed";
  };

  inline mapping_t evaluate_mapping(std::span<const probe_sample_t> samples, const mapping_limits_t &limits) {
    mapping_t mapping;
    mapping.samples = samples.size();
    if (samples.empty() || samples.size() < limits.min_samples) {
      mapping.reason = "too_few_samples";
      return mapping;
    }
    std::optional<std::int64_t> first_offset;
    for (const auto &sample : samples) {
      if (sample.mono_before_ns < 0 || sample.steady_ns < 0 || sample.mono_after_ns < 0) {
        mapping.reason = "clock_read_failed";
        return mapping;
      }
      std::int64_t bracket = 0;
      if (!checked_sub(sample.mono_after_ns, sample.mono_before_ns, bracket) || bracket < 0) {
        mapping.reason = "bracket_reversed";
        return mapping;
      }
      // Both reads are nonnegative, so before + bracket / 2 cannot overflow.
      const std::int64_t mid = sample.mono_before_ns + bracket / 2;
      std::int64_t offset = 0;
      if (!checked_sub(sample.steady_ns, mid, offset)) {
        mapping.reason = "offset_overflow";
        return mapping;
      }
      if (magnitude(offset) > limits.max_abs_offset_ns) {
        mapping.reason = "offset_out_of_bounds";
        return mapping;
      }
      if (!first_offset) first_offset = offset;
      std::int64_t delta = 0;
      if (!checked_sub(offset, *first_offset, delta)) {
        mapping.reason = "delta_overflow";
        return mapping;
      }
      mapping.max_delta_ns = std::max(mapping.max_delta_ns, magnitude(delta));
      mapping.max_bracket_ns = std::max(mapping.max_bracket_ns, static_cast<std::uint64_t>(bracket));
    }
    mapping.steady_to_mono_offset_ns = *first_offset;
    if (!checked_add_unsigned(mapping.max_delta_ns, mapping.max_bracket_ns, mapping.uncertainty_ns)) {
      mapping.reason = "uncertainty_overflow";
      return mapping;
    }
    if (mapping.uncertainty_ns > limits.max_uncertainty_ns) {
      mapping.reason = "uncertainty_out_of_bounds";
      return mapping;
    }
    mapping.valid = true;
    mapping.reason = "bounded";
    return mapping;
  }

  /** A time namespace's identity, from fstat() of an opened /proc/<x>/ns/time descriptor. */
  struct ns_id_t {
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    bool operator==(const ns_id_t &) const = default;
  };

  /** What could be read about one process, each read taken twice to catch a change in between. */
  struct process_facts_t {
    std::int64_t pid = 0;
    std::optional<std::uint32_t> uid;
    std::optional<std::uint64_t> starttime_before;
    std::optional<std::uint64_t> starttime_after;
    std::optional<ns_id_t> ns_first;
    std::optional<ns_id_t> ns_second;
  };

  enum class namespace_e {
    same,
    different,
    unverifiable,
  };

  inline std::string_view name(namespace_e value) {
    switch (value) {
      case namespace_e::same:
        return "same";
      case namespace_e::different:
        return "different";
      case namespace_e::unverifiable:
        break;
    }
    return "unverifiable";
  }

  struct namespace_result_t {
    namespace_e result = namespace_e::unverifiable;
    std::string_view reason = "not_probed";
  };

  /**
   * @brief Compare our time namespace with the socket peer's, failing closed on anything unstable.
   *
   * self needs two equal namespace reads, taken before and after the peer's. The peer needs a pid
   * above 1, the expected uid, two equal namespace reads and two equal process start times, so a pid
   * reused between reads cannot pass. Equal means equal dev AND inode.
   */
  inline namespace_result_t compare_time_namespaces(const process_facts_t &self,
                                                    const process_facts_t &peer,
                                                    std::uint32_t expected_uid) {
    if (peer.pid <= 1) return {namespace_e::unverifiable, "peer_pid_invalid"};
    if (!peer.uid || *peer.uid != expected_uid) return {namespace_e::unverifiable, "peer_uid_mismatch"};
    if (!self.ns_first || !self.ns_second) return {namespace_e::unverifiable, "self_namespace_unavailable"};
    if (!(*self.ns_first == *self.ns_second)) return {namespace_e::unverifiable, "self_namespace_unstable"};
    if (!peer.starttime_before || !peer.starttime_after) {
      return {namespace_e::unverifiable, "peer_starttime_unavailable"};
    }
    if (*peer.starttime_before != *peer.starttime_after) return {namespace_e::unverifiable, "peer_process_replaced"};
    if (!peer.ns_first || !peer.ns_second) return {namespace_e::unverifiable, "peer_namespace_unavailable"};
    if (!(*peer.ns_first == *peer.ns_second)) return {namespace_e::unverifiable, "peer_namespace_unstable"};
    if (*peer.ns_first == *self.ns_first) return {namespace_e::same, "same_dev_and_inode"};
    return {namespace_e::different, "different_dev_or_inode"};
  }

  /** One capture epoch's clock evidence. Immutable once built; shared by every image of the epoch. */
  struct proof_t {
    std::uint64_t capture_epoch_id = 0;
    namespace_e time_namespace = namespace_e::unverifiable;
    std::string_view namespace_reason = "not_probed";
    bool producer_identity_verified = false;
    std::string_view producer_identity_reason = "not_probed";
    mapping_t mapping;
    std::int64_t epoch_start_mono_ns = k_failed_read;
    bool clock_domain_verified = false;
  };

  inline proof_t make_proof(std::uint64_t capture_epoch_id,
                            const namespace_result_t &time_namespace,
                            bool producer_identity_verified,
                            std::string_view producer_identity_reason,
                            const mapping_t &mapping,
                            std::int64_t epoch_start_mono_ns) {
    proof_t proof;
    proof.capture_epoch_id = capture_epoch_id;
    proof.time_namespace = time_namespace.result;
    proof.namespace_reason = time_namespace.reason;
    proof.producer_identity_verified = producer_identity_verified;
    proof.producer_identity_reason = producer_identity_reason;
    proof.mapping = mapping;
    proof.epoch_start_mono_ns = epoch_start_mono_ns;
    proof.clock_domain_verified = capture_epoch_id != 0 && epoch_start_mono_ns >= 0 &&
                                  time_namespace.result == namespace_e::same &&
                                  producer_identity_verified && mapping.valid;
    return proof;
  }

  /** The facts collect() needs, so tests can stand in for the kernel. */
  class system_t {
  public:
    virtual ~system_t() = default;

    struct peer_cred_t {
      std::int64_t pid = 0;
      std::uint32_t uid = 0;
    };

    /** SO_PEERCRED of a connected UNIX socket: socket-peer facts only. */
    virtual std::optional<peer_cred_t> peer_credentials(int fd) = 0;
    /** fstat() dev and inode of an opened /proc/self/ns/time (pid 0) or /proc/<pid>/ns/time. */
    virtual std::optional<ns_id_t> time_namespace(std::int64_t pid) = 0;
    /** /proc/<pid>/stat field 22, starttime in clock ticks since boot. */
    virtual std::optional<std::uint64_t> process_starttime(std::int64_t pid) = 0;
    virtual std::uint32_t current_uid() = 0;
    /** Fields are k_failed_read when a read failed. */
    virtual probe_sample_t sample_clocks() = 0;
    /** k_failed_read when the read failed. */
    virtual std::int64_t monotonic_now_ns() = 0;
  };

  /**
   * @brief Build one epoch's proof from the connected Wayland socket's peer.
   *
   * Producer identity is verified only against the retained lease of the generation this display
   * connected to: current() brackets the very SO_PEERCRED, start time, time-namespace and mapping reads
   * used here, and those actual peer facts must equal the lease's process. A null, retired or replaced
   * lease leaves it false; a peer is never matched by process name. The probe takes min_samples
   * clamped to [k_min_probe_samples, k_max_probe_samples].
   */
  inline std::shared_ptr<const proof_t> collect(int peer_fd,
                                                std::uint64_t capture_epoch_id,
                                                system_t &system,
                                                const mapping_limits_t &limits,
                                                std::shared_ptr<const compositor_lease_t> lease) {
    const compositor_identity_check_t identity {std::move(lease)};
    const auto epoch_start = system.monotonic_now_ns();
    process_facts_t self;
    process_facts_t peer;
    self.ns_first = system.time_namespace(0);
    if (peer_fd >= 0) {
      if (const auto cred = system.peer_credentials(peer_fd)) {
        peer.pid = cred->pid;
        peer.uid = cred->uid;
        if (peer.pid > 1) {
          peer.starttime_before = system.process_starttime(peer.pid);
          peer.ns_first = system.time_namespace(peer.pid);
          peer.ns_second = system.time_namespace(peer.pid);
          peer.starttime_after = system.process_starttime(peer.pid);
        }
      }
    }
    self.ns_second = system.time_namespace(0);
    const auto time_namespace = peer_fd < 0 ?
                                  namespace_result_t {namespace_e::unverifiable, "no_peer_socket"} :
                                  compare_time_namespaces(self, peer, system.current_uid());

    auto bounded = limits;
    bounded.min_samples = std::clamp(limits.min_samples, k_min_probe_samples, k_max_probe_samples);
    std::vector<probe_sample_t> samples;
    samples.reserve(bounded.min_samples);
    for (std::uint64_t index = 0; index < bounded.min_samples; ++index) {
      samples.push_back(system.sample_clocks());
    }
    auto mapping = evaluate_mapping(samples, bounded);
    if (epoch_start < 0) {
      mapping.valid = false;
      mapping.reason = "epoch_start_read_failed";
    }
    // After every read above: the lease must still be current, and the peer it describes the one read.
    const auto producer = peer_fd < 0 ? compositor_match_t {} :
                                        identity.verify(peer.pid, peer.uid, peer.starttime_before, peer.starttime_after);
    return std::make_shared<const proof_t>(make_proof(
      capture_epoch_id, time_namespace, producer.verified, producer.reason, mapping, epoch_start
    ));
  }

  /** Per-frame plausibility. It classifies; it never changes the frame's pacing timestamp. */
  enum class sample_e {
    valid,  ///< plausible against an authoritative, verified, same-namespace clock
    clock_unverified,  ///< syntactically valid, but no authoritative proof permits comparing it with our clock
    missing,
    invalid,
    nonmonotonic,
    future,
    stale,
    before_epoch,
    prefetched_init,
  };

  inline std::string_view name(sample_e value) {
    switch (value) {
      case sample_e::clock_unverified:
        return "clock_unverified";
      case sample_e::missing:
        return "missing";
      case sample_e::invalid:
        return "invalid";
      case sample_e::nonmonotonic:
        return "nonmonotonic";
      case sample_e::future:
        return "future";
      case sample_e::stale:
        return "stale";
      case sample_e::before_epoch:
        return "before_epoch";
      case sample_e::prefetched_init:
        return "prefetched_init";
      case sample_e::valid:
        break;
    }
    return "valid";
  }

  struct plausibility_limits_t {
    std::uint64_t staleness_ns = 100'000'000;  ///< ready − presentation beyond this (plus uncertainty) is stale
  };

  /**
   * @brief Whether a proof authorises comparing a raw presentation time with this process's clock.
   *
   * Every condition is required, and a proof whose flags contradict each other is refused: a raw time
   * from another or unknown namespace, or from an unidentified producer, is never compared at all.
   */
  inline bool authorises_clock_comparison(const proof_t &proof) {
    return proof.capture_epoch_id != 0 && proof.time_namespace == namespace_e::same &&
           proof.producer_identity_verified && proof.mapping.valid && proof.epoch_start_mono_ns >= 0 &&
           proof.clock_domain_verified;
  }

  /**
   * @brief Classify one frame's raw presentation time against its ready time and its epoch's proof.
   *
   * Order: prefetched_init, then the tracker's status (an unknown status fails closed as invalid),
   * then representability. Only a proof that authorises_clock_comparison() permits the
   * before_epoch/future/stale comparisons; without one a syntactically valid time is clock_unverified
   * and is never compared across clocks.
   */
  inline sample_e classify(vrr_policy::presentation_status_e status,
                           std::optional<std::int64_t> presentation_ns,
                           bool prefetched_init,
                           std::optional<std::chrono::steady_clock::time_point> ready_at,
                           const proof_t *proof,
                           const plausibility_limits_t &limits) {
    using status_e = vrr_policy::presentation_status_e;
    if (prefetched_init) return sample_e::prefetched_init;
    if (status == status_e::missing) return sample_e::missing;
    if (status == status_e::invalid_nsec || status == status_e::invalid_overflow) return sample_e::invalid;
    if (status == status_e::nonmonotonic) return sample_e::nonmonotonic;
    if (status != status_e::valid) return sample_e::invalid;  // a value outside the enum
    if (!presentation_ns || *presentation_ns < 0) return sample_e::invalid;
    if (!proof || !ready_at || !authorises_clock_comparison(*proof)) {
      return sample_e::clock_unverified;
    }
    const auto ready_steady_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(ready_at->time_since_epoch()).count();
    std::int64_t ready_mono_ns = 0;
    if (ready_steady_ns < 0 ||
        !checked_sub(static_cast<std::int64_t>(ready_steady_ns), proof->mapping.steady_to_mono_offset_ns, ready_mono_ns) ||
        ready_mono_ns < 0) {
      return sample_e::invalid;
    }
    if (proof->mapping.uncertainty_ns > static_cast<std::uint64_t>(k_int64_max) ||
        limits.staleness_ns > static_cast<std::uint64_t>(k_int64_max)) {
      return sample_e::invalid;
    }
    const auto uncertainty = static_cast<std::int64_t>(proof->mapping.uncertainty_ns);
    const auto present = *presentation_ns;
    if (present < proof->epoch_start_mono_ns) return sample_e::before_epoch;
    std::int64_t latest = 0;
    if (!checked_add(ready_mono_ns, uncertainty, latest)) return sample_e::invalid;
    if (present > latest) return sample_e::future;
    std::int64_t age = 0;
    std::int64_t allowed = 0;
    if (!checked_sub(ready_mono_ns, present, age) ||
        !checked_add(static_cast<std::int64_t>(limits.staleness_ns), uncertainty, allowed)) {
      return sample_e::invalid;
    }
    if (age > allowed) return sample_e::stale;
    return sample_e::valid;
  }

}  // namespace vrr_clock
