/**
 * @file src/video_frame_pacing.h
 * @brief Timestamp pacing for captured frames entering the encoder.
 */
#pragma once

#include <chrono>
#include <optional>

namespace video {

  struct frame_timestamp_decision_t {
    bool should_encode;
    std::chrono::steady_clock::time_point timestamp;
  };

  class frame_timestamp_pacer_t {
  public:
    frame_timestamp_pacer_t(
      std::chrono::steady_clock::duration frame_interval,
      std::chrono::steady_clock::duration variation_tolerance
    ):
        frame_interval_ {frame_interval},
        variation_tolerance_ {variation_tolerance} {
    }

    frame_timestamp_decision_t evaluate(std::chrono::steady_clock::time_point source_timestamp) {
      if (!next_frame_timestamp_) {
        next_frame_timestamp_ = source_timestamp;
      }

      const auto time_diff = source_timestamp - *next_frame_timestamp_;
      if (time_diff < -variation_tolerance_) {
        return {false, source_timestamp};
      }

      auto encode_timestamp = source_timestamp;
      if (time_diff < variation_tolerance_) {
        encode_timestamp = *next_frame_timestamp_;
      } else {
        // Preserve fractional phase across late frames. Snapping the schedule to
        // the source here makes small refresh-rate differences over-drop.
        const auto elapsed_intervals = time_diff / frame_interval_;
        *next_frame_timestamp_ += frame_interval_ * elapsed_intervals;
      }

      *next_frame_timestamp_ += frame_interval_;
      return {true, encode_timestamp};
    }

  private:
    std::chrono::steady_clock::duration frame_interval_;
    std::chrono::steady_clock::duration variation_tolerance_;
    std::optional<std::chrono::steady_clock::time_point> next_frame_timestamp_;
  };

}  // namespace video
