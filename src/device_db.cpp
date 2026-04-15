/**
 * @file src/device_db.cpp
 * @brief Device knowledge base implementation.
 */

#include "device_db.h"
#include "config.h"
#include "logging.h"
#include "platform/common.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include <nlohmann/json.hpp>

using namespace std::literals;

namespace device_db {

  static std::unordered_map<std::string, device_t> devices;
  static std::once_flag load_flag;

  /**
   * @brief Embedded default device database.
   * Curated profiles for known streaming client devices.
   */
  static void load_defaults() {
    // --- Handhelds ---
    devices["RP6"] = {
      "handheld", "1920x1080x60", "hevc", 15000, 2, false, true, 3,
      "Retroid Pocket 6 — Android handheld, 1080p landscape, WiFi 6"
    };
    devices["Retroid Pocket 6"] = devices["RP6"];
    devices["RetroidPocket6"] = devices["RP6"];

    devices["Retroid Pocket Flip 2"] = {
      "handheld", "1920x1080x60", "hevc", 15000, 2, false, true, 3,
      "Retroid Pocket Flip 2 — Android clamshell handheld, 5.5-inch 1080p, WiFi 6"
    };
    devices["RetroidPocketFlip2"] = devices["Retroid Pocket Flip 2"];

    devices["RP5"] = {
      "handheld", "1280x720x60", "hevc", 10000, 2, false, true, 3,
      "Retroid Pocket 5 — Android handheld, 720p, WiFi 5"
    };

    devices["Steam Deck"] = {
      "handheld", "1280x800x60", "hevc", 20000, 2, false, true, 2,
      "Valve Steam Deck LCD — 1280x800, WiFi 5, HEVC decode"
    };
    devices["Steam Deck OLED"] = {
      "handheld", "1280x800x90", "hevc", 25000, 2, true, true, 2,
      "Valve Steam Deck OLED — 1280x800@90Hz, WiFi 6E, HDR"
    };

    devices["ROG Ally"] = {
      "handheld", "1920x1080x120", "hevc", 30000, 2, true, true, 2,
      "ASUS ROG Ally — 1080p@120Hz, WiFi 6E, HDR, AV1 decode"
    };
    devices["ROG Ally X"] = devices["ROG Ally"];

    devices["Legion Go"] = {
      "handheld", "2560x1600x60", "hevc", 35000, 2, true, true, 2,
      "Lenovo Legion Go — 2560x1600, WiFi 6E, HDR"
    };

    // --- Phones ---
    devices["Pixel 10"] = {
      "phone", "2340x1080x120", "av1", 25000, 2, true, true, 3,
      "Google Pixel 10 — 1080x2340, 120Hz, AV1/HEVC, WiFi 6E"
    };
    devices["pxl-10-papi"] = devices["Pixel 10"]; // Michael's specific device
    devices["Pixel10Pro"] = {
      "phone", "2992x1344x120", "av1", 30000, 2, true, true, 3,
      "Google Pixel 10 Pro — 1344x2992, 120Hz LTPO OLED, AV1/HEVC, WiFi 7"
    };
    devices["Pixel 10 Pro"] = devices["Pixel10Pro"];

    devices["Pixel 9"] = {
      "phone", "2424x1080x120", "hevc", 20000, 2, true, true, 3,
      "Google Pixel 9 — 1080p, 120Hz, HEVC, WiFi 6E"
    };

    devices["Galaxy S24"] = {
      "phone", "2340x1080x120", "hevc", 25000, 2, true, true, 3,
      "Samsung Galaxy S24 — 1080p, 120Hz, HEVC/AV1, WiFi 6E"
    };
    devices["Galaxy S24 Ultra"] = {
      "phone", "3120x1440x120", "av1", 35000, 2, true, true, 2,
      "Samsung Galaxy S24 Ultra — 1440p, 120Hz, AV1, WiFi 7"
    };

    devices["iPhone 15"] = {
      "phone", "2556x1179x60", "hevc", 20000, 2, true, true, 3,
      "iPhone 15 — 1179p, 60Hz, HEVC, WiFi 6"
    };
    devices["iPhone 15 Pro"] = {
      "phone", "2556x1179x120", "hevc", 25000, 2, true, true, 3,
      "iPhone 15 Pro — 1179p, 120Hz ProMotion, HEVC, WiFi 6E"
    };
    devices["iPhone 16 Pro"] = {
      "phone", "2622x1206x120", "hevc", 30000, 2, true, true, 3,
      "iPhone 16 Pro — 1206p, 120Hz ProMotion, HEVC/AV1, WiFi 7"
    };

    // --- Tablets ---
    devices["iPad Pro"] = {
      "tablet", "2732x2048x120", "hevc", 40000, 2, true, true, 2,
      "iPad Pro — 2732x2048, 120Hz ProMotion, HEVC, WiFi 6E"
    };
    devices["iPad Air"] = {
      "tablet", "2360x1640x60", "hevc", 30000, 2, false, true, 2,
      "iPad Air — 2360x1640, 60Hz, HEVC, WiFi 6"
    };
    devices["Galaxy Tab S9"] = {
      "tablet", "2560x1600x120", "hevc", 35000, 2, true, true, 2,
      "Samsung Galaxy Tab S9 — 2560x1600, 120Hz, HEVC/AV1"
    };

    // --- Desktop clients ---
    devices["Desktop"] = {
      "desktop", "", "hevc", 80000, 2, true, false, 1,
      "Generic desktop — use client-requested settings, quality preset"
    };
    devices["Moonlight PC"] = devices["Desktop"];

    BOOST_LOG(info) << "device_db: Loaded "sv << devices.size() << " default device profiles"sv;
  }

  /**
   * @brief Load device database from JSON file, falling back to defaults.
   */
  static void load_db() {
    load_defaults();

    auto db_path = platf::appdata() / "device_db.json";
    std::ifstream file(db_path);
    if (!file.is_open()) {
      // Save defaults to disk for user customization
      std::ofstream out(db_path);
      if (out.is_open()) {
        out << get_all_devices_json();
        BOOST_LOG(info) << "device_db: Saved default database to "sv << db_path.string();
      }
      return;
    }

    try {
      auto root = nlohmann::json::parse(file);
      if (!root.is_object()) return;

      for (auto &[name, val] : root.items()) {
        if (!val.is_object()) continue;
        device_t dev;
        dev.type = val.value("type", "");
        dev.display_mode = val.value("display_mode", "");
        dev.preferred_codec = val.value("preferred_codec", "hevc");
        dev.ideal_bitrate_kbps = val.value("ideal_bitrate_kbps", 0);
        dev.color_range = val.value("color_range", 0);
        dev.hdr_capable = val.value("hdr_capable", false);
        dev.virtual_display = val.value("virtual_display", true);
        dev.nvenc_tune = val.value("nvenc_tune", 3);
        dev.notes = val.value("notes", "");
        devices[name] = dev;
      }
      BOOST_LOG(info) << "device_db: Loaded "sv << devices.size() << " device profiles from "sv << db_path.string();
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "device_db: Failed to parse "sv << db_path.string() << ": "sv << e.what();
    }
  }

  void load() {
    std::call_once(load_flag, load_db);
  }

  /**
   * @brief Fuzzy match a device name against the database.
   * Tries: exact match → case-insensitive exact → case-insensitive substring.
   */
  std::optional<device_t> get_device(const std::string &name) {
    load();

    // Exact match
    auto it = devices.find(name);
    if (it != devices.end()) return it->second;

    // Case-insensitive exact match
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    for (const auto &[key, val] : devices) {
      std::string lower_key = key;
      std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
      if (lower_key == lower_name) return val;
    }

    // Case-insensitive substring match (e.g., "pxl-10-papi" contains "pixel 10")
    for (const auto &[key, val] : devices) {
      std::string lower_key = key;
      std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
      if (lower_name.find(lower_key) != std::string::npos ||
          lower_key.find(lower_name) != std::string::npos) {
        return val;
      }
    }

    return std::nullopt;
  }

  optimization_t get_optimization(const std::string &device_name, const std::string &app_name) {
    optimization_t opt;
    opt.source = "device_db";

    auto dev = get_device(device_name);
    if (!dev) {
      opt.reasoning = "Unknown device '" + device_name + "' — using client defaults";
      return opt;
    }

    if (!dev->display_mode.empty())
      opt.display_mode = dev->display_mode;
    if (dev->color_range > 0)
      opt.color_range = dev->color_range;
    opt.hdr = dev->hdr_capable;
    opt.virtual_display = dev->virtual_display;
    if (dev->ideal_bitrate_kbps > 0)
      opt.target_bitrate_kbps = dev->ideal_bitrate_kbps;
    opt.nvenc_tune = dev->nvenc_tune;

    opt.reasoning = dev->type + ": " + dev->notes;
    BOOST_LOG(info) << "device_db: Optimization for \""sv << device_name << "\": "sv
                    << opt.display_mode.value_or("(default)") << ", "sv
                    << opt.target_bitrate_kbps.value_or(0) << "kbps, tune="sv
                    << opt.nvenc_tune.value_or(3);
    return opt;
  }

  std::optional<std::string> normalize_preferred_codec(
    const std::string &device_name,
    const std::string &app_name,
    const std::optional<std::string> &preferred_codec,
    const std::optional<int> &target_bitrate_kbps,
    bool hdr_requested
  ) {
    if (!preferred_codec || *preferred_codec != "hevc") {
      return preferred_codec;
    }

    if (!config::video.linux_display.use_cage_compositor || !config::video.linux_display.headless_mode) {
      return preferred_codec;
    }

    auto dev = get_device(device_name);
    if (!dev) {
      return preferred_codec;
    }

    if (dev->type != "handheld" && dev->type != "phone") {
      return preferred_codec;
    }

    if (hdr_requested) {
      return preferred_codec;
    }

    const auto bitrate_kbps = target_bitrate_kbps.value_or(dev->ideal_bitrate_kbps);
    // Only back off HEVC for truly modest-bitrate handheld UI sessions.
    // At 15 Mbps on Retroid Big Picture, HEVC has already proven stable and
    // forcing H.264 regressed the stream.
    if (bitrate_kbps <= 0 || bitrate_kbps > 9000) {
      return preferred_codec;
    }

    std::string lower_app = app_name;
    std::transform(lower_app.begin(), lower_app.end(), lower_app.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    if (lower_app.find("big picture") == std::string::npos) {
      return preferred_codec;
    }

    return "h264"s;
  }

  std::string get_all_devices_json() {
    load();
    nlohmann::json root = nlohmann::json::object();
    for (const auto &[name, dev] : devices) {
      nlohmann::json entry;
      entry["type"] = dev.type;
      entry["display_mode"] = dev.display_mode;
      entry["preferred_codec"] = dev.preferred_codec;
      entry["ideal_bitrate_kbps"] = dev.ideal_bitrate_kbps;
      entry["color_range"] = dev.color_range;
      entry["hdr_capable"] = dev.hdr_capable;
      entry["virtual_display"] = dev.virtual_display;
      entry["nvenc_tune"] = dev.nvenc_tune;
      entry["notes"] = dev.notes;
      root[name] = entry;
    }
    return root.dump(2);
  }

  void save_device(const std::string &name, const device_t &device) {
    load();
    devices[name] = device;

    // Persist to disk
    auto db_path = platf::appdata() / "device_db.json";
    std::ofstream file(db_path);
    if (file.is_open()) {
      file << get_all_devices_json();
    }
  }

}  // namespace device_db
