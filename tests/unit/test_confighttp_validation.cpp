/**
 * @file tests/unit/test_confighttp_validation.cpp
 * @brief Test src/confighttp_validation.*.
 */
#include "../tests_common.h"

#include <algorithm>
#include <array>

#include <src/confighttp_validation.h>

TEST(ConfigValidationTests, RejectsConfigKeysThatCanBreakSerialization) {
  nlohmann::json payload = {
    {"safe_key", "ok"},
    {"bad\nkey", "oops"}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_config_payload(payload, error));
  EXPECT_NE(error.find("Config key is not allowed"), std::string::npos);
}

TEST(ConfigValidationTests, RejectsConfigValuesWithEmbeddedLineBreaks) {
  nlohmann::json payload = {
    {"output_name", "line1\nline2"}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_config_payload(payload, error));
  EXPECT_NE(error.find("contains disallowed control characters"), std::string::npos);
}

TEST(ConfigValidationTests, RejectsUnsupportedConfigKeys) {
  nlohmann::json payload = {
    {"definitely_not_a_real_config_key", "value"}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_config_payload(payload, error));
  EXPECT_NE(error.find("Unsupported config key"), std::string::npos);
}

TEST(ConfigValidationTests, AcceptsAiApiKeyClearForPersistence) {
  nlohmann::json payload = {
    {"ai_api_key", ""},
    {"clear_ai_api_key", true}
  };

  std::string error;
  EXPECT_TRUE(confighttp::validation::validate_config_payload(payload, error)) << error;
}

TEST(ConfigValidationTests, RejectsNonBooleanAiApiKeyClearFlag) {
  nlohmann::json payload = {
    {"clear_ai_api_key", "true"}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_config_payload(payload, error));
  EXPECT_NE(error.find("clear_ai_api_key must be a boolean"), std::string::npos);
}

TEST(ConfigValidationTests, RejectsNonBooleanSteamGridDbApiKeyClearFlag) {
  nlohmann::json payload = {
    {"clear_steamgriddb_api_key", "true"}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_config_payload(payload, error));
  EXPECT_NE(error.find("clear_steamgriddb_api_key must be a boolean"), std::string::npos);
}

TEST(ConfigValidationTests, RedactedSecretPlaceholdersPreserveExistingSecrets) {
  nlohmann::json payload = {
    {"api_key", ""},
    {"ai_api_key", ""},
    {"steamgriddb_api_key", ""},
    {"clear_ai_api_key", false},
    {"clear_steamgriddb_api_key", false},
    {"sunshine_name", "Updated host"}
  };

  confighttp::validation::normalize_write_only_secret_payload(payload);

  EXPECT_FALSE(payload.contains("api_key"));
  EXPECT_FALSE(payload.contains("ai_api_key"));
  EXPECT_FALSE(payload.contains("steamgriddb_api_key"));
  EXPECT_FALSE(payload.contains("clear_ai_api_key"));
  EXPECT_FALSE(payload.contains("clear_steamgriddb_api_key"));
  EXPECT_EQ(payload.at("sunshine_name"), "Updated host");
}

TEST(ConfigValidationTests, ExplicitSecretClearFlagsWinOverPlaceholders) {
  nlohmann::json payload = {
    {"ai_api_key", "replacement-that-must-not-survive"},
    {"steamgriddb_api_key", "replacement-that-must-not-survive"},
    {"clear_ai_api_key", true},
    {"clear_steamgriddb_api_key", true}
  };

  confighttp::validation::normalize_write_only_secret_payload(payload);

  EXPECT_EQ(payload.at("ai_api_key"), "");
  EXPECT_EQ(payload.at("steamgriddb_api_key"), "");
  EXPECT_FALSE(payload.contains("clear_ai_api_key"));
  EXPECT_FALSE(payload.contains("clear_steamgriddb_api_key"));
}

TEST(ConfigValidationTests, NonEmptySecretReplacementsRemainExplicit) {
  nlohmann::json payload = {
    {"api_key", "legacy-new"},
    {"ai_api_key", "ai-new"},
    {"steamgriddb_api_key", "sgdb-new"}
  };

  confighttp::validation::normalize_write_only_secret_payload(payload);

  EXPECT_EQ(payload.at("api_key"), "legacy-new");
  EXPECT_EQ(payload.at("ai_api_key"), "ai-new");
  EXPECT_EQ(payload.at("steamgriddb_api_key"), "sgdb-new");
}

TEST(ConfigValidationTests, AcceptsBrowserStreamPrimaryAndDeprecatedAliasKeys) {
  nlohmann::json payload = {
    {"browser_streaming", "enabled"},
    {"webrtc_browser_streaming", "disabled"}
  };

  std::string error;
  EXPECT_TRUE(confighttp::validation::validate_config_payload(payload, error)) << error;
}

TEST(ConfigValidationTests, AcceptsNvencSplitEncodeModeConfigKey) {
  nlohmann::json payload = {
    {"nvenc_split_encode_mode", "disabled"}
  };

  std::string error;
  EXPECT_TRUE(confighttp::validation::validate_config_payload(payload, error)) << error;
}

TEST(ConfigValidationTests, AcceptsClientGamepadSeatIsolationConfigKey) {
  nlohmann::json payload = {
    {"client_gamepad_seat_isolation", "enabled"}
  };

  std::string error;
  EXPECT_TRUE(confighttp::validation::validate_config_payload(payload, error)) << error;
}

TEST(ResponseOnlyConfigKeyTests, CoversEveryKeyTheLegacyListsScrubbed) {
  // Union of the two lists previously hardcoded in confighttp.cpp (saveConfig)
  // and config.cpp (apply_config). Removing any of these from the canonical
  // list would let derived state leak back into the config file.
  constexpr std::array legacy_keys {
    "ai_auto_quality_enabled",
    "client_settings_authority",
    "client_settings_available",
    "client_settings_effective_stream_display_mode",
    "client_settings_effective_stream_display_mode_label",
    "client_settings_endpoint",
    "client_settings_endpoint_base_url",
    "client_settings_endpoint_https_port",
    "client_settings_endpoint_origin",
    "client_settings_endpoint_path",
    "client_settings_endpoint_same_origin",
    "client_settings_endpoint_url",
    "client_settings_live_fields",
    "client_settings_relaunch_required",
    "client_settings_restart_fields",
    "client_settings_stream_display_mode",
    "client_settings_stream_display_mode_label",
    "client_settings_sync_mode",
    "client_settings_v1",
    "has_ai_api_key",
    "has_api_key",
    "has_steamgriddb_api_key",
    "platform",
    "runtime_backend",
    "runtime_effective_headless",
    "runtime_gpu_native_override_active",
    "runtime_requested_headless",
    "status",
    "stream_display_mode",
    "stream_display_mode_options",
    "vdisplayAvailable",
    "vdisplayBackend",
    "vdisplayStatus",
    "version",
  };

  for (const auto key : legacy_keys) {
    EXPECT_TRUE(confighttp::validation::is_response_only_config_key(key)) << key;
  }
}

TEST(ResponseOnlyConfigKeyTests, ScrubsStreamPathIdentityAndSelfDescribingKeys) {
  // getConfig emits these, but before the canonical list neither C++ scrub
  // covered them, so a full-config POST persisted them into the config file.
  EXPECT_TRUE(confighttp::validation::is_response_only_config_key("stream_path_id"));
  EXPECT_TRUE(confighttp::validation::is_response_only_config_key("stream_path_label"));
  EXPECT_TRUE(confighttp::validation::is_response_only_config_key("config_response_only_keys"));
}

TEST(ResponseOnlyConfigKeyTests, LeavesRealConfigKeysWritable) {
  // The GameStream sync-field name ai_auto_quality_enabled is response-only,
  // but the config keys behind it must stay writable.
  constexpr std::array writable_keys {
    "adaptive_bitrate_enabled",
    "ai_enabled",
    "audio_sink",
    "fallback_mode",
    "linux_stream_mode",
    "max_bitrate",
  };

  for (const auto key : writable_keys) {
    EXPECT_FALSE(confighttp::validation::is_response_only_config_key(key)) << key;
  }
}

TEST(ResponseOnlyConfigKeyTests, ListStaysSortedAndUnique) {
  const auto keys = confighttp::validation::response_only_config_keys();
  EXPECT_TRUE(std::ranges::is_sorted(keys));
  EXPECT_EQ(std::ranges::adjacent_find(keys), keys.end());
}

TEST(AppValidationTests, AcceptsAWellFormedAppPayload) {
  nlohmann::json payload = {
    {"name", "Nova"},
    {"uuid", "12345678-1234-1234-1234-1234567890ab"},
    {"cmd", "/usr/bin/steam"},
    {"auto-detach", true},
    {"wait-all", true},
    {"exit-timeout", 5},
    {"scale-factor", 100},
    {"env", {{"MANGOHUD", "1"}}},
    {"prep-cmd", {{{"do", "echo start"}, {"undo", "echo stop"}, {"elevated", false}}}},
    {"detached", {"mangohud --dlsym"}},
    {"game-category", "fast_action"},
    {"source", "manual"}
  };

  std::string error;
  EXPECT_TRUE(confighttp::validation::validate_app_payload(payload, error)) << error;
}

TEST(AppValidationTests, RejectsMalformedUuidValues) {
  nlohmann::json payload = {
    {"name", "Broken App"},
    {"uuid", "not-a-uuid"}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_app_payload(payload, error));
  EXPECT_NE(error.find("valid UUID"), std::string::npos);
}

TEST(AppValidationTests, RejectsInvalidEnvironmentVariables) {
  nlohmann::json payload = {
    {"name", "Broken App"},
    {"env", {{"BAD-KEY", "1"}}}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_app_payload(payload, error));
  EXPECT_NE(error.find("invalid variable name"), std::string::npos);
}

TEST(AppValidationTests, RejectsUnexpectedCommandShapes) {
  nlohmann::json payload = {
    {"name", "Broken App"},
    {"prep-cmd", {{{"do", "echo hi"}, {"shell", "bash"}}}}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_app_payload(payload, error));
  EXPECT_NE(error.find("unsupported field"), std::string::npos);
}

TEST(ConfigValidationTests, PatchMergeKeepsUnmentionedKeysAndDropsEmptiedOnes) {
  // POST /api/config rewrites the file from its body; a one-key PATCH must
  // not do that. Found the hard way when a single-key POST truncated a live
  // config to two lines.
  const std::unordered_map<std::string, std::string> existing {
    {"port", "47989"},
    {"encoder", "nvenc"},
    {"steamgriddb_api_key", "kept-secret"},
    {"already_blank", ""},
  };
  const nlohmann::json patch {
    {"encoder", "vaapi"},
    {"port", nullptr},
    {"max_sessions", 2},
    {"headless_mode", ""},
  };
  const auto merged = confighttp::validation::merge_config_patch(existing, patch);
  EXPECT_EQ(merged.value("encoder", ""), "vaapi");
  EXPECT_FALSE(merged.contains("port"));
  EXPECT_EQ(merged.value("max_sessions", 0), 2);
  EXPECT_EQ(merged.value("steamgriddb_api_key", ""), "kept-secret");
  EXPECT_FALSE(merged.contains("already_blank"));
  EXPECT_FALSE(merged.contains("headless_mode"));
}

TEST(ConfigValidationTests, PatchMergeClearsASecretOnlyThroughAnExplicitEmptyValue) {
  const std::unordered_map<std::string, std::string> existing {
    {"steamgriddb_api_key", "old-secret"},
    {"ai_api_key", "other-secret"},
    {"port", "47989"},
  };
  // After normalize_write_only_secret_payload, an empty secret means "clear".
  const nlohmann::json patch {{"steamgriddb_api_key", ""}, {"port", ""}};
  const auto merged = confighttp::validation::merge_config_patch(existing, patch);
  ASSERT_TRUE(merged.contains("steamgriddb_api_key"));
  EXPECT_EQ(merged["steamgriddb_api_key"], "");
  EXPECT_EQ(merged.value("ai_api_key", ""), "other-secret");
  EXPECT_FALSE(merged.contains("port"));
}
