/**
 * @file src/rtsp_handler_parsing.cpp
 * @brief Bounds-safe helpers for the RTSP command handlers, kept testable without a socket.
 */
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

#include "rtsp_handler_parsing.h"

#include <algorithm>
#include <iterator>

namespace rtsp_stream {
  std::string_view setup_stream_type(std::string_view target) {
    // Step past the '=' only when there is one; a target without '=' leaves begin at the end,
    // so the following search and the resulting view stay inside the target.
    auto eq = std::find(target.begin(), target.end(), '=');
    auto begin = eq == target.end() ? eq : std::next(eq);
    auto end = std::find(begin, target.end(), '/');
    return std::string_view {begin, static_cast<std::size_t>(std::distance(begin, end))};
  }

  std::string_view announce_attribute_value(std::string_view value) {
    if (!value.empty() && value.back() == ' ') {
      value = value.substr(0, value.size() - 1);
    }
    return value;
  }

  std::string_view dispatch_command(const _RTSP_MESSAGE &msg) {
    // Only a request carries a command in the union; a response holds statusString/statusCode there,
    // so reading request.command/target for one would misinterpret the response fields.
    if (msg.type != TYPE_REQUEST || msg.message.request.command == nullptr) {
      return {};
    }
    return std::string_view {msg.message.request.command};
  }
}  // namespace rtsp_stream
