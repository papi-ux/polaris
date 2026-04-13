/**
 * @file src/confighttp_validation.cpp
 * @brief Input validation helpers for Web UI write endpoints.
 */
#include "confighttp_validation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

using namespace std::literals;

namespace confighttp::validation {
  namespace {
    template<typename T, std::size_t N>
    bool contains(const std::array<T, N> &values, const T &needle) {
      return std::find(values.begin(), values.end(), needle) != values.end();
    }

    bool has_line_break_or_nul(std::string_view value) {
      return value.find('\n') != std::string_view::npos
        || value.find('\r') != std::string_view::npos
        || value.find('\0') != std::string_view::npos;
    }

    bool is_safe_key(std::string_view key) {
      if (key.empty() || !std::isalnum(static_cast<unsigned char>(key.front()))) {
        return false;
      }

      return std::all_of(key.begin(), key.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.';
      });
    }

    bool is_env_key(std::string_view key) {
      if (key.empty()) {
        return false;
      }

      const auto first = static_cast<unsigned char>(key.front());
      if (!(std::isalpha(first) || first == '_')) {
        return false;
      }

      return std::all_of(key.begin() + 1, key.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '_';
      });
    }

    bool is_uuid(std::string_view value) {
      constexpr std::array<std::size_t, 4> hyphen_positions {8, 13, 18, 23};
      if (value.size() != 36) {
        return false;
      }

      for (std::size_t i = 0; i < value.size(); ++i) {
        if (contains(hyphen_positions, i)) {
          if (value[i] != '-') {
            return false;
          }
          continue;
        }

        if (!std::isxdigit(static_cast<unsigned char>(value[i]))) {
          return false;
        }
      }

      return true;
    }

    bool validate_safe_string(std::string_view key, const nlohmann::json &value, std::string &error) {
      if (!value.is_string()) {
        error = std::string {key} + " must be a string";
        return false;
      }

      const auto text = value.get<std::string>();
      if (has_line_break_or_nul(text)) {
        error = std::string {key} + " contains disallowed control characters";
        return false;
      }

      return true;
    }

    bool validate_scalar_or_nested_strings(const nlohmann::json &value, std::string &error, std::string_view path) {
      if (value.is_string()) {
        return validate_safe_string(path, value, error);
      }

      if (value.is_array()) {
        for (const auto &entry : value) {
          if (!validate_scalar_or_nested_strings(entry, error, path)) {
            return false;
          }
        }
      } else if (value.is_object()) {
        for (const auto &[nested_key, nested_value] : value.items()) {
          if (has_line_break_or_nul(nested_key)) {
            error = std::string {path} + " contains an object key with disallowed control characters";
            return false;
          }
          if (!validate_scalar_or_nested_strings(nested_value, error, path)) {
            return false;
          }
        }
      }

      return true;
    }

    bool validate_array_of_strings(std::string_view key, const nlohmann::json &value, std::string &error) {
      if (!value.is_array()) {
        error = std::string {key} + " must be an array";
        return false;
      }

      for (const auto &entry : value) {
        if (!validate_safe_string(key, entry, error)) {
          return false;
        }
      }

      return true;
    }

    bool validate_non_negative_integer(std::string_view key, const nlohmann::json &value, std::string &error) {
      if (!value.is_number_integer()) {
        error = std::string {key} + " must be an integer";
        return false;
      }

      if (value.get<int64_t>() < 0) {
        error = std::string {key} + " must be non-negative";
        return false;
      }

      return true;
    }

    bool validate_command_array(std::string_view key, const nlohmann::json &value, std::string &error) {
      if (!value.is_array()) {
        error = std::string {key} + " must be an array";
        return false;
      }

      constexpr std::array allowed_command_keys {"do"sv, "undo"sv, "elevated"sv};
      for (const auto &entry : value) {
        if (!entry.is_object()) {
          error = std::string {key} + " entries must be objects";
          return false;
        }

        for (const auto &[command_key, command_value] : entry.items()) {
          if (!contains(allowed_command_keys, std::string_view {command_key})) {
            error = std::string {key} + " contains an unsupported field: " + command_key;
            return false;
          }

          if ((command_key == "do" || command_key == "undo") && !validate_safe_string(command_key, command_value, error)) {
            return false;
          }

          if (command_key == "elevated" && !command_value.is_boolean()) {
            error = std::string {key} + ".elevated must be a boolean";
            return false;
          }
        }
      }

      return true;
    }

    bool validate_env_object(const nlohmann::json &value, std::string &error) {
      if (!value.is_object()) {
        error = "env must be an object";
        return false;
      }

      for (const auto &[env_key, env_value] : value.items()) {
        if (!is_env_key(env_key)) {
          error = "env contains an invalid variable name: " + env_key;
          return false;
        }
        if (!validate_safe_string("env", env_value, error)) {
          return false;
        }
      }

      return true;
    }
  }  // namespace

  bool validate_config_payload(const nlohmann::json &payload, std::string &error) {
    if (!payload.is_object()) {
      error = "Config payload must be a JSON object";
      return false;
    }

    for (const auto &[key, value] : payload.items()) {
      if (!is_safe_key(key)) {
        error = "Config key is not allowed: " + key;
        return false;
      }

      if (!validate_scalar_or_nested_strings(value, error, key)) {
        return false;
      }
    }

    return true;
  }

  bool validate_app_payload(const nlohmann::json &payload, std::string &error) {
    if (!payload.is_object()) {
      error = "App payload must be a JSON object";
      return false;
    }

    constexpr std::array string_keys {
      "cmd"sv,
      "gamepad"sv,
      "id"sv,
      "image-path"sv,
      "name"sv,
      "output"sv,
      "steam-appid"sv,
      "working-dir"sv,
    };
    constexpr std::array bool_keys {
      "allow-client-commands"sv,
      "auto-detach"sv,
      "elevated"sv,
      "exclude-global-prep-cmd"sv,
      "exclude-global-state-cmd"sv,
      "launching"sv,
      "per-client-app-identity"sv,
      "terminate-on-pause"sv,
      "use-app-identity"sv,
      "virtual-display"sv,
      "virtual-display-primary"sv,
      "wait-all"sv,
    };
    constexpr std::array int_keys {
      "exit-timeout"sv,
      "index"sv,
      "last-launched"sv,
      "scale-factor"sv,
    };
    constexpr std::array source_values {
      ""sv,
      "heroic"sv,
      "lutris"sv,
      "manual"sv,
      "steam"sv,
    };
    constexpr std::array category_values {
      ""sv,
      "cinematic"sv,
      "desktop"sv,
      "fast_action"sv,
      "unknown"sv,
      "vr"sv,
    };

    for (const auto &[key, value] : payload.items()) {
      if (!is_safe_key(key)) {
        error = "App key is not allowed: " + key;
        return false;
      }

      const auto key_view = std::string_view {key};
      if (contains(string_keys, key_view)) {
        if (!validate_safe_string(key, value, error)) {
          return false;
        }
        continue;
      }

      if (contains(bool_keys, key_view)) {
        if (!value.is_boolean()) {
          error = key + " must be a boolean";
          return false;
        }
        continue;
      }

      if (contains(int_keys, key_view)) {
        if (!validate_non_negative_integer(key, value, error)) {
          return false;
        }
        continue;
      }

      if (key == "uuid") {
        if (!validate_safe_string(key, value, error)) {
          return false;
        }

        const auto uuid = value.get<std::string>();
        if (!uuid.empty() && !is_uuid(uuid)) {
          error = "uuid must be a valid UUID";
          return false;
        }
        continue;
      }

      if (key == "prep-cmd" || key == "state-cmd") {
        if (!validate_command_array(key, value, error)) {
          return false;
        }
        continue;
      }

      if (key == "detached" || key == "genres") {
        if (!validate_array_of_strings(key, value, error)) {
          return false;
        }
        continue;
      }

      if (key == "env") {
        if (!validate_env_object(value, error)) {
          return false;
        }
        continue;
      }

      if (key == "source") {
        if (!validate_safe_string(key, value, error)) {
          return false;
        }

        if (!contains(source_values, std::string_view {value.get<std::string>()})) {
          error = "source must be one of manual, steam, lutris, or heroic";
          return false;
        }
        continue;
      }

      if (key == "game-category") {
        if (!validate_safe_string(key, value, error)) {
          return false;
        }

        if (!contains(category_values, std::string_view {value.get<std::string>()})) {
          error = "game-category is not recognized";
          return false;
        }
        continue;
      }

      if (!validate_scalar_or_nested_strings(value, error, key)) {
        return false;
      }
    }

    return true;
  }
}  // namespace confighttp::validation
