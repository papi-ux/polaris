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
    std::string sysfs_path;
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

  enum class isolation_mode_e {
    disabled,
    strict_bwrap,
    best_effort_sdl,
    unavailable,
  };

  struct strict_isolation_options_t {
    bool bubblewrap_available = false;
    bool bubblewrap_usable = true;
    std::string bubblewrap_path = "bwrap";
  };

  struct strict_gamepad_isolation_plan_t {
    isolation_mode_e mode = isolation_mode_e::disabled;
    std::string bubblewrap_path = "bwrap";
    std::vector<std::string> allowed_nodes;
    std::vector<std::string> masked_sysfs_paths;
    std::vector<std::string> allowed_sysfs_paths;
    sdl_hint_plan_t fallback_sdl;
    std::string reason;

    bool strict_applied() const {
      return mode == isolation_mode_e::strict_bwrap;
    }

    bool fallback_applied() const {
      return mode == isolation_mode_e::best_effort_sdl && fallback_sdl.applied();
    }
  };

  device_classification_e classify_device(const device_snapshot_t &device);
  std::vector<classified_device_t> classify_devices(const std::vector<device_snapshot_t> &devices);
  sdl_hint_plan_t build_sdl_hint_plan(const std::vector<classified_device_t> &devices);
  strict_gamepad_isolation_plan_t build_strict_gamepad_isolation_plan(
    const std::vector<classified_device_t> &devices,
    const std::vector<std::string> &registered_virtual_nodes,
    const strict_isolation_options_t &options
  );
  std::string command_with_sdl_env_prefix(const std::string &command, const sdl_hint_plan_t &plan);
  std::string command_with_headless_gamepad_isolation(const std::string &command, const strict_gamepad_isolation_plan_t &plan);

  void register_virtual_gamepad_nodes(int gamepad_index, const std::vector<std::string> &nodes);
  void unregister_virtual_gamepad_nodes(int gamepad_index);
  std::vector<std::string> registered_virtual_gamepad_nodes();

  strict_isolation_options_t detect_strict_isolation_options();
  strict_gamepad_isolation_plan_t prepare_headless_labwc_launch();
  std::vector<device_snapshot_t> enumerate_visible_gamepads();
}  // namespace platf::gamepad::isolation
