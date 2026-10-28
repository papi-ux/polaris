/**
 * @file src/control_messages.cpp
 * @brief Preserve the control-thread handlers' length checks and first byte-level decode.
 *
 * Each decoder reproduces exactly what its handler lambda in stream.cpp read inline. Reads
 * are done with std::memcpy into host-order scalars so the fuzz build's alignment checks do
 * not flag what the packed-struct and reinterpret_cast reads performed without diagnostic;
 * the decoded values and the bytes touched are identical to the handler's. No bounds check
 * is added or removed: a handler that read a fixed record without a length guard is mirrored
 * reading the same bytes, so an over-read remains an over-read.
 */
#include "control_messages.h"
#include "utility.h"

#include <cstring>

namespace stream {
  bool control_packet_carries_type(std::size_t data_length) {
    return data_length >= sizeof(std::uint16_t);
  }

  bool encrypted_control_header_present(std::size_t payload_size) {
    return payload_size + control_type_field_size >= encrypted_control_header_size;
  }

  bool encrypted_control_cipher_fits(std::size_t payload_size, std::uint16_t declared_length) {
    if (!encrypted_control_header_present(payload_size) || declared_length < (16 + 4 + 4)) {
      return false;
    }
    const auto available = payload_size + control_type_field_size - encrypted_control_header_size;
    return static_cast<std::size_t>(declared_length - 4) <= available;
  }

  bool input_control_cipher_fits(std::size_t payload_size, std::int32_t declared_cipher_length) {
    if (payload_size < sizeof(std::int32_t) || declared_cipher_length < 0) {
      return false;
    }
    return static_cast<std::size_t>(declared_cipher_length) <= payload_size - sizeof(std::int32_t);
  }

  loss_stats_t decode_loss_stats(std::string_view payload) {
    // Handler: int32_t *stats = (int32_t *) payload.data(); stats[0], stats[1], stats[3].
    const char *base = payload.data();
    loss_stats_t stats {};
    std::memcpy(&stats.count, base + 0 * sizeof(std::int32_t), sizeof(std::int32_t));
    std::memcpy(&stats.time_ms, base + 1 * sizeof(std::int32_t), sizeof(std::int32_t));
    std::memcpy(&stats.last_good_frame, base + 3 * sizeof(std::int32_t), sizeof(std::int32_t));
    return stats;
  }

  invalidate_ref_frames_t decode_invalidate_ref_frames(std::string_view payload) {
    // Handler: std::int64_t *frames = (std::int64_t *) payload.data(); frames[0], frames[1].
    const char *base = payload.data();
    invalidate_ref_frames_t frames {};
    std::memcpy(&frames.first_frame, base + 0 * sizeof(std::int64_t), sizeof(std::int64_t));
    std::memcpy(&frames.last_frame, base + 1 * sizeof(std::int64_t), sizeof(std::int64_t));
    return frames;
  }

  input_cipher_t decode_input_cipher(std::string_view payload) {
    input_cipher_t result;
    // Handler: if (payload.size() < sizeof(std::int32_t)) { warn; return; }
    if (payload.size() < sizeof(std::int32_t)) {
      return result;
    }
    result.has_length = true;
    // Handler: util::endian::big(*(int32_t *) payload.data()).
    std::int32_t raw;
    std::memcpy(&raw, payload.data(), sizeof(raw));
    result.declared_length = util::endian::big(raw);
    // Handler: if (!input_control_cipher_fits(payload.size(), tagged_cipher_length)) { warn; return; }
    if (!input_control_cipher_fits(payload.size(), result.declared_length)) {
      return result;
    }
    result.fits = true;
    // Handler: tagged_cipher {payload.data() + sizeof(tagged_cipher_length), (size_t) tagged_cipher_length}.
    result.cipher = std::string_view {
      payload.data() + sizeof(result.declared_length),
      static_cast<std::size_t>(result.declared_length)
    };
    return result;
  }

  encrypted_control_t decode_encrypted_control(std::string_view payload) {
    encrypted_control_t result;
    // Handler: if (!encrypted_control_header_present(payload.size())) { warn; return; }
    if (!encrypted_control_header_present(payload.size())) {
      return result;
    }
    result.header_present = true;
    // Handler: header = (control_encrypted_p) (payload.data() - 2); length at header+2, seq at header+4.
    // The consumed type field is header+0, so length is payload[0,2) and seq is payload[2,6).
    std::uint16_t raw_length;
    std::uint32_t raw_seq;
    std::memcpy(&raw_length, payload.data(), sizeof(raw_length));
    std::memcpy(&raw_seq, payload.data() + sizeof(raw_length), sizeof(raw_seq));
    result.length = util::endian::little(raw_length);
    result.seq = util::endian::little(raw_seq);
    // Handler: if (length < (16 + 4 + 4)) { warn "Runt"; return; }
    if (result.length < (16 + 4 + 4)) {
      result.runt = true;
      return result;
    }
    // Handler: if (!encrypted_control_cipher_fits(payload.size(), length)) { warn; return; }
    if (!encrypted_control_cipher_fits(payload.size(), result.length)) {
      return result;
    }
    result.fits = true;
    // Handler: tagged_cipher {(char *) header->payload(), (size_t) (length - 4)}; payload() is header+8.
    const auto tagged_cipher_length = result.length - 4;
    result.cipher = std::string_view {
      payload.data() + (encrypted_control_header_size - control_type_field_size),
      static_cast<std::size_t>(tagged_cipher_length)
    };
    return result;
  }

  std::uint8_t decode_server_cmd_index(std::string_view payload) {
    // Handler: uint8_t cmdIndex = *(uint8_t*)payload.data();
    std::uint8_t index;
    std::memcpy(&index, payload.data(), sizeof(index));
    return index;
  }
}  // namespace stream
