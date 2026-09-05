/**
 * @file tests/unit/platform/test_multiseat_moonlight_input_adapter.cpp
 * @brief Offline tests for Moonlight input conversion and feedback queuing.
 */
#include "src/platform/linux/multiseat_moonlight_input_adapter.h"

extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace multiseat::input;

  using bytes_t = std::vector<std::uint8_t>;

  void append_u16_be(bytes_t &bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
  }

  void append_u16_le(bytes_t &bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  }

  void append_i16_be(bytes_t &bytes, std::int16_t value) {
    append_u16_be(bytes, std::bit_cast<std::uint16_t>(value));
  }

  void append_i16_le(bytes_t &bytes, std::int16_t value) {
    append_u16_le(bytes, std::bit_cast<std::uint16_t>(value));
  }

  void append_u32_le(bytes_t &bytes, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
      bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
    }
  }

  void append_netfloat(bytes_t &bytes, float value) {
    append_u32_le(bytes, std::bit_cast<std::uint32_t>(value));
  }

  bytes_t packet(std::uint32_t magic, bytes_t body) {
    bytes_t result;
    const auto declared = static_cast<std::uint32_t>(body.size() + 4);
    result.push_back(static_cast<std::uint8_t>(declared >> 24U));
    result.push_back(static_cast<std::uint8_t>(declared >> 16U));
    result.push_back(static_cast<std::uint8_t>(declared >> 8U));
    result.push_back(static_cast<std::uint8_t>(declared));
    append_u32_le(result, magic);
    result.insert(result.end(), body.begin(), body.end());
    return result;
  }

  bytes_t relative_packet(std::int16_t x, std::int16_t y) {
    bytes_t body;
    append_i16_be(body, x);
    append_i16_be(body, y);
    return packet(MOUSE_MOVE_REL_MAGIC_GEN5, std::move(body));
  }

  bytes_t absolute_packet(
    std::int16_t x,
    std::int16_t y,
    std::int16_t width,
    std::int16_t height,
    std::uint16_t reserved = 0
  ) {
    bytes_t body;
    append_i16_be(body, x);
    append_i16_be(body, y);
    append_u16_be(body, reserved);
    append_i16_be(body, width);
    append_i16_be(body, height);
    return packet(MOUSE_MOVE_ABS_MAGIC, std::move(body));
  }

  bytes_t keyboard_packet(
    std::uint16_t key,
    bool released,
    std::uint8_t flags = 0,
    std::uint8_t modifiers = 0,
    std::uint16_t reserved = 0
  ) {
    bytes_t body {flags};
    append_u16_le(body, key);
    body.push_back(modifiers);
    append_u16_le(body, reserved);
    return packet(
      released ? KEY_UP_EVENT_MAGIC : KEY_DOWN_EVENT_MAGIC,
      std::move(body)
    );
  }

  bytes_t vertical_scroll_packet(
    std::int16_t first,
    std::optional<std::int16_t> second = std::nullopt,
    std::uint16_t reserved = 0
  ) {
    bytes_t body;
    append_i16_be(body, first);
    append_i16_be(body, second.value_or(first));
    append_u16_be(body, reserved);
    return packet(SCROLL_MAGIC_GEN5, std::move(body));
  }

  bytes_t horizontal_scroll_packet(std::int16_t amount) {
    bytes_t body;
    append_i16_be(body, amount);
    return packet(SS_HSCROLL_MAGIC, std::move(body));
  }

  bytes_t touch_packet(
    std::uint8_t event_type,
    std::uint32_t pointer_id,
    float x,
    float y,
    float pressure,
    std::uint16_t rotation = LI_ROT_UNKNOWN,
    float major = 0.0F,
    float minor = 0.0F,
    std::uint8_t reserved = 0
  ) {
    bytes_t body {event_type, reserved};
    append_u16_le(body, rotation);
    append_u32_le(body, pointer_id);
    append_netfloat(body, x);
    append_netfloat(body, y);
    append_netfloat(body, pressure);
    append_netfloat(body, major);
    append_netfloat(body, minor);
    return packet(SS_TOUCH_MAGIC, std::move(body));
  }

  bytes_t pen_packet(
    std::uint8_t event_type,
    std::uint8_t tool,
    std::uint8_t buttons,
    float x,
    float y,
    float pressure_or_distance,
    std::uint16_t rotation,
    std::uint8_t tilt,
    float major = 0.0F,
    float minor = 0.0F,
    std::uint8_t reserved = 0,
    std::uint8_t reserved2 = 0
  ) {
    bytes_t body {event_type, tool, buttons, reserved};
    append_netfloat(body, x);
    append_netfloat(body, y);
    append_netfloat(body, pressure_or_distance);
    append_u16_le(body, rotation);
    body.push_back(tilt);
    body.push_back(reserved2);
    append_netfloat(body, major);
    append_netfloat(body, minor);
    return packet(SS_PEN_MAGIC, std::move(body));
  }

  bytes_t controller_packet(
    std::uint16_t controller,
    std::uint16_t active_mask,
    std::uint32_t buttons,
    std::uint8_t left_trigger = 0,
    std::uint8_t right_trigger = 0,
    std::int16_t left_x = 0,
    std::int16_t left_y = 0,
    std::int16_t right_x = 0,
    std::int16_t right_y = 0,
    std::uint16_t header = MC_HEADER_B,
    std::uint16_t middle = MC_MID_B,
    std::uint16_t tail_a = MC_TAIL_A,
    std::uint16_t tail_b = MC_TAIL_B
  ) {
    bytes_t body;
    append_u16_le(body, header);
    append_u16_le(body, controller);
    append_u16_le(body, active_mask);
    append_u16_le(body, middle);
    append_u16_le(body, static_cast<std::uint16_t>(buttons));
    body.push_back(left_trigger);
    body.push_back(right_trigger);
    append_i16_le(body, left_x);
    append_i16_le(body, left_y);
    append_i16_le(body, right_x);
    append_i16_le(body, right_y);
    append_u16_le(body, tail_a);
    append_u16_le(body, static_cast<std::uint16_t>(buttons >> 16U));
    append_u16_le(body, tail_b);
    return packet(MULTI_CONTROLLER_MAGIC_GEN5, std::move(body));
  }

  decoded_moonlight_packet_t decoded(const bytes_t &packet_bytes) {
    const auto result = decode_moonlight_input_packet(packet_bytes);
    EXPECT_EQ(result.status, moonlight_packet_status_e::converted);
    EXPECT_TRUE(result.decoded.has_value());
    return result.decoded.value_or(decoded_moonlight_packet_t {});
  }

  TEST(MultiseatMoonlightInputAdapter, ConvertsIntegerPacketsWithExplicitEndianRules) {
    EXPECT_EQ(
      decoded(relative_packet(-27, 32767)),
      (decoded_moonlight_packet_t {
        .input_class = moonlight_input_class_e::mouse,
        .event = input_event_t {
          .payload = mouse_relative_event_t {
            .delta_x = -27,
            .delta_y = 32767,
          },
        },
      })
    );
    EXPECT_EQ(
      decoded(absolute_packet(320, 240, 1920, 1080)),
      (decoded_moonlight_packet_t {
        .input_class = moonlight_input_class_e::mouse,
        .event = input_event_t {
          .payload = mouse_absolute_event_t {
            .x = 320,
            .y = 240,
            .width = 1920,
            .height = 1080,
          },
        },
      })
    );
    EXPECT_EQ(
      decoded(packet(MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5, {BUTTON_X2})).event,
      (input_event_t {
        .payload = mouse_button_event_t {
          .button = mouse_button_e::extra,
          .state = button_state_e::released,
        },
      })
    );
    EXPECT_EQ(
      decoded(vertical_scroll_packet(-120)).event,
      (input_event_t {
        .payload = mouse_scroll_event_t {.vertical = -120},
      })
    );
    EXPECT_EQ(
      decoded(horizontal_scroll_packet(240)).event,
      (input_event_t {
        .payload = mouse_scroll_event_t {.horizontal = 240},
      })
    );
    EXPECT_EQ(
      decoded(keyboard_packet(0x41, false, 0, MODIFIER_SHIFT)),
      (decoded_moonlight_packet_t {
        .input_class = moonlight_input_class_e::keyboard,
        .event = input_event_t {
          .payload = keyboard_key_event_t {
            .key_code = 0x41,
            .state = button_state_e::pressed,
          },
        },
      })
    );

    const auto controller = decoded(controller_packet(
      3,
      1U << 3U,
      A_FLAG | PLAY_FLAG,
      10,
      20,
      -32768,
      32767,
      -123,
      456
    ));
    EXPECT_EQ(controller.input_class, moonlight_input_class_e::controller);
    EXPECT_EQ(
      controller.event,
      (input_event_t {
        .slot = 3,
        .payload = gamepad_state_event_t {
          .buttons = A_FLAG | PLAY_FLAG,
          .left_trigger = 10,
          .right_trigger = 20,
          .left_stick_x = -32768,
          .left_stick_y = 32767,
          .right_stick_x = -123,
          .right_stick_y = 456,
        },
      })
    );
  }

  TEST(MultiseatMoonlightInputAdapter, ConvertsTouchAndPenIntoBoundedIntegers) {
    const auto touch = decoded(touch_packet(
      LI_TOUCH_EVENT_DOWN,
      9,
      0.5F,
      1.0F,
      0.25F,
      270,
      0.1F,
      0.05F
    ));
    EXPECT_EQ(touch.input_class, moonlight_input_class_e::touch);
    EXPECT_EQ(
      touch.event,
      (input_event_t {
        .payload = touch_contact_event_t {
          .pointer_id = 9,
          .action = touch_action_e::down,
          .orientation_degrees = -90,
          .x = 32768,
          .y = 65535,
          .pressure = 16384,
        },
      })
    );

    const auto released = decoded(touch_packet(
      LI_TOUCH_EVENT_CANCEL,
      9,
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::quiet_NaN()
    ));
    EXPECT_EQ(
      released.event,
      (input_event_t {
        .payload = touch_contact_event_t {
          .pointer_id = 9,
          .action = touch_action_e::release,
        },
      })
    );

    const auto pen = decoded(pen_packet(
      LI_TOUCH_EVENT_DOWN,
      LI_TOOL_TYPE_ERASER,
      LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_TERTIARY,
      0.25F,
      0.75F,
      0.5F,
      90,
      45,
      0.2F,
      0.1F
    ));
    EXPECT_EQ(pen.input_class, moonlight_input_class_e::pen);
    EXPECT_EQ(
      pen.event,
      (input_event_t {
        .payload = pen_tool_event_t {
          .proximity = pen_proximity_e::contact,
          .tool = pen_tool_e::eraser,
          .buttons = LI_PEN_BUTTON_PRIMARY | LI_PEN_BUTTON_TERTIARY,
          .x = 16384,
          .y = 49151,
          .pressure_or_distance = 32768,
          .tilt_x_degrees = -45,
          .tilt_y_degrees = 0,
        },
      })
    );

    const auto hover = decoded(pen_packet(
      LI_TOUCH_EVENT_UP,
      LI_TOOL_TYPE_PEN,
      0,
      0.25F,
      0.75F,
      0.0F,
      LI_ROT_UNKNOWN,
      LI_TILT_UNKNOWN
    ));
    EXPECT_EQ(
      std::get<pen_tool_event_t>(hover.event.payload).proximity,
      pen_proximity_e::hover
    );
  }

  TEST(MultiseatMoonlightInputAdapter, RejectsTruncationAndTrailingBytesForEveryConvertedShape) {
    const std::vector<bytes_t> exact_packets {
      relative_packet(1, -1),
      absolute_packet(1, 1, 10, 10),
      packet(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, {BUTTON_LEFT}),
      vertical_scroll_packet(1),
      horizontal_scroll_packet(-1),
      keyboard_packet(0x41, false),
      touch_packet(LI_TOUCH_EVENT_DOWN, 1, 0.5F, 0.5F, 0.5F),
      pen_packet(
        LI_TOUCH_EVENT_DOWN,
        LI_TOOL_TYPE_PEN,
        0,
        0.5F,
        0.5F,
        0.5F,
        LI_ROT_UNKNOWN,
        LI_TILT_UNKNOWN
      ),
      controller_packet(0, 1, A_FLAG),
    };

    for (const auto &exact : exact_packets) {
      ASSERT_EQ(
        decode_moonlight_input_packet(exact).status,
        moonlight_packet_status_e::converted
      );
      for (std::size_t size = 0; size < exact.size(); ++size) {
        EXPECT_EQ(
          decode_moonlight_input_packet(
            std::span<const std::uint8_t> {exact}.first(size)
          )
            .status,
          moonlight_packet_status_e::invalid
        );
      }
      auto trailing = exact;
      trailing.push_back(0);
      EXPECT_EQ(
        decode_moonlight_input_packet(trailing).status,
        moonlight_packet_status_e::invalid
      );
    }
  }

  TEST(MultiseatMoonlightInputAdapter, RejectsMalformedOrSemanticallyUnsafePackets) {
    auto malformed = keyboard_packet(0x41, false);
    malformed[3] = 0;
    EXPECT_EQ(
      decode_moonlight_input_packet(malformed).status,
      moonlight_packet_status_e::invalid
    );

    EXPECT_EQ(
      decode_moonlight_input_packet(absolute_packet(1, 1, 10, 10, 1)).status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(absolute_packet(11, 1, 10, 10)).status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(vertical_scroll_packet(1, 2)).status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(keyboard_packet(0x41, false, 0, 0x80)).status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(keyboard_packet(0x18, false)).status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(touch_packet(
                                      LI_TOUCH_EVENT_DOWN,
                                      65536,
                                      0.5F,
                                      0.5F,
                                      0.5F
                                    ))
        .status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(touch_packet(
                                      LI_TOUCH_EVENT_DOWN,
                                      1,
                                      std::numeric_limits<float>::infinity(),
                                      0.5F,
                                      0.5F
                                    ))
        .status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(pen_packet(
                                      LI_TOUCH_EVENT_DOWN,
                                      LI_TOOL_TYPE_PEN,
                                      0,
                                      0.5F,
                                      0.5F,
                                      0.5F,
                                      0,
                                      91
                                    ))
        .status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(controller_packet(
                                      0,
                                      1,
                                      UP_FLAG | DOWN_FLAG
                                    ))
        .status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(controller_packet(
                                      0,
                                      0,
                                      A_FLAG
                                    ))
        .status,
      moonlight_packet_status_e::invalid
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(controller_packet(
                                      0,
                                      1,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0,
                                      0
                                    ))
        .status,
      moonlight_packet_status_e::invalid
    );
  }

  TEST(MultiseatMoonlightInputAdapter, DistinguishesNoOpAndUnsupportedPackets) {
    EXPECT_EQ(
      decode_moonlight_input_packet(relative_packet(0, 0)).status,
      moonlight_packet_status_e::ignored
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(vertical_scroll_packet(0)).status,
      moonlight_packet_status_e::ignored
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(keyboard_packet(
                                      0x41,
                                      false,
                                      SS_KBE_FLAG_NON_NORMALIZED
                                    ))
        .status,
      moonlight_packet_status_e::unsupported
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(touch_packet(
                                      LI_TOUCH_EVENT_CANCEL_ALL,
                                      0,
                                      0.0F,
                                      0.0F,
                                      0.0F
                                    ))
        .status,
      moonlight_packet_status_e::unsupported
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(controller_packet(
                                      0,
                                      1,
                                      PADDLE1_FLAG
                                    ))
        .status,
      moonlight_packet_status_e::unsupported
    );

    bytes_t arrival_body(8, 0);
    arrival_body[0] = 2;
    EXPECT_EQ(
      decode_moonlight_input_packet(packet(
                                      SS_CONTROLLER_ARRIVAL_MAGIC,
                                      std::move(arrival_body)
                                    ))
        .status,
      moonlight_packet_status_e::ignored
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(packet(
                                      UTF8_TEXT_EVENT_MAGIC,
                                      {'h', 'i'}
                                    ))
        .status,
      moonlight_packet_status_e::unsupported
    );
    EXPECT_EQ(
      decode_moonlight_input_packet(packet(0x12345678, {})).status,
      moonlight_packet_status_e::unsupported
    );
  }

  multiseat::seat_handle_t handle_for(std::uint64_t generation) {
    return {
      .controller_epoch = "controller-a",
      .logical_gpu_id = "gpu-primary",
      .slot = 0,
      .generation = generation,
    };
  }

  expectation_t expectation_for(std::uint64_t generation) {
    return {
      .handle = handle_for(generation),
      .input_seat = "polaris-input-controller-a-" + std::to_string(generation),
      .plan = {.touch = true, .pen = true, .gamepad_slots = 2},
    };
  }

  allocation_t allocation_for(const expectation_t &expectation) {
    allocation_t allocation {
      .handle = expectation.handle,
      .input_seat = expectation.input_seat,
      .plan = expectation.plan,
    };
    std::vector<std::pair<device_kind_e, std::uint32_t>> required {
      {device_kind_e::keyboard, 0},
      {device_kind_e::mouse_relative, 0},
      {device_kind_e::mouse_absolute, 0},
      {device_kind_e::touch, 0},
      {device_kind_e::pen, 0},
      {device_kind_e::gamepad, 0},
      {device_kind_e::gamepad, 1},
    };
    for (std::size_t index = 0; index < required.size(); ++index) {
      const auto [kind, slot] = required[index];
      allocation.nodes.push_back({
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(index),
        .worker_path = expected_worker_path(kind, slot),
        .filesystem_device = 41,
        .inode = 1000 + index,
        .character_major = 13,
        .character_minor = static_cast<std::uint32_t>(64 + index),
        .kernel_name = expected_kernel_name(expectation.input_seat, kind, slot),
        .host_seat = std::string {isolated_host_seat},
      });
    }
    return allocation;
  }

  class adapter_backend_t final: public backend_t {
  public:
    struct route_call_t {
      multiseat::seat_handle_t handle;
      std::string input_seat;
      std::uint64_t sequence = 0;
      input_event_t event;
    };

    backend_create_result_t create(const expectation_t &expectation) override {
      current = allocation_for(expectation);
      return {
        .result = backend_result_e::applied,
        .allocation = current,
      };
    }

    backend_result_e destroy(
      const multiseat::seat_handle_t &,
      std::string_view
    ) override {
      current.reset();
      return backend_result_e::applied;
    }

    backend_result_e route(
      const multiseat::seat_handle_t &handle,
      std::string_view input_seat,
      std::uint64_t sequence,
      const input_event_t &event
    ) override {
      calls.push_back({
        .handle = handle,
        .input_seat = std::string {input_seat},
        .sequence = sequence,
        .event = event,
      });
      return next_route;
    }

    std::vector<allocation_t> inventory() override {
      return current ? std::vector<allocation_t> {*current} :
                       std::vector<allocation_t> {};
    }

    std::optional<allocation_t> current;
    std::vector<route_call_t> calls;
    backend_result_e next_route = backend_result_e::applied;
  };

  moonlight_input_permissions_t all_permissions() {
    return {
      .keyboard = true,
      .mouse = true,
      .touch = true,
      .pen = true,
      .controller = true,
    };
  }

  TEST(MultiseatMoonlightInputAdapter, RoutesOnlyPermittedPacketsWithOneSequence) {
    adapter_backend_t backend;
    authority_t authority {backend};
    ASSERT_TRUE(authority.reconcile({}).admission_ready);
    const auto expectation = expectation_for(1);
    ASSERT_TRUE(authority.prepare(expectation).prepared());
    moonlight_input_adapter_t adapter {
      authority,
      expectation.handle,
      moonlight_input_permissions_t {.keyboard = true}
    };

    auto result = adapter.route(relative_packet(1, 2));
    EXPECT_EQ(result.status, moonlight_route_status_e::permission_denied);
    EXPECT_EQ(adapter.next_sequence(), 1U);
    auto malformed = keyboard_packet(0x41, false);
    malformed[3] = 0;
    result = adapter.route(malformed);
    EXPECT_EQ(result.status, moonlight_route_status_e::invalid_packet);
    EXPECT_EQ(adapter.next_sequence(), 1U);
    result = adapter.route(keyboard_packet(
      0x41,
      false,
      SS_KBE_FLAG_NON_NORMALIZED
    ));
    EXPECT_EQ(result.status, moonlight_route_status_e::unsupported_packet);
    EXPECT_EQ(adapter.next_sequence(), 1U);

    result = adapter.route(keyboard_packet(0x41, false));
    EXPECT_EQ(result.status, moonlight_route_status_e::applied);
    EXPECT_EQ(result.authority_status, status_e::applied);
    EXPECT_EQ(result.sequence, 1U);
    EXPECT_EQ(adapter.next_sequence(), 2U);
    ASSERT_EQ(backend.calls.size(), 1U);
    EXPECT_EQ(backend.calls[0].handle, expectation.handle);
    EXPECT_EQ(backend.calls[0].input_seat, expectation.input_seat);
    EXPECT_EQ(backend.calls[0].sequence, 1U);
    EXPECT_EQ(
      backend.calls[0].event,
      (input_event_t {
        .payload = keyboard_key_event_t {
          .key_code = 0x41,
          .state = button_state_e::pressed,
        },
      })
    );

    result = adapter.route(packet(SS_CONTROLLER_ARRIVAL_MAGIC, bytes_t(8, 0)));
    EXPECT_EQ(result.status, moonlight_route_status_e::ignored_packet);
    EXPECT_EQ(adapter.next_sequence(), 2U);
  }

  TEST(MultiseatMoonlightInputAdapter, FencesTheCompleteGenerationAndSerializesCallers) {
    adapter_backend_t backend;
    authority_t authority {backend};
    ASSERT_TRUE(authority.reconcile({}).admission_ready);
    const auto expectation = expectation_for(5);
    ASSERT_TRUE(authority.prepare(expectation).prepared());

    moonlight_input_adapter_t stale {
      authority,
      handle_for(4),
      all_permissions()
    };
    const auto stale_result = stale.route(relative_packet(1, 0));
    EXPECT_EQ(stale_result.status, moonlight_route_status_e::authority_rejected);
    EXPECT_EQ(stale_result.authority_status, status_e::stale_authority);
    EXPECT_EQ(stale.next_sequence(), 1U);

    moonlight_input_adapter_t adapter {
      authority,
      expectation.handle,
      all_permissions()
    };
    constexpr std::size_t call_count = 64;
    std::vector<std::future<moonlight_route_result_t>> futures;
    futures.reserve(call_count);
    for (std::size_t call = 0; call < call_count; ++call) {
      futures.push_back(std::async(std::launch::async, [&adapter, call]() {
        return adapter.route(relative_packet(
          static_cast<std::int16_t>(call + 1),
          0
        ));
      }));
    }
    for (auto &future : futures) {
      EXPECT_EQ(future.get().status, moonlight_route_status_e::applied);
    }
    ASSERT_EQ(backend.calls.size(), call_count);
    for (std::size_t index = 0; index < backend.calls.size(); ++index) {
      EXPECT_EQ(backend.calls[index].sequence, index + 1);
    }
    EXPECT_EQ(adapter.next_sequence(), call_count + 1);
  }

  TEST(MultiseatMoonlightInputAdapter, RetriesTheSameSequenceAfterAuthorityRejection) {
    adapter_backend_t backend;
    authority_t authority {backend};
    ASSERT_TRUE(authority.reconcile({}).admission_ready);
    const auto expectation = expectation_for(6);
    ASSERT_TRUE(authority.prepare(expectation).prepared());
    moonlight_input_adapter_t adapter {
      authority,
      expectation.handle,
      all_permissions()
    };

    backend.next_route = backend_result_e::rejected;
    auto result = adapter.route(relative_packet(1, 0));
    EXPECT_EQ(result.status, moonlight_route_status_e::authority_rejected);
    EXPECT_EQ(result.authority_status, status_e::backend_rejected);
    EXPECT_EQ(result.sequence, 1U);
    EXPECT_EQ(adapter.next_sequence(), 1U);
    ASSERT_EQ(backend.calls.size(), 1U);

    result = adapter.route(relative_packet(2, 0));
    EXPECT_EQ(result.status, moonlight_route_status_e::authority_rejected);
    EXPECT_EQ(result.authority_status, status_e::reconciliation_required);
    EXPECT_EQ(result.sequence, 1U);
    EXPECT_EQ(adapter.next_sequence(), 1U);
    ASSERT_EQ(backend.calls.size(), 1U);

    backend.next_route = backend_result_e::applied;
    ASSERT_TRUE(authority.reconcile({expectation}).admission_ready);
    result = adapter.route(relative_packet(3, 0));
    EXPECT_EQ(result.status, moonlight_route_status_e::applied);
    EXPECT_EQ(result.authority_status, status_e::applied);
    EXPECT_EQ(result.sequence, 1U);
    EXPECT_EQ(adapter.next_sequence(), 2U);
    ASSERT_EQ(backend.calls.size(), 2U);
    EXPECT_EQ(backend.calls.back().sequence, 1U);
  }

  controller_feedback_t feedback_for(
    const multiseat::seat_handle_t &handle,
    std::uint64_t sequence,
    std::uint32_t slot,
    std::uint16_t low,
    std::uint16_t high
  ) {
    return {
      .handle = handle,
      .sequence = sequence,
      .event = feedback_event_t {
        .kind = feedback_kind_e::rumble,
        .gamepad_slot = slot,
        .low_frequency = low,
        .high_frequency = high,
      },
    };
  }

  TEST(MultiseatMoonlightInputAdapter, ConvertsAndCoalescesBoundedFeedbackBySlot) {
    const auto handle = handle_for(10);
    moonlight_controller_feedback_queue_t queue {handle};
    for (std::uint32_t slot = 0; slot < maximum_gamepad_slots; ++slot) {
      EXPECT_EQ(
        queue.push(feedback_for(handle, slot + 1, slot, slot, slot + 1)),
        controller_feedback_queue_result_e::enqueued
      );
    }
    EXPECT_EQ(queue.pending(), maximum_pending_controller_feedback);
    EXPECT_EQ(
      queue.push(feedback_for(handle, 17, 0, 1000, 2000)),
      controller_feedback_queue_result_e::coalesced
    );
    EXPECT_EQ(queue.pending(), maximum_pending_controller_feedback);
    EXPECT_EQ(queue.last_sequence(), 17U);

    for (std::uint64_t sequence = 2; sequence <= 16; ++sequence) {
      const auto item = queue.pop();
      ASSERT_TRUE(item.has_value());
      EXPECT_EQ(item->source_sequence, sequence);
      EXPECT_EQ(item->message.type, platf::gamepad_feedback_e::rumble);
      EXPECT_EQ(item->message.id, sequence - 1);
      EXPECT_EQ(item->message.data.rumble.lowfreq, sequence - 1);
      EXPECT_EQ(item->message.data.rumble.highfreq, sequence);
    }
    const auto latest = queue.pop();
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->source_sequence, 17U);
    EXPECT_EQ(latest->message.id, 0U);
    EXPECT_EQ(latest->message.data.rumble.lowfreq, 1000U);
    EXPECT_EQ(latest->message.data.rumble.highfreq, 2000U);
    EXPECT_FALSE(queue.pop().has_value());
  }

  TEST(MultiseatMoonlightInputAdapter, FeedbackQueueRejectsStaleGapsAndClose) {
    const auto handle = handle_for(20);
    moonlight_controller_feedback_queue_t queue {handle};
    EXPECT_EQ(
      queue.push(feedback_for(handle_for(19), 1, 0, 1, 2)),
      controller_feedback_queue_result_e::stale_generation
    );
    EXPECT_EQ(
      queue.push(feedback_for(handle, 2, 0, 1, 2)),
      controller_feedback_queue_result_e::invalid_sequence
    );
    auto invalid = feedback_for(handle, 1, maximum_gamepad_slots, 1, 2);
    EXPECT_EQ(
      queue.push(invalid),
      controller_feedback_queue_result_e::invalid_event
    );
    EXPECT_EQ(queue.last_sequence(), 0U);
    EXPECT_EQ(
      queue.push(feedback_for(handle, 1, 0, 1, 2)),
      controller_feedback_queue_result_e::enqueued
    );
    queue.close();
    EXPECT_TRUE(queue.closed());
    EXPECT_EQ(queue.pending(), 0U);
    EXPECT_FALSE(queue.pop().has_value());
    EXPECT_EQ(
      queue.push(feedback_for(handle, 2, 0, 0, 0)),
      controller_feedback_queue_result_e::closed
    );
  }

  TEST(MultiseatMoonlightInputAdapter, FeedbackQueueIsRaceFreeUnderDrain) {
    const auto handle = handle_for(30);
    moonlight_controller_feedback_queue_t queue {handle};
    constexpr std::uint64_t event_count = 512;
    std::atomic<bool> producer_done = false;
    std::vector<std::uint64_t> observed;

    auto producer = std::async(std::launch::async, [&]() {
      for (std::uint64_t sequence = 1; sequence <= event_count; ++sequence) {
        const auto result = queue.push(feedback_for(
          handle,
          sequence,
          static_cast<std::uint32_t>(sequence % 2),
          static_cast<std::uint16_t>(sequence),
          static_cast<std::uint16_t>(sequence + 1)
        ));
        EXPECT_TRUE(
          result == controller_feedback_queue_result_e::enqueued ||
          result == controller_feedback_queue_result_e::coalesced
        );
      }
      producer_done = true;
    });
    auto consumer = std::async(std::launch::async, [&]() {
      while (!producer_done || queue.pending() != 0) {
        if (const auto item = queue.pop()) {
          observed.push_back(item->source_sequence);
        } else {
          std::this_thread::yield();
        }
      }
    });
    producer.get();
    consumer.get();

    ASSERT_FALSE(observed.empty());
    EXPECT_TRUE(std::is_sorted(observed.begin(), observed.end()));
    EXPECT_EQ(observed.back(), event_count);
    EXPECT_EQ(queue.last_sequence(), event_count);
    EXPECT_EQ(queue.pending(), 0U);
  }
}  // namespace
