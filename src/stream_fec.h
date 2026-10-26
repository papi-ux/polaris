/**
 * @file src/stream_fec.h
 * @brief Bounded diagnostics for video frames outside the FEC envelope.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace stream {
  // The wire header dedicates two bits to the FEC block count.
  inline constexpr std::size_t MAX_VIDEO_FEC_BLOCKS = 4;

  struct fec_frame_fit_t {
    int fec_percentage;
    // The envelope used for the existing oversized-frame warning. When parity
    // is skipped this is still the configured percentage, not zero.
    int envelope_percentage;
    std::size_t required_blocks;
    std::size_t block_count;
    std::size_t protected_payload_limit_bytes;
  };

  /**
   * Fit a session's requested parity to this packetized frame, before splitting
   * it into blocks. A raised target may step down as far as the configured
   * percentage; it must not strip parity from a frame the configured value fits.
   *
   * Uses stream.cpp's nominal block sizing, alignment and minimum-parity rule.
   * A configured value >= 50 stays fixed. An oversized configured envelope
   * keeps the existing zero-parity/four-block fallback and warning inputs.
   * nullopt means invalid arithmetic or an unsafe protected minimum-parity
   * envelope; the caller must not activate an adaptive target from that result.
   * This does not validate the unprotected frame's packet-index limit, prove a
   * wire bitrate, or authorize an encoder change.
   */
  std::optional<fec_frame_fit_t> fit_frame_fec(
    std::size_t packetized_bytes,
    std::size_t block_bytes,
    int configured_percentage,
    int requested_percentage,
    int minimum_parity_packets
  );

  enum class fec_frame_type_e {
    delta,
    idr,
    reference_invalidation
  };

  constexpr std::string_view fec_frame_type_name(fec_frame_type_e type) {
    switch (type) {
      case fec_frame_type_e::idr:
        return "idr";
      case fec_frame_type_e::reference_invalidation:
        return "reference_invalidation";
      default:
        return "delta";
    }
  }

  struct oversized_fec_frame_observation_t {
    std::size_t encoded_frame_bytes = 0;
    std::size_t packetized_frame_bytes = 0;
    std::size_t protected_payload_limit_bytes = 0;
    std::size_t required_blocks = 0;
    int fec_percentage = 0;
    int packet_size = 0;
    fec_frame_type_e frame_type = fec_frame_type_e::delta;
  };

  struct oversized_fec_frame_summary_t {
    std::uint64_t frame_count = 0;
    std::uint64_t idr_frame_count = 0;
    std::uint64_t reference_invalidation_frame_count = 0;
    std::size_t largest_encoded_frame_bytes = 0;
    std::size_t largest_packetized_frame_bytes = 0;
    std::size_t protected_payload_limit_bytes = 0;
    std::size_t max_required_blocks = 0;
    int fec_percentage = 0;
    int packet_size = 0;
    fec_frame_type_e largest_frame_type = fec_frame_type_e::delta;
  };

  /**
   * @brief Emit the first oversized frame immediately, then aggregate repeats.
   *
   * One instance belongs to one stream session and is called only from the
   * video broadcast thread. This keeps the hot path lock-free while preventing
   * a high-bitrate stream from filling the bounded async logging queue.
   */
  class fec_warning_tracker_t {
  public:
    using clock = std::chrono::steady_clock;

    explicit fec_warning_tracker_t(
      std::chrono::steady_clock::duration report_interval = std::chrono::seconds(20)
    );

    std::optional<oversized_fec_frame_summary_t> observe(
      const oversized_fec_frame_observation_t &observation,
      clock::time_point now = clock::now()
    );

  private:
    static void accumulate(
      oversized_fec_frame_summary_t &summary,
      const oversized_fec_frame_observation_t &observation
    );

    std::chrono::steady_clock::duration report_interval_;
    std::optional<clock::time_point> last_report_at_;
    oversized_fec_frame_summary_t pending_;
  };
}  // namespace stream
