/**
 * @file src/confighttp_validation.h
 * @brief Input validation helpers for Web UI write endpoints.
 */
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace confighttp::validation {
  bool validate_app_payload(const nlohmann::json &payload, std::string &error);
  bool validate_config_payload(const nlohmann::json &payload, std::string &error);
  void normalize_write_only_secret_payload(nlohmann::json &payload);

  // Merge a partial config write onto the existing file contents. Keys the
  // patch does not mention keep their current values; a key set to null or an
  // empty string is dropped so it reverts to its default. Secrets arrive
  // already normalized, so an empty secret is an explicit clear and stays in
  // the result as an empty assignment.
  nlohmann::json merge_config_patch(
    const std::unordered_map<std::string, std::string> &existing,
    const nlohmann::json &patch
  );

  // Keys that appear in GET /api/config responses but are derived state, not
  // settings. They must never be written: saveConfig strips them from POST
  // payloads and config load scrubs any that leaked into the config file.
  std::span<const std::string_view> response_only_config_keys();
  bool is_response_only_config_key(std::string_view key);
}  // namespace confighttp::validation
