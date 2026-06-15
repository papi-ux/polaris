/**
 * @file src/platform/linux/input/inputtino_gamepad_isolation.h
 * @brief Linux gamepad visibility classification for headless labwc launch diagnostics.
 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace platf::gamepad::isolation {
  enum class device_classification_e {
    ignored,
    polaris_virtual,
    host_visible,
  };

  struct device_snapshot_t {
    std::string event_node;
    std::string name;
    std::optional<uint16_t> vendor_id;
    std::optional<uint16_t> product_id;
    std::optional<uint16_t> version;
    std::string phys;
    std::string uniq;
    bool gamepad_class = false;
  };

  struct classified_device_t {
    device_snapshot_t device;
    device_classification_e classification = device_classification_e::ignored;
  };

  struct sdl_hint_plan_t {
    std::map<std::string, std::string> env;
    std::string reason;

    bool applied() const {
      return !env.empty();
    }
  };

  device_classification_e classify_device(const device_snapshot_t &device);
  std::vector<classified_device_t> classify_devices(const std::vector<device_snapshot_t> &devices);
  sdl_hint_plan_t build_sdl_hint_plan(const std::vector<classified_device_t> &devices);
  std::string command_with_sdl_env_prefix(const std::string &command, const sdl_hint_plan_t &plan);

  sdl_hint_plan_t prepare_headless_labwc_launch();
  std::vector<device_snapshot_t> enumerate_visible_gamepads();
}  // namespace platf::gamepad::isolation
