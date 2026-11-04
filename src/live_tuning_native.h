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
    bool spent() const noexcept { return phase_ != phase_e::reference; }

    // Missing/unavailable telemetry only destroys an observation window. It cannot undo a cut.
    void unavailable() noexcept {
      if (phase_ == phase_e::awaiting_application || phase_ == phase_e::response)
        phase_ = phase_e::unassessed;
      else if (phase_ == phase_e::reference) clear_reference();
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
        phase_ = phase_e::unassessed;
    }
    void check_deadline(std::int64_t now_ns) noexcept {
      wait_for_application(now_ns);
      if (phase_ == phase_e::response &&
          (now_ns < response_started_ns_ || now_ns - response_started_ns_ > response_ns))
        phase_ = phase_e::ineffective;
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
        else unavailable();
        return std::nullopt;
      }
      const auto now = signal.received_steady_ns;
      if (last_ns_ >= 0 && (now <= last_ns_ || signal.sequence <= last_sequence_ ||
          (phase_ != phase_e::awaiting_application && now - last_ns_ > gap_ns))) {
        unavailable();
        if (spent()) return std::nullopt;
      }
      last_ns_ = now; last_sequence_ = signal.sequence;
      if (phase_ == phase_e::awaiting_application) {
        if (now < evidence_.dispatched_ns || now - evidence_.dispatched_ns > proof_ns) {
          phase_ = phase_e::unassessed; return std::nullopt;
        }
        const auto &s = *signal.setting;
        if (s == evidence_.source) return std::nullopt; // Old in-flight frames earn no response.
        if (!evidence_.accepted_revision || s.encoder_lifetime != evidence_.source.encoder_lifetime ||
            s.stream_generation != evidence_.source.stream_generation ||
            s.application_sequence <= evidence_.source.application_sequence ||
            s.configured_encoder_kbps != evidence_.requested_kbps ||
            s.origin_revision != evidence_.accepted_revision ||
            s.receipt_origin != video::bitrate_provenance::receipt_origin_e::backend_reconfiguration) {
          phase_ = phase_e::unassessed; return std::nullopt;
        }
        evidence_.applied = s;
        phase_ = phase_e::response;
        response_started_ns_ = now;
        clear_bucket(); response_buckets_ = 0;
      }
      if (phase_ == phase_e::response) {
        if (*signal.setting != *evidence_.applied || now < response_started_ns_ ||
            now - response_started_ns_ > response_ns) {
          phase_ = phase_e::ineffective; return std::nullopt;
        }
      } else {
        if (!source_ || *source_ != *signal.setting || action_revision_ != signal.context.action_revision ||
            floor_ != signal.context.floor_kbps || now - reference_started_ns_ > 60 * second_ns) {
          clear_reference(); source_ = signal.setting; action_revision_ = signal.context.action_revision;
          floor_ = signal.context.floor_kbps; reference_started_ns_ = now;
        }
      }
      const auto second = now / second_ns;
      std::optional<int> request;
      if (bucket_second_ >= 0 && second != bucket_second_) {
        if (second != bucket_second_ + 1 || bucket_count_ < 4 || bucket_last_ns_ - bucket_first_ns_ < gap_ns) {
          unavailable();
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
      if (bucket_count_ == lower_.size()) { unavailable(); return std::nullopt; }
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
        if (response_buckets_ >= 4) phase_ = phase_e::responded;
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
      phase_ = phase_e::awaiting_application;
      clear_bucket();
      return next;
    }
    phase_e phase_ = phase_e::reference;
    evidence_t evidence_;
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
