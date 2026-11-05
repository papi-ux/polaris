/** Experimental, opt-in native downward trial. Elapsed bounds never enter the ENet estimator.
 * Fixed storage; one rate-local reference, one cut, no automatic return or learned background.
 * The owner must serialize current permission/producer/admission checks before every observation.
 */
#pragma once
#include "native_video_signal.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace live_tuning_native {
  using setting_t = stream_frame_feedback::encoder_setting_t;
  inline constexpr std::int64_t second_ns = 1'000'000'000;
  inline constexpr std::int64_t gap_ns = 500'000'000;
  inline constexpr std::int64_t proof_ns = 6 * second_ns, response_ns = 6 * second_ns;

  enum class phase_e { reference, awaiting_application, response, responded, ineffective, unassessed };
  /// Why the episode last changed phase. Static and sanitized; diagnostic only, never read by a decision.
  enum class transition_reason_e {
    none, standing_trigger, backend_tuple_confirmed, four_buckets_below_trigger,
    proof_deadline, wrong_application, response_deadline, setting_changed,
    median_unavailable, order, gap, bucket_second_skipped, bucket_coverage, bucket_overflow,
    permission, not_consumed_receipt, not_consumed_measurement, not_consumed_setting, not_consumed_current,
    policy_refused, reset_by_writer_or_scope, unavailable,
  };
  /// What happened to one closed response bucket. Diagnostic only; a fact is never response credit by itself.
  enum class bucket_disposition_e {
    none, initial_partial_discarded, below_trigger, not_below_trigger, coverage_short, second_skipped, overflow,
  };
  inline const char *name(transition_reason_e reason) noexcept {
    switch (reason) {
#define NATIVE_TRANSITION_REASON(x) case transition_reason_e::x: return #x
      NATIVE_TRANSITION_REASON(none); NATIVE_TRANSITION_REASON(standing_trigger);
      NATIVE_TRANSITION_REASON(backend_tuple_confirmed); NATIVE_TRANSITION_REASON(four_buckets_below_trigger);
      NATIVE_TRANSITION_REASON(proof_deadline); NATIVE_TRANSITION_REASON(wrong_application);
      NATIVE_TRANSITION_REASON(response_deadline); NATIVE_TRANSITION_REASON(setting_changed);
      NATIVE_TRANSITION_REASON(median_unavailable); NATIVE_TRANSITION_REASON(order); NATIVE_TRANSITION_REASON(gap);
      NATIVE_TRANSITION_REASON(bucket_second_skipped); NATIVE_TRANSITION_REASON(bucket_coverage);
      NATIVE_TRANSITION_REASON(bucket_overflow); NATIVE_TRANSITION_REASON(permission);
      NATIVE_TRANSITION_REASON(not_consumed_receipt); NATIVE_TRANSITION_REASON(not_consumed_measurement);
      NATIVE_TRANSITION_REASON(not_consumed_setting); NATIVE_TRANSITION_REASON(not_consumed_current);
      NATIVE_TRANSITION_REASON(policy_refused); NATIVE_TRANSITION_REASON(reset_by_writer_or_scope);
      NATIVE_TRANSITION_REASON(unavailable);
#undef NATIVE_TRANSITION_REASON
    }
    return "unavailable";
  }
  inline const char *name(bucket_disposition_e disposition) noexcept {
    switch (disposition) {
#define NATIVE_BUCKET_DISPOSITION(x) case bucket_disposition_e::x: return #x
      NATIVE_BUCKET_DISPOSITION(none); NATIVE_BUCKET_DISPOSITION(initial_partial_discarded);
      NATIVE_BUCKET_DISPOSITION(below_trigger); NATIVE_BUCKET_DISPOSITION(not_below_trigger);
      NATIVE_BUCKET_DISPOSITION(coverage_short); NATIVE_BUCKET_DISPOSITION(second_skipped);
      NATIVE_BUCKET_DISPOSITION(overflow);
#undef NATIVE_BUCKET_DISPOSITION
    }
    return "none";
  }
  struct bucket_fact_t {
    bucket_disposition_e disposition = bucket_disposition_e::none;
    std::size_t count = 0;
    std::int64_t span_ns = 0, median_lower_ns = 0, median_upper_ns = 0, trigger_lower_ns = 0;
    int consecutive_below = 0; // Consecutive sub-trigger response buckets after this one.
  };
  struct diagnostic_t {
    transition_reason_e reason = transition_reason_e::none;
    bucket_fact_t bucket;      // The latest response bucket fact.
    std::uint32_t buckets = 0; // Response bucket facts so far, so a reader can tell a new one apart.
  };
  /// Why a reference/standing window that already held observations was discarded before any cut.
  /// Diagnostic only: never serialized and never read by a decision. Reference-phase clears change no phase,
  /// so they leave no history row; this record is the only place they are counted.
  enum class reference_clear_e {
    none, gap, order, median_unavailable, bucket_coverage, bucket_second_skipped, bucket_overflow,
    source_changed, action_revision_changed, floor_changed, reference_expired,
    permission, not_consumed_receipt, not_consumed_measurement, not_consumed_setting, not_consumed_current,
    policy_refused, writer_or_scope, other,
  };
  inline constexpr std::size_t reference_clear_count = static_cast<std::size_t>(reference_clear_e::other) + 1;
  inline const char *name(reference_clear_e clear) noexcept {
    switch (clear) {
#define NATIVE_REFERENCE_CLEAR(x) case reference_clear_e::x: return #x
      NATIVE_REFERENCE_CLEAR(none); NATIVE_REFERENCE_CLEAR(gap); NATIVE_REFERENCE_CLEAR(order);
      NATIVE_REFERENCE_CLEAR(median_unavailable); NATIVE_REFERENCE_CLEAR(bucket_coverage);
      NATIVE_REFERENCE_CLEAR(bucket_second_skipped); NATIVE_REFERENCE_CLEAR(bucket_overflow);
      NATIVE_REFERENCE_CLEAR(source_changed); NATIVE_REFERENCE_CLEAR(action_revision_changed);
      NATIVE_REFERENCE_CLEAR(floor_changed); NATIVE_REFERENCE_CLEAR(reference_expired);
      NATIVE_REFERENCE_CLEAR(permission); NATIVE_REFERENCE_CLEAR(not_consumed_receipt);
      NATIVE_REFERENCE_CLEAR(not_consumed_measurement); NATIVE_REFERENCE_CLEAR(not_consumed_setting);
      NATIVE_REFERENCE_CLEAR(not_consumed_current); NATIVE_REFERENCE_CLEAR(policy_refused);
      NATIVE_REFERENCE_CLEAR(writer_or_scope); NATIVE_REFERENCE_CLEAR(other);
#undef NATIVE_REFERENCE_CLEAR
    }
    return "other";
  }
  inline void saturating_increment(std::uint32_t &value) noexcept {
    if (value != UINT32_MAX) ++value;
  }
  /// Reference-phase interruptions of one episode. Fixed storage and saturating counts; diagnostic only.
  struct reference_interruptions_t {
    std::array<std::uint32_t, reference_clear_count> clears {}; // Windows discarded while holding observations.
    std::uint32_t total = 0;
    reference_clear_e first = reference_clear_e::none, last = reference_clear_e::none;
    // What the latest clear discarded: closed buckets, reference buckets (<= 4) and consecutive standing buckets.
    int last_lost_closed = 0, last_lost_reference = 0, last_lost_standing = 0, max_lost_standing = 0;
  };
  struct evidence_t {
    setting_t source;
    std::optional<setting_t> applied;
    std::uint32_t report_sequence = 0;
    std::int64_t reference_upper_ns = 0, trigger_lower_ns = 0, dispatched_ns = 0;
    int requested_kbps = 0;
    std::uint64_t accepted_revision = 0;
  };
  class episode_t {
  public:
    phase_e phase() const noexcept { return phase_; }
    const evidence_t &evidence() const noexcept { return evidence_; }
    const diagnostic_t &diagnostic() const noexcept { return diagnostic_; }
    bool spent() const noexcept { return phase_ != phase_e::reference; }
    /// Reference-phase clears so far. Diagnostic only; no decision reads it.
    const reference_interruptions_t &reference_interruptions() const noexcept { return interruptions_; }
    /// True while a reference/standing window holds observations that a clear would discard.
    bool reference_window() const noexcept { return phase_ == phase_e::reference && source_.has_value(); }
    /// Diagnostic only, for an owner about to replace this episode for a writer or scope: name the window that
    /// the replacement discards. It touches only the interruption record; every decision field is unchanged.
    void note_writer_or_scope_clear() noexcept { note_reference_clear(reference_clear_e::writer_or_scope); }

    // Missing/unavailable telemetry only destroys an observation window. It cannot undo a cut.
    void unavailable(transition_reason_e reason = transition_reason_e::unavailable) noexcept {
      if (phase_ == phase_e::awaiting_application || phase_ == phase_e::response)
        enter(phase_e::unassessed, reason);
      else if (phase_ == phase_e::reference) { note_reference_clear(clear_of(reason)); clear_reference(); }
    }
    void accepted_application(std::uint64_t revision) noexcept {
      if (phase_ == phase_e::awaiting_application) evidence_.accepted_revision = revision;
    }
    void dispatched_at(std::int64_t now_ns) noexcept {
      if (phase_ == phase_e::awaiting_application) evidence_.dispatched_ns = now_ns;
    }
    void wait_for_application(std::int64_t now_ns) noexcept {
      // Transition telemetry is not clean, loss, response or permission. It cannot move this deadline.
      if (phase_ == phase_e::awaiting_application &&
          (now_ns < evidence_.dispatched_ns || now_ns - evidence_.dispatched_ns > proof_ns))
        enter(phase_e::unassessed, transition_reason_e::proof_deadline);
    }
    void check_deadline(std::int64_t now_ns) noexcept {
      wait_for_application(now_ns);
      if (phase_ == phase_e::response &&
          (now_ns < response_started_ns_ || now_ns - response_started_ns_ > response_ns))
        enter(phase_e::ineffective, transition_reason_e::response_deadline);
    }
    bool pending_setting_matches(const setting_t &s) const noexcept {
      return s == evidence_.source || (evidence_.accepted_revision &&
        s.encoder_lifetime == evidence_.source.encoder_lifetime &&
        s.stream_generation == evidence_.source.stream_generation &&
        s.application_sequence > evidence_.source.application_sequence &&
        s.configured_encoder_kbps == evidence_.requested_kbps &&
        s.origin_revision == evidence_.accepted_revision &&
        s.receipt_origin == video::bitrate_provenance::receipt_origin_e::backend_reconfiguration);
    }

    std::optional<int> observe(const native_video_signal::signal_t &signal) noexcept {
      if (phase_ == phase_e::responded || phase_ == phase_e::ineffective || phase_ == phase_e::unassessed)
        return std::nullopt;
      const auto point = data_median(signal);
      if (!point || !signal.setting || signal.received_steady_ns < 0 || !signal.sequence) {
        if (phase_ == phase_e::awaiting_application && signal.admitted && signal.received_steady_ns >= 0)
          wait_for_application(signal.received_steady_ns);
        else unavailable(transition_reason_e::median_unavailable);
        return std::nullopt;
      }
      const auto now = signal.received_steady_ns;
      if (last_ns_ >= 0 && (now <= last_ns_ || signal.sequence <= last_sequence_ ||
          (phase_ != phase_e::awaiting_application && now - last_ns_ > gap_ns))) {
        unavailable(now <= last_ns_ || signal.sequence <= last_sequence_ ?
          transition_reason_e::order : transition_reason_e::gap);
        if (spent()) return std::nullopt;
      }
      last_ns_ = now; last_sequence_ = signal.sequence;
      if (phase_ == phase_e::awaiting_application) {
        if (now < evidence_.dispatched_ns || now - evidence_.dispatched_ns > proof_ns) {
          enter(phase_e::unassessed, transition_reason_e::proof_deadline); return std::nullopt;
        }
        const auto &s = *signal.setting;
        if (s == evidence_.source) return std::nullopt; // Old in-flight frames earn no response.
        if (!evidence_.accepted_revision || s.encoder_lifetime != evidence_.source.encoder_lifetime ||
            s.stream_generation != evidence_.source.stream_generation ||
            s.application_sequence <= evidence_.source.application_sequence ||
            s.configured_encoder_kbps != evidence_.requested_kbps ||
            s.origin_revision != evidence_.accepted_revision ||
            s.receipt_origin != video::bitrate_provenance::receipt_origin_e::backend_reconfiguration) {
          enter(phase_e::unassessed, transition_reason_e::wrong_application); return std::nullopt;
        }
        evidence_.applied = s;
        enter(phase_e::response, transition_reason_e::backend_tuple_confirmed);
        response_started_ns_ = now;
        clear_bucket(); response_buckets_ = 0;
        response_initial_bucket_ = true;
      }
      if (phase_ == phase_e::response) {
        if (*signal.setting != *evidence_.applied || now < response_started_ns_ ||
            now - response_started_ns_ > response_ns) {
          enter(phase_e::ineffective, *signal.setting != *evidence_.applied ?
            transition_reason_e::setting_changed : transition_reason_e::response_deadline);
          return std::nullopt;
        }
      } else {
        // The same five window-restart conditions, in order; naming the first that holds is diagnostic only.
        const auto restart = !source_ ? reference_clear_e::none :
          *source_ != *signal.setting ? reference_clear_e::source_changed :
          action_revision_ != signal.context.action_revision ? reference_clear_e::action_revision_changed :
          floor_ != signal.context.floor_kbps ? reference_clear_e::floor_changed :
          now - reference_started_ns_ > 60 * second_ns ? reference_clear_e::reference_expired : reference_clear_e::none;
        if (!source_ || restart != reference_clear_e::none) {
          note_reference_clear(restart);
          clear_reference(); source_ = signal.setting; action_revision_ = signal.context.action_revision;
          floor_ = signal.context.floor_kbps; reference_started_ns_ = now;
        }
      }
      const auto second = now / second_ns;
      std::optional<int> request;
      if (bucket_second_ >= 0 && second != bucket_second_) {
        const bool adjacent = second == bucket_second_ + 1;
        const bool covered = adjacent && bucket_count_ >= 4 && bucket_last_ns_ - bucket_first_ns_ >= gap_ns;
        // Only a first bucket whose response began strictly inside its absolute second is a leading partial second.
        // A response that begins exactly on the boundary has a whole-second first bucket under the original rule.
        const bool initial = phase_ == phase_e::response && response_initial_bucket_ &&
          response_started_ns_ % second_ns != 0;
        if (phase_ == phase_e::response) response_initial_bucket_ = false;
        if (initial && adjacent && !covered) {
          // Source behavior correction: the first response bucket starts at the response itself, inside an absolute
          // steady second, so it covers only the rest of that second. If short, discard it with no qualifying or
          // response credit; its reports still passed every gap, order, median, tuple and deadline check above.
          // The six-second deadline, absolute grouping and the rule for every later bucket are unchanged, and a
          // covered first bucket keeps its original credit below.
          note_bucket(bucket_disposition_e::initial_partial_discarded, 0, 0);
        } else if (!covered) {
          if (phase_ == phase_e::response)
            note_bucket(adjacent ? bucket_disposition_e::coverage_short : bucket_disposition_e::second_skipped, 0, 0);
          unavailable(adjacent ? transition_reason_e::bucket_coverage : transition_reason_e::bucket_second_skipped);
          if (spent()) return std::nullopt;
          source_ = signal.setting; action_revision_ = signal.context.action_revision;
          floor_ = signal.context.floor_kbps; reference_started_ns_ = now;
        } else {
          auto lower = lower_, upper = upper_;
          std::sort(lower.begin(), lower.begin() + bucket_count_);
          std::sort(upper.begin(), upper.begin() + bucket_count_);
          request = close_bucket(lower[bucket_count_ / 2], upper[bucket_count_ / 2],
                                 bucket_first_ns_, signal);
        }
        clear_bucket();
      }
      bucket_second_ = second;
      if (bucket_count_ == lower_.size()) {
        if (phase_ == phase_e::response) note_bucket(bucket_disposition_e::overflow, 0, 0);
        unavailable(transition_reason_e::bucket_overflow); return std::nullopt;
      }
      if (!bucket_count_) bucket_first_ns_ = now;
      bucket_last_ns_ = now;
      lower_[bucket_count_] = point->first; upper_[bucket_count_++] = point->second;
      return request;
    }

  private:
    static std::optional<std::pair<std::int64_t, std::int64_t>> data_median(
        const native_video_signal::signal_t &s) noexcept {
      if (!s.admitted || !s.measurement_available || !s.sample_count ||
          s.sample_count > native_video_signal::max_samples) return std::nullopt;
      std::array<std::int64_t, native_video_signal::max_samples> lower {}, upper {};
      std::size_t count = 0;
      for (std::size_t i = 0; i < s.sample_count; ++i) {
        const auto &p = s.samples[i];
        if (p.reason != native_video_signal::reason_e::ready || !p.elapsed_lower_ns || !p.elapsed_upper_ns ||
            *p.elapsed_lower_ns < 0 || *p.elapsed_upper_ns < *p.elapsed_lower_ns ||
            *p.elapsed_upper_ns > 2 * second_ns) return std::nullopt;
        if (!p.parity) { lower[count] = *p.elapsed_lower_ns; upper[count++] = *p.elapsed_upper_ns; }
      }
      if (!count) return std::nullopt;
      std::sort(lower.begin(), lower.begin() + count); std::sort(upper.begin(), upper.begin() + count);
      return std::pair {lower[count / 2], upper[count / 2]};
    }
    void enter(phase_e phase, transition_reason_e reason) noexcept { phase_ = phase; diagnostic_.reason = reason; }
    static reference_clear_e clear_of(transition_reason_e reason) noexcept {
      switch (reason) {
        case transition_reason_e::gap: return reference_clear_e::gap;
        case transition_reason_e::order: return reference_clear_e::order;
        case transition_reason_e::median_unavailable: return reference_clear_e::median_unavailable;
        case transition_reason_e::bucket_coverage: return reference_clear_e::bucket_coverage;
        case transition_reason_e::bucket_second_skipped: return reference_clear_e::bucket_second_skipped;
        case transition_reason_e::bucket_overflow: return reference_clear_e::bucket_overflow;
        case transition_reason_e::permission: return reference_clear_e::permission;
        case transition_reason_e::not_consumed_receipt: return reference_clear_e::not_consumed_receipt;
        case transition_reason_e::not_consumed_measurement: return reference_clear_e::not_consumed_measurement;
        case transition_reason_e::not_consumed_setting: return reference_clear_e::not_consumed_setting;
        case transition_reason_e::not_consumed_current: return reference_clear_e::not_consumed_current;
        case transition_reason_e::policy_refused: return reference_clear_e::policy_refused;
        default: return reference_clear_e::other;
      }
    }
    // Diagnostic only: count a reference/standing window discarded while it held observations, before the clear.
    void note_reference_clear(reference_clear_e why) noexcept {
      if (phase_ != phase_e::reference || !source_ || why == reference_clear_e::none) return;
      auto &r = interruptions_;
      saturating_increment(r.clears[static_cast<std::size_t>(why)]);
      saturating_increment(r.total);
      if (r.first == reference_clear_e::none) r.first = why;
      r.last = why;
      r.last_lost_closed = static_cast<int>(std::min<std::size_t>(prior_count_, 1'000'000));
      r.last_lost_reference = reference_buckets_;
      r.last_lost_standing = standing_buckets_;
      r.max_lost_standing = std::max(r.max_lost_standing, standing_buckets_);
    }
    void note_bucket(bucket_disposition_e disposition, std::int64_t lower, std::int64_t upper) noexcept {
      diagnostic_.bucket = {disposition, bucket_count_, bucket_count_ ? bucket_last_ns_ - bucket_first_ns_ : 0,
                            lower, upper, evidence_.trigger_lower_ns, response_buckets_};
      ++diagnostic_.buckets;
    }
    void clear_bucket() noexcept { bucket_second_ = -1; bucket_count_ = 0; }
    void clear_reference() noexcept {
      source_.reset(); reference_buckets_ = standing_buckets_ = prior_count_ = 0;
      reference_upper_ns_ = trigger_lower_ns_ = 0;
      clear_bucket();
    }
    std::optional<int> close_bucket(std::int64_t lower, std::int64_t upper,
                                    std::int64_t first_ns, const native_video_signal::signal_t &signal) noexcept {
      if (phase_ == phase_e::response) {
        response_buckets_ = upper < evidence_.trigger_lower_ns ? response_buckets_ + 1 : 0;
        note_bucket(upper < evidence_.trigger_lower_ns ? bucket_disposition_e::below_trigger :
                    bucket_disposition_e::not_below_trigger, lower, upper);
        if (response_buckets_ >= 4) enter(phase_e::responded, transition_reason_e::four_buckets_below_trigger);
        return std::nullopt;
      }
      const bool have_prior = prior_count_ >= 3;
      const auto three_ago = prior_[prior_count_ % 3];
      prior_[prior_count_++ % 3] = lower;
      if (reference_buckets_ < 4) {
        reference_upper_ns_ = std::max(reference_upper_ns_, upper); ++reference_buckets_;
        return std::nullopt;
      }
      // Deliberate product prototype amendment: the existing ENet standing geometry,
      // applied to separate native bounds, not the ENet estimator or a learned floor.
      const bool elevated = lower > reference_upper_ns_ + std::max<std::int64_t>(10'000'000, reference_upper_ns_ / 2);
      const bool standing = elevated && have_prior && lower >= three_ago - 500'000;
      if (!standing) { standing_buckets_ = 0; return std::nullopt; }
      if (!standing_buckets_) { standing_started_ns_ = first_ns; trigger_lower_ns_ = lower; }
      else trigger_lower_ns_ = std::min(trigger_lower_ns_, lower);
      ++standing_buckets_;
      if (standing_buckets_ < 8 || signal.received_steady_ns - standing_started_ns_ < 8 * second_ns)
        return std::nullopt;
      const int from = source_->configured_encoder_kbps;
      const int next = std::max(floor_, static_cast<int>((static_cast<std::int64_t>(from) * 85 + 50) / 100));
      if (floor_ <= 0 || next >= from) return std::nullopt;
      evidence_ = {*source_, std::nullopt, signal.sequence, reference_upper_ns_, trigger_lower_ns_,
                   signal.received_steady_ns, next, 0};
      enter(phase_e::awaiting_application, transition_reason_e::standing_trigger);
      clear_bucket();
      return next;
    }
    phase_e phase_ = phase_e::reference;
    evidence_t evidence_;
    diagnostic_t diagnostic_;
    reference_interruptions_t interruptions_;
    bool response_initial_bucket_ = false;
    std::optional<setting_t> source_;
    std::uint64_t action_revision_ = 0;
    std::uint32_t last_sequence_ = 0;
    std::int64_t last_ns_ = -1, reference_started_ns_ = 0, response_started_ns_ = 0;
    std::int64_t bucket_second_ = -1, bucket_first_ns_ = 0, bucket_last_ns_ = 0;
    std::array<std::int64_t, 32> lower_ {}, upper_ {};
    std::size_t bucket_count_ = 0;
    std::array<std::int64_t, 3> prior_ {};
    std::size_t prior_count_ = 0;
    int reference_buckets_ = 0, standing_buckets_ = 0, response_buckets_ = 0, floor_ = 0;
    std::int64_t reference_upper_ns_ = 0, trigger_lower_ns_ = 0, standing_started_ns_ = 0;
  };
} // namespace live_tuning_native
