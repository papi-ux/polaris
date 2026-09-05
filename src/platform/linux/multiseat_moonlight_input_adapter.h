/**
 * @file src/platform/linux/multiseat_moonlight_input_adapter.h
 * @brief Offline Moonlight input conversion and bounded controller feedback.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_input_authority.h"
  #include "src/platform/common.h"

  #include <array>
  #include <cstddef>
  #include <cstdint>
  #include <mutex>
  #include <optional>
  #include <span>

namespace multiseat::input {

  inline constexpr std::size_t maximum_moonlight_input_packet_bytes = 40;
  inline constexpr std::size_t maximum_pending_controller_feedback =
    maximum_gamepad_slots;

  enum class moonlight_input_class_e {
    keyboard,
    mouse,
    touch,
    pen,
    controller,
  };

  enum class moonlight_packet_status_e {
    converted,
    ignored,
    unsupported,
    invalid,
  };

  struct decoded_moonlight_packet_t {
    moonlight_input_class_e input_class = moonlight_input_class_e::keyboard;
    input_event_t event;

    bool operator==(const decoded_moonlight_packet_t &) const = default;
  };

  struct moonlight_packet_result_t {
    moonlight_packet_status_e status = moonlight_packet_status_e::invalid;
    std::optional<decoded_moonlight_packet_t> decoded;
  };

  /**
   * Parse one complete, decrypted Moonlight input packet without casting the
   * caller's storage to a packed native structure.
   */
  [[nodiscard]] moonlight_packet_result_t decode_moonlight_input_packet(
    std::span<const std::uint8_t> packet
  );

  struct moonlight_input_permissions_t {
    bool keyboard = false;
    bool mouse = false;
    bool touch = false;
    bool pen = false;
    bool controller = false;

    [[nodiscard]] bool permits(moonlight_input_class_e input_class) const;

    bool operator==(const moonlight_input_permissions_t &) const = default;
  };

  enum class moonlight_route_status_e {
    applied,
    ignored_packet,
    unsupported_packet,
    invalid_packet,
    permission_denied,
    sequence_exhausted,
    authority_rejected,
  };

  struct moonlight_route_result_t {
    moonlight_route_status_e status = moonlight_route_status_e::invalid_packet;
    std::optional<status_e> authority_status;
    std::uint64_t sequence = 0;
  };

  /**
   * Binds input packets to one immutable seat generation and assigns the
   * canonical authority sequence only after parsing and permission checks.
   */
  class moonlight_input_adapter_t {
  public:
    moonlight_input_adapter_t(
      authority_t &authority,
      seat_handle_t handle,
      moonlight_input_permissions_t permissions
    );

    moonlight_input_adapter_t(const moonlight_input_adapter_t &) = delete;
    moonlight_input_adapter_t &operator=(const moonlight_input_adapter_t &) = delete;
    moonlight_input_adapter_t(moonlight_input_adapter_t &&) = delete;
    moonlight_input_adapter_t &operator=(moonlight_input_adapter_t &&) = delete;

    [[nodiscard]] moonlight_route_result_t route(
      std::span<const std::uint8_t> packet
    );
    [[nodiscard]] std::uint64_t next_sequence() const;

  private:
    authority_t &authority_;
    const seat_handle_t handle_;
    const moonlight_input_permissions_t permissions_;
    mutable std::mutex mutex_;
    std::uint64_t next_sequence_ = 1;
    bool sequence_exhausted_ = false;
  };

  struct moonlight_feedback_t {
    std::uint64_t source_sequence = 0;
    platf::gamepad_feedback_msg_t message =
      platf::gamepad_feedback_msg_t::make_rumble(0, 0, 0);
  };

  /** Convert the currently supported typed feedback to the existing sender shape. */
  [[nodiscard]] std::optional<moonlight_feedback_t> convert_controller_feedback(
    const controller_feedback_t &feedback
  );

  enum class controller_feedback_queue_result_e {
    enqueued,
    coalesced,
    closed,
    stale_generation,
    invalid_event,
    invalid_sequence,
  };

  /**
   * One bounded queue per immutable seat generation. At most one latest
   * rumble state is retained for each of the sixteen controller slots.
   */
  class moonlight_controller_feedback_queue_t {
  public:
    explicit moonlight_controller_feedback_queue_t(seat_handle_t handle);

    moonlight_controller_feedback_queue_t(
      const moonlight_controller_feedback_queue_t &
    ) = delete;
    moonlight_controller_feedback_queue_t &operator=(
      const moonlight_controller_feedback_queue_t &
    ) = delete;
    moonlight_controller_feedback_queue_t(
      moonlight_controller_feedback_queue_t &&
    ) = delete;
    moonlight_controller_feedback_queue_t &operator=(
      moonlight_controller_feedback_queue_t &&
    ) = delete;

    [[nodiscard]] controller_feedback_queue_result_e push(
      const controller_feedback_t &feedback
    );
    [[nodiscard]] std::optional<moonlight_feedback_t> pop();
    void close();

    [[nodiscard]] std::size_t pending() const;
    [[nodiscard]] std::uint64_t last_sequence() const;
    [[nodiscard]] bool closed() const;

  private:
    const seat_handle_t handle_;
    mutable std::mutex mutex_;
    std::array<
      std::optional<moonlight_feedback_t>,
      maximum_pending_controller_feedback>
      pending_;
    std::size_t pending_count_ = 0;
    std::uint64_t last_sequence_ = 0;
    bool sequence_exhausted_ = false;
    bool closed_ = false;
  };

}  // namespace multiseat::input

#endif
