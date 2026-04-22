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
  static std::unordered_map<std::string, std::string> canonical_aliases;
  static std::unordered_map<std::string, std::string> friendly_aliases;
  static std::once_flag load_flag;

  static nlohmann::json build_devices_json() {
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
    return root;
  }

  static void write_defaults_to_disk(const std::filesystem::path &db_path) {
    std::ofstream out(db_path, std::ios::trunc);
    if (out.is_open()) {
      out << build_devices_json().dump(2);
      BOOST_LOG(info) << "device_db: Saved default database to "sv << db_path.string();
    }
  }

  static std::string normalize_name_token(const std::string &name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (const unsigned char ch : name) {
      if (std::isalnum(ch)) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
      }
    }
    return normalized;
  }

  static void register_canonical_alias(const std::string &alias, const std::string &canonical) {
    canonical_aliases[normalize_name_token(alias)] = canonical;
  }

  static void register_friendly_alias(const std::string &alias, const std::string &friendly) {
    friendly_aliases[normalize_name_token(alias)] = friendly;
  }

  /**
   * @brief Embedded default device database.
   * Curated profiles for known streaming client devices.
   */
  static void load_defaults() {
    // --- Handhelds ---
    devices["RetroidPocket6"] = {
      "handheld", "1920x1080x60", "hevc", 15000, 2, false, true, 3,
      "Retroid Pocket 6 — Android handheld, 1080p landscape, WiFi 6"
    };
    devices["RP6"] = devices["RetroidPocket6"];
    devices["Retroid Pocket 6"] = devices["RetroidPocket6"];
    register_canonical_alias("RetroidPocket6", "RetroidPocket6");
    register_canonical_alias("RP6", "RetroidPocket6");
    register_canonical_alias("Retroid Pocket 6", "RetroidPocket6");
    register_friendly_alias("RetroidPocket6", "Retroid Pocket 6");
    register_friendly_alias("RP6", "Retroid Pocket 6");
    register_friendly_alias("Retroid Pocket 6", "Retroid Pocket 6");

    devices["RetroidPocketFlip2"] = {
      "handheld", "1920x1080x60", "hevc", 15000, 2, false, true, 3,
      "Retroid Pocket Flip 2 — Android clamshell handheld, 5.5-inch 1080p, WiFi 6"
    };
    devices["Retroid Pocket Flip 2"] = devices["RetroidPocketFlip2"];
    register_canonical_alias("RetroidPocketFlip2", "RetroidPocketFlip2");
    register_canonical_alias("Retroid Pocket Flip 2", "RetroidPocketFlip2");
    register_friendly_alias("RetroidPocketFlip2", "Retroid Pocket Flip 2");
    register_friendly_alias("Retroid Pocket Flip 2", "Retroid Pocket Flip 2");

    devices["RP5"] = {
      "handheld", "1280x720x60", "hevc", 10000, 2, false, true, 3,
      "Retroid Pocket 5 — Android handheld, 720p, WiFi 5"
    };

    devices["Steam Deck"] = {
      "handheld", "1280x800x60", "hevc", 20000, 2, false, true, 2,
      "Valve Steam Deck LCD — 1280x800, WiFi 5, HEVC decode"
    };
    register_friendly_alias("Steam Deck", "Steam Deck");
    devices["Steam Deck OLED"] = {
      "handheld", "1280x800x90", "hevc", 25000, 2, true, true, 2,
      "Valve Steam Deck OLED — 1280x800@90Hz, WiFi 6E, HDR"
    };
    register_friendly_alias("Steam Deck OLED", "Steam Deck OLED");

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
    register_canonical_alias("Pixel 10", "Pixel 10");
    register_canonical_alias("pxl-10-papi", "Pixel 10");
    register_friendly_alias("Pixel 10", "Pixel 10");
    register_friendly_alias("pxl-10-papi", "Pixel 10");
    devices["Pixel10Pro"] = {
      "phone", "2992x1344x120", "av1", 30000, 2, true, true, 3,
      "Google Pixel 10 Pro — 1344x2992, 120Hz LTPO OLED, AV1/HEVC, WiFi 7"
    };
    devices["Pixel 10 Pro"] = devices["Pixel10Pro"];
    register_canonical_alias("Pixel10Pro", "Pixel10Pro");
    register_canonical_alias("Pixel 10 Pro", "Pixel10Pro");
    register_friendly_alias("Pixel10Pro", "Pixel 10 Pro");
    register_friendly_alias("Pixel 10 Pro", "Pixel 10 Pro");

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
      write_defaults_to_disk(db_path);
      return;
    }

    if (file.peek() == std::ifstream::traits_type::eof()) {
      BOOST_LOG(warning) << "device_db: "sv << db_path.string() << " is empty; restoring defaults"sv;
      file.close();
      write_defaults_to_disk(db_path);
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
    const auto canonical = canonicalize_name(name);
    auto it = devices.find(canonical);
    if (it != devices.end()) return it->second;
    return std::nullopt;
  }

  std::string canonicalize_name(const std::string &name) {
    load();

    if (name.empty()) {
      return name;
    }

    if (devices.find(name) != devices.end()) {
      const auto alias = canonical_aliases.find(normalize_name_token(name));
      return alias != canonical_aliases.end() ? alias->second : name;
    }

    const auto normalized_name = normalize_name_token(name);
    const auto alias = canonical_aliases.find(normalized_name);
    if (alias != canonical_aliases.end()) {
      return alias->second;
    }

    for (const auto &[key, val] : devices) {
      (void) val;
      const auto normalized_key = normalize_name_token(key);
      if (normalized_key == normalized_name) {
        const auto exact_alias = canonical_aliases.find(normalized_key);
        return exact_alias != canonical_aliases.end() ? exact_alias->second : key;
      }
    }

    for (const auto &[key, val] : devices) {
      (void) val;
      const auto normalized_key = normalize_name_token(key);
      if (normalized_name.find(normalized_key) != std::string::npos ||
          normalized_key.find(normalized_name) != std::string::npos) {
        const auto fuzzy_alias = canonical_aliases.find(normalized_key);
        return fuzzy_alias != canonical_aliases.end() ? fuzzy_alias->second : key;
      }
    }

    return name;
  }

  std::string friendly_name(const std::string &name) {
    load();

    if (name.empty()) {
      return name;
    }

    std::string base_name = name;
    std::string suffix;
    const auto duplicate_suffix = name.find_last_of('(');
    if (duplicate_suffix != std::string::npos && duplicate_suffix > 0 && name.back() == ')') {
      base_name = name.substr(0, duplicate_suffix - 1);
      suffix = name.substr(duplicate_suffix - 1);
    }

    const auto alias = friendly_aliases.find(normalize_name_token(base_name));
    if (alias != friendly_aliases.end()) {
      return alias->second + suffix;
    }

    return name;
  }

  optimization_t get_optimization(const std::string &device_name, const std::string &app_name) {
    optimization_t opt;
    opt.source = "device_db";
    opt.cache_status = "fallback";
    opt.confidence = "high";
    opt.signals_used = {"device_profile"};

    auto dev = get_device(device_name);
    if (!dev) {
      opt.reasoning = "Unknown device '" + device_name + "' — using client defaults";
      opt.confidence = "low";
      opt.signals_used = {"safe_defaults"};
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
    return build_devices_json().dump(2);
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
