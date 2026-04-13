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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

// platform includes
#include <dlfcn.h>
#include <sys/wait.h>

// local includes
#include "misc.h"
#include "virtual_display.h"
#include "src/config.h"
#include "src/logging.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace virtual_display {

  // ---------------------------------------------------------------------------
  // Utility: run a shell command and capture stdout
  // ---------------------------------------------------------------------------
  static std::string exec_cmd(const std::string &cmd) {
    std::array<char, 256> buffer;
    std::string result;

    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
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
      if (is_module_loaded()) {
        return true;
      }

      BOOST_LOG(info) << "Virtual display: attempting to load EVDI kernel module"sv;
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
      std::string gpu_sysfs_path;
      try {
        for (const auto &entry : fs::directory_iterator("/sys/class/drm/")) {
          std::string name = entry.path().filename().string();
          // Look for cardN (not cardN-DP-1 etc)
          if (name.find("card") == 0 && name.find('-') == std::string::npos) {
            auto driver_link = entry.path() / "device" / "driver" / "module";
            if (fs::exists(driver_link)) {
              auto driver = fs::read_symlink(driver_link).filename().string();
              if (driver == "nvidia" || driver == "amdgpu" || driver == "i915") {
                gpu_sysfs_path = fs::canonical(entry.path() / "device").string();
                BOOST_LOG(info) << "Virtual display: attaching EVDI to GPU at "sv << gpu_sysfs_path;
                break;
              }
            }
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Virtual display: error finding GPU sysfs path: "sv << e.what();
      }

      // Try to open an existing EVDI device first (created via modprobe initial_device_count=1).
      // Falls back to evdi_open_attached_to() which creates a new device (needs privileges).
      evdi_handle_t handle = nullptr;

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
              if (handle) break;
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

      // Find the output connector name
      std::string output_name;
      if (!new_device_path.empty()) {
        output_name = find_evdi_output(new_device_path);
      }

      if (output_name.empty()) {
        // Fallback: use a generic name that downstream code can try to match
        output_name = "VIRTUAL-1";
        BOOST_LOG(warning) << "Virtual display: could not determine EVDI output name, using fallback ["sv
                           << output_name << "]"sv;
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

      // Check for KDE/KWin on Wayland
      const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
      if (desktop && std::string(desktop).find("KDE") != std::string::npos) {
        if (std::getenv("WAYLAND_DISPLAY")) {
          return "kwin";
        }
      }

      return {};
    }

    /**
     * @brief Check if the Wayland headless backend is available.
     */
    static bool is_available() {
      if (window_system != window_system_e::WAYLAND) {
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

      // Check for wlr-randr as a generic wlroots tool
      return exec_cmd_rc("which wlr-randr >/dev/null 2>&1") == 0;
    }

    /**
     * @brief Create a virtual display via the Wayland compositor.
     */
    static std::optional<vdisplay_t> create(int width, int height, int fps) {
      std::string compositor = detect_compositor();
      std::string output_name;

      if (compositor == "hyprland") {
        // Hyprland: create a headless output
        std::string result = exec_cmd("hyprctl output create headless 2>&1");
        BOOST_LOG(info) << "Virtual display: hyprctl output create headless: "sv << result;

        // Give compositor time to set up the output
        std::this_thread::sleep_for(500ms);

        // Find the new HEADLESS output
        std::string monitors = exec_cmd("hyprctl monitors -j 2>/dev/null");
        // Parse the output name from the JSON - look for HEADLESS-N
        // Simple approach: find "HEADLESS-" pattern in output
        size_t pos = monitors.rfind("HEADLESS-");
        if (pos != std::string::npos) {
          size_t end = monitors.find_first_of("\",} \n", pos);
          output_name = monitors.substr(pos, end - pos);
        }

        if (output_name.empty()) {
          output_name = "HEADLESS-1";
        }

        // Set resolution and refresh rate
        std::string mode_cmd = "hyprctl keyword monitor " + output_name + "," +
                               std::to_string(width) + "x" + std::to_string(height) +
                               "@" + std::to_string(fps) + ",auto,1";
        int rc = exec_cmd_rc(mode_cmd);
        if (rc != 0) {
          BOOST_LOG(warning) << "Virtual display: failed to set Hyprland monitor mode (rc="sv << rc << ")"sv;
        }
      }
      else if (compositor == "sway") {
        // Sway: create a headless output
        std::string result = exec_cmd("swaymsg create_output 2>&1");
        BOOST_LOG(info) << "Virtual display: swaymsg create_output: "sv << result;

        std::this_thread::sleep_for(500ms);

        // Sway names headless outputs as HEADLESS-N
        output_name = "HEADLESS-1";

        // Set the resolution
        std::string mode_str = std::to_string(width) + "x" + std::to_string(height) +
                               "@" + std::to_string(fps) + "Hz";
        std::string mode_cmd = "swaymsg output " + output_name + " mode " + mode_str;
        exec_cmd_rc(mode_cmd);
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
        std::string cmd = "hyprctl output remove " + display.output_name;
        int rc = exec_cmd_rc(cmd);
        if (rc != 0) {
          BOOST_LOG(warning) << "Virtual display: hyprctl output remove failed (rc="sv << rc << ")"sv;
        }
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
      std::string cmd = "kscreen-doctor output." + output + ".enable "
                        "output." + output + ".mode." + mode_str + " "
                        "output." + output + ".priority.1";

      if (!cfg.primary_output.empty()) {
        cmd += " output." + cfg.primary_output + ".priority.2";
      }

      BOOST_LOG(info) << "Virtual display: kscreen-doctor enable ["sv << cmd << "]"sv;
      int rc = exec_cmd_rc(cmd);
      if (rc != 0) {
        BOOST_LOG(warning) << "Virtual display: kscreen-doctor enable failed (rc="sv << rc << ")"sv;

        // Try without explicit mode setting (just enable)
        cmd = "kscreen-doctor output." + output + ".enable output." + output + ".priority.1";
        if (!cfg.primary_output.empty()) {
          cmd += " output." + cfg.primary_output + ".priority.2";
        }

        rc = exec_cmd_rc(cmd);
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

      std::string cmd = "kscreen-doctor";
      if (!cfg.primary_output.empty()) {
        cmd += " output." + cfg.primary_output + ".priority.1";
      }
      cmd += " output." + display.output_name + ".priority.2"
             " output." + display.output_name + ".disable";

      BOOST_LOG(info) << "Virtual display: kscreen-doctor disable ["sv << cmd << "]"sv;
      int rc = exec_cmd_rc(cmd);
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
    // Priority 1: EVDI — creates true virtual connectors
    if (evdi::is_module_loaded() || evdi::load_module()) {
      if (evdi::load_library()) {
        BOOST_LOG(info) << "Virtual display: EVDI backend available"sv;
        return backend_e::EVDI;
      }
    }

    // Priority 2: Wayland compositor headless outputs
    if (wayland_wlr::is_available()) {
      BOOST_LOG(info) << "Virtual display: Wayland headless output backend available"sv;
      return backend_e::WAYLAND_WLR;
    }

    // Priority 3: kscreen-doctor (KDE Plasma)
    if (kscreen::is_available()) {
      BOOST_LOG(info) << "Virtual display: kscreen-doctor backend available"sv;
      return backend_e::KSCREEN_DOCTOR;
    }

    BOOST_LOG(info) << "Virtual display: no backend available"sv;
    return backend_e::NONE;
  }

  bool is_available() {
    return detect_backend() != backend_e::NONE;
  }

  std::optional<vdisplay_t> create(int width, int height, int fps) {
    backend_e backend = detect_backend();

    BOOST_LOG(info) << "Virtual display: creating "sv << width << "x"sv << height
                    << "@"sv << fps << "Hz using backend: "sv << backend_name(backend);

    switch (backend) {
      case backend_e::EVDI:
        return evdi::create(width, height, fps);

      case backend_e::WAYLAND_WLR:
        return wayland_wlr::create(width, height, fps);

      case backend_e::KSCREEN_DOCTOR:
        return kscreen::create(width, height, fps);

      case backend_e::NONE:
      default:
        BOOST_LOG(warning) << "Virtual display: no backend available to create virtual display"sv;
        return std::nullopt;
    }
  }

  void destroy(vdisplay_t &display) {
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
  }

}  // namespace virtual_display
