/**
 * @file src/client_profiles.cpp
 * @brief Definitions for per-client display profile support.
 */

// standard includes
#include <fstream>
#include <mutex>
#include <unordered_map>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "client_profiles.h"
#include "logging.h"
#include "platform/common.h"

using namespace std::literals;

namespace client_profiles {

  static std::unordered_map<std::string, client_profile_t> profiles;
  static std::once_flag load_flag;

  /**
   * @brief Internal: parse the JSON file and populate the profiles map.
   */
  static void load_profiles() {
    auto profile_path = platf::appdata() / "client_profiles.json";

    std::ifstream file(profile_path);
    if (!file.is_open()) {
      BOOST_LOG(info) << "client_profiles: No profile file found at "sv << profile_path.string() << ", per-client profiles disabled"sv;
      return;
    }

    nlohmann::json root;
    try {
      root = nlohmann::json::parse(file);
    }
    catch (const nlohmann::json::parse_error &e) {
      BOOST_LOG(error) << "client_profiles: Failed to parse "sv << profile_path.string() << ": "sv << e.what();
      return;
    }

    if (!root.is_object()) {
      BOOST_LOG(error) << "client_profiles: Expected top-level JSON object in "sv << profile_path.string();
      return;
    }

    for (auto &[name, value] : root.items()) {
      if (!value.is_object()) {
        BOOST_LOG(warning) << "client_profiles: Skipping non-object entry for client \""sv << name << '"';
        continue;
      }

      client_profile_t profile;

      if (value.contains("output_name") && value["output_name"].is_string()) {
        profile.output_name = value["output_name"].get<std::string>();
      }

      if (value.contains("color_range") && value["color_range"].is_number_integer()) {
        profile.color_range = value["color_range"].get<int>();
      }

      if (value.contains("hdr") && value["hdr"].is_boolean()) {
        profile.hdr = value["hdr"].get<bool>();
      }

      if (value.contains("mac_address") && value["mac_address"].is_string()) {
        profile.mac_address = value["mac_address"].get<std::string>();
      }

      BOOST_LOG(info) << "client_profiles: Loaded profile for \""sv << name << "\""sv
                      << " output_name="sv << (profile.output_name.empty() ? "(default)"s : profile.output_name)
                      << " color_range="sv << (profile.color_range.has_value() ? std::to_string(profile.color_range.value()) : "(default)"s)
                      << " hdr="sv << (profile.hdr.has_value() ? (profile.hdr.value() ? "true"s : "false"s) : "(default)"s)
                      << " mac_address="sv << (profile.mac_address.empty() ? "(none)"s : profile.mac_address);

      profiles[name] = std::move(profile);
    }

    BOOST_LOG(info) << "client_profiles: Loaded "sv << profiles.size() << " client profile(s)"sv;
  }

  void load() {
    std::call_once(load_flag, load_profiles);
  }

  std::optional<client_profile_t> get_client_profile(const std::string &device_name) {
    // Ensure profiles are loaded (idempotent via call_once)
    load();

    auto it = profiles.find(device_name);
    if (it != profiles.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  /**
   * @brief Internal: write the current profiles map to disk.
   */
  static void save_profiles_to_disk() {
    auto profile_path = platf::appdata() / "client_profiles.json";

    nlohmann::json root = nlohmann::json::object();
    for (const auto &[name, profile] : profiles) {
      nlohmann::json entry;
      if (!profile.output_name.empty()) entry["output_name"] = profile.output_name;
      if (profile.color_range.has_value()) entry["color_range"] = profile.color_range.value();
      if (profile.hdr.has_value()) entry["hdr"] = profile.hdr.value();
      if (!profile.mac_address.empty()) entry["mac_address"] = profile.mac_address;
      root[name] = entry;
    }

    std::ofstream file(profile_path);
    if (file.is_open()) {
      file << root.dump(2);
      BOOST_LOG(info) << "client_profiles: Saved "sv << profiles.size() << " profile(s) to "sv << profile_path.string();
    } else {
      BOOST_LOG(error) << "client_profiles: Failed to write to "sv << profile_path.string();
    }
  }

  std::string get_all_profiles_json() {
    load();
    nlohmann::json root = nlohmann::json::object();
    for (const auto &[name, profile] : profiles) {
      nlohmann::json entry;
      entry["output_name"] = profile.output_name;
      entry["color_range"] = profile.color_range.has_value() ? profile.color_range.value() : 0;
      entry["hdr"] = profile.hdr.has_value() ? profile.hdr.value() : false;
      entry["mac_address"] = profile.mac_address;
      root[name] = entry;
    }
    return root.dump();
  }

  void save_client_profile(const std::string &device_name, const client_profile_t &profile) {
    load();
    profiles[device_name] = profile;
    save_profiles_to_disk();
  }

  bool delete_client_profile(const std::string &device_name) {
    load();
    auto it = profiles.find(device_name);
    if (it == profiles.end()) return false;
    profiles.erase(it);
    save_profiles_to_disk();
    return true;
  }

}  // namespace client_profiles
