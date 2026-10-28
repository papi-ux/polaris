/**
 * @file tests/unit/test_rtsp_content_length.cpp
 * @brief Pin the plaintext RTSP Content-length lookup and decoding the request framing uses.
 */
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

#include <src/rtsp_content_length.h>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {
  // Link synthetic headers as the parser leaves them: borrowed strings, no allocation flags.
  struct option_list_t {
    explicit option_list_t(std::vector<std::pair<std::string, std::string>> headers):
        text {std::move(headers)},
        items(text.size()) {
      for (std::size_t i = 0; i < text.size(); ++i) {
        items[i].option = text[i].first.data();
        items[i].content = text[i].second.data();
        items[i].next = i + 1 < items.size() ? &items[i + 1] : nullptr;
      }
    }

    const OPTION_ITEM *head() const {
      return items.empty() ? nullptr : items.data();
    }

    std::vector<std::pair<std::string, std::string>> text;
    std::vector<OPTION_ITEM> items;
  };

  int length_of(const std::string &content) {
    const option_list_t list {{{"CSeq", "1"}, {"Content-length", content}}};
    const auto declared = rtsp_stream::find_content_length(list.head());
    EXPECT_EQ(declared.content, list.items[1].content);
    return declared.length;
  }
}  // namespace

TEST(RtspContentLength, AbsentHeaderMeansNoPayload) {
  const auto none = rtsp_stream::find_content_length(nullptr);
  EXPECT_EQ(none.content, nullptr);
  EXPECT_EQ(none.length, 0);

  const option_list_t list {{{"CSeq", "3"}, {"X-Tag", "12"}}};
  const auto unrelated = rtsp_stream::find_content_length(list.head());
  EXPECT_EQ(unrelated.content, nullptr);
  EXPECT_EQ(unrelated.length, 0);
}

TEST(RtspContentLength, OnlyTheExactSpellingIsRead) {
  const option_list_t list {{{"Content-Length", "5"}, {"content-length", "6"}, {"Content-length ", "7"}}};
  const auto declared = rtsp_stream::find_content_length(list.head());
  EXPECT_EQ(declared.content, nullptr);
  EXPECT_EQ(declared.length, 0);
}

TEST(RtspContentLength, FirstMatchingNodeWins) {
  const option_list_t list {{{"Content-length", "4"}, {"Content-length", "9"}}};
  const auto declared = rtsp_stream::find_content_length(list.head());
  EXPECT_EQ(declared.content, list.items[0].content);
  EXPECT_EQ(declared.length, 4);
}

TEST(RtspContentLength, DecodingKeepsTheLenientFromCharsArithmetic) {
  const std::vector<std::pair<std::string, int>> cases {
    {"", 0}, {" ", 0}, {"none", 0}, {"0", 0}, {"42", 42}, {" 42", 42},
    {"len=42", 42}, {"-5", 5}, {"12abc", 17451}, {"99999999999999999999", 0},
    {"2147483647", 2147483647}, {"4294967296", 0}, {"4294967338", 42}
  };
  for (const auto &[content, expected] : cases) {
    SCOPED_TRACE(content);
    EXPECT_EQ(length_of(content), expected);
  }
}
