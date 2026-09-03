/**
 * @file src/platform/linux/input/inputtino_common.h
 * @brief Declarations for inputtino common input handling.
 */
#pragma once

#include <mutex>

// standard includes
#include <optional>

// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "input_seat_isolation.h"
#include "inputtino_wayland_virtual_input.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/gamepad_feedback_router.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf {

  using joypads_t = std::variant<inputtino::XboxOneJoypad, inputtino::SwitchJoypad, inputtino::PS5Joypad>;

  struct joypad_state {
    std::unique_ptr<joypads_t> joypad;
    std::shared_ptr<gamepad_feedback_router_t> feedback_router;
    gamepad_feedback_msg_t last_rumble;
    gamepad_feedback_msg_t last_rgb_led;
  };

  /**
   * @brief Log once when a seat isolation request did not reach the kernel.
   *
   * Creating the device is not the same as isolating it. The uinput backend can
   * accept device_phys and never write it, which leaves the operator with a
   * setting that saves, reads back as enabled, and does nothing.
   */
  template<class Device>
  void warn_if_seat_isolation_inert(const inputtino::Result<Device> &device, std::string_view label) {
    if (!config::input.client_keyboard_mouse_seat_isolation || !device) {
      return;
    }

    const auto status = input_isolation::inspect_devices(true, (*device).get_nodes());
    const auto warning_text = input_isolation::isolation_warning(status, label);
    if (warning_text.empty()) {
      return;
    }

    static std::once_flag reported;
    std::call_once(reported, [&warning_text]() {
      BOOST_LOG(warning) << warning_text;
    });
  }

  struct input_raw_t {
    input_raw_t():
        gamepads(MAX_GAMEPADS) {
    }

    ~input_raw_t() = default;

    inputtino::Result<inputtino::Mouse> *ensure_mouse() {
      if (!mouse) {
        mouse.emplace(inputtino::Mouse::create({
          .name = "Polaris Mouse passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
          .device_phys = input_isolation::client_input_phys_marker(
            config::input.client_keyboard_mouse_seat_isolation,
            "mouse"
          ),
        }));
        if (!*mouse) {
          BOOST_LOG(warning) << "Unable to create virtual mouse: " << mouse->getErrorMessage();
        }
        warn_if_seat_isolation_inert(*mouse, "mouse");
      }
      return &*mouse;
    }

    inputtino::Result<inputtino::Keyboard> *ensure_keyboard() {
      if (!keyboard) {
        keyboard.emplace(inputtino::Keyboard::create({
          .name = "Polaris Keyboard passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
          .device_phys = input_isolation::client_input_phys_marker(
            config::input.client_keyboard_mouse_seat_isolation,
            "keyboard"
          ),
        }));
        if (!*keyboard) {
          BOOST_LOG(warning) << "Unable to create virtual keyboard: " << keyboard->getErrorMessage();
        }
        warn_if_seat_isolation_inert(*keyboard, "keyboard");
      }
      return &*keyboard;
    }

    wayland_virtual_input_t wayland_input;

    // All devices are wrapped in Result because it might be that we aren't able to create them (ex: udev permission denied)
    std::optional<inputtino::Result<inputtino::Mouse>> mouse;
    std::optional<inputtino::Result<inputtino::Keyboard>> keyboard;

    /**
     * A list of gamepads that are currently connected.
     * The pointer is shared because that state will be shared with background threads that deal with rumble and LED
     */
    std::vector<std::shared_ptr<joypad_state>> gamepads;
  };

  struct client_input_raw_t: public client_input_t {
    client_input_raw_t(input_t &input):
        touch(inputtino::TouchScreen::create({
          .name = "Touch passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
          .device_phys = input_isolation::client_input_phys_marker(
            config::input.client_keyboard_mouse_seat_isolation,
            "touch"
          ),
        })),
        pen(inputtino::PenTablet::create({
          .name = "Pen passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
          .device_phys = input_isolation::client_input_phys_marker(
            config::input.client_keyboard_mouse_seat_isolation,
            "pen"
          ),
        })) {
      global = (input_raw_t *) input.get();
      if (!touch) {
        BOOST_LOG(warning) << "Unable to create virtual touch screen: " << touch.getErrorMessage();
      }
      warn_if_seat_isolation_inert(touch, "touch screen");
      if (!pen) {
        BOOST_LOG(warning) << "Unable to create virtual pen tablet: " << pen.getErrorMessage();
      }
      warn_if_seat_isolation_inert(pen, "pen tablet");
    }

    input_raw_t *global;

    // Device state and handles for pen and touch input must be stored in the per-client
    // input context, because each connected client may be sending their own independent
    // pen/touch events. To maintain separation, we expose separate pen and touch devices
    // for each client.
    inputtino::Result<inputtino::TouchScreen> touch;
    inputtino::Result<inputtino::PenTablet> pen;
  };

  inline float deg2rad(float degree) {
    return degree * (M_PI / 180.f);
  }
}  // namespace platf
