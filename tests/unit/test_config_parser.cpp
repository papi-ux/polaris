/**
 * @file tests/unit/test_config_parser.cpp
 * @brief Test configuration value parsers.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/nvenc/nvenc_config.h>
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
