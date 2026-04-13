/**
 * @file src/wol.h
 * @brief Declarations for Wake-on-LAN support.
 *
 * Provides the ability to send WoL magic packets to wake sleeping
 * network devices by their MAC address.
 */
#pragma once

// standard includes
#include <string>

namespace wol {

  /**
   * @brief Send a Wake-on-LAN magic packet to the given MAC address.
   * @param mac_address The target MAC address in "AA:BB:CC:DD:EE:FF" format.
   * @return true on success, false on failure.
   */
  bool send_magic_packet(const std::string &mac_address);

}  // namespace wol
