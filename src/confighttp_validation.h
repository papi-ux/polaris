/**
 * @file src/confighttp_validation.h
 * @brief Input validation helpers for Web UI write endpoints.
 */
#pragma once

#include <vector>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace confighttp::validation {
  bool validate_app_payload(const nlohmann::json &payload, std::string &error);
  bool validate_config_payload(const nlohmann::json &payload, std::string &error);
  // Runtime ownership is configured through its dedicated setup path. Generic
  // config GET omits these keys; POST/PATCH reject changes and preserve them.
  bool is_local_config_key(std::string_view key);
  void preserve_local_config(const std::unordered_map<std::string, std::string> &existing,
    nlohmann::json &payload);
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

  /**
   * @brief The keys whose value differs between two parsed configuration files, sorted.
   * A key missing on one side counts as an empty value.
   */
  std::vector<std::string> changed_config_keys(const std::unordered_map<std::string, std::string> &before,
                                               const std::unordered_map<std::string, std::string> &after);

  /**
   * @brief Whether a configuration key is one of the AI explanation settings.
   */
  bool is_ai_config_key(std::string_view key);

  /**
   * @brief Whether the running host applies a saved change to this key without a restart.
   */
  bool is_live_applied_config_key(std::string_view key);

  /**
   * @brief Whether any changed key still needs a restart before it takes effect.
   */
  bool config_change_requires_restart(const std::vector<std::string> &changed_keys);

  /**
   * @brief Whether a written configuration file still needs a restart to take effect.
   * @details Some key the running host does not apply live differs from the file the process loaded
   *          at start. An earlier unrestarted change keeps counting across saves, and a change reverted
   *          to its loaded value needs no restart.
   */
  bool written_config_requires_restart(const std::unordered_map<std::string, std::string> &loaded,
                                       const std::unordered_map<std::string, std::string> &written);
}  // namespace confighttp::validation
