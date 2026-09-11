/**
 * @file tests/unit/test_multiseat_worker_protocol.cpp
 * @brief Contract tests for the local multiseat worker protocol.
 */
#include "src/multiseat_worker_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {
  using namespace multiseat::worker_ipc;

  endpoint_identity_t identity() {
    return {
      .controller_epoch = "controller-a1b2",
      .logical_gpu_id = "gpu-primary",
      .slot = 7,
      .generation = 42,
      .worker_name = "polaris-worker-controller-a1b2-42",
    };
  }

  capability_t capability() {
    capability_t value {};
    for (std::size_t index = 0; index < value.size(); ++index) {
      value[index] = static_cast<std::uint8_t>(index);
    }
    return value;
  }

  challenge_t challenge() {
    challenge_t value {};
    for (std::size_t index = 0; index < value.size(); ++index) {
      value[index] = static_cast<std::uint8_t>(0xa0U + index);
    }
    return value;
  }

  std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
      result.push_back(digits[byte >> 4U]);
      result.push_back(digits[byte & 0x0FU]);
    }
    return result;
  }

  std::vector<std::uint8_t> heartbeat_frame() {
    return encode_frame({
      .channel = channel_e::control,
      .message = message_e::heartbeat,
      .slot = 7,
      .generation = 42,
      .sequence = 3,
    });
  }
}  // namespace

TEST(MultiseatWorkerProtocol, CapabilityEncodingIsCanonicalAndStrict) {
  const auto encoded = capability_hex(capability());
  EXPECT_EQ(
    encoded,
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
  );
  ASSERT_TRUE(parse_capability_hex(encoded));
  EXPECT_EQ(*parse_capability_hex(encoded), capability());
  EXPECT_FALSE(parse_capability_hex(encoded + "00"));
  EXPECT_FALSE(parse_capability_hex(encoded.substr(1)));

  auto uppercase = encoded;
  uppercase[20] = 'A';
  EXPECT_FALSE(parse_capability_hex(uppercase));
  auto non_hex = encoded;
  non_hex[20] = 'g';
  EXPECT_FALSE(parse_capability_hex(non_hex));
}

TEST(MultiseatWorkerProtocol, AuthenticationMatchesLanguageNeutralGoldenVectors) {
  const auto controller = authentication_proof(
    capability(),
    proof_role_e::controller,
    channel_e::control,
    identity(),
    challenge()
  );
  const auto worker = authentication_proof(
    capability(),
    proof_role_e::worker,
    channel_e::control,
    identity(),
    challenge()
  );
  ASSERT_TRUE(controller);
  ASSERT_TRUE(worker);
  EXPECT_EQ(
    hex(*controller),
    "f2bf394498b9a0bf65a3bc7cc9fb75b48eecf54f5f12923ad2aeb8b7f30d73db"
  );
  EXPECT_EQ(
    hex(*worker),
    "86dfcf28d826940e7695bfd705f40b14ffa1d1efb5179a1e6dbcd76ae05247c3"
  );
  EXPECT_NE(*controller, *worker);
  EXPECT_TRUE(verify_authentication_proof(
    capability(),
    proof_role_e::controller,
    channel_e::control,
    identity(),
    challenge(),
    *controller
  ));
}

TEST(MultiseatWorkerProtocol, AuthenticationIsBoundToEveryAuthorityField) {
  const auto proof = authentication_proof(
    capability(),
    proof_role_e::controller,
    channel_e::control,
    identity(),
    challenge()
  );
  ASSERT_TRUE(proof);

  auto changed = identity();
  changed.generation++;
  EXPECT_FALSE(verify_authentication_proof(
    capability(), proof_role_e::controller, channel_e::control, changed, challenge(), *proof
  ));
  changed = identity();
  changed.slot++;
  EXPECT_FALSE(verify_authentication_proof(
    capability(), proof_role_e::controller, channel_e::control, changed, challenge(), *proof
  ));
  changed = identity();
  changed.worker_name += "-other";
  EXPECT_FALSE(verify_authentication_proof(
    capability(), proof_role_e::controller, channel_e::control, changed, challenge(), *proof
  ));
  EXPECT_FALSE(verify_authentication_proof(
    capability(), proof_role_e::controller, channel_e::media, identity(), challenge(), *proof
  ));
  EXPECT_FALSE(verify_authentication_proof(
    capability(), proof_role_e::worker, channel_e::control, identity(), challenge(), *proof
  ));
}

TEST(MultiseatWorkerProtocol, FrameEncodingMatchesLanguageNeutralGoldenVector) {
  const auto encoded = heartbeat_frame();
  EXPECT_EQ(
    hex(encoded),
    "50535731010400000000000000000007000000000000002a0000000000000003"
  );
  const auto parsed = parse_frame(encoded, channel_e::control, 7, 42);
  ASSERT_EQ(parsed.status, parse_status_e::complete);
  ASSERT_TRUE(parsed.frame);
  EXPECT_EQ(parsed.consumed, header_size);
  EXPECT_EQ(parsed.required, header_size);
  EXPECT_EQ(parsed.frame->message, message_e::heartbeat);
  EXPECT_EQ(parsed.frame->sequence, 3U);
}

TEST(MultiseatWorkerProtocol, ParserIsIncrementalAndLeavesFollowingFrame) {
  auto first = heartbeat_frame();
  auto second = first;
  second[31] = 4;
  first.insert(first.end(), second.begin(), second.end());

  auto parsed = parse_frame(
    std::span<const std::uint8_t> {first}.first(10),
    channel_e::control,
    7,
    42
  );
  EXPECT_EQ(parsed.status, parse_status_e::incomplete);
  EXPECT_EQ(parsed.required, header_size);

  parsed = parse_frame(first, channel_e::control, 7, 42);
  ASSERT_EQ(parsed.status, parse_status_e::complete);
  EXPECT_EQ(parsed.consumed, header_size);
  const auto following = parse_frame(
    std::span<const std::uint8_t> {first}.subspan(parsed.consumed),
    channel_e::control,
    7,
    42
  );
  ASSERT_EQ(following.status, parse_status_e::complete);
  EXPECT_EQ(following.frame->sequence, 4U);
}

TEST(MultiseatWorkerProtocol, ParserRejectsAuthorityAndHeaderConfusion) {
  const auto clean = heartbeat_frame();
  EXPECT_EQ(
    parse_frame(clean, channel_e::media, 7, 42).status,
    parse_status_e::rejected
  );
  EXPECT_EQ(
    parse_frame(clean, channel_e::control, 8, 42).status,
    parse_status_e::rejected
  );
  EXPECT_EQ(
    parse_frame(clean, channel_e::control, 7, 43).status,
    parse_status_e::rejected
  );

  for (const auto offset : std::array<std::size_t, 3> {0, 6, 16}) {
    auto corrupted = clean;
    corrupted[offset] ^= 0xffU;
    EXPECT_EQ(
      parse_frame(corrupted, channel_e::control, 7, 42).status,
      parse_status_e::rejected
    ) << "offset " << offset;
  }
  auto zero_sequence = clean;
  std::fill(zero_sequence.begin() + 24, zero_sequence.end(), 0);
  EXPECT_EQ(
    parse_frame(zero_sequence, channel_e::control, 7, 42).status,
    parse_status_e::rejected
  );
}

TEST(MultiseatWorkerProtocol, AdvertisedPayloadBoundsFailBeforeAllocation) {
  auto oversized_control = heartbeat_frame();
  oversized_control[5] = static_cast<std::uint8_t>(message_e::input);
  const auto advertised = static_cast<std::uint32_t>(max_control_payload + 1);
  oversized_control[8] = static_cast<std::uint8_t>(advertised >> 24U);
  oversized_control[9] = static_cast<std::uint8_t>(advertised >> 16U);
  oversized_control[10] = static_cast<std::uint8_t>(advertised >> 8U);
  oversized_control[11] = static_cast<std::uint8_t>(advertised);
  const auto rejected = parse_frame(oversized_control, channel_e::control, 7, 42);
  EXPECT_EQ(rejected.status, parse_status_e::rejected);
  EXPECT_EQ(rejected.required, 0U);

  auto bounded_media = heartbeat_frame();
  bounded_media[4] = static_cast<std::uint8_t>(channel_e::media);
  bounded_media[5] = static_cast<std::uint8_t>(message_e::video);
  const auto media_size = static_cast<std::uint32_t>(max_media_payload);
  bounded_media[8] = static_cast<std::uint8_t>(media_size >> 24U);
  bounded_media[9] = static_cast<std::uint8_t>(media_size >> 16U);
  bounded_media[10] = static_cast<std::uint8_t>(media_size >> 8U);
  bounded_media[11] = static_cast<std::uint8_t>(media_size);
  const auto incomplete = parse_frame(bounded_media, channel_e::media, 7, 42);
  EXPECT_EQ(incomplete.status, parse_status_e::incomplete);
  EXPECT_EQ(incomplete.required, header_size + max_media_payload);
}

TEST(MultiseatWorkerProtocol, EncoderRejectsWrongChannelShapeAndZeroSequence) {
  EXPECT_THROW(
    (void) encode_frame({
      .channel = channel_e::control,
      .message = message_e::video,
      .slot = 7,
      .generation = 42,
      .sequence = 1,
      .payload = {1},
    }),
    std::invalid_argument
  );
  EXPECT_THROW(
    (void) encode_frame({
      .channel = channel_e::control,
      .message = message_e::heartbeat,
      .slot = 7,
      .generation = 42,
      .sequence = 0,
    }),
    std::invalid_argument
  );
}

TEST(MultiseatWorkerProtocol, DataPlaneAttachmentAndDirectionShapesAreBounded) {
  for (const auto channel : {channel_e::control, channel_e::media}) {
    EXPECT_NO_THROW((void) encode_frame({
      .channel = channel,
      .message = message_e::attach,
      .slot = 7,
      .generation = 42,
      .sequence = 1,
    }));
  }
  EXPECT_THROW(
    (void) encode_frame({
      .channel = channel_e::media,
      .message = message_e::input_ack,
      .slot = 7,
      .generation = 42,
      .sequence = 1,
    }),
    std::invalid_argument
  );
  EXPECT_THROW(
    (void) encode_frame({
      .channel = channel_e::control,
      .message = message_e::attached,
      .slot = 7,
      .generation = 42,
      .sequence = 1,
      .payload = {1},
    }),
    std::invalid_argument
  );
}

TEST(MultiseatWorkerProtocol, SequenceGuardRejectsReplayAndGaps) {
  sequence_guard_t guard;
  EXPECT_EQ(guard.next(), 1U);
  EXPECT_TRUE(guard.accept(1));
  EXPECT_FALSE(guard.accept(1));
  EXPECT_FALSE(guard.accept(3));
  EXPECT_TRUE(guard.accept(2));
  EXPECT_EQ(guard.next(), 3U);
}

TEST(MultiseatWorkerProtocol, InvalidIdentityCannotProduceProof) {
  auto invalid = identity();
  invalid.generation = 0;
  EXPECT_FALSE(valid_identity(invalid));
  EXPECT_FALSE(authentication_proof(
    capability(),
    proof_role_e::controller,
    channel_e::control,
    invalid,
    challenge()
  ));
  invalid = identity();
  invalid.controller_epoch = "../other";
  EXPECT_FALSE(valid_identity(invalid));
}

namespace {
  media_config_t golden_media_config() {
    return {
      .video_codec = video_codec_e::h264,
      .profile_idc = 100,
      .level_idc = 41,
      .width = 1920,
      .height = 1080,
      .fps_numerator = 60,
      .fps_denominator = 1,
      .bitrate_ceiling_kbps = 20000,
      .audio_codec = audio_codec_e::opus,
      .audio_channels = 2,
      .audio_frame_duration_us = 5000,
      .audio_sample_rate = 48000,
    };
  }
}  // namespace

TEST(MultiseatWorkerProtocol, MediaConfigMatchesLanguageNeutralGoldenVector) {
  const auto encoded = encode_media_config(golden_media_config());
  ASSERT_EQ(encoded.size(), media_config_size);
  EXPECT_EQ(
    hex(encoded),
    "01016429078004380000003c0000000100004e20010213880000bb8000000000"
  );
  const auto parsed = parse_media_config(encoded);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, golden_media_config());

  EXPECT_FALSE(parse_media_config(std::span {encoded}.first(media_config_size - 1)));
  EXPECT_FALSE(parse_media_config({}));

  // A body the worker never produced must not decode into a usable contract.
  for (const auto &[offset, value] : {
         std::pair<std::size_t, std::uint8_t> {0, 2},   // unknown contract version
         std::pair<std::size_t, std::uint8_t> {31, 1},  // reserved tail in use
         std::pair<std::size_t, std::uint8_t> {1, 7},   // unknown video codec
       }) {
    auto body = encoded;
    body[offset] = value;
    EXPECT_FALSE(parse_media_config(body)) << "offset " << offset;
  }
}

TEST(MultiseatWorkerProtocol, MediaConfigRejectsContractsTheHostCannotHonor) {
  const std::vector<std::pair<std::string, media_config_t>> rejected {
    {"unknown video codec", [] { auto c = golden_media_config(); c.video_codec = static_cast<video_codec_e>(2); return c; }()},
    {"unknown audio codec", [] { auto c = golden_media_config(); c.audio_codec = static_cast<audio_codec_e>(2); return c; }()},
    {"odd width", [] { auto c = golden_media_config(); c.width = 1921; return c; }()},
    {"tiny height", [] { auto c = golden_media_config(); c.height = 8; return c; }()},
    {"zero fps", [] { auto c = golden_media_config(); c.fps_numerator = 0; return c; }()},
    {"zero fps denominator", [] { auto c = golden_media_config(); c.fps_denominator = 0; return c; }()},
    {"sub-hertz fps", [] { auto c = golden_media_config(); c.fps_numerator = 1; c.fps_denominator = 2; return c; }()},
    {"kilohertz fps", [] { auto c = golden_media_config(); c.fps_numerator = 1001; return c; }()},
    {"zero bitrate", [] { auto c = golden_media_config(); c.bitrate_ceiling_kbps = 0; return c; }()},
    {"wrong sample rate", [] { auto c = golden_media_config(); c.audio_sample_rate = 44100; return c; }()},
    {"no channels", [] { auto c = golden_media_config(); c.audio_channels = 0; return c; }()},
    {"illegal frame duration", [] { auto c = golden_media_config(); c.audio_frame_duration_us = 3000; return c; }()},
    {"unknown profile", [] { auto c = golden_media_config(); c.profile_idc = 1; return c; }()},
    {"unknown level", [] { auto c = golden_media_config(); c.level_idc = 99; return c; }()},
  };
  for (const auto &[name, config] : rejected) {
    EXPECT_FALSE(valid_media_config(config)) << name;
    EXPECT_THROW((void) encode_media_config(config), std::invalid_argument) << name;
  }
}

TEST(MultiseatWorkerProtocol, MediaFrameMatchesLanguageNeutralGoldenVector) {
  const media_frame_t frame {
    .frame_index = 7,
    .idr = true,
    .capture_timestamp_ns = 1000000000,
    .encode_timestamp_ns = 1000500000,
  };
  const std::string_view sample = "h264";
  const std::span<const std::uint8_t> bytes {
    reinterpret_cast<const std::uint8_t *>(sample.data()), sample.size()
  };
  const auto payload = encode_media_frame(frame, bytes);
  EXPECT_EQ(
    hex(payload),
    "01010000000000000000000000000007000000003b9aca00000000003ba26b2068323634"
  );

  std::span<const std::uint8_t> encoded;
  const auto parsed = parse_media_frame(payload, encoded);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, frame);
  EXPECT_EQ(std::string(encoded.begin(), encoded.end()), "h264");

  EXPECT_THROW((void) encode_media_frame(frame, {}), std::invalid_argument);
  // A prefix with nothing after it carries no frame.
  EXPECT_FALSE(parse_media_frame(std::span {payload}.first(media_frame_prefix_size), encoded));

  for (const auto &[offset, value] : {
         std::pair<std::size_t, std::uint8_t> {0, 0},     // unknown contract version
         std::pair<std::size_t, std::uint8_t> {1, 0x03},  // unknown flag bit
         std::pair<std::size_t, std::uint8_t> {2, 1},     // reserved 16 in use
         std::pair<std::size_t, std::uint8_t> {7, 1},     // reserved 32 in use
       }) {
    auto body = payload;
    body[offset] = value;
    EXPECT_FALSE(parse_media_frame(body, encoded)) << "offset " << offset;
  }
}

TEST(MultiseatWorkerProtocol, FrameRangeAndMediaContractShapesAreFixedAndControlOnly) {
  const auto range = encode_frame_range({.first = 5, .last = 9});
  EXPECT_EQ(hex(range), "00000000000000050000000000000009");
  const auto parsed = parse_frame_range(range);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->first, 5U);
  EXPECT_EQ(parsed->last, 9U);

  EXPECT_THROW((void) encode_frame_range({.first = 9, .last = 5}), std::invalid_argument);
  auto inverted = encode_frame_range({.first = 9, .last = 9});
  inverted[15] = 5;
  EXPECT_FALSE(parse_frame_range(inverted));
  EXPECT_FALSE(parse_frame_range(std::span {range}.first(8)));

  // The contract is negotiated on control only, and every body is exact.
  const auto config = encode_media_config(golden_media_config());
  const auto span = encode_frame_range({.first = 1, .last = 2});
  EXPECT_NO_THROW((void) encode_frame({.channel = channel_e::media, .message = message_e::media_config, .slot = 7, .generation = 42, .sequence = 1, .payload = config}));
  EXPECT_NO_THROW((void) encode_frame({.channel = channel_e::control, .message = message_e::media_config_ack, .slot = 7, .generation = 42, .sequence = 1}));
  EXPECT_NO_THROW((void) encode_frame({.channel = channel_e::control, .message = message_e::request_idr, .slot = 7, .generation = 42, .sequence = 1}));
  EXPECT_NO_THROW((void) encode_frame({.channel = channel_e::control, .message = message_e::media_control_ack, .slot = 7, .generation = 42, .sequence = 1}));
  EXPECT_NO_THROW((void) encode_frame({.channel = channel_e::control, .message = message_e::invalidate_ref_frames, .slot = 7, .generation = 42, .sequence = 1, .payload = span}));

  auto short_config = config;
  short_config.pop_back();
  EXPECT_THROW((void) encode_frame({.channel = channel_e::control, .message = message_e::media_config, .slot = 7, .generation = 42, .sequence = 1, .payload = config}), std::invalid_argument);
  EXPECT_THROW((void) encode_frame({.channel = channel_e::media, .message = message_e::media_config, .slot = 7, .generation = 42, .sequence = 1, .payload = short_config}), std::invalid_argument);
  EXPECT_THROW((void) encode_frame({.channel = channel_e::media, .message = message_e::media_control_ack, .slot = 7, .generation = 42, .sequence = 1}), std::invalid_argument);
  EXPECT_THROW((void) encode_frame({.channel = channel_e::control, .message = message_e::media_config_ack, .slot = 7, .generation = 42, .sequence = 1, .payload = {1}}), std::invalid_argument);
  EXPECT_THROW((void) encode_frame({.channel = channel_e::media, .message = message_e::request_idr, .slot = 7, .generation = 42, .sequence = 1}), std::invalid_argument);
  EXPECT_THROW((void) encode_frame({.channel = channel_e::control, .message = message_e::invalidate_ref_frames, .slot = 7, .generation = 42, .sequence = 1, .payload = std::vector<std::uint8_t>(span.begin(), span.begin() + 8)}), std::invalid_argument);
}
