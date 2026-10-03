/**
 * @file src/app_launch_as.h
 * @brief Exact per-app display vocabulary, shared by parsing and write validation.
 */
#pragma once

#include <array>
#include <string_view>

namespace proc {
  inline constexpr std::array<std::string_view, 7> launch_as_values {
    "host_default", "headless_stream", "windowed_stream", "gamescope_stream",
    "host_virtual_display", "desktop_takeover", "desktop_display",
  };
  inline constexpr std::array<std::string_view, 4> launch_as_basis_values {
    "none", "desktop-mirror", "virtual-display", "both",
  };
}
