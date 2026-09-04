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
