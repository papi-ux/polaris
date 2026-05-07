/**
 * @file src/browser_stream_protocol.cpp
 * @brief Browser Stream media envelope helpers.
 */

#include "browser_stream_protocol.h"

#include <algorithm>

namespace browser_stream::protocol {
  namespace {
    constexpr std::uint32_t magic = 0x31534250;  // "PBS1" little-endian.

    void write_u16(std::vector<std::uint8_t> &out, std::size_t offset, std::uint16_t value) {
      out[offset] = static_cast<std::uint8_t>(value & 0xff);
      out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    }

    void write_u32(std::vector<std::uint8_t> &out, std::size_t offset, std::uint32_t value) {
      for (int i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
      }
    }

    void write_u64(std::vector<std::uint8_t> &out, std::size_t offset, std::uint64_t value) {
      for (int i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
      }
    }

    std::uint16_t read_u16(std::span<const std::uint8_t> data, std::size_t offset) {
      return static_cast<std::uint16_t>(data[offset]) |
             static_cast<std::uint16_t>(data[offset + 1] << 8);
    }

    std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t offset) {
      std::uint32_t value = 0;
      for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data[offset + i]) << (8 * i);
      }
      return value;
    }

    std::uint64_t read_u64(std::span<const std::uint8_t> data, std::size_t offset) {
      std::uint64_t value = 0;
      for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[offset + i]) << (8 * i);
      }
      return value;
    }
  }  // namespace

  std::vector<std::uint8_t> encode_datagram(const media_envelope_t &envelope) {
    std::vector<std::uint8_t> out(header_size + envelope.payload.size());
    write_u32(out, 0, magic);
    out[4] = version;
    out[5] = static_cast<std::uint8_t>(envelope.kind);
    out[6] = static_cast<std::uint8_t>(envelope.codec);
    out[7] = envelope.flags;
    write_u32(out, 8, envelope.sequence);
    write_u64(out, 12, envelope.frame_id);
    write_u64(out, 20, envelope.timestamp_us);
    write_u16(out, 28, envelope.chunk_index);
    write_u16(out, 30, envelope.chunk_count);
    write_u32(out, 32, static_cast<std::uint32_t>(envelope.payload.size()));
    std::copy(envelope.payload.begin(), envelope.payload.end(), out.begin() + header_size);
    return out;
  }

  bool decode_datagram(std::span<const std::uint8_t> datagram, media_envelope_t &envelope, std::string &error) {
    if (datagram.size() < header_size) {
      error = "Browser Stream datagram is shorter than the media header";
      return false;
    }

    if (read_u32(datagram, 0) != magic) {
      error = "Browser Stream datagram has an invalid magic value";
      return false;
    }

    if (datagram[4] != version) {
      error = "Browser Stream datagram has an unsupported version";
      return false;
    }

    const auto payload_size = read_u32(datagram, 32);
    if (payload_size != datagram.size() - header_size) {
      error = "Browser Stream datagram payload length does not match the header";
      return false;
    }

    const auto chunk_index = read_u16(datagram, 28);
    const auto chunk_count = read_u16(datagram, 30);
    if (chunk_count == 0 || chunk_index >= chunk_count) {
      error = "Browser Stream datagram has invalid chunk indexes";
      return false;
    }

    envelope.kind = static_cast<media_kind>(datagram[5]);
    envelope.codec = static_cast<codec_id>(datagram[6]);
    envelope.flags = datagram[7];
    envelope.sequence = read_u32(datagram, 8);
    envelope.frame_id = read_u64(datagram, 12);
    envelope.timestamp_us = read_u64(datagram, 20);
    envelope.chunk_index = chunk_index;
    envelope.chunk_count = chunk_count;
    envelope.payload.assign(datagram.begin() + header_size, datagram.end());
    error.clear();
    return true;
  }
}  // namespace browser_stream::protocol
