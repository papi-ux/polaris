/**
 * @file src/platform/linux/input/inputtino_common.h
 * @brief Declarations for inputtino common input handling.
 */
#pragma once

// standard includes
#include <optional>

// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_wayland_virtual_input.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf {

  using joypads_t = std::variant<inputtino::XboxOneJoypad, inputtino::SwitchJoypad, inputtino::PS5Joypad>;

  struct joypad_state {
    std::unique_ptr<joypads_t> joypad;
    gamepad_feedback_msg_t last_rumble;
    gamepad_feedback_msg_t last_rgb_led;
  };

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
        }));
        if (!*mouse) {
          BOOST_LOG(warning) << "Unable to create virtual mouse: " << mouse->getErrorMessage();
        }
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
        }));
        if (!*keyboard) {
          BOOST_LOG(warning) << "Unable to create virtual keyboard: " << keyboard->getErrorMessage();
        }
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
        })),
        pen(inputtino::PenTablet::create({
          .name = "Pen passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })) {
      global = (input_raw_t *) input.get();
      if (!touch) {
        BOOST_LOG(warning) << "Unable to create virtual touch screen: " << touch.getErrorMessage();
      }
      if (!pen) {
        BOOST_LOG(warning) << "Unable to create virtual pen tablet: " << pen.getErrorMessage();
      }
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
