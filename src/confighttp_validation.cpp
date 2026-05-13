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

    constexpr std::array allowed_config_keys {
      "adapter_name"sv,
      "adaptive_bitrate_enabled"sv,
      "adaptive_bitrate_max"sv,
      "adaptive_bitrate_min"sv,
      "address_family"sv,
      "ai_api_key"sv,
      "ai_auth_mode"sv,
      "ai_base_url"sv,
      "ai_cache_ttl_hours"sv,
      "ai_enabled"sv,
      "ai_model"sv,
      "ai_provider"sv,
      "ai_timeout_ms"sv,
      "ai_use_subscription"sv,
      "always_send_scancodes"sv,
      "amd_coder"sv,
      "amd_enforce_hrd"sv,
      "amd_preanalysis"sv,
      "amd_quality"sv,
      "amd_rc"sv,
      "amd_usage"sv,
      "amd_vbaq"sv,
      "api_key"sv,
      "audio_sink"sv,
      "auto_capture_sink"sv,
      "av1_mode"sv,
      "back_button_timeout"sv,
      "browser_streaming"sv,
      "capture"sv,
      "cert"sv,
      "color_range"sv,
      "controller"sv,
      "credentials_file"sv,
      "dd_config_revert_delay"sv,
      "dd_config_revert_on_disconnect"sv,
      "dd_configuration_option"sv,
      "dd_hdr_option"sv,
      "dd_manual_refresh_rate"sv,
      "dd_manual_resolution"sv,
      "dd_mode_remapping"sv,
      "disconnect_resume_timeout_seconds"sv,
      "dd_refresh_rate_option"sv,
      "dd_resolution_option"sv,
      "dd_wa_hdr_toggle_delay"sv,
      "double_refreshrate"sv,
      "ds4_back_as_touchpad_click"sv,
      "ds5_inputtino_randomize_mac"sv,
      "enable_discovery"sv,
      "enable_input_only_mode"sv,
      "enable_pairing"sv,
      "encoder"sv,
      "envvar_compatibility_mode"sv,
      "external_ip"sv,
      "fallback_mode"sv,
      "fec_percentage"sv,
      "file_apps"sv,
      "file_state"sv,
      "forward_rumble"sv,
      "gamepad"sv,
      "global_prep_cmd"sv,
      "global_state_cmd"sv,
      "hdr_mode"sv,
      "headless_mode"sv,
      "hevc_mode"sv,
      "hide_tray_controls"sv,
      "high_resolution_scrolling"sv,
      "ignore_encoder_probe_failure"sv,
      "install_steam_audio_drivers"sv,
      "isolated_virtual_display_option"sv,
      "keep_sink_default"sv,
      "key_repeat_delay"sv,
      "key_repeat_frequency"sv,
      "key_rightalt_to_key_win"sv,
      "keybindings"sv,
      "keyboard"sv,
      "lan_encryption_mode"sv,
      "legacy_ordering"sv,
      "limit_framerate"sv,
      "linux_auto_manage_displays"sv,
      "linux_capture_profile"sv,
      "linux_prefer_gpu_native_capture"sv,
      "linux_primary_output"sv,
      "linux_streaming_output"sv,
      "linux_use_cage_compositor"sv,
      "locale"sv,
      "log_path"sv,
      "max_bitrate"sv,
      "max_sessions"sv,
      "min_log_level"sv,
      "min_threads"sv,
      "minimum_fps_target"sv,
      "motion_as_ds4"sv,
      "mouse"sv,
      "mouse_cursor_visible"sv,
      "native_pen_touch"sv,
      "notify_pre_releases"sv,
      "nvenc_h264_cavlc"sv,
      "nvenc_intra_refresh"sv,
      "nvenc_latency_over_power"sv,
      "nvenc_opengl_vulkan_on_dxgi"sv,
      "nvenc_preset"sv,
      "nvenc_realtime_hags"sv,
      "nvenc_spatial_aq"sv,
      "nvenc_tune"sv,
      "nvenc_twopass"sv,
      "nvenc_vbv_increase"sv,
      "origin_web_ui_allowed"sv,
      "output_name"sv,
      "ping_timeout"sv,
      "pkey"sv,
      "port"sv,
      "qp"sv,
      "qsv_coder"sv,
      "qsv_preset"sv,
      "qsv_slow_hevc"sv,
      "recording_enabled"sv,
      "recording_output_dir"sv,
      "recording_replay_buffer"sv,
      "recording_replay_buffer_minutes"sv,
      "server_cmd"sv,
      "steamgriddb_api_key"sv,
      "stream_audio"sv,
      "sunshine_name"sv,
      "sw_preset"sv,
      "sw_tune"sv,
      "system_tray"sv,
      "touchpad_as_ds4"sv,
      "trusted_subnet_auto_pairing"sv,
      "trusted_subnets"sv,
      "upnp"sv,
      "vaapi_strict_rc_buffer"sv,
      "virtual_sink"sv,
      "webrtc_browser_streaming"sv,
      "vt_coder"sv,
      "vt_realtime"sv,
      "vt_software"sv,
      "wan_encryption_mode"sv,
    };

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

      if (!contains(allowed_config_keys, std::string_view {key})) {
        error = "Unsupported config key: " + key;
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
