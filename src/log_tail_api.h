/**
 * @file src/log_tail_api.h
 * @brief Versioned bounded log-tail request and response contract.
 */
#pragma once

#include "file_handler.h"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace log_tail_api {
  inline constexpr std::size_t default_max_bytes = 256U * 1024U;
  inline constexpr std::size_t maximum_max_bytes = 1024U * 1024U;
  inline constexpr std::size_t default_max_lines = 2000U;
  inline constexpr std::size_t maximum_max_lines = 10000U;
  inline constexpr std::size_t legacy_max_bytes = maximum_max_bytes;

  struct request_t {
    std::size_t max_bytes = default_max_bytes;
    std::size_t max_lines = default_max_lines;
    std::optional<std::uintmax_t> after;
    std::optional<std::uint64_t> after_generation;
  };

  struct response_t {
    std::string content;
    std::uintmax_t start_offset = 0;
    std::uintmax_t end_offset = 0;
    std::uint64_t generation = 0;
    bool truncated = false;
    bool reset = true;
  };

  /**
   * @brief Parse strict decimal query values for the v1 log-tail endpoint.
   * @throws std::invalid_argument when a value is malformed or out of range.
   */
  request_t parse_request(
    std::optional<std::string_view> max_bytes,
    std::optional<std::string_view> max_lines,
    std::optional<std::string_view> after,
    std::optional<std::string_view> after_generation
  );

  /**
   * @brief Apply cursor and logical-line bounds to a bounded file tail.
   *
   * `reset` is false only when the returned bytes are a contiguous delta from
   * the requested `after` offset. Callers must replace local state when it is
   * true.
   */
  response_t build_response(
    file_handler::tail_result_t tail,
    const request_t &request,
    std::uint64_t generation
  );

  /**
   * @brief Serialize a binary-safe v1 response. `content` is Base64 encoded.
   */
  nlohmann::json serialize_response(const response_t &response);
}  // namespace log_tail_api
