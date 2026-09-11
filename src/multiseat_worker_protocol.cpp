/**
 * @file src/multiseat_worker_protocol.cpp
 * @brief Bounded, generation-fenced protocol for local multiseat workers.
 */
#include "multiseat_worker_protocol.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace multiseat::worker_ipc {
  namespace {
    constexpr std::array<std::uint8_t, 4> magic {'P', 'S', 'W', '1'};
    constexpr std::string_view authentication_domain =
      "polaris-seat-worker-ipc-v1";

    bool ascii_alphanumeric(char value) {
      return (value >= 'a' && value <= 'z') ||
             (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9');
    }

    bool opaque_name_token(std::string_view value, std::size_t max_size = 128) {
      return !value.empty() &&
             value.size() <= max_size &&
             ascii_alphanumeric(value.front()) &&
             std::all_of(
               value.begin(),
               value.end(),
               [](char character) {
                 return ascii_alphanumeric(character) ||
                        character == '-' ||
                        character == '_' ||
                        character == '.';
               }
             );
    }

    bool valid_channel(channel_e channel) {
      return channel == channel_e::control || channel == channel_e::media;
    }

    bool valid_message(
      channel_e channel,
      message_e message,
      std::size_t payload_size
    ) {
      const auto fixed_proof = payload_size == proof_size;
      switch (message) {
        case message_e::challenge:
        case message_e::authenticate:
        case message_e::authenticated:
          return fixed_proof;
        case message_e::heartbeat:
        case message_e::heartbeat_ack:
          return payload_size == 0;
        case message_e::ready:
          return channel == channel_e::control &&
                 payload_size > 0 && payload_size <= 4096;
        case message_e::shutdown:
        case message_e::shutdown_ack:
          return channel == channel_e::control && payload_size == 0;
        case message_e::attach:
        case message_e::attached:
          return payload_size == 0;
        case message_e::input_ack:
          return channel == channel_e::control && payload_size == 0;
        case message_e::media_config:
          // The contract rides the channel it describes, ahead of the frames it
          // describes, so it can never be read out of order with them.
          return channel == channel_e::media && payload_size == media_config_size;
        case message_e::media_config_ack:
        case message_e::request_idr:
        case message_e::media_control_ack:
          return channel == channel_e::control && payload_size == 0;
        case message_e::invalidate_ref_frames:
          return channel == channel_e::control && payload_size == frame_range_size;
        case message_e::input:
        case message_e::feedback:
          return channel == channel_e::control && payload_size > 0;
        case message_e::error:
          return channel == channel_e::control &&
                 payload_size > 0 && payload_size <= 4096;
        case message_e::video:
        case message_e::audio:
          return channel == channel_e::media && payload_size > 0;
        case message_e::end_of_stream:
        case message_e::discontinuity:
          return channel == channel_e::media && payload_size == 0;
      }
      return false;
    }

    std::size_t payload_limit(channel_e channel) {
      return channel == channel_e::control ? max_control_payload : max_media_payload;
    }

    void append_u16(std::vector<std::uint8_t> &bytes, std::uint16_t value) {
      bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
      bytes.push_back(static_cast<std::uint8_t>(value));
    }

    void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
      }
    }

    void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
      for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
      }
    }

    std::uint16_t read_u16(std::span<const std::uint8_t> bytes) {
      return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8U) |
        static_cast<std::uint16_t>(bytes[1])
      );
    }

    std::uint32_t read_u32(std::span<const std::uint8_t> bytes) {
      std::uint32_t value = 0;
      for (const auto byte : bytes.first<4>()) {
        value = (value << 8U) | byte;
      }
      return value;
    }

    std::uint64_t read_u64(std::span<const std::uint8_t> bytes) {
      std::uint64_t value = 0;
      for (const auto byte : bytes.first<8>()) {
        value = (value << 8U) | byte;
      }
      return value;
    }

    void append_string(std::vector<std::uint8_t> &bytes, std::string_view value) {
      append_u16(bytes, static_cast<std::uint16_t>(value.size()));
      bytes.insert(bytes.end(), value.begin(), value.end());
    }

    std::vector<std::uint8_t> authentication_transcript(
      proof_role_e role,
      channel_e channel,
      const endpoint_identity_t &identity,
      const challenge_t &challenge
    ) {
      std::vector<std::uint8_t> transcript;
      transcript.reserve(
        authentication_domain.size() + 2 + 4 + 8 + 6 +
        identity.controller_epoch.size() + identity.logical_gpu_id.size() +
        identity.worker_name.size() + challenge.size()
      );
      transcript.insert(
        transcript.end(),
        authentication_domain.begin(),
        authentication_domain.end()
      );
      transcript.push_back(0);
      transcript.push_back(static_cast<std::uint8_t>(role));
      transcript.push_back(static_cast<std::uint8_t>(channel));
      append_u32(transcript, identity.slot);
      append_u64(transcript, identity.generation);
      append_string(transcript, identity.controller_epoch);
      append_string(transcript, identity.logical_gpu_id);
      append_string(transcript, identity.worker_name);
      transcript.insert(transcript.end(), challenge.begin(), challenge.end());
      return transcript;
    }
  }  // namespace

  bool valid_identity(const endpoint_identity_t &identity) {
    return opaque_name_token(identity.controller_epoch, 64) &&
           opaque_name_token(identity.logical_gpu_id) &&
           identity.generation != 0 &&
           opaque_name_token(identity.worker_name);
  }

  bool sequence_guard_t::accept(std::uint64_t sequence) {
    if (exhausted_ || sequence != next_) {
      return false;
    }
    if (next_ == std::numeric_limits<std::uint64_t>::max()) {
      exhausted_ = true;
    } else {
      ++next_;
    }
    return true;
  }

  std::uint64_t sequence_guard_t::next() const {
    return exhausted_ ? 0 : next_;
  }

  std::optional<capability_t> parse_capability_hex(std::string_view encoded) {
    if (encoded.size() != capability_size * 2) {
      return std::nullopt;
    }
    capability_t capability {};
    const auto nibble = [](char value) -> std::optional<std::uint8_t> {
      if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
      }
      if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
      }
      return std::nullopt;
    };
    for (std::size_t index = 0; index < capability.size(); ++index) {
      const auto high = nibble(encoded[index * 2]);
      const auto low = nibble(encoded[index * 2 + 1]);
      if (!high || !low) {
        return std::nullopt;
      }
      capability[index] = static_cast<std::uint8_t>((*high << 4U) | *low);
    }
    return capability;
  }

  std::string capability_hex(const capability_t &capability) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(capability.size() * 2);
    for (const auto byte : capability) {
      encoded.push_back(digits[byte >> 4U]);
      encoded.push_back(digits[byte & 0x0FU]);
    }
    return encoded;
  }

  std::optional<proof_t> authentication_proof(
    const capability_t &capability,
    proof_role_e role,
    channel_e channel,
    const endpoint_identity_t &identity,
    const challenge_t &challenge
  ) {
    if (!valid_identity(identity) ||
        !valid_channel(channel) ||
        (role != proof_role_e::controller && role != proof_role_e::worker)) {
      return std::nullopt;
    }
    const auto transcript = authentication_transcript(role, channel, identity, challenge);
    proof_t proof {};
    unsigned int length = 0;
    const auto *result = HMAC(
      EVP_sha256(),
      capability.data(),
      static_cast<int>(capability.size()),
      transcript.data(),
      transcript.size(),
      proof.data(),
      &length
    );
    if (!result || length != proof.size()) {
      return std::nullopt;
    }
    return proof;
  }

  bool verify_authentication_proof(
    const capability_t &capability,
    proof_role_e role,
    channel_e channel,
    const endpoint_identity_t &identity,
    const challenge_t &challenge,
    std::span<const std::uint8_t> presented
  ) {
    const auto expected = authentication_proof(
      capability,
      role,
      channel,
      identity,
      challenge
    );
    return expected &&
           presented.size() == expected->size() &&
           CRYPTO_memcmp(expected->data(), presented.data(), expected->size()) == 0;
  }

  std::vector<std::uint8_t> encode_frame(const frame_t &frame) {
    if (!valid_channel(frame.channel) ||
        frame.generation == 0 ||
        frame.sequence == 0 ||
        frame.payload.size() > payload_limit(frame.channel) ||
        frame.payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        !valid_message(frame.channel, frame.message, frame.payload.size())) {
      throw std::invalid_argument {"invalid multiseat worker IPC frame"};
    }

    std::vector<std::uint8_t> encoded;
    encoded.reserve(header_size + frame.payload.size());
    encoded.insert(encoded.end(), magic.begin(), magic.end());
    encoded.push_back(static_cast<std::uint8_t>(frame.channel));
    encoded.push_back(static_cast<std::uint8_t>(frame.message));
    append_u16(encoded, 0);
    append_u32(encoded, static_cast<std::uint32_t>(frame.payload.size()));
    append_u32(encoded, frame.slot);
    append_u64(encoded, frame.generation);
    append_u64(encoded, frame.sequence);
    encoded.insert(encoded.end(), frame.payload.begin(), frame.payload.end());
    return encoded;
  }

  parse_result_t parse_frame(
    std::span<const std::uint8_t> bytes,
    channel_e expected_channel,
    std::uint32_t expected_slot,
    std::uint64_t expected_generation
  ) {
    if (!valid_channel(expected_channel) || expected_generation == 0) {
      return {};
    }
    if (bytes.size() < header_size) {
      return {
        .status = parse_status_e::incomplete,
        .required = header_size,
      };
    }
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
      return {};
    }

    const auto channel = static_cast<channel_e>(bytes[4]);
    const auto message = static_cast<message_e>(bytes[5]);
    const auto flags = read_u16(bytes.subspan(6, 2));
    const auto payload_size = read_u32(bytes.subspan(8, 4));
    const auto slot = read_u32(bytes.subspan(12, 4));
    const auto generation = read_u64(bytes.subspan(16, 8));
    const auto sequence = read_u64(bytes.subspan(24, 8));
    if (channel != expected_channel ||
        slot != expected_slot ||
        generation != expected_generation ||
        sequence == 0 ||
        flags != 0 ||
        payload_size > payload_limit(channel) ||
        !valid_message(channel, message, payload_size)) {
      return {};
    }

    const auto total_size = header_size + static_cast<std::size_t>(payload_size);
    if (bytes.size() < total_size) {
      return {
        .status = parse_status_e::incomplete,
        .required = total_size,
      };
    }

    frame_t frame {
      .channel = channel,
      .message = message,
      .slot = slot,
      .generation = generation,
      .sequence = sequence,
      .payload = {},
    };
    frame.payload.assign(
      bytes.begin() + static_cast<std::ptrdiff_t>(header_size),
      bytes.begin() + static_cast<std::ptrdiff_t>(total_size)
    );
    return {
      .status = parse_status_e::complete,
      .frame = std::move(frame),
      .consumed = total_size,
      .required = total_size,
    };
  }

  namespace {
    constexpr std::uint8_t media_frame_flag_idr = 0x01;
    constexpr std::uint8_t media_frame_known_flags = media_frame_flag_idr;

    bool legal_opus_frame_duration(std::uint16_t microseconds) {
      switch (microseconds) {
        case 2500:
        case 5000:
        case 10000:
        case 20000:
        case 40000:
        case 60000:
          return true;
      }
      return false;
    }

    bool legal_h264_profile(std::uint8_t profile_idc) {
      switch (profile_idc) {
        case 66:  // Baseline
        case 77:  // Main
        case 88:  // Extended
        case 100:  // High
          return true;
      }
      return false;
    }
  }  // namespace

  bool valid_media_config(const media_config_t &config) {
    constexpr std::uint16_t minimum_dimension = 16;
    constexpr std::uint16_t maximum_dimension = 16384;
    constexpr std::uint32_t maximum_bitrate_kbps = 1000000;
    constexpr std::uint64_t maximum_fps = 1000;
    if (config.video_codec != video_codec_e::h264 ||
        !legal_h264_profile(config.profile_idc) ||
        config.level_idc < 10 || config.level_idc > 62) {
      return false;
    }
    if (config.width < minimum_dimension || config.width > maximum_dimension ||
        config.height < minimum_dimension || config.height > maximum_dimension ||
        config.width % 2 != 0 || config.height % 2 != 0) {
      return false;
    }
    if (config.fps_numerator == 0 || config.fps_denominator == 0 ||
        config.fps_numerator < config.fps_denominator ||
        static_cast<std::uint64_t>(config.fps_numerator) >
          maximum_fps * static_cast<std::uint64_t>(config.fps_denominator)) {
      return false;
    }
    if (config.bitrate_ceiling_kbps == 0 ||
        config.bitrate_ceiling_kbps > maximum_bitrate_kbps) {
      return false;
    }
    return config.audio_codec == audio_codec_e::opus &&
           config.audio_channels >= 1 && config.audio_channels <= 8 &&
           config.audio_sample_rate == 48000 &&
           legal_opus_frame_duration(config.audio_frame_duration_us);
  }

  std::vector<std::uint8_t> encode_media_config(const media_config_t &config) {
    if (!valid_media_config(config)) {
      throw std::invalid_argument {"invalid multiseat worker media configuration"};
    }
    std::vector<std::uint8_t> body;
    body.reserve(media_config_size);
    body.push_back(media_contract_version);
    body.push_back(static_cast<std::uint8_t>(config.video_codec));
    body.push_back(config.profile_idc);
    body.push_back(config.level_idc);
    append_u16(body, config.width);
    append_u16(body, config.height);
    append_u32(body, config.fps_numerator);
    append_u32(body, config.fps_denominator);
    append_u32(body, config.bitrate_ceiling_kbps);
    body.push_back(static_cast<std::uint8_t>(config.audio_codec));
    body.push_back(config.audio_channels);
    append_u16(body, config.audio_frame_duration_us);
    append_u32(body, config.audio_sample_rate);
    append_u32(body, 0);
    return body;
  }

  std::optional<media_config_t> parse_media_config(std::span<const std::uint8_t> body) {
    if (body.size() != media_config_size || body[0] != media_contract_version ||
        read_u32(body.subspan(28, 4)) != 0) {
      return std::nullopt;
    }
    media_config_t config {
      .video_codec = static_cast<video_codec_e>(body[1]),
      .profile_idc = body[2],
      .level_idc = body[3],
      .width = read_u16(body.subspan(4, 2)),
      .height = read_u16(body.subspan(6, 2)),
      .fps_numerator = read_u32(body.subspan(8, 4)),
      .fps_denominator = read_u32(body.subspan(12, 4)),
      .bitrate_ceiling_kbps = read_u32(body.subspan(16, 4)),
      .audio_codec = static_cast<audio_codec_e>(body[20]),
      .audio_channels = body[21],
      .audio_frame_duration_us = read_u16(body.subspan(22, 2)),
      .audio_sample_rate = read_u32(body.subspan(24, 4)),
    };
    if (!valid_media_config(config)) {
      return std::nullopt;
    }
    return config;
  }

  std::vector<std::uint8_t> encode_media_frame(
    const media_frame_t &frame,
    std::span<const std::uint8_t> encoded
  ) {
    if (encoded.empty() || encoded.size() > max_media_payload - media_frame_prefix_size) {
      throw std::invalid_argument {"invalid multiseat worker media frame"};
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(media_frame_prefix_size + encoded.size());
    payload.push_back(media_contract_version);
    payload.push_back(frame.idr ? media_frame_flag_idr : 0);
    append_u16(payload, 0);
    append_u32(payload, 0);
    append_u64(payload, frame.frame_index);
    append_u64(payload, frame.capture_timestamp_ns);
    append_u64(payload, frame.encode_timestamp_ns);
    payload.insert(payload.end(), encoded.begin(), encoded.end());
    return payload;
  }

  std::optional<media_frame_t> parse_media_frame(
    std::span<const std::uint8_t> payload,
    std::span<const std::uint8_t> &encoded
  ) {
    encoded = {};
    if (payload.size() <= media_frame_prefix_size ||
        payload.size() > max_media_payload ||
        payload[0] != media_contract_version ||
        (payload[1] & ~media_frame_known_flags) != 0 ||
        read_u16(payload.subspan(2, 2)) != 0 ||
        read_u32(payload.subspan(4, 4)) != 0) {
      return std::nullopt;
    }
    encoded = payload.subspan(media_frame_prefix_size);
    return media_frame_t {
      .frame_index = read_u64(payload.subspan(8, 8)),
      .idr = (payload[1] & media_frame_flag_idr) != 0,
      .capture_timestamp_ns = read_u64(payload.subspan(16, 8)),
      .encode_timestamp_ns = read_u64(payload.subspan(24, 8)),
    };
  }

  std::vector<std::uint8_t> encode_frame_range(const frame_range_t &range) {
    if (range.first > range.last) {
      throw std::invalid_argument {"invalid multiseat worker frame range"};
    }
    std::vector<std::uint8_t> body;
    body.reserve(frame_range_size);
    append_u64(body, range.first);
    append_u64(body, range.last);
    return body;
  }

  std::optional<frame_range_t> parse_frame_range(std::span<const std::uint8_t> body) {
    if (body.size() != frame_range_size) {
      return std::nullopt;
    }
    const frame_range_t range {
      .first = read_u64(body.subspan(0, 8)),
      .last = read_u64(body.subspan(8, 8)),
    };
    if (range.first > range.last) {
      return std::nullopt;
    }
    return range;
  }

}  // namespace multiseat::worker_ipc
