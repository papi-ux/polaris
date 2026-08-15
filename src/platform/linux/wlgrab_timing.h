#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace wl {
  struct extcopy_timing_sample_t {
    std::optional<std::chrono::microseconds> request_to_ready;
    std::optional<std::chrono::microseconds> presentation_interval;
    std::optional<std::chrono::steady_clock::time_point> ready_at;
  };

  class extcopy_timing_tracker_t {
  public:
    using time_point_t = std::chrono::steady_clock::time_point;

    void reset() {
      request_started_at.reset();
      current_presentation_ns.reset();
      previous_presentation_ns.reset();
      sample = {};
    }

    void mark_requested(time_point_t now) {
      request_started_at = now;
      current_presentation_ns.reset();
    }

    bool mark_presentation(std::uint32_t tv_sec_hi, std::uint32_t tv_sec_lo, std::uint32_t tv_nsec) {
      if (tv_nsec >= 1'000'000'000U) {
        current_presentation_ns.reset();
        return false;
      }

      const std::uint64_t seconds =
        (static_cast<std::uint64_t>(tv_sec_hi) << 32U) |
        static_cast<std::uint64_t>(tv_sec_lo);
      if (seconds > (UINT64_MAX - tv_nsec) / 1'000'000'000ULL) {
        current_presentation_ns.reset();
        return false;
      }

      current_presentation_ns = seconds * 1'000'000'000ULL + tv_nsec;
      return true;
    }

    void mark_ready(time_point_t now) {
      sample = {};
      sample.ready_at = now;
      if (request_started_at && now >= *request_started_at) {
        sample.request_to_ready = std::chrono::duration_cast<std::chrono::microseconds>(
          now - *request_started_at
        );
      }

      if (current_presentation_ns) {
        if (previous_presentation_ns && *current_presentation_ns >= *previous_presentation_ns) {
          sample.presentation_interval = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::nanoseconds {*current_presentation_ns - *previous_presentation_ns}
          );
        }
        previous_presentation_ns = current_presentation_ns;
      }

      request_started_at.reset();
      current_presentation_ns.reset();
    }

    const extcopy_timing_sample_t &last_sample() const {
      return sample;
    }

  private:
    std::optional<time_point_t> request_started_at;
    std::optional<std::uint64_t> current_presentation_ns;
    std::optional<std::uint64_t> previous_presentation_ns;
    extcopy_timing_sample_t sample;
  };
}  // namespace wl
