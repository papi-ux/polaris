#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace wl {
  enum class capture_pacing_policy_e {
    fixed_interval,
    source_driven,
  };

  inline capture_pacing_policy_e screencopy_pacing_policy(bool private_compositor_capture) {
    // wlr-screencopy completes on a compositor output frame. Adding a second
    // fixed timer in front of the private compositor can miss every next tick
    // and turn a 120 Hz output into a metronomic ~60 FPS capture source.
    return private_compositor_capture ?
      capture_pacing_policy_e::source_driven :
      capture_pacing_policy_e::fixed_interval;
  }

  inline const char *capture_pacing_policy_name(capture_pacing_policy_e policy) {
    return policy == capture_pacing_policy_e::source_driven ? "source_driven" : "fixed_interval";
  }

  class capture_source_rate_tracker_t {
  public:
    using clock_t = std::chrono::steady_clock;
    using time_point_t = clock_t::time_point;

    std::optional<double> note_frame(time_point_t now) {
      if (!window_start_) {
        window_start_ = now;
        return std::nullopt;
      }

      ++frame_intervals_;
      const auto elapsed = std::chrono::duration<double>(now - *window_start_).count();
      if (elapsed < 1.0) {
        return std::nullopt;
      }

      const double fps = static_cast<double>(frame_intervals_) / elapsed;
      window_start_ = now;
      frame_intervals_ = 0;
      return fps;
    }

  private:
    std::optional<time_point_t> window_start_;
    std::uint64_t frame_intervals_ = 0;
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
