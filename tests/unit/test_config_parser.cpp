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
