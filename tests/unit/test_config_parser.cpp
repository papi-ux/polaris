/**
 * @file tests/unit/test_config_parser.cpp
 * @brief Test configuration value parsers.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/nvenc/nvenc_config.h>

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
