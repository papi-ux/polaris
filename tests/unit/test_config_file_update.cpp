/**
 * @file tests/unit/test_config_file_update.cpp
 * @brief Tests for lossless Polaris configuration updates.
 */

#include <gtest/gtest.h>

#include "src/config_file_update.h"

TEST(ConfigFileUpdate, EqualValuesPreserveEveryByte) {
  const std::string existing =
    "# Keep this header and CRLF ordering\r\n"
    "adaptive_bitrate_enabled = enabled  # paired client\r\n"
    "unknown_future_setting = keep-me\r\n";

  const auto result = config_file_update::apply(
    existing,
    {{"adaptive_bitrate_enabled", "enabled"}}
  );

  EXPECT_FALSE(result.changed);
  EXPECT_EQ(result.content, existing);
}

TEST(ConfigFileUpdate, RealChangesPreserveFormattingAndAppendDeterministically) {
  const std::string existing =
    "# Host configuration\n"
    "adaptive_bitrate_enabled = disabled  # keep this note\n"
    "unknown_future_setting = keep-me\n";

  const auto result = config_file_update::apply(
    existing,
    {
      {"disconnect_resume_timeout_seconds", "300"},
      {"adaptive_bitrate_enabled", "enabled"},
    }
  );

  ASSERT_TRUE(result.changed);
  EXPECT_EQ(
    result.content,
    "# Host configuration\n"
    "adaptive_bitrate_enabled = enabled  # keep this note\n"
    "unknown_future_setting = keep-me\n"
    "disconnect_resume_timeout_seconds = 300\n"
  );
}

TEST(ConfigFileUpdate, ClearingAValueRemovesEveryDuplicateAssignment) {
  const std::string existing =
    "output_name = DP-1\n"
    "# retained comment\n"
    "output_name = DP-2\n"
    "capture = portal\n";

  const auto result = config_file_update::apply(existing, {{"output_name", ""}});

  ASSERT_TRUE(result.changed);
  EXPECT_EQ(result.content, "# retained comment\ncapture = portal\n");
}

TEST(ConfigFileUpdate, DoesNotTreatAssignmentsInsideListsAsTopLevelSettings) {
  const std::string existing =
    "prep_cmds = [\n"
    "  output_name = nested-value\n"
    "]\n";

  const auto result = config_file_update::apply(existing, {{"output_name", "DP-3"}});

  ASSERT_TRUE(result.changed);
  EXPECT_EQ(
    result.content,
    "prep_cmds = [\n"
    "  output_name = nested-value\n"
    "]\n"
    "output_name = DP-3\n"
  );
}
