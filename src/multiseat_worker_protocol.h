/**
 * @file src/multiseat_worker_protocol.h
 * @brief Bounded, generation-fenced protocol for local multiseat workers.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace multiseat::worker_ipc {

  inline constexpr std::size_t header_size = 32;
  inline constexpr std::size_t capability_size = 32;
  inline constexpr std::size_t proof_size = 32;
  inline constexpr std::size_t challenge_size = 32;
  inline constexpr std::size_t max_control_payload = 64 * 1024;
  inline constexpr std::size_t max_media_payload = 16 * 1024 * 1024;

  enum class channel_e : std::uint8_t {
    control = 1,
    media = 2,
  };

  enum class message_e : std::uint8_t {
    challenge = 1,
    authenticate = 2,
    authenticated = 3,
    heartbeat = 4,
    heartbeat_ack = 5,

    ready = 16,
    shutdown = 17,
    shutdown_ack = 18,
    input = 19,
    feedback = 20,
    error = 21,
    attach = 22,
    attached = 23,
    input_ack = 24,

    video = 32,
    audio = 33,
    end_of_stream = 34,
    discontinuity = 35,
  };

  enum class parse_status_e {
    complete,
    incomplete,
    rejected,
  };

  enum class proof_role_e : std::uint8_t {
    controller = 1,
    worker = 2,
  };

  using capability_t = std::array<std::uint8_t, capability_size>;
  using challenge_t = std::array<std::uint8_t, challenge_size>;
  using proof_t = std::array<std::uint8_t, proof_size>;

  /** Identity mixed into every authentication proof. */
  struct endpoint_identity_t {
    std::string controller_epoch;
    std::string logical_gpu_id;
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    std::string worker_name;

    bool operator==(const endpoint_identity_t &) const = default;
  };

  struct frame_t {
    channel_e channel = channel_e::control;
    message_e message = message_e::heartbeat;
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> payload;

    bool operator==(const frame_t &) const = default;
  };

  /**
   * Result from parsing one frame at the beginning of a byte stream.
   *
   * A rejected result never trusts the advertised length. An incomplete
   * result reports the exact bounded byte count needed for one frame.
   */
  struct parse_result_t {
    parse_status_e status = parse_status_e::rejected;
    std::optional<frame_t> frame;
    std::size_t consumed = 0;
    std::size_t required = 0;
  };

  /** Per-direction replay/gap guard; a new connection starts at sequence 1. */
  class sequence_guard_t {
  public:
    [[nodiscard]] bool accept(std::uint64_t sequence);
    [[nodiscard]] std::uint64_t next() const;

  private:
    std::uint64_t next_ = 1;
    bool exhausted_ = false;
  };

  [[nodiscard]] bool valid_identity(const endpoint_identity_t &identity);
  [[nodiscard]] std::optional<capability_t> parse_capability_hex(
    std::string_view encoded
  );
  [[nodiscard]] std::string capability_hex(const capability_t &capability);

  /** HMAC-SHA256 proof bound to role, channel, exact seat, and challenge. */
  [[nodiscard]] std::optional<proof_t> authentication_proof(
    const capability_t &capability,
    proof_role_e role,
    channel_e channel,
    const endpoint_identity_t &identity,
    const challenge_t &challenge
  );
  [[nodiscard]] bool verify_authentication_proof(
    const capability_t &capability,
    proof_role_e role,
    channel_e channel,
    const endpoint_identity_t &identity,
    const challenge_t &challenge,
    std::span<const std::uint8_t> presented
  );

  /** Encode one valid protocol frame, throwing on an invalid combination. */
  [[nodiscard]] std::vector<std::uint8_t> encode_frame(const frame_t &frame);

  /**
   * Parse one frame and fence it to the expected channel and exact seat
   * generation. Extra bytes are left to the caller via `consumed`.
   */
  [[nodiscard]] parse_result_t parse_frame(
    std::span<const std::uint8_t> bytes,
    channel_e expected_channel,
    std::uint32_t expected_slot,
    std::uint64_t expected_generation
  );

}  // namespace multiseat::worker_ipc
