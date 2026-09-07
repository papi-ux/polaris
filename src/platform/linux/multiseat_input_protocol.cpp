/**
 * @file src/platform/linux/multiseat_input_protocol.cpp
 * @brief Canonical typed events for the host-brokered multiseat input path.
 */
#include "multiseat_input_protocol.h"

#ifdef __linux__

  #include <algorithm>
  #include <array>
  #include <bit>
  #include <limits>
  #include <stdexcept>
  #include <type_traits>

namespace multiseat::input {
  namespace {
    constexpr std::array<std::uint8_t, 4> input_magic {'P', 'I', 'N', '1'};
    constexpr std::array<std::uint8_t, 4> feedback_magic {'P', 'F', 'B', '1'};

    bool valid_button_state(button_state_e state) {
      return state == button_state_e::pressed || state == button_state_e::released;
    }

    bool valid_mouse_button(mouse_button_e button) {
      return button >= mouse_button_e::left && button <= mouse_button_e::extra;
    }

    bool valid_touch_action(touch_action_e action) {
      return action == touch_action_e::down ||
             action == touch_action_e::move ||
             action == touch_action_e::release;
    }

    bool valid_pen_proximity(pen_proximity_e proximity) {
      return proximity == pen_proximity_e::hover ||
             proximity == pen_proximity_e::contact;
    }

    bool valid_pen_tool(pen_tool_e tool) {
      return tool == pen_tool_e::pen || tool == pen_tool_e::eraser;
    }

    bool bounded_signed(std::int32_t value, std::int32_t maximum) {
      return value >= -maximum && value <= maximum;
    }

    void append_u16(std::vector<std::uint8_t> &bytes, std::uint16_t value) {
      bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
      bytes.push_back(static_cast<std::uint8_t>(value));
    }

    void append_i16(std::vector<std::uint8_t> &bytes, std::int16_t value) {
      append_u16(bytes, std::bit_cast<std::uint16_t>(value));
    }

    void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
      }
    }

    void append_i32(std::vector<std::uint8_t> &bytes, std::int32_t value) {
      append_u32(bytes, std::bit_cast<std::uint32_t>(value));
    }

    std::uint16_t read_u16(std::span<const std::uint8_t> bytes) {
      return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8U) |
        static_cast<std::uint16_t>(bytes[1])
      );
    }

    std::int16_t read_i16(std::span<const std::uint8_t> bytes) {
      return std::bit_cast<std::int16_t>(read_u16(bytes));
    }

    std::uint32_t read_u32(std::span<const std::uint8_t> bytes) {
      std::uint32_t value = 0;
      for (const auto byte : bytes.first<4>()) {
        value = (value << 8U) | byte;
      }
      return value;
    }

    std::int32_t read_i32(std::span<const std::uint8_t> bytes) {
      return std::bit_cast<std::int32_t>(read_u32(bytes));
    }

    void append_header(
      std::vector<std::uint8_t> &bytes,
      const std::array<std::uint8_t, 4> &magic,
      std::uint8_t kind,
      std::uint32_t slot
    ) {
      bytes.insert(bytes.end(), magic.begin(), magic.end());
      bytes.push_back(kind);
      bytes.push_back(0);
      append_u16(bytes, static_cast<std::uint16_t>(slot));
    }

    bool valid_header(
      std::span<const std::uint8_t> encoded,
      const std::array<std::uint8_t, 4> &magic
    ) {
      return encoded.size() >= encoded_event_header_size &&
             std::equal(magic.begin(), magic.end(), encoded.begin()) &&
             encoded[5] == 0;
    }

    bool reserved_zero(std::span<const std::uint8_t> bytes) {
      return std::all_of(bytes.begin(), bytes.end(), [](auto byte) {
        return byte == 0;
      });
    }

  }  // namespace

  input_event_kind_e kind_of(const input_event_t &event) {
    return std::visit(
      [](const auto &payload) {
        using value_t = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<value_t, keyboard_key_event_t>) {
          return input_event_kind_e::keyboard_key;
        } else if constexpr (std::is_same_v<value_t, mouse_relative_event_t>) {
          return input_event_kind_e::mouse_relative;
        } else if constexpr (std::is_same_v<value_t, mouse_absolute_event_t>) {
          return input_event_kind_e::mouse_absolute;
        } else if constexpr (std::is_same_v<value_t, mouse_button_event_t>) {
          return input_event_kind_e::mouse_button;
        } else if constexpr (std::is_same_v<value_t, mouse_scroll_event_t>) {
          return input_event_kind_e::mouse_scroll;
        } else if constexpr (std::is_same_v<value_t, touch_contact_event_t>) {
          return input_event_kind_e::touch_contact;
        } else if constexpr (std::is_same_v<value_t, pen_tool_event_t>) {
          return input_event_kind_e::pen_tool;
        } else {
          return input_event_kind_e::gamepad_state;
        }
      },
      event.payload
    );
  }

  bool supported_keyboard_code(std::uint16_t key_code) {
    return key_code == 0x08 || key_code == 0x09 || key_code == 0x0C ||
           key_code == 0x0D ||
           (key_code >= 0x10 && key_code <= 0x17) ||
           key_code == 0x19 || key_code == 0x1B ||
           (key_code >= 0x20 && key_code <= 0x2A) ||
           (key_code >= 0x2C && key_code <= 0x39) ||
           (key_code >= 0x41 && key_code <= 0x5D) ||
           key_code == 0x5F ||
           (key_code >= 0x60 && key_code <= 0x7B) ||
           (key_code >= 0x90 && key_code <= 0x91) ||
           (key_code >= 0xA0 && key_code <= 0xA5) ||
           (key_code >= 0xBA && key_code <= 0xC0) ||
           (key_code >= 0xDB && key_code <= 0xDE) ||
           key_code == 0xE2;
  }

  bool valid_input_event(const input_event_t &event) {
    if (event.slot >= maximum_gamepad_slots) {
      return false;
    }
    return std::visit(
      [&event](const auto &payload) {
        using value_t = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<value_t, keyboard_key_event_t>) {
          return event.slot == 0 && supported_keyboard_code(payload.key_code) &&
                 valid_button_state(payload.state);
        } else if constexpr (std::is_same_v<value_t, mouse_relative_event_t>) {
          return event.slot == 0 &&
                 bounded_signed(payload.delta_x, maximum_relative_pointer_delta) &&
                 bounded_signed(payload.delta_y, maximum_relative_pointer_delta) &&
                 (payload.delta_x != 0 || payload.delta_y != 0);
        } else if constexpr (std::is_same_v<value_t, mouse_absolute_event_t>) {
          return event.slot == 0 && payload.width > 0 && payload.height > 0 &&
                 payload.width <= maximum_pointer_extent &&
                 payload.height <= maximum_pointer_extent &&
                 payload.x <= payload.width && payload.y <= payload.height;
        } else if constexpr (std::is_same_v<value_t, mouse_button_event_t>) {
          return event.slot == 0 && valid_mouse_button(payload.button) &&
                 valid_button_state(payload.state);
        } else if constexpr (std::is_same_v<value_t, mouse_scroll_event_t>) {
          return event.slot == 0 &&
                 bounded_signed(payload.vertical, maximum_scroll_delta) &&
                 bounded_signed(payload.horizontal, maximum_scroll_delta) &&
                 (payload.vertical != 0 || payload.horizontal != 0);
        } else if constexpr (std::is_same_v<value_t, touch_contact_event_t>) {
          if (event.slot != 0 || payload.pointer_id > 0xFFFFU ||
              !valid_touch_action(payload.action) ||
              payload.orientation_degrees < -90 ||
              payload.orientation_degrees > 90) {
            return false;
          }
          return payload.action != touch_action_e::release ||
                 (payload.x == 0 && payload.y == 0 && payload.pressure == 0 &&
                  payload.orientation_degrees == 0);
        } else if constexpr (std::is_same_v<value_t, pen_tool_event_t>) {
          return event.slot == 0 && valid_pen_proximity(payload.proximity) &&
                 valid_pen_tool(payload.tool) && (payload.buttons & ~0x07U) == 0 &&
                 payload.tilt_x_degrees >= -90 && payload.tilt_x_degrees <= 90 &&
                 payload.tilt_y_degrees >= -90 && payload.tilt_y_degrees <= 90;
        } else {
          return (payload.buttons & ~supported_gamepad_buttons) == 0 &&
                 (payload.buttons & 0x0003U) != 0x0003U &&
                 (payload.buttons & 0x000CU) != 0x000CU;
        }
      },
      event.payload
    );
  }

  bool valid_feedback_event(const feedback_event_t &event) {
    return event.kind == feedback_kind_e::rumble &&
           event.gamepad_slot < maximum_gamepad_slots;
  }

  std::vector<std::uint8_t> encode_input_event(const input_event_t &event) {
    if (!valid_input_event(event)) {
      throw std::invalid_argument {"invalid multiseat input event"};
    }
    std::vector<std::uint8_t> encoded;
    encoded.reserve(maximum_encoded_input_event_bytes);
    append_header(
      encoded,
      input_magic,
      static_cast<std::uint8_t>(kind_of(event)),
      event.slot
    );
    std::visit(
      [&encoded](const auto &payload) {
        using value_t = std::remove_cvref_t<decltype(payload)>;
        if constexpr (std::is_same_v<value_t, keyboard_key_event_t>) {
          append_u16(encoded, payload.key_code);
          encoded.push_back(static_cast<std::uint8_t>(payload.state));
          encoded.push_back(0);
        } else if constexpr (std::is_same_v<value_t, mouse_relative_event_t>) {
          append_i32(encoded, payload.delta_x);
          append_i32(encoded, payload.delta_y);
        } else if constexpr (std::is_same_v<value_t, mouse_absolute_event_t>) {
          append_u32(encoded, payload.x);
          append_u32(encoded, payload.y);
          append_u32(encoded, payload.width);
          append_u32(encoded, payload.height);
        } else if constexpr (std::is_same_v<value_t, mouse_button_event_t>) {
          encoded.push_back(static_cast<std::uint8_t>(payload.button));
          encoded.push_back(static_cast<std::uint8_t>(payload.state));
          append_u16(encoded, 0);
        } else if constexpr (std::is_same_v<value_t, mouse_scroll_event_t>) {
          append_i32(encoded, payload.vertical);
          append_i32(encoded, payload.horizontal);
        } else if constexpr (std::is_same_v<value_t, touch_contact_event_t>) {
          append_u32(encoded, payload.pointer_id);
          encoded.push_back(static_cast<std::uint8_t>(payload.action));
          encoded.push_back(0);
          append_i16(encoded, payload.orientation_degrees);
          append_u16(encoded, payload.x);
          append_u16(encoded, payload.y);
          append_u16(encoded, payload.pressure);
          append_u16(encoded, 0);
        } else if constexpr (std::is_same_v<value_t, pen_tool_event_t>) {
          encoded.push_back(static_cast<std::uint8_t>(payload.proximity));
          encoded.push_back(static_cast<std::uint8_t>(payload.tool));
          encoded.push_back(payload.buttons);
          encoded.push_back(0);
          append_u16(encoded, payload.x);
          append_u16(encoded, payload.y);
          append_u16(encoded, payload.pressure_or_distance);
          append_i16(encoded, payload.tilt_x_degrees);
          append_i16(encoded, payload.tilt_y_degrees);
          append_u16(encoded, 0);
        } else {
          append_u32(encoded, payload.buttons);
          encoded.push_back(payload.left_trigger);
          encoded.push_back(payload.right_trigger);
          append_i16(encoded, payload.left_stick_x);
          append_i16(encoded, payload.left_stick_y);
          append_i16(encoded, payload.right_stick_x);
          append_i16(encoded, payload.right_stick_y);
          append_u16(encoded, 0);
        }
      },
      event.payload
    );
    return encoded;
  }

  std::optional<input_event_t> decode_input_event(
    std::span<const std::uint8_t> encoded
  ) {
    if (!valid_header(encoded, input_magic) ||
        encoded.size() > maximum_encoded_input_event_bytes) {
      return std::nullopt;
    }
    const auto kind = static_cast<input_event_kind_e>(encoded[4]);
    const auto slot = static_cast<std::uint32_t>(read_u16(encoded.subspan(6, 2)));
    input_event_t event {.slot = slot};
    switch (kind) {
      case input_event_kind_e::keyboard_key:
        if (encoded.size() != 12 || encoded[11] != 0) {
          return std::nullopt;
        }
        event.payload = keyboard_key_event_t {
          .key_code = read_u16(encoded.subspan(8, 2)),
          .state = static_cast<button_state_e>(encoded[10]),
        };
        break;
      case input_event_kind_e::mouse_relative:
        if (encoded.size() != 16) {
          return std::nullopt;
        }
        event.payload = mouse_relative_event_t {
          .delta_x = read_i32(encoded.subspan(8, 4)),
          .delta_y = read_i32(encoded.subspan(12, 4)),
        };
        break;
      case input_event_kind_e::mouse_absolute:
        if (encoded.size() != 24) {
          return std::nullopt;
        }
        event.payload = mouse_absolute_event_t {
          .x = read_u32(encoded.subspan(8, 4)),
          .y = read_u32(encoded.subspan(12, 4)),
          .width = read_u32(encoded.subspan(16, 4)),
          .height = read_u32(encoded.subspan(20, 4)),
        };
        break;
      case input_event_kind_e::mouse_button:
        if (encoded.size() != 12 || !reserved_zero(encoded.subspan(10, 2))) {
          return std::nullopt;
        }
        event.payload = mouse_button_event_t {
          .button = static_cast<mouse_button_e>(encoded[8]),
          .state = static_cast<button_state_e>(encoded[9]),
        };
        break;
      case input_event_kind_e::mouse_scroll:
        if (encoded.size() != 16) {
          return std::nullopt;
        }
        event.payload = mouse_scroll_event_t {
          .vertical = read_i32(encoded.subspan(8, 4)),
          .horizontal = read_i32(encoded.subspan(12, 4)),
        };
        break;
      case input_event_kind_e::touch_contact:
        if (encoded.size() != 24 || encoded[13] != 0 ||
            !reserved_zero(encoded.subspan(22, 2))) {
          return std::nullopt;
        }
        event.payload = touch_contact_event_t {
          .pointer_id = read_u32(encoded.subspan(8, 4)),
          .action = static_cast<touch_action_e>(encoded[12]),
          .orientation_degrees = read_i16(encoded.subspan(14, 2)),
          .x = read_u16(encoded.subspan(16, 2)),
          .y = read_u16(encoded.subspan(18, 2)),
          .pressure = read_u16(encoded.subspan(20, 2)),
        };
        break;
      case input_event_kind_e::pen_tool:
        if (encoded.size() != 24 || encoded[11] != 0 ||
            !reserved_zero(encoded.subspan(22, 2))) {
          return std::nullopt;
        }
        event.payload = pen_tool_event_t {
          .proximity = static_cast<pen_proximity_e>(encoded[8]),
          .tool = static_cast<pen_tool_e>(encoded[9]),
          .buttons = encoded[10],
          .x = read_u16(encoded.subspan(12, 2)),
          .y = read_u16(encoded.subspan(14, 2)),
          .pressure_or_distance = read_u16(encoded.subspan(16, 2)),
          .tilt_x_degrees = read_i16(encoded.subspan(18, 2)),
          .tilt_y_degrees = read_i16(encoded.subspan(20, 2)),
        };
        break;
      case input_event_kind_e::gamepad_state:
        if (encoded.size() != 24 || !reserved_zero(encoded.subspan(22, 2))) {
          return std::nullopt;
        }
        event.payload = gamepad_state_event_t {
          .buttons = read_u32(encoded.subspan(8, 4)),
          .left_trigger = encoded[12],
          .right_trigger = encoded[13],
          .left_stick_x = read_i16(encoded.subspan(14, 2)),
          .left_stick_y = read_i16(encoded.subspan(16, 2)),
          .right_stick_x = read_i16(encoded.subspan(18, 2)),
          .right_stick_y = read_i16(encoded.subspan(20, 2)),
        };
        break;
      default:
        return std::nullopt;
    }
    return valid_input_event(event) ? std::optional<input_event_t> {event} :
                                      std::nullopt;
  }

  std::vector<std::uint8_t> encode_feedback_event(
    const feedback_event_t &event
  ) {
    if (!valid_feedback_event(event)) {
      throw std::invalid_argument {"invalid multiseat feedback event"};
    }
    std::vector<std::uint8_t> encoded;
    encoded.reserve(encoded_feedback_event_bytes);
    append_header(
      encoded,
      feedback_magic,
      static_cast<std::uint8_t>(event.kind),
      event.gamepad_slot
    );
    append_u16(encoded, event.low_frequency);
    append_u16(encoded, event.high_frequency);
    return encoded;
  }

  std::optional<feedback_event_t> decode_feedback_event(
    std::span<const std::uint8_t> encoded
  ) {
    if (encoded.size() != encoded_feedback_event_bytes ||
        !valid_header(encoded, feedback_magic)) {
      return std::nullopt;
    }
    feedback_event_t event {
      .kind = static_cast<feedback_kind_e>(encoded[4]),
      .gamepad_slot = read_u16(encoded.subspan(6, 2)),
      .low_frequency = read_u16(encoded.subspan(8, 2)),
      .high_frequency = read_u16(encoded.subspan(10, 2)),
    };
    return valid_feedback_event(event) ? std::optional<feedback_event_t> {event} :
                                         std::nullopt;
  }

}  // namespace multiseat::input

#endif
