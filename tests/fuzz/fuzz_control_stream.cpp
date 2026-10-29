/**
 * @file tests/fuzz/fuzz_control_stream.cpp
 * @brief Exercise the real typed control-stream payload decoders without a session.
 *
 * The control thread strips the 2-byte type field, then routes the remaining payload to a
 * handler that reinterprets its bytes. This driver selects the message type from the first
 * input byte, exactly as the dispatcher routes by type, and hands the rest to the production
 * decoder for that type in src/control_messages.cpp. There is no networking, ENet peer,
 * session, decryption, input injection or thread: only the first byte-level decode each
 * handler performs before any side effect. Expected refusals and bounded empty views are
 * not failures; no decoder rule is copied here.
 */
#include "control_messages.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {
  // Keep every decoded field observable in optimized builds; do not copy the decoders' rules.
  volatile std::uint64_t result_sink;

  std::uint64_t consume_view(std::uint64_t result, std::string_view view) {
    result = (result * 131u) ^ view.size();
    for (const char byte : view) {
      result = (result * 131u) ^ static_cast<unsigned char>(byte);
    }
    return result;
  }
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  // Harness resource bound; it is not a new control-channel limit. The encrypted wrapper's
  // length is a uint16, so this still lets a declared cipher length both fit and overrun.
  if (size > 65536) {
    return 0;
  }

  // The first byte selects the message type the dispatcher would route to; the rest is the
  // payload the handler receives after the 2-byte type field is stripped. An empty input has
  // no selector, so there is nothing to route.
  if (size == 0) {
    return 0;
  }
  const std::uint8_t selector = data[0];
  const std::string_view payload {
    size > 1 ? reinterpret_cast<const char *>(data + 1) : "",
    size - 1
  };

  // The dispatcher's own guard: a full ENet packet must be long enough to carry the type.
  std::uint64_t result = stream::control_packet_carries_type(payload.size() + stream::control_type_field_size)
                           ? 1u : 0u;

  switch (selector % 5u) {
    case 0: {
      // IDX_LOSS_STATS: the fixed int32 record, or absent when the payload is shorter.
      const auto stats = stream::decode_loss_stats(payload);
      result ^= stats.present ? 2u : 0u;
      if (stats.present) {
        result ^= static_cast<std::uint32_t>(stats.count);
        result ^= static_cast<std::uint32_t>(stats.time_ms);
        result ^= static_cast<std::uint32_t>(stats.last_good_frame);
      }
      break;
    }
    case 1: {
      // IDX_INVALIDATE_REF_FRAMES: the two int64 fields, or absent when the payload is shorter.
      const auto frames = stream::decode_invalidate_ref_frames(payload);
      result ^= frames.present ? 2u : 0u;
      if (frames.present) {
        result ^= static_cast<std::uint64_t>(frames.first_frame);
        result ^= static_cast<std::uint64_t>(frames.last_frame);
      }
      break;
    }
    case 2: {
      // IDX_INPUT_DATA: big-endian declared cipher length and the bounded cipher view.
      const auto in = stream::decode_input_cipher(payload);
      result ^= in.has_length ? 2u : 0u;
      result ^= static_cast<std::uint32_t>(in.declared_length);
      if (in.fits) {
        result = consume_view(result, in.cipher);
      }
      break;
    }
    case 3: {
      // IDX_ENCRYPTED: wrapper header length/seq and the bounded tagged-cipher view.
      const auto wrapper = stream::decode_encrypted_control(payload);
      result ^= wrapper.header_present ? 4u : 0u;
      if (wrapper.header_present) {
        result ^= wrapper.length;
        result ^= wrapper.seq;
        result ^= wrapper.runt ? 8u : 0u;
        if (wrapper.fits) {
          result = consume_view(result, wrapper.cipher);
        }
      }
      break;
    }
    case 4: {
      // IDX_EXEC_SERVER_CMD: the one-byte command index, or absent for an empty payload.
      const auto command = stream::decode_server_cmd_index(payload);
      result ^= command.present ? 2u : 0u;
      if (command.present) {
        result ^= command.index;
      }
      break;
    }
  }

  result_sink = result;
  return 0;
}
