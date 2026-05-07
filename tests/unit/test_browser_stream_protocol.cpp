/**
 * @file tests/unit/test_browser_stream_protocol.cpp
 * @brief Test Browser Stream media envelope helpers.
 */

#include "../tests_common.h"

#include <src/browser_stream_protocol.h>

TEST(BrowserStreamProtocolTests, RoundTripsMediaDatagram) {
  browser_stream::protocol::media_envelope_t input;
  input.kind = browser_stream::protocol::media_kind::video;
  input.codec = browser_stream::protocol::codec_id::h264;
  input.flags = browser_stream::protocol::keyframe;
  input.sequence = 42;
  input.frame_id = 7;
  input.timestamp_us = 123456;
  input.chunk_index = 1;
  input.chunk_count = 3;
  input.payload = {1, 2, 3, 4};

  auto datagram = browser_stream::protocol::encode_datagram(input);

  browser_stream::protocol::media_envelope_t output;
  std::string error;
  ASSERT_TRUE(browser_stream::protocol::decode_datagram(datagram, output, error)) << error;

  EXPECT_EQ(output.kind, input.kind);
  EXPECT_EQ(output.codec, input.codec);
  EXPECT_EQ(output.flags, input.flags);
  EXPECT_EQ(output.sequence, input.sequence);
  EXPECT_EQ(output.frame_id, input.frame_id);
  EXPECT_EQ(output.timestamp_us, input.timestamp_us);
  EXPECT_EQ(output.chunk_index, input.chunk_index);
  EXPECT_EQ(output.chunk_count, input.chunk_count);
  EXPECT_EQ(output.payload, input.payload);
}

TEST(BrowserStreamProtocolTests, RejectsInvalidChunkIndexes) {
  browser_stream::protocol::media_envelope_t input;
  input.kind = browser_stream::protocol::media_kind::audio;
  input.codec = browser_stream::protocol::codec_id::opus;
  input.chunk_index = 2;
  input.chunk_count = 2;
  input.payload = {1};

  auto datagram = browser_stream::protocol::encode_datagram(input);

  browser_stream::protocol::media_envelope_t output;
  std::string error;
  EXPECT_FALSE(browser_stream::protocol::decode_datagram(datagram, output, error));
  EXPECT_NE(error.find("invalid chunk"), std::string::npos);
}
