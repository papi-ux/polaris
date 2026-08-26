/**
 * @file src/recovery_profile.cpp
 * @brief Durable, owner-and-game-scoped next-launch recovery profiles.
 */

#include "recovery_profile.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <string_view>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>

#include "private_state_file.h"

using namespace std::literals;

namespace recovery_profile {
  namespace {
    constexpr std::size_t maximum_store_bytes = 256 * 1024;
    constexpr int schema_version = 2;
    std::mutex store_mutex;

    struct store_t {
      std::vector<record_t> records;
    };

    bool nonblank(std::string_view value, std::size_t maximum = 256) {
      return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
          return byte >= 0x20 && byte != 0x7f;
        });
    }

    bool valid_profile(const safe_profile_t &profile) {
      const bool valid_mode =
        profile.stream_display_mode == "headless_stream" ||
        profile.stream_display_mode == "host_virtual_display" ||
        profile.stream_display_mode == "gamescope_stream" ||
        profile.stream_display_mode == "windowed_stream" ||
        profile.stream_display_mode == "desktop_display";
      const bool valid_codec =
        profile.preferred_codec == "h264" ||
        profile.preferred_codec == "hevc" ||
        profile.preferred_codec == "av1";
      return valid_mode && valid_codec &&
        profile.width >= 320 && profile.width <= 16384 &&
        profile.height >= 240 && profile.height <= 16384 &&
        profile.target_fps >= 15 && profile.target_fps <= 240 &&
        profile.target_bitrate_kbps >= 1000 && profile.target_bitrate_kbps <= 300000;
    }

    nlohmann::json record_json(const record_t &record) {
      return {
        {"run_id", record.run_id},
        {"owner_uuid", record.owner_uuid},
        {"app_uuid", record.app_uuid},
        {"source_result_id", record.source_result_id},
        {"state", record.state},
        {"created_at", record.created_at},
        {"expires_at", record.expires_at},
        {"queued_launch_instance_id", record.queued_launch_instance_id},
        {"queued_session_generation", record.queued_session_generation},
        {"optimizer_prepared", record.optimizer_prepared},
        {"profile", profile_json(record.profile)}
      };
    }

    std::optional<safe_profile_t> parse_profile(const nlohmann::json &value) {
      if (!value.is_object()) return std::nullopt;
      safe_profile_t profile;
      profile.stream_display_mode = value.value("stream_display_mode", std::string {});
      profile.width = value.value("width", 0);
      profile.height = value.value("height", 0);
      profile.target_fps = value.value("target_fps", 0);
      profile.target_bitrate_kbps = value.value("target_bitrate_kbps", 0);
      profile.preferred_codec = value.value("preferred_codec", std::string {});
      profile.hdr = value.value("hdr", false);
      if (!valid_profile(profile)) return std::nullopt;
      return profile;
    }

    std::optional<record_t> parse_record(const nlohmann::json &value) {
      if (!value.is_object()) return std::nullopt;
      record_t record;
      record.run_id = value.value("run_id", std::string {});
      record.owner_uuid = value.value("owner_uuid", std::string {});
      record.app_uuid = value.value("app_uuid", std::string {});
      record.source_result_id = value.value("source_result_id", std::string {});
      record.state = value.value("state", std::string {});
      record.created_at = value.value("created_at", std::int64_t {0});
      record.expires_at = value.value("expires_at", std::int64_t {0});
      record.queued_launch_instance_id = value.value("queued_launch_instance_id", std::string {});
      record.queued_session_generation = value.value("queued_session_generation", std::uint64_t {0});
      record.optimizer_prepared = value.value("optimizer_prepared", false);
      const auto profile = parse_profile(value.value("profile", nlohmann::json::object()));
      const bool valid_state = record.state == "queued" || record.state == "applied" || record.state == "expired";
      if (!nonblank(record.run_id) || !nonblank(record.owner_uuid) ||
          !nonblank(record.app_uuid) || !nonblank(record.source_result_id) ||
          !nonblank(record.queued_launch_instance_id) || record.queued_session_generation == 0 ||
          !valid_state || record.created_at <= 0 ||
          record.expires_at <= record.created_at || !profile) {
        return std::nullopt;
      }
      record.profile = *profile;
      return record;
    }

    enum class load_status_e { ok, rejected, io_error };

    struct load_result_t {
      load_status_e status = load_status_e::io_error;
      store_t store;
    };

    load_result_t load(const std::filesystem::path &path) {
      const auto stored = private_state_file::read_secure(path, maximum_store_bytes);
      if (stored.status == private_state_file::read_status_e::missing) {
        return {load_status_e::ok, {}};
      }
      if (stored.status == private_state_file::read_status_e::rejected) {
        return {load_status_e::rejected, {}};
      }
      if (stored.status != private_state_file::read_status_e::ok) {
        return {load_status_e::io_error, {}};
      }

      try {
        const auto root = nlohmann::json::parse(stored.payload);
        if (!root.is_object() || root.value("version", 0) != schema_version ||
            !root.contains("records") || !root["records"].is_array() ||
            root["records"].size() > 1024) {
          return {load_status_e::rejected, {}};
        }
        store_t result;
        for (const auto &value : root["records"]) {
          const auto record = parse_record(value);
          if (!record) return {load_status_e::rejected, {}};
          const auto duplicate = std::find_if(result.records.begin(), result.records.end(), [&](const record_t &existing) {
            return existing.run_id == record->run_id ||
              (existing.owner_uuid == record->owner_uuid && existing.app_uuid == record->app_uuid);
          });
          if (duplicate != result.records.end()) return {load_status_e::rejected, {}};
          result.records.push_back(*record);
        }
        return {load_status_e::ok, std::move(result)};
      } catch (...) {
        return {load_status_e::rejected, {}};
      }
    }

    bool persist(const std::filesystem::path &path, const store_t &store) {
      nlohmann::json root;
      root["version"] = schema_version;
      root["records"] = nlohmann::json::array();
      for (const auto &record : store.records) {
        root["records"].push_back(record_json(record));
      }
      return static_cast<bool>(private_state_file::write_atomic(path, root.dump()));
    }

    bool expire_and_prune(store_t &store, std::int64_t now) {
      bool changed = false;
      for (auto &record : store.records) {
        if (record.state == "queued" && now >= record.expires_at) {
          record.state = "expired";
          changed = true;
        }
      }
      const auto old_size = store.records.size();
      std::erase_if(store.records, [now](const record_t &record) {
        // Terminal receipts remain for one additional day so a reconnect can
        // reconstruct applied/expired state without keeping unbounded history.
        return record.state != "queued" && now >= record.expires_at + lifetime_seconds;
      });
      return changed || old_size != store.records.size();
    }

    bool contains(const std::vector<std::string> &values, const std::string &candidate) {
      return std::find(values.begin(), values.end(), candidate) != values.end();
    }

    std::optional<std::string> safest_allowed_mode(const std::vector<std::string> &allowed) {
      for (const auto candidate : {
             "headless_stream"sv,
             "host_virtual_display"sv,
             "gamescope_stream"sv,
             "windowed_stream"sv,
             "desktop_display"sv,
           }) {
        if (contains(allowed, std::string {candidate})) return std::string {candidate};
      }
      return std::nullopt;
    }

    std::string next_run_id() {
      std::array<unsigned char, 16> bytes {};
      std::random_device random;
      for (auto &byte : bytes) byte = static_cast<unsigned char>(random());
      constexpr char hex[] = "0123456789abcdef";
      std::string id = "recovery-run-";
      id.reserve(id.size() + bytes.size() * 2);
      for (const auto byte : bytes) {
        id.push_back(hex[(byte >> 4) & 0x0f]);
        id.push_back(hex[byte & 0x0f]);
      }
      return id;
    }

    nlohmann::json unavailable(load_status_e status) {
      return {
        {"status", false},
        {"changed", false},
        {"state", "rejected"},
        {"error", status == load_status_e::rejected ?
          "Recovery profile state failed secure validation." :
          "Recovery profile state is unavailable."}
      };
    }

    auto find_key(store_t &store, const std::string &owner_uuid, const std::string &app_uuid) {
      return std::find_if(store.records.begin(), store.records.end(), [&](const record_t &record) {
        return record.owner_uuid == owner_uuid && record.app_uuid == app_uuid;
      });
    }

    auto find_run(store_t &store,
                  const std::string &run_id,
                  const std::optional<std::string> &owner_scope) {
      return std::find_if(store.records.begin(), store.records.end(), [&](const record_t &record) {
        return record.run_id == run_id &&
          (!owner_scope || record.owner_uuid == *owner_scope);
      });
    }

    bool launch_matches(const record_t &record,
                        const observed_launch_t &observed,
                        nlohmann::json &mismatches) {
      const auto &profile = record.profile;
      const auto mismatch = [&mismatches](std::string_view field) {
        mismatches.push_back(field);
      };
      if (!observed.streaming) mismatch("streaming");
      if (observed.owner_uuid != record.owner_uuid) mismatch("owner");
      if (observed.app_uuid != record.app_uuid) mismatch("app");
      if (observed.stream_display_mode != profile.stream_display_mode) mismatch("stream_display_mode");
      if (observed.width != profile.width || observed.height != profile.height) mismatch("resolution");
      if (observed.target_fps != profile.target_fps) mismatch("target_fps");
      if (observed.bitrate_kbps != profile.target_bitrate_kbps) mismatch("target_bitrate_kbps");
      if (observed.codec != profile.preferred_codec) mismatch("preferred_codec");
      if (observed.hdr != profile.hdr) mismatch("hdr");
      if (observed.launch_instance_id.empty() ||
          observed.launch_instance_id == record.queued_launch_instance_id) {
        mismatch("fresh_launch");
      }
      if (observed.session_generation == 0) mismatch("session_generation");
      return mismatches.empty();
    }
  }  // namespace

  std::int64_t now_epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()
    ).count();
  }

  std::optional<std::string> resolve_canonical_app_uuid(
    std::string_view query,
    const std::vector<app_identity_t> &apps
  ) {
    if (query.empty()) return std::nullopt;
    const auto unique_match = [&](const auto &member) -> std::optional<std::string> {
      std::optional<std::string> result;
      for (const auto &app : apps) {
        const auto &candidate = app.*member;
        if (candidate.empty() || !boost::iequals(candidate, query)) continue;
        if (result) return std::nullopt;
        result = app.uuid;
      }
      return result;
    };
    if (const auto uuid = unique_match(&app_identity_t::uuid)) return uuid;
    if (const auto id = unique_match(&app_identity_t::id)) return id;
    return unique_match(&app_identity_t::name);
  }

  nlohmann::json profile_json(const safe_profile_t &profile) {
    return {
      {"stream_display_mode", profile.stream_display_mode},
      {"width", profile.width},
      {"height", profile.height},
      {"target_fps", profile.target_fps},
      {"target_bitrate_kbps", profile.target_bitrate_kbps},
      {"preferred_codec", profile.preferred_codec},
      {"hdr", profile.hdr},
      {"preserve_paired_resolution", true},
      {"requires_fresh_launch", true}
    };
  }

  nlohmann::json receipt_json(const record_t &record) {
    return {
      {"status", true},
      {"changed", false},
      {"state", record.state},
      {"recovery_state", record.state},
      {"run_id", record.run_id},
      {"source_result_id", record.source_result_id},
      {"app_uuid", record.app_uuid},
      {"expires_at", record.expires_at},
      {"safe_profile", profile_json(record.profile)},
      {"undo", {
        {"supported", record.state == "queued"},
        {"available", record.state == "queued"},
        {"action_id", "undo"},
        {"run_id", record.run_id},
        {"endpoint", "/api/doctor/action"},
        {"paired_endpoint", "/polaris/v1/doctor/action"}
      }}
    };
  }

  nlohmann::json overlay_optimization(nlohmann::json normalized,
                                      const record_t &record) {
    if (record.state != "queued" || !record.optimizer_prepared ||
        !valid_profile(record.profile)) return normalized;

    const auto &effective_profile = record.profile;
    const auto display_mode =
      std::to_string(effective_profile.width) + "x" +
      std::to_string(effective_profile.height) + "x" +
      std::to_string(effective_profile.target_fps);

    normalized["display_mode"] = display_mode;
    normalized["target_bitrate_kbps"] = effective_profile.target_bitrate_kbps;
    normalized["preferred_codec"] = effective_profile.preferred_codec;
    normalized["hdr"] = effective_profile.hdr;
    normalized["virtual_display"] = effective_profile.stream_display_mode == "host_virtual_display";
    normalized["stream_display_mode"] = effective_profile.stream_display_mode;
    normalized["requires_fresh_launch"] = true;
    normalized["recovery_run_id"] = record.run_id;
    normalized["recovery_state"] = record.state;
    normalized["recovery_profile"] = profile_json(effective_profile);
    normalized["auto_action"] = "apply_recovery";

    if (!normalized.contains("stability") || !normalized["stability"].is_object()) {
      normalized["stability"] = nlohmann::json::object();
    }
    normalized["stability"]["relaunch_required"] = true;
    normalized["stability"]["auto_action"] = "apply_recovery";
    normalized["stability"]["safe_profile"] = normalized["recovery_profile"];
    if (!normalized.contains("recovery_policy") || !normalized["recovery_policy"].is_object()) {
      normalized["recovery_policy"] = nlohmann::json::object();
    }
    normalized["recovery_policy"]["state"] = "recovery_queued";
    normalized["recovery_policy"]["relaunch_required"] = true;
    return normalized;
  }

  nlohmann::json queue(const std::filesystem::path &path,
                       const std::string &owner_uuid,
                       const std::string &app_uuid,
                       const std::string &source_result_id,
                       const safe_profile_t &profile,
                       const std::string &current_launch_instance_id,
                       std::uint64_t current_session_generation,
                       std::int64_t now) {
    if (!nonblank(owner_uuid) || !nonblank(app_uuid) ||
        !nonblank(source_result_id) || !valid_profile(profile) ||
        !nonblank(current_launch_instance_id) || current_session_generation == 0 || now <= 0) {
      return {{"status", false}, {"changed", false}, {"state", "rejected"},
              {"error", "Current Doctor evidence cannot produce a safe next-launch profile."}};
    }
    std::lock_guard<std::mutex> lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool expired_records_changed = expire_and_prune(loaded.store, now);

    auto existing = find_key(loaded.store, owner_uuid, app_uuid);
    if (existing != loaded.store.records.end() &&
        (existing->state == "queued" || existing->state == "applied") &&
        existing->source_result_id == source_result_id) {
      auto receipt = receipt_json(*existing);
      receipt["idempotent"] = true;
      if (expired_records_changed && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      return receipt;
    }

    record_t record;
    record.run_id = next_run_id();
    record.owner_uuid = owner_uuid;
    record.app_uuid = app_uuid;
    record.source_result_id = source_result_id;
    record.state = "queued";
    record.created_at = now;
    record.expires_at = now + lifetime_seconds;
    record.queued_launch_instance_id = current_launch_instance_id;
    record.queued_session_generation = current_session_generation;
    record.optimizer_prepared = false;
    record.profile = profile;
    if (existing == loaded.store.records.end()) {
      loaded.store.records.push_back(record);
    } else {
      *existing = record;
    }
    if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    auto receipt = receipt_json(record);
    receipt["changed"] = true;
    receipt["idempotent"] = false;
    return receipt;
  }

  nlohmann::json status(const std::filesystem::path &path,
                        const std::string &owner_uuid,
                        const std::string &app_uuid,
                        std::int64_t now) {
    if (owner_uuid.empty() || app_uuid.empty()) {
      return {{"status", true}, {"changed", false}, {"state", "none"}, {"recovery_state", "none"}};
    }
    std::lock_guard<std::mutex> lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool changed = expire_and_prune(loaded.store, now);
    if (changed && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    const auto record = find_key(loaded.store, owner_uuid, app_uuid);
    if (record == loaded.store.records.end()) {
      return {{"status", true}, {"changed", false}, {"state", "none"}, {"recovery_state", "none"}};
    }
    return receipt_json(*record);
  }

  std::optional<record_t> queued(const std::filesystem::path &path,
                                 const std::string &owner_uuid,
                                 const std::string &app_uuid,
                                 std::int64_t now) {
    if (owner_uuid.empty() || app_uuid.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return std::nullopt;
    const bool changed = expire_and_prune(loaded.store, now);
    if (changed && !persist(path, loaded.store)) return std::nullopt;
    const auto record = find_key(loaded.store, owner_uuid, app_uuid);
    if (record == loaded.store.records.end() || record->state != "queued") return std::nullopt;
    return *record;
  }

  std::optional<record_t> prepare_for_optimizer(const std::filesystem::path &path,
                                                const std::string &owner_uuid,
                                                const std::string &app_uuid,
                                                const optimizer_constraints_t &constraints,
                                                std::int64_t now) {
    if (owner_uuid.empty() || app_uuid.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return std::nullopt;
    bool changed = expire_and_prune(loaded.store, now);
    const auto record = find_key(loaded.store, owner_uuid, app_uuid);
    if (record == loaded.store.records.end() || record->state != "queued") {
      if (changed && !persist(path, loaded.store)) return std::nullopt;
      return std::nullopt;
    }

    const bool valid_paired_resolution =
      constraints.paired_width >= 320 && constraints.paired_width <= 16384 &&
      constraints.paired_height >= 240 && constraints.paired_height <= 16384;
    if (valid_paired_resolution &&
        (record->profile.width != constraints.paired_width ||
         record->profile.height != constraints.paired_height)) {
      record->profile.width = constraints.paired_width;
      record->profile.height = constraints.paired_height;
      changed = true;
    }

    if (!contains(constraints.supported_codecs, "h264") ||
        constraints.allowed_stream_display_modes.empty()) {
      if (changed && !persist(path, loaded.store)) return std::nullopt;
      return std::nullopt;
    }
    if (!contains(constraints.supported_codecs, record->profile.preferred_codec)) {
      record->profile.preferred_codec =
        record->profile.hdr && contains(constraints.supported_codecs, "hevc") ? "hevc" : "h264";
      changed = true;
    }
    if (record->profile.hdr &&
        (!constraints.hdr_supported || record->profile.preferred_codec == "h264")) {
      record->profile.hdr = false;
      changed = true;
    }
    if (!contains(constraints.allowed_stream_display_modes, record->profile.stream_display_mode)) {
      const auto fallback_mode = safest_allowed_mode(constraints.allowed_stream_display_modes);
      if (!fallback_mode) {
        if (changed && !persist(path, loaded.store)) return std::nullopt;
        return std::nullopt;
      }
      record->profile.stream_display_mode = *fallback_mode;
      changed = true;
    }
    if (!record->optimizer_prepared) {
      record->optimizer_prepared = true;
      changed = true;
    }
    if (changed && !persist(path, loaded.store)) return std::nullopt;
    return *record;
  }

  nlohmann::json statuses_for_owner(const std::filesystem::path &path,
                                    const std::string &owner_uuid,
                                    std::int64_t now) {
    auto receipts = nlohmann::json::array();
    if (owner_uuid.empty()) return receipts;
    std::lock_guard<std::mutex> lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return receipts;
    const bool changed = expire_and_prune(loaded.store, now);
    if (changed && !persist(path, loaded.store)) return nlohmann::json::array();
    for (const auto &record : loaded.store.records) {
      if (record.owner_uuid == owner_uuid) receipts.push_back(receipt_json(record));
    }
    return receipts;
  }

  nlohmann::json undo(const std::filesystem::path &path,
                      const std::string &owner_uuid,
                      const std::string &app_uuid,
                      const std::string &run_id,
                      std::int64_t now) {
    std::lock_guard<std::mutex> lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool expired_records_changed = expire_and_prune(loaded.store, now);
    const auto record = find_key(loaded.store, owner_uuid, app_uuid);
    if (record == loaded.store.records.end() || record->run_id != run_id || record->state != "queued") {
      if (expired_records_changed && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      return {{"status", false}, {"changed", false}, {"state", "rejected"},
              {"error", "This queued recovery profile is no longer available to undo."}};
    }
    loaded.store.records.erase(record);
    if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    return {{"status", true}, {"changed", true}, {"state", "undone"},
            {"recovery_state", "undone"}, {"run_id", run_id}, {"app_uuid", app_uuid},
            {"undo", {{"supported", false}, {"available", false}}}};
  }

  nlohmann::json undo_run(const std::filesystem::path &path,
                          const std::string &run_id,
                          const std::optional<std::string> &owner_scope,
                          std::int64_t now) {
    if (!nonblank(run_id) || (owner_scope && !nonblank(*owner_scope))) {
      return {{"status", false}, {"changed", false}, {"state", "rejected"},
              {"error", "A valid queued recovery run is required for Undo."}};
    }
    std::lock_guard<std::mutex> lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool expired_records_changed = expire_and_prune(loaded.store, now);
    const auto record = find_run(loaded.store, run_id, owner_scope);
    if (record == loaded.store.records.end() || record->state != "queued") {
      if (expired_records_changed && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      return {{"status", false}, {"changed", false}, {"state", "rejected"},
              {"error", "This queued recovery profile is no longer available to undo."}};
    }
    const auto app_uuid = record->app_uuid;
    loaded.store.records.erase(record);
    if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    return {{"status", true}, {"changed", true}, {"state", "undone"},
            {"recovery_state", "undone"}, {"run_id", run_id}, {"app_uuid", app_uuid},
            {"undo", {{"supported", false}, {"available", false}}}};
  }

  nlohmann::json verify(const std::filesystem::path &path,
                        const std::string &owner_uuid,
                        const std::string &app_uuid,
                        const std::string &run_id,
                        const observed_launch_t &observed,
                        std::int64_t now) {
    std::lock_guard<std::mutex> lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool changed = expire_and_prune(loaded.store, now);
    auto record = find_key(loaded.store, owner_uuid, app_uuid);
    if (record == loaded.store.records.end() || record->run_id != run_id) {
      if (changed && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      return {{"status", false}, {"changed", false}, {"state", "expired"},
              {"recovery_state", "expired"}, {"error", "Recovery run not found or expired."}};
    }
    if (record->state == "applied") {
      if (changed && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      auto receipt = receipt_json(*record);
      receipt["idempotent"] = true;
      return receipt;
    }
    if (record->state == "expired") {
      if (changed && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      auto receipt = receipt_json(*record);
      receipt["status"] = false;
      receipt["changed"] = false;
      receipt["idempotent"] = true;
      receipt["error"] = "Recovery run expired before a matching launch was verified.";
      return receipt;
    }

    auto mismatches = nlohmann::json::array();
    if (!launch_matches(*record, observed, mismatches)) {
      if (changed && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      auto receipt = receipt_json(*record);
      receipt["status"] = false;
      receipt["changed"] = false;
      receipt["state"] = "rejected";
      receipt["recovery_state"] = "queued";
      receipt["mismatches"] = std::move(mismatches);
      receipt["error"] = "The connected stream does not match the queued recovery profile; it remains queued.";
      return receipt;
    }

    record->state = "applied";
    if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    auto receipt = receipt_json(*record);
    receipt["changed"] = true;
    receipt["idempotent"] = false;
    return receipt;
  }
}  // namespace recovery_profile
