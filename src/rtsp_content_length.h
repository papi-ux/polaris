/**
 * @file src/rtsp_content_length.h
 * @brief Decode a parsed plaintext RTSP request's declared payload length without a socket.
 */
#pragma once

// moonlight-common-c's Rtsp.h has no include guard, and rtsp.cpp already includes it.
struct _OPTION_ITEM;

namespace rtsp_stream {
  struct content_length_t {
    const char *content = nullptr;  ///< The first exactly named "Content-length" value, if any.
    int length = 0;  ///< Its bytes from the first digit on, with util::from_chars arithmetic.
  };

  // Keep the call after a successful parse and before the payload completeness check.
  content_length_t find_content_length(const _OPTION_ITEM *options);
}  // namespace rtsp_stream
