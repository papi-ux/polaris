#pragma once

#include <chrono>

namespace wl {
  enum class capture_pacing_policy_e {
    fixed_interval,
    source_driven,
  };

  class capture_pacer_t {
  public:
    using clock_t = std::chrono::steady_clock;
    using duration_t = clock_t::duration;
    using time_point_t = clock_t::time_point;

    capture_pacer_t(
      capture_pacing_policy_e policy,
      duration_t interval,
      time_point_t start
    ):
        policy_ {policy},
        interval_ {interval},
        next_frame_ {start} {
    }

    duration_t wait_duration(time_point_t now) const {
      if (policy_ == capture_pacing_policy_e::source_driven || next_frame_ <= now) {
        return duration_t::zero();
      }

      return next_frame_ - now;
    }

    void advance(time_point_t now) {
      if (policy_ == capture_pacing_policy_e::source_driven) {
        return;
      }

      next_frame_ += interval_;
      if (next_frame_ < now) {
        next_frame_ = now + interval_;
      }
    }

  private:
    capture_pacing_policy_e policy_;
    duration_t interval_;
    time_point_t next_frame_;
  };
}  // namespace wl
