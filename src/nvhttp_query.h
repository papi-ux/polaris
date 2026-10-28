/**
 * @file src/nvhttp_query.h
 * @brief Decode individual client launch fields without a session or crypto work.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include <Simple-Web-Server/utility.hpp>

namespace nvhttp {
  using args_t = SimpleWeb::CaseInsensitiveMultimap;

  inline constexpr std::size_t client_launch_key_bytes = 16;

  std::string get_arg(const args_t &args, const char *name, const char *default_value = nullptr);

  // Keep these calls at their original product statements: required-field
  // exceptions must not move across cipher setup, random draws or IV allocation.
  std::string decode_client_rikey(const args_t &args);
  std::int64_t decode_client_corever(const args_t &args);
  std::uint32_t decode_client_iv_prefix(const args_t &args);
}
