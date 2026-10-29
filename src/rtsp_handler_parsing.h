/**
 * @file src/rtsp_handler_parsing.h
 * @brief Bounds-safe helpers for parsing fields the RTSP command handlers read from received bytes.
 */
#pragma once

#include <string_view>

// moonlight-common-c's Rtsp.h has no include guard, and rtsp.cpp already includes it.
struct _RTSP_MESSAGE;

namespace rtsp_stream {
  /**
   * @brief The SETUP stream type carried in a request target, e.g. "video" from "streamid=video/0/0".
   * @details The type is the text between the first '=' and the next '/'. A target without '=' has no
   *          type and yields an empty view without reading past the target.
   */
  std::string_view setup_stream_type(std::string_view target);

  /**
   * @brief An ANNOUNCE `a=` attribute value with a single trailing space removed.
   * @details An empty value is returned unchanged, without indexing an empty view.
   */
  std::string_view announce_attribute_value(std::string_view value);

  /**
   * @brief The command a parsed message should be dispatched on, or an empty view when the message is
   *        not a request (e.g. a parsed response, whose union holds the response fields instead).
   */
  std::string_view dispatch_command(const _RTSP_MESSAGE &msg);
}  // namespace rtsp_stream
