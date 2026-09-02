/**
 * @file tests/unit/test_confighttp_validation.cpp
 * @brief Test src/confighttp_validation.*.
 */
#include "../tests_common.h"

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
