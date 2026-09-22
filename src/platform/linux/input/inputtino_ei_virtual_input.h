/**
 * @file src/platform/linux/input/inputtino_ei_virtual_input.h
 * @brief Emulated-input routing into a Polaris-owned gamescope.
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <string_view>

// local includes
#include "src/platform/common.h"

namespace platf {

  /**
   * @brief Mouse and keyboard injection into gamescope over libei.
   *
   * The Wayland route in wayland_virtual_input_t exists only for the private
   * labwc compositor: it speaks zwlr_virtual_pointer_v1 to labwc's own socket,
   * and gamescope's wlserver creates no virtual-pointer or virtual-keyboard
   * manager to talk to. gamescope_stream therefore fell through to host uinput,
   * which a headless gamescope has no libinput seat to read — leaving gamepads,
   * which Steam reads from /dev/input itself, as the only working input.
   *
   * gamescope does run an EIS server (src/InputEmulation.cpp) offering pointer,
   * absolute pointer, button, scroll and keyboard, and feeds each event
   * straight into wlserver. That is the same path XWayland's XTEST support
   * already takes on our behalf, so speaking to it directly drops the X hop
   * along with X's habit of exiting the process when a display goes away.
   */
  class ei_virtual_input_t {
  public:
    ei_virtual_input_t();
    ~ei_virtual_input_t();

    ei_virtual_input_t(ei_virtual_input_t &&) = delete;
    ei_virtual_input_t(const ei_virtual_input_t &) = delete;
    ei_virtual_input_t &operator=(ei_virtual_input_t &&) = delete;
    ei_virtual_input_t &operator=(const ei_virtual_input_t &) = delete;

    bool move(int delta_x, int delta_y);
    bool move_abs(const touch_port_t &touch_port, float x, float y);
    bool button(int button, bool release);
    bool scroll(int high_res_distance);
    bool hscroll(int high_res_distance);
    bool keyboard_update(std::uint16_t modcode, bool release);
    bool unicode(std::string_view hex_unicode);
    bool should_block_host_fallback();
    /// Touch and pen have no libei route, so they are blocked only where host uinput cannot reach.
    bool should_block_host_touch();
    void reset();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };

}  // namespace platf
