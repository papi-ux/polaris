/**
 * @file tests/fuzz/fuzz_control_fec.cpp
 * @brief Exercise the real selective-FEC payload parser without a session.
 */
#include "stream_fec_status.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace {
  // Keep decoding observable even in optimized builds; do not copy its rules.
  volatile std::uint32_t result_sink;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  // Harness budget only: production still enforces its own exact payload size.
  if (size > 64) {
    return 0;
  }
  const std::string_view payload {size ? reinterpret_cast<const char *>(data) : "", size};
  // Capability absent/present, including unrelated bits. The seed is the raw
  // decrypted payload, without a fuzzer-only prefix or the LE control header.
  for (const std::uint32_t features : {0u, 1u, 3u, 0xffffffffu}) {
    const auto parsed = stream::fec_status::parse(payload, features);
    std::uint32_t result = static_cast<std::uint32_t>(parsed.refusal);
    if (parsed.block) {
      const auto &b = *parsed.block;
      result ^= b.frame_index ^ b.highest_received_sequence_number ^
                b.next_contiguous_sequence_number ^ b.missing_packets_before_highest_received ^
                b.total_data_packets ^ b.total_parity_packets ^ b.received_data_packets ^
                b.received_parity_packets ^ b.fec_percentage ^ b.block_index ^ b.block_count ^
                b.absent_data_shards() ^ b.rank_deficit_shards();
    }
    result_sink = result;
  }
  return 0;
}
