/**
 * @file src/platform/linux/multiseat_moonlight_input_adapter.cpp
 * @brief Offline Moonlight input conversion and bounded controller feedback.
 */
#include "multiseat_moonlight_input_adapter.h"

#ifdef __linux__

extern "C" {
  #include <moonlight-common-c/src/Input.h>
  #include <moonlight-common-c/src/Limelight.h>
}

  #include <algorithm>
  #include <bit>
  #include <cmath>
  #include <limits>
  #include <numbers>
  #include <stdexcept>
  #include <utility>

namespace multiseat::input {
  namespace {
    static_assert(sizeof(NV_INPUT_HEADER) == 8);
    static_assert(sizeof(NV_REL_MOUSE_MOVE_PACKET) == 12);
    static_assert(sizeof(NV_ABS_MOUSE_MOVE_PACKET) == 18);
    static_assert(sizeof(NV_MOUSE_BUTTON_PACKET) == 9);
    static_assert(sizeof(NV_KEYBOARD_PACKET) == 14);
    static_assert(sizeof(NV_SCROLL_PACKET) == 14);
    static_assert(sizeof(SS_HSCROLL_PACKET) == 10);
    static_assert(sizeof(NV_MULTI_CONTROLLER_PACKET) == 34);
    static_assert(sizeof(SS_TOUCH_PACKET) == 36);
    static_assert(sizeof(SS_PEN_PACKET) == 36);
    static_assert(sizeof(NV_UNICODE_PACKET) == maximum_moonlight_input_packet_bytes);

    constexpr std::uint8_t known_modifier_mask =
      MODIFIER_SHIFT | MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_META;

    std::uint16_t read_u16_be(std::span<const std::uint8_t> bytes) {
      return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8U) |
        static_cast<std::uint16_t>(bytes[1])
      );
    }

    std::uint16_t read_u16_le(std::span<const std::uint8_t> bytes) {
      return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0]) |
        (static_cast<std::uint16_t>(bytes[1]) << 8U)
      );
    }

    std::int16_t read_i16_be(std::span<const std::uint8_t> bytes) {
      return std::bit_cast<std::int16_t>(read_u16_be(bytes));
    }

    std::int16_t read_i16_le(std::span<const std::uint8_t> bytes) {
      return std::bit_cast<std::int16_t>(read_u16_le(bytes));
    }

    std::uint32_t read_u32_be(std::span<const std::uint8_t> bytes) {
      std::uint32_t value = 0;
      for (const auto byte : bytes.first<4>()) {
        value = (value << 8U) | byte;
      }
      return value;
    }

    std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes) {
      std::uint32_t value = 0;
      for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
      }
      return value;
    }

    bool all_zero(std::span<const std::uint8_t> bytes) {
      return std::all_of(bytes.begin(), bytes.end(), [](auto byte) {
        return byte == 0;
      });
    }

    std::optional<float> read_normalized_float(
      std::span<const std::uint8_t> bytes
    ) {
      const auto value = std::bit_cast<float>(read_u32_le(bytes));
      if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
        return std::nullopt;
      }
      return value;
    }

    std::optional<std::uint16_t> canonical_normalized(
      std::span<const std::uint8_t> bytes
    ) {
      const auto value = read_normalized_float(bytes);
      if (!value) {
        return std::nullopt;
      }
      return static_cast<std::uint16_t>(std::lround(
        static_cast<double>(*value) *
        static_cast<double>(std::numeric_limits<std::uint16_t>::max())
      ));
    }

    std::int16_t canonical_touch_orientation(std::uint16_t rotation) {
      if (rotation == LI_ROT_UNKNOWN) {
        return 0;
      }
      int adjusted = rotation % 360;
      if (adjusted > 90 && adjusted < 270) {
        adjusted = 180 - adjusted;
      }
      if (adjusted > 90) {
        adjusted -= 360;
      } else if (adjusted < -90) {
        adjusted += 360;
      }
      return static_cast<std::int16_t>(adjusted);
    }

    std::optional<std::pair<std::int16_t, std::int16_t>> canonical_pen_tilt(
      std::uint16_t rotation,
      std::uint8_t tilt
    ) {
      if (rotation == LI_ROT_UNKNOWN || tilt == LI_TILT_UNKNOWN) {
        return std::pair<std::int16_t, std::int16_t> {0, 0};
      }
      if (tilt > 90) {
        return std::nullopt;
      }

      const auto rotation_radians =
        static_cast<double>(rotation % 360) * std::numbers::pi / 180.0;
      const auto tilt_radians =
        static_cast<double>(tilt) * std::numbers::pi / 180.0;
      const auto radius = std::sin(tilt_radians);
      const auto z = std::cos(tilt_radians);
      const auto tilt_x = std::atan2(
                            std::sin(-rotation_radians) * radius,
                            z
                          ) *
                          180.0 / std::numbers::pi;
      const auto tilt_y = std::atan2(
                            std::cos(-rotation_radians) * radius,
                            z
                          ) *
                          180.0 / std::numbers::pi;
      return std::pair {
        static_cast<std::int16_t>(std::clamp(std::lround(tilt_x), -90L, 90L)),
        static_cast<std::int16_t>(std::clamp(std::lround(tilt_y), -90L, 90L)),
      };
    }

    moonlight_packet_result_t converted(
      moonlight_input_class_e input_class,
      input_event_t event
    ) {
      if (!valid_input_event(event)) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      return {
        .status = moonlight_packet_status_e::converted,
        .decoded = decoded_moonlight_packet_t {
          .input_class = input_class,
          .event = std::move(event),
        },
      };
    }

    moonlight_packet_result_t decode_relative_mouse(
      std::span<const std::uint8_t> packet
    ) {
      if (packet.size() != sizeof(NV_REL_MOUSE_MOVE_PACKET)) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      const auto delta_x = read_i16_be(packet.subspan(8, 2));
      const auto delta_y = read_i16_be(packet.subspan(10, 2));
      if (delta_x == 0 && delta_y == 0) {
        return {.status = moonlight_packet_status_e::ignored};
      }
      return converted(
        moonlight_input_class_e::mouse,
        input_event_t {
          .payload = mouse_relative_event_t {
            .delta_x = delta_x,
            .delta_y = delta_y,
          },
        }
      );
    }

    moonlight_packet_result_t decode_absolute_mouse(
      std::span<const std::uint8_t> packet
    ) {
      if (packet.size() != sizeof(NV_ABS_MOUSE_MOVE_PACKET) ||
          !all_zero(packet.subspan(12, 2))) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      const auto x = read_i16_be(packet.subspan(8, 2));
      const auto y = read_i16_be(packet.subspan(10, 2));
      const auto width = read_i16_be(packet.subspan(14, 2));
      const auto height = read_i16_be(packet.subspan(16, 2));
      if (x < 0 || y < 0 || width <= 0 || height <= 0) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      return converted(
        moonlight_input_class_e::mouse,
        input_event_t {
          .payload = mouse_absolute_event_t {
            .x = static_cast<std::uint32_t>(x),
            .y = static_cast<std::uint32_t>(y),
            .width = static_cast<std::uint32_t>(width),
            .height = static_cast<std::uint32_t>(height),
          },
        }
      );
    }

    moonlight_packet_result_t decode_mouse_button(
      std::span<const std::uint8_t> packet,
      button_state_e state
    ) {
      if (packet.size() != sizeof(NV_MOUSE_BUTTON_PACKET)) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      mouse_button_e button;
      switch (packet[8]) {
        case BUTTON_LEFT:
          button = mouse_button_e::left;
          break;
        case BUTTON_MIDDLE:
          button = mouse_button_e::middle;
          break;
        case BUTTON_RIGHT:
          button = mouse_button_e::right;
          break;
        case BUTTON_X1:
          button = mouse_button_e::side;
          break;
        case BUTTON_X2:
          button = mouse_button_e::extra;
          break;
        default:
          return {.status = moonlight_packet_status_e::invalid};
      }
      return converted(
        moonlight_input_class_e::mouse,
        input_event_t {
          .payload = mouse_button_event_t {
            .button = button,
            .state = state,
          },
        }
      );
    }

    moonlight_packet_result_t decode_vertical_scroll(
      std::span<const std::uint8_t> packet
    ) {
      if (packet.size() != sizeof(NV_SCROLL_PACKET) ||
          read_u16_be(packet.subspan(8, 2)) !=
            read_u16_be(packet.subspan(10, 2)) ||
          !all_zero(packet.subspan(12, 2))) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      const auto amount = read_i16_be(packet.subspan(8, 2));
      if (amount == 0) {
        return {.status = moonlight_packet_status_e::ignored};
      }
      return converted(
        moonlight_input_class_e::mouse,
        input_event_t {
          .payload = mouse_scroll_event_t {.vertical = amount},
        }
      );
    }

    moonlight_packet_result_t decode_horizontal_scroll(
      std::span<const std::uint8_t> packet
    ) {
      if (packet.size() != sizeof(SS_HSCROLL_PACKET)) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      const auto amount = read_i16_be(packet.subspan(8, 2));
      if (amount == 0) {
        return {.status = moonlight_packet_status_e::ignored};
      }
      return converted(
        moonlight_input_class_e::mouse,
        input_event_t {
          .payload = mouse_scroll_event_t {.horizontal = amount},
        }
      );
    }

    moonlight_packet_result_t decode_keyboard(
      std::span<const std::uint8_t> packet,
      button_state_e state
    ) {
      if (packet.size() != sizeof(NV_KEYBOARD_PACKET) ||
          !all_zero(packet.subspan(12, 2))) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      if (packet[8] != 0) {
        return {.status = moonlight_packet_status_e::unsupported};
      }
      if ((packet[11] & ~known_modifier_mask) != 0) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      return converted(
        moonlight_input_class_e::keyboard,
        input_event_t {
          .payload = keyboard_key_event_t {
            .key_code = read_u16_le(packet.subspan(9, 2)),
            .state = state,
          },
        }
      );
    }

    moonlight_packet_result_t decode_touch(
      std::span<const std::uint8_t> packet
    ) {
      if (packet.size() != sizeof(SS_TOUCH_PACKET) || packet[9] != 0) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      const auto pointer_id = read_u32_le(packet.subspan(12, 4));
      if (pointer_id > 0xFFFFU) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      if (packet[8] == LI_TOUCH_EVENT_UP ||
          packet[8] == LI_TOUCH_EVENT_CANCEL) {
        return converted(
          moonlight_input_class_e::touch,
          input_event_t {
            .payload = touch_contact_event_t {
              .pointer_id = pointer_id,
              .action = touch_action_e::release,
            },
          }
        );
      }
      if (packet[8] != LI_TOUCH_EVENT_DOWN &&
          packet[8] != LI_TOUCH_EVENT_MOVE) {
        return {.status = moonlight_packet_status_e::unsupported};
      }

      const auto x = canonical_normalized(packet.subspan(16, 4));
      const auto y = canonical_normalized(packet.subspan(20, 4));
      const auto pressure = canonical_normalized(packet.subspan(24, 4));
      const auto major = read_normalized_float(packet.subspan(28, 4));
      const auto minor = read_normalized_float(packet.subspan(32, 4));
      if (!x || !y || !pressure || !major || !minor) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      return converted(
        moonlight_input_class_e::touch,
        input_event_t {
          .payload = touch_contact_event_t {
            .pointer_id = pointer_id,
            .action = packet[8] == LI_TOUCH_EVENT_DOWN ?
                        touch_action_e::down :
                        touch_action_e::move,
            .orientation_degrees = canonical_touch_orientation(
              read_u16_le(packet.subspan(10, 2))
            ),
            .x = *x,
            .y = *y,
            .pressure = *pressure,
          },
        }
      );
    }

    moonlight_packet_result_t decode_pen(
      std::span<const std::uint8_t> packet
    ) {
      if (packet.size() != sizeof(SS_PEN_PACKET) || packet[11] != 0 ||
          packet[27] != 0 || (packet[10] & ~0x07U) != 0) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      if (packet[8] != LI_TOUCH_EVENT_HOVER &&
          packet[8] != LI_TOUCH_EVENT_DOWN &&
          packet[8] != LI_TOUCH_EVENT_UP &&
          packet[8] != LI_TOUCH_EVENT_MOVE) {
        return {.status = moonlight_packet_status_e::unsupported};
      }
      pen_tool_e tool;
      if (packet[9] == LI_TOOL_TYPE_PEN) {
        tool = pen_tool_e::pen;
      } else if (packet[9] == LI_TOOL_TYPE_ERASER) {
        tool = pen_tool_e::eraser;
      } else {
        return {.status = moonlight_packet_status_e::unsupported};
      }

      const auto x = canonical_normalized(packet.subspan(12, 4));
      const auto y = canonical_normalized(packet.subspan(16, 4));
      const auto pressure_or_distance = canonical_normalized(
        packet.subspan(20, 4)
      );
      const auto major = read_normalized_float(packet.subspan(28, 4));
      const auto minor = read_normalized_float(packet.subspan(32, 4));
      const auto tilt = canonical_pen_tilt(
        read_u16_le(packet.subspan(24, 2)),
        packet[26]
      );
      if (!x || !y || !pressure_or_distance || !major || !minor || !tilt) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      const bool contact = packet[8] == LI_TOUCH_EVENT_DOWN ||
                           packet[8] == LI_TOUCH_EVENT_MOVE;
      return converted(
        moonlight_input_class_e::pen,
        input_event_t {
          .payload = pen_tool_event_t {
            .proximity = contact ? pen_proximity_e::contact :
                                   pen_proximity_e::hover,
            .tool = tool,
            .buttons = packet[10],
            .x = *x,
            .y = *y,
            .pressure_or_distance = *pressure_or_distance,
            .tilt_x_degrees = tilt->first,
            .tilt_y_degrees = tilt->second,
          },
        }
      );
    }

    moonlight_packet_result_t decode_controller(
      std::span<const std::uint8_t> packet
    ) {
      if (packet.size() != sizeof(NV_MULTI_CONTROLLER_PACKET) ||
          read_u16_le(packet.subspan(8, 2)) != MC_HEADER_B ||
          read_u16_le(packet.subspan(14, 2)) != MC_MID_B ||
          read_u16_le(packet.subspan(28, 2)) != MC_TAIL_A ||
          read_u16_le(packet.subspan(32, 2)) != MC_TAIL_B) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      const auto controller = read_u16_le(packet.subspan(10, 2));
      if (controller >= maximum_gamepad_slots) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      const auto active_mask = read_u16_le(packet.subspan(12, 2));
      const auto buttons = static_cast<std::uint32_t>(
                             read_u16_le(packet.subspan(16, 2))
                           ) |
                           (static_cast<std::uint32_t>(
                              read_u16_le(packet.subspan(30, 2))
                            )
                            << 16U);
      gamepad_state_event_t state {
        .buttons = buttons,
        .left_trigger = packet[18],
        .right_trigger = packet[19],
        .left_stick_x = read_i16_le(packet.subspan(20, 2)),
        .left_stick_y = read_i16_le(packet.subspan(22, 2)),
        .right_stick_x = read_i16_le(packet.subspan(24, 2)),
        .right_stick_y = read_i16_le(packet.subspan(26, 2)),
      };
      if ((active_mask & (1U << controller)) == 0) {
        if (state != gamepad_state_event_t {}) {
          return {.status = moonlight_packet_status_e::invalid};
        }
      } else if ((buttons & ~supported_gamepad_buttons) != 0) {
        return {.status = moonlight_packet_status_e::unsupported};
      }
      return converted(
        moonlight_input_class_e::controller,
        input_event_t {
          .slot = controller,
          .payload = state,
        }
      );
    }

    moonlight_packet_result_t unsupported_exact(
      std::span<const std::uint8_t> packet,
      std::size_t expected_size,
      std::span<const std::uint8_t> reserved = {}
    ) {
      if (packet.size() != expected_size || !all_zero(reserved)) {
        return {.status = moonlight_packet_status_e::invalid};
      }
      return {.status = moonlight_packet_status_e::unsupported};
    }

  }  // namespace

  moonlight_packet_result_t decode_moonlight_input_packet(
    std::span<const std::uint8_t> packet
  ) {
    if (packet.size() < sizeof(NV_INPUT_HEADER) ||
        packet.size() > maximum_moonlight_input_packet_bytes ||
        read_u32_be(packet.first<4>()) != packet.size() - sizeof(std::uint32_t)) {
      return {.status = moonlight_packet_status_e::invalid};
    }

    switch (read_u32_le(packet.subspan(4, 4))) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return decode_relative_mouse(packet);
      case MOUSE_MOVE_ABS_MAGIC:
        return decode_absolute_mouse(packet);
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
        return decode_mouse_button(packet, button_state_e::pressed);
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        return decode_mouse_button(packet, button_state_e::released);
      case SCROLL_MAGIC_GEN5:
        return decode_vertical_scroll(packet);
      case SS_HSCROLL_MAGIC:
        return decode_horizontal_scroll(packet);
      case KEY_DOWN_EVENT_MAGIC:
        return decode_keyboard(packet, button_state_e::pressed);
      case KEY_UP_EVENT_MAGIC:
        return decode_keyboard(packet, button_state_e::released);
      case MULTI_CONTROLLER_MAGIC_GEN5:
        return decode_controller(packet);
      case SS_TOUCH_MAGIC:
        return decode_touch(packet);
      case SS_PEN_MAGIC:
        return decode_pen(packet);
      case UTF8_TEXT_EVENT_MAGIC:
        return packet.size() >= sizeof(NV_INPUT_HEADER) &&
                   packet.size() <= sizeof(NV_UNICODE_PACKET) ?
                 moonlight_packet_result_t {
                   .status = moonlight_packet_status_e::unsupported,
                 } :
                 moonlight_packet_result_t {
                   .status = moonlight_packet_status_e::invalid,
                 };
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        if (packet.size() != sizeof(SS_CONTROLLER_ARRIVAL_PACKET) ||
            packet[8] >= maximum_gamepad_slots) {
          return {.status = moonlight_packet_status_e::invalid};
        }
        return {.status = moonlight_packet_status_e::ignored};
      case SS_CONTROLLER_TOUCH_MAGIC:
        return unsupported_exact(
          packet,
          sizeof(SS_CONTROLLER_TOUCH_PACKET),
          packet.size() >= 12 ? packet.subspan(10, 2) :
                                std::span<const std::uint8_t> {}
        );
      case SS_CONTROLLER_MOTION_MAGIC:
        return unsupported_exact(
          packet,
          sizeof(SS_CONTROLLER_MOTION_PACKET),
          packet.size() >= 12 ? packet.subspan(10, 2) :
                                std::span<const std::uint8_t> {}
        );
      case SS_CONTROLLER_BATTERY_MAGIC:
        return unsupported_exact(
          packet,
          sizeof(SS_CONTROLLER_BATTERY_PACKET),
          packet.size() >= 12 ? packet.subspan(11, 1) :
                                std::span<const std::uint8_t> {}
        );
      case ENABLE_HAPTICS_MAGIC:
        if (packet.size() != sizeof(NV_HAPTICS_PACKET)) {
          return {.status = moonlight_packet_status_e::invalid};
        }
        return {.status = moonlight_packet_status_e::ignored};
      default:
        return {.status = moonlight_packet_status_e::unsupported};
    }
  }

  bool moonlight_input_permissions_t::permits(
    moonlight_input_class_e input_class
  ) const {
    switch (input_class) {
      case moonlight_input_class_e::keyboard:
        return keyboard;
      case moonlight_input_class_e::mouse:
        return mouse;
      case moonlight_input_class_e::touch:
        return touch;
      case moonlight_input_class_e::pen:
        return pen;
      case moonlight_input_class_e::controller:
        return controller;
    }
    return false;
  }

  moonlight_input_adapter_t::moonlight_input_adapter_t(
    authority_t &authority,
    seat_handle_t handle,
    moonlight_input_permissions_t permissions
  ):
      authority_(authority),
      handle_(std::move(handle)),
      permissions_(permissions) {
    if (!handle_.valid()) {
      throw std::invalid_argument {"invalid multiseat Moonlight input handle"};
    }
  }

  moonlight_route_result_t moonlight_input_adapter_t::route(
    std::span<const std::uint8_t> packet
  ) {
    auto parsed = decode_moonlight_input_packet(packet);
    switch (parsed.status) {
      case moonlight_packet_status_e::ignored:
        return {.status = moonlight_route_status_e::ignored_packet};
      case moonlight_packet_status_e::unsupported:
        return {.status = moonlight_route_status_e::unsupported_packet};
      case moonlight_packet_status_e::invalid:
        return {.status = moonlight_route_status_e::invalid_packet};
      case moonlight_packet_status_e::converted:
        break;
    }
    if (!parsed.decoded ||
        !permissions_.permits(parsed.decoded->input_class)) {
      return {.status = moonlight_route_status_e::permission_denied};
    }

    std::scoped_lock lock {mutex_};
    if (sequence_exhausted_) {
      return {.status = moonlight_route_status_e::sequence_exhausted};
    }
    const auto sequence = next_sequence_;
    const auto canonical = encode_input_event(parsed.decoded->event);
    const auto authority_status = authority_.route(
      handle_,
      sequence,
      canonical
    );
    if (authority_status == status_e::applied) {
      if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        sequence_exhausted_ = true;
      } else {
        ++next_sequence_;
      }
      return {
        .status = moonlight_route_status_e::applied,
        .authority_status = authority_status,
        .sequence = sequence,
      };
    }
    return {
      .status = moonlight_route_status_e::authority_rejected,
      .authority_status = authority_status,
      .sequence = sequence,
    };
  }

  std::uint64_t moonlight_input_adapter_t::next_sequence() const {
    std::scoped_lock lock {mutex_};
    return next_sequence_;
  }

  std::optional<moonlight_feedback_t> convert_controller_feedback(
    const controller_feedback_t &feedback
  ) {
    if (!feedback.handle.valid() || feedback.sequence == 0 ||
        !valid_feedback_event(feedback.event)) {
      return std::nullopt;
    }
    return moonlight_feedback_t {
      .source_sequence = feedback.sequence,
      .message = platf::gamepad_feedback_msg_t::make_rumble(
        static_cast<std::uint16_t>(feedback.event.gamepad_slot),
        feedback.event.low_frequency,
        feedback.event.high_frequency
      ),
    };
  }

  moonlight_controller_feedback_queue_t::moonlight_controller_feedback_queue_t(
    seat_handle_t handle
  ):
      handle_(std::move(handle)) {
    if (!handle_.valid()) {
      throw std::invalid_argument {"invalid multiseat feedback queue handle"};
    }
  }

  controller_feedback_queue_result_e
    moonlight_controller_feedback_queue_t::push(
      const controller_feedback_t &feedback
    ) {
    std::scoped_lock lock {mutex_};
    if (closed_) {
      return controller_feedback_queue_result_e::closed;
    }
    if (feedback.handle != handle_) {
      return controller_feedback_queue_result_e::stale_generation;
    }
    const auto converted = convert_controller_feedback(feedback);
    if (!converted) {
      return controller_feedback_queue_result_e::invalid_event;
    }
    if (sequence_exhausted_ ||
        last_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
        feedback.sequence != last_sequence_ + 1) {
      return controller_feedback_queue_result_e::invalid_sequence;
    }

    auto &pending = pending_[feedback.event.gamepad_slot];
    const bool coalesced = pending.has_value();
    pending = *converted;
    if (!coalesced) {
      ++pending_count_;
    }
    last_sequence_ = feedback.sequence;
    if (last_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      sequence_exhausted_ = true;
    }
    return coalesced ? controller_feedback_queue_result_e::coalesced :
                       controller_feedback_queue_result_e::enqueued;
  }

  std::optional<moonlight_feedback_t>
    moonlight_controller_feedback_queue_t::pop() {
    std::scoped_lock lock {mutex_};
    std::size_t selected = pending_.size();
    for (std::size_t index = 0; index < pending_.size(); ++index) {
      if (!pending_[index]) {
        continue;
      }
      if (selected == pending_.size() ||
          pending_[index]->source_sequence <
            pending_[selected]->source_sequence) {
        selected = index;
      }
    }
    if (selected == pending_.size()) {
      return std::nullopt;
    }
    auto result = pending_[selected];
    pending_[selected].reset();
    --pending_count_;
    return result;
  }

  void moonlight_controller_feedback_queue_t::close() {
    std::scoped_lock lock {mutex_};
    closed_ = true;
    for (auto &pending : pending_) {
      pending.reset();
    }
    pending_count_ = 0;
  }

  std::size_t moonlight_controller_feedback_queue_t::pending() const {
    std::scoped_lock lock {mutex_};
    return pending_count_;
  }

  std::uint64_t moonlight_controller_feedback_queue_t::last_sequence() const {
    std::scoped_lock lock {mutex_};
    return last_sequence_;
  }

  bool moonlight_controller_feedback_queue_t::closed() const {
    std::scoped_lock lock {mutex_};
    return closed_;
  }

}  // namespace multiseat::input

#endif
