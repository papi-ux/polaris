/**
 * @file src/confighttp_validation.h
 * @brief Input validation helpers for Web UI write endpoints.
 */
#pragma once

#include <span>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace confighttp::validation {
  bool validate_app_payload(const nlohmann::json &payload, std::string &error);
  bool validate_config_payload(const nlohmann::json &payload, std::string &error);
  void normalize_write_only_secret_payload(nlohmann::json &payload);

  // Keys that appear in GET /api/config responses but are derived state, not
  // settings. They must never be written: saveConfig strips them from POST
  // payloads and config load scrubs any that leaked into the config file.
  std::span<const std::string_view> response_only_config_keys();
  bool is_response_only_config_key(std::string_view key);
}  // namespace confighttp::validation
