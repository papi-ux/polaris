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
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <filesystem>
#include <fstream>
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

// local includes
#include "misc.h"
#include "virtual_display.h"
#include "src/platform/common.h"
#include "src/config.h"
#include "src/logging.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace virtual_display {
  namespace {
    constexpr auto backend_detection_cache_ttl = 30s;
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
      return backend_e::NONE;
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
      return node;
    }

    /**
     * @brief Read every persisted display. Callers must hold persisted_state_mutex.
     */
    std::vector<persisted_display_t> load_persisted_entries() {
      std::ifstream file(persisted_state_path());
      if (!file.is_open()) {
        return {};
      }

      std::ostringstream buffer;
      buffer << file.rdbuf();
      return parse_persisted_displays(buffer.str());
    }

    /**
     * @brief Replace the persisted state with these displays. Callers must hold persisted_state_mutex.
     */
    void write_persisted_entries(const std::vector<persisted_display_t> &entries) {
      const auto path = persisted_state_path();
      std::error_code ec;

      if (entries.empty()) {
        fs::remove(path, ec);
        return;
      }

      fs::create_directories(path.parent_path(), ec);

      nlohmann::json root;
      root["displays"] = nlohmann::json::array();
      for (const auto &entry : entries) {
        root["displays"].push_back(serialize_persisted_entry(entry));
      }

      // Rename over the live file rather than truncating it: a crash midway
      // through a write would otherwise strand every display it was tracking.
      auto temp_path = path;
      temp_path += ".tmp";
      {
        std::ofstream file(temp_path, std::ios::trunc);
        if (!file.is_open()) {
          BOOST_LOG(warning) << "Virtual display: failed to persist state at "sv << path.string();
          return;
        }
        file << root.dump(2);
      }

      fs::rename(temp_path, path, ec);
      if (ec) {
        BOOST_LOG(warning) << "Virtual display: failed to persist state at "sv << path.string()
                           << ": "sv << ec.message();
        fs::remove(temp_path, ec);
      }
    }

    void record_persisted_display(const vdisplay_t &display) {
      const pid_t self = getpid();
      std::lock_guard lock {persisted_state_mutex};

      auto entries = load_persisted_entries();
      std::erase_if(entries, [&](const persisted_display_t &entry) {
        return entry.owner_pid == self && entry.display.output_name == display.output_name;
      });
      entries.push_back({self, display});
      write_persisted_entries(entries);
    }

    void forget_persisted_display(const vdisplay_t &display) {
      std::lock_guard lock {persisted_state_mutex};

      auto entries = load_persisted_entries();
      std::erase_if(entries, [&](const persisted_display_t &entry) {
        return entry.display.output_name == display.output_name;
      });
      write_persisted_entries(entries);
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
        case backend_e::NONE:
        default:
          BOOST_LOG(info) << "Virtual display: no backend available"sv;
          break;
      }
    }
  }  // namespace

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

  bool hyprland_monitors_contain_output(std::string_view monitors_json, std::string_view output_name) {
    const auto monitors = nlohmann::json::parse(monitors_json, nullptr, false);
    if (!monitors.is_array()) {
      return false;
    }

    for (const auto &monitor : monitors) {
      if (monitor.is_object() &&
          monitor.contains("name") &&
          monitor["name"].is_string() &&
          monitor["name"].get_ref<const std::string &>() == output_name) {
        return true;
      }
    }

    return false;
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

  static bool sway_output_snapshot_is_valid(std::string_view snapshot) {
    const auto outputs = nlohmann::json::parse(snapshot, nullptr, false);
    if (!outputs.is_array()) return false;
    for (const auto &output : outputs) {
      if (!output.is_object() || !output.contains("name") || !output["name"].is_string()) return false;
    }
    return true;
  }

  static bool with_valid_sway_before_snapshot(
    std::string_view snapshot,
    const std::function<void()> &create_callback
  ) {
    if (!sway_output_snapshot_is_valid(snapshot)) return false;
    create_callback();
    return true;
  }

#ifdef POLARIS_TESTS
  bool with_valid_sway_before_snapshot_for_tests(
    std::string_view snapshot,
    const std::function<void()> &create_callback
  ) {
    return with_valid_sway_before_snapshot(snapshot, create_callback);
  }
#endif

  bool sway_create_output_succeeded(std::string_view response_json) {
    const auto response = nlohmann::json::parse(response_json, nullptr, false);
    if (!response.is_array() || response.empty()) {
      return false;
    }
    for (const auto &entry : response) {
      if (!entry.is_object() ||
          !entry.contains("success") ||
          !entry["success"].is_boolean() ||
          !entry["success"].get<bool>()) {
        return false;
      }
    }
    return true;
  }

  std::optional<std::string> sway_new_headless_output(
    std::string_view before_outputs_json,
    std::string_view after_outputs_json
  ) {
    if (!sway_output_snapshot_is_valid(before_outputs_json) ||
        !sway_output_snapshot_is_valid(after_outputs_json)) {
      return std::nullopt;
    }
    const auto before = nlohmann::json::parse(before_outputs_json, nullptr, false);
    const auto after = nlohmann::json::parse(after_outputs_json, nullptr, false);
    if (!before.is_array() || !after.is_array()) {
      return std::nullopt;
    }

    std::set<std::string> previous_names;
    for (const auto &output : before) {
      if (output.is_object() && output.contains("name") && output["name"].is_string()) {
        previous_names.insert(output["name"].get<std::string>());
      }
    }

    std::optional<std::string> owned_output;
    for (const auto &output : after) {
      if (!output.is_object() || !output.contains("name") || !output["name"].is_string()) {
        continue;
      }
      const auto name = output["name"].get<std::string>();
      if (previous_names.contains(name) || !std::string_view {name}.starts_with("HEADLESS-")) {
        continue;
      }
      if (owned_output) {
        return std::nullopt;
      }
      owned_output = name;
    }
    return owned_output;
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
    static void destroy(vdisplay_t &display) {
      if (!display.evdi_handle) {
        return;
      }

      BOOST_LOG(info) << "Virtual display: disconnecting EVDI display ["sv << display.output_name << "]"sv;

      fn_disconnect((evdi_handle_t)display.evdi_handle);

      // Small delay to let DRM process the disconnect
      std::this_thread::sleep_for(500ms);

      fn_close((evdi_handle_t)display.evdi_handle);

      display.evdi_handle = nullptr;
      display.active = false;

      BOOST_LOG(info) << "Virtual display: EVDI display destroyed"sv;
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
      if (compositor == "hyprland") {
        // hyprctl is the control interface for Hyprland
        return exec_cmd_rc("which hyprctl >/dev/null 2>&1") == 0;
      }
      if (compositor == "sway") {
        // swaymsg can create outputs
        return exec_cmd_rc("which swaymsg >/dev/null 2>&1") == 0;
      }
      if (compositor == "kwin") {
        // KWin supports virtual outputs via DBus or kscreen-doctor
        // (handled by kscreen fallback)
        return false;
      }

      // No supported compositor identified. wlr-randr alone cannot create an
      // output (create() refuses the generic branch), so reporting available
      // here would advertise a display that can never appear.
      return false;
    }

    /**
     * @brief Create a virtual display via the Wayland compositor.
     */
    static std::optional<vdisplay_t> create(int width, int height, int fps) {
      std::string compositor = detect_compositor();
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

          if (hyprland_monitors_contain_output(monitors_before, candidate)) {
            // In the compositor but reserved by nothing: an orphan from an
            // earlier create of ours that materialized after we gave up on it.
            // The name is ours by construction, so removing it is safe. Take the
            // next slot either way — removal may not land before we need a name.
            BOOST_LOG(info) << "Virtual display: removing orphaned Hyprland output ["sv
                            << candidate << "]"sv;
            exec_cmd_rc("hyprctl output remove " + candidate + " >/dev/null 2>&1");
            hyprland_release_output_name(candidate);
            continue;
          }

          output_name = std::move(candidate);
        }

        if (output_name.empty()) {
          BOOST_LOG(warning) << "Virtual display: no free Hyprland output slot for pid "sv << getpid();
          return std::nullopt;
        }

        std::string result = exec_cmd("hyprctl output create headless " + output_name + " 2>&1");
        BOOST_LOG(info) << "Virtual display: hyprctl output create headless: "sv << result;
        if (result != "ok") {
          BOOST_LOG(warning) << "Virtual display: Hyprland rejected headless output creation for ["sv
                             << output_name << "]"sv;
          hyprland_release_output_name(output_name);
          return std::nullopt;
        }

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        bool output_appeared = false;
        do {
          if (hyprland_monitors_contain_output(
                exec_cmd("hyprctl monitors -j 2>/dev/null"),
                output_name
              )) {
            output_appeared = true;
            break;
          }
          std::this_thread::sleep_for(50ms);
        } while (std::chrono::steady_clock::now() < deadline);

        if (!output_appeared) {
          BOOST_LOG(warning) << "Virtual display: created Hyprland output did not appear ["sv
                             << output_name << "]"sv;
          // Best effort: the output may still materialize after this removal
          // runs. Releasing the reservation lets the next create recognize it as
          // an orphan and clear it rather than inheriting a name it cannot use.
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
      else if (compositor == "sway") {
        const std::string sway_outputs_before = exec_cmd("swaymsg -t get_outputs -r 2>/dev/null");
        std::string create_result;
        if (!with_valid_sway_before_snapshot(sway_outputs_before, [&] {
              create_result = exec_cmd("swaymsg -r create_output 2>&1");
            })) {
          BOOST_LOG(error) << "Virtual display: refusing Sway creation without a valid before snapshot"sv;
          return std::nullopt;
        }
        BOOST_LOG(info) << "Virtual display: swaymsg create_output: "sv << create_result;
        if (!sway_create_output_succeeded(create_result)) {
          BOOST_LOG(error) << "Virtual display: Sway rejected headless output creation"sv;
          return std::nullopt;
        }

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        do {
          output_name = sway_new_headless_output(
            sway_outputs_before,
            exec_cmd("swaymsg -t get_outputs -r 2>/dev/null")
          ).value_or("");
          if (!output_name.empty()) {
            break;
          }
          std::this_thread::sleep_for(50ms);
        } while (std::chrono::steady_clock::now() < deadline);

        if (output_name.empty()) {
          BOOST_LOG(error) << "Virtual display: could not prove ownership of the newly created Sway output"sv;
          return std::nullopt;
        }

        std::string mode_str = std::to_string(width) + "x" + std::to_string(height) +
                               "@" + std::to_string(fps) + "Hz";
        std::string mode_cmd = "swaymsg output " + output_name + " mode " + mode_str;
        if (exec_cmd_rc(mode_cmd) != 0) {
          BOOST_LOG(error) << "Virtual display: failed to configure owned Sway output ["sv << output_name << ']';
          exec_cmd_rc("swaymsg output " + output_name + " disable >/dev/null 2>&1");
          return std::nullopt;
        }
      }
      else {
        // Generic wlr-randr approach
        BOOST_LOG(warning) << "Virtual display: no supported Wayland compositor detected for headless output creation"sv;
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
    static void destroy(vdisplay_t &display) {
      std::string compositor = detect_compositor();

      if (compositor == "hyprland") {
        if (!hyprland_output_is_polaris_owned(display.output_name)) {
          BOOST_LOG(warning) << "Virtual display: refusing to remove unowned Hyprland output ["sv
                             << display.output_name << "]"sv;
          display.active = false;
          return;
        }
        std::string cmd = "hyprctl output remove " + display.output_name;
        int rc = exec_cmd_rc(cmd);
        if (rc != 0) {
          BOOST_LOG(warning) << "Virtual display: hyprctl output remove failed (rc="sv << rc << ")"sv;
        }
        hyprland_release_output_name(display.output_name);
      }
      else if (compositor == "sway") {
        // Sway doesn't have a direct "remove output" command for headless outputs,
        // but we can disable it
        std::string cmd = "swaymsg output " + display.output_name + " disable";
        exec_cmd_rc(cmd);
      }

      display.active = false;
      BOOST_LOG(info) << "Virtual display: Wayland headless display destroyed ["sv << display.output_name << "]"sv;
    }

  }  // namespace wayland_wlr

  // ---------------------------------------------------------------------------
  // kscreen-doctor fallback — manages existing displays
  // ---------------------------------------------------------------------------
  namespace kscreen {

    static bool is_available() {
      if (!backend_has_required_configuration(backend_e::KSCREEN_DOCTOR, config::video.linux_display.streaming_output)) {
        return false;
      }
      return exec_cmd_rc("which kscreen-doctor >/dev/null 2>&1") == 0;
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

      // We need a configured streaming output for kscreen-doctor to manage
      if (cfg.streaming_output.empty()) {
        BOOST_LOG(warning) << "Virtual display: kscreen-doctor fallback requires "
                              "'linux_streaming_output' to be configured"sv;
        return std::nullopt;
      }

      std::string output = cfg.streaming_output;

      // Set mode and enable the output
      std::string mode_str = std::to_string(width) + "x" + std::to_string(height) +
                             "@" + std::to_string(fps);
      std::vector<std::string> args {
        "kscreen-doctor",
        "output." + output + ".enable",
        "output." + output + ".mode." + mode_str,
        "output." + output + ".priority.1",
      };

      if (!cfg.primary_output.empty()) {
        args.push_back("output." + cfg.primary_output + ".priority.2");
      }

      BOOST_LOG(info) << "Virtual display: running kscreen-doctor enable with "sv << args.size() - 1 << " argument(s)"sv;
      int rc = platf::run_process_argv(args);
      if (rc != 0) {
        BOOST_LOG(warning) << "Virtual display: kscreen-doctor enable failed (rc="sv << rc << ")"sv;

        // Try without explicit mode setting (just enable)
        args = {
          "kscreen-doctor",
          "output." + output + ".enable",
          "output." + output + ".priority.1",
        };
        if (!cfg.primary_output.empty()) {
          args.push_back("output." + cfg.primary_output + ".priority.2");
        }

        rc = platf::run_process_argv(args);
        if (rc != 0) {
          BOOST_LOG(error) << "Virtual display: kscreen-doctor fallback enable also failed (rc="sv << rc << ")"sv;
          return std::nullopt;
        }
      }

      vdisplay_t display;
      display.output_name = output;
      display.width = width;
      display.height = height;
      display.fps = fps;
      display.active = true;
      display.backend = backend_e::KSCREEN_DOCTOR;

      BOOST_LOG(info) << "Virtual display: kscreen-doctor display enabled ["sv
                      << output << "] "sv << width << "x"sv << height << "@"sv << fps << "Hz"sv;

      return display;
    }

    /**
     * @brief Disable the display managed by kscreen-doctor.
     */
    static void destroy(vdisplay_t &display) {
      const auto &cfg = config::video.linux_display;

      std::vector<std::string> args {"kscreen-doctor"};
      if (!cfg.primary_output.empty()) {
        args.push_back("output." + cfg.primary_output + ".priority.1");
      }
      args.push_back("output." + display.output_name + ".priority.2");
      args.push_back("output." + display.output_name + ".disable");

      BOOST_LOG(info) << "Virtual display: running kscreen-doctor disable with "sv << args.size() - 1 << " argument(s)"sv;
      int rc = platf::run_process_argv(args);
      if (rc != 0) {
        BOOST_LOG(warning) << "Virtual display: kscreen-doctor disable failed (rc="sv << rc << ")"sv;
      }

      display.active = false;
      BOOST_LOG(info) << "Virtual display: kscreen-doctor display disabled ["sv << display.output_name << "]"sv;
    }

  }  // namespace kscreen

  // ---------------------------------------------------------------------------
  // Public API
  // ---------------------------------------------------------------------------

  const char *backend_name(backend_e backend) {
    switch (backend) {
      case backend_e::EVDI:
        return "EVDI";
      case backend_e::WAYLAND_WLR:
        return "Wayland (headless output)";
      case backend_e::KSCREEN_DOCTOR:
        return "kscreen-doctor";
      case backend_e::NONE:
      default:
        return "None";
    }
  }

  backend_e detect_backend() {
    const auto now = std::chrono::steady_clock::now();
    if (cached_backend.has_value() && (now - cached_backend_time) <= backend_detection_cache_ttl) {
      return *cached_backend;
    }

    backend_e backend = backend_e::NONE;

    // Priority 1: EVDI — creates true virtual connectors. Module + library
    // presence alone is not enough to advertise it: creation must actually be
    // possible, or the mode is offered and then silently fails at launch.
    if (evdi::is_module_loaded() || evdi::load_module()) {
      if (evdi::load_library() && evdi::can_create()) {
        backend = backend_e::EVDI;
      }
    }

    // Priority 2: Wayland compositor headless outputs
    if (backend == backend_e::NONE && wayland_wlr::is_available()) {
      backend = backend_e::WAYLAND_WLR;
    }

    // Priority 3: kscreen-doctor (KDE Plasma)
    if (backend == backend_e::NONE && kscreen::is_available()) {
      backend = backend_e::KSCREEN_DOCTOR;
    }

    cached_backend = backend;
    cached_backend_time = now;
    log_detected_backend(backend);
    return backend;
  }

  bool backend_has_required_configuration(backend_e backend, const std::string &streaming_output) {
    switch (backend) {
      case backend_e::NONE:
        return false;
      case backend_e::KSCREEN_DOCTOR:
        return !streaming_output.empty();
      case backend_e::EVDI:
      case backend_e::WAYLAND_WLR:
        return true;
    }
    return false;
  }

  bool is_available() {
    const auto backend = detect_backend();
    return backend_has_required_configuration(backend, config::video.linux_display.streaming_output);
  }

  std::string unavailable_reason_for(backend_e backend, bool evdi_blocked, bool streaming_output_configured) {
    switch (backend) {
      case backend_e::EVDI:
      case backend_e::WAYLAND_WLR:
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
        return "No virtual display backend is available on this host (EVDI not usable, no "
               "supported Wayland compositor, kscreen-doctor not configured).";
    }
  }

  std::string unavailable_reason() {
    const auto backend = detect_backend();
    // Probe without side effects: report the module as blocked only when it is
    // already loaded and usable but a device still cannot be obtained.
    const bool evdi_blocked = evdi::is_module_loaded() && evdi::load_library() && !evdi::can_create();
    return unavailable_reason_for(backend, evdi_blocked, !config::video.linux_display.streaming_output.empty());
  }

  static void destroy_unlocked(vdisplay_t &display);

  static bool cleanup_stale_unlocked() {
    const pid_t self = getpid();
    std::vector<persisted_display_t> stale;

    {
      std::lock_guard lock {persisted_state_mutex};
      auto entries = load_persisted_entries();
      if (entries.empty()) {
        // Nothing usable in the file, so drop whatever is left of it.
        write_persisted_entries({});
        return false;
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
    for (auto &entry : stale) {
      BOOST_LOG(info) << "Virtual display: cleaning up stale persisted display ["sv
                      << entry.display.output_name << "] from pid "sv << entry.owner_pid;
      destroy_unlocked(entry.display);
    }

    return !stale.empty();
  }

  bool cleanup_stale() {
    std::lock_guard creation_lock {creation_mutex};
    return cleanup_stale_unlocked();
  }

#ifdef POLARIS_TESTS
  void with_creation_lock_for_tests(const std::function<void()> &callback) {
    std::lock_guard creation_lock {creation_mutex};
    callback();
  }
#endif

  std::optional<vdisplay_t> create(int width, int height, int fps) {
    std::lock_guard creation_lock {creation_mutex};
    cleanup_stale_unlocked();

    backend_e backend = detect_backend();

    BOOST_LOG(info) << "Virtual display: creating "sv << width << "x"sv << height
                    << "@"sv << fps << "Hz using backend: "sv << backend_name(backend);

    switch (backend) {
      case backend_e::EVDI:
        if (auto display = evdi::create(width, height, fps)) {
          record_persisted_display(*display);
          return display;
        }
        return std::nullopt;

      case backend_e::WAYLAND_WLR:
        if (auto display = wayland_wlr::create(width, height, fps)) {
          record_persisted_display(*display);
          return display;
        }
        return std::nullopt;

      case backend_e::KSCREEN_DOCTOR:
        if (auto display = kscreen::create(width, height, fps)) {
          record_persisted_display(*display);
          return display;
        }
        return std::nullopt;

      case backend_e::NONE:
      default:
        BOOST_LOG(warning) << "Virtual display: no backend available to create virtual display"sv;
        return std::nullopt;
    }
  }

  static void destroy_unlocked(vdisplay_t &display) {
    if (!display.active) {
      return;
    }

    BOOST_LOG(info) << "Virtual display: destroying ["sv << display.output_name
                    << "] via "sv << backend_name(display.backend);

    switch (display.backend) {
      case backend_e::EVDI:
        evdi::destroy(display);
        break;

      case backend_e::WAYLAND_WLR:
        wayland_wlr::destroy(display);
        break;

      case backend_e::KSCREEN_DOCTOR:
        kscreen::destroy(display);
        break;

      case backend_e::NONE:
      default:
        break;
    }

    // Drop only this display's record. Clearing the whole file here used to
    // strand a sibling display that was still live.
    forget_persisted_display(display);
  }

  void destroy(vdisplay_t &display) {
    std::lock_guard creation_lock {creation_mutex};
    destroy_unlocked(display);
  }

}  // namespace virtual_display
