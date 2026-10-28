/**
 * @file tests/unit/test_rtsp_handler_parsing.cpp
 * @brief Discriminating tests for the bounds-safe RTSP handler parsing helpers.
 *
 * Each red case models exactly what the handler reads from received bytes and trips a sanitizer on
 * the pre-fix logic; each control pins a valid-input result the fix preserves.
 */
extern "C" {
#include <moonlight-common-c/src/Rtsp.h>
}

#include <src/rtsp_handler_parsing.h>

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace {
  // A heap view sized to exactly its bytes, so one read past the end lands in an ASan redzone,
  // the way the parser's malloc(length + 1) message buffer bounds the request target.
  struct exact_view_t {
    explicit exact_view_t(std::string_view bytes):
        store {std::make_unique<char[]>(bytes.size())},
        size {bytes.size()} {
      std::memcpy(store.get(), bytes.data(), bytes.size());
    }

    std::string_view view() const {
      return {store.get(), size};
    }

    std::unique_ptr<char[]> store;
    std::size_t size;
  };
}  // namespace

// Item 1: a SETUP target without '=' must not step the iterator past the target's end.
TEST(RtspHandlerParsing, SetupTargetWithoutEqualsStaysInBounds) {
  const exact_view_t target {"rtsp://none"};  // no '=' and no '/'
  const auto type = rtsp_stream::setup_stream_type(target.view());
  EXPECT_TRUE(type.empty());
}

// Item 1 control: the valid targets the handler must keep classifying.
TEST(RtspHandlerParsing, SetupTargetTypesAreUnchangedForValidInput) {
  EXPECT_EQ(rtsp_stream::setup_stream_type("streamid=video/0/0"), "video");
  EXPECT_EQ(rtsp_stream::setup_stream_type("streamid=audio/9/0"), "audio");
  EXPECT_EQ(rtsp_stream::setup_stream_type("streamid=control"), "control");
  EXPECT_TRUE(rtsp_stream::setup_stream_type("streamid=").empty());
}

// Item 2: an ANNOUNCE `a=` attribute with an empty value must not index an empty view.
TEST(RtspHandlerParsing, AnnounceEmptyAttributeValueHasNoUndefinedIndex) {
  // The handler's value view is the tail after ':' in a heap-backed payload line; here it is empty.
  const std::string line = "a=x:";
  const std::string_view whole {line};
  const auto value = whole.substr(whole.find(':') + 1);
  ASSERT_TRUE(value.empty());
  EXPECT_TRUE(rtsp_stream::announce_attribute_value(value).empty());
}

// Item 2 control: a single trailing space is trimmed; other values are returned as-is.
TEST(RtspHandlerParsing, AnnounceAttributeValueTrimsOneTrailingSpaceOnly) {
  EXPECT_EQ(rtsp_stream::announce_attribute_value("5 "), "5");
  EXPECT_EQ(rtsp_stream::announce_attribute_value("5"), "5");
  EXPECT_EQ(rtsp_stream::announce_attribute_value("abc"), "abc");
}

// Item 3: a parsed response must not be dispatched as a request command.
TEST(RtspHandlerParsing, ParsedResponseIsNotDispatchedAsACommand) {
  RTSP_MESSAGE resp {};  // value-initialized: the union's bytes start at zero
  resp.type = TYPE_RESPONSE;
  resp.message.response.statusString = const_cast<char *>("SETUP");
  resp.message.response.statusCode = 200;

  const auto command = rtsp_stream::dispatch_command(resp);
  EXPECT_TRUE(command.empty());

  // The pre-fix dispatch returned this response's statusString as the command. When it matched a
  // handler, the handler then read request.target, which for a response aliases the statusCode bytes
  // (here the pointer value 200). Only that pre-fix path reaches the wild read below.
  if (!command.empty()) {
    std::string_view target {resp.message.request.target};
    (void) target.size();
  }
}

// Item 3 control: a genuine request keeps its dispatch command.
TEST(RtspHandlerParsing, ParsedRequestKeepsItsDispatchCommand) {
  RTSP_MESSAGE req {};
  req.type = TYPE_REQUEST;
  req.message.request.command = const_cast<char *>("SETUP");
  req.message.request.target = const_cast<char *>("streamid=video/0/0");
  EXPECT_EQ(rtsp_stream::dispatch_command(req), std::string_view {"SETUP"});
}
