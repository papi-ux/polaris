/**
 * @file tests/unit/test_input.cpp
 * @brief Tests for retained stream input and virtual gamepad lifecycle behavior.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// moonlight-common-c includes
extern "C" {
#include <moonlight-common-c/src/Input.h>
}

// local includes
#include "src/config.h"
#include "src/input.h"
#include "src/utility.h"

namespace {
  /**
   * @brief Protocol metadata for a fixed-size input packet.
   */
  struct input_packet_spec_t {
    std::uint32_t magic;  ///< Packet type identifier.
    std::size_t size;  ///< Complete fixed packet size.
  };

  /**
   * @brief Create raw input bytes with a caller-selected header and buffer size.
   *
   * @param magic Packet type identifier.
   * @param declared_size Packet size declared after the size field.
   * @param actual_size Number of bytes available in the packet buffer.
   * @return Raw packet bytes.
   */
  std::vector<std::uint8_t> make_input_packet(std::uint32_t magic, std::uint32_t declared_size, std::size_t actual_size) {
    std::vector<std::uint8_t> packet(actual_size);
    const NV_INPUT_HEADER header {
      util::endian::big(declared_size),
      util::endian::little(magic),
    };
    if (!packet.empty()) {
      std::memcpy(packet.data(), &header, std::min(packet.size(), sizeof(header)));
    }
    return packet;
  }

}  // namespace

TEST(InputPacketValidationTest, RejectsEveryBufferShorterThanTheHeader) {
  for (std::size_t actual_size = 0; actual_size < sizeof(NV_INPUT_HEADER); ++actual_size) {
    const auto packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), actual_size);
    EXPECT_FALSE(input::is_valid_input_packet_for_tests(packet)) << "actual_size=" << actual_size;
  }
}

TEST(InputPacketValidationTest, RejectsDeclaredSizesOutsideTheAvailableBuffer) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  for (std::uint32_t declared_size = 0; declared_size < sizeof(std::uint32_t); ++declared_size) {
    const auto packet = make_input_packet(unknown_magic, declared_size, sizeof(NV_INPUT_HEADER));
    EXPECT_FALSE(input::is_valid_input_packet_for_tests(packet)) << "declared_size=" << declared_size;
  }

  const auto truncated_packet = make_input_packet(unknown_magic, sizeof(std::uint32_t) + 1, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::is_valid_input_packet_for_tests(truncated_packet));

  const auto overflowing_size = make_input_packet(
    unknown_magic,
    std::numeric_limits<std::uint32_t>::max(),
    sizeof(NV_INPUT_HEADER)
  );
  EXPECT_FALSE(input::is_valid_input_packet_for_tests(overflowing_size));
}

TEST(InputPacketValidationTest, ValidatesUnicodePacketBoundsWithoutOverflow) {
  const auto empty_packet = make_input_packet(UTF8_TEXT_EVENT_MAGIC, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::is_valid_input_packet_for_tests(empty_packet));

  constexpr std::uint32_t maximum_declared_size = sizeof(std::uint32_t) + UTF8_TEXT_EVENT_MAX_COUNT;
  const auto maximum_packet = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size,
    sizeof(std::uint32_t) + maximum_declared_size
  );
  EXPECT_TRUE(input::is_valid_input_packet_for_tests(maximum_packet));

  const auto oversized_text = make_input_packet(
    UTF8_TEXT_EVENT_MAGIC,
    maximum_declared_size + 1,
    sizeof(std::uint32_t) + maximum_declared_size + 1
  );
  EXPECT_FALSE(input::is_valid_input_packet_for_tests(oversized_text));

  const auto truncated_text = make_input_packet(UTF8_TEXT_EVENT_MAGIC, maximum_declared_size, sizeof(NV_INPUT_HEADER));
  EXPECT_FALSE(input::is_valid_input_packet_for_tests(truncated_text));
}

TEST(InputPacketValidationTest, EnforcesEveryFixedPacketSize) {
  constexpr std::array packet_specs {
    input_packet_spec_t {MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_MOVE_ABS_MAGIC, sizeof(NV_ABS_MOUSE_MOVE_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET)},
    input_packet_spec_t {SCROLL_MAGIC_GEN5, sizeof(NV_SCROLL_PACKET)},
    input_packet_spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET)},
    input_packet_spec_t {KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {KEY_UP_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET)},
    input_packet_spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET)},
    input_packet_spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET)},
    input_packet_spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_ARRIVAL_MAGIC, sizeof(SS_CONTROLLER_ARRIVAL_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET)},
    input_packet_spec_t {SS_CONTROLLER_BATTERY_MAGIC, sizeof(SS_CONTROLLER_BATTERY_PACKET)},
  };

  for (const auto &[magic, size] : packet_specs) {
    const auto declared_size = static_cast<std::uint32_t>(size - sizeof(std::uint32_t));
    const auto valid_packet = make_input_packet(magic, declared_size, size);
    EXPECT_TRUE(input::is_valid_input_packet_for_tests(valid_packet)) << "magic=" << magic;

    const auto trailing_bytes = make_input_packet(magic, declared_size, size + 8);
    EXPECT_TRUE(input::is_valid_input_packet_for_tests(trailing_bytes)) << "magic=" << magic;

    const auto wrong_declared_size = make_input_packet(magic, declared_size - 1, size);
    EXPECT_FALSE(input::is_valid_input_packet_for_tests(wrong_declared_size)) << "magic=" << magic;

    const auto oversized_declared_size = make_input_packet(magic, declared_size + 1, size + 1);
    EXPECT_FALSE(input::is_valid_input_packet_for_tests(oversized_declared_size)) << "magic=" << magic;

    const auto truncated_packet = make_input_packet(magic, declared_size, size - 1);
    EXPECT_FALSE(input::is_valid_input_packet_for_tests(truncated_packet)) << "magic=" << magic;
  }
}

TEST(InputPacketValidationTest, PreservesUnknownPacketHandlingWithinDeclaredBounds) {
  constexpr std::uint32_t unknown_magic = 0x12345678;
  const auto packet = make_input_packet(unknown_magic, sizeof(std::uint32_t), sizeof(NV_INPUT_HEADER));
  EXPECT_TRUE(input::is_valid_input_packet_for_tests(packet));
}


namespace {
  class InputPacketAdmissionTest: public testing::Test {
  protected:
    void TearDown() override {
      // The test environment never starts task_pool. Discard queued work so
      // admission can be observed without opening or sending to input devices.
      while (task_pool.pop()) {}
    }
  };
}

TEST_F(InputPacketAdmissionTest, ValidatesBeforePermissionsAndQueueing) {
  constexpr std::array permissions {
    crypto::PERM::_no, crypto::PERM::input_mouse, crypto::PERM::_all_inputs,
  };
  for (const auto permission : permissions) {
    auto queue = input::alloc_queue_for_tests();
    for (std::size_t size = 0; size < sizeof(NV_REL_MOUSE_MOVE_PACKET); ++size) {
      auto packet = make_input_packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - 4, size);
      input::passthrough(queue, std::move(packet), permission);
    }
    EXPECT_EQ(input::queued_input_packet_count_for_tests(queue), 0);
    auto valid = make_input_packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - 4, sizeof(NV_REL_MOUSE_MOVE_PACKET));
    input::passthrough(queue, std::move(valid), permission);
    const auto expected = permission == crypto::PERM::_no ? 0 : 1;
    EXPECT_EQ(input::queued_input_packet_count_for_tests(queue), expected);
    auto truncated = make_input_packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - 4, sizeof(NV_REL_MOUSE_MOVE_PACKET) - 1);
    input::passthrough(queue, std::move(truncated), permission);
    EXPECT_EQ(input::queued_input_packet_count_for_tests(queue), expected);
  }
}

TEST_F(InputPacketAdmissionTest, PreservesEachExtensionPermissionBoundary) {
  struct spec_t {
    std::uint32_t magic;
    std::size_t size;
    crypto::PERM permission;
  };
  constexpr std::array specs {
    spec_t {KEY_DOWN_EVENT_MAGIC, sizeof(NV_KEYBOARD_PACKET), crypto::PERM::input_kbd},
    spec_t {UTF8_TEXT_EVENT_MAGIC, sizeof(NV_UNICODE_PACKET), crypto::PERM::input_kbd},
    spec_t {SS_HSCROLL_MAGIC, sizeof(SS_HSCROLL_PACKET), crypto::PERM::input_mouse},
    spec_t {SS_TOUCH_MAGIC, sizeof(SS_TOUCH_PACKET), crypto::PERM::input_touch},
    spec_t {SS_PEN_MAGIC, sizeof(SS_PEN_PACKET), crypto::PERM::input_pen},
    spec_t {MULTI_CONTROLLER_MAGIC_GEN5, sizeof(NV_MULTI_CONTROLLER_PACKET), crypto::PERM::input_controller},
    spec_t {SS_CONTROLLER_ARRIVAL_MAGIC, sizeof(SS_CONTROLLER_ARRIVAL_PACKET), crypto::PERM::input_controller},
    spec_t {SS_CONTROLLER_TOUCH_MAGIC, sizeof(SS_CONTROLLER_TOUCH_PACKET), crypto::PERM::input_controller},
    spec_t {SS_CONTROLLER_MOTION_MAGIC, sizeof(SS_CONTROLLER_MOTION_PACKET), crypto::PERM::input_controller},
    spec_t {SS_CONTROLLER_BATTERY_MAGIC, sizeof(SS_CONTROLLER_BATTERY_PACKET), crypto::PERM::input_controller},
  };
  for (const auto &[magic, size, permission] : specs) {
    auto queue = input::alloc_queue_for_tests();
    auto valid = make_input_packet(magic, size - 4, size);
    input::passthrough(queue, std::move(valid), permission);
    EXPECT_EQ(input::queued_input_packet_count_for_tests(queue), 1) << magic;
    auto denied = make_input_packet(magic, size - 4, size);
    const auto other_permission = permission == crypto::PERM::input_mouse ? crypto::PERM::input_kbd : crypto::PERM::input_mouse;
    input::passthrough(queue, std::move(denied), other_permission);
    EXPECT_EQ(input::queued_input_packet_count_for_tests(queue), 1) << magic;
  }
}

TEST(InputPacketValidationTest, PreservesBatchingAndStateChangeBoundaries) {
  auto relative = make_input_packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - 4, sizeof(NV_REL_MOUSE_MOVE_PACKET));
  NV_REL_MOUSE_MOVE_PACKET movement {};
  std::memcpy(&movement, relative.data(), sizeof(movement));
  movement.deltaX = util::endian::big<short>(2);
  std::memcpy(relative.data(), &movement, sizeof(movement));
  auto second = relative;
  EXPECT_TRUE(input::batch_input_packets_for_tests(relative, second));
  std::memcpy(&movement, relative.data(), sizeof(movement));
  EXPECT_EQ(util::endian::big(movement.deltaX), 4);

  auto button = make_input_packet(MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5, sizeof(NV_MOUSE_BUTTON_PACKET) - 4, sizeof(NV_MOUSE_BUTTON_PACKET));
  EXPECT_FALSE(input::batch_input_packets_for_tests(relative, button));
  auto truncated = second;
  truncated.pop_back();
  EXPECT_FALSE(input::batch_input_packets_for_tests(relative, truncated));
  EXPECT_FALSE(input::batch_input_packets_for_tests(truncated, relative));
}

TEST(InputPacketValidationTest, NeverBatchesOverflowingRelativeMotion) {
  auto first = make_input_packet(MOUSE_MOVE_REL_MAGIC_GEN5, sizeof(NV_REL_MOUSE_MOVE_PACKET) - 4, sizeof(NV_REL_MOUSE_MOVE_PACKET));
  NV_REL_MOUSE_MOVE_PACKET movement {};
  std::memcpy(&movement, first.data(), sizeof(movement));
  movement.deltaX = util::endian::big<short>(std::numeric_limits<short>::max());
  movement.deltaY = movement.deltaX;
  std::memcpy(first.data(), &movement, sizeof(movement));
  const auto unchanged = first;
  EXPECT_FALSE(input::batch_input_packets_for_tests(first, unchanged));
  EXPECT_EQ(first, unchanged);
}

TEST(InputPacketValidationTest, BatchesScrollOnlyWhenTheSumFits) {
  for (const auto magic : {SCROLL_MAGIC_GEN5, SS_HSCROLL_MAGIC}) {
    const auto size = magic == SCROLL_MAGIC_GEN5 ? sizeof(NV_SCROLL_PACKET) : sizeof(SS_HSCROLL_PACKET);
    auto packet = make_input_packet(magic, size - 4, size);
    // Both protocols store their first signed big-endian delta after the header.
    const auto amount = util::endian::big<short>(7);
    std::memcpy(packet.data() + sizeof(NV_INPUT_HEADER), &amount, sizeof(amount));
    const auto original = packet;
    ASSERT_TRUE(input::batch_input_packets_for_tests(packet, original));
    short sum;
    std::memcpy(&sum, packet.data() + sizeof(NV_INPUT_HEADER), sizeof(sum));
    EXPECT_EQ(util::endian::big(sum), 14);

    const auto maximum = util::endian::big<short>(std::numeric_limits<short>::max());
    std::memcpy(packet.data() + sizeof(NV_INPUT_HEADER), &maximum, sizeof(maximum));
    const auto unchanged = packet;
    EXPECT_FALSE(input::batch_input_packets_for_tests(packet, original));
    EXPECT_EQ(packet, unchanged);
  }
}
