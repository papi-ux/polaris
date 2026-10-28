/**
 * @file tests/fuzz/fuzz_nvhttp_launch_query.cpp
 * @brief Exercise the real query parser and launch-field decoders without a session.
 */
#include "nvhttp_query.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {
  volatile std::uint64_t result_sink;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  // Harness resource bound only; this does not add an HTTP admission limit.
  if (size > 65536) {
    return 0;
  }
  const std::string query {size ? reinterpret_cast<const char *>(data) : "", size};
  const auto args = SimpleWeb::QueryString::parse(query);

  std::string key;
  try {
    key = nvhttp::decode_client_rikey(args);
  } catch (const std::out_of_range &) {
    if (args.find("rikey") == args.end()) {
      return 0;
    }
    throw;
  }

  std::uint64_t result = key.size();
  for (const unsigned char byte : key) {
    result = (result * 131u) ^ byte;
  }
  result_sink = result;
  if (key.size() != nvhttp::client_launch_key_bytes) {
    return 0;
  }

  const auto corever = nvhttp::decode_client_corever(args);
  result ^= static_cast<std::uint64_t>(corever);
  result_sink = result;

  std::uint32_t iv_prefix;
  try {
    iv_prefix = nvhttp::decode_client_iv_prefix(args);
  } catch (const std::out_of_range &) {
    if (args.find("rikeyid") == args.end()) {
      return 0;
    }
    throw;
  }
  result_sink = result ^ iv_prefix;
  return 0;
}
