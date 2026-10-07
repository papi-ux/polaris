#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

#include "src/vrr_session_policy.h"

namespace wl {
  struct extcopy_timing_sample_t {
    std::optional<std::chrono::microseconds> request_to_ready;
    /// From the last VALID presentation to this one, present only when this one is valid. Invalid
    /// values never become the baseline, so an interval may span invalid samples between two valid
    /// ones but is never measured from or to an invalid value.
    std::optional<std::chrono::microseconds> presentation_interval;
    std::optional<std::chrono::steady_clock::time_point> ready_at;
    /// The compositor's presentation time exactly as delivered, in system monotonic nanoseconds
    /// (ext-image-copy-capture-v1 presentation_time, wayland-protocols 0091197f). Kept for every
    /// frame that carried a representable value, including a nonmonotonic one, so nothing is
    /// corrected or estimated. Empty when the frame carried none or an unrepresentable one.
    std::optional<std::int64_t> presentation_ns;
    vrr_policy::presentation_status_e presentation_status = vrr_policy::presentation_status_e::missing;
    /// This tracker's session: 1 at first, advanced by every reset(). It saturates at its maximum and
    /// never wraps back to an earlier session's value.
    std::uint64_t baseline = 0;
  };

  class extcopy_timing_tracker_t {
  public:
    using time_point_t = std::chrono::steady_clock::time_point;

    extcopy_timing_tracker_t() = default;
    /// Start at a given session number; production starts at 1.
    explicit extcopy_timing_tracker_t(std::uint64_t first_baseline):
        baseline_ {first_baseline} {}

    void reset() {
      if (baseline_ != std::numeric_limits<std::uint64_t>::max()) ++baseline_;
      request_started_at.reset();
      current_presentation_ns.reset();
      current_presentation_invalid.reset();
      last_valid_presentation_ns.reset();
      sample = {};
    }

    void mark_requested(time_point_t now) {
      request_started_at = now;
      current_presentation_ns.reset();
      current_presentation_invalid.reset();
    }

    bool mark_presentation(std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec) {
      current_presentation_ns.reset();
      current_presentation_invalid.reset();
      if (tv_nsec >= 1'000'000'000U) {
        current_presentation_invalid = vrr_policy::presentation_status_e::invalid_nsec;
        return false;
      }

      const std::uint64_t seconds =
        (static_cast<std::uint64_t>(tv_sec_hi) << 32U) |
        static_cast<std::uint64_t>(tv_sec_lo);
      // Bound by signed nanoseconds so the value maps onto std::chrono and steady_clock without wrap.
      constexpr auto max_ns = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
      if (seconds > (max_ns - tv_nsec) / 1'000'000'000ULL) {
        current_presentation_invalid = vrr_policy::presentation_status_e::invalid_overflow;
        return false;
      }

      current_presentation_ns = static_cast<std::int64_t>(seconds * 1'000'000'000ULL + tv_nsec);
      return true;
    }

    void mark_ready(time_point_t now) {
      sample = {};
      sample.ready_at = now;
      sample.baseline = baseline_;
      if (request_started_at && now >= *request_started_at) {
        sample.request_to_ready = std::chrono::duration_cast<std::chrono::microseconds>(
          now - *request_started_at
        );
      }

      if (current_presentation_ns) {
        sample.presentation_ns = current_presentation_ns;
        if (last_valid_presentation_ns && *current_presentation_ns <= *last_valid_presentation_ns) {
          // Retained but never trusted, and it does not move the baseline: one backward sample must not
          // let a later frame pass as monotonic against it.
          sample.presentation_status = vrr_policy::presentation_status_e::nonmonotonic;
        } else {
          sample.presentation_status = vrr_policy::presentation_status_e::valid;
          if (last_valid_presentation_ns) {
            sample.presentation_interval = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::nanoseconds {*current_presentation_ns - *last_valid_presentation_ns}
            );
          }
          last_valid_presentation_ns = current_presentation_ns;
        }
      } else if (current_presentation_invalid) {
        sample.presentation_status = *current_presentation_invalid;
      } else {
        sample.presentation_status = vrr_policy::presentation_status_e::missing;
      }

      request_started_at.reset();
      current_presentation_ns.reset();
      current_presentation_invalid.reset();
    }

    const extcopy_timing_sample_t &last_sample() const {
      return sample;
    }

    std::uint64_t baseline() const {
      return baseline_;
    }

  private:
    std::optional<time_point_t> request_started_at;
    std::optional<std::int64_t> current_presentation_ns;
    std::optional<vrr_policy::presentation_status_e> current_presentation_invalid;
    std::optional<std::int64_t> last_valid_presentation_ns;
    extcopy_timing_sample_t sample;
    std::uint64_t baseline_ = 1;
  };
}  // namespace wl
