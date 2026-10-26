/**
 * @file src/stream_fec.cpp
 * @brief Bounded diagnostics for video frames outside the FEC envelope.
 */

#include "stream_fec.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace stream {
  std::optional<fec_frame_fit_t> fit_frame_fec(
    std::size_t packetized_bytes,
    std::size_t block_bytes,
    int configured_percentage,
    int requested_percentage,
    int minimum_parity_packets
  ) {
    if (packetized_bytes == 0 || block_bytes == 0 ||
        configured_percentage < 0 || configured_percentage > 255 ||
        requested_percentage < 0 || requested_percentage > 255 ||
        minimum_parity_packets < 0 || minimum_parity_packets > 255) {
      return std::nullopt;
    }
    const auto ceil_div = [](std::size_t value, std::size_t divisor) {
      return value / divisor + (value % divisor != 0);
    };
    const auto plan = [&](int percentage) -> std::optional<fec_frame_fit_t> {
      const std::size_t data_limit = 25500 / (100 + percentage);
      if (block_bytes > std::numeric_limits<std::size_t>::max() /
                          (data_limit * MAX_VIDEO_FEC_BLOCKS)) {
        return std::nullopt;
      }
      const auto block_limit = data_limit * block_bytes;
      const auto needed = ceil_div(packetized_bytes, block_limit);
      return fec_frame_fit_t {
        .fec_percentage = needed > MAX_VIDEO_FEC_BLOCKS ? 0 : percentage,
        .envelope_percentage = percentage,
        .required_blocks = needed,
        .block_count = std::min(needed, MAX_VIDEO_FEC_BLOCKS),
        .protected_payload_limit_bytes = block_limit * MAX_VIDEO_FEC_BLOCKS
      };
    };
    const auto protected_blocks_fit = [&](const fec_frame_fit_t &candidate) {
      if (candidate.fec_percentage == 0) {
        return true;
      }
      // Match the sender: align each earlier block upward; the last gets the
      // remainder. Check actual parity, including the client's small-block
      // minimum, rather than assuming the nominal percentage is the wire F.
      const auto aligned = ceil_div(packetized_bytes / candidate.block_count, block_bytes) * block_bytes;
      const auto earlier = (candidate.block_count - 1) * aligned;
      if (earlier >= packetized_bytes) {
        return false;
      }
      for (std::size_t i = 0; i < candidate.block_count; ++i) {
        const auto bytes = i + 1 == candidate.block_count ? packetized_bytes - earlier : aligned;
        const auto data = ceil_div(bytes, block_bytes);
        const auto nominal = (data * candidate.fec_percentage + 99) / 100;
        const auto parity = std::max(nominal, static_cast<std::size_t>(minimum_parity_packets));
        const auto effective = nominal < parity ? 100 * parity / data : candidate.fec_percentage;
        if (data + parity > 255 || effective > 255) {
          return false;
        }
      }
      return true;
    };

    const int target = configured_percentage >= 50 ? configured_percentage :
      std::clamp(requested_percentage, configured_percentage, 50);
    for (int percentage = target; percentage >= configured_percentage; --percentage) {
      const auto candidate = plan(percentage);
      if (!candidate) {
        return std::nullopt;
      }
      if (candidate->required_blocks <= MAX_VIDEO_FEC_BLOCKS && protected_blocks_fit(*candidate)) {
        return candidate;
      }
    }
    // Keep the configured envelope for the existing warning and zero-parity
    // fallback. Do not present an unsafe minimum-parity block as protected.
    const auto baseline = plan(configured_percentage);
    if (baseline && baseline->required_blocks > MAX_VIDEO_FEC_BLOCKS) {
      return baseline;
    }
    return std::nullopt;
  }

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
