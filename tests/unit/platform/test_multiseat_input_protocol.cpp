/**
 * @file tests/unit/platform/test_multiseat_input_protocol.cpp
 * @brief Canonical-codec tests for host-brokered multiseat input.
 */
#include "src/platform/linux/multiseat_input_protocol.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace {
  using namespace multiseat::input;

  std::vector<input_event_t> canonical_events() {
    return {
      {
        .payload = keyboard_key_event_t {
          .key_code = 0x41,
          .state = button_state_e::pressed,
        },
      },
      {
        .payload = mouse_relative_event_t {
          .delta_x = -32767,
          .delta_y = 32767,
        },
      },
      {
        .payload = mouse_absolute_event_t {
          .x = 2560,
          .y = 1440,
          .width = 2560,
          .height = 1440,
        },
      },
      {
        .payload = mouse_button_event_t {
          .button = mouse_button_e::extra,
          .state = button_state_e::released,
        },
      },
      {
        .payload = mouse_scroll_event_t {
          .vertical = -120,
          .horizontal = 240,
        },
      },
      {
        .payload = touch_contact_event_t {
          .pointer_id = 65535,
          .action = touch_action_e::move,
          .orientation_degrees = -90,
          .x = 65535,
          .y = 1234,
          .pressure = 4321,
        },
      },
      {
        .payload = touch_contact_event_t {
          .pointer_id = 5,
          .action = touch_action_e::release,
        },
      },
      {
        .payload = pen_tool_event_t {
          .proximity = pen_proximity_e::contact,
          .tool = pen_tool_e::eraser,
          .buttons = 0x07,
          .x = 65535,
          .y = 42,
          .pressure_or_distance = 32000,
          .tilt_x_degrees = -90,
          .tilt_y_degrees = 90,
        },
      },
      {
        .slot = 15,
        .payload = gamepad_state_event_t {
          .buttons = supported_gamepad_buttons & ~0x000AU,
          .left_trigger = 255,
          .right_trigger = 128,
          .left_stick_x = -32768,
          .left_stick_y = 32767,
          .right_stick_x = 1234,
          .right_stick_y = -5678,
        },
      },
    };
  }

  TEST(MultiseatInputProtocol, CanonicalEventsRoundTripExactly) {
    for (const auto &event : canonical_events()) {
      SCOPED_TRACE(static_cast<int>(kind_of(event)));
      ASSERT_TRUE(valid_input_event(event));
      const auto encoded = encode_input_event(event);
      EXPECT_GE(encoded.size(), encoded_event_header_size);
      EXPECT_LE(encoded.size(), maximum_encoded_input_event_bytes);
      const auto decoded = decode_input_event(encoded);
      ASSERT_TRUE(decoded.has_value());
      EXPECT_EQ(*decoded, event);
    }

    const auto keyboard = encode_input_event(canonical_events().front());
    EXPECT_EQ(
      keyboard,
      (std::vector<std::uint8_t> {
        'P',
        'I',
        'N',
        '1',
        1,
        0,
        0,
        0,
        0,
        0x41,
        1,
        0,
      })
    );
  }

  TEST(MultiseatInputProtocol, DecoderRejectsMalleableOrMalformedFrames) {
    auto encoded = encode_input_event(canonical_events().front());
    for (std::size_t size = 0; size < encoded.size(); ++size) {
      EXPECT_FALSE(decode_input_event(
                     std::span<const std::uint8_t> {encoded}.first(size)
      )
                     .has_value());
    }

    auto malformed = encoded;
    malformed.push_back(0);
    EXPECT_FALSE(decode_input_event(malformed).has_value());
    malformed = encoded;
    malformed[0] = 'X';
    EXPECT_FALSE(decode_input_event(malformed).has_value());
    malformed = encoded;
    malformed[4] = 0xFF;
    EXPECT_FALSE(decode_input_event(malformed).has_value());
    malformed = encoded;
    malformed[5] = 1;
    EXPECT_FALSE(decode_input_event(malformed).has_value());
    malformed = encoded;
    malformed[7] = 1;
    EXPECT_FALSE(decode_input_event(malformed).has_value());
    malformed = encoded;
    malformed[11] = 1;
    EXPECT_FALSE(decode_input_event(malformed).has_value());

    malformed.assign(maximum_encoded_input_event_bytes + 1, 0);
    EXPECT_FALSE(decode_input_event(malformed).has_value());
  }

  TEST(MultiseatInputProtocol, TypedValidationRejectsOutOfContractValues) {
    const std::vector<input_event_t> invalid {
      {
        .slot = 1,
        .payload = keyboard_key_event_t {.key_code = 0x41},
      },
      {
        .payload = keyboard_key_event_t {.key_code = 0x18},
      },
      {
        .payload = mouse_relative_event_t {},
      },
      {
        .payload = mouse_relative_event_t {
          .delta_x = maximum_relative_pointer_delta + 1,
        },
      },
      {
        .payload = mouse_absolute_event_t {
          .x = 1,
          .width = 0,
          .height = 1080,
        },
      },
      {
        .payload = mouse_absolute_event_t {
          .x = 1921,
          .width = 1920,
          .height = 1080,
        },
      },
      {
        .payload = mouse_button_event_t {
          .button = static_cast<mouse_button_e>(99),
        },
      },
      {
        .payload = mouse_scroll_event_t {},
      },
      {
        .payload = touch_contact_event_t {
          .pointer_id = 65536,
          .action = touch_action_e::down,
        },
      },
      {
        .payload = touch_contact_event_t {
          .pointer_id = 1,
          .action = touch_action_e::release,
          .x = 1,
        },
      },
      {
        .payload = pen_tool_event_t {
          .buttons = 0x08,
        },
      },
      {
        .payload = pen_tool_event_t {
          .tilt_x_degrees = -91,
        },
      },
      {
        .slot = maximum_gamepad_slots,
        .payload = gamepad_state_event_t {},
      },
      {
        .payload = gamepad_state_event_t {.buttons = 0x0800},
      },
      {
        .payload = gamepad_state_event_t {.buttons = 0x0003},
      },
      {
        .payload = gamepad_state_event_t {.buttons = 0x000C},
      },
    };

    for (const auto &event : invalid) {
      EXPECT_FALSE(valid_input_event(event));
      EXPECT_THROW((void) encode_input_event(event), std::invalid_argument);
    }
  }

  TEST(MultiseatInputProtocol, FeedbackHasOneCanonicalControllerShape) {
    const feedback_event_t event {
      .kind = feedback_kind_e::rumble,
      .gamepad_slot = 3,
      .low_frequency = 0x1234,
      .high_frequency = 0xABCD,
    };
    const auto encoded = encode_feedback_event(event);
    EXPECT_EQ(
      encoded,
      (std::vector<std::uint8_t> {
        'P',
        'F',
        'B',
        '1',
        1,
        0,
        0,
        3,
        0x12,
        0x34,
        0xAB,
        0xCD,
      })
    );
    ASSERT_EQ(encoded.size(), encoded_feedback_event_bytes);
    ASSERT_TRUE(decode_feedback_event(encoded).has_value());
    EXPECT_EQ(*decode_feedback_event(encoded), event);

    auto malformed = encoded;
    malformed[5] = 1;
    EXPECT_FALSE(decode_feedback_event(malformed).has_value());
    malformed = encoded;
    malformed[4] = 2;
    EXPECT_FALSE(decode_feedback_event(malformed).has_value());
    malformed = encoded;
    malformed[7] = maximum_gamepad_slots;
    EXPECT_FALSE(decode_feedback_event(malformed).has_value());
    malformed = encoded;
    malformed.push_back(0);
    EXPECT_FALSE(decode_feedback_event(malformed).has_value());

    auto invalid = event;
    invalid.gamepad_slot = maximum_gamepad_slots;
    EXPECT_THROW((void) encode_feedback_event(invalid), std::invalid_argument);
  }
}  // namespace
