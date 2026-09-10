/**
 * @file tests/unit/test_config_parser.cpp
 * @brief Test configuration value parsers.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/nvenc/nvenc_config.h>
#include <src/private_state_file.h>
#include <src/utility.h>

TEST(ConfigParserTests, ProtocolDecimalsUseDotAndRequireTheWholeValue) {
  const auto fps = util::parse_decimal<double>("60.0");
  ASSERT_TRUE(fps.has_value());
  EXPECT_DOUBLE_EQ(*fps, 60.0);

  const auto fractional = util::parse_decimal<double>("59.94");
  ASSERT_TRUE(fractional.has_value());
  EXPECT_DOUBLE_EQ(*fractional, 59.94);

  EXPECT_FALSE(util::parse_decimal<double>("60,0").has_value());
  EXPECT_FALSE(util::parse_decimal<double>("60.0fps").has_value());
  EXPECT_FALSE(util::parse_decimal<double>(" 60.0").has_value());
  EXPECT_FALSE(util::parse_decimal<double>("nan").has_value());
  EXPECT_FALSE(util::parse_decimal<double>("inf").has_value());
}

TEST(ConfigParserTests, ProtocolDecimalFormattingNeverUsesAComma) {
  EXPECT_EQ(util::format_decimal(59.94), "59.94");
  EXPECT_EQ(util::format_decimal(60.0), "60");
}

TEST(ConfigParserTests, ParsesNvencSplitEncodeModeValues) {
  EXPECT_EQ(config::nv::split_encode_mode_from_view("disabled"), nvenc::nvenc_split_encode_mode::disabled);
  EXPECT_EQ(config::nv::split_encode_mode_from_view("auto"), nvenc::nvenc_split_encode_mode::auto_mode);
  EXPECT_EQ(config::nv::split_encode_mode_from_view("forced"), nvenc::nvenc_split_encode_mode::forced);
  EXPECT_EQ(config::nv::split_encode_mode_from_view("2"), nvenc::nvenc_split_encode_mode::two_way);
  EXPECT_EQ(config::nv::split_encode_mode_from_view("3"), nvenc::nvenc_split_encode_mode::three_way);
}

TEST(ConfigParserTests, UnknownNvencSplitEncodeModeFallsBackToDisabled) {
  EXPECT_EQ(config::nv::split_encode_mode_from_view("not-a-real-mode"), nvenc::nvenc_split_encode_mode::disabled);
}

namespace {
  bool contains(const std::string &text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
  }
}  // namespace

TEST(ConfigParserTests, MoonlightMultiseatInputDefaultsOff) {
  EXPECT_FALSE(config::input.multiseat_moonlight_input);
}

TEST(ConfigParserTests, BooleanValuesParseRegardlessOfCase) {
  // The lowercasing in to_bool() was a no-op, so every capitalized spelling in a
  // hand-edited polaris.conf read as false without saying anything.
  EXPECT_EQ(config::parse_bool("Enabled"), std::optional<bool> {true});
  EXPECT_EQ(config::parse_bool("TRUE"), std::optional<bool> {true});
  EXPECT_EQ(config::parse_bool("On"), std::optional<bool> {true});
  EXPECT_EQ(config::parse_bool("Disabled"), std::optional<bool> {false});
  EXPECT_EQ(config::parse_bool("OFF"), std::optional<bool> {false});
}

TEST(ConfigParserTests, DocumentedBooleanSpellingsParseBothWays) {
  for (const auto *value : {"true", "yes", "enable", "enabled", "on", "1"}) {
    EXPECT_EQ(config::parse_bool(value), std::optional<bool> {true}) << value;
  }

  for (const auto *value : {"false", "no", "disable", "disabled", "off", "0"}) {
    EXPECT_EQ(config::parse_bool(value), std::optional<bool> {false}) << value;
  }
}

TEST(ConfigParserTests, AValueThatIsNotABooleanIsReportedRatherThanReadAsFalse) {
  // #517, found via #409: linux_capture_profile = gpu_native is a mode name on a
  // boolean key. It read as false, capture telemetry stayed off, and nothing in
  // the log said the value had been rejected.
  EXPECT_FALSE(config::parse_bool("gpu_native").has_value());
  EXPECT_FALSE(config::parse_bool("sometimes").has_value());
  EXPECT_FALSE(config::parse_bool("").has_value());
}

TEST(ConfigParserTests, PlausibleBackButtonTimeoutsAreNotWarnedAbout) {
  // -1 is the documented way to disable Home emulation, and any other negative
  // value disables it too, so neither is a mistake.
  EXPECT_TRUE(config::back_button_timeout_warning(-1).empty());
  EXPECT_TRUE(config::back_button_timeout_warning(-500).empty());

  // At and above the emulated press length the setting behaves as described.
  EXPECT_TRUE(config::back_button_timeout_warning(config::back_button_emulated_press_ms).empty());
  EXPECT_TRUE(config::back_button_timeout_warning(2000).empty());
}

TEST(ConfigParserTests, BackButtonTimeoutShorterThanTheEmulatedPressIsWarnedAbout) {
  // The value from #222: intended as two seconds, applied as two milliseconds,
  // which turned every Select press into Home.
  const auto advice = config::back_button_timeout_warning(2);
  ASSERT_FALSE(advice.empty());

  EXPECT_TRUE(contains(advice, "2 milliseconds"));
  // Naming the replacement value is the whole point; a warning that only says
  // "too small" leaves the reader exactly where they started.
  EXPECT_TRUE(contains(advice, "use 2000"));
  EXPECT_TRUE(contains(advice, "-1 to disable"));
}

TEST(ConfigParserTests, BackButtonTimeoutWarningPluralizesTheSecondsGuess) {
  EXPECT_TRUE(contains(config::back_button_timeout_warning(1), "1 second, use 1000"));
  EXPECT_TRUE(contains(config::back_button_timeout_warning(5), "5 seconds, use 5000"));
}

TEST(ConfigParserTests, ZeroBackButtonTimeoutWarnsWithoutASecondsGuess) {
  const auto advice = config::back_button_timeout_warning(0);
  ASSERT_FALSE(advice.empty());

  // Zero fires Home immediately, so it is worth warning about, but "0 seconds"
  // is not a guess at intent worth printing. Match the guess clause rather than
  // the word "second", which also occurs inside "milliseconds".
  EXPECT_FALSE(contains(advice, "If you meant"));
  EXPECT_TRUE(contains(advice, "0 milliseconds"));
  EXPECT_TRUE(contains(advice, "-1 to disable"));
}

TEST(ConfigParserTests, VaapiOptionsParseAndCanReturnToAutomatic) {
  const auto initial = config::parse_vaapi_settings({
    {"vaapi_quality", "balanced"}, {"vaapi_rc", "qvbr"}, {"vaapi_blbrc", "enabled"}, {"vaapi_strict_rc_buffer", "true"}
  });
  EXPECT_EQ(initial.quality, config::vaapi::quality_e::balanced);
  EXPECT_EQ(initial.rc, config::vaapi::rc_e::qvbr);
  EXPECT_EQ(initial.blbrc, true);
  EXPECT_TRUE(initial.strict_rc_buffer);
  const auto restored = config::parse_vaapi_settings({
    {"vaapi_quality", "auto"}, {"vaapi_rc", "auto"}, {"vaapi_blbrc", "auto"}, {"vaapi_strict_rc_buffer", "false"}
  }, initial);
  EXPECT_EQ(restored.quality, config::vaapi::quality_e::automatic);
  EXPECT_EQ(restored.rc, config::vaapi::rc_e::automatic);
  EXPECT_FALSE(restored.blbrc.has_value());
  EXPECT_FALSE(restored.strict_rc_buffer);
  const auto invalid = config::parse_vaapi_settings({
    {"vaapi_quality", "ultra"}, {"vaapi_rc", "unknown"}, {"vaapi_blbrc", "sometimes"}, {"vaapi_strict_rc_buffer", "unknown"}
  }, initial);
  EXPECT_EQ(invalid.quality, initial.quality);
  EXPECT_EQ(invalid.rc, initial.rc);
  EXPECT_EQ(invalid.blbrc, initial.blbrc);
  EXPECT_EQ(invalid.strict_rc_buffer, initial.strict_rc_buffer);
}

TEST(ConfigParserTests, ConcurrentVaapiSnapshotsNeverMixSavedSettings) {
  const auto saved = config::vaapi::snapshot();
  auto restore = util::fail_guard([&] { config::vaapi::publish(saved); });
  const config::vaapi::settings_t manual {
    .strict_rc_buffer = true, .quality = config::vaapi::quality_e::quality,
    .rc = config::vaapi::rc_e::qvbr, .blbrc = true
  };
  config::vaapi::publish({});
  std::thread writer([&] {
    for (int i = 0; i < 20000; ++i) config::vaapi::publish(i % 2 ? manual : config::vaapi::settings_t {});
  });
  for (int i = 0; i < 20000; ++i) {
    const auto value = config::vaapi::snapshot();
    const bool automatic = value.quality == config::vaapi::quality_e::automatic;
    EXPECT_EQ(value.rc, automatic ? config::vaapi::rc_e::automatic : manual.rc);
    EXPECT_EQ(value.strict_rc_buffer, !automatic);
    EXPECT_EQ(value.blbrc, automatic ? std::optional<bool> {} : manual.blbrc);
  }
  writer.join();
}

#ifdef __linux__
TEST(ConfigParserTests, FailedConfigWriteDoesNotPublishVaapiSettings) {
  const auto saved = config::vaapi::snapshot();
  const auto directory = std::filesystem::temp_directory_path() / ("polaris-vaapi-write-failure-" + std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count()));
  auto restore = util::fail_guard([&] {
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
    config::vaapi::publish(saved);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  });
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
  const auto path = (directory / "polaris.conf").string();
  config::vaapi::publish({});
  ASSERT_EQ(config::write_config_with_vaapi_settings(path, ""), 0);
  // The protected writer rejects device nodes before writing. Exercise failures
  // after admission instead, and verify both the saved file and published state.
  for (const auto fault : {private_state_file::write_fault_e::short_write,
         private_state_file::write_fault_e::flush, private_state_file::write_fault_e::sync,
         private_state_file::write_fault_e::rename}) {
    SCOPED_TRACE(static_cast<int>(fault));
    private_state_file::set_write_fault_for_tests(fault);
    ASSERT_NE(config::write_config_with_vaapi_settings(path,
      "vaapi_quality = quality\nvaapi_rc = vbr\nvaapi_blbrc = enabled\nvaapi_strict_rc_buffer = enabled\n"), 0);
    const auto after = config::vaapi::snapshot();
    EXPECT_EQ(after.quality, config::vaapi::quality_e::automatic);
    EXPECT_EQ(after.rc, config::vaapi::rc_e::automatic);
    EXPECT_FALSE(after.blbrc.has_value());
    EXPECT_FALSE(after.strict_rc_buffer);
    const auto persisted = private_state_file::read_secure(path, 4096);
    ASSERT_TRUE(persisted);
    EXPECT_TRUE(persisted.payload.empty());
  }
}
#endif

TEST(ConfigParserTests, SuccessfulConfigWritePublishesCompleteVaapiSettingsAndClearRestoresDefaults) {
  const auto saved = config::vaapi::snapshot();
  const auto directory = std::filesystem::temp_directory_path() / ("polaris-vaapi-config-" + std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count()));
  auto restore = util::fail_guard([&] {
    config::vaapi::publish(saved);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  });
  // The atomic writer requires an owned parent that others cannot modify.
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
  const auto path = directory / "polaris.conf";
  ASSERT_EQ(config::write_config_with_vaapi_settings(path.string(),
    "vaapi_quality = balanced\nvaapi_rc = qvbr\nvaapi_blbrc = enabled\nvaapi_strict_rc_buffer = enabled\n"), 0);
  const auto after = config::vaapi::snapshot();
  EXPECT_EQ(after.quality, config::vaapi::quality_e::balanced);
  EXPECT_EQ(after.rc, config::vaapi::rc_e::qvbr);
  EXPECT_EQ(after.blbrc, true);
  EXPECT_TRUE(after.strict_rc_buffer);
  ASSERT_EQ(config::write_config_with_vaapi_settings(path.string(), ""), 0);
  const auto cleared = config::vaapi::snapshot();
  EXPECT_EQ(cleared.quality, config::vaapi::quality_e::automatic);
  EXPECT_EQ(cleared.rc, config::vaapi::rc_e::automatic);
  EXPECT_FALSE(cleared.blbrc.has_value());
  EXPECT_FALSE(cleared.strict_rc_buffer);
}
