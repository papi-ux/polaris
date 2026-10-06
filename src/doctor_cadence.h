/**
 * @file src/doctor_cadence.h
 * @brief Read-only source-relative cadence evidence; never grants VRR or tuning authority.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace doctor_cadence {
  struct evaluation_t {
    bool available = true;  // Older hosts retain the existing fixed-grid diagnostic.
    bool source_time = false;
    bool source_stall = false;
    bool delivery_jitter = false;
    bool timestamp_error = false;
    bool source_encode_gap = false;
    double source_fps = 0.0;
    nlohmann::json observation;
  };

  inline bool exact_nonnegative(const nlohmann::json &value) {
    return value.is_number_unsigned() ? value.get<std::uint64_t>() <= std::uint64_t(INT64_MAX) :
      value.is_number_integer() && value.get<std::int64_t>() >= 0;
  }

  inline bool finite_nonnegative(const nlohmann::json &value) {
    return value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() >= 0.0;
  }

  /** Optional host-only aggregate contract. Counters and requests never prove cadence. */
  inline evaluation_t evaluate(const nlohmann::json &host, std::int64_t generation, std::int64_t now_ms,
                               std::int64_t expected_begin_ms, std::int64_t expected_end_ms) {
    evaluation_t result;
    if (!host.contains("vrr")) return result;
    result.available = false;
    result.observation = {{"id", "source_cadence_policy"}, {"state", "unavailable"},
                          {"reason", "invalid_pacing_evidence"}, {"physical_acceptance", false},
                          {"receiver_presentation_verified", false}};
    const auto unavailable = [&](const char *reason) {
      result.observation["reason"] = reason;
      return result;
    };
    try {
      const auto &vrr = host.at("vrr");
      if (!vrr.is_object() || !exact_nonnegative(vrr.at("schema")) || vrr.at("schema") != 1 ||
          !exact_nonnegative(vrr.at("session_generation")) || generation <= 0 ||
          vrr.at("session_generation") != generation || !vrr.at("effective").is_boolean()) {
        return unavailable("pacing_generation_or_schema_unavailable");
      }
      const auto requested = vrr.at("requested").get<std::string>();
      const auto requested_from = vrr.at("requested_from").get<std::string>();
      const auto pacing = vrr.at("pacing_mode").get<std::string>();
      const auto capture = vrr.at("capture_pacing").get<std::string>();
      if ((requested != "absent" && requested != "false" && requested != "true" && requested != "malformed") ||
          (requested_from != "launch" && requested_from != "resume") ||
          (capture != "source_driven" && capture != "fixed_interval" && capture != "unknown")) {
        return unavailable("invalid_pacing_enum");
      }
      result.observation["requested"] = requested;
      result.observation["effective"] = vrr.at("effective");
      result.observation["pacing_mode"] = pacing;
      result.observation["capture_pacing"] = capture;
      result.observation["session_generation"] = generation;
      if (!vrr.at("effective").get<bool>()) {
        const auto fallback = vrr.at("fallback_reason").get<std::string>();
        if (pacing != "fixed_grid" || (fallback != "not_requested" && fallback != "request_malformed" &&
            fallback != "watch_or_input_only" && fallback != "route_unreported" &&
            fallback != "route_fixed_interval" && fallback != "clock_domain_unverified" &&
            fallback != "presentation_time_missing" && fallback != "presentation_time_invalid")) {
          return unavailable("inconsistent_fixed_grid_policy");
        }
        result.available = true;
        result.observation["state"] = "fixed_grid";
        result.observation["reason"] = fallback;
        return result;
      }
      result.source_time = true;
      if (requested != "true" || pacing != "source_time" || capture != "source_driven" ||
          !vrr.at("fallback_reason").is_null()) return unavailable("inconsistent_source_time_policy");
      const auto &cadence = host.at("source_cadence");
      if (!cadence.is_object() || !exact_nonnegative(cadence.at("schema")) || cadence.at("schema") != 1 ||
          !exact_nonnegative(cadence.at("session_generation")) || cadence.at("session_generation") != generation ||
          cadence.at("clock_domain_verified") != true || !cadence.at("clock_domain_verified").is_boolean() ||
          !cadence.at("clock_epoch_id").is_string() || cadence.at("clock_epoch_id").get<std::string>().empty()) {
        return unavailable("source_clock_or_generation_unavailable");
      }
      const auto &start = cadence.at("window_start_ms");
      const auto &end = cadence.at("window_end_ms");
      if (!exact_nonnegative(start) || !exact_nonnegative(end)) return unavailable("source_window_unavailable");
      const auto begin_ms = start.get<std::int64_t>(), end_ms = end.get<std::int64_t>();
      if (end_ms < begin_ms || end_ms - begin_ms < 1000 || now_ms < end_ms || now_ms - end_ms > 2000) {
        return unavailable("source_window_stale_or_uncovered");
      }
      if (begin_ms != expected_begin_ms || end_ms != expected_end_ms) {
        return unavailable("source_and_client_windows_differ");
      }
      if (!exact_nonnegative(cadence.at("source_frames")) || cadence.at("source_frames").get<std::int64_t>() < 3 ||
          !exact_nonnegative(cadence.at("encoded_source_frames")) ||
          cadence.at("encoded_source_frames") > cadence.at("source_frames")) {
        return unavailable("source_frame_coverage_unavailable");
      }
      if (!exact_nonnegative(cadence.at("invalid_source_frames")) || cadence.at("invalid_source_frames") != 0 ||
          !exact_nonnegative(cadence.at("routine_repeat_frames")) || cadence.at("routine_repeat_frames") != 0) {
        return unavailable("invalid_sources_or_routine_repeats");
      }
      const auto &limits = cadence.at("limits");
      for (const char *key : {"source_fps", "max_source_gap_ms", "max_delivery_interval_error_ms", "timestamp_error_ms"}) {
        if (!finite_nonnegative(cadence.at(key))) return unavailable("source_measurement_unavailable");
      }
      for (const char *key : {"max_source_gap_ms", "max_delivery_interval_error_ms", "max_timestamp_error_ms"}) {
        if (!finite_nonnegative(limits.at(key))) return unavailable("source_limit_unavailable");
      }
      if (cadence.at("source_fps").get<double>() <= 0.0 || limits.at("max_source_gap_ms").get<double>() <= 0.0) {
        return unavailable("source_measurement_or_limit_unavailable");
      }
      result.source_fps = cadence.at("source_fps").get<double>();
      // A declared aggregate FPS must agree with its retained ordinary-source count.
      const auto counted_fps = 1000.0 * (cadence.at("source_frames").get<std::int64_t>() - 1) / (end_ms - begin_ms);
      if (std::abs(result.source_fps - counted_fps) > 0.01 * std::max(1.0, counted_fps)) {
        return unavailable("source_fps_count_mismatch");
      }
      if (cadence.at("max_source_gap_ms").get<double>() + 0.000001 <
          double(end_ms - begin_ms) / (cadence.at("source_frames").get<std::int64_t>() - 1)) {
        return unavailable("source_gap_count_mismatch");
      }
      result.source_stall = cadence.at("max_source_gap_ms").get<double>() > limits.at("max_source_gap_ms").get<double>();
      result.delivery_jitter = cadence.at("max_delivery_interval_error_ms").get<double>() >
        limits.at("max_delivery_interval_error_ms").get<double>();
      result.timestamp_error = cadence.at("timestamp_error_ms").get<double>() > limits.at("max_timestamp_error_ms").get<double>();
      result.source_encode_gap = cadence.at("encoded_source_frames").get<double>() <
        cadence.at("source_frames").get<double>() * 0.90;
      result.available = true;
      result.observation["state"] = "observed_source_time";
      result.observation["reason"] = "fresh_host_source_and_delivery_measurements";
      result.observation["measurements"] = cadence;
    } catch (const nlohmann::json::exception &) {
      return unavailable("source_or_pacing_evidence_unavailable");
    }
    return result;
  }
}  // namespace doctor_cadence
