/**
 * @file src/platform/linux/input/inputtino_wayland_virtual_input.h
 * @brief Labwc-local Wayland virtual input routing for Linux.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <string_view>

// local includes
#include "src/platform/common.h"

namespace platf {

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
