/**
 * @file src/log_tail_api.cpp
 * @brief Versioned bounded log-tail request and response contract.
 */

#include "log_tail_api.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

namespace log_tail_api {
  namespace {
    std::uintmax_t parse_decimal(
      const std::string_view value,
      const std::string_view name,
      const std::uintmax_t minimum,
      const std::uintmax_t maximum
    ) {
      std::uintmax_t parsed = 0;
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (value.empty() || error != std::errc {} || end != value.data() + value.size() || parsed < minimum || parsed > maximum) {
        throw std::invalid_argument(std::string {name} + " must be a decimal integer between " + std::to_string(minimum) + " and " + std::to_string(maximum));
      }
      return parsed;
    }

    std::size_t trim_to_line_limit(std::string &content, const std::size_t max_lines) {
      if (content.empty()) {
        return 0;
      }

      std::size_t line_count = static_cast<std::size_t>(std::count(content.begin(), content.end(), '\n'));
      if (content.back() != '\n') {
        ++line_count;
      }
      if (line_count <= max_lines) {
        return 0;
      }

      auto lines_to_skip = line_count - max_lines;
      std::size_t bytes_to_skip = 0;
      while (lines_to_skip > 0) {
        const auto delimiter = content.find('\n', bytes_to_skip);
        if (delimiter == std::string::npos) {
          break;
        }
        bytes_to_skip = delimiter + 1;
        --lines_to_skip;
      }
      content.erase(0, bytes_to_skip);
      return bytes_to_skip;
    }

    std::string base64_encode(const std::string_view content) {
      static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

      std::string encoded;
      encoded.reserve(((content.size() + 2) / 3) * 4);

      std::size_t index = 0;
      while (index + 3 <= content.size()) {
        const auto value =
          (static_cast<std::uint32_t>(static_cast<unsigned char>(content[index])) << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(content[index + 1])) << 8) |
          static_cast<std::uint32_t>(static_cast<unsigned char>(content[index + 2]));
        encoded.push_back(alphabet[(value >> 18) & 0x3f]);
        encoded.push_back(alphabet[(value >> 12) & 0x3f]);
        encoded.push_back(alphabet[(value >> 6) & 0x3f]);
        encoded.push_back(alphabet[value & 0x3f]);
        index += 3;
      }

      const auto remaining = content.size() - index;
      if (remaining == 1) {
        const auto value = static_cast<std::uint32_t>(static_cast<unsigned char>(content[index])) << 16;
        encoded.push_back(alphabet[(value >> 18) & 0x3f]);
        encoded.push_back(alphabet[(value >> 12) & 0x3f]);
        encoded.append("==");
      } else if (remaining == 2) {
        const auto value =
          (static_cast<std::uint32_t>(static_cast<unsigned char>(content[index])) << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(content[index + 1])) << 8);
        encoded.push_back(alphabet[(value >> 18) & 0x3f]);
        encoded.push_back(alphabet[(value >> 12) & 0x3f]);
        encoded.push_back(alphabet[(value >> 6) & 0x3f]);
        encoded.push_back('=');
      }

      return encoded;
    }
  }  // namespace

  request_t parse_request(
    const std::optional<std::string_view> max_bytes,
    const std::optional<std::string_view> max_lines,
    const std::optional<std::string_view> after,
    const std::optional<std::string_view> after_generation
  ) {
    request_t request;
    if (max_bytes) {
      request.max_bytes = static_cast<std::size_t>(parse_decimal(*max_bytes, "max_bytes", 1, maximum_max_bytes));
    }
    if (max_lines) {
      request.max_lines = static_cast<std::size_t>(parse_decimal(*max_lines, "max_lines", 1, maximum_max_lines));
    }
    if (after) {
      request.after = parse_decimal(*after, "after", 0, std::numeric_limits<std::uintmax_t>::max());
    }
    if (after_generation) {
      request.after_generation = parse_decimal(*after_generation, "after_generation", 0, std::numeric_limits<std::uint64_t>::max());
    }
    if (request.after.has_value() != request.after_generation.has_value()) {
      throw std::invalid_argument("after and after_generation must appear together");
    }
    return request;
  }

  response_t build_response(
    file_handler::tail_result_t tail,
    const request_t &request,
    const std::uint64_t generation
  ) {
    if (tail.end_offset < tail.start_offset || tail.end_offset - tail.start_offset != tail.content.size()) {
      throw std::invalid_argument("tail offsets do not match content size");
    }

    response_t response {
      .content = std::move(tail.content),
      .start_offset = tail.start_offset,
      .end_offset = tail.end_offset,
      .generation = generation,
      .truncated = tail.truncated,
      .reset = true,
    };

    if (request.after && request.after_generation == generation &&
        *request.after >= response.start_offset && *request.after <= response.end_offset) {
      const auto bytes_to_skip = static_cast<std::size_t>(*request.after - response.start_offset);
      response.content.erase(0, bytes_to_skip);
      response.start_offset = *request.after;
      response.reset = false;
    }

    const auto line_prefix_bytes = trim_to_line_limit(response.content, request.max_lines);
    if (line_prefix_bytes > 0) {
      response.start_offset += line_prefix_bytes;
      response.truncated = true;
      response.reset = true;
    }

    return response;
  }

  nlohmann::json serialize_response(const response_t &response) {
    return {
      {"status", true},
      {"schema_version", 1},
      {"content_encoding", "base64"},
      {"content", base64_encode(response.content)},
      {"content_bytes", response.content.size()},
      {"start_offset", response.start_offset},
      {"end_offset", response.end_offset},
      {"generation", response.generation},
      {"truncated", response.truncated},
      {"reset", response.reset},
    };
  }
}  // namespace log_tail_api
