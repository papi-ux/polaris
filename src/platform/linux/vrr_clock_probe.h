/**
 * @file src/platform/linux/vrr_clock_probe.h
 * @brief The kernel side of vrr_clock::system_t: SO_PEERCRED, /proc namespace and start-time reads.
 */
#pragma once

#include <string_view>

#include "src/vrr_clock_proof.h"

namespace vrr_clock::linux_probe {

  /** Real kernel facts. Every read is read-only and fails closed by returning nothing. */
  system_t &real_system();

  /**
   * @brief Parse field 22 (starttime) from /proc/<pid>/stat text.
   *
   * comm (field 2) may contain spaces and parentheses, so parsing starts after the last ')'.
   */
  std::optional<std::uint64_t> parse_starttime(std::string_view stat);

}  // namespace vrr_clock::linux_probe
