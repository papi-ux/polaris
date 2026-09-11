/**
 * @file src/multiseat_worker_protocol.h
 * @brief Bounded, generation-fenced protocol for local multiseat workers.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace multiseat::worker_ipc {

  inline constexpr std::size_t header_size = 32;
  inline constexpr std::size_t capability_size = 32;
  inline constexpr std::size_t proof_size = 32;
  inline constexpr std::size_t challenge_size = 32;
  inline constexpr std::size_t max_control_payload = 64 * 1024;
  inline constexpr std::size_t max_media_payload = 16 * 1024 * 1024;
  /** Fixed big-endian body of a media_config control message. */
  inline constexpr std::size_t media_config_size = 32;
  /** Fixed big-endian prefix inside every video and audio media payload. */
  inline constexpr std::size_t media_frame_prefix_size = 32;
  /** Fixed big-endian body of an invalidate_ref_frames control message. */
  inline constexpr std::size_t frame_range_size = 16;
  inline constexpr std::uint8_t media_contract_version = 1;

  enum class channel_e : std::uint8_t {
    control = 1,
    media = 2,
  };

  enum class message_e : std::uint8_t {
    challenge = 1,
    authenticate = 2,
    authenticated = 3,
    heartbeat = 4,
    heartbeat_ack = 5,

    ready = 16,
    shutdown = 17,
    shutdown_ack = 18,
    input = 19,
    feedback = 20,
    error = 21,
    attach = 22,
    attached = 23,
    input_ack = 24,
    /** Worker to controller on the media channel, once, before any frame. */
    media_config = 25,
    /** Controller to worker on control; the worker sends no media before it. */
    media_config_ack = 26,
    /** Controller to worker; the next produced frame must be an IDR. */
    request_idr = 27,
    /** Controller to worker; body is an inclusive frame_range_t. */
    invalidate_ref_frames = 28,
    /** Worker to controller; the one acknowledgement for the three above. */
    media_control_ack = 29,

    video = 32,
    audio = 33,
    end_of_stream = 34,
    discontinuity = 35,
  };

  enum class parse_status_e {
    complete,
    incomplete,
    rejected,
  };

  enum class proof_role_e : std::uint8_t {
    controller = 1,
    worker = 2,
  };

  using capability_t = std::array<std::uint8_t, capability_size>;
  using challenge_t = std::array<std::uint8_t, challenge_size>;
  using proof_t = std::array<std::uint8_t, proof_size>;

  /** Identity mixed into every authentication proof. */
  struct endpoint_identity_t {
    std::string controller_epoch;
    std::string logical_gpu_id;
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    std::string worker_name;

    bool operator==(const endpoint_identity_t &) const = default;
  };

  struct frame_t {
    channel_e channel = channel_e::control;
    message_e message = message_e::heartbeat;
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> payload;

    bool operator==(const frame_t &) const = default;
  };

  enum class video_codec_e : std::uint8_t {
    h264 = 1,
  };

  enum class audio_codec_e : std::uint8_t {
    opus = 1,
  };

  /**
   * The media contract one worker produces for the lifetime of its data
   * plane. The worker announces it as the first frame on the media channel and
   * sends nothing else there until the controller acknowledges it on control.
   * Only H.264 video and 48 kHz Opus audio are representable in this version.
   */
  struct media_config_t {
    video_codec_e video_codec = video_codec_e::h264;
    std::uint8_t profile_idc = 0;
    std::uint8_t level_idc = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t fps_numerator = 0;
    std::uint32_t fps_denominator = 0;
    std::uint32_t bitrate_ceiling_kbps = 0;
    audio_codec_e audio_codec = audio_codec_e::opus;
    std::uint8_t audio_channels = 0;
    std::uint16_t audio_frame_duration_us = 0;
    std::uint32_t audio_sample_rate = 0;

    bool operator==(const media_config_t &) const = default;
  };

  /** Prefix carried inside every video and audio payload, ahead of the bytes. */
  struct media_frame_t {
    std::uint64_t frame_index = 0;
    bool idr = false;
    /** CLOCK_MONOTONIC nanoseconds on the shared kernel; zero means unknown. */
    std::uint64_t capture_timestamp_ns = 0;
    std::uint64_t encode_timestamp_ns = 0;

    bool operator==(const media_frame_t &) const = default;
  };

  /** Inclusive frame index range whose references must not be used again. */
  struct frame_range_t {
    std::uint64_t first = 0;
    std::uint64_t last = 0;

    bool operator==(const frame_range_t &) const = default;
  };

  /**
   * Result from parsing one frame at the beginning of a byte stream.
   *
   * A rejected result never trusts the advertised length. An incomplete
   * result reports the exact bounded byte count needed for one frame.
   */
  struct parse_result_t {
    parse_status_e status = parse_status_e::rejected;
    std::optional<frame_t> frame;
    std::size_t consumed = 0;
    std::size_t required = 0;
  };

  /** Per-direction replay/gap guard; a new connection starts at sequence 1. */
  class sequence_guard_t {
  public:
    [[nodiscard]] bool accept(std::uint64_t sequence);
    [[nodiscard]] std::uint64_t next() const;

  private:
    std::uint64_t next_ = 1;
    bool exhausted_ = false;
  };

  [[nodiscard]] bool valid_identity(const endpoint_identity_t &identity);
  [[nodiscard]] std::optional<capability_t> parse_capability_hex(
    std::string_view encoded
  );
  [[nodiscard]] std::string capability_hex(const capability_t &capability);

  /** HMAC-SHA256 proof bound to role, channel, exact seat, and challenge. */
  [[nodiscard]] std::optional<proof_t> authentication_proof(
    const capability_t &capability,
    proof_role_e role,
    channel_e channel,
    const endpoint_identity_t &identity,
    const challenge_t &challenge
  );
  [[nodiscard]] bool verify_authentication_proof(
    const capability_t &capability,
    proof_role_e role,
    channel_e channel,
    const endpoint_identity_t &identity,
    const challenge_t &challenge,
    std::span<const std::uint8_t> presented
  );

  /** Encode one valid protocol frame, throwing on an invalid combination. */
  [[nodiscard]] std::vector<std::uint8_t> encode_frame(const frame_t &frame);

  /**
   * Parse one frame and fence it to the expected channel and exact seat
   * generation. Extra bytes are left to the caller via `consumed`.
   */
  [[nodiscard]] parse_result_t parse_frame(
    std::span<const std::uint8_t> bytes,
    channel_e expected_channel,
    std::uint32_t expected_slot,
    std::uint64_t expected_generation
  );

  /**
   * A representable contract: known codecs, even geometry between 16 and
   * 16384 pixels, a positive frame rate of at most 1000 Hz, a bitrate ceiling
   * of at most 1 Gbit/s, 48 kHz Opus with one to eight channels and a legal
   * Opus frame duration.
   */
  [[nodiscard]] bool valid_media_config(const media_config_t &config);
  /** Encode a valid media_config body, throwing std::invalid_argument otherwise. */
  [[nodiscard]] std::vector<std::uint8_t> encode_media_config(const media_config_t &config);
  /** Parse and validate one exact media_config body; unknown versions fail. */
  [[nodiscard]] std::optional<media_config_t> parse_media_config(
    std::span<const std::uint8_t> body
  );

  /**
   * Build one media payload: the fixed prefix followed by the encoded bytes.
   * Throws std::invalid_argument for an empty frame or one over the limit.
   */
  [[nodiscard]] std::vector<std::uint8_t> encode_media_frame(
    const media_frame_t &frame,
    std::span<const std::uint8_t> encoded
  );
  /**
   * Split one media payload into its prefix and encoded bytes. Rejects an
   * unknown version, unknown flags, non-zero reserved bytes and a payload
   * without at least one encoded byte after the prefix.
   */
  [[nodiscard]] std::optional<media_frame_t> parse_media_frame(
    std::span<const std::uint8_t> payload,
    std::span<const std::uint8_t> &encoded
  );

  [[nodiscard]] std::vector<std::uint8_t> encode_frame_range(const frame_range_t &range);
  /** Parse one exact frame range body; first must not exceed last. */
  [[nodiscard]] std::optional<frame_range_t> parse_frame_range(
    std::span<const std::uint8_t> body
  );

}  // namespace multiseat::worker_ipc
