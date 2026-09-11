/**
 * @file src/platform/linux/input/inputtino_gamepad.h
 * @brief Declarations for inputtino gamepad input handling.
 */
#pragma once

// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_common.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace platf::gamepad {

  /**
   * @brief Bit hid-playstation ORs into the HID version of a pad it has claimed.
   *
   * The kernel patches the version so userspace can tell hid-playstation's axis
   * and button mapping apart from hid-generic's.
   *
   * @see https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c
   */
  constexpr std::uint16_t hid_playstation_version_patch = 0x8000;

  /**
   * @brief HID version advertised for the virtual DualSense.
   *
   * inputtino creates the DualSense on the Bluetooth bus, where a real pad
   * reports 0x0100 and the kernel patch above turns that into 0x8100. SDL keys
   * its DualSense mappings on the patched value, and has separate entries for
   * Bluetooth (0x8100) and USB (0x8111). Advertising the USB value over
   * Bluetooth matches neither, so SDL falls back to a pre-hid-playstation
   * mapping that rotates the face buttons and swaps the triggers with the right
   * stick.
   */
  constexpr std::uint16_t ds5_hid_version = 0x0100;

  /**
   * @brief HID version advertised for the virtual Switch Pro pad.
   *
   * inputtino builds this one on uinput, on the USB bus, where 0x8111 is the
   * value SDL expects. The same number over Bluetooth is what breaks the
   * DualSense above, so the two are kept apart deliberately.
   */
  constexpr std::uint16_t switch_pro_hid_version = 0x8111;

  enum ControllerType {
    XboxOneWired,  ///< Xbox One Wired Controller
    DualSenseWired,  ///< DualSense Wired Controller
    SwitchProWired  ///< Switch Pro Wired Controller
  };

  int alloc(input_raw_t *raw, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);

  void rebind_feedback(input_raw_t *raw, int nr, feedback_queue_t feedback_queue);

  void free(input_raw_t *raw, int nr);

  void update(input_raw_t *raw, int nr, const gamepad_state_t &gamepad_state);

  void touch(input_raw_t *raw, const gamepad_touch_t &touch);

  void motion(input_raw_t *raw, const gamepad_motion_t &motion);

  void battery(input_raw_t *raw, const gamepad_battery_t &battery);

  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input);
}  // namespace platf::gamepad
