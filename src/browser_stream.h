/**
 * @file src/browser_stream.h
 * @brief Browser Stream API and session metadata helpers.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace browser_stream {
  constexpr std::uint16_t default_webtransport_port = 47992;

  struct session_token_t {
    std::string token;
    std::chrono::steady_clock::time_point expires_at;
  };

  bool build_enabled();
  nlohmann::json status_json();
  nlohmann::json create_session(
    std::string_view remote_address,
    std::string_view host,
    std::string_view app_uuid = {},
    std::string_view profile_id = "balanced"
  );
  nlohmann::json submit_input(std::string_view token, const nlohmann::json &events);
  bool stop_session(std::string_view token);
  session_token_t issue_session_token(std::string_view remote_address, std::string_view app_uuid = {}, bool owns_app = false);
  bool consume_session_token(std::string_view token, std::string_view remote_address);
}  // namespace browser_stream
