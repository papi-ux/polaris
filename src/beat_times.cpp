/**
 * @file src/beat_times.cpp
 * @brief Completion estimates, read from a local dataset rather than a third party.
 */
#include "beat_times.h"

#include "logging.h"
#include "platform/common.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>

#include <nlohmann/json.hpp>

namespace beat_times {

  namespace {

    std::filesystem::path dataset_path() {
      return platf::appdata() / "beat_times.json";
    }

    int64_t seconds_field(const nlohmann::json &entry, const char *key) {
      if (!entry.contains(key) || !entry[key].is_number()) {
        return 0;
      }
      const auto value = entry[key].get<double>();
      return value > 0 ? static_cast<int64_t>(value) : 0;
    }

  }  // namespace

  std::string normalise_name(std::string_view name) {
    std::string folded;
    folded.reserve(name.size());
    for (const unsigned char ch : name) {
      if (std::isalnum(ch)) {
        folded.push_back(static_cast<char>(std::tolower(ch)));
      }
    }
    return folded;
  }

  dataset_t parse(std::string_view json_payload) {
    dataset_t data;
    if (json_payload.empty()) {
      return data;
    }

    try {
      const auto payload = nlohmann::json::parse(json_payload);
      if (!payload.is_object()) {
        return data;
      }

      if (payload.contains("generated_at") && payload["generated_at"].is_number()) {
        data.generated_at = payload["generated_at"].get<int64_t>();
      }

      const auto games = payload.find("games");
      if (games == payload.end() || !games->is_array()) {
        return data;
      }

      for (const auto &entry : *games) {
        if (!entry.is_object()) {
          continue;
        }

        estimate_t estimate;
        estimate.matched_name = entry.value("name", "");
        estimate.url = entry.value("url", "");
        estimate.main_seconds = seconds_field(entry, "main_seconds");
        estimate.extras_seconds = seconds_field(entry, "extras_seconds");
        estimate.completionist_seconds = seconds_field(entry, "completionist_seconds");

        // A row with a name and no figures says nothing, and would draw an empty gauge.
        if (estimate.matched_name.empty() || !estimate.has_any()) {
          continue;
        }

        const auto appid = entry.value("steam_appid", "");
        if (!appid.empty()) {
          data.by_steam_appid.emplace(appid, estimate);
        }

        const auto key = normalise_name(estimate.matched_name);
        if (!key.empty()) {
          // First spelling wins: a later duplicate is ambiguous, and guessing between
          // two rows is how a wrong number gets presented as a fact.
          data.by_name.emplace(key, estimate);
        }
      }
    } catch (...) {
      return {};
    }

    return data;
  }

  const dataset_t &dataset() {
    static std::mutex guard;
    static dataset_t cached;
    static std::filesystem::file_time_type loaded_stamp {};
    static bool loaded = false;

    const std::lock_guard<std::mutex> lock {guard};

    std::error_code ec;
    const auto path = dataset_path();
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
      // No dataset is a normal state, not a failure: the gauge simply has no estimates.
      if (loaded) {
        cached = {};
        loaded = false;
      }
      return cached;
    }

    if (loaded && stamp == loaded_stamp) {
      return cached;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
      return cached;
    }

    const std::string payload {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    cached = parse(payload);
    loaded_stamp = stamp;
    loaded = true;

    BOOST_LOG(info) << "Beat times: loaded "sv << cached.by_steam_appid.size()
                    << " entries by app id and "sv << cached.by_name.size() << " by name"sv;
    return cached;
  }

  std::optional<estimate_t> lookup(const dataset_t &data, std::string_view steam_appid, std::string_view name) {
    if (!steam_appid.empty()) {
      const auto by_id = data.by_steam_appid.find(std::string(steam_appid));
      if (by_id != data.by_steam_appid.end()) {
        return by_id->second;
      }
    }

    const auto key = normalise_name(name);
    if (key.empty()) {
      return std::nullopt;
    }

    const auto by_name = data.by_name.find(key);
    if (by_name == data.by_name.end()) {
      return std::nullopt;
    }
    return by_name->second;
  }

}  // namespace beat_times
