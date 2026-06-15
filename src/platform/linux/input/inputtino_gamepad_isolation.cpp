/**
 * @file src/platform/linux/input/inputtino_gamepad_isolation.cpp
 * @brief Linux gamepad visibility classification for headless labwc launch diagnostics.
 */
#include "inputtino_gamepad_isolation.h"
#include "src/logging.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <libevdev/libevdev.h>
#include <set>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace platf::gamepad::isolation {
namespace {
  using vid_pid_pair_t = std::pair<uint16_t, uint16_t>;

  constexpr std::string_view xbox_virtual_name = "Sunshine X-Box One (virtual) pad";
  constexpr std::string_view switch_virtual_name = "Sunshine Nintendo (virtual) pad";
  constexpr std::string_view ps5_virtual_name = "Sunshine PS5 (virtual) pad";
  constexpr std::string_view xbox_private_prefix = "02:00:00:00:10:";
  constexpr std::string_view ds5_private_prefix = "02:00:00:00:00:";

  std::string safe_string(const char *value) {
    return value ? std::string(value) : std::string {};
  }

  std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return value;
  }

  bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
  }

  bool has_private_virtual_prefix(const device_snapshot_t &device) {
    const auto phys = lowercase(device.phys);
    const auto uniq = lowercase(device.uniq);

    return starts_with(phys, xbox_private_prefix) ||
           starts_with(uniq, xbox_private_prefix) ||
           starts_with(phys, ds5_private_prefix) ||
           starts_with(uniq, ds5_private_prefix);
  }

  bool has_known_virtual_name(std::string_view name) {
    return name == xbox_virtual_name ||
           name == switch_virtual_name ||
           name == ps5_virtual_name;
  }

  std::optional<vid_pid_pair_t> vid_pid(const device_snapshot_t &device) {
    if (!device.vendor_id || !device.product_id) {
      return std::nullopt;
    }

    return vid_pid_pair_t {*device.vendor_id, *device.product_id};
  }

  std::string format_vid_pid(const vid_pid_pair_t &pair) {
    std::ostringstream out;
    out << std::hex << std::nouppercase << std::setfill(static_cast<char>(48));
    out << "0x" << std::setw(4) << pair.first << "/0x" << std::setw(4) << pair.second;
    return out.str();
  }

  std::string join_vid_pid_pairs(const std::set<vid_pid_pair_t> &pairs) {
    std::string joined;
    for (const auto &pair : pairs) {
      if (!joined.empty()) {
        joined += ",";
      }
      joined += format_vid_pid(pair);
    }
    return joined;
  }

  std::string shell_quote(std::string_view value) {
    const auto quote = std::string(1, static_cast<char>(39));
    const auto slash = std::string(1, static_cast<char>(92));
    std::string result = quote;

    for (char ch : value) {
      if (ch == static_cast<char>(39)) {
        result += quote;
        result += slash;
        result += quote;
        result += quote;
      } else {
        result += ch;
      }
    }

    result += quote;
    return result;
  }

  bool libevdev_has_any_key(const libevdev *device, const std::array<int, 12> &codes) {
    return std::any_of(codes.begin(), codes.end(), [device](int code) {
      return libevdev_has_event_code(device, EV_KEY, code) != 0;
    });
  }

  bool libevdev_has_any_abs(const libevdev *device, const std::array<int, 8> &codes) {
    return std::any_of(codes.begin(), codes.end(), [device](int code) {
      return libevdev_has_event_code(device, EV_ABS, code) != 0;
    });
  }

  bool libevdev_looks_like_gamepad(const libevdev *device) {
    static constexpr std::array<int, 12> gamepad_keys {
      BTN_GAMEPAD,
      BTN_SOUTH,
      BTN_EAST,
      BTN_NORTH,
      BTN_WEST,
      BTN_A,
      BTN_B,
      BTN_X,
      BTN_Y,
      BTN_TL,
      BTN_TR,
      BTN_START,
    };

    static constexpr std::array<int, 8> gamepad_axes {
      ABS_X,
      ABS_Y,
      ABS_RX,
      ABS_RY,
      ABS_Z,
      ABS_RZ,
      ABS_HAT0X,
      ABS_HAT0Y,
    };

    return libevdev_has_any_key(device, gamepad_keys) &&
           libevdev_has_any_abs(device, gamepad_axes);
  }

  std::optional<uint16_t> positive_id(int value) {
    if (value <= 0) {
      return std::nullopt;
    }
    return static_cast<uint16_t>(value);
  }
}  // namespace

device_classification_e classify_device(const device_snapshot_t &device) {
  if (!device.gamepad_class) {
    return device_classification_e::ignored;
  }

  if (has_known_virtual_name(device.name) || has_private_virtual_prefix(device)) {
    return device_classification_e::polaris_virtual;
  }

  return device_classification_e::host_visible;
}

std::vector<classified_device_t> classify_devices(const std::vector<device_snapshot_t> &devices) {
  std::vector<classified_device_t> classified;
  classified.reserve(devices.size());

  for (const auto &device : devices) {
    classified.push_back(classified_device_t {device, classify_device(device)});
  }

  return classified;
}

sdl_hint_plan_t build_sdl_hint_plan(const std::vector<classified_device_t> &devices) {
  std::vector<device_snapshot_t> polaris_virtual;
  std::vector<device_snapshot_t> host_visible;

  for (const auto &classified : devices) {
    if (classified.classification == device_classification_e::polaris_virtual) {
      polaris_virtual.push_back(classified.device);
    } else if (classified.classification == device_classification_e::host_visible) {
      host_visible.push_back(classified.device);
    }
  }

  sdl_hint_plan_t plan;

  if (host_visible.empty()) {
    plan.reason = "gamepad_isolation: no host physical controllers visible; SDL hints not needed";
    return plan;
  }

  if (polaris_virtual.empty()) {
    plan.reason = "gamepad_isolation: host physical controllers are visible, but no Polaris virtual gamepad was identified; SDL hints skipped";
    return plan;
  }

  std::set<vid_pid_pair_t> virtual_pairs;
  std::set<vid_pid_pair_t> host_pairs;

  for (const auto &device : polaris_virtual) {
    const auto pair = vid_pid(device);
    if (!pair) {
      plan.reason = "gamepad_isolation: Polaris virtual gamepad is missing VID/PID; SDL hints skipped";
      return plan;
    }
    virtual_pairs.insert(*pair);
  }

  for (const auto &device : host_visible) {
    const auto pair = vid_pid(device);
    if (!pair) {
      plan.reason = "gamepad_isolation: host-visible controller is missing VID/PID; SDL hints skipped";
      return plan;
    }
    host_pairs.insert(*pair);
  }

  for (const auto &pair : host_pairs) {
    if (virtual_pairs.contains(pair)) {
      plan.reason = "gamepad_isolation: host-visible controller VID/PID overlaps Polaris virtual gamepad; SDL hints skipped";
      return plan;
    }
  }

  plan.env["SDL_GAMECONTROLLER_IGNORE_DEVICES"] = join_vid_pid_pairs(host_pairs);
  plan.env["SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"] = join_vid_pid_pairs(virtual_pairs);
  plan.reason = "gamepad_isolation: exporting best-effort SDL controller privacy hints; strict /dev/input and hidraw isolation is not active";
  return plan;
}

std::string command_with_sdl_env_prefix(const std::string &command, const sdl_hint_plan_t &plan) {
  if (!plan.applied() || command.empty()) {
    return command;
  }

  std::string prefixed = "env ";
  for (const auto &[key, value] : plan.env) {
    prefixed += key;
    prefixed += "=";
    prefixed += shell_quote(value);
    prefixed += " ";
  }

  prefixed += command;
  return prefixed;
}

sdl_hint_plan_t prepare_headless_labwc_launch() {
  const auto devices = classify_devices(enumerate_visible_gamepads());

  std::size_t polaris_virtual_count = 0;
  std::size_t host_visible_count = 0;
  for (const auto &device : devices) {
    if (device.classification == device_classification_e::polaris_virtual) {
      ++polaris_virtual_count;
    } else if (device.classification == device_classification_e::host_visible) {
      ++host_visible_count;
    }
  }

  BOOST_LOG(info) << "gamepad_isolation: headless labwc visible controllers: polaris_virtual="
                  << polaris_virtual_count
                  << " host_visible="
                  << host_visible_count;

  auto plan = build_sdl_hint_plan(devices);
  if (host_visible_count > 0 && !plan.applied()) {
    BOOST_LOG(warning) << plan.reason;
  } else {
    BOOST_LOG(info) << plan.reason;
  }
  BOOST_LOG(info) << "gamepad_isolation: diagnostics and SDL hints are best-effort only; strict /dev/input and hidraw hiding is not active";
  return plan;
}

std::vector<device_snapshot_t> enumerate_visible_gamepads() {
  std::vector<device_snapshot_t> devices;

  auto *dir = opendir("/dev/input");
  if (!dir) {
    return devices;
  }

  while (auto *entry = readdir(dir)) {
    const std::string name = safe_string(entry->d_name);
    if (!starts_with(name, "event")) {
      continue;
    }

    const auto event_node = std::string("/dev/input/") + name;
    const int fd = open(event_node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }

    libevdev *evdev = nullptr;
    if (libevdev_new_from_fd(fd, &evdev) < 0 || !evdev) {
      close(fd);
      continue;
    }

    device_snapshot_t snapshot;
    snapshot.event_node = event_node;
    snapshot.name = safe_string(libevdev_get_name(evdev));
    snapshot.vendor_id = positive_id(libevdev_get_id_vendor(evdev));
    snapshot.product_id = positive_id(libevdev_get_id_product(evdev));
    snapshot.version = positive_id(libevdev_get_id_version(evdev));
    snapshot.phys = safe_string(libevdev_get_phys(evdev));
    snapshot.uniq = safe_string(libevdev_get_uniq(evdev));
    snapshot.gamepad_class = libevdev_looks_like_gamepad(evdev);

    if (snapshot.gamepad_class) {
      devices.push_back(std::move(snapshot));
    }

    libevdev_free(evdev);
    close(fd);
  }

  closedir(dir);
  return devices;
}
}  // namespace platf::gamepad::isolation
