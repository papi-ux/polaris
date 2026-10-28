/**
 * @file src/rtsp_content_length.cpp
 * @brief Preserve the plaintext request's existing Content-length lookup and lenient decoding.
 */
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

#include "rtsp_content_length.h"
#include "utility.h"

#include <algorithm>
#include <cctype>
#include <string_view>

using namespace std::literals;

namespace rtsp_stream {
  content_length_t find_content_length(const _OPTION_ITEM *options) {
    content_length_t result;
    for (auto option = options; option != nullptr; option = option->next) {
      if ("Content-length"sv == option->option) {
        result.content = option->content;

        std::string_view content {option->content};
        auto begin = std::find_if(std::begin(content), std::end(content), [](auto ch) {
          return (bool) std::isdigit(ch);
        });

        result.length = util::from_chars(begin, std::end(content));
        break;
      }
    }
    return result;
  }
}  // namespace rtsp_stream
