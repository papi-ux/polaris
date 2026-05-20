/**
 * @file src/platform/linux/input/inputtino_wayland_virtual_input.h
 * @brief Labwc-local Wayland virtual input routing for Linux.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// local includes
#include "src/platform/common.h"

namespace platf {

#if defined(POLARIS_BUILD_WAYLAND_VIRTUAL_INPUT)
  namespace wayland_virtual_input {
    struct modifier_state_t {
      std::uint32_t depressed;
      std::uint32_t latched;
      std::uint32_t locked;
      std::uint32_t group;
    };

#ifdef POLARIS_TESTS
    std::optional<modifier_state_t> modifier_state_after_key_events_for_tests(
      const std::vector<std::pair<int, bool>> &evdev_key_events,
      std::string_view layout
    );
#endif
  }  // namespace wayland_virtual_input
#endif

  class wayland_virtual_input_t {
  public:
    wayland_virtual_input_t();
    ~wayland_virtual_input_t();

    wayland_virtual_input_t(wayland_virtual_input_t &&) = delete;
    wayland_virtual_input_t(const wayland_virtual_input_t &) = delete;
    wayland_virtual_input_t &operator=(wayland_virtual_input_t &&) = delete;
    wayland_virtual_input_t &operator=(const wayland_virtual_input_t &) = delete;

    bool move(int delta_x, int delta_y);
    bool move_abs(const touch_port_t &touch_port, float x, float y);
    bool button(int button, bool release);
    bool scroll(int high_res_distance);
    bool hscroll(int high_res_distance);
    bool keyboard_update(std::uint16_t modcode, bool release);
    bool unicode(std::string_view hex_unicode);
    bool should_block_host_fallback();
    void reset();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };

}  // namespace platf
