/**
 * @file src/confighttp_validation.h
 * @brief Input validation helpers for Web UI write endpoints.
 */
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace confighttp::validation {
  bool validate_app_payload(const nlohmann::json &payload, std::string &error);
  bool validate_config_payload(const nlohmann::json &payload, std::string &error);
  void normalize_write_only_secret_payload(nlohmann::json &payload);
}  // namespace confighttp::validation
