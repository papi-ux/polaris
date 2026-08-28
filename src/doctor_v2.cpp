#include "doctor_v2.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace doctor_v2 {
  namespace {
    using clock_t = std::chrono::steady_clock;

    constexpr std::int64_t k_warmup_ms = 15'000;
    constexpr std::int64_t k_window_ms = 30'000;
    constexpr std::int64_t k_retention_ms = 180'000;
    constexpr double k_required_coverage = 0.90;
    constexpr std::size_t k_max_samples_per_scope = 256;
    constexpr std::size_t k_max_scopes = 64;

    struct sample_t {
      std::int64_t client_ms = 0;
      std::int64_t received_ms = 0;
      std::int64_t generation = 0;
      std::int64_t frames_expected = 0;
      std::int64_t frames_received = 0;
      std::int64_t frames_rendered = 0;
      std::int64_t frames_lost = 0;
      double source_fps = 0.0;
      double received_fps = 0.0;
      double rendered_fps = 0.0;
      double target_fps = 0.0;
      double refresh_rate_hz = 0.0;
      double rtt_ms = 0.0;
      double decode_ms = 0.0;
      std::optional<double> host_processing_ms;
      int width = 0;
      int height = 0;
      int bitrate_kbps = 0;
      std::string codec;
      std::string topology;
      bool hdr = false;
    };

    std::mutex samples_mutex;
    std::unordered_map<std::string, std::deque<sample_t>> samples_by_scope;
#ifdef POLARIS_TESTS
    std::atomic<std::int64_t> test_now_ms {-1};
#endif

    std::int64_t now_ms() {
#ifdef POLARIS_TESTS
      const auto test_value = test_now_ms.load(std::memory_order_relaxed);
      if (test_value >= 0) return test_value;
#endif
      return std::chrono::duration_cast<std::chrono::milliseconds>(
        clock_t::now().time_since_epoch()
      ).count();
    }

    std::string scope_key(const std::string &owner_uuid, const std::string &app_uuid) {
      return owner_uuid + '\0' + app_uuid;
    }

    bool flag_enabled(const char *name) {
      const char *raw = std::getenv(name);
      if (!raw) return false;
      std::string value {raw};
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return value == "1" || value == "true" || value == "yes" || value == "on";
    }

    bool finite_nonnegative(double value) {
      return std::isfinite(value) && value >= 0.0;
    }

    std::optional<sample_t> parse_sample(const nlohmann::json &payload, std::string &error) {
      const auto &raw = payload.contains("sample") ? payload["sample"] : payload;
      if (!raw.is_object()) {
        error = "sample must be an object";
        return std::nullopt;
      }
      // Fail closed if a client attempts to cross the evidence/action boundary.
      for (const char *forbidden : {
             "settings", "actions", "action", "primary_issue", "safe_profile",
             "safe_settings", "confidence", "observations", "hypotheses",
             "recommendation", "relaunch_recommended"
           }) {
        if (payload.contains(forbidden) || raw.contains(forbidden)) {
          error = std::string {"raw evidence cannot contain "} + forbidden;
          return std::nullopt;
        }
      }

      sample_t sample;
      sample.client_ms = raw.value("monotonic_timestamp_ms", std::int64_t {0});
      sample.generation = raw.value("session_generation", std::int64_t {0});
      sample.frames_expected = raw.value("frames_expected", std::int64_t {0});
      sample.frames_received = raw.value("frames_received", std::int64_t {0});
      sample.frames_rendered = raw.value("frames_rendered", std::int64_t {0});
      sample.frames_lost = raw.value("frames_lost", std::int64_t {0});
      sample.source_fps = raw.value("source_fps", 0.0);
      sample.received_fps = raw.value("received_fps", 0.0);
      sample.rendered_fps = raw.value("rendered_fps", 0.0);
      sample.target_fps = raw.value("target_fps", 0.0);
      sample.refresh_rate_hz = raw.value("refresh_rate_hz", 0.0);
      sample.rtt_ms = raw.value("rtt_ms", 0.0);
      sample.decode_ms = raw.value("decode_latency_ms", 0.0);
      if (raw.contains("host_processing_latency_ms") && raw["host_processing_latency_ms"].is_number()) {
        sample.host_processing_ms = raw["host_processing_latency_ms"].get<double>();
      }
      sample.width = raw.value("width", 0);
      sample.height = raw.value("height", 0);
      sample.bitrate_kbps = raw.value("bitrate_kbps", 0);
      sample.codec = raw.value("codec", std::string {});
      sample.topology = raw.value("topology", std::string {});
      sample.hdr = raw.value("hdr", false);

      if (sample.client_ms <= 0 || sample.generation <= 0 ||
          sample.frames_expected < 0 || sample.frames_received < 0 ||
          sample.frames_rendered < 0 || sample.frames_lost < 0 ||
          !finite_nonnegative(sample.source_fps) || !finite_nonnegative(sample.received_fps) ||
          !finite_nonnegative(sample.rendered_fps) || !finite_nonnegative(sample.target_fps) ||
          !finite_nonnegative(sample.refresh_rate_hz) ||
          !finite_nonnegative(sample.rtt_ms) || !finite_nonnegative(sample.decode_ms) ||
          (sample.host_processing_ms && !finite_nonnegative(*sample.host_processing_ms))) {
        error = "sample contains invalid monotonic counters or measurements";
        return std::nullopt;
      }
      sample.received_ms = now_ms();
      return sample;
    }

    struct window_t {
      std::vector<sample_t> samples;
      double coverage = 0.0;
      bool complete = false;
    };

    window_t window_between(const std::vector<sample_t> &samples,
                            std::int64_t begin_ms,
                            std::int64_t end_ms) {
      window_t result;
      std::array<bool, 30> covered_seconds {};
      for (const auto &sample : samples) {
        if (sample.received_ms >= begin_ms && sample.received_ms < end_ms) {
          result.samples.push_back(sample);
          const auto bucket = static_cast<std::size_t>((sample.received_ms - begin_ms) / 1000);
          if (bucket < covered_seconds.size()) covered_seconds[bucket] = true;
        }
      }
      const auto covered = std::count(covered_seconds.begin(), covered_seconds.end(), true);
      result.coverage = static_cast<double>(covered) / static_cast<double>(covered_seconds.size());
      result.complete = result.coverage >= k_required_coverage;
      return result;
    }

    double average(const std::vector<sample_t> &samples, double sample_t::*field) {
      if (samples.empty()) return 0.0;
      double sum = 0.0;
      for (const auto &sample : samples) sum += sample.*field;
      return sum / static_cast<double>(samples.size());
    }

    std::optional<double> average_host_processing(const std::vector<sample_t> &samples) {
      double sum = 0.0;
      std::size_t count = 0;
      for (const auto &sample : samples) {
        if (!sample.host_processing_ms) continue;
        sum += *sample.host_processing_ms;
        ++count;
      }
      return count > 0 ? std::optional {sum / static_cast<double>(count)} : std::nullopt;
    }

    nlohmann::json stage(std::string state,
                         nlohmann::json measurements,
                         std::string provenance) {
      return {
        {"state", std::move(state)},
        {"measurements", std::move(measurements)},
        {"provenance", std::move(provenance)}
      };
    }
  }  // namespace

  bool shadow_enabled() {
    return flag_enabled("POLARIS_DOCTOR_V2_SHADOW");
  }

  bool trials_enabled() {
#ifdef POLARIS_ENABLE_DOCTOR_TRIALS
    return flag_enabled("POLARIS_DOCTOR_TRIALS");
#else
    // v1.3.14 is a containment release. Keep the authenticated trial contract
    // compiled for unit coverage and follow-up development, but do not let a
    // runtime environment variable turn a shadow observer into launch policy.
    return false;
#endif
  }

  nlohmann::json ingest(const std::string &owner_uuid,
                        const std::string &app_uuid,
                        const nlohmann::json &payload) {
    if (!shadow_enabled()) {
      return {
        {"status", false}, {"changed", false}, {"state", "disabled"},
        {"code", "doctor_v2_shadow_disabled"}
      };
    }
    if (owner_uuid.empty() || app_uuid.empty()) {
      return {
        {"status", false}, {"changed", false}, {"state", "rejected"},
        {"code", "scope_required"}
      };
    }
    std::string error;
    auto parsed = parse_sample(payload, error);
    if (!parsed) {
      return {
        {"status", false}, {"changed", false}, {"state", "rejected"},
        {"code", "invalid_evidence"}, {"error", error}
      };
    }

    std::lock_guard lock(samples_mutex);
    const auto key = scope_key(owner_uuid, app_uuid);
    if (!samples_by_scope.contains(key) && samples_by_scope.size() >= k_max_scopes) {
      const auto oldest = std::min_element(
        samples_by_scope.begin(),
        samples_by_scope.end(),
        [](const auto &lhs, const auto &rhs) {
          const auto lhs_ms = lhs.second.empty() ? 0 : lhs.second.back().received_ms;
          const auto rhs_ms = rhs.second.empty() ? 0 : rhs.second.back().received_ms;
          return lhs_ms < rhs_ms;
        }
      );
      if (oldest != samples_by_scope.end()) samples_by_scope.erase(oldest);
    }
    auto &samples = samples_by_scope[key];
    bool counter_epoch_reset = false;
    if (!samples.empty()) {
      const auto &last = samples.back();
      if (parsed->generation < last.generation ||
          (parsed->generation == last.generation && parsed->client_ms <= last.client_ms)) {
        return {
          {"status", false}, {"changed", false}, {"state", "rejected"},
          {"code", "non_monotonic_evidence"}
        };
      }
      if (parsed->generation > last.generation) samples.clear();
      if (!samples.empty() &&
          (parsed->frames_expected < last.frames_expected ||
           parsed->frames_received < last.frames_received ||
           parsed->frames_rendered < last.frames_rendered ||
           parsed->frames_lost < last.frames_lost)) {
        // Decoder/counter producers can restart inside one authenticated host
        // stream generation. Begin a fresh evidence epoch; host receipt time
        // still requires a full warm-up and two windows before classification.
        samples.clear();
        counter_epoch_reset = true;
      }
    }
    samples.push_back(*parsed);
    while (!samples.empty() &&
           (parsed->received_ms - samples.front().received_ms > k_retention_ms ||
            samples.size() > k_max_samples_per_scope)) {
      samples.pop_front();
    }
    return {
      {"status", true}, {"changed", true},
      {"state", counter_epoch_reset ? "counter_epoch_reset" : "observed"},
      {"sample_count", samples.size()}, {"session_generation", parsed->generation}
    };
  }

  nlohmann::json status(const std::string &owner_uuid,
                        const std::string &app_uuid,
                        const nlohmann::json &host_evidence) {
    nlohmann::json result {
      {"schema_version", 2},
      {"mode", "shadow"},
      {"enabled", shadow_enabled()},
      {"actions_exposed", false},
      {"warmup_seconds", 15},
      {"window_seconds", 30},
      {"required_consecutive_windows", 2},
      {"observations", nlohmann::json::array()},
      {"hypotheses", nlohmann::json::array()},
      {"missing_evidence", nlohmann::json::array()},
      {"actions", nlohmann::json::array()}
    };
    if (!shadow_enabled()) {
      result["state"] = "disabled";
      result["primary_issue"] = "undetermined";
      result["stable"] = false;
      return result;
    }

    std::vector<sample_t> samples;
    {
      std::lock_guard lock(samples_mutex);
      const auto it = samples_by_scope.find(scope_key(owner_uuid, app_uuid));
      if (it != samples_by_scope.end()) samples.assign(it->second.begin(), it->second.end());
    }
    if (samples.empty()) {
      result["state"] = "collecting";
      result["primary_issue"] = "undetermined";
      result["stable"] = false;
      result["missing_evidence"].push_back("nova_continuous_sample");
      return result;
    }

    const auto generation = samples.back().generation;
    std::erase_if(samples, [generation](const sample_t &sample) { return sample.generation != generation; });
    const auto warmup_end = samples.front().received_ms + k_warmup_ms;
    const auto end = samples.back().received_ms;
    const auto rolling_begin = end - (2 * k_window_ms);
    const auto begin = std::max(warmup_end, rolling_begin);
    const auto first = window_between(samples, begin, begin + k_window_ms);
    const auto second = window_between(samples, begin + k_window_ms, begin + (2 * k_window_ms));
    const bool windows_complete = end >= warmup_end + (2 * k_window_ms) && first.complete && second.complete;
    result["window"] = {
      {"session_generation", generation},
      {"warmup_excluded", end >= warmup_end},
      {"first", {{"samples", first.samples.size()}, {"coverage", first.coverage}, {"complete", first.complete}}},
      {"second", {{"samples", second.samples.size()}, {"coverage", second.coverage}, {"complete", second.complete}}},
      {"two_consecutive_complete", windows_complete}
    };

    const auto &current = second.samples.empty() ? samples : second.samples;
    const auto &latest = samples.back();
    const double received_fps = average(current, &sample_t::received_fps);
    const double rendered_fps = average(current, &sample_t::rendered_fps);
    const double target_fps = average(current, &sample_t::target_fps);
    const double rtt_ms = average(current, &sample_t::rtt_ms);
    const double decode_ms = average(current, &sample_t::decode_ms);
    double loss_pct = 0.0;
    if (current.size() >= 2) {
      const auto expected_delta = current.back().frames_expected - current.front().frames_expected;
      const auto loss_delta = current.back().frames_lost - current.front().frames_lost;
      if (expected_delta > 0) loss_pct = 100.0 * static_cast<double>(loss_delta) / expected_delta;
    }

    std::vector<sample_t> trial_samples = first.samples;
    trial_samples.insert(trial_samples.end(), second.samples.begin(), second.samples.end());
    const double trial_target_fps = average(trial_samples, &sample_t::target_fps);
    const double trial_rendered_fps = average(trial_samples, &sample_t::rendered_fps);
    double trial_loss_pct = 0.0;
    if (trial_samples.size() >= 2) {
      const auto expected_delta = trial_samples.back().frames_expected - trial_samples.front().frames_expected;
      const auto loss_delta = trial_samples.back().frames_lost - trial_samples.front().frames_lost;
      if (expected_delta > 0) {
        trial_loss_pct = 100.0 * static_cast<double>(loss_delta) / expected_delta;
      }
    }
    const auto trial_host_processing = average_host_processing(trial_samples);
    result["trial_metrics"] = {
      {"ready", windows_complete},
      {"duration_seconds", windows_complete ? 60 : 0},
      {"coverage", std::min(first.coverage, second.coverage)},
      {"session_generation", generation},
      {"target_fps", trial_target_fps},
      {"rendered_fps", trial_rendered_fps},
      {"pacing_error_pct", trial_target_fps > 0.0 ?
        100.0 * std::max(0.0, trial_target_fps - trial_rendered_fps) / trial_target_fps : 0.0},
      {"confirmed_media_loss_pct", trial_loss_pct},
      {"rtt_ms", average(trial_samples, &sample_t::rtt_ms)},
      {"decode_latency_ms", average(trial_samples, &sample_t::decode_ms)},
      {"host_processing_latency_ms", trial_host_processing ?
        nlohmann::json(*trial_host_processing) : nlohmann::json(nullptr)}
    };

    const auto source_capture_evidence =
      host_evidence.contains("source_capture") && host_evidence["source_capture"].is_object() ?
        host_evidence["source_capture"] : nlohmann::json::object();
    const bool source_capture_measured =
      source_capture_evidence.contains("source_fps") &&
      source_capture_evidence["source_fps"].is_number() &&
      source_capture_evidence["source_fps"].get<double>() >= 0.0 &&
      source_capture_evidence.contains("duplicate_frame_ratio") &&
      source_capture_evidence["duplicate_frame_ratio"].is_number() &&
      source_capture_evidence["duplicate_frame_ratio"].get<double>() >= 0.0 &&
      source_capture_evidence.contains("capture_pacing") &&
      source_capture_evidence["capture_pacing"].is_string() &&
      !source_capture_evidence["capture_pacing"].get<std::string>().empty() &&
      source_capture_evidence["capture_pacing"].get<std::string>() != "unknown";
    const auto encode_evidence =
      host_evidence.contains("encode") && host_evidence["encode"].is_object() ?
        host_evidence["encode"] : nlohmann::json::object();
    const bool encode_measured =
      encode_evidence.contains("encoded_fps") && encode_evidence["encoded_fps"].is_number() &&
      encode_evidence["encoded_fps"].get<double>() > 0.0 &&
      encode_evidence.contains("encode_latency_ms") && encode_evidence["encode_latency_ms"].is_number() &&
      encode_evidence["encode_latency_ms"].get<double>() >= 0.0 &&
      encode_evidence.contains("target_fps") && encode_evidence["target_fps"].is_number() &&
      encode_evidence["target_fps"].get<double>() > 0.0;
    const auto effective_settings_evidence =
      host_evidence.contains("effective_settings") && host_evidence["effective_settings"].is_object() ?
        host_evidence["effective_settings"] : nlohmann::json::object();
    const bool effective_settings_measured =
      effective_settings_evidence.contains("topology") && effective_settings_evidence["topology"].is_string() &&
      !effective_settings_evidence["topology"].get<std::string>().empty() &&
      effective_settings_evidence.contains("width") && effective_settings_evidence["width"].is_number_integer() &&
      effective_settings_evidence["width"].get<int>() > 0 &&
      effective_settings_evidence.contains("height") && effective_settings_evidence["height"].is_number_integer() &&
      effective_settings_evidence["height"].get<int>() > 0 &&
      effective_settings_evidence.contains("target_fps") && effective_settings_evidence["target_fps"].is_number() &&
      effective_settings_evidence["target_fps"].get<double>() > 0.0;
    const auto host_transport =
      host_evidence.contains("transport") && host_evidence["transport"].is_object() ?
        host_evidence["transport"] : nlohmann::json::object();
    const auto bytes_sent = host_transport.contains("bytes_sent") && host_transport["bytes_sent"].is_number() ?
      host_transport["bytes_sent"] : nlohmann::json(nullptr);
    const auto retransmissions =
      host_transport.contains("retransmissions") && host_transport["retransmissions"].is_number() ?
        host_transport["retransmissions"] : nlohmann::json(nullptr);
    const bool transport_measured =
      !bytes_sent.is_null() && bytes_sent.get<double>() > 0.0 &&
      !retransmissions.is_null() && retransmissions.get<double>() >= 0.0;
    const auto presentation_evidence =
      host_evidence.contains("receive_decode_render") && host_evidence["receive_decode_render"].is_object() ?
        host_evidence["receive_decode_render"] : nlohmann::json::object();
    const bool receive_decode_render_complete =
      presentation_evidence.contains("decoded_fps") && presentation_evidence["decoded_fps"].is_number() &&
      presentation_evidence["decoded_fps"].get<double>() >= 0.0 &&
      presentation_evidence.contains("queue_latency_ms") && presentation_evidence["queue_latency_ms"].is_number() &&
      presentation_evidence["queue_latency_ms"].get<double>() >= 0.0 &&
      presentation_evidence.contains("render_latency_ms") && presentation_evidence["render_latency_ms"].is_number() &&
      presentation_evidence["render_latency_ms"].get<double>() >= 0.0;
    const bool core_evidence_adequate =
      source_capture_measured && encode_measured && transport_measured &&
      effective_settings_measured && receive_decode_render_complete;

    nlohmann::json stages;
    stages["source_capture"] = source_capture_measured ?
      stage("measured", source_capture_evidence, "polaris_host_monotonic") :
      stage("unknown", nlohmann::json::object(), "unavailable");
    stages["encode"] = encode_measured ?
      stage("measured", encode_evidence, "polaris_encoder_stats") :
      stage("unknown", nlohmann::json::object(), "unavailable");
    stages["transport"] = stage(transport_measured ? "measured" : "partial", {
      {"confirmed_media_loss_pct", loss_pct},
      {"rtt_ms", rtt_ms},
      {"bytes_sent", bytes_sent},
      {"retransmissions", retransmissions}
    }, "nova_media_counters_host_transport_and_control_rtt");
    stages["receive_decode_render"] = stage(receive_decode_render_complete ? "measured" : "partial", {
      {"received_fps", received_fps},
      {"decoded_fps", receive_decode_render_complete ? presentation_evidence["decoded_fps"] : nlohmann::json(nullptr)},
      {"rendered_fps", rendered_fps},
      {"decode_latency_ms", decode_ms},
      {"queue_latency_ms", receive_decode_render_complete ? presentation_evidence["queue_latency_ms"] : nlohmann::json(nullptr)},
      {"render_latency_ms", receive_decode_render_complete ? presentation_evidence["render_latency_ms"] : nlohmann::json(nullptr)},
      {"host_processing_latency_ms", latest.host_processing_ms ? nlohmann::json(*latest.host_processing_ms) : nlohmann::json(nullptr)},
      {"frames_received", latest.frames_received},
      {"frames_rendered", latest.frames_rendered}
    }, "nova_decoder_monotonic");
    if (effective_settings_measured) {
      auto effective_settings = effective_settings_evidence;
      effective_settings["session_generation"] = generation;
      stages["effective_settings"] = stage(
        "measured", std::move(effective_settings), "polaris_active_session"
      );
    } else {
      stages["effective_settings"] = stage("partial", {
        {"topology", latest.topology},
        {"width", latest.width},
        {"height", latest.height},
        {"codec", latest.codec},
        {"hdr", latest.hdr},
        {"bitrate_kbps", latest.bitrate_kbps},
        {"target_fps", target_fps},
        {"refresh_rate_hz", latest.refresh_rate_hz},
        {"session_generation", generation}
      }, "nova_reported_active_session");
    }
    result["stages"] = stages;
    result["coverage_adequate"] = windows_complete && core_evidence_adequate;
    result["trial_metrics"]["ready"] = windows_complete && core_evidence_adequate;
    if (!core_evidence_adequate) result["trial_metrics"]["duration_seconds"] = 0;

    result["missing_evidence"] = nlohmann::json::array();
    if (retransmissions.is_null()) result["missing_evidence"].push_back("retransmissions");
    if (!receive_decode_render_complete) {
      result["missing_evidence"].push_back("decoded_frame_counter");
      result["missing_evidence"].push_back("queue_latency");
      result["missing_evidence"].push_back("render_latency");
    }
    if (!effective_settings_measured) {
      result["missing_evidence"].push_back("host_effective_settings");
    }
    if (!source_capture_measured) {
      result["missing_evidence"].push_back("source_capture_cadence");
      result["missing_evidence"].push_back("source_duplicate_frame_counter");
    }
    if (!encode_measured) {
      result["missing_evidence"].push_back("encoded_frame_cadence");
      result["missing_evidence"].push_back("encode_latency");
    }
    if (bytes_sent.is_null()) result["missing_evidence"].push_back("transport_bytes");
    result["observations"].push_back({
      {"id", "delivery_cadence"}, {"received_fps", received_fps},
      {"rendered_fps", rendered_fps}, {"target_fps", target_fps},
      {"provenance", "nova_decoder_monotonic"}
    });
    result["observations"].push_back({
      {"id", "transport"}, {"confirmed_media_loss_pct", loss_pct},
      {"rtt_ms", rtt_ms}, {"provenance", "nova_media_counters_and_control_rtt"}
    });

    const bool network_warning = loss_pct > 2.0 || rtt_ms >= 45.0;
    const bool render_gap = target_fps > 0.0 && rendered_fps + 1.0 < target_fps * 0.90;
    const bool decoder_gap = received_fps > 0.0 && rendered_fps + 3.0 < received_fps;
    const auto &source_capture = source_capture_evidence;
    const double source_fps = source_capture.value("source_fps", 0.0);
    const double duplicate_ratio = source_capture.value("duplicate_frame_ratio", 0.0);
    const auto &encode = encode_evidence;
    const double encoded_fps = encode.value("encoded_fps", 0.0);
    const double encode_target_fps = encode.value("target_fps", target_fps);
    const double dropped_frame_ratio = encode.value("dropped_frame_ratio", 0.0);
    const bool encode_warning = encode_measured &&
      ((encode_target_fps > 0.0 && encoded_fps + 1.0 < encode_target_fps * 0.90) ||
       dropped_frame_ratio > 0.05);
    const bool static_or_duplicate_content =
      duplicate_ratio >= 0.50 ||
      (target_fps > 0.0 && source_fps > 0.0 && source_fps < target_fps * 0.50 && duplicate_ratio >= 0.10);
    const bool pacing_gap = render_gap && !static_or_duplicate_content;
    if (encode_measured) {
      result["observations"].push_back({
        {"id", "encode_cadence"},
        {"encoded_fps", encoded_fps},
        {"target_fps", encode_target_fps},
        {"dropped_frame_ratio", dropped_frame_ratio},
        {"provenance", "polaris_encoder_stats"}
      });
    }
    if (static_or_duplicate_content && render_gap) {
      result["observations"].push_back({
        {"id", "static_or_duplicate_content"},
        {"source_fps", source_fps},
        {"duplicate_frame_ratio", duplicate_ratio},
        {"interpretation", "low source cadence is not classified as a pacing fault"},
        {"provenance", "polaris_host_monotonic"}
      });
    }
    if (network_warning) {
      auto missing = nlohmann::json::array();
      if (bytes_sent.is_null()) missing.push_back("transport_bytes");
      if (retransmissions.is_null()) missing.push_back("retransmissions");
      result["hypotheses"].push_back({
        {"id", "network_pressure"}, {"confidence", windows_complete ? "high" : "medium"},
        {"evidence_for", nlohmann::json::array({"confirmed media loss or RTT threshold exceeded"})},
        {"evidence_against", nlohmann::json::array()},
        {"missing_measurements", std::move(missing)}
      });
    }
    if (decoder_gap) {
      auto missing = nlohmann::json::array();
      if (!receive_decode_render_complete) {
        missing.push_back("decoded_frame_counter");
        missing.push_back("queue_latency");
        missing.push_back("render_latency");
      }
      result["hypotheses"].push_back({
        {"id", "decoder_or_render_pressure"}, {"confidence", windows_complete && decode_ms > 0.0 ? "medium" : "low"},
        {"evidence_for", nlohmann::json::array({"rendered cadence trails received cadence"})},
        {"evidence_against", nlohmann::json::array()},
        {"missing_measurements", std::move(missing)}
      });
    }
    if (encode_warning) {
      result["hypotheses"].push_back({
        {"id", "encoder_pressure"}, {"confidence", windows_complete ? "high" : "medium"},
        {"evidence_for", nlohmann::json::array({"encoded cadence or dropped-frame ratio is degraded"})},
        {"evidence_against", nlohmann::json::array()},
        {"missing_measurements", nlohmann::json::array()}
      });
    }
    if (pacing_gap && !network_warning && !decoder_gap) {
      auto missing = nlohmann::json::array();
      if (!source_capture_measured) missing.push_back("source_duplicate_frame_counter");
      result["hypotheses"].push_back({
        {"id", "source_or_capture_cadence"}, {"confidence", "low"},
        {"evidence_for", nlohmann::json::array({"rendered cadence is below the stream target"})},
        {"evidence_against", nlohmann::json::array({"confirmed network evidence is clean"})},
        {"missing_measurements", std::move(missing)}
      });
    }

    const int warning_count = static_cast<int>(network_warning) + static_cast<int>(pacing_gap) +
      static_cast<int>(decoder_gap) + static_cast<int>(encode_warning);
    std::string primary_issue = "undetermined";
    if (windows_complete && core_evidence_adequate && warning_count == 1) {
      if (network_warning) primary_issue = "network_pressure";
      else if (encode_warning) primary_issue = "encoder_pressure";
      else if (decoder_gap) primary_issue = "decoder_or_render_pressure";
      else if (pacing_gap) primary_issue = "source_or_capture_cadence";
    }
    const bool trial_action_exposed = trials_enabled() && windows_complete &&
      core_evidence_adequate && pacing_gap && !network_warning && !decoder_gap && !encode_warning;
    result["mode"] = trials_enabled() ? "trial_enabled" : "shadow";
    result["actions_exposed"] = trial_action_exposed;
    result["primary_issue"] = primary_issue;
    result["stable"] = windows_complete && core_evidence_adequate && warning_count == 0;
    result["state"] = windows_complete && core_evidence_adequate ? "classified" : "collecting";
    result["confidence"] = windows_complete && core_evidence_adequate ? "high" : "low";
    result["actions"].push_back(network_warning ? nlohmann::json {
      {"id", "lower_bitrate"}, {"capability", "auto_fix"},
      {"exposed", false}, {"reason", "shadow_mode"}
    } : trial_action_exposed ? nlohmann::json {
      {"id", "run_pacing_trial"}, {"capability", "run_trial"},
      {"exposed", true}, {"reason", "explicit_trial_flag_and_complete_evidence"}
    } : pacing_gap ? nlohmann::json {
      {"id", "recheck_pacing"}, {"capability", "recheck"},
      {"exposed", false}, {"reason", "trial actions remain shadowed or evidence is incomplete"}
    } : nlohmann::json {
      {"id", "manual_review"}, {"capability", "manual"},
      {"exposed", false}, {"reason", "shadow_mode"}
    });
    return result;
  }

#ifdef POLARIS_TESTS
  void clear_for_tests() {
    std::lock_guard lock(samples_mutex);
    samples_by_scope.clear();
    test_now_ms.store(-1, std::memory_order_relaxed);
  }

  void set_now_ms_for_tests(std::int64_t value) {
    test_now_ms.store(value, std::memory_order_relaxed);
  }
#endif

}  // namespace doctor_v2
