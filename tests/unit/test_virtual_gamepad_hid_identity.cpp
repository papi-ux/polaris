/**
 * @file tests/unit/test_virtual_gamepad_hid_identity.cpp
 * @brief Pins the HID versions the virtual pads advertise, because those decide
 *        which controller mapping SDL applies to them.
 */
// standard includes
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

// lib includes
#include <gtest/gtest.h>

// local includes
#include "src/platform/linux/input/inputtino_gamepad.h"

namespace {
  // The versions SDL's controller database holds for each DualSense entry, as
  // userspace sees them once hid-playstation has patched what the device
  // advertised. The two differ by bus, and picking the wrong one matches
  // neither entry.
  constexpr std::uint16_t sdl_dualsense_bluetooth_entry = 0x8100;
  constexpr std::uint16_t sdl_dualsense_usb_entry = 0x8111;

  // The Switch Pro pad has its own pair of entries in the same database, and its
  // USB one carries the same number as the DualSense USB entry. That coincidence
  // is what made the value look safe to copy from one pad to the other.
  constexpr std::uint16_t sdl_switch_pro_usb_entry = 0x8111;

  std::uint16_t as_userspace_sees_it(std::uint16_t advertised) {
    return advertised | platf::gamepad::hid_playstation_version_patch;
  }

  std::string read_source_file(std::string_view relative_path) {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path;
    std::ifstream input {path};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
  }
}  // namespace

TEST(VirtualGamepadHidIdentity, TheDualSenseLandsOnSDLsBluetoothMapping) {
  const auto seen = as_userspace_sees_it(platf::gamepad::ds5_hid_version);

  EXPECT_EQ(seen, sdl_dualsense_bluetooth_entry)
    << "inputtino creates the DualSense on the Bluetooth bus, and SDL keys its mapping on the "
       "version hid-playstation leaves behind. Anything but 0x8100 misses the Bluetooth entry.";

  EXPECT_NE(seen, sdl_dualsense_usb_entry)
    << "0x8111 is the USB DualSense's patched version. Advertised over Bluetooth it matches no "
       "entry at all, and SDL falls back to the pre-hid-playstation mapping, which rotates the "
       "face buttons and swaps the triggers with the right stick.";
}

TEST(VirtualGamepadHidIdentity, TheSwitchProKeepsTheUsbVersionItIsCreatedWith) {
  // This pad is built on uinput, on the USB bus, so the USB value is the right
  // one here. It is the same number that breaks the DualSense, so it is pinned
  // to stop the two being reconciled in the wrong direction.
  EXPECT_EQ(platf::gamepad::switch_pro_hid_version, sdl_switch_pro_usb_entry)
    << "The Switch Pro pad is created on the USB bus, where 0x8111 is what SDL expects. "
       "Reconciling it with the DualSense would break the pad that already works.";
}

TEST(VirtualGamepadHidIdentity, TheDualSenseCallSiteUsesTheNamedVersion) {
  const auto source = read_source_file("src/platform/linux/input/inputtino_gamepad.cpp");
  ASSERT_FALSE(source.empty()) << "could not read inputtino_gamepad.cpp";

  EXPECT_NE(source.find(".version = ds5_hid_version"), std::string::npos)
    << "create_ds5 must pass ds5_hid_version so the reasoning recorded beside the constant "
       "travels with the value. A bare literal here is how the USB version reached the "
       "Bluetooth pad in the first place.";

  EXPECT_EQ(source.find(".version = 0x8111"), std::string::npos)
    << "0x8111 must not come back as a bare literal. It is right for one pad and wrong for "
       "the other, so it is the one version number that has to say which bus it means.";
}
