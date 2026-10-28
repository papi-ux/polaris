/**
 * @file tests/unit/test_control_messages.cpp
 * @brief Pin the control-stream decoders to the inline reads their handlers used before extraction.
 *
 * Each case computes the expected value with the exact expression the handler lambda in
 * stream.cpp ran inline, then asserts the extracted decoder returns the same for a
 * representative payload. All buffers are at least the size the handler read, so the
 * reference reads stay in bounds; the decoders reproduce the unbounded reads only when a
 * fuzzer feeds a shorter payload, which these tests do not.
 */
#include <src/control_messages.h>
#include <src/utility.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {
  std::string_view view_of(const std::vector<std::uint8_t> &bytes) {
    return std::string_view {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
  }
}  // namespace

TEST(ControlMessageDecoders, LossStatsReadsTheSameInt32RecordAsTheHandler) {
  // Handler: int32_t *stats = (int32_t *) payload.data(); stats[0], stats[1], stats[3].
  alignas(std::int32_t) std::array<std::int32_t, 4> record {-7, 123456, 9999, -2024};
  std::string_view payload {reinterpret_cast<const char *>(record.data()), record.size() * sizeof(std::int32_t)};

  const auto *stats = reinterpret_cast<const std::int32_t *>(payload.data());
  const auto decoded = stream::decode_loss_stats(payload);
  EXPECT_EQ(decoded.count, stats[0]);
  EXPECT_EQ(decoded.time_ms, stats[1]);
  EXPECT_EQ(decoded.last_good_frame, stats[3]);
  EXPECT_EQ(decoded.count, -7);
  EXPECT_EQ(decoded.time_ms, 123456);
  EXPECT_EQ(decoded.last_good_frame, -2024);

  // Index 2 is never read, and trailing bytes beyond the record are ignored.
  alignas(std::int32_t) std::array<std::int32_t, 6> longer {1, 2, 3, 4, 5, 6};
  std::string_view extra {reinterpret_cast<const char *>(longer.data()), longer.size() * sizeof(std::int32_t)};
  const auto decoded_longer = stream::decode_loss_stats(extra);
  EXPECT_EQ(decoded_longer.count, 1);
  EXPECT_EQ(decoded_longer.time_ms, 2);
  EXPECT_EQ(decoded_longer.last_good_frame, 4);
}

TEST(ControlMessageDecoders, InvalidateRefFramesReadsTheSameInt64PairAsTheHandler) {
  // Handler: std::int64_t *frames = (std::int64_t *) payload.data(); frames[0], frames[1].
  alignas(std::int64_t) std::array<std::int64_t, 2> record {-1, 0x7fffffffffffffffLL};
  std::string_view payload {reinterpret_cast<const char *>(record.data()), record.size() * sizeof(std::int64_t)};

  const auto *frames = reinterpret_cast<const std::int64_t *>(payload.data());
  const auto decoded = stream::decode_invalidate_ref_frames(payload);
  EXPECT_EQ(decoded.first_frame, frames[0]);
  EXPECT_EQ(decoded.last_frame, frames[1]);
  EXPECT_EQ(decoded.first_frame, -1);
  EXPECT_EQ(decoded.last_frame, 0x7fffffffffffffffLL);
}

TEST(ControlMessageDecoders, InputCipherDecodesTheBigEndianLengthAndBoundsTheView) {
  // Too short to carry the 4-byte length: the handler warns and drops it.
  const std::vector<std::uint8_t> runt {0x00, 0x00, 0x01};
  const auto no_length = stream::decode_input_cipher(view_of(runt));
  EXPECT_FALSE(no_length.has_length);
  EXPECT_FALSE(no_length.fits);

  // Declared length 20, exactly 20 cipher bytes follow: fits, and the view is those bytes.
  std::vector<std::uint8_t> packet {0x00, 0x00, 0x00, 0x14};
  for (std::uint8_t i = 0; i < 20; ++i) {
    packet.push_back(static_cast<std::uint8_t>(0xA0 + i));
  }
  const auto payload = view_of(packet);
  const auto expected_length = util::endian::big(*reinterpret_cast<const std::int32_t *>(payload.data()));
  const auto decoded = stream::decode_input_cipher(payload);
  EXPECT_TRUE(decoded.has_length);
  EXPECT_EQ(decoded.declared_length, expected_length);
  EXPECT_EQ(decoded.declared_length, 20);
  EXPECT_TRUE(decoded.fits);
  EXPECT_EQ(decoded.cipher, payload.substr(sizeof(std::int32_t), 20));

  // One more declared byte than arrived: has_length, but it does not fit, and no view.
  std::vector<std::uint8_t> overlong {0x00, 0x00, 0x00, 0x15};
  for (std::uint8_t i = 0; i < 20; ++i) {
    overlong.push_back(i);
  }
  const auto decoded_overlong = stream::decode_input_cipher(view_of(overlong));
  EXPECT_TRUE(decoded_overlong.has_length);
  EXPECT_EQ(decoded_overlong.declared_length, 21);
  EXPECT_FALSE(decoded_overlong.fits);

  // A negative declared length widens to SIZE_MAX if used; it must not fit.
  const std::vector<std::uint8_t> negative {0xff, 0xff, 0xff, 0xff, 0x00, 0x00};
  const auto decoded_negative = stream::decode_input_cipher(view_of(negative));
  EXPECT_TRUE(decoded_negative.has_length);
  EXPECT_EQ(decoded_negative.declared_length, -1);
  EXPECT_FALSE(decoded_negative.fits);
}

TEST(ControlMessageDecoders, EncryptedControlDecodesTheLittleEndianHeaderAndBoundsTheView) {
  // Too short for the header (payload + 2 < 8, i.e. fewer than 6 bytes): not present.
  const std::vector<std::uint8_t> tiny {0x18, 0x00, 0x01, 0x00, 0x00};
  const auto absent = stream::decode_encrypted_control(view_of(tiny));
  EXPECT_FALSE(absent.header_present);

  // length 0x18 = 24 (the 16 + 4 + 4 floor), seq 0x04030201, then length - 4 = 20 body bytes.
  std::vector<std::uint8_t> packet {0x18, 0x00, 0x01, 0x02, 0x03, 0x04};
  for (std::uint8_t i = 0; i < 20; ++i) {
    packet.push_back(static_cast<std::uint8_t>(0x10 + i));
  }
  const auto payload = view_of(packet);
  std::uint16_t raw_length;
  std::uint32_t raw_seq;
  std::memcpy(&raw_length, payload.data(), sizeof(raw_length));
  std::memcpy(&raw_seq, payload.data() + sizeof(raw_length), sizeof(raw_seq));
  const auto decoded = stream::decode_encrypted_control(payload);
  EXPECT_TRUE(decoded.header_present);
  EXPECT_FALSE(decoded.runt);
  EXPECT_TRUE(decoded.fits);
  EXPECT_EQ(decoded.length, util::endian::little(raw_length));
  EXPECT_EQ(decoded.length, 24);
  EXPECT_EQ(decoded.seq, util::endian::little(raw_seq));
  EXPECT_EQ(decoded.seq, 0x04030201u);
  // header->payload() is 6 bytes into the handler's payload; length - 4 = 20 bytes.
  EXPECT_EQ(decoded.cipher, payload.substr(stream::encrypted_control_header_size - stream::control_type_field_size, 20));

  // Below the floor: present, but runt, and no view.
  const std::vector<std::uint8_t> runt {0x17, 0x00, 0x01, 0x02, 0x03, 0x04, 0x00};
  const auto decoded_runt = stream::decode_encrypted_control(view_of(runt));
  EXPECT_TRUE(decoded_runt.header_present);
  EXPECT_TRUE(decoded_runt.runt);
  EXPECT_EQ(decoded_runt.length, 23);
  EXPECT_FALSE(decoded_runt.fits);

  // Declares 24 (needs 20 body bytes) but only a few arrived: present, not runt, does not fit.
  const std::vector<std::uint8_t> short_body {0x18, 0x00, 0x01, 0x02, 0x03, 0x04, 0x00, 0x00};
  const auto decoded_short = stream::decode_encrypted_control(view_of(short_body));
  EXPECT_TRUE(decoded_short.header_present);
  EXPECT_FALSE(decoded_short.runt);
  EXPECT_FALSE(decoded_short.fits);
}

TEST(ControlMessageDecoders, ServerCmdIndexReadsTheFirstByte) {
  // Handler: uint8_t cmdIndex = *(uint8_t*)payload.data();
  const std::vector<std::uint8_t> payload {0x07, 0x55, 0x55};
  EXPECT_EQ(stream::decode_server_cmd_index(view_of(payload)), 0x07);
}

TEST(ControlMessageDecoders, LengthChecksMatchTheirDocumentedBoundaries) {
  // The same assertions the full-runtime ControlPacketBounds suite pins, reachable in the fast build.
  EXPECT_FALSE(stream::control_packet_carries_type(0));
  EXPECT_FALSE(stream::control_packet_carries_type(1));
  EXPECT_TRUE(stream::control_packet_carries_type(2));

  EXPECT_FALSE(stream::encrypted_control_header_present(5));
  EXPECT_TRUE(stream::encrypted_control_header_present(6));

  EXPECT_FALSE(stream::encrypted_control_cipher_fits(6, 23));
  EXPECT_FALSE(stream::encrypted_control_cipher_fits(6, 24));
  EXPECT_TRUE(stream::encrypted_control_cipher_fits(26, 24));

  EXPECT_FALSE(stream::input_control_cipher_fits(3, 0));
  EXPECT_TRUE(stream::input_control_cipher_fits(4, 0));
  EXPECT_TRUE(stream::input_control_cipher_fits(20, 16));
  EXPECT_FALSE(stream::input_control_cipher_fits(20, 17));
  EXPECT_FALSE(stream::input_control_cipher_fits(1024, -1));
}
