/**
 * @file src/platform/linux/input/input_seat_isolation.h
 * @brief Seat isolation identities for the virtual keyboard, mouse, touch and pen.
 *
 * A Polaris host can run a private stream session while someone is using the
 * desktop session logged in at the machine. Without isolation both sessions see
 * the same devices: the client's typing and pointer movement drive the host
 * desktop as well as the stream.
 *
 * Tagging the virtual devices with a phys marker lets the bundled udev rules
 * assign them to a dedicated seat, which stops logind from handing them to the
 * session at seat0. This is the same mechanism the client gamepads use, and it
 * carries the same boundary: it separates seats, not processes. Applications
 * running under the same Unix account, and members of the `input` group, still
 * reach the devices directly.
 */
#pragma once

#include <string>
#include <string_view>

namespace platf::input_isolation {

  /// Marker written to the device's phys attribute; matched by 60-polaris.rules.
  constexpr std::string_view seat_isolated_phys_prefix = "polaris/client-input-seat-isolated/";

  /**
   * @brief Build the phys marker for a virtual input device.
   * @param enabled Whether client keyboard/mouse seat isolation is enabled.
   * @param device Short device kind, e.g. "mouse" or "keyboard".
   * @return The marker, or an empty string when isolation is disabled.
   */
  inline std::string client_input_phys_marker(bool enabled, std::string_view device) {
    if (!enabled) {
      return {};
    }

    return std::string {seat_isolated_phys_prefix} + std::string {device};
  }

}  // namespace platf::input_isolation
