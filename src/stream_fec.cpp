/**
 * @file src/stream_fec.cpp
 * @brief Bounded diagnostics for video frames outside the FEC envelope.
 */

#include "stream_fec.h"

#include <utility>

namespace stream {
  fec_warning_tracker_t::fec_warning_tracker_t(
    std::chrono::steady_clock::duration report_interval
  ):
      report_interval_ {report_interval} {
  }

  void fec_warning_tracker_t::accumulate(
    oversized_fec_frame_summary_t &summary,
    const oversized_fec_frame_observation_t &observation
  ) {
    ++summary.frame_count;
    if (observation.frame_type == fec_frame_type_e::idr) {
      ++summary.idr_frame_count;
    } else if (observation.frame_type == fec_frame_type_e::reference_invalidation) {
      ++summary.reference_invalidation_frame_count;
    }

    const bool new_largest =
      observation.required_blocks > summary.max_required_blocks ||
      (observation.required_blocks == summary.max_required_blocks &&
       observation.packetized_frame_bytes > summary.largest_packetized_frame_bytes);
    if (new_largest) {
      summary.largest_encoded_frame_bytes = observation.encoded_frame_bytes;
      summary.largest_packetized_frame_bytes = observation.packetized_frame_bytes;
      summary.max_required_blocks = observation.required_blocks;
      summary.largest_frame_type = observation.frame_type;
    }

    // These describe the current session envelope. Keep the latest values in
    // case a future live configuration path changes them between reports.
    summary.protected_payload_limit_bytes = observation.protected_payload_limit_bytes;
    summary.fec_percentage = observation.fec_percentage;
    summary.packet_size = observation.packet_size;
  }

  std::optional<oversized_fec_frame_summary_t> fec_warning_tracker_t::observe(
    const oversized_fec_frame_observation_t &observation,
    clock::time_point now
  ) {
    if (!last_report_at_) {
      oversized_fec_frame_summary_t first;
      accumulate(first, observation);
      last_report_at_ = now;
      return first;
    }

    accumulate(pending_, observation);
    if (now - *last_report_at_ < report_interval_) {
      return std::nullopt;
    }

    last_report_at_ = now;
    return std::exchange(pending_, {});
  }
}  // namespace stream
