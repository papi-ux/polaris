/**
 * @file src/control_messages.h
 * @brief Decode the typed control-stream payloads a client sends, without a session or socket.
 *
 * The control thread dispatches each inbound control packet to a handler lambda that
 * reinterprets the raw payload bytes before any side effect. These free functions carry
 * the length checks and the first byte-level decode of each such message, so the handlers
 * and an opt-in fuzz target share one owner for that decode instead of a copy. Every
 * function is total over the bytes that arrived off the wire: a fixed-size record is read
 * only when the payload is long enough to hold it, and a payload that is long enough decodes
 * to exactly the values the previous inline reads produced.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace stream {
  // The dispatcher consumes this leading type field before a handler sees the payload.
  inline constexpr std::size_t control_type_field_size = sizeof(std::uint16_t);

  // sizeof(stream::control_encrypted_t): LE 0x0001 header (2) + length (2) + seq (4), packed.
  // stream.cpp statically asserts this mirrors the packed struct it still defines.
  inline constexpr std::size_t encrypted_control_header_size = 8;

  // The fixed records the loss-stats, invalidate-ref-frames and server-command handlers read.
  // Loss stats reads int32 offsets 0, 1 and 3, so it needs all four slots even though it
  // skips slot 2.
  inline constexpr std::size_t loss_stats_record_size = 4 * sizeof(std::int32_t);
  inline constexpr std::size_t invalidate_ref_frames_record_size = 2 * sizeof(std::int64_t);
  inline constexpr std::size_t server_cmd_record_size = sizeof(std::uint8_t);

  // --- Length checks for peer-supplied control packets. Free functions with external
  // linkage so tests can drive them directly; every one is total, so a caller can hand
  // them any value that arrived off the wire. ---

  /**
   * @brief Whether an ENet packet is long enough to hold the type field.
   *
   * Below this, `dataLength - sizeof(type)` wraps and yields a payload view
   * claiming the whole address space.
   */
  bool control_packet_carries_type(std::size_t data_length);

  /**
   * @brief Whether the encrypted control header itself is present.
   *
   * The dispatcher consumes the leading type field before the handler sees the
   * payload, so the header starts two bytes behind it.
   */
  bool encrypted_control_header_present(std::size_t payload_size);

  /**
   * @brief Whether a declared cipher length fits in the bytes that arrived.
   */
  bool encrypted_control_cipher_fits(std::size_t payload_size, std::uint16_t declared_length);

  /**
   * @brief Whether an IDX_INPUT_DATA packet's declared cipher length is usable.
   *
   * The length arrives as a signed 32-bit value, so a negative one would widen
   * to SIZE_MAX when used as a view length.
   */
  bool input_control_cipher_fits(std::size_t payload_size, std::int32_t declared_cipher_length);

  // --- First byte-level decode of each typed client->host control message, extracted
  // verbatim from the control-thread handler lambdas. ---

  /**
   * @brief IDX_LOSS_STATS: the lost-packet count, elapsed milliseconds and last good frame.
   *
   * The handler reads a fixed little-endian int32 record at offsets 0, 1 and 3.
   * `present` is false when the payload is shorter than loss_stats_record_size; the handler
   * warns and drops it, and nothing past the payload is read.
   */
  struct loss_stats_t {
    bool present = false;
    std::int32_t count = 0;
    std::int32_t time_ms = 0;
    std::int32_t last_good_frame = 0;
  };

  loss_stats_t decode_loss_stats(std::string_view payload);

  /**
   * @brief IDX_INVALIDATE_REF_FRAMES: the first and last frame of the invalidated range.
   *
   * The handler reads two int64 fields. `present` is false when the payload is shorter
   * than invalidate_ref_frames_record_size; the handler warns and drops it.
   */
  struct invalidate_ref_frames_t {
    bool present = false;
    std::int64_t first_frame = 0;
    std::int64_t last_frame = 0;
  };

  invalidate_ref_frames_t decode_invalidate_ref_frames(std::string_view payload);

  /**
   * @brief IDX_INPUT_DATA: the big-endian declared cipher length and the bounded cipher view.
   *
   * `has_length` is false when the payload is too short to carry the 4-byte length prefix;
   * the handler warns and drops it. `fits` applies input_control_cipher_fits to the decoded
   * length; `cipher` is the tagged-cipher view only when it fits.
   */
  struct input_cipher_t {
    bool has_length = false;
    std::int32_t declared_length = 0;
    bool fits = false;
    std::string_view cipher {};
  };

  input_cipher_t decode_input_cipher(std::string_view payload);

  /**
   * @brief IDX_ENCRYPTED: the wrapper header length/seq and the bounded tagged-cipher view.
   *
   * The dispatcher already consumed the 2-byte type field (the header's LE 0x0001), so the
   * remaining header fields and body live at the start of `payload`: length at [0,2), seq at
   * [2,6), body at [6,...). `header_present` guards reading the length and seq; `runt` is the
   * handler's `length < 16 + 4 + 4` floor; `fits` applies encrypted_control_cipher_fits. The
   * cipher view (header->payload(), length - 4) is set only when present, not runt, and fits.
   */
  struct encrypted_control_t {
    bool header_present = false;
    std::uint16_t length = 0;
    std::uint32_t seq = 0;
    bool runt = false;
    bool fits = false;
    std::string_view cipher {};
  };

  encrypted_control_t decode_encrypted_control(std::string_view payload);

  /**
   * @brief IDX_EXEC_SERVER_CMD: the one-byte server command index.
   *
   * The handler reads a single byte after its permission gate. `present` is false for an
   * empty payload; the handler warns and drops it.
   */
  struct server_cmd_index_t {
    bool present = false;
    std::uint8_t index = 0;
  };

  server_cmd_index_t decode_server_cmd_index(std::string_view payload);
}  // namespace stream
