/**
 * @file src/platform/linux/virtual_display.cpp
 * @brief Linux virtual display creation and management implementation.
 *
 * Implements virtual display support using multiple backends:
 *   1. EVDI - Creates true virtual DRM connectors via the EVDI kernel module
 *   2. Wayland compositor - Creates headless outputs via compositor-specific commands
 *   3. kscreen-doctor - Manages existing physical displays as a fallback
 *
 * The EVDI backend dynamically loads libevdi via dlopen to avoid a hard
 * dependency. If the EVDI module is not loaded or libevdi is not installed,
 * we fall back to compositor or kscreen-doctor approaches.
 */

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <errno.h>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <signal.h>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <vector>

// platform includes
#include <dlfcn.h>
#include <nlohmann/json.hpp>
#include <sys/wait.h>
#include <sys/stat.h>

// local includes
#include "misc.h"
#include "virtual_display.h"
#if defined(POLARIS_BUILD_WAYLAND) && defined(POLARIS_BUILD_PORTAL)
  #include "kwin_virtual_output.h"
  #define POLARIS_HAS_KWIN_VIRTUAL_OUTPUT 1
#endif
#include "src/platform/common.h"
#include "src/config.h"
#include "src/logging.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace virtual_display {
  namespace {
    constexpr auto backend_detection_cache_ttl = 30s;
    std::mutex backend_detection_mutex;
    std::optional<backend_e> cached_backend;
    std::chrono::steady_clock::time_point cached_backend_time {};
    backend_detection_log_cache_t backend_detection_log_cache;

    fs::path persisted_state_path() {
      return platf::appdata() / "virtual_display_state.json";
    }

    bool pid_is_alive(pid_t pid) {
      if (pid <= 0) {
        return false;
      }

      if (kill(pid, 0) == 0) {
        return true;
      }

      return errno == EPERM;
    }

    std::mutex persisted_state_mutex;
    // One creation transaction across stream and web UI callers. The lock is
    // held through output discovery/configuration and persisted ownership
    // publication so Sway before/after attribution cannot overlap in-process.
    static std::mutex creation_mutex;

    const char *backend_persist_name(backend_e backend) {
      switch (backend) {
        case backend_e::EVDI:
          return "evdi";
        case backend_e::WAYLAND_WLR:
          return "wayland_wlr";
        case backend_e::KSCREEN_DOCTOR:
          return "kscreen_doctor";
        case backend_e::KWIN_VIRTUAL_OUTPUT:
          return "kwin_virtual_output";
        case backend_e::NONE:
        default:
          return "none";
      }
    }

    backend_e backend_from_persist_name(std::string_view name) {
      if (name == "evdi"sv) {
        return backend_e::EVDI;
      }
      if (name == "wayland_wlr"sv) {
        return backend_e::WAYLAND_WLR;
      }
      if (name == "kscreen_doctor"sv) {
        return backend_e::KSCREEN_DOCTOR;
      }
      if (name == "kwin_virtual_output"sv) {
        return backend_e::KWIN_VIRTUAL_OUTPUT;
      }
      return backend_e::NONE;
    }

    std::optional<kscreen_output_state_t> parse_kscreen_state(const nlohmann::json &node) {
      if (!node.is_object() ||
          !node.contains("name") || !node["name"].is_string() ||
          !node.contains("enabled") || !node["enabled"].is_boolean() ||
          !node.contains("current_mode_id") || !node["current_mode_id"].is_string() ||
          !node.contains("priority") || !node["priority"].is_number_integer()) {
        return std::nullopt;
      }
      const int priority = node["priority"].get<int>();
      const auto name = node["name"].get<std::string>();
      if (name.empty() || priority < 0 || priority > 100) {
        return std::nullopt;
      }
      return kscreen_output_state_t {
        .name = name,
        .enabled = node["enabled"].get<bool>(),
        .current_mode_id = node["current_mode_id"].get<std::string>(),
        .priority = priority,
      };
    }

    nlohmann::json serialize_kscreen_state(const kscreen_output_state_t &state) {
      return {
        {"name", state.name},
        {"enabled", state.enabled},
        {"current_mode_id", state.current_mode_id},
        {"priority", state.priority},
      };
    }

    std::optional<persisted_display_t> parse_persisted_entry(const nlohmann::json &node) {
      if (!node.is_object()) {
        return std::nullopt;
      }

      persisted_display_t entry;
      entry.owner_pid = node.value("pid", 0);
      entry.display.device_path = node.value("device_path", "");
      entry.display.output_name = node.value("output_name", "");
      entry.display.width = node.value("width", 0);
      entry.display.height = node.value("height", 0);
      entry.display.fps = node.value("fps", 0);
      entry.display.active = node.value("active", false);
      entry.display.backend = backend_from_persist_name(node.value("backend", "none"));
      if (node.contains("kscreen_output_before")) {
        entry.display.kscreen_output_before = parse_kscreen_state(node["kscreen_output_before"]);
      }
      if (node.contains("kscreen_primary_before")) {
        entry.display.kscreen_primary_before = parse_kscreen_state(node["kscreen_primary_before"]);
      }

      if (!entry.display.active || entry.display.backend == backend_e::NONE || entry.display.output_name.empty()) {
        return std::nullopt;
      }

      return entry;
    }

    nlohmann::json serialize_persisted_entry(const persisted_display_t &entry) {
      nlohmann::json node;
      node["pid"] = static_cast<int>(entry.owner_pid);
      node["device_path"] = entry.display.device_path;
      node["output_name"] = entry.display.output_name;
      node["width"] = entry.display.width;
      node["height"] = entry.display.height;
      node["fps"] = entry.display.fps;
      node["active"] = entry.display.active;
      node["backend"] = backend_persist_name(entry.display.backend);
      if (entry.display.kscreen_output_before) {
        node["kscreen_output_before"] = serialize_kscreen_state(*entry.display.kscreen_output_before);
      }
      if (entry.display.kscreen_primary_before) {
        node["kscreen_primary_before"] = serialize_kscreen_state(*entry.display.kscreen_primary_before);
      }
      return node;
    }

    /**
     * @brief Read every persisted display. Callers must hold persisted_state_mutex.
     */
    std::optional<std::vector<persisted_display_t>> load_persisted_entries() {
      const auto path = persisted_state_path();
      std::ifstream file(path);
      if (!file.is_open()) {
        std::error_code ec;
        if (!fs::exists(path, ec) && !ec) {
          return std::vector<persisted_display_t> {};
        }
        BOOST_LOG(error) << "Virtual display: persisted recovery state is unreadable at "sv << path.string();
        return std::nullopt;
      }

      std::ostringstream buffer;
      buffer << file.rdbuf();
      const auto serialized = buffer.str();
      const auto root = nlohmann::json::parse(serialized, nullptr, false);
      if (!root.is_object()) {
        BOOST_LOG(error) << "Virtual display: persisted recovery state is malformed; refusing to replace it"sv;
        return std::nullopt;
      }

      auto entries = parse_persisted_displays(serialized);
      const std::size_t expected_entries =
        root.contains("displays") && root["displays"].is_array() ?
          root["displays"].size() :
          1;
      if (entries.size() != expected_entries) {
        BOOST_LOG(error) << "Virtual display: persisted recovery state contains an unusable entry; refusing to replace it"sv;
        return std::nullopt;
      }
      return entries;
    }

    /**
     * @brief Replace the persisted state with these displays. Callers must hold persisted_state_mutex.
     */
    bool write_persisted_entries(const std::vector<persisted_display_t> &entries) {
      const auto path = persisted_state_path();
      std::error_code ec;

      const auto sync_parent = [&]() {
        const int dir_fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd < 0) {
          return false;
        }
        const bool synced = ::fsync(dir_fd) == 0;
        ::close(dir_fd);
        return synced;
      };

      if (entries.empty()) {
        const bool removed = fs::remove(path, ec);
        if (ec) {
          BOOST_LOG(error) << "Virtual display: failed to remove persisted recovery state: "sv << ec.message();
          return false;
        }
        return !removed || sync_parent();
      }

      fs::create_directories(path.parent_path(), ec);
      if (ec) {
        BOOST_LOG(error) << "Virtual display: failed to create recovery state directory: "sv << ec.message();
        return false;
      }

      nlohmann::json root;
      root["displays"] = nlohmann::json::array();
      for (const auto &entry : entries) {
        root["displays"].push_back(serialize_persisted_entry(entry));
      }

      // Rename over the live file rather than truncating it: a crash midway
      // through a write would otherwise strand every display it was tracking.
      auto temp_path = path;
      temp_path += ".tmp";
      const auto serialized = root.dump(2);
      const int fd = ::open(
        temp_path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        S_IRUSR | S_IWUSR
      );
      if (fd < 0 || ::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        if (fd >= 0) {
          ::close(fd);
        }
        BOOST_LOG(error) << "Virtual display: failed to open private recovery state at "sv << temp_path.string();
        return false;
      }
      std::size_t offset = 0;
      bool wrote = true;
      while (offset < serialized.size()) {
        const auto result = ::write(fd, serialized.data() + offset, serialized.size() - offset);
        if (result < 0 && errno == EINTR) {
          continue;
        }
        if (result <= 0) {
          wrote = false;
          break;
        }
        offset += static_cast<std::size_t>(result);
      }
      const bool synced = wrote && ::fsync(fd) == 0;
      const bool closed = ::close(fd) == 0;
      if (!synced || !closed) {
        BOOST_LOG(error) << "Virtual display: failed to durably write recovery state at "sv << temp_path.string();
        fs::remove(temp_path, ec);
        return false;
      }

      fs::rename(temp_path, path, ec);
      if (ec) {
        BOOST_LOG(error) << "Virtual display: failed to persist state at "sv << path.string()
                           << ": "sv << ec.message();
        fs::remove(temp_path, ec);
        return false;
      }
      if (!sync_parent()) {
        BOOST_LOG(error) << "Virtual display: recovery state rename was not durably committed"sv;
        return false;
      }
      return true;
    }

    bool record_persisted_display(const vdisplay_t &display, pid_t owner_pid = getpid()) {
      std::lock_guard lock {persisted_state_mutex};

      auto loaded = load_persisted_entries();
      if (!loaded) {
        return false;
      }
      auto &entries = *loaded;
      const auto incoming = persisted_display_t {owner_pid, display};
      const auto existing = std::find_if(entries.begin(), entries.end(), [&](const persisted_display_t &entry) {
        return entry.display.output_name == display.output_name;
      });
      if (existing != entries.end()) {
        auto existing_display = serialize_persisted_entry(*existing);
        auto incoming_display = serialize_persisted_entry(incoming);
        existing_display.erase("pid");
        incoming_display.erase("pid");
        const bool same_display = existing_display == incoming_display;
        if (!same_display || (existing->owner_pid != owner_pid && existing->owner_pid != 0)) {
          BOOST_LOG(error) << "Virtual display: refusing to replace existing recovery authority for ["sv
                           << display.output_name << ']';
          return false;
        }
        if (existing->owner_pid == owner_pid) {
          return true;
        }
        *existing = incoming;  // Promote a durable pre-mutation intent to its live owner.
      } else {
        entries.push_back(incoming);
      }
      return write_persisted_entries(entries);
    }

    bool forget_persisted_display(const vdisplay_t &display) {
      std::lock_guard lock {persisted_state_mutex};

      auto loaded = load_persisted_entries();
      if (!loaded) {
        return false;
      }
      auto &entries = *loaded;
      std::erase_if(entries, [&](const persisted_display_t &entry) {
        return entry.display.output_name == display.output_name;
      });
      return write_persisted_entries(entries);
    }

    void log_detected_backend(backend_e backend) {
      if (!backend_detection_log_cache.note(backend)) {
        return;
      }

      switch (backend) {
        case backend_e::EVDI:
          BOOST_LOG(info) << "Virtual display: EVDI backend available"sv;
          break;
        case backend_e::WAYLAND_WLR:
          BOOST_LOG(info) << "Virtual display: Wayland headless output backend available"sv;
          break;
        case backend_e::KSCREEN_DOCTOR:
          BOOST_LOG(info) << "Virtual display: kscreen-doctor backend available"sv;
          break;
        case backend_e::KWIN_VIRTUAL_OUTPUT:
          BOOST_LOG(info) << "Virtual display: KWin virtual output backend available"sv;
          break;
        case backend_e::NONE:
        default:
          BOOST_LOG(info) << "Virtual display: no backend available"sv;
          break;
      }
    }
  }  // namespace

  bool wayland_compositor_supports_exact_output_creation(std::string_view compositor) {
    return compositor == "hyprland"sv;
  }

  std::vector<persisted_display_t> parse_persisted_displays(std::string_view state_json) {
    std::vector<persisted_display_t> entries;

    const auto root = nlohmann::json::parse(state_json, nullptr, false);
    if (root.is_discarded()) {
      BOOST_LOG(warning) << "Virtual display: failed to parse persisted state"sv;
      return entries;
    }

    // A Polaris that predates the list wrote one bare display object per file.
    // Still read that shape so an upgrade cleans up what the old build left.
    if (root.is_object() && root.contains("displays") && root["displays"].is_array()) {
      for (const auto &node : root["displays"]) {
        if (auto entry = parse_persisted_entry(node)) {
          entries.push_back(std::move(*entry));
        }
      }
    } else if (auto entry = parse_persisted_entry(root)) {
      entries.push_back(std::move(*entry));
    }

    return entries;
  }

  bool persisted_display_is_stale(int owner_pid, int self_pid, bool owner_alive) {
    if (owner_pid == self_pid) {
      return false;
    }

    return owner_pid <= 0 || !owner_alive;
  }

  // ---------------------------------------------------------------------------
  // Utility: run a shell command and capture stdout
  // ---------------------------------------------------------------------------
  static std::string exec_cmd(const std::string &cmd) {
    std::array<char, 256> buffer;
    std::string result;
    auto pipe_closer = [](FILE *pipe) {
      if (pipe) {
        pclose(pipe);
      }
    };

    std::unique_ptr<FILE, decltype(pipe_closer)> pipe(popen(cmd.c_str(), "r"), pipe_closer);
    if (!pipe) {
      return {};
    }

    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
      result += buffer.data();
    }

    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
      result.pop_back();
    }

    return result;
  }

  static int exec_cmd_rc(const std::string &cmd) {
    int ret = std::system(cmd.c_str());
    return WEXITSTATUS(ret);
  }

  bool wayland_backend_probe_allowed(bool platform_reports_wayland, std::string_view wayland_display) {
    return platform_reports_wayland || !wayland_display.empty();
  }

  std::string hyprland_output_name_for_pid(int pid, int slot) {
    return "POLARIS-HEADLESS-" + std::to_string(pid) + "-" + std::to_string(slot);
  }

  std::optional<bool> hyprland_monitors_contain_output(
    std::string_view monitors_json,
    std::string_view output_name
  ) {
    if (output_name.empty()) {
      return std::nullopt;
    }
    const auto monitors = nlohmann::json::parse(monitors_json, nullptr, false);
    if (!monitors.is_array()) {
      return std::nullopt;
    }

    for (const auto &monitor : monitors) {
      if (!monitor.is_object() ||
          !monitor.contains("name") ||
          !monitor["name"].is_string()) {
        return std::nullopt;
      }
      if (monitor["name"].get_ref<const std::string &>() == output_name) {
        return true;
      }
    }

    return false;
  }

  std::optional<bool> evdi_connector_status_is_connected(std::string_view status) {
    if (status == "connected"sv) {
      return true;
    }
    if (status == "disconnected"sv) {
      return false;
    }
    return std::nullopt;
  }

  std::optional<hyprland_mode_t> hyprland_monitor_mode(
    std::string_view monitors_json,
    std::string_view output_name
  ) {
    const auto monitors = nlohmann::json::parse(monitors_json, nullptr, false);
    if (!monitors.is_array()) {
      return std::nullopt;
    }

    for (const auto &monitor : monitors) {
      if (!monitor.is_object() ||
          !monitor.contains("name") ||
          !monitor["name"].is_string() ||
          monitor["name"].get_ref<const std::string &>() != output_name) {
        continue;
      }

      hyprland_mode_t mode;
      if (monitor.contains("width") && monitor["width"].is_number()) {
        mode.width = monitor["width"].get<int>();
      }
      if (monitor.contains("height") && monitor["height"].is_number()) {
        mode.height = monitor["height"].get<int>();
      }
      if (monitor.contains("refreshRate") && monitor["refreshRate"].is_number()) {
        mode.refresh_hz = monitor["refreshRate"].get<double>();
      }

      // A named output with no usable geometry is not an answer, it is a gap.
      if (mode.width <= 0 || mode.height <= 0) {
        return std::nullopt;
      }
      return mode;
    }

    return std::nullopt;
  }

  std::optional<kscreen_output_state_t> kscreen_output_state_from_json(
    std::string_view output_json,
    std::string_view output_name
  ) {
    const auto root = nlohmann::json::parse(output_json, nullptr, false);
    if (!root.is_object() || !root.contains("outputs") || !root["outputs"].is_array()) {
      return std::nullopt;
    }

    for (const auto &output : root["outputs"]) {
      if (!output.is_object() ||
          !output.contains("name") || !output["name"].is_string() ||
          output["name"].get_ref<const std::string &>() != output_name ||
          !output.contains("enabled") || !output["enabled"].is_boolean() ||
          !output.contains("currentModeId") || !output["currentModeId"].is_string() ||
          !output.contains("priority") || !output["priority"].is_number_integer()) {
        continue;
      }

      const int priority = output["priority"].get<int>();
      if (priority < 0 || priority > 100) {
        return std::nullopt;
      }
      return kscreen_output_state_t {
        .name = std::string {output_name},
        .enabled = output["enabled"].get<bool>(),
        .current_mode_id = output["currentModeId"].get<std::string>(),
        .priority = priority,
      };
    }
    return std::nullopt;
  }

  std::string host_virtual_display_connector() {
    const auto &linux_display = config::video.linux_display;
    return !linux_display.streaming_output.empty() ?
             linux_display.streaming_output :
             linux_display.saved_streaming_output;
  }

  std::vector<std::string> kscreen_enable_args(
    std::string_view output,
    std::string_view mode,
    std::string_view primary_output
  ) {
    const auto prefix = "output."s + std::string {output};
    std::vector<std::string> args {"kscreen-doctor", prefix + ".enable"};
    if (!mode.empty()) {
      args.push_back(prefix + ".mode." + std::string {mode});
    }
    args.push_back(prefix + ".priority.1");
    if (!primary_output.empty() && primary_output != output) {
      args.push_back("output."s + std::string {primary_output} + ".priority.2");
    }
    return args;
  }

  bool evdi_output_name_is_proven(std::string_view output_name) {
    return !output_name.empty();
  }

  bool hyprland_output_is_polaris_owned(std::string_view output_name) {
    constexpr std::string_view prefix = "POLARIS-HEADLESS-";
    if (!output_name.starts_with(prefix)) {
      return false;
    }

    const auto all_digits = [](std::string_view value) {
      return !value.empty() && value.find_first_not_of("0123456789"sv) == std::string_view::npos;
    };

    // <pid> is still accepted alongside <pid>-<slot> so an output left behind by
    // a Polaris that predates the slot suffix stays removable after an upgrade.
    const auto suffix = output_name.substr(prefix.size());
    const auto separator = suffix.find('-');
    if (separator == std::string_view::npos) {
      return all_digits(suffix);
    }

    return all_digits(suffix.substr(0, separator)) && all_digits(suffix.substr(separator + 1));
  }

  std::optional<backend_preference_e> parse_backend_preference(std::string_view value) {
    if (value.empty() || value == "auto"sv) {
      return backend_preference_e::AUTO;
    }
    if (value == "evdi"sv) {
      return backend_preference_e::EVDI;
    }
    if (value == "kwin"sv) {
      return backend_preference_e::KWIN;
    }
    if (value == "wlr"sv) {
      return backend_preference_e::WLR;
    }
    if (value == "kscreen"sv) {
      return backend_preference_e::KSCREEN;
    }
    return std::nullopt;
  }

  std::string_view backend_preference_name(backend_preference_e preference) {
    switch (preference) {
      case backend_preference_e::EVDI:
        return "evdi"sv;
      case backend_preference_e::KWIN:
        return "kwin"sv;
      case backend_preference_e::WLR:
        return "wlr"sv;
      case backend_preference_e::KSCREEN:
        return "kscreen"sv;
      case backend_preference_e::AUTO:
      default:
        return "auto"sv;
    }
  }

  backend_e select_backend(backend_preference_e preference, const probe_snapshot_t &probe) {
    switch (preference) {
      case backend_preference_e::EVDI:
        return probe.evdi ? backend_e::EVDI : backend_e::NONE;
      case backend_preference_e::KWIN:
        return probe.kwin ? backend_e::KWIN_VIRTUAL_OUTPUT : backend_e::NONE;
      case backend_preference_e::WLR:
        return probe.wlr ? backend_e::WAYLAND_WLR : backend_e::NONE;
      case backend_preference_e::KSCREEN:
        return probe.kscreen ? backend_e::KSCREEN_DOCTOR : backend_e::NONE;
      case backend_preference_e::AUTO:
      default:
        break;
    }
    // A real new screen before a borrowed monitor: KWin and EVDI create one,
    // kscreen-doctor can only take over an output the user already has. On a
    // Plasma session KWin's own screen comes first: Polaris places it at scale 1,
    // ranks it last and moves new windows onto it, while an EVDI screen there
    // kept whatever layout KWin had stored for it (a 1.35 scale on the test
    // host) and the game opened on the primary monitor instead of the stream.
    if (probe.kwin) {
      return backend_e::KWIN_VIRTUAL_OUTPUT;
    }
    if (probe.evdi) {
      return backend_e::EVDI;
    }
    if (probe.wlr) {
      return backend_e::WAYLAND_WLR;
    }
    if (probe.kscreen) {
      return backend_e::KSCREEN_DOCTOR;
    }
    return backend_e::NONE;
  }

  std::string kwin_output_request_name(int slot) {
    return "polaris-" + std::to_string(slot);
  }

  std::string kwin_output_expected_name(std::string_view request_name) {
    return "Virtual-" + std::string {request_name};
  }

  bool kwin_output_is_polaris_owned(std::string_view output_name) {
    constexpr std::string_view prefix = "Virtual-polaris-";
    if (!output_name.starts_with(prefix)) {
      return false;
    }
    const auto slot = output_name.substr(prefix.size());
    return !slot.empty() && slot.find_first_not_of("0123456789"sv) == std::string_view::npos;
  }

  bool kwin_screencast_version_supported(std::uint32_t version) {
    return version >= 4;
  }

  std::optional<std::vector<kscreen_output_layout_t>> kscreen_layout_from_json(std::string_view json) {
    const auto root = nlohmann::json::parse(json, nullptr, false);
    if (!root.is_object() || !root.contains("outputs") || !root["outputs"].is_array()) {
      return std::nullopt;
    }

    // A field of the wrong type reads as absent rather than throwing: this parses
    // another program's output, and one odd field must not take the stream down.
    const auto int_field = [](const nlohmann::json &node, const char *key) {
      return node.contains(key) && node[key].is_number_integer() ? node[key].get<int>() : 0;
    };

    std::vector<kscreen_output_layout_t> layout;
    for (const auto &node : root["outputs"]) {
      if (!node.is_object() || !node.contains("name") || !node["name"].is_string()) {
        return std::nullopt;
      }
      kscreen_output_layout_t output;
      output.name = node["name"].get<std::string>();
      output.enabled = node.contains("enabled") && node["enabled"].is_boolean() && node["enabled"].get<bool>();
      output.priority = int_field(node, "priority");
      if (node.contains("scale") && node["scale"].is_number()) {
        output.scale = node["scale"].get<double>();
      }
      if (node.contains("pos") && node["pos"].is_object()) {
        output.x = int_field(node["pos"], "x");
        output.y = int_field(node["pos"], "y");
      }
      const std::string current = node.contains("currentModeId") && node["currentModeId"].is_string() ?
                                    node["currentModeId"].get<std::string>() :
                                    std::string {};
      if (node.contains("modes") && node["modes"].is_array()) {
        for (const auto &mode : node["modes"]) {
          if (!mode.is_object()) {
            continue;
          }
          if (mode.contains("name") && mode["name"].is_string()) {
            output.mode_names.push_back(mode["name"].get<std::string>());
          }
          const bool is_current = mode.contains("id") && mode["id"].is_string() &&
                                  !current.empty() && mode["id"].get_ref<const std::string &>() == current;
          if (!is_current || !mode.contains("size") || !mode["size"].is_object()) {
            continue;
          }
          output.mode_width = int_field(mode["size"], "width");
          output.mode_height = int_field(mode["size"], "height");
          if (mode.contains("refreshRate") && mode["refreshRate"].is_number()) {
            output.refresh_hz = mode["refreshRate"].get<double>();
          }
        }
      }
      layout.push_back(std::move(output));
    }
    return layout;
  }

  std::optional<std::string> kscreen_primary_output(const std::vector<kscreen_output_layout_t> &layout) {
    const kscreen_output_layout_t *primary = nullptr;
    for (const auto &output : layout) {
      if (!output.enabled || output.priority < 1) {
        continue;
      }
      if (!primary || output.priority < primary->priority) {
        primary = &output;
      }
    }
    if (!primary) {
      return std::nullopt;
    }
    return primary->name;
  }

  int kscreen_right_edge(const std::vector<kscreen_output_layout_t> &layout, std::string_view excluding) {
    int edge = 0;
    for (const auto &output : layout) {
      if (!output.enabled || output.name == excluding || output.mode_width <= 0) {
        continue;
      }
      const double scale = output.scale > 0.0 ? output.scale : 1.0;
      const int logical_width = static_cast<int>(output.mode_width / scale + 0.5);
      edge = std::max(edge, output.x + logical_width);
    }
    return edge;
  }

  std::string kwin_mode_name(int width, int height, int hz) {
    return std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(hz);
  }

  std::vector<std::string> kwin_custom_mode_args(std::string_view output, int width, int height, int hz) {
    return {
      "output."s + std::string {output} + ".addCustomMode." + std::to_string(width) + "." +
      std::to_string(height) + "." + std::to_string(hz * 1000) + ".full"
    };
  }

  std::vector<std::string> kwin_mode_args(std::string_view output, int width, int height, int hz) {
    return {"output."s + std::string {output} + ".mode." + kwin_mode_name(width, height, hz)};
  }

  std::vector<std::string> kwin_placement_args(std::string_view output, int x) {
    const auto prefix = "output."s + std::string {output};
    return {prefix + ".scale.1", prefix + ".position." + std::to_string(x) + ",0"};
  }

  std::vector<std::string> kwin_keep_positions_args(std::string_view output, const std::vector<kscreen_output_layout_t> &layout_before) {
    std::vector<std::string> args;
    for (const auto &screen : layout_before) {
      if (!screen.enabled || screen.name == output) {
        continue;
      }
      args.push_back("output."s + screen.name + ".position." + std::to_string(screen.x) + "," + std::to_string(screen.y));
    }
    return args;
  }

  bool kwin_positions_match(
    const std::vector<kscreen_output_layout_t> &layout,
    const std::vector<kscreen_output_layout_t> &layout_before,
    std::string_view output
  ) {
    for (const auto &before : layout_before) {
      if (!before.enabled || before.name == output) {
        continue;
      }
      const auto now = std::find_if(layout.begin(), layout.end(), [&](const auto &screen) {
        return screen.name == before.name;
      });
      if (now == layout.end() || !now->enabled || now->x != before.x || now->y != before.y) {
        return false;
      }
    }
    return true;
  }

  bool routes_to_stream_screen(std::string_view kwin_device_name) {
    // The names inputtino_common.h gives the devices. The absolute mouse is left
    // out: KWin places an absolute pointer over the whole workspace whatever its
    // outputName says, so pointing it at the stream screen changes nothing.
    return kwin_device_name == "Touch passthrough"sv ||
           kwin_device_name == "Pen passthrough"sv;
  }

  std::string input_event_name(std::string_view node) {
    const auto slash = node.find_last_of('/');
    const auto name = slash == std::string_view::npos ? node : node.substr(slash + 1);
    constexpr auto prefix = "event"sv;
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
      return {};
    }
    for (const char digit : name.substr(prefix.size())) {
      if (!std::isdigit(static_cast<unsigned char>(digit))) {
        return {};
      }
    }
    return std::string {name};
  }

  namespace {
    /**
     * Enabled screens in rank order; `except` is left out. KWin reports a
     * lone monitor with priority 0, so an unranked screen still counts: it
     * comes after the ranked ones, in the order kscreen lists it.
     */
    std::vector<std::string> ranked_screens(const std::vector<kscreen_output_layout_t> &layout, std::string_view except) {
      std::vector<const kscreen_output_layout_t *> ranked;
      for (const auto &output : layout) {
        if (output.enabled && output.name != except) {
          ranked.push_back(&output);
        }
      }
      const auto rank = [](const kscreen_output_layout_t *output) {
        return output->priority > 0 ? output->priority : std::numeric_limits<int>::max();
      };
      std::stable_sort(ranked.begin(), ranked.end(), [&](const auto *a, const auto *b) {
        return rank(a) < rank(b);
      });
      std::vector<std::string> names;
      names.reserve(ranked.size());
      for (const auto *output : ranked) {
        names.push_back(output->name);
      }
      return names;
    }
  }  // namespace

  std::vector<std::string> kwin_priority_args(std::string_view output, const std::vector<kscreen_output_layout_t> &layout_before) {
    // kscreen-doctor inserts each screen at the rank it is given, so naming
    // them in order from the first leaves exactly this order.
    std::vector<std::string> args;
    int rank = 1;
    for (const auto &name : ranked_screens(layout_before, output)) {
      args.push_back("output." + name + ".priority." + std::to_string(rank++));
    }
    args.push_back("output."s + std::string {output} + ".priority." + std::to_string(rank));
    return args;
  }

  bool kwin_ranking_matches(
    const std::vector<kscreen_output_layout_t> &layout,
    const std::vector<kscreen_output_layout_t> &layout_before,
    std::string_view output
  ) {
    auto expected = ranked_screens(layout_before, output);
    expected.emplace_back(output);
    return ranked_screens(layout, {}) == expected;
  }

  std::string kwin_unidentifiable_process_reason(int dumpable, bool holds_capabilities) {
    if (dumpable == 1) {
      return {};
    }
    if (holds_capabilities) {
      return "KWin does not offer its screencast protocol to Polaris because this Polaris process holds file "
             "capabilities (the cap_sys_admin that --enable-kms grants for KMS capture), and KWin cannot tell "
             "which program holds them. Set linux_virtual_display_backend to kwin, leave capture on auto or "
             "portal, and restart Polaris; it then drops them at start";
    }
    return "KWin does not offer its screencast protocol to Polaris because the process runs non-dumpable, so "
           "KWin cannot tell which program it is. A binary with file capabilities started under "
           "NoNewPrivileges does that; set linux_virtual_display_backend to kwin and restart Polaris, or remove "
           "the capability from the binary";
  }

  std::string kwin_window_follow_plugin_name(std::string_view output_name) {
    return "polaris-follow-" + std::string {output_name};
  }

  std::string kwin_window_follow_script(std::string_view output_name) {
    // The name goes in as a JSON string literal, which is also a valid
    // JavaScript one, so no output name can end the string early.
    const auto target = nlohmann::json(std::string {output_name}).dump();
    return "// Polaris: moves windows onto its Host Virtual Display screen while that screen exists.\n"
           "const target = " + target + ";\n"
           "// The desktop's own prompts are for whoever sits at the host.\n"
           "const hostPrompts = [\"polkit-kde-authentication-agent-1\", \"org.kde.polkit-kde-authentication-agent-1\",\n"
           "  \"ksshaskpass\", \"org.kde.ksshaskpass\", \"kwalletd5\", \"org.kde.kwalletd5\", \"kwalletd6\", \"org.kde.kwalletd6\",\n"
           "  \"krunner\", \"org.kde.krunner\", \"plasmashell\", \"org.kde.plasmashell\"];\n"
           "function outputNamed(name) {\n"
           "  const screens = workspace.screens;\n"
           "  for (let i = 0; i < screens.length; i++) { if (screens[i].name === name) return screens[i]; }\n"
           "  return null;\n"
           "}\n"
           "function isHostPrompt(window) {\n"
           "  return hostPrompts.indexOf(String(window.resourceClass || \"\")) >= 0 ||\n"
           "         hostPrompts.indexOf(String(window.desktopFileName || \"\")) >= 0;\n"
           "}\n"
           "workspace.windowAdded.connect(function (window) {\n"
           "  if (!window || !(window.normalWindow || window.dialog || window.splash)) return;\n"
           "  if (isHostPrompt(window)) return;\n"
           "  const screen = outputNamed(target);\n"
           "  if (!screen) return;\n"
           "  if (window.output !== screen) {\n"
           "    if (window.output && window.output.name.indexOf(\"Virtual-polaris-\") === 0) return;\n"
           "    workspace.sendClientToScreen(window, screen);\n"
           "  }\n"
           "  // The player is at the stream, so the window they started takes the focus too. KWin\n"
           "  // otherwise leaves it at the desk, and an unfocused game ignores its controller and taps.\n"
           "  if (window.normalWindow || window.dialog) workspace.activeWindow = window;\n"
           "});\n";
  }

  bool kwin_mode_matches(const kscreen_output_layout_t &output, int width, int height, int hz) {
    return output.mode_width == width && output.mode_height == height &&
           output.refresh_hz > 0.0 && std::abs(output.refresh_hz - hz) <= 0.5;
  }

  bool kwin_placement_matches(const kscreen_output_layout_t &output, int x) {
    return output.enabled && std::abs(output.scale - 1.0) < 0.01 && output.x == x && output.y == 0;
  }

  namespace {
    constexpr int hyprland_max_output_slots = 16;

    // Connector names this process currently has spoken for. A streaming session
    // (proc::linux_vdisplay) and the web UI (confighttp::ui_vdisplay) each own an
    // independent virtual display in the same process and cannot see each other,
    // so the reservation is what stops them requesting the same connector.
    std::mutex hyprland_reserved_names_mutex;
    std::set<std::string> hyprland_reserved_names;

    bool hyprland_reserve_output_name(const std::string &output_name) {
      std::lock_guard lock {hyprland_reserved_names_mutex};
      return hyprland_reserved_names.insert(output_name).second;
    }

    void hyprland_release_output_name(const std::string &output_name) {
      std::lock_guard lock {hyprland_reserved_names_mutex};
      hyprland_reserved_names.erase(output_name);
    }
  }  // namespace

  // ---------------------------------------------------------------------------
  // EVDI backend — dynamically loaded libevdi
  // ---------------------------------------------------------------------------
  namespace evdi {

    // Minimal EDID for a virtual display.
    // This is a 128-byte base EDID block that declares a digital display
    // with the specified resolution as the preferred timing.
    // We generate it dynamically based on requested width/height/fps.

    /**
     * @brief Compute EDID checksum (sum of all 128 bytes must be 0 mod 256).
     */
    static void edid_checksum(unsigned char *edid) {
      unsigned char sum = 0;
      for (int i = 0; i < 127; i++) {
        sum += edid[i];
      }
      edid[127] = (unsigned char)(256 - sum);
    }

    /**
     * @brief Generate a minimal 128-byte EDID for the given resolution/refresh.
     *
     * This creates a valid EDID 1.4 block with:
     *   - Manufacturer ID "VRT" (Virtual)
     *   - A detailed timing descriptor for the requested mode
     *   - Monitor name "Apollo Virtual"
     */
    static std::vector<unsigned char> generate_edid(int width, int height, int fps) {
      // Start with a known-good base EDID template
      std::vector<unsigned char> edid(128, 0);

      // Header (bytes 0-7)
      edid[0] = 0x00;
      edid[1] = 0xFF;
      edid[2] = 0xFF;
      edid[3] = 0xFF;
      edid[4] = 0xFF;
      edid[5] = 0xFF;
      edid[6] = 0xFF;
      edid[7] = 0x00;

      // Manufacturer ID "VRT" encoded as 3 5-bit chars: V=22, R=18, T=20
      // Byte 8: 0|10110|10010 -> high byte = (22 << 2) | (18 >> 3) = 88 | 2 = 0x5A
      // Byte 9: 010|10100|00000000 -> (18 & 0x7) << 5 | 20 = 0x54
      edid[8] = 0x5A;
      edid[9] = 0x54;

      // Product code (bytes 10-11)
      edid[10] = 0x01;
      edid[11] = 0x00;

      // Serial number (bytes 12-15)
      edid[12] = 0x01;
      edid[13] = 0x00;
      edid[14] = 0x00;
      edid[15] = 0x00;

      // Week of manufacture, year (bytes 16-17)
      edid[16] = 1;     // week 1
      edid[17] = 36;    // year 2026 - 1990 = 36

      // EDID version 1.4 (bytes 18-19)
      edid[18] = 1;
      edid[19] = 4;

      // Video input: digital, 8 bits per color, DisplayPort (byte 20)
      edid[20] = 0xA5;  // Digital, 8bpc, DP

      // Screen size in cm (bytes 21-22) - approximate based on resolution
      int diag_cm = 60;  // ~24 inches
      edid[21] = (unsigned char)(diag_cm * width / (width + height));
      edid[22] = (unsigned char)(diag_cm * height / (width + height));

      // Gamma 2.2 (byte 23) = (gamma * 100) - 100 = 120
      edid[23] = 120;

      // Feature support (byte 24): DPMS standby/suspend/off, RGB color, preferred timing
      edid[24] = 0x0E;

      // Chromaticity coordinates (bytes 25-34) - standard sRGB
      edid[25] = 0xEE;
      edid[26] = 0x95;
      edid[27] = 0xA3;
      edid[28] = 0x54;
      edid[29] = 0x4C;
      edid[30] = 0x99;
      edid[31] = 0x26;
      edid[32] = 0x0F;
      edid[33] = 0x50;
      edid[34] = 0x54;

      // Established timings (bytes 35-37)
      edid[35] = 0x00;
      edid[36] = 0x00;
      edid[37] = 0x00;

      // Standard timings (bytes 38-53) - all unused
      for (int i = 38; i < 54; i += 2) {
        edid[i] = 0x01;
        edid[i + 1] = 0x01;
      }

      // Descriptor block 1 (bytes 54-71): Detailed Timing Descriptor
      // Pixel clock calculation: we use a simplified CVT-like formula
      // For the detailed timing, compute approximate blanking intervals
      int h_active = width;
      int v_active = height;
      int h_blank = (int)(h_active * 0.2);  // ~20% horizontal blanking
      int v_blank = (int)(v_active * 0.05) + 1; // ~5% vertical blanking
      int h_total = h_active + h_blank;
      int v_total = v_active + v_blank;

      // Pixel clock in 10 kHz units
      uint32_t pixel_clock = (uint32_t)((long long)h_total * v_total * fps / 10000);

      edid[54] = (unsigned char)(pixel_clock & 0xFF);
      edid[55] = (unsigned char)((pixel_clock >> 8) & 0xFF);

      // Horizontal active & blanking
      edid[56] = (unsigned char)(h_active & 0xFF);
      edid[57] = (unsigned char)(h_blank & 0xFF);
      edid[58] = (unsigned char)(((h_active >> 8) & 0xF) << 4 | ((h_blank >> 8) & 0xF));

      // Vertical active & blanking
      edid[59] = (unsigned char)(v_active & 0xFF);
      edid[60] = (unsigned char)(v_blank & 0xFF);
      edid[61] = (unsigned char)(((v_active >> 8) & 0xF) << 4 | ((v_blank >> 8) & 0xF));

      // H/V sync offset and pulse width (simplified)
      int h_sync_offset = h_blank / 4;
      int h_sync_width = h_blank / 4;
      int v_sync_offset = v_blank / 4;
      int v_sync_width = v_blank / 4;

      edid[62] = (unsigned char)(h_sync_offset & 0xFF);
      edid[63] = (unsigned char)(h_sync_width & 0xFF);
      edid[64] = (unsigned char)(((v_sync_offset & 0xF) << 4) | (v_sync_width & 0xF));
      edid[65] = (unsigned char)(
        ((h_sync_offset >> 8) & 0x3) << 6 |
        ((h_sync_width >> 8) & 0x3) << 4 |
        ((v_sync_offset >> 4) & 0x3) << 2 |
        ((v_sync_width >> 4) & 0x3)
      );

      // Image size in mm (approximate)
      int h_mm = edid[21] * 10;
      int v_mm = edid[22] * 10;
      edid[66] = (unsigned char)(h_mm & 0xFF);
      edid[67] = (unsigned char)(v_mm & 0xFF);
      edid[68] = (unsigned char)(((h_mm >> 8) & 0xF) << 4 | ((v_mm >> 8) & 0xF));

      // No border
      edid[69] = 0;
      edid[70] = 0;

      // Flags: non-interlaced, no stereo, digital separate sync
      edid[71] = 0x18;

      // Descriptor block 2 (bytes 72-89): Monitor name
      edid[72] = 0x00;
      edid[73] = 0x00;
      edid[74] = 0x00;
      edid[75] = 0xFC;  // Monitor name tag
      edid[76] = 0x00;
      const char *name = "Apollo Virtual";
      int name_len = std::min((int)strlen(name), 13);
      for (int i = 0; i < 13; i++) {
        edid[77 + i] = (i < name_len) ? (unsigned char)name[i] : 0x0A;
      }

      // Descriptor block 3 (bytes 90-107): Monitor range limits
      edid[90] = 0x00;
      edid[91] = 0x00;
      edid[92] = 0x00;
      edid[93] = 0xFD;  // Monitor range limits tag
      edid[94] = 0x00;
      edid[95] = (unsigned char)std::max(fps - 1, 1);   // Min V rate
      edid[96] = (unsigned char)std::min(fps + 1, 255);  // Max V rate
      edid[97] = 30;    // Min H rate (kHz)
      edid[98] = (unsigned char)(pixel_clock * 10 / h_total / 1000 + 1); // Max H rate (kHz)
      edid[99] = (unsigned char)(pixel_clock / 10000 + 1);  // Max pixel clock / 10 MHz
      edid[100] = 0x00; // Default GTF
      for (int i = 101; i < 108; i++) {
        edid[i] = 0x0A;
      }

      // Descriptor block 4 (bytes 108-125): Dummy descriptor
      edid[108] = 0x00;
      edid[109] = 0x00;
      edid[110] = 0x00;
      edid[111] = 0x10; // Dummy descriptor tag
      edid[112] = 0x00;
      for (int i = 113; i < 126; i++) {
        edid[i] = 0x20;
      }

      // Extension count (byte 126)
      edid[126] = 0;

      // Checksum (byte 127)
      edid_checksum(edid.data());

      return edid;
    }

    // libevdi function pointer types
    using evdi_handle_t = void *;
    using fn_evdi_open_t = evdi_handle_t (*)(int device);
    using fn_evdi_open_attached_to_t = evdi_handle_t (*)(const char *sysfs_parent_device);
    using fn_evdi_connect_t = void (*)(evdi_handle_t handle, const unsigned char *edid,
                                       const unsigned int edid_length,
                                       const uint32_t sku_area_limit);
    using fn_evdi_disconnect_t = void (*)(evdi_handle_t handle);
    using fn_evdi_close_t = void (*)(evdi_handle_t handle);

    // Dynamically loaded function pointers
    static void *lib_handle = nullptr;
    static fn_evdi_open_t fn_open = nullptr;
    static fn_evdi_open_attached_to_t fn_open_attached_to = nullptr;
    static fn_evdi_connect_t fn_connect = nullptr;
    static fn_evdi_disconnect_t fn_disconnect = nullptr;
    static fn_evdi_close_t fn_close = nullptr;

    /**
     * @brief Check if EVDI kernel module is loaded.
     */
    static bool is_module_loaded() {
      return fs::exists("/sys/module/evdi");
    }

    /**
     * @brief Attempt to load the EVDI kernel module via modprobe.
     * @return true if module is now loaded.
     */
    static bool load_module() {
      static bool module_load_attempt_logged = false;

      if (is_module_loaded()) {
        return true;
      }

      if (!module_load_attempt_logged) {
        BOOST_LOG(info) << "Virtual display: attempting to load EVDI kernel module"sv;
        module_load_attempt_logged = true;
      }
      int ret = exec_cmd_rc("modprobe evdi 2>/dev/null");
      if (ret != 0) {
        BOOST_LOG(debug) << "Virtual display: modprobe evdi failed (rc="sv << ret << "), module may not be installed"sv;
        return false;
      }

      // Give the module a moment to initialize
      std::this_thread::sleep_for(500ms);
      return is_module_loaded();
    }

    /**
     * @brief Load libevdi dynamically and resolve required function pointers.
     * @return true if all functions were resolved successfully.
     */
    static bool load_library() {
      if (lib_handle) {
        return true;  // Already loaded
      }

      lib_handle = dyn::handle({"libevdi.so.0", "libevdi.so"});
      if (!lib_handle) {
        BOOST_LOG(debug) << "Virtual display: libevdi not found on this system"sv;
        return false;
      }

      std::vector<std::tuple<dyn::apiproc *, const char *>> funcs {
        {(dyn::apiproc *) &fn_open, "evdi_open"},
        {(dyn::apiproc *) &fn_open_attached_to, "evdi_open_attached_to"},
        {(dyn::apiproc *) &fn_connect, "evdi_connect"},
        {(dyn::apiproc *) &fn_disconnect, "evdi_disconnect"},
        {(dyn::apiproc *) &fn_close, "evdi_close"},
      };

      if (dyn::load(lib_handle, funcs)) {
        BOOST_LOG(warning) << "Virtual display: libevdi loaded but required functions not found"sv;
        dlclose(lib_handle);
        lib_handle = nullptr;
        return false;
      }

      BOOST_LOG(info) << "Virtual display: libevdi loaded successfully"sv;
      return true;
    }

    /**
     * @brief Check if the EVDI backend is available.
     */
    static bool is_available() {
      return (is_module_loaded() || load_module()) && load_library();
    }

    /**
     * @brief Check whether an evdi DRM card already exists.
     */
    static bool evdi_card_exists() {
      try {
        for (const auto &entry : fs::directory_iterator("/sys/class/drm/")) {
          std::string name = entry.path().filename().string();
          if (name.find("card") == 0 && name.find('-') == std::string::npos) {
            auto driver_path = entry.path() / "device" / "driver";
            if (fs::exists(driver_path) &&
                fs::read_symlink(driver_path).filename().string() == "evdi") {
              return true;
            }
          }
        }
      } catch (...) {}
      return false;
    }

    /**
     * @brief Check whether this process could actually obtain an EVDI device.
     *
     * Module + library presence is not enough: without a pre-created evdi card
     * (modprobe initial_device_count=N) creating one requires write access to
     * /sys/devices/evdi/add, which an unprivileged Polaris does not have.
     */
    static bool can_create() {
      return evdi_card_exists() || access("/sys/devices/evdi/add", W_OK) == 0;
    }

    /**
     * @brief Find the output name for a newly created EVDI device.
     *
     * After EVDI creates a new card, the DRM subsystem assigns it a connector
     * name. We scan /sys/class/drm/ to find the new card's connector.
     */
    static std::string find_evdi_output(const std::string &device_path) {
      // The device_path is something like /dev/dri/card2
      // We need to find the connector name, e.g. "DVI-I-1" or the card name
      std::string card_name;
      if (device_path.find("/dev/dri/card") != std::string::npos) {
        card_name = device_path.substr(device_path.rfind('/') + 1);
      }

      if (card_name.empty()) {
        return {};
      }

      // Look for connectors associated with this card
      std::string drm_path = "/sys/class/drm/";
      try {
        for (const auto &entry : fs::directory_iterator(drm_path)) {
          std::string name = entry.path().filename().string();
          // EVDI connectors show up as cardN-<type>-<num>
          if (name.find(card_name + "-") == 0) {
            // Extract the connector part after "cardN-"
            std::string connector = name.substr(card_name.size() + 1);
            BOOST_LOG(debug) << "Virtual display: found EVDI connector ["sv << connector << "] on "sv << card_name;
            return connector;
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Virtual display: error scanning DRM connectors: "sv << e.what();
      }

      return {};
    }

    static std::optional<bool> output_is_connected(const vdisplay_t &display) {
      const auto card_name = fs::path {display.device_path}.filename().string();
      if (card_name.empty() || display.output_name.empty()) {
        return std::nullopt;
      }
      const auto status_path = fs::path {"/sys/class/drm"} /
                               (card_name + "-" + display.output_name) /
                               "status";
      std::error_code ec;
      const bool exists = fs::exists(status_path, ec);
      if (ec) {
        return std::nullopt;
      }
      if (!exists) {
        return false;
      }
      std::ifstream status(status_path);
      if (!status.is_open()) {
        return std::nullopt;
      }
      std::string value;
      if (!(status >> value)) {
        return std::nullopt;
      }
      return evdi_connector_status_is_connected(value);
    }

    static bool supported_gpu_driver(const std::string &driver) {
      return driver == "nvidia" || driver == "amdgpu" || driver == "i915";
    }

    static std::string drm_device_sysfs_path(const std::string &drm_node) {
      if (drm_node.empty()) {
        return {};
      }

      fs::path node_path {drm_node};
      std::string node_name = node_path.filename().string();
      if (node_name.empty()) {
        node_name = drm_node;
      }

      if (node_name.find("card") != 0 && node_name.find("renderD") != 0) {
        return {};
      }

      const auto device_path = fs::path("/sys/class/drm") / node_name / "device";
      try {
        if (fs::exists(device_path)) {
          return fs::canonical(device_path).string();
        }
      } catch (const std::exception &e) {
        BOOST_LOG(debug) << "Virtual display: failed to resolve adapter sysfs path for ["
                         << drm_node << "]: " << e.what();
      }

      return {};
    }

    static std::string configured_gpu_sysfs_path() {
      const auto &adapter_name = config::video.adapter_name;
      if (adapter_name.empty()) {
        return {};
      }

      auto path = drm_device_sysfs_path(adapter_name);
      if (!path.empty()) {
        BOOST_LOG(info) << "Virtual display: using configured adapter ["sv
                        << adapter_name << "] at "sv << path;
        return path;
      }

      BOOST_LOG(info) << "Virtual display: configured adapter ["sv
                      << adapter_name
                      << "] is not a DRM node; falling back to DRM GPU discovery"sv;
      return {};
    }

    static std::string first_supported_gpu_sysfs_path() {
      try {
        for (const auto &entry : fs::directory_iterator("/sys/class/drm/")) {
          std::string name = entry.path().filename().string();
          if (name.find("card") != 0 || name.find('-') != std::string::npos) {
            continue;
          }

          auto driver_link = entry.path() / "device" / "driver" / "module";
          if (!fs::exists(driver_link)) {
            continue;
          }

          auto driver = fs::read_symlink(driver_link).filename().string();
          if (supported_gpu_driver(driver)) {
            auto path = fs::canonical(entry.path() / "device").string();
            BOOST_LOG(info) << "Virtual display: attaching EVDI to discovered GPU ["
                            << name << "] driver=" << driver << " path=" << path;
            return path;
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Virtual display: error finding GPU sysfs path: "sv << e.what();
      }

      return {};
    }

    /**
     * @brief Create a virtual display using EVDI.
     */
    static std::optional<vdisplay_t> create(int width, int height, int fps) {
      if (!is_available()) {
        return std::nullopt;
      }

      // Snapshot existing DRI cards before creating a new one
      std::vector<std::string> existing_cards;
      try {
        for (const auto &entry : fs::directory_iterator("/dev/dri/")) {
          std::string name = entry.path().filename().string();
          if (name.find("card") == 0) {
            existing_cards.push_back(entry.path().string());
          }
        }
      } catch (...) {}

      // Find the GPU's sysfs device path to attach the EVDI device to.
      // evdi_open_attached_to(nullptr) crashes with strlen(nullptr) on some libevdi versions.
      std::string gpu_sysfs_path = configured_gpu_sysfs_path();
      if (gpu_sysfs_path.empty()) {
        gpu_sysfs_path = first_supported_gpu_sysfs_path();
      }

      // Try to open an existing EVDI device first (created via modprobe initial_device_count=1).
      // Falls back to evdi_open_attached_to() which creates a new device (needs privileges).
      evdi_handle_t handle = nullptr;
      std::string opened_device_path;

      // Find existing EVDI card index
      for (const auto &entry : fs::directory_iterator("/sys/class/drm/")) {
        std::string name = entry.path().filename().string();
        if (name.find("card") == 0 && name.find('-') == std::string::npos) {
          auto driver_path = entry.path() / "device" / "driver";
          if (fs::exists(driver_path)) {
            auto driver = fs::read_symlink(driver_path).filename().string();
            if (driver == "evdi") {
              int card_idx = 0;
              try { card_idx = std::stoi(name.substr(4)); } catch (...) {}
              BOOST_LOG(info) << "Virtual display: opening existing EVDI device "sv << name << " (index "sv << card_idx << ")"sv;
              handle = fn_open(card_idx);
              if (handle) {
                opened_device_path = "/dev/dri/" + name;
                break;
              }
            }
          }
        }
      }

      // Fallback: create new device attached to GPU
      if (!handle && !gpu_sysfs_path.empty()) {
        BOOST_LOG(info) << "Virtual display: creating new EVDI device attached to "sv << gpu_sysfs_path;
        handle = fn_open_attached_to(gpu_sysfs_path.c_str());
      }

      if (!handle) {
        BOOST_LOG(warning) << "Virtual display: failed to open EVDI device"sv;
        return std::nullopt;
      }

      BOOST_LOG(info) << "Virtual display: EVDI device opened successfully"sv;

      // Generate EDID for the requested resolution
      auto edid = generate_edid(width, height, fps);

      // Connect with our EDID (sku_area_limit = 0 means no limit)
      fn_connect(handle, edid.data(), (unsigned int)edid.size(), 0);

      BOOST_LOG(info) << "Virtual display: EVDI connected with "sv
                      << width << "x"sv << height << "@"sv << fps << "Hz"sv;

      // Give the system time to register the new display
      std::this_thread::sleep_for(1s);

      // Find the new DRI device path
      std::string new_device_path;
      try {
        for (const auto &entry : fs::directory_iterator("/dev/dri/")) {
          std::string path = entry.path().string();
          std::string name = entry.path().filename().string();
          if (name.find("card") == 0) {
            bool is_new = true;
            for (const auto &existing : existing_cards) {
              if (existing == path) {
                is_new = false;
                break;
              }
            }
            if (is_new) {
              new_device_path = path;
              break;
            }
          }
        }
      } catch (...) {}

      if (new_device_path.empty() && !opened_device_path.empty()) {
        new_device_path = opened_device_path;
      }

      // Find the output connector name
      std::string output_name;
      if (!new_device_path.empty()) {
        output_name = find_evdi_output(new_device_path);
      }

      if (!evdi_output_name_is_proven(output_name)) {
        BOOST_LOG(error) << "Virtual display: could not prove the EVDI output connector; refusing to publish a guessed name"sv;
        fn_disconnect(handle);
        fn_close(handle);
        return std::nullopt;
      }

      vdisplay_t display;
      display.device_path = new_device_path;
      display.output_name = output_name;
      display.width = width;
      display.height = height;
      display.fps = fps;
      display.active = true;
      display.backend = backend_e::EVDI;
      display.evdi_handle = handle;

      BOOST_LOG(info) << "Virtual display: EVDI display created ["sv
                      << output_name << "] at "sv << new_device_path;

      return display;
    }

    /**
     * @brief Destroy an EVDI virtual display.
     */
    static bool destroy(vdisplay_t &display) {
      if (!display.evdi_handle) {
        const auto connected = output_is_connected(display);
        if (connected && !*connected) {
          display.active = false;
          return true;
        }
        BOOST_LOG(error) << "Virtual display: EVDI absence could not be verified without a disconnect handle; retaining recovery authority"sv;
        return false;
      }

      BOOST_LOG(info) << "Virtual display: disconnecting EVDI display ["sv << display.output_name << "]"sv;

      fn_disconnect((evdi_handle_t)display.evdi_handle);

      // Small delay to let DRM process the disconnect
      std::this_thread::sleep_for(500ms);

      fn_close((evdi_handle_t)display.evdi_handle);

      display.evdi_handle = nullptr;
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      do {
        const auto connected = output_is_connected(display);
        if (connected && !*connected) {
          display.active = false;
          BOOST_LOG(info) << "Virtual display: EVDI disconnect verified"sv;
          return true;
        }
        std::this_thread::sleep_for(50ms);
      } while (std::chrono::steady_clock::now() < deadline);

      BOOST_LOG(error) << "Virtual display: EVDI connector remains connected; retaining recovery authority"sv;
      return false;
    }

  }  // namespace evdi

  // ---------------------------------------------------------------------------
  // Wayland compositor backend — headless outputs
  // ---------------------------------------------------------------------------
  namespace wayland_wlr {

    /**
     * @brief Detect the running Wayland compositor type.
     * @return A string identifier: "hyprland", "sway", "kwin", or empty.
     */
    static std::string detect_compositor() {
      // Check for Hyprland
      if (std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
        return "hyprland";
      }

      // Check for Sway
      if (std::getenv("SWAYSOCK")) {
        return "sway";
      }

      // Session-manager variables survive into environments the compositor
      // sockets' variables do not (e.g. systemd user services).
      const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
      const char *session_desktop = std::getenv("XDG_SESSION_DESKTOP");
      const auto env_contains = [](const char *value, std::string_view needle) {
        return value && std::string_view(value).find(needle) != std::string_view::npos;
      };
      if (env_contains(desktop, "Hyprland") || env_contains(session_desktop, "Hyprland")) {
        return "hyprland";
      }
      if (env_contains(desktop, "sway") || env_contains(session_desktop, "sway")) {
        return "sway";
      }

      // Check for KDE/KWin on Wayland
      if (env_contains(desktop, "KDE")) {
        if (std::getenv("WAYLAND_DISPLAY")) {
          return "kwin";
        }
      }

      // A systemd user service under Hyprland may carry none of the variables
      // above; hyprctl can still reach the compositor through XDG_RUNTIME_DIR.
      if (exec_cmd_rc("hyprctl -j version >/dev/null 2>&1") == 0) {
        return "hyprland";
      }

      return {};
    }

    /**
     * @brief Check if the Wayland headless backend is available.
     */
    static bool is_available() {
      std::string_view wayland_display;
#ifdef POLARIS_BUILD_WAYLAND
      if (const char *value = std::getenv("WAYLAND_DISPLAY")) {
        wayland_display = value;
      }
#endif
      if (!wayland_backend_probe_allowed(
            window_system == window_system_e::WAYLAND,
            wayland_display
          )) {
        return false;
      }

      std::string compositor = detect_compositor();
      if (!wayland_compositor_supports_exact_output_creation(compositor)) {
        return false;
      }

      // hyprctl is the control interface for Hyprland and accepts a caller-owned name.
      return exec_cmd_rc("which hyprctl >/dev/null 2>&1") == 0;
    }

    /**
     * @brief Create a virtual display via the Wayland compositor.
     */
    static std::optional<vdisplay_t> create(int width, int height, int fps) {
      std::string compositor = detect_compositor();
      if (!wayland_compositor_supports_exact_output_creation(compositor)) {
        BOOST_LOG(warning) << "Virtual display: compositor ["sv << compositor
                           << "] cannot create a caller-named output; refusing before mutation"sv;
        return std::nullopt;
      }

      std::string output_name;

      if (compositor == "hyprland") {
        // Request a process-scoped name so an existing user-owned HEADLESS-N
        // output can never be selected, reconfigured, or removed by Polaris.
        // The slot suffix keeps the name unique per display rather than per
        // process: a session launch and the web UI can hold one each, and a
        // create that timed out before Hyprland published its output leaves an
        // orphan that occupies one slot instead of wedging the process.
        const std::string monitors_before = exec_cmd("hyprctl monitors -j 2>/dev/null");
        for (int slot = 0; slot < hyprland_max_output_slots && output_name.empty(); ++slot) {
          std::string candidate = hyprland_output_name_for_pid(getpid(), slot);
          if (!hyprland_reserve_output_name(candidate)) {
            // A live display in this process already holds the slot.
            continue;
          }

          const auto present = hyprland_monitors_contain_output(monitors_before, candidate);
          if (!present) {
            BOOST_LOG(error) << "Virtual display: Hyprland monitor inventory was not valid; refusing named-output creation"sv;
            hyprland_release_output_name(candidate);
            return std::nullopt;
          }
          if (*present) {
            // Never mutate an untracked orphan during selection. A durable
            // recovery record, if one exists, is handled by stale cleanup;
            // otherwise occupying this slot is safer than guessing ownership.
            hyprland_release_output_name(candidate);
            continue;
          }

          output_name = std::move(candidate);
        }

        if (output_name.empty()) {
          BOOST_LOG(warning) << "Virtual display: no free Hyprland output slot for pid "sv << getpid();
          return std::nullopt;
        }

        vdisplay_t intent;
        intent.output_name = output_name;
        intent.width = width;
        intent.height = height;
        intent.fps = fps;
        intent.active = true;
        intent.backend = backend_e::WAYLAND_WLR;
        if (!record_persisted_display(intent, 0)) {
          BOOST_LOG(error) << "Virtual display: refusing Hyprland mutation because durable recovery intent could not be committed"sv;
          hyprland_release_output_name(output_name);
          return std::nullopt;
        }

        std::string result = exec_cmd("hyprctl output create headless " + output_name + " 2>&1");
        BOOST_LOG(info) << "Virtual display: hyprctl output create headless: "sv << result;
        if (result != "ok") {
          BOOST_LOG(warning) << "Virtual display: Hyprland rejected headless output creation for ["sv
                             << output_name << "]; retaining recovery intent until serialized cleanup verifies stable absence"sv;
          hyprland_release_output_name(output_name);
          return std::nullopt;
        }

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        bool output_appeared = false;
        do {
          const auto present = hyprland_monitors_contain_output(
            exec_cmd("hyprctl monitors -j 2>/dev/null"),
            output_name
          );
          if (present && *present) {
            output_appeared = true;
            break;
          }
          std::this_thread::sleep_for(50ms);
        } while (std::chrono::steady_clock::now() < deadline);

        if (!output_appeared) {
          BOOST_LOG(warning) << "Virtual display: created Hyprland output did not appear ["sv
                             << output_name << "]"sv;
          // The output may still materialize after this removal runs. Keep the
          // owner-zero recovery intent even if one immediate query says absent;
          // serialized stale cleanup must verify removal before another create.
          exec_cmd_rc("hyprctl output remove " + output_name + " >/dev/null 2>&1");
          hyprland_release_output_name(output_name);
          return std::nullopt;
        }

        // Set resolution and refresh rate.
        //
        // hyprctl's exit status does not mean the request was honored. Hyprland
        // 0.56 answers `hyprctl keyword` with "unknown request" and still exits
        // 0 (#444), so trusting rc left Polaris logging the client's geometry
        // while the output stayed at the compositor's default. Capture then ran
        // at the wrong size and was silently scaled, with nothing in the log to
        // explain the wrong-aspect stream. Read the mode back and believe that.
        const std::string mode_spec = std::to_string(width) + "x" + std::to_string(height) +
                                      "@" + std::to_string(fps);

        const auto mode_landed = [&]() {
          // The set is not always instant; give it the same short settle budget
          // the output-creation poll above uses.
          const auto settle = std::chrono::steady_clock::now() + 1s;
          do {
            const auto actual = hyprland_monitor_mode(
              exec_cmd("hyprctl monitors -j 2>/dev/null"),
              output_name
            );
            if (actual && actual->width == width && actual->height == height) {
              return true;
            }
            std::this_thread::sleep_for(50ms);
          } while (std::chrono::steady_clock::now() < settle);
          return false;
        };

        const std::string keyword_result = exec_cmd(
          "hyprctl keyword monitor " + output_name + "," + mode_spec + ",auto,1 2>&1"
        );

        if (!mode_landed()) {
          BOOST_LOG(info) << "Virtual display: hyprctl keyword did not apply the mode ["sv
                          << keyword_result << "]; trying the Lua monitor form"sv;

          // Hyprland 0.56 moved config/IPC to Lua. hl.monitor takes a table and
          // the field is `output`, not `name`.
          const std::string eval_result = exec_cmd(
            "hyprctl eval 'hl.monitor({output=\"" + output_name + "\", mode=\"" + mode_spec +
            "\", position=\"auto\", scale=1})' 2>&1"
          );

          if (!mode_landed()) {
            const auto actual = hyprland_monitor_mode(
              exec_cmd("hyprctl monitors -j 2>/dev/null"),
              output_name
            );
            BOOST_LOG(warning) << "Virtual display: Hyprland did not apply mode ["sv << mode_spec
                               << "] on ["sv << output_name << "]; it reports ["sv
                               << (actual ? std::to_string(actual->width) + "x" + std::to_string(actual->height) : std::string {"unknown"})
                               << "]. The stream will be captured at that size and scaled. keyword=["sv
                               << keyword_result << "] eval=["sv << eval_result << ']';
          } else {
            BOOST_LOG(info) << "Virtual display: Lua monitor form applied ["sv << mode_spec
                            << "] on ["sv << output_name << ']';
          }
        }
      }
      else {
        BOOST_LOG(warning) << "Virtual display: no supported Wayland compositor detected for named output creation"sv;
        return std::nullopt;
      }

      if (output_name.empty()) {
        return std::nullopt;
      }

      vdisplay_t display;
      display.output_name = output_name;
      display.width = width;
      display.height = height;
      display.fps = fps;
      display.active = true;
      display.backend = backend_e::WAYLAND_WLR;

      BOOST_LOG(info) << "Virtual display: Wayland headless display created ["sv
                      << output_name << "] "sv << width << "x"sv << height << "@"sv << fps << "Hz"sv;

      return display;
    }

    /**
     * @brief Destroy a Wayland headless output.
     */
    static bool destroy(vdisplay_t &display) {
      std::string compositor = detect_compositor();

      if (compositor == "hyprland") {
        if (!hyprland_output_is_polaris_owned(display.output_name)) {
          BOOST_LOG(warning) << "Virtual display: refusing to remove unowned Hyprland output ["sv
                             << display.output_name << "]"sv;
          return false;
        }
        std::string cmd = "hyprctl output remove " + display.output_name;
        int rc = exec_cmd_rc(cmd);
        if (rc != 0) {
          BOOST_LOG(warning) << "Virtual display: hyprctl output remove failed (rc="sv << rc << ")"sv;
        }

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        std::optional<std::chrono::steady_clock::time_point> absent_since;
        do {
          const auto now = std::chrono::steady_clock::now();
          const auto present = hyprland_monitors_contain_output(
            exec_cmd("hyprctl monitors -j 2>/dev/null"),
            display.output_name
          );
          if (present && !*present) {
            if (!absent_since) {
              absent_since = now;
            }
            if (now - *absent_since >= 500ms) {
              hyprland_release_output_name(display.output_name);
              display.active = false;
              BOOST_LOG(info) << "Virtual display: Wayland headless display removal verified after stable absence ["sv
                              << display.output_name << "]"sv;
              return true;
            }
          } else {
            // Presence or an invalid query breaks the proof window. This also
            // covers a create request that materializes after its caller timed out.
            absent_since.reset();
          }
          std::this_thread::sleep_for(50ms);
        } while (std::chrono::steady_clock::now() < deadline);

        BOOST_LOG(error) << "Virtual display: Hyprland output remains after removal attempt ["sv
                         << display.output_name << "]; retaining recovery authority"sv;
        return false;
      }

      BOOST_LOG(error) << "Virtual display: cannot verify removal for compositor ["sv
                       << compositor << "]; retaining recovery authority for ["sv
                       << display.output_name << "]"sv;
      return false;
    }

  }  // namespace wayland_wlr

  // ---------------------------------------------------------------------------
  // kscreen-doctor fallback — manages existing displays
  // ---------------------------------------------------------------------------
  namespace kscreen {

    static bool is_installed() {
      return exec_cmd_rc("which kscreen-doctor >/dev/null 2>&1") == 0;
    }

    static std::optional<kscreen_output_state_t> current_output_state(
      const std::string &output_name
    ) {
      return kscreen_output_state_from_json(
        exec_cmd("kscreen-doctor --json 2>/dev/null"),
        output_name
      );
    }

    static void append_restore_args(
      std::vector<std::string> &args,
      const kscreen_output_state_t &state
    ) {
      const auto prefix = "output." + state.name;
      if (state.enabled) {
        args.push_back(prefix + ".enable");
      }
      if (!state.current_mode_id.empty()) {
        args.push_back(prefix + ".mode." + state.current_mode_id);
      }
      args.push_back(prefix + ".priority." + std::to_string(state.priority));
      if (!state.enabled) {
        args.push_back(prefix + ".disable");
      }
    }

    static bool restore_exact(vdisplay_t &display) {
      if (!display.kscreen_output_before) {
        BOOST_LOG(error) << "Virtual display: KScreen teardown has no exact pre-launch output state; retaining recovery authority"sv;
        return false;
      }

      std::vector<std::string> args {"kscreen-doctor"};
      append_restore_args(args, *display.kscreen_output_before);
      if (display.kscreen_primary_before &&
          display.kscreen_primary_before->name != display.kscreen_output_before->name) {
        append_restore_args(args, *display.kscreen_primary_before);
      }

      const int rc = platf::run_process_argv(args);
      const auto output_after = current_output_state(display.kscreen_output_before->name);
      const auto primary_after = display.kscreen_primary_before ?
        current_output_state(display.kscreen_primary_before->name) :
        std::optional<kscreen_output_state_t> {};
      const bool output_restored = output_after && *output_after == *display.kscreen_output_before;
      const bool primary_restored = !display.kscreen_primary_before ||
        (primary_after && *primary_after == *display.kscreen_primary_before);
      if (!output_restored || !primary_restored) {
        BOOST_LOG(error) << "Virtual display: KScreen exact topology restore did not read back"
                         << " rc="sv << rc << "; retaining recovery authority"sv;
        return false;
      }

      display.active = false;
      BOOST_LOG(info) << "Virtual display: KScreen topology restored exactly for ["sv
                      << display.output_name << ']';
      return true;
    }

    /**
     * @brief Create (enable) a display via kscreen-doctor.
     *
     * This doesn't create a truly new display — it enables and configures
     * an existing output that may be disabled or connected to a dummy plug.
     * Uses the streaming_output from the Linux display configuration.
     */
    static std::optional<vdisplay_t> create(int width, int height, int fps) {
      const auto &cfg = config::video.linux_display;

      // We need a configured streaming output for kscreen-doctor to manage. The
      // saved one still counts after a private or desktop mode retired it.
      const std::string output = host_virtual_display_connector();
      if (output.empty()) {
        BOOST_LOG(warning) << "Virtual display: kscreen-doctor fallback requires "
                              "'linux_streaming_output' to be configured"sv;
        return std::nullopt;
      }

      const auto output_before = current_output_state(output);
      const auto primary_before =
        !cfg.primary_output.empty() && cfg.primary_output != output ?
          current_output_state(cfg.primary_output) :
          std::optional<kscreen_output_state_t> {};
      if (!output_before ||
          (!cfg.primary_output.empty() && cfg.primary_output != output && !primary_before)) {
        BOOST_LOG(error) << "Virtual display: refusing KScreen mutation because exact pre-launch topology could not be read"sv;
        return std::nullopt;
      }

      vdisplay_t display;
      display.output_name = output;
      display.width = width;
      display.height = height;
      display.fps = fps;
      display.active = true;
      display.backend = backend_e::KSCREEN_DOCTOR;
      display.kscreen_output_before = output_before;
      display.kscreen_primary_before = primary_before;
      if (!record_persisted_display(display, 0)) {
        BOOST_LOG(error) << "Virtual display: refusing KScreen mutation because durable pre-launch recovery state could not be committed"sv;
        return std::nullopt;
      }
      const auto restore_or_retain = [&]() {
        if (restore_exact(display)) {
          if (!forget_persisted_display(display)) {
            BOOST_LOG(error) << "Virtual display: KScreen topology was restored but its recovery record could not be retired"sv;
          }
        } else {
          display.active = true;
        }
      };

      if (cfg.primary_output == output) {
        BOOST_LOG(info) << "Virtual display: linux_streaming_output and linux_primary_output both name ["sv << output
                        << "]; that monitor is reconfigured for the stream and made first"sv;
      }

      // Set mode and enable the output
      std::string mode_str = std::to_string(width) + "x" + std::to_string(height) +
                             "@" + std::to_string(fps);
      auto args = kscreen_enable_args(output, mode_str, cfg.primary_output);

      BOOST_LOG(info) << "Virtual display: running kscreen-doctor enable with "sv << args.size() - 1 << " argument(s)"sv;
      int rc = platf::run_process_argv(args);
      if (rc != 0) {
        BOOST_LOG(warning) << "Virtual display: kscreen-doctor enable failed (rc="sv << rc << ")"sv;

        // Try without explicit mode setting (just enable)
        args = kscreen_enable_args(output, {}, cfg.primary_output);

        rc = platf::run_process_argv(args);
        if (rc != 0) {
          BOOST_LOG(error) << "Virtual display: kscreen-doctor fallback enable also failed (rc="sv << rc << ")"sv;
          restore_or_retain();
          return std::nullopt;
        }
      }

      const auto output_after = current_output_state(output);
      if (!output_after || !output_after->enabled) {
        BOOST_LOG(error) << "Virtual display: KScreen enable did not read back; restoring exact pre-launch topology"sv;
        restore_or_retain();
        return std::nullopt;
      }

      BOOST_LOG(info) << "Virtual display: kscreen-doctor display enabled ["sv
                      << output << "] "sv << width << "x"sv << height << "@"sv << fps << "Hz"sv;

      return display;
    }

    /**
     * @brief Disable the display managed by kscreen-doctor.
     */
    static bool destroy(vdisplay_t &display) {
      BOOST_LOG(info) << "Virtual display: restoring exact pre-launch KScreen topology"sv;
      return restore_exact(display);
    }

  }  // namespace kscreen

  // ---------------------------------------------------------------------------
  // Absolute input on the stream screen (KDE Plasma)
  // ---------------------------------------------------------------------------
  namespace input_routing {

    // How long KWin gets to list a device Polaris just created.
    constexpr int device_wait_attempts = 20;
    constexpr auto device_wait_step = 100ms;

    struct state_t {
      std::mutex mutex;
      std::condition_variable wake;
      std::deque<std::string> pending;  ///< eventN names to point again, oldest first
      std::map<std::string, input_route_t> devices;  ///< eventN -> how it was last pointed
      std::vector<std::string> screens;  ///< KWin screens that exist, oldest first; the newest takes input
      bool stopping = false;
      std::thread worker;

      ~state_t() {
        {
          std::lock_guard lock {mutex};
          stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) {
          worker.join();
        }
      }
    };

    static state_t &state() {
      static state_t instance;
      return instance;
    }

#ifdef POLARIS_HAS_KWIN_VIRTUAL_OUTPUT
    static std::string target_unlocked(const state_t &s) {
      return s.screens.empty() ? std::string {} : s.screens.back();
    }

    /** Queue a device once; the caller holds the lock. */
    static void queue_unlocked(state_t &s, const std::string &sys_name) {
      if (std::find(s.pending.begin(), s.pending.end(), sys_name) == s.pending.end()) {
        s.pending.push_back(sys_name);
      }
    }

    static void requeue_all_unlocked(state_t &s) {
      for (const auto &[sys_name, route] : s.devices) {
        queue_unlocked(s, sys_name);
      }
    }

    /** Point one device at the newest KWin screen, or at none. Runs on the worker. */
    static void point(state_t &s, const std::string &sys_name) {
      // KWin lists a device a moment after the kernel creates it.
      std::string error;
      std::optional<std::string> name;
      for (int attempt = 0; attempt < device_wait_attempts; ++attempt) {
        {
          std::unique_lock lock {s.mutex};
          if (s.stopping || !s.devices.contains(sys_name)) {
            return;  // Forgotten meanwhile: the device is on its way out.
          }
          if (attempt > 0 && s.wake.wait_for(lock, device_wait_step, [&] {
                return s.stopping;
              })) {
            return;
          }
        }
        name = kwin_virtual_output::input_device_name(sys_name, error);
        if (name) {
          break;
        }
      }

      std::string target;
      {
        std::lock_guard lock {s.mutex};
        const auto it = s.devices.find(sys_name);
        if (it == s.devices.end()) {
          return;
        }
        if (!name) {
          it->second.output = target_unlocked(s);
          it->second.routed = false;
          it->second.error = "KWin does not list " + sys_name + ": " + error;
          BOOST_LOG(warning) << "Input: "sv << it->second.error;
          return;
        }
        if (!routes_to_stream_screen(*name)) {
          s.devices.erase(it);  // A relative pointer or a keyboard: nothing to point.
          return;
        }
        it->second.device = *name;
        target = target_unlocked(s);
      }

      // Without a Polaris screen there is only a leftover of ours to undo. A tie to
      // any other screen is the owner's choice in System Settings, kept as it is.
      if (target.empty()) {
        std::string ignored;
        const auto current = kwin_virtual_output::input_device_output(sys_name, ignored);
        if (!current || !current->starts_with("Virtual-polaris-"sv)) {
          std::lock_guard lock {s.mutex};
          if (const auto it = s.devices.find(sys_name); it != s.devices.end()) {
            it->second.output.clear();
            it->second.routed = true;
            it->second.error.clear();
          }
          return;
        }
      }

      const bool routed = kwin_virtual_output::set_input_device_output(sys_name, target, error);
      std::lock_guard lock {s.mutex};
      const auto it = s.devices.find(sys_name);
      if (it == s.devices.end()) {
        return;
      }
      it->second.output = target;
      it->second.routed = routed;
      it->second.error = routed ? std::string {} : error;
      if (!routed) {
        BOOST_LOG(warning) << "Input: ["sv << it->second.device << "] could not be pointed at "sv
                           << (target.empty() ? "no screen"s : "["s + target + "]"s) << ": "sv << error;
      } else if (target.empty()) {
        BOOST_LOG(info) << "Input: ["sv << it->second.device << "] spans the desktop again"sv;
      } else {
        BOOST_LOG(info) << "Input: ["sv << it->second.device << "] lands on ["sv << target << ']';
      }
    }

    static void run(state_t &s) {
      std::unique_lock lock {s.mutex};
      while (true) {
        s.wake.wait(lock, [&] {
          return s.stopping || !s.pending.empty();
        });
        if (s.stopping) {
          return;
        }
        const auto sys_name = s.pending.front();
        s.pending.pop_front();
        lock.unlock();
        point(s, sys_name);
        lock.lock();
      }
    }

    /** Start the worker the first time there is something to point; the caller holds the lock. */
    static void wake_unlocked(state_t &s) {
      if (s.pending.empty()) {
        return;
      }
      if (!s.worker.joinable()) {
        s.worker = std::thread([&s] {
          run(s);
        });
      }
      s.wake.notify_all();
    }

    /**
     * KWin is the one to ask, and it can see the devices: a Plasma session, and
     * no seat isolation, which hides them from the desktop on purpose.
     */
    static bool applies() {
      static const bool plasma = wayland_wlr::detect_compositor() == "kwin";
      return plasma && !config::input.client_keyboard_mouse_seat_isolation;
    }
#endif

    /** A Polaris screen exists now and takes the absolute input. */
    static void screen_added(const std::string &output_name) {
#ifdef POLARIS_HAS_KWIN_VIRTUAL_OUTPUT
      auto &s = state();
      std::lock_guard lock {s.mutex};
      std::erase(s.screens, output_name);
      s.screens.push_back(output_name);
      requeue_all_unlocked(s);
      wake_unlocked(s);
#endif
    }

    /** A Polaris screen is on its way out; the one before it, or none, takes the input. */
    static void screen_removed(const std::string &output_name) {
#ifdef POLARIS_HAS_KWIN_VIRTUAL_OUTPUT
      auto &s = state();
      std::lock_guard lock {s.mutex};
      std::erase(s.screens, output_name);
      requeue_all_unlocked(s);
      wake_unlocked(s);
#endif
    }

  }  // namespace input_routing

  // ---------------------------------------------------------------------------
  // KWin virtual output — a new screen KWin creates for a screencast stream
  // ---------------------------------------------------------------------------
  namespace kwin_vo {

    constexpr int max_output_slots = 8;
    constexpr auto release_budget = 3s;

    // Output names this process has spoken for: a streaming session and the
    // web UI can each hold one and cannot see each other's.
    static std::mutex reserved_names_mutex;
    static std::set<std::string> reserved_names;

    static bool reserve_name(const std::string &name) {
      std::lock_guard lock {reserved_names_mutex};
      return reserved_names.insert(name).second;
    }

    static void release_name(const std::string &name) {
      std::lock_guard lock {reserved_names_mutex};
      reserved_names.erase(name);
    }

    // The last probe's reason, so a forced backend can say why it cannot run.
    // Written and read under backend_detection_mutex.
    static std::string last_probe_reason;

    static std::string layout_json() {
      // Fixed argv, no output name, so no shell interpolation of user input.
      return exec_cmd("timeout 8 env QT_QPA_PLATFORM=wayland kscreen-doctor --json 2>/dev/null");
    }

    static int run_kscreen(std::vector<std::string> args) {
      // QT_QPA_PLATFORM=wayland keeps kscreen-doctor from aborting outside a
      // desktop session's environment; timeout guards against a hang.
      args.insert(args.begin(), {"timeout", "8", "env", "QT_QPA_PLATFORM=wayland", "kscreen-doctor"});
      return platf::run_process_argv(args);
    }

    static std::optional<kscreen_output_layout_t> find_output(
      const std::optional<std::vector<kscreen_output_layout_t>> &layout,
      std::string_view name
    ) {
      if (!layout) {
        return std::nullopt;
      }
      for (const auto &output : *layout) {
        if (output.name == name) {
          return output;
        }
      }
      return std::nullopt;
    }

    /** @brief Whether this backend can create a screen here, and why not when it cannot. */
    static bool probe(std::string &reason) {
#ifdef POLARIS_HAS_KWIN_VIRTUAL_OUTPUT
      if (wayland_wlr::detect_compositor() != "kwin") {
        reason = "This is not a KDE Plasma Wayland session.";
        return false;
      }
      // KWin can give a new output the layout it stored for another virtual
      // output, which on the test host meant scale 0.5 on top of the real
      // monitor. kscreen-doctor is what puts it right.
      if (!kscreen::is_installed()) {
        reason = "kscreen-doctor is not installed; Polaris needs it to place the new screen.";
        return false;
      }
      const auto result = kwin_virtual_output::probe();
      reason = result.available ? std::string {} : result.reason;
      return result.available;
#else
      reason = "This Polaris was built without KWin screencast support.";
      return false;
#endif
    }

    /**
     * @brief Put the new screen where the stream needs it: the client's mode,
     * scale 1, beside the other screens, ranked after every one of them.
     *
     * Best effort past the mode: a refresh rate KWin will not run is a warning,
     * and the stream still gets a screen of its own.
     *
     * @param layout_before The layout read before KWin was asked: the ranking to
     *        keep, and where to place the screen when a later read fails rather
     *        than putting it at 0,0.
     */
    static void place(const vdisplay_t &display, const std::vector<kscreen_output_layout_t> &layout_before) {
      const auto &name = display.output_name;
      // kscreen can list the output a moment after its Wayland global appears.
      auto layout = kscreen_layout_from_json(layout_json());
      auto output = find_output(layout, name);
      for (int attempt = 0; !output && attempt < 10; ++attempt) {
        std::this_thread::sleep_for(100ms);
        layout = kscreen_layout_from_json(layout_json());
        output = find_output(layout, name);
      }
      if (!output) {
        BOOST_LOG(warning) << "Virtual display: kscreen-doctor does not list ["sv << name
                           << "]; it keeps the layout KWin gave it"sv;
        return;
      }

      // Rank it last before anything else: until then a stored layout can hold
      // a real monitor's rank, and with it that monitor's desktop and panel.
      const auto ranking = kwin_priority_args(name, layout_before);
      if (const int rc = run_kscreen(ranking); rc != 0) {
        BOOST_LOG(warning) << "Virtual display: kscreen-doctor could not rank ["sv << name
                           << "] after the other screens (rc="sv << rc << ')';
      }

      if (!kwin_mode_matches(*output, display.width, display.height, display.fps)) {
        const auto mode = kwin_mode_name(display.width, display.height, display.fps);
        if (std::find(output->mode_names.begin(), output->mode_names.end(), mode) == output->mode_names.end()) {
          const int rc = run_kscreen(kwin_custom_mode_args(name, display.width, display.height, display.fps));
          if (rc != 0) {
            BOOST_LOG(warning) << "Virtual display: kscreen-doctor could not add mode ["sv << mode
                               << "] to ["sv << name << "] (rc="sv << rc << ')';
          }
        }
        const int rc = run_kscreen(kwin_mode_args(name, display.width, display.height, display.fps));
        layout = kscreen_layout_from_json(layout_json());
        output = find_output(layout, name);
        if (rc != 0 || !output || !kwin_mode_matches(*output, display.width, display.height, display.fps)) {
          BOOST_LOG(warning) << "Virtual display: ["sv << name << "] did not take mode ["sv << mode << "]; it runs at "sv
                             << (output ? kwin_mode_name(output->mode_width, output->mode_height, static_cast<int>(output->refresh_hz + 0.5)) : "an unknown mode"s)
                             << " and the stream is paced to that"sv;
        }
      }

      // Placement and the ranking again last: adding a custom mode can reorder
      // priorities. The other screens go back where they were before KWin
      // applied the layout it stored for this set of outputs, and the new one
      // goes just past them, measured on that same layout.
      const int x = kscreen_right_edge(layout_before, name);
      auto args = kwin_keep_positions_args(name, layout_before);
      const auto placement = kwin_placement_args(name, x);
      args.insert(args.end(), placement.begin(), placement.end());
      args.insert(args.end(), ranking.begin(), ranking.end());
      const int rc = run_kscreen(std::move(args));
      const auto placed = kscreen_layout_from_json(layout_json());
      output = find_output(placed, name);
      if (rc != 0 || !output || !kwin_placement_matches(*output, x)) {
        BOOST_LOG(warning) << "Virtual display: ["sv << name << "] could not be placed at scale 1 beside the other screens (rc="sv
                           << rc << ')';
      } else {
        BOOST_LOG(info) << "Virtual display: ["sv << name << "] placed at "sv << x << ",0, scale 1, ranked last"sv;
      }
      if (placed && !kwin_positions_match(*placed, layout_before, name)) {
        BOOST_LOG(warning) << "Virtual display: the other screens did not go back where they were with ["sv << name
                           << "] beside them"sv;
      }
      if (placed && !kwin_ranking_matches(*placed, layout_before, name)) {
        BOOST_LOG(warning) << "Virtual display: the screens are not ranked as they were with ["sv << name
                           << "] last; Plasma may have moved a desktop and panel onto it"sv;
      }
    }

    static std::optional<vdisplay_t> create(int width, int height, int fps) {
#ifdef POLARIS_HAS_KWIN_VIRTUAL_OUTPUT
      // Read the layout before asking KWin: it can apply a stored layout that
      // ranks the new output first the moment it exists.
      const auto layout_before_json = layout_json();
      const auto layout_before = kscreen_layout_from_json(layout_before_json);
      if (!layout_before) {
        BOOST_LOG(error) << "Virtual display: refusing a KWin screen because the current layout could not be read"sv;
        return std::nullopt;
      }
      const auto names = kwin_virtual_output::output_names();
      if (!names) {
        BOOST_LOG(error) << "Virtual display: refusing a KWin screen because the output list could not be read"sv;
        return std::nullopt;
      }

      std::string request_name;
      std::string output_name;
      for (int slot = 0; slot < max_output_slots && output_name.empty(); ++slot) {
        auto candidate = kwin_output_request_name(slot);
        auto expected = kwin_output_expected_name(candidate);
        if (!reserve_name(expected)) {
          continue;  // This process already holds the slot.
        }
        if (std::find(names->begin(), names->end(), expected) != names->end()) {
          // Present but not reserved here: not ours to take.
          release_name(expected);
          continue;
        }
        request_name = std::move(candidate);
        output_name = std::move(expected);
      }
      if (output_name.empty()) {
        BOOST_LOG(warning) << "Virtual display: no free KWin screen slot"sv;
        return std::nullopt;
      }

      vdisplay_t display;
      display.output_name = output_name;
      display.width = width;
      display.height = height;
      display.fps = fps;
      display.active = true;
      display.backend = backend_e::KWIN_VIRTUAL_OUTPUT;
      // No recovery record, unlike the other backends: KWin removes the screen
      // with the connection that asked for it, so a crash leaves nothing to
      // clean up, and a record would only outlive the screen it describes.

      std::string error;
      const auto created = kwin_virtual_output::create(request_name, width, height, error);
      if (!created) {
        BOOST_LOG(warning) << "Virtual display: KWin screen ["sv << output_name << "] was not created: "sv << error;
        release_name(output_name);
        return std::nullopt;
      }

      place(display, *layout_before);

      // The screen stays secondary, so a game would open on the real monitor.
      // Move windows that open while it exists onto it instead.
      if (std::string follow_error; !kwin_virtual_output::follow_windows(output_name, follow_error)) {
        BOOST_LOG(warning) << "Virtual display: new windows will not be moved onto ["sv << output_name
                           << "]; a game may open on another screen: "sv << follow_error;
      }
      // Touch, pen and the absolute mouse would otherwise span every monitor, so
      // a tap on the client landed wherever that point fell on the whole desk.
      input_routing::screen_added(output_name);
      BOOST_LOG(info) << "Virtual display: KWin screen created ["sv << output_name << "] "sv
                      << width << "x"sv << height << "@"sv << fps << "Hz"sv;
      return display;
#else
      BOOST_LOG(warning) << "Virtual display: this Polaris was built without KWin screencast support"sv;
      return std::nullopt;
#endif
    }

    static bool destroy(vdisplay_t &display) {
#ifdef POLARIS_HAS_KWIN_VIRTUAL_OUTPUT
      if (!kwin_output_is_polaris_owned(display.output_name)) {
        BOOST_LOG(warning) << "Virtual display: refusing to treat ["sv << display.output_name
                           << "] as a KWin screen Polaris created"sv;
        return false;
      }

      if (!kwin_virtual_output::anchored(display.output_name)) {
        // Not held by this process image, so KWin already removed it with the
        // connection that asked for it; only that connection could remove it.
        // Nothing to verify that could change the answer, and refusing here
        // would refuse every later launch, EVDI's included.
        BOOST_LOG(info) << "Virtual display: KWin screen ["sv << display.output_name
                        << "] is not held by this process; nothing to remove"sv;
        // Its window script may still be loaded if that process crashed.
        input_routing::screen_removed(display.output_name);
        kwin_virtual_output::stop_following_windows(display.output_name);
        release_name(display.output_name);
        display.active = false;
        return true;
      }

      // Stop moving windows and input onto it first, so neither is sent to a
      // screen on its way out.
      input_routing::screen_removed(display.output_name);
      kwin_virtual_output::stop_following_windows(display.output_name);
      if (!kwin_virtual_output::release(display.output_name, release_budget)) {
        BOOST_LOG(error) << "Virtual display: KWin screen ["sv << display.output_name
                         << "] is still present after release; retaining recovery authority"sv;
        return false;
      }
      release_name(display.output_name);
      display.active = false;
      return true;
#else
      // This build cannot hold a KWin screen, so one described here died with
      // the process that created it.
      BOOST_LOG(info) << "Virtual display: KWin screen ["sv << display.output_name
                      << "] belongs to a process that is gone; nothing to remove"sv;
      display.active = false;
      return true;
#endif
    }

  }  // namespace kwin_vo

  // ---------------------------------------------------------------------------
  // Public API
  // ---------------------------------------------------------------------------

  namespace {
    // What the Doctor reads about the screens this process created.
    std::mutex doctor_mutex;
    std::optional<backend_e> last_created_backend;
    std::map<std::string, double> scaled_screens;  ///< Live stream screens KWin runs at another scale
#ifdef POLARIS_TESTS
    std::optional<doctor_notes_t> doctor_notes_override;
#endif

    bool plasma_session() {
      static const bool plasma = wayland_wlr::detect_compositor() == "kwin";
      return plasma;
    }

    /**
     * Note the scale KWin gave a new stream screen. KWin applies the layout it
     * stored for a screen, and an EVDI screen on the test host came back at
     * 1.35, which draws everything on it larger than the stream asked for.
     */
    void note_stream_screen(const vdisplay_t &display) {
      {
        std::lock_guard lock {doctor_mutex};
        last_created_backend = display.backend;
      }
      // A borrowed kscreen connector is the owner's own monitor, at the scale they chose.
      if (display.backend == backend_e::KSCREEN_DOCTOR || !plasma_session() || !kscreen::is_installed() ||
          display.output_name.empty()) {
        return;
      }
      // kscreen can list the output a moment after it appears.
      std::optional<kscreen_output_layout_t> output;
      for (int attempt = 0; attempt < 10; ++attempt) {
        if (attempt > 0) {
          std::this_thread::sleep_for(100ms);
        }
        output = kwin_vo::find_output(kscreen_layout_from_json(kwin_vo::layout_json()), display.output_name);
        if (output) {
          break;
        }
      }
      if (!output) {
        return;
      }
      std::lock_guard lock {doctor_mutex};
      if (std::abs(output->scale - 1.0) >= 0.01) {
        scaled_screens[display.output_name] = output->scale;
        BOOST_LOG(warning) << "Virtual display: KWin runs ["sv << display.output_name << "] at scale "sv << output->scale
                           << ", from a layout it stored for that screen"sv;
      } else {
        scaled_screens.erase(display.output_name);
      }
    }

    void forget_stream_screen(const vdisplay_t &display) {
      std::lock_guard lock {doctor_mutex};
      scaled_screens.erase(display.output_name);
    }
  }  // namespace

  doctor_notes_t doctor_notes() {
#ifdef POLARIS_TESTS
    {
      std::lock_guard lock {doctor_mutex};
      if (doctor_notes_override) {
        return *doctor_notes_override;
      }
    }
#endif
    doctor_notes_t notes;
    notes.plasma = plasma_session();
    notes.preference = backend_preference_value();
    {
      // The Doctor and the stats stream read this; a probe in progress (a KWin round
      // trip, loading EVDI) must not stall them, and its reason is being rewritten anyway.
      std::unique_lock lock {backend_detection_mutex, std::try_to_lock};
      if (lock.owns_lock()) {
        notes.kwin_reason = kwin_vo::last_probe_reason;
      }
    }
    {
      std::lock_guard lock {doctor_mutex};
      notes.last_backend = last_created_backend;
      if (!scaled_screens.empty()) {
        notes.scaled_screen = scaled_screens.begin()->first;
        notes.scaled_screen_scale = scaled_screens.begin()->second;
      }
    }
    notes.input_routes = input_routes();
    return notes;
  }

#ifdef POLARIS_TESTS
  void set_doctor_notes_for_tests(std::optional<doctor_notes_t> notes) {
    std::lock_guard lock {doctor_mutex};
    doctor_notes_override = std::move(notes);
  }
#endif

  void route_absolute_input(const std::vector<std::string> &nodes) {
#ifdef POLARIS_HAS_KWIN_VIRTUAL_OUTPUT
    if (!input_routing::applies()) {
      return;
    }
    auto &s = input_routing::state();
    std::lock_guard lock {s.mutex};
    for (const auto &node : nodes) {
      auto sys_name = input_event_name(node);
      if (sys_name.empty()) {
        continue;
      }
      s.devices.try_emplace(sys_name);
      input_routing::queue_unlocked(s, sys_name);
    }
    input_routing::wake_unlocked(s);
#endif
  }

  void forget_absolute_input(const std::vector<std::string> &nodes) {
    auto &s = input_routing::state();
    std::lock_guard lock {s.mutex};
    for (const auto &node : nodes) {
      const auto sys_name = input_event_name(node);
      s.devices.erase(sys_name);
      std::erase(s.pending, sys_name);
    }
  }

  std::vector<input_route_t> input_routes() {
    auto &s = input_routing::state();
    std::lock_guard lock {s.mutex};
    std::vector<input_route_t> routes;
    for (const auto &[sys_name, route] : s.devices) {
      if (!route.device.empty() || !route.error.empty()) {
        routes.push_back(route);
      }
    }
    return routes;
  }

  const char *backend_name(backend_e backend) {
    switch (backend) {
      case backend_e::EVDI:
        return "EVDI";
      case backend_e::WAYLAND_WLR:
        return "Wayland (headless output)";
      case backend_e::KSCREEN_DOCTOR:
        return "kscreen-doctor";
      case backend_e::KWIN_VIRTUAL_OUTPUT:
        return "KWin virtual output";
      case backend_e::NONE:
      default:
        return "None";
    }
  }

  backend_e select_preferred_backend(bool evdi_ready, bool wayland_ready, bool kscreen_installed) {
    return select_backend(
      backend_preference_e::AUTO,
      {.evdi = evdi_ready, .kwin = false, .wlr = wayland_ready, .kscreen = kscreen_installed}
    );
  }

  namespace {
    std::optional<backend_preference_e> cached_preference;
    // A choice saved while Polaris runs. Guarded by backend_detection_mutex, and
    // kept apart from config::video, which launch threads copy and restore whole.
    std::optional<std::string> live_backend_preference;

    // Callers hold backend_detection_mutex.
    backend_preference_e configured_preference() {
      const auto value = live_backend_preference.value_or(config::video.linux_display.virtual_display_backend);
      const auto preference = parse_backend_preference(value);
      if (!preference) {
        BOOST_LOG(warning) << "Virtual display: unknown linux_virtual_display_backend ["sv << value << "]; using auto"sv;
      }
      return preference.value_or(backend_preference_e::AUTO);
    }

    backend_e detect_backend_with_cache_policy(
      bool force_refresh,
      bool *evdi_blocked = nullptr,
      std::string *kwin_reason = nullptr,
      backend_preference_e *preference_out = nullptr
    ) {
      // Detection touches both the cache and lazily initialized backend state
      // (including the EVDI library handle). Keep the full probe serialized,
      // rather than protecting only the two cache assignments.
      std::lock_guard cache_lock {backend_detection_mutex};
      const auto preference = configured_preference();
      if (preference_out) {
        *preference_out = preference;
      }
      const auto now = std::chrono::steady_clock::now();
      if (!force_refresh && cached_backend.has_value() && cached_preference == preference &&
          (now - cached_backend_time) <= backend_detection_cache_ttl) {
        if (evdi_blocked) {
          *evdi_blocked = *cached_backend == backend_e::NONE &&
                          (preference == backend_preference_e::AUTO || preference == backend_preference_e::EVDI) &&
                          evdi::is_module_loaded() &&
                          evdi::load_library() &&
                          !evdi::can_create();
        }
        if (kwin_reason) {
          *kwin_reason = kwin_vo::last_probe_reason;
        }
        return *cached_backend;
      }

      const auto wanted = [&](backend_preference_e backend) {
        return preference == backend_preference_e::AUTO || preference == backend;
      };
      const bool automatic = preference == backend_preference_e::AUTO;
      probe_snapshot_t probe;
      bool evdi_module_ready = false;
      bool evdi_library_ready = false;

      // KWin first: its probe answers at once off a Plasma session, and on one
      // its screen is the one games land on (see select_backend). A Plasma host
      // that gets the KWin screen never has the EVDI module loaded for it.
      kwin_vo::last_probe_reason.clear();
      if (wanted(backend_preference_e::KWIN)) {
        probe.kwin = kwin_vo::probe(kwin_vo::last_probe_reason);
      }
      // EVDI next: it creates true virtual connectors. Module + library presence
      // alone is not enough to advertise it: creation must actually be possible,
      // or the mode is offered and then silently fails at launch. A host set to
      // another backend never has the module loaded on its behalf.
      // Probe lower-priority candidates only when no earlier backend is ready.
      if (wanted(backend_preference_e::EVDI) && !(automatic && probe.kwin)) {
        evdi_module_ready = evdi::is_module_loaded() || evdi::load_module();
        if (evdi_module_ready) {
          evdi_library_ready = evdi::load_library();
          probe.evdi = evdi_library_ready && evdi::can_create();
        }
      }
      if (wanted(backend_preference_e::WLR) && !(automatic && (probe.evdi || probe.kwin))) {
        probe.wlr = wayland_wlr::is_available();
      }

      // kscreen-doctor last. Detect the installed backend even before its
      // connector is configured. is_available() separately applies
      // backend_has_required_configuration(), so launch admission stays
      // fail-closed while the UI can explain how to make this fallback ready.
      if (wanted(backend_preference_e::KSCREEN) && !(automatic && (probe.evdi || probe.kwin || probe.wlr))) {
        probe.kscreen = kscreen::is_installed();
      }
      const auto backend = select_backend(preference, probe);

      cached_backend = backend;
      cached_preference = preference;
      cached_backend_time = now;
      if (evdi_blocked) {
        *evdi_blocked = evdi_module_ready && evdi_library_ready && !probe.evdi;
      }
      if (kwin_reason) {
        *kwin_reason = kwin_vo::last_probe_reason;
      }
      log_detected_backend(backend);
      return backend;
    }
  }  // namespace

  backend_e detect_backend() {
    return detect_backend_with_cache_policy(false);
  }

  void set_backend_preference(const std::string &value) {
    std::lock_guard cache_lock {backend_detection_mutex};
    live_backend_preference = value;
    cached_backend.reset();
  }

  std::string backend_preference_value() {
    std::lock_guard cache_lock {backend_detection_mutex};
    const auto value = live_backend_preference.value_or(config::video.linux_display.virtual_display_backend);
    return value.empty() ? "auto" : value;
  }

  backend_e detect_backend_fresh() {
    return detect_backend_with_cache_policy(true);
  }

  bool backend_has_required_configuration(backend_e backend, const std::string &streaming_output) {
    switch (backend) {
      case backend_e::NONE:
        return false;
      case backend_e::KSCREEN_DOCTOR:
        return !streaming_output.empty();
      case backend_e::EVDI:
      case backend_e::WAYLAND_WLR:
      case backend_e::KWIN_VIRTUAL_OUTPUT:
        return true;
    }
    return false;
  }

  bool is_available() {
    const auto backend = detect_backend();
    return backend_has_required_configuration(backend, host_virtual_display_connector());
  }

  bool is_available_fresh() {
    const auto backend = detect_backend_fresh();
    return backend_has_required_configuration(backend, host_virtual_display_connector());
  }

  std::string unavailable_reason_for(backend_e backend, bool evdi_blocked, bool streaming_output_configured) {
    switch (backend) {
      case backend_e::EVDI:
      case backend_e::WAYLAND_WLR:
      case backend_e::KWIN_VIRTUAL_OUTPUT:
        return {};
      case backend_e::KSCREEN_DOCTOR:
        if (streaming_output_configured) {
          return {};
        }
        return "kscreen-doctor backend needs linux_streaming_output set to the output it may reconfigure.";
      case backend_e::NONE:
      default:
        if (evdi_blocked) {
          return "EVDI module is loaded but no device can be created: no evdi card exists and "
                 "/sys/devices/evdi/add is not writable by Polaris. Load evdi with "
                 "initial_device_count=1 or grant write access.";
        }
        return "No virtual display backend is available on this host (EVDI not usable, no KWin "
               "virtual output, no supported Wayland compositor, kscreen-doctor not configured).";
    }
  }

  std::string forced_backend_unavailable_reason(backend_preference_e preference, std::string_view detail) {
    std::string reason = "linux_virtual_display_backend is set to "s + std::string {backend_preference_name(preference)} + ", and ";
    switch (preference) {
      case backend_preference_e::EVDI:
        reason += "EVDI cannot create a display on this host.";
        break;
      case backend_preference_e::KWIN:
        reason += "KWin cannot create a screen here";
        reason += detail.empty() ? "."s : ": "s + std::string {detail};
        break;
      case backend_preference_e::WLR:
        reason += "no compositor that can create a named output (Hyprland) is running.";
        break;
      case backend_preference_e::KSCREEN:
        reason += "kscreen-doctor is not installed.";
        break;
      case backend_preference_e::AUTO:
      default:
        return {};
    }
    return reason + " Set it to auto to let Polaris choose.";
  }

  std::string unavailable_reason() {
    bool evdi_blocked = false;
    // Keep diagnostics in the same serialized probe as backend selection.
    // load_library() owns lazy handle/function-pointer state and must never run
    // outside backend_detection_mutex, including on its broken-library path.
    std::string kwin_reason;
    backend_preference_e preference = backend_preference_e::AUTO;
    const auto backend = detect_backend_with_cache_policy(false, &evdi_blocked, &kwin_reason, &preference);
    if (backend == backend_e::NONE && preference != backend_preference_e::AUTO) {
      return forced_backend_unavailable_reason(preference, preference == backend_preference_e::KWIN ? kwin_reason : std::string {});
    }
    return unavailable_reason_for(backend, evdi_blocked, !host_virtual_display_connector().empty());
  }

  static bool destroy_unlocked(vdisplay_t &display);

  struct stale_cleanup_result_t {
    bool found = false;
    bool succeeded = true;
  };

  static stale_cleanup_result_t cleanup_stale_unlocked() {
    const pid_t self = getpid();
    std::vector<persisted_display_t> stale;

    {
      std::lock_guard lock {persisted_state_mutex};
      auto loaded = load_persisted_entries();
      if (!loaded) {
        return {.found = false, .succeeded = false};
      }
      const auto &entries = *loaded;
      if (entries.empty()) {
        return {};
      }

      for (const auto &entry : entries) {
        // An entry owned by this process backs a display someone here still
        // holds — the streaming session and the web UI each own one — so it is
        // never stale no matter how many entries the file carries. Tearing
        // those down here is what used to kill a live sibling display.
        if (!persisted_display_is_stale(entry.owner_pid, self, pid_is_alive(entry.owner_pid))) {
          BOOST_LOG(info) << "Virtual display: persisted display ["sv << entry.display.output_name
                          << "] belongs to live pid "sv << entry.owner_pid << ", skipping stale cleanup"sv;
          continue;
        }
        stale.push_back(entry);
      }
    }

    // destroy() drops each entry from the file itself, so the lock is released
    // before it runs.
    bool succeeded = true;
    for (auto &entry : stale) {
      BOOST_LOG(info) << "Virtual display: cleaning up stale persisted display ["sv
                      << entry.display.output_name << "] from pid "sv << entry.owner_pid;
      if (!destroy_unlocked(entry.display)) {
        succeeded = false;
      }
    }

    return {.found = !stale.empty(), .succeeded = succeeded};
  }

  bool cleanup_stale() {
    std::lock_guard creation_lock {creation_mutex};
    const auto result = cleanup_stale_unlocked();
    return result.found && result.succeeded;
  }

#ifdef POLARIS_TESTS
  void with_creation_lock_for_tests(const std::function<void()> &callback) {
    std::lock_guard creation_lock {creation_mutex};
    callback();
  }
#endif

  std::optional<vdisplay_t> create(int width, int height, int fps) {
    std::lock_guard creation_lock {creation_mutex};
    const auto cleanup = cleanup_stale_unlocked();
    if (!cleanup.succeeded) {
      BOOST_LOG(error) << "Virtual display: stale recovery cleanup failed; refusing a replacement display"sv;
      return std::nullopt;
    }

    backend_e backend = detect_backend();

    BOOST_LOG(info) << "Virtual display: creating "sv << width << "x"sv << height
                    << "@"sv << fps << "Hz using backend: "sv << backend_name(backend);

    const auto publish = [&](std::optional<vdisplay_t> display) -> std::optional<vdisplay_t> {
      if (!display) {
        return std::nullopt;
      }
      if (display->backend == backend_e::KWIN_VIRTUAL_OUTPUT) {
        // Nothing for recovery to do: KWin removes the screen with this
        // process's connection. An older Polaris would also refuse the whole
        // state file over a backend it does not know.
        note_stream_screen(*display);
        return display;
      }
      if (record_persisted_display(*display)) {
        note_stream_screen(*display);
        return display;
      }
      BOOST_LOG(error) << "Virtual display: live output could not be bound to durable recovery authority; tearing it down"sv;
      if (!destroy_unlocked(*display)) {
        BOOST_LOG(error) << "Virtual display: emergency teardown was not verified after persistence failure"sv;
      }
      return std::nullopt;
    };

    switch (backend) {
      case backend_e::EVDI:
        return publish(evdi::create(width, height, fps));

      case backend_e::WAYLAND_WLR:
        return publish(wayland_wlr::create(width, height, fps));

      case backend_e::KSCREEN_DOCTOR:
        return publish(kscreen::create(width, height, fps));

      case backend_e::KWIN_VIRTUAL_OUTPUT:
        return publish(kwin_vo::create(width, height, fps));

      case backend_e::NONE:
      default:
        BOOST_LOG(warning) << "Virtual display: no backend available to create virtual display"sv;
        return std::nullopt;
    }
  }

  static bool destroy_unlocked(vdisplay_t &display) {
    if (!display.active) {
      return forget_persisted_display(display);
    }

    BOOST_LOG(info) << "Virtual display: destroying ["sv << display.output_name
                    << "] via "sv << backend_name(display.backend);
    forget_stream_screen(display);

    bool destroyed = false;
    switch (display.backend) {
      case backend_e::EVDI:
        destroyed = evdi::destroy(display);
        break;

      case backend_e::WAYLAND_WLR:
        destroyed = wayland_wlr::destroy(display);
        break;

      case backend_e::KSCREEN_DOCTOR:
        destroyed = kscreen::destroy(display);
        break;

      case backend_e::KWIN_VIRTUAL_OUTPUT:
        destroyed = kwin_vo::destroy(display);
        break;

      case backend_e::NONE:
      default:
        break;
    }

    if (!teardown_is_verified(destroyed, display.active)) {
      BOOST_LOG(error) << "Virtual display: teardown was not verified; persisted recovery record retained for ["sv
                       << display.output_name << "]"sv;
      return false;
    }

    // Drop only this display's record after verified teardown. Clearing the
    // whole file here used to strand a sibling display that was still live.
    if (!forget_persisted_display(display)) {
      BOOST_LOG(error) << "Virtual display: teardown was verified but persisted recovery retirement failed for ["sv
                       << display.output_name << ']';
      return false;
    }
    return true;
  }

  bool destroy(vdisplay_t &display) {
    std::lock_guard creation_lock {creation_mutex};
    return destroy_unlocked(display);
  }

}  // namespace virtual_display
