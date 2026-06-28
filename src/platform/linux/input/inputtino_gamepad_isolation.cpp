/**
 * @file src/platform/linux/input/inputtino_gamepad_isolation.cpp
 * @brief Linux gamepad visibility classification for headless labwc launch diagnostics.
 */
#include "inputtino_gamepad_isolation.h"
#include "src/logging.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <iomanip>
#include <limits.h>
#include <libevdev/libevdev.h>
#include <mutex>
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
  constexpr std::string_view polaris_xbox_virtual_name = "Polaris X-Box One (virtual) pad";
  constexpr std::string_view polaris_switch_virtual_name = "Polaris Nintendo (virtual) pad";
  constexpr std::string_view polaris_ps5_virtual_name = "Polaris PS5 (virtual) pad";
  constexpr std::string_view xbox_private_prefix = "02:00:00:00:10:";
  constexpr std::string_view switch_private_prefix = "02:00:00:00:20:";
  constexpr std::string_view ds5_private_prefix = "02:00:00:00:00:";

  std::mutex virtual_nodes_mutex;
  std::map<int, std::vector<std::string>> virtual_nodes_by_gamepad;

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
           starts_with(phys, switch_private_prefix) ||
           starts_with(uniq, switch_private_prefix) ||
           starts_with(phys, ds5_private_prefix) ||
           starts_with(uniq, ds5_private_prefix);
  }

  bool has_known_virtual_name(std::string_view name) {
    return name == xbox_virtual_name ||
           name == switch_virtual_name ||
           name == ps5_virtual_name ||
           name == polaris_xbox_virtual_name ||
           name == polaris_switch_virtual_name ||
           name == polaris_ps5_virtual_name;
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

  bool has_numeric_suffix(std::string_view value, std::string_view prefix) {
    if (!starts_with(value, prefix) || value.size() == prefix.size()) {
      return false;
    }

    return std::all_of(value.begin() + static_cast<std::ptrdiff_t>(prefix.size()), value.end(), [](unsigned char ch) {
      return std::isdigit(ch) != 0;
    });
  }

  bool is_virtual_gamepad_node(std::string_view node) {
    return has_numeric_suffix(node, "/dev/input/event") ||
           has_numeric_suffix(node, "/dev/input/js") ||
           has_numeric_suffix(node, "/dev/hidraw");
  }

  bool is_hidraw_node(std::string_view node) {
    return starts_with(node, "/dev/hidraw");
  }

  std::vector<std::string> sanitized_virtual_nodes(const std::vector<std::string> &nodes) {
    std::set<std::string> unique;
    for (const auto &node : nodes) {
      if (is_virtual_gamepad_node(node)) {
        unique.insert(node);
      }
    }

    return std::vector<std::string>(unique.begin(), unique.end());
  }

  bool is_safe_sysfs_path(std::string_view path) {
    if (!starts_with(path, "/sys/devices/")) {
      return false;
    }

    return std::all_of(path.begin(), path.end(), [](unsigned char ch) {
      return std::isalnum(ch) != 0 ||
             ch == 47 ||
             ch == 45 ||
             ch == 95 ||
             ch == 46 ||
             ch == 58;
    });
  }

  std::optional<std::string> host_input_sysfs_root(std::string_view sysfs_path) {
    if (!is_safe_sysfs_path(sysfs_path)) {
      return std::nullopt;
    }

    constexpr std::string_view input_dir_marker = "/input/";
    const auto input_dir_pos = sysfs_path.find(input_dir_marker);
    if (input_dir_pos == std::string_view::npos) {
      return std::nullopt;
    }

    const auto input_device_start = input_dir_pos + input_dir_marker.size();
    if (!starts_with(sysfs_path.substr(input_device_start), "input")) {
      return std::nullopt;
    }

    auto input_device_end = sysfs_path.find("/", input_device_start);
    if (input_device_end == std::string_view::npos) {
      input_device_end = sysfs_path.size();
    }

    const auto root = std::string(sysfs_path.substr(0, input_device_end));
    if (root.find("/sys/devices/virtual/input/") == 0) {
      return std::nullopt;
    }
    return root;
  }

  std::vector<std::string> host_input_sysfs_mask_paths(const std::vector<classified_device_t> &devices) {
    std::set<std::string> paths;
    for (const auto &classified : devices) {
      if (classified.classification != device_classification_e::host_visible) {
        continue;
      }
      auto root = host_input_sysfs_root(classified.device.sysfs_path);
      if (root) {
        paths.insert(*root);
      }
    }
    return std::vector<std::string>(paths.begin(), paths.end());
  }

  std::string event_sysfs_path(std::string_view event_node) {
    constexpr std::string_view input_prefix = "/dev/input/";
    if (!starts_with(event_node, input_prefix)) {
      return {};
    }

    const auto sysfs = std::string("/sys/class/input/") + std::string(event_node.substr(input_prefix.size()));
    char resolved[PATH_MAX];
    if (realpath(sysfs.c_str(), resolved) == nullptr) {
      return {};
    }
    return std::string(resolved);
  }

  std::string find_executable(std::string_view name) {
    if (name.empty()) {
      return {};
    }

    const auto requested = std::string(name);
    if (requested.find("/") != std::string::npos) {
      return access(requested.c_str(), X_OK) == 0 ? requested : std::string {};
    }

    const char *path_env = std::getenv("PATH");
    const std::string path = path_env && *path_env ? path_env : "/usr/local/bin:/usr/bin:/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
      const auto end = path.find(static_cast<char>(58), start);
      auto dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (dir.empty()) {
        dir = ".";
      }

      const auto candidate = dir + "/" + requested;
      if (access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }

      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }

    return {};
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

  bool probe_bubblewrap_usable(std::string_view bubblewrap_path) {
    if (bubblewrap_path.empty()) {
      return false;
    }

    std::string command = shell_quote(bubblewrap_path);
    command += " --bind / /";
    command += " --dev /dev";
    command += " --proc /proc";
    command += " --ro-bind /sys /sys";
    command += " --tmpfs /dev/input";
    command += " --dir /dev/input/by-id";
    command += " --dir /dev/input/by-path";
    command += " --tmpfs /run/udev";
    command += " --tmpfs /sys/class/input";
    command += " --tmpfs /sys/class/hidraw";
    command += " --tmpfs /sys/bus/hid/devices";
    command += " -- /usr/bin/true >/dev/null 2>&1";
    return std::system(command.c_str()) == 0;
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


strict_gamepad_isolation_plan_t build_strict_gamepad_isolation_plan(
  const std::vector<classified_device_t> &devices,
  const std::vector<std::string> &registered_virtual_nodes,
  const strict_isolation_options_t &options
) {
  strict_gamepad_isolation_plan_t plan;
  plan.bubblewrap_path = options.bubblewrap_path.empty() ? "bwrap" : options.bubblewrap_path;
  plan.fallback_sdl = build_sdl_hint_plan(devices);
  plan.allowed_nodes = sanitized_virtual_nodes(registered_virtual_nodes);
  plan.masked_sysfs_paths = host_input_sysfs_mask_paths(devices);

  if (!options.bubblewrap_available || !options.bubblewrap_usable) {
    const auto unavailable_reason = options.bubblewrap_available ?
                                      std::string("bubblewrap probe failed") :
                                      std::string("bubblewrap is unavailable");
    if (plan.fallback_sdl.applied()) {
      plan.mode = isolation_mode_e::best_effort_sdl;
      plan.reason = "gamepad_isolation: strict device isolation is not active because " + unavailable_reason + "; using best-effort SDL hints";
    } else {
      plan.mode = isolation_mode_e::unavailable;
      plan.reason = "gamepad_isolation: strict device isolation is not active because " + unavailable_reason;
    }
    return plan;
  }

  plan.mode = isolation_mode_e::strict_bwrap;
  if (plan.allowed_nodes.empty()) {
    plan.reason = "gamepad_isolation: strict bubblewrap device isolation active; no registered Polaris virtual gamepad nodes are exposed";
  } else {
    plan.reason = "gamepad_isolation: strict bubblewrap device isolation active; binding " +
                  std::to_string(plan.allowed_nodes.size()) +
                  " registered Polaris virtual gamepad node(s)";
  }
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

std::string command_with_headless_gamepad_isolation(const std::string &command, const strict_gamepad_isolation_plan_t &plan) {
  if (command.empty()) {
    return command;
  }

  if (!plan.strict_applied()) {
    return command_with_sdl_env_prefix(command, plan.fallback_sdl);
  }

  std::string wrapped = shell_quote(plan.bubblewrap_path.empty() ? "bwrap" : plan.bubblewrap_path);
  wrapped += " --bind / /";
  wrapped += " --dev /dev";
  wrapped += " --proc /proc";
  wrapped += " --ro-bind /sys /sys";
  wrapped += " --tmpfs /dev/input";
  wrapped += " --dir /dev/input/by-id";
  wrapped += " --dir /dev/input/by-path";
  wrapped += " --tmpfs /run/udev";
  wrapped += " --tmpfs /sys/class/input";
  wrapped += " --tmpfs /sys/class/hidraw";
  wrapped += " --tmpfs /sys/bus/hid/devices";

  for (const auto &path : plan.masked_sysfs_paths) {
    wrapped += " --tmpfs ";
    wrapped += path;
  }

  static constexpr std::array<std::string_view, 11> runtime_device_roots {
    "/dev/dri",
    "/dev/snd",
    "/dev/kfd",
    "/dev/nvidiactl",
    "/dev/nvidia0",
    "/dev/nvidia-uvm",
    "/dev/nvidia-uvm-tools",
    "/dev/nvidia-modeset",
    "/dev/nvidia-caps",
    "/dev/shm",
    "/dev/pts",
  };

  for (const auto node : runtime_device_roots) {
    wrapped += " --dev-bind-try ";
    wrapped += node;
    wrapped += " ";
    wrapped += node;
  }

  for (const auto &node : plan.allowed_nodes) {
    wrapped += is_hidraw_node(node) ? " --dev-bind-try " : " --dev-bind ";
    wrapped += node;
    wrapped += " ";
    wrapped += node;
  }

  wrapped += " -- /bin/sh -lc ";
  wrapped += shell_quote(command);
  return wrapped;
}

void register_virtual_gamepad_nodes(int gamepad_index, const std::vector<std::string> &nodes) {
  auto sanitized = sanitized_virtual_nodes(nodes);
  std::lock_guard lock(virtual_nodes_mutex);
  if (sanitized.empty()) {
    virtual_nodes_by_gamepad.erase(gamepad_index);
    return;
  }
  virtual_nodes_by_gamepad[gamepad_index] = std::move(sanitized);
}

void unregister_virtual_gamepad_nodes(int gamepad_index) {
  std::lock_guard lock(virtual_nodes_mutex);
  virtual_nodes_by_gamepad.erase(gamepad_index);
}

std::vector<std::string> registered_virtual_gamepad_nodes() {
  std::vector<std::string> nodes;
  std::lock_guard lock(virtual_nodes_mutex);
  for (const auto &[_, device_nodes] : virtual_nodes_by_gamepad) {
    nodes.insert(nodes.end(), device_nodes.begin(), device_nodes.end());
  }
  return sanitized_virtual_nodes(nodes);
}

strict_isolation_options_t detect_strict_isolation_options() {
  strict_isolation_options_t options;
  options.bubblewrap_path = find_executable("bwrap");
  options.bubblewrap_available = !options.bubblewrap_path.empty();
  if (options.bubblewrap_path.empty()) {
    options.bubblewrap_path = "bwrap";
  }
  options.bubblewrap_usable = options.bubblewrap_available && probe_bubblewrap_usable(options.bubblewrap_path);
  return options;
}

strict_gamepad_isolation_plan_t prepare_headless_labwc_launch() {
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

  const auto registered_nodes = registered_virtual_gamepad_nodes();
  auto plan = build_strict_gamepad_isolation_plan(devices, registered_nodes, detect_strict_isolation_options());
  BOOST_LOG(info) << "gamepad_isolation: registered Polaris virtual nodes="
                  << registered_nodes.size();

  if (plan.strict_applied()) {
    BOOST_LOG(info) << plan.reason;
  } else {
    BOOST_LOG(warning) << plan.reason;
    if (!plan.fallback_sdl.reason.empty()) {
      BOOST_LOG(info) << plan.fallback_sdl.reason;
    }
    BOOST_LOG(info) << "gamepad_isolation: fallback is best-effort only; strict /dev/input and hidraw hiding is not active";
  }
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
    snapshot.sysfs_path = event_sysfs_path(event_node);
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
