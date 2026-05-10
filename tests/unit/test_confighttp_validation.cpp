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

TEST(ConfigValidationTests, AcceptsBrowserStreamPrimaryAndDeprecatedAliasKeys) {
  nlohmann::json payload = {
    {"browser_streaming", "enabled"},
    {"webrtc_browser_streaming", "disabled"}
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
    {"lutris-runner", "wine"},
    {"platform", "windows"},
    {"runtime", "wine"},
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

TEST(AppValidationTests, RejectsUnknownPlatformMetadata) {
  nlohmann::json payload = {
    {"name", "Broken App"},
    {"platform", "solaris"}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_app_payload(payload, error));
  EXPECT_NE(error.find("platform must be one of"), std::string::npos);
}

TEST(AppValidationTests, RejectsUnknownRuntimeMetadata) {
  nlohmann::json payload = {
    {"name", "Broken App"},
    {"runtime", "dosbox"}
  };

  std::string error;
  EXPECT_FALSE(confighttp::validation::validate_app_payload(payload, error));
  EXPECT_NE(error.find("runtime must be one of"), std::string::npos);
}
