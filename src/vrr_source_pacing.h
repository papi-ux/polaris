/**
 * @file src/vrr_source_pacing.h
 * @brief The encode-side rules for a session that paces from mapped compositor source time.
 *
 * Pure and portable: every input is passed in, so the rules are testable without an encoder. The
 * encode loop owns one generation_t per session generation, outside any single encoder session, so a
 * display reopen or encoder recreation cannot reset what was already emitted or a latched fallback.
 *
 * Decisions are two-phase. precheck_source()/on_wait() only look; commit() changes state, and is
 * called only after the encoder accepted the frame. A refused, cancelled or failed encode therefore
 * never consumes a sequence, an emitted time or rate credit.
 *
 * The rate ceiling bounds actual work: it refills and spends on the real steady time at which the
 * loop judges and hands a frame to the encoder, never on the frame's (possibly older) source time. A
 * valid backlog of queued source frames therefore cannot pass more than the burst at once.
 *
 * Emitted times are never snapped to a grid and a valid source time is never rewritten: a frame that
 * cannot be emitted at its own time is dropped with a fixed reason. Recovery, readiness and liveness
 * frames, which carry no source time, use the current steady time (never earlier than one 90 kHz
 * tick after the last emission).
 *
 * Design credit: variable-cadence presentation for Moonlight clients follows Nonary's VRR work
 * (https://github.com/Nonary/moonlight-qt/releases/tag/v6.1.0-vrr18,
 * https://github.com/Nonary/moonlight-qt/commit/052457f2c02b1985ddc846975963c39a0cf8db25). No code
 * is copied from it.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>

#include "vrr_pacing_receipt.h"
#include "vrr_source_time.h"

namespace vrr_pacing {

  /// One 90 kHz RTP tick, rounded up: emissions this far apart always get distinct RTP timestamps.
  inline constexpr std::int64_t k_tick_ns = 11'112;
  /// With no new source frame, emit one labelled liveness repeat at most this often.
  inline constexpr std::int64_t k_liveness_ns = 500'000'000;
  /// A mapped source time older than this (plus its stated uncertainty) at encode is stale.
  inline constexpr std::int64_t k_max_source_age_ns = 100'000'000;
  /// Frames the rate ceiling lets through back to back: one catch-up pair.
  inline constexpr std::int64_t k_burst_frames = 2;
  /// The largest uncertainty the mapper itself can produce; anything above is not a mapper receipt.
  inline constexpr std::uint64_t k_max_receipt_uncertainty_ns = vrr_clock::source_time_limits_t {}.max_total_uncertainty_ns;

  /// The negotiated stream rate as an exact fraction: num/den frames per second.
  struct rate_t {
    std::int64_t num = 0;
    std::int64_t den = 1;
  };

  /**
   * @brief An exact integer token bucket bounding emissions to the negotiated rate.
   *
   * Credit is kept in units where one frame costs den × 1e9 and one nanosecond of emitted time
   * refills num, so any rational rate is exact. It holds at most k_burst_frames frames, which lets
   * ordinary uneven intervals and a short catch-up pair through while a sustained excess is refused.
   */
  class ceiling_t {
  public:
    ceiling_t() = default;

    explicit ceiling_t(rate_t rate) {
      if (rate.num <= 0 || rate.den <= 0) return;
      const auto cost = vrr_clock::checked_mul_positive(rate.den, 1'000'000'000);
      if (!cost) return;
      const auto capacity = vrr_clock::checked_mul_positive(*cost, k_burst_frames);
      if (!capacity) return;
      num_ = rate.num;
      cost_ = *cost;
      capacity_ = *capacity;
      credit_ = *capacity;
      valid_ = true;
    }

    bool valid() const {
      return valid_;
    }

    /// Whether an emission at at_ns fits. A time before the last committed one never fits.
    bool allows(std::int64_t at_ns) const {
      const auto credit = credit_at(at_ns);
      return credit && *credit >= cost_;
    }

    /// Spend one frame at at_ns. Only for an emission allows() accepted and the encoder took.
    void commit(std::int64_t at_ns) {
      const auto credit = credit_at(at_ns);
      if (!credit || *credit < cost_) return;
      credit_ = *credit - cost_;
      last_ns_ = at_ns;
    }

  private:
    std::optional<std::int64_t> credit_at(std::int64_t at_ns) const {
      if (!valid_) return std::nullopt;
      if (!last_ns_) return credit_;
      std::int64_t elapsed = 0;
      if (!vrr_clock::checked_sub(at_ns, *last_ns_, elapsed) || elapsed < 0) return std::nullopt;
      // A refill too large to represent fills the bucket.
      const auto refill = vrr_clock::checked_mul_positive(elapsed, num_);
      if (!refill || *refill >= capacity_ - credit_) return capacity_;
      return credit_ + *refill;
    }

    bool valid_ = false;
    std::int64_t num_ = 0;
    std::int64_t cost_ = 0;
    std::int64_t capacity_ = 0;
    std::int64_t credit_ = 0;
    std::optional<std::int64_t> last_ns_;
  };

  /// What an emission is, for its fixed event name. Only source carries a source time.
  enum class kind_e {
    source,
    readiness,
    idr_recovery,
    liveness,
  };

  inline event_e event_of(kind_e kind) {
    switch (kind) {
      case kind_e::source:
        return event_e::accepted_source;
      case kind_e::readiness:
        return event_e::readiness;
      case kind_e::idr_recovery:
        return event_e::idr_recovery;
      case kind_e::liveness:
        return event_e::liveness;
    }
    return event_e::liveness;
  }

  /// An emission the rules would allow now. Nothing is spent until commit().
  struct candidate_t {
    kind_e kind = kind_e::liveness;
    std::int64_t emit_ns = 0;
    std::uint64_t capture_epoch = 0;
    std::uint64_t source_sequence = 0;
  };

  /// Either a candidate, or the fixed reason nothing may be emitted now.
  struct decision_t {
    std::optional<candidate_t> candidate;
    event_e event = event_e::routine_repeat_suppressed;
  };

  /// The facts of one newly captured frame that pacing may use. Copied from its immutable provenance.
  struct source_frame_t {
    std::uint64_t capture_epoch = 0;
    std::uint64_t source_sequence = 0;
    vrr_clock::source_time_e receipt = vrr_clock::source_time_e::not_observed;
    std::optional<std::int64_t> source_ns;
    std::uint64_t uncertainty_ns = 0;
  };

  /**
   * @brief One session generation's pacing state, across every encoder session and display reopen.
   *
   * mode() is waiting until the first source frame is committed, then source_time. latch() moves it
   * to latched_fallback for good; nothing in this class can leave that state.
   */
  class generation_t {
  public:
    generation_t() = default;

    explicit generation_t(rate_t rate):
        ceiling_ {rate} {
    }

    bool rate_valid() const {
      return ceiling_.valid();
    }

    mode_e mode() const {
      return latched_ ? mode_e::latched_fallback : committed_source_ ? mode_e::source_time : mode_e::waiting;
    }

    bool latched() const {
      return latched_;
    }

    /// The last committed emission of any kind, in steady nanoseconds.
    std::optional<std::int64_t> last_emitted_ns() const {
      return last_emitted_ns_;
    }

    /**
     * Whether a newly captured frame may be emitted at its own source time, judged at now_ns.
     * Called before conversion and again after it, with a fresh now_ns, before encoding.
     */
    decision_t precheck_source(const source_frame_t &frame, std::int64_t now_ns) const {
      decision_t refused;
      if (latched_) {
        refused.event = event_e::authority_lost;
        return refused;
      }
      if (frame.receipt != vrr_clock::source_time_e::mapped || !frame.source_ns || frame.capture_epoch == 0 ||
          frame.source_sequence == 0) {
        refused.event = event_e::receipt_not_mapped;
        return refused;
      }
      if (frame.capture_epoch < last_epoch_ ||
          (frame.capture_epoch == last_epoch_ && frame.source_sequence <= last_sequence_)) {
        refused.event = event_e::stale_or_replayed;
        return refused;
      }
      if (frame.uncertainty_ns > k_max_receipt_uncertainty_ns) {
        refused.event = event_e::receipt_not_mapped;
        return refused;
      }
      const auto source = *frame.source_ns;
      // Steady times are nonnegative; anything else is not a usable time. With both nonnegative,
      // every difference below is representable, so no negation or wrap is ever evaluated.
      if (now_ns < 0 || source < 0) {
        refused.event = event_e::source_stale;
        return refused;
      }
      // Fresh at encode: no older than the bound plus its uncertainty, and not from the future.
      const auto uncertainty = static_cast<std::int64_t>(frame.uncertainty_ns);
      if (source <= now_ns ? now_ns - source > k_max_source_age_ns + uncertainty : source - now_ns > uncertainty) {
        refused.event = event_e::source_stale;
        return refused;
      }
      if (const auto monotonic = monotonic_refusal(source)) {
        refused.event = *monotonic;
        return refused;
      }
      // Bounded by actual work now, not by when the source presented it.
      if (!ceiling_.allows(now_ns)) {
        refused.event = event_e::rate_ceiling;
        return refused;
      }
      return {candidate_t {kind_e::source, source, frame.capture_epoch, frame.source_sequence}, event_e::accepted_source};
    }

    /**
     * No new frame arrived within the wait. A pending IDR gets a labelled recovery frame; before
     * anything was emitted, a labelled readiness frame; after no emission for k_liveness_ns, one
     * labelled liveness repeat. Anything else is a suppressed routine repeat.
     */
    decision_t on_wait(std::int64_t now_ns, bool idr_pending) const {
      decision_t decision;
      if (now_ns < 0) {
        decision.event = event_e::source_stale;
        return decision;
      }
      kind_e kind;
      if (idr_pending) {
        kind = kind_e::idr_recovery;
      } else if (!last_emitted_ns_) {
        kind = kind_e::readiness;
      } else {
        std::int64_t quiet = 0;
        if (!vrr_clock::checked_sub(now_ns, last_emit_wall_ns_, quiet) || quiet < k_liveness_ns) {
          decision.event = event_e::routine_repeat_suppressed;
          return decision;
        }
        kind = kind_e::liveness;
      }
      return emission_at_now(kind, now_ns);
    }

    /**
     * A frame with no source time to give, for example after the session fell back: its time is the
     * legacy time when it is late enough, else the current time, and it is still bounded and ordered.
     */
    decision_t untimed_emission(kind_e kind, std::int64_t now_ns) const {
      return emission_at_now(kind, now_ns);
    }

    /**
     * After the session fell back: the legacy frame at its legacy time when that keeps emission order
     * and the ceiling, otherwise at the current time (never a source time, so nothing is rewritten).
     */
    decision_t fallback_emission(std::optional<std::int64_t> legacy_ns, std::int64_t now_ns) const {
      if (now_ns < 0) return {std::nullopt, event_e::source_stale};
      if (legacy_ns && *legacy_ns >= 0 && !monotonic_refusal(*legacy_ns) && ceiling_.allows(now_ns)) {
        return {candidate_t {kind_e::liveness, *legacy_ns, 0, 0}, event_e::liveness};
      }
      return emission_at_now(kind_e::liveness, now_ns);
    }

    /// Record an emission the encoder accepted. now_ns is the actual steady time it was handed over.
    void commit(const candidate_t &candidate, std::int64_t now_ns) {
      last_emitted_ns_ = candidate.emit_ns;
      last_emit_wall_ns_ = now_ns;
      ceiling_.commit(now_ns);
      if (candidate.kind == kind_e::source) {
        last_epoch_ = candidate.capture_epoch;
        last_sequence_ = candidate.source_sequence;
        committed_source_ = true;
      }
    }

    /// The live authority was lost: this generation never emits a source time again.
    void latch() {
      latched_ = true;
    }

  private:
    std::optional<event_e> monotonic_refusal(std::int64_t at_ns) const {
      if (!last_emitted_ns_) return std::nullopt;
      if (at_ns <= *last_emitted_ns_) return event_e::regressed_vs_emitted;
      std::int64_t gap = 0;
      if (!vrr_clock::checked_sub(at_ns, *last_emitted_ns_, gap) || gap < k_tick_ns) return event_e::tick_collision;
      return std::nullopt;
    }

    decision_t emission_at_now(kind_e kind, std::int64_t now_ns) const {
      decision_t decision;
      if (now_ns < 0) {
        decision.event = event_e::source_stale;
        return decision;
      }
      std::int64_t at = now_ns;
      if (last_emitted_ns_) {
        std::int64_t floor = 0;
        if (!vrr_clock::checked_add(*last_emitted_ns_, k_tick_ns, floor)) {
          decision.event = event_e::tick_collision;
          return decision;
        }
        if (at < floor) at = floor;
      }
      if (!ceiling_.allows(now_ns)) {
        decision.event = event_e::rate_ceiling;
        return decision;
      }
      decision.candidate = candidate_t {kind, at, 0, 0};
      decision.event = event_of(kind);
      return decision;
    }

    ceiling_t ceiling_;
    std::optional<std::int64_t> last_emitted_ns_;
    std::int64_t last_emit_wall_ns_ = 0;
    std::uint64_t last_epoch_ = 0;
    std::uint64_t last_sequence_ = 0;
    bool committed_source_ = false;
    bool latched_ = false;
  };

  /// What a source-paced session hands the adaptive bitrate controller.
  struct health_inputs_t {
    double fps_ratio = 1.0;
    double dropped_frame_ratio = 0.0;
    double duplicate_frame_ratio = 0.0;
    double frame_jitter_ms = 0.0;
    double encode_time_ms = 0.0;
    double avg_frame_age_ms = 0.0;
  };

  /**
   * @brief The controller inputs of a source-paced session: its cadence is the game's, not pressure.
   *
   * A lower game frame rate, intentional ceiling refusals and suppressed routine repeats are not
   * delivery shortfall, drops or duplicates, so they are neutral here (fps ratio 1, no duplicates).
   * Measured encode time and frame age are passed as observed, so real encoder pressure still acts.
   * Source-relative jitter is passed only when measured; unavailable is NaN, never a healthy zero.
   */
  inline health_inputs_t source_health_inputs(double involuntary_dropped_ratio,
                                              std::optional<double> source_jitter_ms,
                                              double encode_time_ms,
                                              double avg_frame_age_ms) {
    health_inputs_t inputs;
    inputs.dropped_frame_ratio = involuntary_dropped_ratio;
    inputs.frame_jitter_ms = source_jitter_ms.value_or(std::numeric_limits<double>::quiet_NaN());
    inputs.encode_time_ms = encode_time_ms;
    inputs.avg_frame_age_ms = avg_frame_age_ms;
    return inputs;
  }

  /**
   * @brief Each encoder submission's emitted time, keyed by its frame index, for delayed output.
   *
   * An encoder may return a frame's packet during a later call. Taking by the packet's own index
   * attaches ITS time, never the time of whatever frame was just submitted. Bounded: the oldest
   * entries are discarded past k_capacity, and take() discards every older entry.
   */
  class submission_times_t {
  public:
    static constexpr std::size_t k_capacity = 64;

    struct entry_t {
      std::int64_t emit_ns = 0;
      /// Submitted by the effective source-time path (not a latched or legacy fallback emission).
      bool source_time_paced = false;
    };

    /// Whether the next put() is an effective source-time path submission. Set by the encode loop.
    bool next_source_time_paced = false;

    void put(std::int64_t index, std::int64_t emit_ns) {
      times_.insert_or_assign(index, entry_t {emit_ns, next_source_time_paced});
      while (times_.size() > k_capacity) times_.erase(times_.begin());
    }

    std::optional<entry_t> take(std::int64_t index) {
      const auto found = times_.find(index);
      if (found == times_.end()) return std::nullopt;
      const auto value = found->second;
      times_.erase(times_.begin(), std::next(found));
      return value;
    }

    std::size_t size() const {
      return times_.size();
    }

  private:
    std::map<std::int64_t, entry_t> times_;
  };

}  // namespace vrr_pacing
