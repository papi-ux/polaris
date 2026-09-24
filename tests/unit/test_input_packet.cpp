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
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
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

TEST(ControllerTouchPoint, ASteamControllersTwoTouchpadsTakeAHalfEach) {
  // The DualSense Polaris emulates has one touchpad with two finger slots. A Steam Controller's
  // left pad takes the left half and slot 0, its right pad the right half and slot 1, so a game
  // sees both thumbs at once instead of one pad's touch replacing the other's.
  const auto left = input::controller_touch_point(0.5f, 7, 0, true);
  const auto right = input::controller_touch_point(0.5f, 7, 1, true);
  EXPECT_FLOAT_EQ(left.x, 0.25f);
  EXPECT_EQ(left.finger, 0U);
  EXPECT_FLOAT_EQ(right.x, 0.75f);
  EXPECT_EQ(right.finger, 1U);
  EXPECT_FLOAT_EQ(input::controller_touch_point(0.0f, 0, 1, true).x, 0.5f);
  EXPECT_FLOAT_EQ(input::controller_touch_point(1.0f, 0, 1, true).x, 1.0f);
  // Only indices 0 and 1 exist; anything else is the right pad rather than off the touchpad.
  EXPECT_EQ(input::controller_touch_point(0.5f, 0, 9, true).finger, 1U);

  // A pad with one touchpad keeps the whole width and its own pointer ids.
  const auto single = input::controller_touch_point(0.5f, 1, 0, false);
  EXPECT_FLOAT_EQ(single.x, 0.5f);
  EXPECT_EQ(single.finger, 1U);
  // An older client leaves the byte at zero, which reads as the one touchpad it has.
  EXPECT_FLOAT_EQ(input::controller_touch_point(0.3f, 0, 0, false).x, 0.3f);
}

namespace {
  std::string input_source_for_contract() {
    const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/input.cpp";
    std::ifstream in(path);
    if (!in) {
      return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  std::string between_markers(const std::string &text, const std::string &begin, const std::string &end) {
    const auto from = text.find(begin);
    if (from == std::string::npos) {
      return {};
    }
    const auto to = text.find(end, from);
    return text.substr(from, to == std::string::npos ? std::string::npos : to - from);
  }
}  // namespace

/**
 * A session that ends must not leave anything held down on the host.
 *
 * All three of these were true at once, and the first one is what a user sees: a selection rectangle
 * on the desktop that follows the pointer and cannot be dismissed, after a stream ends.
 *
 * Read as source because releasing input needs a real input device to observe, and the thing that was
 * wrong is which lines run rather than what any one of them computes.
 */
TEST(InputResetContract, TheDeferredLeftButtonReleaseIsFlushedRatherThanCancelled) {
  const auto source = input_source_for_contract();
  ASSERT_FALSE(source.empty());
  const auto reset = between_markers(source, "void reset(std::shared_ptr<input_t> &input)", "class deinit_t");
  ASSERT_FALSE(reset.empty());

  EXPECT_NE(reset.find("left_release_pending"), std::string::npos)
    << "the ten millisecond left button release is cancelled without being sent, and the bookkeeping "
       "already says the button is up, so nothing releases it";
  EXPECT_NE(reset.find("platf::button_mouse(platf_input, BUTTON_LEFT, true)"), std::string::npos);
}

TEST(InputResetContract, EveryButtonTheProtocolCarriesIsTracked) {
  const auto source = input_source_for_contract();
  ASSERT_FALSE(source.empty());
  EXPECT_NE(source.find("std::array<std::uint8_t, BUTTON_X2 + 1> mouse_press"), std::string::npos)
    << "a button above the end of the array is never recorded as held, so it is never released";
  // The reason the size is written as a name rather than a number.
  EXPECT_EQ(BUTTON_X2, 0x05);
}

TEST(InputResetContract, KeysAreReleasedThroughTheSameMappingTheyWerePressedThrough) {
  const auto source = input_source_for_contract();
  ASSERT_FALSE(source.empty());
  const auto reset = between_markers(source, "void reset(std::shared_ptr<input_t> &input)", "class deinit_t");
  ASSERT_FALSE(reset.empty());

  EXPECT_NE(reset.find("map_keycode(vk_from_kpid(kp.first) & 0x00FF)"), std::string::npos)
    << "a keybinding sends one key to the host and this releases another, leaving the mapped key held";
}
