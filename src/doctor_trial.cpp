/**
 * @file src/doctor_trial.cpp
 * @brief Durable one-dimension Doctor v2 trial state.
 */
#include "doctor_trial.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>
#include <random>
#include <string_view>
#include <vector>

#include "private_state_file.h"

namespace doctor_trial {
  namespace {
    constexpr std::size_t maximum_store_bytes = 256 * 1024;
    constexpr int schema_version = 1;
    std::mutex store_mutex;

    struct metrics_t {
      double coverage = 0.0;
      double target_fps = 0.0;
      double rendered_fps = 0.0;
      double pacing_error_pct = 0.0;
      double media_loss_pct = 0.0;
      double rtt_ms = 0.0;
      double decode_latency_ms = 0.0;
      std::optional<double> host_processing_latency_ms;
      std::uint64_t session_generation = 0;
    };

    struct record_t {
      std::string run_id;
      std::string owner_uuid;
      std::string app_uuid;
      std::string state;
      std::int64_t created_at = 0;
      std::int64_t expires_at = 0;
      std::string baseline_launch_instance_id;
      std::uint64_t baseline_session_generation = 0;
      std::string trial_launch_instance_id;
      std::uint64_t trial_launch_generation = 0;
      std::uint64_t trial_evidence_generation = 0;
      int candidate_target_fps = 0;
      effective_settings_t preserved;
      metrics_t baseline;
      metrics_t result;
    };

    struct store_t {
      std::vector<record_t> records;
    };

    enum class load_status_e { ok, rejected, io_error };
    struct load_result_t {
      load_status_e status = load_status_e::io_error;
      store_t store;
    };

    bool nonblank(std::string_view value, std::size_t maximum = 256) {
      return !value.empty() && value.size() <= maximum &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
          return byte >= 0x20 && byte != 0x7f;
        });
    }

    bool finite_nonnegative(double value) {
      return std::isfinite(value) && value >= 0.0;
    }

    bool valid_settings(const effective_settings_t &settings) {
      return nonblank(settings.topology) && nonblank(settings.codec) &&
        settings.width >= 320 && settings.width <= 16384 &&
        settings.height >= 240 && settings.height <= 16384 &&
        settings.target_fps >= 15 && settings.target_fps <= 240 &&
        settings.bitrate_kbps >= 1000 && settings.bitrate_kbps <= 300000;
    }

    bool nonterminal(std::string_view state) {
      return state == "proposed" || state == "queued" ||
             state == "running" || state == "collecting";
    }

    bool valid_state(std::string_view state) {
      return nonterminal(state) || state == "improved" || state == "no_change" ||
             state == "worse" || state == "inconclusive" ||
             state == "cancelled" || state == "expired";
    }

    nlohmann::json settings_json(const effective_settings_t &settings) {
      return {
        {"topology", settings.topology}, {"width", settings.width},
        {"height", settings.height}, {"target_fps", settings.target_fps},
        {"bitrate_kbps", settings.bitrate_kbps}, {"codec", settings.codec},
        {"hdr", settings.hdr}
      };
    }

    std::optional<effective_settings_t> parse_settings(const nlohmann::json &value) {
      if (!value.is_object()) return std::nullopt;
      effective_settings_t settings;
      settings.topology = value.value("topology", std::string {});
      settings.width = value.value("width", 0);
      settings.height = value.value("height", 0);
      settings.target_fps = value.value("target_fps", 0);
      settings.bitrate_kbps = value.value("bitrate_kbps", 0);
      settings.codec = value.value("codec", std::string {});
      settings.hdr = value.value("hdr", false);
      return valid_settings(settings) ? std::optional {settings} : std::nullopt;
    }

    nlohmann::json metrics_json(const metrics_t &metrics) {
      return {
        {"coverage", metrics.coverage}, {"target_fps", metrics.target_fps},
        {"rendered_fps", metrics.rendered_fps},
        {"pacing_error_pct", metrics.pacing_error_pct},
        {"confirmed_media_loss_pct", metrics.media_loss_pct},
        {"rtt_ms", metrics.rtt_ms}, {"decode_latency_ms", metrics.decode_latency_ms},
        {"host_processing_latency_ms", metrics.host_processing_latency_ms ?
          nlohmann::json(*metrics.host_processing_latency_ms) : nlohmann::json(nullptr)},
        {"session_generation", metrics.session_generation}
      };
    }

    std::optional<metrics_t> parse_metrics(const nlohmann::json &value, bool require_ready = false) {
      if (!value.is_object() || (require_ready && !value.value("ready", false))) return std::nullopt;
      metrics_t metrics;
      metrics.coverage = value.value("coverage", 0.0);
      metrics.target_fps = value.value("target_fps", 0.0);
      metrics.rendered_fps = value.value("rendered_fps", 0.0);
      metrics.pacing_error_pct = value.value("pacing_error_pct", 0.0);
      metrics.media_loss_pct = value.value("confirmed_media_loss_pct", 0.0);
      metrics.rtt_ms = value.value("rtt_ms", 0.0);
      metrics.decode_latency_ms = value.value("decode_latency_ms", 0.0);
      if (value.contains("host_processing_latency_ms") && value["host_processing_latency_ms"].is_number()) {
        metrics.host_processing_latency_ms = value["host_processing_latency_ms"].get<double>();
      }
      metrics.session_generation = value.value("session_generation", std::uint64_t {0});
      if (!finite_nonnegative(metrics.coverage) || metrics.coverage < 0.90 ||
          !finite_nonnegative(metrics.target_fps) || metrics.target_fps <= 0.0 ||
          !finite_nonnegative(metrics.rendered_fps) ||
          !finite_nonnegative(metrics.pacing_error_pct) ||
          !finite_nonnegative(metrics.media_loss_pct) ||
          !finite_nonnegative(metrics.rtt_ms) ||
          !finite_nonnegative(metrics.decode_latency_ms) ||
          (metrics.host_processing_latency_ms && !finite_nonnegative(*metrics.host_processing_latency_ms)) ||
          metrics.session_generation == 0) {
        return std::nullopt;
      }
      return metrics;
    }

    nlohmann::json record_json(const record_t &record) {
      return {
        {"run_id", record.run_id}, {"owner_uuid", record.owner_uuid},
        {"app_uuid", record.app_uuid}, {"state", record.state},
        {"created_at", record.created_at}, {"expires_at", record.expires_at},
        {"baseline_launch_instance_id", record.baseline_launch_instance_id},
        {"baseline_session_generation", record.baseline_session_generation},
        {"trial_launch_instance_id", record.trial_launch_instance_id},
        {"trial_launch_generation", record.trial_launch_generation},
        {"trial_evidence_generation", record.trial_evidence_generation},
        {"candidate_target_fps", record.candidate_target_fps},
        {"preserved", settings_json(record.preserved)},
        {"baseline", metrics_json(record.baseline)},
        {"result", metrics_json(record.result)}
      };
    }

    std::optional<record_t> parse_record(const nlohmann::json &value) {
      if (!value.is_object()) return std::nullopt;
      record_t record;
      record.run_id = value.value("run_id", std::string {});
      record.owner_uuid = value.value("owner_uuid", std::string {});
      record.app_uuid = value.value("app_uuid", std::string {});
      record.state = value.value("state", std::string {});
      record.created_at = value.value("created_at", std::int64_t {0});
      record.expires_at = value.value("expires_at", std::int64_t {0});
      record.baseline_launch_instance_id = value.value("baseline_launch_instance_id", std::string {});
      record.baseline_session_generation = value.value("baseline_session_generation", std::uint64_t {0});
      record.trial_launch_instance_id = value.value("trial_launch_instance_id", std::string {});
      record.trial_launch_generation = value.value("trial_launch_generation", std::uint64_t {0});
      record.trial_evidence_generation = value.value("trial_evidence_generation", std::uint64_t {0});
      record.candidate_target_fps = value.value("candidate_target_fps", 0);
      const auto preserved = parse_settings(value.value("preserved", nlohmann::json::object()));
      const auto baseline = parse_metrics(value.value("baseline", nlohmann::json::object()));
      const auto result = parse_metrics(value.value("result", nlohmann::json::object()));
      if (!nonblank(record.run_id) || !nonblank(record.owner_uuid) || !nonblank(record.app_uuid) ||
          !nonblank(record.baseline_launch_instance_id) || record.baseline_session_generation == 0 ||
          !valid_state(record.state) || record.created_at <= 0 || record.expires_at <= record.created_at ||
          record.candidate_target_fps < 15 || record.candidate_target_fps > 240 ||
          !preserved || !baseline || !result) return std::nullopt;
      record.preserved = *preserved;
      record.baseline = *baseline;
      record.result = *result;
      return record;
    }

    load_result_t load(const std::filesystem::path &path) {
      const auto stored = private_state_file::read_secure(path, maximum_store_bytes);
      if (stored.status == private_state_file::read_status_e::missing) return {load_status_e::ok, {}};
      if (stored.status == private_state_file::read_status_e::rejected) return {load_status_e::rejected, {}};
      if (stored.status != private_state_file::read_status_e::ok) return {load_status_e::io_error, {}};
      try {
        const auto root = nlohmann::json::parse(stored.payload);
        if (!root.is_object() || root.value("version", 0) != schema_version ||
            !root.contains("records") || !root["records"].is_array() || root["records"].size() > 1024) {
          return {load_status_e::rejected, {}};
        }
        store_t store;
        for (const auto &value : root["records"]) {
          const auto record = parse_record(value);
          if (!record) return {load_status_e::rejected, {}};
          if (std::any_of(store.records.begin(), store.records.end(), [&](const record_t &existing) {
                return existing.run_id == record->run_id ||
                  (existing.owner_uuid == record->owner_uuid && existing.app_uuid == record->app_uuid);
              })) return {load_status_e::rejected, {}};
          store.records.push_back(*record);
        }
        return {load_status_e::ok, std::move(store)};
      } catch (...) {
        return {load_status_e::rejected, {}};
      }
    }

    bool persist(const std::filesystem::path &path, const store_t &store) {
      nlohmann::json root {{"version", schema_version}, {"records", nlohmann::json::array()}};
      for (const auto &record : store.records) root["records"].push_back(record_json(record));
      return static_cast<bool>(private_state_file::write_atomic(path, root.dump()));
    }

    bool expire_and_prune(store_t &store, std::int64_t now) {
      bool changed = false;
      for (auto &record : store.records) {
        if (nonterminal(record.state) && now >= record.expires_at) {
          record.state = "expired";
          changed = true;
        }
      }
      const auto old_size = store.records.size();
      std::erase_if(store.records, [now](const record_t &record) {
        return !nonterminal(record.state) && now >= record.expires_at + lifetime_seconds;
      });
      return changed || old_size != store.records.size();
    }

    auto find_scope(store_t &store, const std::string &owner, const std::string &app) {
      return std::find_if(store.records.begin(), store.records.end(), [&](const record_t &record) {
        return record.owner_uuid == owner && record.app_uuid == app;
      });
    }

    std::string next_run_id() {
      std::array<unsigned char, 16> bytes {};
      std::random_device random;
      for (auto &byte : bytes) byte = static_cast<unsigned char>(random());
      constexpr char hex[] = "0123456789abcdef";
      std::string id = "doctor-trial-";
      for (const auto byte : bytes) {
        id.push_back(hex[(byte >> 4) & 0x0f]);
        id.push_back(hex[byte & 0x0f]);
      }
      return id;
    }

    nlohmann::json unavailable(load_status_e status) {
      return {{"status", false}, {"changed", false}, {"state", "rejected"},
              {"code", status == load_status_e::rejected ? "trial_state_rejected" : "trial_state_unavailable"}};
    }

    nlohmann::json receipt(const record_t &record, bool changed = false) {
      return {
        {"status", true}, {"changed", changed}, {"state", record.state},
        {"run_id", record.run_id}, {"app_uuid", record.app_uuid},
        {"expires_at", record.expires_at}, {"mode", "fresh_launch_trial"},
        {"changed_dimension", "stream_fps_ceiling"},
        {"candidate", {{"target_fps", record.candidate_target_fps}}},
        {"preserved", {
          {"topology", record.preserved.topology}, {"width", record.preserved.width},
          {"height", record.preserved.height}, {"bitrate_kbps", record.preserved.bitrate_kbps},
          {"codec", record.preserved.codec}, {"hdr", record.preserved.hdr}
        }},
        {"baseline", metrics_json(record.baseline)},
        {"result", metrics_json(record.result)},
        {"cancellable", nonterminal(record.state)},
        {"one_shot", true}, {"becomes_policy_automatically", false}
      };
    }

    int derive_candidate(const metrics_t &baseline, int configured_target) {
      static constexpr std::array<int, 9> ceilings {240, 165, 144, 120, 90, 60, 45, 40, 30};
      const double observed_ceiling = std::max(30.0, baseline.rendered_fps * 1.05);
      for (const auto ceiling : ceilings) {
        if (ceiling < configured_target && ceiling <= observed_ceiling + 0.5) return ceiling;
      }
      return 0;
    }

    bool regressed_more_than_ten_percent(double baseline, double current) {
      if (baseline <= 0.001) return current > 0.001;
      return current > baseline * 1.10;
    }
  }  // namespace

  std::int64_t now_epoch_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  }

  nlohmann::json propose(const std::filesystem::path &path,
                         const std::string &owner_uuid,
                         const std::string &app_uuid,
                         const std::string &launch_instance_id,
                         std::uint64_t session_generation,
                         const nlohmann::json &doctor_status,
                         const effective_settings_t &settings,
                         std::int64_t now) {
    const auto baseline = parse_metrics(doctor_status.value("trial_metrics", nlohmann::json::object()), true);
    if (!nonblank(owner_uuid) || !nonblank(app_uuid) || !nonblank(launch_instance_id) ||
        session_generation == 0 || !valid_settings(settings) || !baseline || now <= 0 ||
        baseline->session_generation != session_generation ||
        baseline->pacing_error_pct <= 0.0 ||
        std::abs(baseline->target_fps - settings.target_fps) > 1.0) {
      return {{"status", false}, {"changed", false}, {"state", "rejected"},
              {"code", "complete_matching_baseline_required"}};
    }
    const int candidate = derive_candidate(*baseline, settings.target_fps);
    if (candidate <= 0) {
      return {{"status", false}, {"changed", false}, {"state", "rejected"},
              {"code", "no_single_dimension_candidate"}};
    }

    std::lock_guard lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool expired = expire_and_prune(loaded.store, now);
    auto existing = find_scope(loaded.store, owner_uuid, app_uuid);
    if (existing != loaded.store.records.end() && nonterminal(existing->state)) {
      if (expired && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      auto output = receipt(*existing);
      output["idempotent"] = true;
      return output;
    }

    record_t record;
    record.run_id = next_run_id();
    record.owner_uuid = owner_uuid;
    record.app_uuid = app_uuid;
    record.state = "proposed";
    record.created_at = now;
    record.expires_at = now + lifetime_seconds;
    record.baseline_launch_instance_id = launch_instance_id;
    record.baseline_session_generation = session_generation;
    record.candidate_target_fps = candidate;
    record.preserved = settings;
    record.baseline = *baseline;
    record.result = *baseline;
    if (existing == loaded.store.records.end()) loaded.store.records.push_back(record);
    else *existing = record;
    if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    return receipt(record, true);
  }

  nlohmann::json confirm(const std::filesystem::path &path,
                         const std::string &owner_uuid,
                         const std::string &app_uuid,
                         const std::string &run_id,
                         std::int64_t now) {
    std::lock_guard lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool expired = expire_and_prune(loaded.store, now);
    auto record = find_scope(loaded.store, owner_uuid, app_uuid);
    if (record == loaded.store.records.end() || record->run_id != run_id ||
        (record->state != "proposed" && record->state != "queued")) {
      if (expired && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      return {{"status", false}, {"changed", false}, {"state", "rejected"},
              {"code", "proposal_not_available"}};
    }
    if (record->state == "queued") {
      auto output = receipt(*record);
      output["idempotent"] = true;
      return output;
    }
    record->state = "queued";
    if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    return receipt(*record, true);
  }

  nlohmann::json status(const std::filesystem::path &path,
                        const std::string &owner_uuid,
                        const std::string &app_uuid,
                        std::int64_t now) {
    if (owner_uuid.empty() || app_uuid.empty()) return {{"status", true}, {"state", "none"}};
    std::lock_guard lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool changed = expire_and_prune(loaded.store, now);
    if (changed && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    const auto record = find_scope(loaded.store, owner_uuid, app_uuid);
    return record == loaded.store.records.end() ?
      nlohmann::json {{"status", true}, {"changed", false}, {"state", "none"}} : receipt(*record);
  }

  nlohmann::json cancel(const std::filesystem::path &path,
                        const std::string &owner_uuid,
                        const std::string &app_uuid,
                        const std::string &run_id,
                        std::int64_t now) {
    std::lock_guard lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool expired = expire_and_prune(loaded.store, now);
    auto record = find_scope(loaded.store, owner_uuid, app_uuid);
    if (record == loaded.store.records.end() || record->run_id != run_id || !nonterminal(record->state)) {
      if (record != loaded.store.records.end() && record->run_id == run_id && record->state == "cancelled") {
        auto output = receipt(*record);
        output["idempotent"] = true;
        return output;
      }
      if (expired && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      return {{"status", false}, {"changed", false}, {"state", "rejected"},
              {"code", "trial_not_cancellable"}};
    }
    record->state = "cancelled";
    if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    return receipt(*record, true);
  }

  std::optional<launch_override_t> begin_launch(const std::filesystem::path &path,
                                                 const std::string &owner_uuid,
                                                 const std::string &app_uuid,
                                                 const std::string &launch_instance_id,
                                                 std::int64_t now) {
    if (!nonblank(owner_uuid) || !nonblank(app_uuid) || !nonblank(launch_instance_id)) {
      return std::nullopt;
    }
    std::lock_guard lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return std::nullopt;
    const bool expired = expire_and_prune(loaded.store, now);
    auto record = find_scope(loaded.store, owner_uuid, app_uuid);
    if (record == loaded.store.records.end() || record->state != "queued" ||
        record->baseline_launch_instance_id == launch_instance_id) {
      if (expired) static_cast<void>(persist(path, loaded.store));
      return std::nullopt;
    }
    record->state = "running";
    record->trial_launch_instance_id = launch_instance_id;
    // proc_t's launch counter is not stream::session_t's evidence generation.
    // Bind the latter only after a full host-received Doctor window arrives.
    record->trial_launch_generation = 0;
    if (!persist(path, loaded.store)) return std::nullopt;
    return launch_override_t {record->run_id, record->candidate_target_fps};
  }

  nlohmann::json observe(const std::filesystem::path &path,
                         const std::string &owner_uuid,
                         const std::string &app_uuid,
                         const std::string &launch_instance_id,
                         const nlohmann::json &doctor_status,
                         const effective_settings_t &settings,
                         bool crashed,
                         std::int64_t now) {
    std::lock_guard lock(store_mutex);
    auto loaded = load(path);
    if (loaded.status != load_status_e::ok) return unavailable(loaded.status);
    const bool expired = expire_and_prune(loaded.store, now);
    auto record = find_scope(loaded.store, owner_uuid, app_uuid);
    if (record == loaded.store.records.end() ||
        (record->state != "running" && record->state != "collecting")) {
      if (expired && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      return record == loaded.store.records.end() ?
        nlohmann::json {{"status", true}, {"changed", false}, {"state", "none"}} : receipt(*record);
    }
    if (!nonblank(launch_instance_id) ||
        record->trial_launch_instance_id != launch_instance_id) {
      if (expired && !persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      auto output = receipt(*record);
      output["reason_code"] = "trial_launch_instance_mismatch";
      return output;
    }
    if (crashed) {
      record->state = "worse";
      if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      auto output = receipt(*record, true);
      output["reason_code"] = "trial_session_crashed";
      return output;
    }

    const auto metrics = parse_metrics(doctor_status.value("trial_metrics", nlohmann::json::object()), true);
    const bool metrics_match_candidate = metrics &&
      std::abs(metrics->target_fps - record->candidate_target_fps) <= 1.0;
    if (!metrics_match_candidate) {
      bool changed = false;
      if (record->state != "collecting") {
        record->state = "collecting";
        changed = true;
        if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      } else if (expired && !persist(path, loaded.store)) {
        return unavailable(load_status_e::io_error);
      }
      auto output = receipt(*record, changed);
      output["reason_code"] = metrics ?
        "trial_target_window_mismatch" : "complete_trial_window_required";
      return output;
    }

    // The authenticated evidence route replaces Nova's local generation with
    // stream::session_t's host generation. It is therefore safe to bind here,
    // and remains correct after a Polaris restart resets either process-local
    // counter independently.
    record->trial_launch_generation = metrics->session_generation;

    const bool settings_match = valid_settings(settings) &&
      settings.topology == record->preserved.topology &&
      settings.width == record->preserved.width && settings.height == record->preserved.height &&
      settings.bitrate_kbps == record->preserved.bitrate_kbps &&
      settings.codec == record->preserved.codec && settings.hdr == record->preserved.hdr &&
      std::abs(settings.target_fps - record->candidate_target_fps) <= 1;
    if (!settings_match || !metrics->host_processing_latency_ms ||
        !record->baseline.host_processing_latency_ms) {
      record->state = "inconclusive";
      record->result = *metrics;
      if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
      auto output = receipt(*record, true);
      output["reason_code"] = settings_match ? "missing_guardrail_evidence" : "confounded_trial_settings";
      return output;
    }

    record->trial_evidence_generation = metrics->session_generation;
    record->result = *metrics;
    const bool guardrail_regressed =
      regressed_more_than_ten_percent(record->baseline.media_loss_pct, metrics->media_loss_pct) ||
      regressed_more_than_ten_percent(record->baseline.rtt_ms, metrics->rtt_ms) ||
      regressed_more_than_ten_percent(record->baseline.decode_latency_ms, metrics->decode_latency_ms) ||
      regressed_more_than_ten_percent(
        *record->baseline.host_processing_latency_ms,
        *metrics->host_processing_latency_ms
      );
    const double improvement = record->baseline.pacing_error_pct > 0.0 ?
      (record->baseline.pacing_error_pct - metrics->pacing_error_pct) /
        record->baseline.pacing_error_pct : 0.0;
    if (guardrail_regressed || metrics->pacing_error_pct > record->baseline.pacing_error_pct * 1.10) {
      record->state = "worse";
    } else if (improvement >= 0.20) {
      record->state = "improved";
    } else {
      record->state = "no_change";
    }
    if (!persist(path, loaded.store)) return unavailable(load_status_e::io_error);
    auto output = receipt(*record, true);
    output["pacing_improvement_ratio"] = improvement;
    output["guardrail_regressed"] = guardrail_regressed;
    return output;
  }
}  // namespace doctor_trial
