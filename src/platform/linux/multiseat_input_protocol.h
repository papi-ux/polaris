/**
 * @file src/platform/linux/multiseat_input_protocol.h
 * @brief Canonical typed events for the host-brokered multiseat input path.
 */
#pragma once

#ifdef __linux__

  #include "src/multiseat_runtime.h"

  #include <cstddef>
  #include <cstdint>
  #include <optional>
  #include <span>
  #include <variant>
  #include <vector>

namespace multiseat::input {

  inline constexpr std::size_t encoded_event_header_size = 8;
  inline constexpr std::size_t maximum_encoded_input_event_bytes = 24;
  inline constexpr std::size_t encoded_feedback_event_bytes = 12;
  inline constexpr std::uint32_t maximum_gamepad_slots = 16;
  inline constexpr std::uint32_t maximum_touch_contacts = 16;
  inline constexpr std::uint32_t maximum_pointer_extent = 32768;
  inline constexpr std::int32_t maximum_relative_pointer_delta = 32767;
  inline constexpr std::int32_t maximum_scroll_delta = 32767;
  inline constexpr std::uint32_t supported_gamepad_buttons = 0x0000F7FFU;

  enum class button_state_e : std::uint8_t {
    pressed = 1,
    released = 2,
  };

  enum class mouse_button_e : std::uint8_t {
    left = 1,
    middle = 2,
    right = 3,
    side = 4,
    extra = 5,
  };

  enum class touch_action_e : std::uint8_t {
    down = 1,
    move = 2,
    release = 3,
  };

  enum class pen_proximity_e : std::uint8_t {
    hover = 1,
    contact = 2,
  };

  enum class pen_tool_e : std::uint8_t {
    pen = 1,
    eraser = 2,
  };

  enum class input_event_kind_e : std::uint8_t {
    keyboard_key = 1,
    mouse_relative = 2,
    mouse_absolute = 3,
    mouse_button = 4,
    mouse_scroll = 5,
    touch_contact = 6,
    pen_tool = 7,
    gamepad_state = 8,
  };

  struct keyboard_key_event_t {
    std::uint16_t key_code = 0;
    button_state_e state = button_state_e::pressed;

    bool operator==(const keyboard_key_event_t &) const = default;
  };

  struct mouse_relative_event_t {
    std::int32_t delta_x = 0;
    std::int32_t delta_y = 0;

    bool operator==(const mouse_relative_event_t &) const = default;
  };

  struct mouse_absolute_event_t {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const mouse_absolute_event_t &) const = default;
  };

  struct mouse_button_event_t {
    mouse_button_e button = mouse_button_e::left;
    button_state_e state = button_state_e::pressed;

    bool operator==(const mouse_button_event_t &) const = default;
  };

  struct mouse_scroll_event_t {
    std::int32_t vertical = 0;
    std::int32_t horizontal = 0;

    bool operator==(const mouse_scroll_event_t &) const = default;
  };

  /** Coordinates and pressure are canonical unsigned normalized values. */
  struct touch_contact_event_t {
    std::uint32_t pointer_id = 0;
    touch_action_e action = touch_action_e::down;
    std::int16_t orientation_degrees = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t pressure = 0;

    bool operator==(const touch_contact_event_t &) const = default;
  };

  /**
   * `pressure_or_distance` is pressure for contact and distance for hover.
   * Coordinates and that scalar are canonical unsigned normalized values.
   */
  struct pen_tool_event_t {
    pen_proximity_e proximity = pen_proximity_e::hover;
    pen_tool_e tool = pen_tool_e::pen;
    std::uint8_t buttons = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t pressure_or_distance = 0;
    std::int16_t tilt_x_degrees = 0;
    std::int16_t tilt_y_degrees = 0;

    bool operator==(const pen_tool_event_t &) const = default;
  };

  struct gamepad_state_event_t {
    std::uint32_t buttons = 0;
    std::uint8_t left_trigger = 0;
    std::uint8_t right_trigger = 0;
    std::int16_t left_stick_x = 0;
    std::int16_t left_stick_y = 0;
    std::int16_t right_stick_x = 0;
    std::int16_t right_stick_y = 0;

    bool operator==(const gamepad_state_event_t &) const = default;
  };

  using input_event_payload_t = std::variant<
    keyboard_key_event_t,
    mouse_relative_event_t,
    mouse_absolute_event_t,
    mouse_button_event_t,
    mouse_scroll_event_t,
    touch_contact_event_t,
    pen_tool_event_t,
    gamepad_state_event_t>;

  /** Slot is zero for every event except a gamepad state event. */
  struct input_event_t {
    std::uint32_t slot = 0;
    input_event_payload_t payload;

    bool operator==(const input_event_t &) const = default;
  };

  enum class feedback_kind_e : std::uint8_t {
    rumble = 1,
  };

  struct feedback_event_t {
    feedback_kind_e kind = feedback_kind_e::rumble;
    std::uint32_t gamepad_slot = 0;
    std::uint16_t low_frequency = 0;
    std::uint16_t high_frequency = 0;

    bool operator==(const feedback_event_t &) const = default;
  };

  /** Full controller-facing envelope assigned by one generation's backend. */
  struct controller_feedback_t {
    seat_handle_t handle;
    std::uint64_t sequence = 0;
    feedback_event_t event;

    bool operator==(const controller_feedback_t &) const = default;
  };

  [[nodiscard]] input_event_kind_e kind_of(const input_event_t &event);
  [[nodiscard]] bool supported_keyboard_code(std::uint16_t key_code);
  [[nodiscard]] bool valid_input_event(const input_event_t &event);
  [[nodiscard]] bool valid_feedback_event(const feedback_event_t &event);

  /** Encode one canonical event, throwing when the typed value is invalid. */
  [[nodiscard]] std::vector<std::uint8_t> encode_input_event(
    const input_event_t &event
  );
  /** Decode exactly one canonical event. Trailing bytes are rejected. */
  [[nodiscard]] std::optional<input_event_t> decode_input_event(
    std::span<const std::uint8_t> encoded
  );

  [[nodiscard]] std::vector<std::uint8_t> encode_feedback_event(
    const feedback_event_t &event
  );
  [[nodiscard]] std::optional<feedback_event_t> decode_feedback_event(
    std::span<const std::uint8_t> encoded
  );

}  // namespace multiseat::input

#endif
