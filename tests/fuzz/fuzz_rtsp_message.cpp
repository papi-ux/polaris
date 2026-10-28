/**
 * @file tests/fuzz/fuzz_rtsp_message.cpp
 * @brief Exercise the adapted production RTSP parser and request Content-length decoding.
 */
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

#include "rtsp_content_length.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
  // Keep parsed fields observable in optimized builds; do not copy the parser's rules.
  volatile std::uint64_t result_sink;

  // The product reads each parsed string as a std::string_view, so measure it the same way.
  std::uint64_t mix(std::uint64_t result, const char *text) {
    return (result * 131u) ^ std::strlen(text);
  }
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  // Harness resource bound equal to the product's 2048-byte RTSP receive buffer;
  // it adds no RTSP admission limit. The parser copies its input before tokenizing.
  if (size > 2048) {
    return 0;
  }
  static char empty[1];
  char *bytes = size ? reinterpret_cast<char *>(const_cast<std::uint8_t *>(data)) : empty;

  RTSP_MESSAGE message {};
  const int status = parseRtspMessage(&message, bytes, static_cast<int>(size));
  if (status != RTSP_ERROR_SUCCESS) {
    // Rejection is an expected outcome; failure cleanup is the parser's own.
    result_sink = static_cast<std::uint32_t>(status);
    return 0;
  }

  // Read what print_msg and the request path read, by the parsed message type.
  std::uint64_t result = static_cast<std::uint64_t>(message.type) ^
                         static_cast<std::uint32_t>(message.sequenceNumber);
  result = mix(result, message.protocol);
  result = mix(result, message.messageBuffer);
  if (message.type == TYPE_RESPONSE) {
    result ^= static_cast<std::uint32_t>(message.message.response.statusCode);
    result = mix(result, message.message.response.statusString);
  } else {
    result = mix(result, message.message.request.command);
    result = mix(result, message.message.request.target);
  }
  for (auto option = message.options; option != nullptr; option = option->next) {
    result = mix(mix(result, option->option), option->content);
  }
  for (int i = 0; i < message.payloadLength; ++i) {
    result = (result * 131u) ^ static_cast<unsigned char>(message.payload[i]);
  }

  // Plaintext request framing: the production decoder over the parsed headers.
  const auto declared = rtsp_stream::find_content_length(message.options);
  result ^= static_cast<std::uint32_t>(declared.length);
  if (declared.content != nullptr) {
    result = mix(result, declared.content);
  }
  result_sink = result;

  freeMessage(&message);
  return 0;
}
