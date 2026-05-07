/**
 * @file src/browser_stream_protocol.h
 * @brief Browser Stream media envelope helpers.
 */
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace browser_stream::protocol {
  constexpr std::uint8_t version = 1;
  constexpr std::size_t header_size = 36;
  constexpr std::size_t max_datagram_payload = 1180;

  enum class media_kind : std::uint8_t {
    video = 1,
    audio = 2,
  };

  enum class codec_id : std::uint8_t {
    h264 = 1,
    opus = 2,
  };

  enum flags : std::uint8_t {
    keyframe = 1 << 0,
  };

  struct media_envelope_t {
    media_kind kind = media_kind::video;
    codec_id codec = codec_id::h264;
    std::uint8_t flags = 0;
    std::uint32_t sequence = 0;
    std::uint64_t frame_id = 0;
    std::uint64_t timestamp_us = 0;
    std::uint16_t chunk_index = 0;
    std::uint16_t chunk_count = 1;
    std::vector<std::uint8_t> payload;
  };

  std::vector<std::uint8_t> encode_datagram(const media_envelope_t &envelope);
  bool decode_datagram(std::span<const std::uint8_t> datagram, media_envelope_t &envelope, std::string &error);
}  // namespace browser_stream::protocol
