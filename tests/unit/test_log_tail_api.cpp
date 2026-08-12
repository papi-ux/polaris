/**
 * @file tests/unit/test_log_tail_api.cpp
 * @brief Test the versioned bounded log-tail contract.
 */
#include <array>
#include <gtest/gtest.h>
#include <src/log_tail_api.h>
#include <utility>

using namespace std::literals;
namespace {
  constexpr std::uint64_t generation = 7;
}

TEST(LogTailApiRequestTests, UsesBoundedDefaults) {
  const auto request = log_tail_api::parse_request(std::nullopt, std::nullopt, std::nullopt, std::nullopt);

  EXPECT_EQ(request.max_bytes, log_tail_api::default_max_bytes);
  EXPECT_EQ(request.max_lines, log_tail_api::default_max_lines);
  EXPECT_FALSE(request.after.has_value());
  EXPECT_FALSE(request.after_generation.has_value());
}

TEST(LogTailApiRequestTests, ParsesStrictDecimalLimitsAndCursor) {
  const auto request = log_tail_api::parse_request("4096"sv, "25"sv, "123"sv, "7"sv);

  EXPECT_EQ(request.max_bytes, 4096);
  EXPECT_EQ(request.max_lines, 25);
  ASSERT_TRUE(request.after.has_value());
  EXPECT_EQ(*request.after, 123);
  ASSERT_TRUE(request.after_generation.has_value());
  EXPECT_EQ(*request.after_generation, generation);
}

TEST(LogTailApiRequestTests, RejectsMalformedAndOutOfRangeValues) {
  for (const auto value : {""sv, "0"sv, "-1"sv, "+1"sv, " 1"sv, "1x"sv, "999999999999999999999999"sv}) {
    EXPECT_THROW(log_tail_api::parse_request(value, std::nullopt, std::nullopt, std::nullopt), std::invalid_argument) << value;
  }
  EXPECT_THROW(
    log_tail_api::parse_request(std::to_string(log_tail_api::maximum_max_bytes + 1), std::nullopt, std::nullopt, std::nullopt),
    std::invalid_argument
  );

  for (const auto value : {""sv, "0"sv, "-1"sv, "+1"sv, " 1"sv, "1x"sv, "999999999999999999999999"sv}) {
    EXPECT_THROW(log_tail_api::parse_request(std::nullopt, value, std::nullopt, std::nullopt), std::invalid_argument) << value;
  }
  EXPECT_THROW(
    log_tail_api::parse_request(std::nullopt, std::to_string(log_tail_api::maximum_max_lines + 1), std::nullopt, std::nullopt),
    std::invalid_argument
  );

  for (const auto value : {""sv, "-1"sv, "+1"sv, " 1"sv, "1x"sv, "999999999999999999999999"sv}) {
    EXPECT_THROW(log_tail_api::parse_request(std::nullopt, std::nullopt, value, "7"sv), std::invalid_argument) << value;
    EXPECT_THROW(log_tail_api::parse_request(std::nullopt, std::nullopt, "1"sv, value), std::invalid_argument) << value;
  }

  EXPECT_THROW(log_tail_api::parse_request(std::nullopt, std::nullopt, "1"sv, std::nullopt), std::invalid_argument);
  EXPECT_THROW(log_tail_api::parse_request(std::nullopt, std::nullopt, std::nullopt, "7"sv), std::invalid_argument);
}

TEST(LogTailApiResponseTests, InitialResponseRequiresResetAndPreservesTailMetadata) {
  file_handler::tail_result_t tail {
    .content = "recent",
    .start_offset = 10,
    .end_offset = 16,
    .truncated = true,
  };
  const auto request = log_tail_api::parse_request(std::nullopt, std::nullopt, std::nullopt, std::nullopt);

  const auto response = log_tail_api::build_response(std::move(tail), request, generation);

  EXPECT_EQ(response.content, "recent");
  EXPECT_EQ(response.start_offset, 10);
  EXPECT_EQ(response.end_offset, 16);
  EXPECT_EQ(response.generation, generation);
  EXPECT_TRUE(response.truncated);
  EXPECT_TRUE(response.reset);
}

TEST(LogTailApiResponseTests, ValidCursorReturnsOnlyContiguousDelta) {
  file_handler::tail_result_t tail {
    .content = "abcdefghij",
    .start_offset = 90,
    .end_offset = 100,
    .truncated = true,
  };
  const auto request = log_tail_api::parse_request(std::nullopt, std::nullopt, "95"sv, "7"sv);

  const auto response = log_tail_api::build_response(std::move(tail), request, generation);

  EXPECT_EQ(response.content, "fghij");
  EXPECT_EQ(response.start_offset, 95);
  EXPECT_EQ(response.end_offset, 100);
  EXPECT_TRUE(response.truncated);
  EXPECT_FALSE(response.reset);
}

TEST(LogTailApiResponseTests, CursorAtEndReturnsAnEmptyContiguousDelta) {
  file_handler::tail_result_t tail {
    .content = "abcdefghij",
    .start_offset = 90,
    .end_offset = 100,
    .truncated = true,
  };
  const auto request = log_tail_api::parse_request(std::nullopt, std::nullopt, "100"sv, "7"sv);

  const auto response = log_tail_api::build_response(std::move(tail), request, generation);

  EXPECT_TRUE(response.content.empty());
  EXPECT_EQ(response.start_offset, 100);
  EXPECT_EQ(response.end_offset, 100);
  EXPECT_FALSE(response.reset);
}

TEST(LogTailApiResponseTests, CursorOutsideAvailableRangeRequiresReset) {
  const auto request_before = log_tail_api::parse_request(std::nullopt, std::nullopt, "89"sv, "7"sv);
  const auto request_after = log_tail_api::parse_request(std::nullopt, std::nullopt, "101"sv, "7"sv);

  for (const auto &request : {request_before, request_after}) {
    file_handler::tail_result_t tail {
      .content = "abcdefghij",
      .start_offset = 90,
      .end_offset = 100,
      .truncated = true,
    };
    const auto response = log_tail_api::build_response(std::move(tail), request, generation);

    EXPECT_EQ(response.content, "abcdefghij");
    EXPECT_EQ(response.start_offset, 90);
    EXPECT_EQ(response.end_offset, 100);
    EXPECT_TRUE(response.truncated);
    EXPECT_TRUE(response.reset);
  }
}

TEST(LogTailApiResponseTests, ReusedOffsetsFromAnotherGenerationRequireReset) {
  file_handler::tail_result_t tail {
    .content = "new generation",
    .start_offset = 90,
    .end_offset = 104,
    .truncated = false,
  };
  const auto request = log_tail_api::parse_request(std::nullopt, std::nullopt, "95"sv, "6"sv);

  const auto response = log_tail_api::build_response(std::move(tail), request, generation);

  EXPECT_EQ(response.content, "new generation");
  EXPECT_EQ(response.start_offset, 90);
  EXPECT_EQ(response.generation, generation);
  EXPECT_TRUE(response.reset);
}

TEST(LogTailApiResponseTests, LineLimitKeepsNewestLogicalLinesAndAdjustsOffset) {
  file_handler::tail_result_t tail {
    .content = "one\ntwo\nthree\nfour\n",
    .start_offset = 100,
    .end_offset = 119,
    .truncated = false,
  };
  const auto request = log_tail_api::parse_request(std::nullopt, "2"sv, std::nullopt, std::nullopt);

  const auto response = log_tail_api::build_response(std::move(tail), request, generation);

  EXPECT_EQ(response.content, "three\nfour\n");
  EXPECT_EQ(response.start_offset, 108);
  EXPECT_EQ(response.end_offset, 119);
  EXPECT_TRUE(response.truncated);
  EXPECT_TRUE(response.reset);
}

TEST(LogTailApiResponseTests, LineLimitHandlesContentWithoutTrailingNewline) {
  file_handler::tail_result_t tail {
    .content = "one\ntwo\nthree",
    .start_offset = 0,
    .end_offset = 13,
    .truncated = false,
  };
  const auto request = log_tail_api::parse_request(std::nullopt, "1"sv, std::nullopt, std::nullopt);

  const auto response = log_tail_api::build_response(std::move(tail), request, generation);

  EXPECT_EQ(response.content, "three");
  EXPECT_EQ(response.start_offset, 8);
  EXPECT_EQ(response.end_offset, 13);
  EXPECT_TRUE(response.truncated);
}

TEST(LogTailApiResponseTests, LineLimitForcesResetWhenItDropsPartOfAnIncrementalDelta) {
  file_handler::tail_result_t tail {
    .content = "one\ntwo\nthree\n",
    .start_offset = 0,
    .end_offset = 14,
    .truncated = false,
  };
  const auto request = log_tail_api::parse_request(std::nullopt, "1"sv, "4"sv, "7"sv);

  const auto response = log_tail_api::build_response(std::move(tail), request, generation);

  EXPECT_EQ(response.content, "three\n");
  EXPECT_EQ(response.start_offset, 8);
  EXPECT_EQ(response.end_offset, 14);
  EXPECT_TRUE(response.truncated);
  EXPECT_TRUE(response.reset);
}

TEST(LogTailApiResponseTests, SerializationIsBinarySafeAndVersioned) {
  log_tail_api::response_t response {
    .content = std::string {"\0\xff", 2},
    .start_offset = 12,
    .end_offset = 14,
    .generation = generation,
    .truncated = true,
    .reset = false,
  };

  const auto output = log_tail_api::serialize_response(response);

  EXPECT_TRUE(output.at("status"));
  EXPECT_EQ(output.at("schema_version"), 1);
  EXPECT_EQ(output.at("content_encoding"), "base64");
  EXPECT_EQ(output.at("content"), "AP8=");
  EXPECT_EQ(output.at("content_bytes"), 2);
  EXPECT_EQ(output.at("start_offset"), 12);
  EXPECT_EQ(output.at("end_offset"), 14);
  EXPECT_EQ(output.at("generation"), generation);
  EXPECT_TRUE(output.at("truncated"));
  EXPECT_FALSE(output.at("reset"));
}

TEST(LogTailApiResponseTests, Base64SerializationCoversEveryQuantumShape) {
  const std::array examples {
    std::pair {""s, ""s},
    std::pair {"f"s, "Zg=="s},
    std::pair {"fo"s, "Zm8="s},
    std::pair {"foo"s, "Zm9v"s},
    std::pair {"foobar"s, "Zm9vYmFy"s},
  };

  for (const auto &[content, encoded] : examples) {
    const log_tail_api::response_t response {.content = content};
    EXPECT_EQ(log_tail_api::serialize_response(response).at("content"), encoded);
  }
}
