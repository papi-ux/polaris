/**
 * @file src/doctor_ai_explanation.cpp
 * @brief The paired Doctor AI explanation request (#613): strict input, typed projection, bounded output.
 */
#include "doctor_ai_explanation.h"

#include "crypto.h"
#include "generated/polaris_v1_contract.h"
#include "logging.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <regex>
#include <set>
#include <utility>

using namespace std::literals;

namespace doctor_ai_explanation {
  namespace v1 = polaris::contract::v1;

  namespace {
    using steady = std::chrono::steady_clock;

    struct code_info_t {
      code_e code;
      std::string_view name;
      int status;
    };

    constexpr std::array<code_info_t, 21> k_codes {{
      {code_e::explained, "explained", 200},
      {code_e::invalid_request, "invalid_request", 400},
      {code_e::body_too_large, "body_too_large", 413},
      {code_e::unsupported_media_type, "unsupported_media_type", 415},
      {code_e::unauthorized, "unauthorized", 401},
      {code_e::permission_required, "permission_required", 403},
      {code_e::active_owner_required, "active_owner_required", 403},
      {code_e::unsupported_scope, "unsupported_scope", 403},
      {code_e::scope_changed, "scope_changed", 409},
      {code_e::certificate_changed, "certificate_changed", 409},
      {code_e::finding_changed, "finding_changed", 409},
      {code_e::snapshot_expired, "snapshot_expired", 409},
      {code_e::duplicate_request, "duplicate_request", 409},
      {code_e::host_ai_disabled, "host_ai_disabled", 503},
      {code_e::provider_not_ready, "provider_not_ready", 503},
      {code_e::provider_failed, "provider_failed", 502},
      {code_e::invalid_explanation, "invalid_explanation", 502},
      {code_e::provider_timeout, "provider_timeout", 504},
      {code_e::request_busy, "request_busy", 429},
      {code_e::rate_limited, "rate_limited", 429},
      {code_e::server_stopping, "server_stopping", 503},
    }};

    // ---- UTF-8 ----

    bool next_code_point(std::string_view text, std::size_t &at, char32_t &out) {
      const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(text[index]); };
      const unsigned char lead = byte(at);
      std::size_t length = 0;
      char32_t value = 0;
      if (lead < 0x80) {
        out = lead;
        ++at;
        return true;
      } else if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        value = lead & 0x1F;
      } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        value = lead & 0x0F;
      } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = lead & 0x07;
      } else {
        return false;
      }
      if (at + length > text.size()) return false;
      for (std::size_t index = 1; index < length; ++index) {
        const auto continuation = byte(at + index);
        if ((continuation & 0xC0) != 0x80) return false;
        value = (value << 6) | (continuation & 0x3F);
      }
      if ((length == 3 && value < 0x800) || (length == 4 && (value < 0x10000 || value > 0x10FFFF)) ||
          (value >= 0xD800 && value <= 0xDFFF)) {
        return false;
      }
      out = value;
      at += length;
      return true;
    }

    /// Plain informational prose: no control, formatting or direction-override characters.
    bool plain_text(std::string_view text) {
      std::size_t at = 0;
      while (at < text.size()) {
        char32_t point = 0;
        if (!next_code_point(text, at, point)) return false;
        if ((point < 0x20 && point != U'\n') || point == 0x7F || (point >= 0x80 && point <= 0x9F) ||
            (point >= 0x200B && point <= 0x200F) || (point >= 0x202A && point <= 0x202E) ||
            (point >= 0x2066 && point <= 0x2069) || point == 0xFEFF) {
          return false;
        }
      }
      return true;
    }

    std::string lower_ascii(std::string_view text) {
      std::string lowered(text);
      std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
      });
      return lowered;
    }

    std::string hex(const unsigned char *data, std::size_t size) {
      static constexpr char digits[] = "0123456789abcdef";
      std::string out;
      out.reserve(size * 2);
      for (std::size_t index = 0; index < size; ++index) {
        out.push_back(digits[data[index] >> 4]);
        out.push_back(digits[data[index] & 0x0F]);
      }
      return out;
    }

    bool lower_hex_of_length(std::string_view text, std::size_t length) {
      return text.size() == length && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
    }

    // ---- Typed reads from the final Doctor ----

    /// A short categorical identifier ([a-z0-9_.-], at most 64 bytes); empty is allowed.
    std::optional<std::string> token_at(const nlohmann::json &object, const char *key) {
      if (!object.is_object()) return std::nullopt;
      const auto it = object.find(key);
      if (it == object.end() || !it->is_string()) return std::nullopt;
      const auto &value = it->get_ref<const std::string &>();
      if (value.size() > 64) return std::nullopt;
      for (const char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) {
          return std::nullopt;
        }
      }
      return value;
    }

    template<class Allowed>
    std::string one_of_in(const std::optional<std::string> &value, const Allowed &allowed, std::string_view fallback) {
      if (value && std::find(allowed.begin(), allowed.end(), std::string_view {*value}) != allowed.end()) {
        return *value;
      }
      return std::string {fallback};
    }

    std::string one_of(const std::optional<std::string> &value,
                       std::initializer_list<std::string_view> allowed,
                       std::string_view fallback) {
      return one_of_in(value, allowed, fallback);
    }

    const nlohmann::json *evidence_row(const nlohmann::json &doctor, std::string_view id) {
      const auto evidence = doctor.find("evidence");
      if (evidence == doctor.end() || !evidence->is_array()) return nullptr;
      for (const auto &row : *evidence) {
        if (row.is_object() && row.contains("id") && row["id"].is_string() &&
            row["id"].get_ref<const std::string &>() == id) {
          return &row;
        }
      }
      return nullptr;
    }

    // Rows whose categorical status/source/availability make up the meaningful finding.
    constexpr std::array<std::string_view, 11> k_fingerprint_rows {
      "bitrate", "capture_path", "control_channel_packet_loss", "encoder", "frame_pacing", "latency",
      "live_bitrate_control", "live_bitrate_owner", "packet_loss", "streaming", "target_fps_gap",
    };

    constexpr std::array<std::string_view, 23> k_primary_issues {
      "none", "unknown", "network_jitter", "network_observation", "encoder_load", "capture_missing",
      "frame_pacing", "cadence_collecting", "pyrowave_starved", "quality_reduced_live",
      "pyrowave_needs_more_than_allowed", "control_channel_observation", "steam_input_conflict",
      "gpu_native_requested_shm_fallback", "headless_shm_fallback", "shm_capture", "no_capture_metadata",
      "headless_extcopy_dmabuf_cross_gpu_risk", "headless_extcopy_dmabuf", "windowed_dmabuf_override",
      "gpu_native", "dmabuf_gpu_capture", "mixed_or_unknown",
    };

    constexpr std::array<std::string_view, 10> k_confidence_bases {
      "unknown", "insufficient_data", "direct_telemetry", "local_steam_config_and_isolation_plan",
      "session_policy_and_live_telemetry", "live_evidence_overrode_stale_finding", "live_evidence_recheck",
      "control_channel_only", "pacing_window_collecting", "live_evidence_overrode_unconfirmed_pacing",
    };

    constexpr std::array<std::string_view, 11> k_capture_reasons {
      "unknown", "no_capture_metadata", "headless_extcopy_dmabuf_cross_gpu_risk", "headless_extcopy_dmabuf",
      "windowed_dmabuf_override", "gpu_native", "gpu_native_requested_shm_fallback", "headless_shm_fallback",
      "shm_capture", "dmabuf_gpu_capture", "mixed_or_unknown",
    };

    struct metric_spec_t {
      std::string_view id;
      std::string_view unit;
      std::string_view source;
      double maximum;
    };

    // The six typed rows. Bitrate comes from the observed encoder rate, not the Doctor's live target.
    constexpr std::array<metric_spec_t, 6> k_metrics {{
      {"bitrate", "kbps", "stream_stats", 1000000.0},
      {"control_channel_packet_loss", "%", "enet_control_channel", 100.0},
      {"frame_pacing", "ms", "stream_stats", 60000.0},
      {"latency", "ms", "stream_stats", 60000.0},
      {"packet_loss", "%", "media_transport", 100.0},
      {"target_fps_gap", "FPS", "stream_stats", 1000.0},
    }};

    std::optional<double> finite_in_range(const nlohmann::json *row, double maximum) {
      if (!row) return std::nullopt;
      const auto value = row->find("value");
      if (value == row->end() || !value->is_number()) return std::nullopt;
      const auto number = value->get<double>();
      if (!std::isfinite(number) || number < 0.0 || number > maximum) return std::nullopt;
      return std::round(number * 1000.0) / 1000.0;
    }

    nlohmann::json metric_row(const nlohmann::json &doctor, const metric_spec_t &spec, const projection_inputs_t &inputs) {
      const auto *row = evidence_row(doctor, spec.id);
      std::optional<double> value;
      if (spec.id == "bitrate") {
        if (inputs.observed_encoder_kbps && *inputs.observed_encoder_kbps >= 0 &&
            *inputs.observed_encoder_kbps <= static_cast<std::int64_t>(spec.maximum)) {
          value = static_cast<double>(*inputs.observed_encoder_kbps);
        }
      } else if (row && token_at(*row, "source").value_or("") == spec.source) {
        // A row whose own source says unavailable stays unavailable; loss is never turned into zero.
        value = finite_in_range(row, spec.maximum);
      }
      nlohmann::json out = nlohmann::json::object();
      out["id"] = std::string {spec.id};
      out["unit"] = std::string {spec.unit};
      if (!value) {
        out["status"] = "unknown";
        out["source"] = "unavailable";
        out["value"] = nullptr;
        return out;
      }
      out["status"] = row ? one_of(token_at(*row, "status"), {"pass", "watch", "fail", "info", "unknown"}, "unknown") : "unknown";
      out["source"] = std::string {spec.source};
      if (spec.id == "bitrate") {
        out["value"] = static_cast<std::int64_t>(*value);
      } else {
        out["value"] = *value;
      }
      return out;
    }

    // ---- Request parsing ----

    bool media_type_is_json(std::string_view header) {
      const auto trim = [](std::string_view text) {
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string_view::npos) return std::string_view {};
        const auto last = text.find_last_not_of(" \t");
        return text.substr(first, last - first + 1);
      };
      const auto semicolon = header.find(';');
      if (lower_ascii(trim(header.substr(0, semicolon))) != "application/json") return false;
      if (semicolon == std::string_view::npos) return true;
      // Only a UTF-8 charset parameter is acceptable.
      const auto parameter = lower_ascii(trim(header.substr(semicolon + 1)));
      return parameter == "charset=utf-8" || parameter == "charset=\"utf-8\"";
    }

    std::optional<std::uint64_t> safe_integer(const nlohmann::json &value, std::uint64_t minimum) {
      if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number >= minimum && number <= k_max_safe_integer) return number;
        return std::nullopt;
      }
      if (value.is_number_integer()) {
        const auto number = value.get<std::int64_t>();
        if (number >= 0 && static_cast<std::uint64_t>(number) >= minimum &&
            static_cast<std::uint64_t>(number) <= k_max_safe_integer) {
          return static_cast<std::uint64_t>(number);
        }
      }
      return std::nullopt;
    }

    std::optional<std::string> bounded_string(const nlohmann::json &value, std::size_t minimum, std::size_t maximum) {
      if (!value.is_string()) return std::nullopt;
      const auto &text = value.get_ref<const std::string &>();
      const auto points = utf8_code_points(text);
      if (!points || *points < minimum || *points > maximum || text.size() > maximum * 2) return std::nullopt;
      return text;
    }

    bool request_uuid(std::string_view text) {
      static const std::regex pattern {"^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};
      return text.size() == 36 && std::regex_match(text.begin(), text.end(), pattern);
    }

    // ---- Redaction ----

    struct pattern_t {
      std::regex regex;
      const char *replacement;
    };

    const std::vector<pattern_t> &redaction_patterns() {
      static const std::vector<pattern_t> patterns = [] {
        const auto icase = std::regex::ECMAScript | std::regex::icase;
        std::vector<pattern_t> list;
        list.push_back({std::regex {R"((https?://)[^\s/@:]+:[^\s/@]+@)", icase}, "$1[redacted]:[redacted]@"});
        list.push_back({std::regex {R"(\b(Bearer|Basic)\s+[A-Za-z0-9._~+/=-]+)", icase}, "$1 [redacted]"});
        list.push_back({std::regex {R"(\b(cookie|set-cookie)(\s*[:=]\s*)[^\n;]+)", icase}, "$1$2[redacted]"});
        // A sensitive name, an optional closing quote/bracket, then its separator and one value; the
        // name and separator stay so the prose still reads (shapes from redaction-shape-cases.test.js).
        list.push_back({std::regex {R"(\b(api[_-]?key|access[_-]?token|refresh[_-]?token|session[_-]?token|auth[_-]?token|token|secret|client[_-]?secret|password|passwd|passphrase|authorization|private[_-]?key|pin)(s?["'\)\]]?\s*[:=]\s*)("[^"]*"|'[^']*'|\{[^}]*\}|\[[^\]]*\]|[^\s,;]+))", icase}, "$1$2[redacted]"});
        list.push_back({std::regex {R"(\b[A-Za-z][A-Za-z0-9+.-]*://[^\s"'<>]+)"}, "[redacted-url]"});
        list.push_back({std::regex {R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})"}, "[redacted-email]"});
        list.push_back({std::regex {R"(\b[A-Za-z]:\\[^\s"'<>]*)"}, "[redacted-path]"});
        list.push_back({std::regex {R"((^|[\s("'=,;])(~?/[^\s"'<>]+))"}, "$1[redacted-path]"});
        list.push_back({std::regex {R"(\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b)"}, "[redacted-address]"});
        list.push_back({std::regex {R"(\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b)"}, "[redacted-address]"});
        list.push_back({std::regex {R"(\b[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.(?:local|lan|home|internal|localdomain|ts\.net|home\.arpa)\b)", icase}, "[redacted-host]"});
        list.push_back({std::regex {R"(\b(?:sk|pk|ghp|gho|xox[abp])[-_][A-Za-z0-9_-]{8,})"}, "[redacted-token]"});
        list.push_back({std::regex {R"([A-Za-z0-9+/_-]{32,}={0,2})"}, "[redacted-token]"});
        list.push_back({std::regex {R"(\b[0-9A-Fa-f]{16,}\b)"}, "[redacted-token]"});
        return list;
      }();
      return patterns;
    }

    // IPv6: a run of hex digits, colons and dots that holds "::" or at least three colons.
    std::string redact_ipv6(const std::string &text) {
      std::string out;
      std::size_t at = 0;
      const auto run_char = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.';
      };
      while (at < text.size()) {
        if (!run_char(text[at])) {
          out.push_back(text[at++]);
          continue;
        }
        auto end = at;
        while (end < text.size() && run_char(text[end])) ++end;
        const auto run = std::string_view {text}.substr(at, end - at);
        const auto colons = std::count(run.begin(), run.end(), ':');
        if (run.find("::") != std::string_view::npos || colons >= 3) {
          out += "[redacted-address]";
        } else {
          out.append(run);
        }
        at = end;
      }
      return out;
    }

    std::string replace_identities(std::string text, const std::vector<std::string> &identities) {
      for (const auto &identity : identities) {
        if (identity.size() < 3) continue;
        const auto needle = lower_ascii(identity);
        auto lowered = lower_ascii(text);
        std::size_t at = 0;
        while ((at = lowered.find(needle, at)) != std::string::npos) {
          text.replace(at, identity.size(), "[redacted]");
          lowered.replace(at, identity.size(), "[redacted]");
          at += std::string_view {"[redacted]"}.size();
        }
      }
      return text;
    }

    // ---- Bounded explanation ----

    std::optional<std::string> clean_text(const nlohmann::json &value, std::size_t maximum,
                                          const std::vector<std::string> &identities) {
      const auto bounded = bounded_string(value, 0, maximum);
      if (!bounded || !plain_text(*bounded)) return std::nullopt;
      auto redacted = redact_text(*bounded, identities);
      const auto points = utf8_code_points(redacted);
      if (!points || *points > maximum || redacted.size() > maximum * 2 || !plain_text(redacted)) return std::nullopt;
      return redacted;
    }

    std::optional<nlohmann::json> clean_list(const nlohmann::json &value, std::size_t items, std::size_t maximum,
                                             const std::vector<std::string> &identities) {
      if (!value.is_array() || value.size() > items) return std::nullopt;
      nlohmann::json list = nlohmann::json::array();
      for (const auto &item : value) {
        const auto text = clean_text(item, maximum, identities);
        if (!text) return std::nullopt;
        list.push_back(*text);
      }
      return list;
    }

    /// The six-field explanation object a paired response may carry (manifest: doctor_ai_explanation).
    std::optional<nlohmann::json> bounded_explanation_json(const nlohmann::json &input,
                                                           const std::vector<std::string> &identities) {
      static constexpr std::array<std::string_view, 6> keys {
        "likely_cause", "evidence", "try_first", "advanced_detail", "confidence", "destructive_action_allowed"
      };
      if (!input.is_object() || input.size() != keys.size()) return std::nullopt;
      for (const auto key : keys) {
        if (!input.contains(std::string {key})) return std::nullopt;
      }
      const auto likely_cause = clean_text(input.at("likely_cause"), 512, identities);
      const auto evidence = clean_list(input.at("evidence"), 6, 384, identities);
      const auto try_first = clean_list(input.at("try_first"), 3, 384, identities);
      const auto advanced_detail = clean_text(input.at("advanced_detail"), 1536, identities);
      const auto &confidence = input.at("confidence");
      const auto &destructive = input.at("destructive_action_allowed");
      if (!likely_cause || !evidence || !try_first || !advanced_detail || !confidence.is_string() ||
          !destructive.is_boolean() || destructive.get<bool>()) {
        return std::nullopt;
      }
      const auto &level = confidence.get_ref<const std::string &>();
      if (level != "low" && level != "medium" && level != "high") return std::nullopt;
      nlohmann::json clean = nlohmann::json::object();
      clean[v1::doctor_ai_explanation::likely_cause] = *likely_cause;
      clean[v1::doctor_ai_explanation::evidence] = *evidence;
      clean[v1::doctor_ai_explanation::try_first] = *try_first;
      clean[v1::doctor_ai_explanation::advanced_detail] = *advanced_detail;
      clean[v1::doctor_ai_explanation::confidence] = level;
      clean[v1::doctor_ai_explanation::destructive_action_allowed] = false;
      return clean;
    }

    /// The pure status descriptor (manifest: doctor_ai_explanation_request).
    nlohmann::json request_descriptor_json(const scope_view_t &scope, const config_view_t &config, const peek_t &peek) {
      const auto finding = finding_of(scope.doctor);
      const bool permitted = scope.cert_record && scope.permission_view && !scope.unsupported_scope && scope.sole_active_owner;
      const bool evidence_ready = finding.supported && project_evidence(scope.doctor, scope.projection).has_value();
      std::string blocked;
      if (scope.stopping) {
        blocked = "server_stopping";
      } else if (scope.unsupported_scope) {
        blocked = "unsupported_scope";
      } else if (!scope.cert_record || !scope.permission_view) {
        blocked = "permission_required";
      } else if (!scope.sole_active_owner) {
        blocked = "active_owner_required";
      } else if (!config.enabled) {
        blocked = "host_ai_disabled";
      } else if (!config.transport_ready) {
        blocked = "provider_not_ready";
      } else if (!evidence_ready) {
        blocked = "evidence_unavailable";
      } else if (peek.busy) {
        blocked = "request_busy";
      } else if (peek.rate_limited) {
        blocked = "rate_limited";
      }
      nlohmann::json out = nlohmann::json::object();
      out[v1::doctor_ai_explanation_request::schema] = 1;
      out[v1::doctor_ai_explanation_request::enabled] = config.enabled;
      out[v1::doctor_ai_explanation_request::permitted] = permitted;
      out[v1::doctor_ai_explanation_request::available] = blocked.empty();
      out[v1::doctor_ai_explanation_request::blocked_reason] = blocked;
      out[v1::doctor_ai_explanation_request::source_result_id] = finding.source_result_id;
      out[v1::doctor_ai_explanation_request::finding_fingerprint] =
        permitted && finding.supported ? finding.fingerprint : std::string {};
      out[v1::doctor_ai_explanation_request::controller_revision] =
        permitted ? std::min<std::uint64_t>(scope.controller_revision, k_max_safe_integer) : 0;
      return out;
    }

    /// One paired response (manifest: doctor_ai_explanation_response).
    nlohmann::json explanation_response_json(code_e code,
                                             const response_identity_t &identity,
                                             std::int64_t expires_in_ms,
                                             std::optional<std::int64_t> retry_after_ms,
                                             const std::optional<nlohmann::json> &explanation) {
      const bool success = code == code_e::explained && explanation.has_value();
      const auto served_code = code == code_e::explained && !success ? code_e::invalid_explanation : code;
      nlohmann::json out = nlohmann::json::object();
      out[v1::doctor_ai_explanation_response::schema] = 1;
      out[v1::doctor_ai_explanation_response::status] = success;
      out[v1::doctor_ai_explanation_response::fallback] = !success;
      out[v1::doctor_ai_explanation_response::changed] = false;
      out[v1::doctor_ai_explanation_response::code] = std::string {code_name(served_code)};
      out[v1::doctor_ai_explanation_response::request_id] = identity.request_id;
      out[v1::doctor_ai_explanation_response::app_session_id] = identity.app_session_id;
      out[v1::doctor_ai_explanation_response::session_generation] = identity.session_generation;
      out[v1::doctor_ai_explanation_response::source_result_id] = identity.source_result_id;
      out[v1::doctor_ai_explanation_response::snapshot_age_ms] =
        identity.snapshot_age_ms ? nlohmann::json(std::max<std::int64_t>(*identity.snapshot_age_ms, 0)) : nlohmann::json(nullptr);
      out[v1::doctor_ai_explanation_response::expires_in_ms] =
        success ? std::clamp<std::int64_t>(expires_in_ms, 0, k_snapshot_lifetime.count()) : 0;
      out[v1::doctor_ai_explanation_response::retry_after_ms] =
        retry_after_ms ? nlohmann::json(std::max<std::int64_t>(*retry_after_ms, 0)) : nlohmann::json(nullptr);
      out[v1::doctor_ai_explanation_response::authority] = "explanation_only";
      out[v1::doctor_ai_explanation_response::may_define_settings] = false;
      out[v1::doctor_ai_explanation_response::source] = {{"kind", "ai-explanation"}, {"mode", "host-configured"}, {"informational", true}};
      out[v1::doctor_ai_explanation_response::explanation] = success ? *explanation : nlohmann::json(nullptr);
      out[v1::doctor_ai_explanation_response::snapshot_id] = identity.snapshot_id;
      out[v1::doctor_ai_explanation_response::finding_fingerprint] = identity.finding_fingerprint;
      out[v1::doctor_ai_explanation_response::controller_revision] = identity.controller_revision;
      return out;
    }

    std::int64_t milliseconds_between(steady::time_point earlier, steady::time_point later) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(later - earlier).count();
    }

    std::int64_t ceil_milliseconds(steady::duration duration) {
      return std::max<std::int64_t>(0, std::chrono::ceil<std::chrono::milliseconds>(duration).count());
    }
  }  // namespace

  std::string_view code_name(code_e code) {
    for (const auto &info : k_codes) {
      if (info.code == code) return info.name;
    }
    return "provider_failed";
  }

  int http_status(code_e code) {
    for (const auto &info : k_codes) {
      if (info.code == code) return info.status;
    }
    return 502;
  }

  std::optional<std::size_t> utf8_code_points(std::string_view text) {
    std::size_t at = 0;
    std::size_t count = 0;
    while (at < text.size()) {
      char32_t point = 0;
      if (!next_code_point(text, at, point)) return std::nullopt;
      ++count;
    }
    return count;
  }

  finding_t finding_of(const nlohmann::json &doctor) {
    finding_t finding;
    if (!doctor.is_object()) return finding;
    if (const auto id = doctor.find("result_id"); id != doctor.end()) {
      const auto source_id = bounded_string(*id, 0, 256);
      if (!source_id) return finding;
      finding.source_result_id = *source_id;
    }

    nlohmann::json canonical = nlohmann::json::object();
    for (const char *key : {"status", "primary_issue", "traffic_light", "severity"}) {
      const auto value = token_at(doctor, key);
      if (!value || value->empty()) return finding;
      canonical[key] = *value;
    }
    for (const char *key : {"contract", "derived_from"}) {
      if (!doctor.contains(key)) {
        canonical[key] = "";
        continue;
      }
      const auto value = token_at(doctor, key);
      if (!value) return finding;
      canonical[key] = *value;
    }
    const auto confidence = doctor.find("confidence");
    if (confidence == doctor.end() || !confidence->is_object()) return finding;
    const auto level = token_at(*confidence, "level");
    const auto basis = token_at(*confidence, "basis");
    if (!level || !basis) return finding;
    canonical["confidence"] = {{"level", *level}, {"basis", *basis}};

    const auto evidence = doctor.find("evidence");
    if (evidence == doctor.end() || !evidence->is_array()) return finding;
    std::vector<nlohmann::json> rows;
    for (const auto &row : *evidence) {
      if (!row.is_object()) return finding;
      const auto id = token_at(row, "id");
      if (!id) return finding;
      if (std::find(k_fingerprint_rows.begin(), k_fingerprint_rows.end(), std::string_view {*id}) == k_fingerprint_rows.end()) {
        continue;
      }
      const auto status = token_at(row, "status");
      const auto source = token_at(row, "source");
      if (!status || !source) return finding;
      const auto value = row.find("value");
      const bool available = value != row.end() && !value->is_null();
      nlohmann::json tuple = nlohmann::json::array({*id, *status, *source, available});
      if (*id == "live_bitrate_owner") {
        // Who owns the live bitrate is categorical and meaningful; the value is one of three tokens.
        tuple.push_back(token_at(row, "value").value_or(""));
      }
      rows.push_back(std::move(tuple));
    }
    std::sort(rows.begin(), rows.end());
    canonical["evidence"] = rows;

    nlohmann::json action = nlohmann::json::object();
    if (const auto found = doctor.find("safe_recovery_action"); found != doctor.end()) {
      if (!found->is_object()) return finding;
      for (const char *key : {"id", "capability", "kind"}) {
        const auto value = token_at(*found, key);
        if (!value) return finding;
        action[key] = *value;
      }
      for (const char *key : {"requires_owner", "owner_tuning_allowed", "allowed_in_viewer_mode",
                              "requires_confirmation", "destructive"}) {
        const auto value = found->find(key);
        action[key] = value != found->end() && value->is_boolean() ? nlohmann::json(value->get<bool>()) : nlohmann::json(nullptr);
      }
    }
    canonical["safe_recovery_action"] = action;

    const auto digest = crypto::hash(canonical.dump());
    finding.fingerprint = hex(digest.data(), digest.size());
    finding.supported = true;
    return finding;
  }

  std::optional<std::string> project_evidence(const nlohmann::json &doctor, const projection_inputs_t &inputs) {
    if (!doctor.is_object()) return std::nullopt;
    const auto status = one_of(token_at(doctor, "status"), {"ok", "needs_action", "unknown"}, "unknown");
    const auto primary_issue = one_of_in(token_at(doctor, "primary_issue"), k_primary_issues, "unknown");
    const auto severity = one_of(token_at(doctor, "severity"), {"info", "warning", "critical"}, "info");
    const auto traffic_light = one_of(token_at(doctor, "traffic_light"), {"green", "amber", "red"}, "amber");
    const auto confidence = doctor.value("confidence", nlohmann::json::object());
    const auto level = one_of(token_at(confidence, "level"), {"low", "medium", "high", "unknown"}, "unknown");
    const auto basis = one_of_in(token_at(confidence, "basis"), k_confidence_bases, "unknown");

    nlohmann::json rows = nlohmann::json::array();
    for (const auto &spec : k_metrics) {
      rows.push_back(metric_row(doctor, spec, inputs));
    }

    std::string owner = "unknown";
    if (const auto *row = evidence_row(doctor, "live_bitrate_owner")) {
      owner = one_of(token_at(*row, "value"), {"auto_safe", "doctor_available", "fixed_session"}, "unknown");
    }

    nlohmann::json pacing = {{"ready", false}, {"confirmed", false}, {"available", nullptr}};
    const auto advanced = doctor.find("advanced_evidence");
    if (advanced != doctor.end() && advanced->is_object()) {
      const auto coverage = advanced->find("pacing_coverage");
      if (coverage != advanced->end() && coverage->is_object()) {
        for (const char *key : {"ready", "confirmed", "available"}) {
          const auto value = coverage->find(key);
          if (value != coverage->end() && value->is_boolean()) pacing[key] = value->get<bool>();
        }
      }
    }

    nlohmann::json projection = nlohmann::json::object();
    projection["schema"] = 1;
    projection["deterministic_source_of_truth"] = {
      {"status", status},
      {"primary_issue", primary_issue},
      {"severity", severity},
      {"traffic_light", traffic_light},
      {"confidence", {{"level", level}, {"basis", basis}}},
      {"summary", "Deterministic Doctor status: " + status + "; primary issue: " + primary_issue + "."},
    };
    projection["doctor"] = {
      {"evidence", rows},
      {"capture_reason", one_of_in(std::optional<std::string> {inputs.capture_reason}, k_capture_reasons, "unknown")},
      {"live_bitrate_owner", owner},
      {"pacing_coverage", pacing},
      {"limits", nlohmann::json::array({
        "Deterministic Doctor owns every diagnosis, action and setting; this explanation cannot change any of them.",
        "Control-channel loss is not video frame loss.",
        "These observations do not verify what the player sees on screen.",
        "Values describe one captured reading and may have changed since.",
      })},
    };
    auto text = projection.dump();
    if (text.size() > k_max_evidence_bytes) return std::nullopt;
    return text;
  }

  parse_result_t parse_request(std::string_view content_type, std::string_view query_string, std::string_view body) {
    parse_result_t result;
    if (!media_type_is_json(content_type)) {
      result.code = code_e::unsupported_media_type;
      return result;
    }
    if (body.size() > k_max_request_bytes) {
      result.code = code_e::body_too_large;
      return result;
    }
    result.code = code_e::invalid_request;
    if (!query_string.empty() || !utf8_code_points(body)) return result;

    std::set<std::string> keys;
    bool duplicate = false;
    bool nested = false;
    nlohmann::json::parser_callback_t callback = [&](int depth, nlohmann::json::parse_event_t event, nlohmann::json &parsed) {
      if (event == nlohmann::json::parse_event_t::key && depth == 1 && parsed.is_string()) {
        if (!keys.insert(parsed.get<std::string>()).second) duplicate = true;
      }
      if ((event == nlohmann::json::parse_event_t::object_start || event == nlohmann::json::parse_event_t::array_start) && depth > 0) {
        nested = true;
      }
      return true;
    };
    const auto parsed = nlohmann::json::parse(body.begin(), body.end(), callback, false);
    if (parsed.is_discarded() || !parsed.is_object() || duplicate || nested || parsed.size() != 7) return result;
    for (const char *key : {"schema", "request_id", "app_session_id", "session_generation", "source_result_id",
                            "finding_fingerprint", "controller_revision"}) {
      if (!parsed.contains(key)) return result;
    }
    const auto schema = safe_integer(parsed["schema"], 1);
    if (!schema || *schema != 1) return result;
    const auto &request_id = parsed["request_id"];
    if (!request_id.is_string() || !request_uuid(request_id.get_ref<const std::string &>())) return result;
    const auto app_session_id = bounded_string(parsed["app_session_id"], 1, 256);
    const auto session_generation = safe_integer(parsed["session_generation"], 1);
    const auto source_result_id = bounded_string(parsed["source_result_id"], 0, 256);
    const auto &fingerprint = parsed["finding_fingerprint"];
    const auto controller_revision = safe_integer(parsed["controller_revision"], 1);
    if (!app_session_id || !session_generation || !source_result_id || !fingerprint.is_string() ||
        !lower_hex_of_length(fingerprint.get_ref<const std::string &>(), 64) || !controller_revision) {
      return result;
    }
    result.request = request_t {
      .request_id = request_id.get<std::string>(),
      .app_session_id = *app_session_id,
      .session_generation = *session_generation,
      .source_result_id = *source_result_id,
      .finding_fingerprint = fingerprint.get<std::string>(),
      .controller_revision = *controller_revision,
    };
    result.code = code_e::explained;
    return result;
  }

  std::string redact_text(std::string_view text, const std::vector<std::string> &identities) {
    auto out = replace_identities(std::string {text}, identities);
    for (const auto &pattern : redaction_patterns()) {
      out = std::regex_replace(out, pattern.regex, pattern.replacement);
    }
    return redact_ipv6(out);
  }

  std::optional<nlohmann::json> sanitize_explanation(const nlohmann::json &explanation,
                                                     const std::vector<std::string> &identities) {
    return bounded_explanation_json(explanation, identities);
  }

  nlohmann::json descriptor(const scope_view_t &scope, const config_view_t &config, const peek_t &peek) {
    return request_descriptor_json(scope, config, peek);
  }

  std::string response_body(code_e code,
                            const response_identity_t &identity,
                            std::int64_t expires_in_ms,
                            std::optional<std::int64_t> retry_after_ms,
                            const std::optional<nlohmann::json> &explanation) {
    return explanation_response_json(code, identity, expires_in_ms, retry_after_ms, explanation).dump();
  }

  config_view_t current_config() {
    auto snapshot = ai_optimizer::paired_explanation_config();
    config_view_t view;
    view.enabled = snapshot.enabled;
    view.transport_ready = snapshot.transport_ready;
    view.generation = snapshot.generation;
    view.config = std::move(snapshot.config);
    return view;
  }

  provider_outcome_t default_provider(const provider_call_t &call) {
    const auto result = ai_optimizer::explain_paired_doctor(call.config, call.evidence, call.cancel);
    provider_outcome_t outcome;
    switch (result.status) {
      case ai_optimizer::paired_explanation_status_e::explained: {
        auto parsed = nlohmann::json::parse(result.explanation_json.value_or(""), nullptr, false);
        if (parsed.is_discarded()) {
          outcome.code = code_e::invalid_explanation;
        } else {
          outcome.code = code_e::explained;
          outcome.explanation = std::move(parsed);
        }
        break;
      }
      case ai_optimizer::paired_explanation_status_e::invalid_explanation:
        outcome.code = code_e::invalid_explanation;
        break;
      case ai_optimizer::paired_explanation_status_e::provider_timeout:
        outcome.code = code_e::provider_timeout;
        break;
      case ai_optimizer::paired_explanation_status_e::provider_not_ready:
        outcome.code = code_e::provider_not_ready;
        break;
      case ai_optimizer::paired_explanation_status_e::provider_failed:
      default:
        outcome.code = code_e::provider_failed;
        break;
    }
    return outcome;
  }

  std::string random_snapshot_id() {
    std::array<unsigned char, 16> bytes {};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
    return hex(bytes.data(), bytes.size());
  }

  // ---- Request store and owned worker ----

  struct service_t::snapshot_t {
    request_t request;
    scope_reader_t read_scope;
    writer_t write;
    std::shared_ptr<const void> cert_record;
    std::string fingerprint;
    std::string source_result_id;
    std::string app_session_id;
    std::uint64_t session_generation = 0;
    std::uint64_t controller_revision = 0;
    std::uint64_t config_generation = 0;
    ai_optimizer::config_t config;
    std::string evidence;
    std::string snapshot_id;
    steady::time_point created {};
    /// The caller's own private strings as captured, kept for redaction only.
    std::vector<std::string> identities;
  };

  service_t::service_t(hooks_t hooks):
      _hooks(std::move(hooks)) {
    if (!_hooks.read_config) _hooks.read_config = current_config;
    if (!_hooks.provider) _hooks.provider = default_provider;
    if (!_hooks.clock) _hooks.clock = [] { return steady::now(); };
    if (!_hooks.snapshot_id) _hooks.snapshot_id = random_snapshot_id;
    _worker = std::thread([this] { run(); });
  }

  service_t::~service_t() {
    shutdown();
  }

  peek_t service_t::peek(std::string_view cert_uuid) const {
    const auto now = _hooks.clock();
    std::lock_guard lock(_mutex);
    const auto within = [&](const std::deque<steady::time_point> &attempts) {
      return static_cast<std::size_t>(std::count_if(attempts.begin(), attempts.end(), [&](const auto &at) {
        return now - at < k_rate_window;
      }));
    };
    peek_t peek;
    peek.busy = _in_flight;
    bool cert_limited = false;
    if (const auto it = _cert_attempts.find(cert_uuid); it != _cert_attempts.end()) {
      cert_limited = within(it->second) >= k_attempts_per_window;
    }
    peek.rate_limited = within(_global_attempts) >= k_attempts_per_window || cert_limited;
    return peek;
  }

  config_view_t service_t::config() const {
    return _hooks.read_config();
  }

  counters_t service_t::counters() const {
    std::lock_guard lock(_mutex);
    return _counters;
  }

  void service_t::submit(const request_t &request, const scope_reader_t &read_scope, writer_t write) {
    response_identity_t echo {
      .request_id = request.request_id,
      .app_session_id = request.app_session_id,
      .session_generation = request.session_generation,
      .source_result_id = request.source_result_id,
      .finding_fingerprint = request.finding_fingerprint,
      .controller_revision = request.controller_revision,
    };
    const auto refuse = [&](code_e code, std::optional<std::int64_t> retry_after_ms = std::nullopt) {
      write(http_status(code), response_body(code, echo, 0, retry_after_ms, std::nullopt));
    };

    // Every comparison claim is checked against the host's own current state before anything is reserved.
    auto scope = read_scope();
    if (!scope.cert_record) {
      // Never echo identity before authorization.
      write(http_status(code_e::unauthorized), response_body(code_e::unauthorized, {}, 0, std::nullopt, std::nullopt));
      return;
    }
    if (!scope.permission_view) return refuse(code_e::permission_required);
    if (scope.unsupported_scope) return refuse(code_e::unsupported_scope);
    if (scope.stopping) return refuse(code_e::server_stopping);
    if (!scope.sole_active_owner) return refuse(code_e::active_owner_required);
    if (scope.app_session_id != request.app_session_id || scope.session_generation != request.session_generation) {
      return refuse(code_e::scope_changed);
    }
    const auto finding = finding_of(scope.doctor);
    if (!finding.supported || finding.fingerprint != request.finding_fingerprint ||
        finding.source_result_id != request.source_result_id) {
      return refuse(code_e::finding_changed);
    }
    if (scope.controller_revision != request.controller_revision) return refuse(code_e::scope_changed);
    const auto config = _hooks.read_config();
    if (!config.enabled) return refuse(code_e::host_ai_disabled);
    if (!config.transport_ready) return refuse(code_e::provider_not_ready);
    auto evidence = project_evidence(scope.doctor, scope.projection);
    if (!evidence) return refuse(code_e::finding_changed);

    // One lock covers the budget check, the record of the attempt and the slot reservation.
    std::optional<code_e> refusal;
    std::optional<std::int64_t> retry_after_ms;
    {
      std::lock_guard lock(_mutex);
      const auto now = _hooks.clock();
      const auto expired = [&](const steady::time_point &at) { return now - at >= k_rate_window; };
      while (!_global_attempts.empty() && expired(_global_attempts.front())) _global_attempts.pop_front();
      while (!_tombstones.empty() && expired(_tombstones.front().second)) _tombstones.pop_front();
      for (auto it = _cert_attempts.begin(); it != _cert_attempts.end();) {
        while (!it->second.empty() && expired(it->second.front())) it->second.pop_front();
        it = it->second.empty() ? _cert_attempts.erase(it) : std::next(it);
      }
      const bool duplicate = std::any_of(_tombstones.begin(), _tombstones.end(), [&](const auto &tombstone) {
        return tombstone.first == request.request_id;
      });
      const auto cert = _cert_attempts.find(scope.cert_uuid);
      const bool cert_full = cert != _cert_attempts.end() && cert->second.size() >= k_attempts_per_window;
      const bool global_full = _global_attempts.size() >= k_attempts_per_window;
      if (!_accepting) {
        refusal = code_e::server_stopping;
      } else if (duplicate) {
        refusal = code_e::duplicate_request;
      } else if (_in_flight) {
        refusal = code_e::request_busy;
      } else if (cert_full || global_full) {
        refusal = code_e::rate_limited;
        std::int64_t wait = 0;
        if (global_full) wait = std::max(wait, ceil_milliseconds(_global_attempts.front() + k_rate_window - now));
        if (cert_full) wait = std::max(wait, ceil_milliseconds(cert->second.front() + k_rate_window - now));
        retry_after_ms = wait;
      } else {
        auto id = _hooks.snapshot_id();
        ++_counters.snapshots_allocated;
        if (!lower_hex_of_length(id, 32)) {
          refusal = code_e::provider_not_ready;
        } else {
          _in_flight = true;
          _global_attempts.push_back(now);
          _cert_attempts[scope.cert_uuid].push_back(now);
          _tombstones.emplace_back(request.request_id, now);
          ++_counters.accepted_attempts;
          auto job = std::make_unique<snapshot_t>();
          job->request = request;
          job->read_scope = read_scope;
          job->write = std::move(write);
          job->cert_record = scope.cert_record;
          job->fingerprint = finding.fingerprint;
          job->source_result_id = finding.source_result_id;
          job->app_session_id = scope.app_session_id;
          job->session_generation = scope.session_generation;
          job->controller_revision = scope.controller_revision;
          job->config_generation = config.generation;
          job->config = config.config;
          job->evidence = std::move(*evidence);
          job->snapshot_id = std::move(id);
          job->created = now;
          job->identities = scope.identities;
          _job = std::move(job);
          _changed.notify_all();
        }
      }
    }
    if (refusal) refuse(*refusal, retry_after_ms);
  }

  std::optional<code_e> service_t::recheck(const snapshot_t &snapshot, std::vector<std::string> *identities) {
    {
      std::lock_guard lock(_mutex);
      if (!_accepting) return code_e::server_stopping;
    }
    if (_hooks.clock() - snapshot.created >= k_snapshot_lifetime) return code_e::snapshot_expired;
    // The reader takes and releases its own short guards; nothing is held across provider I/O.
    const auto scope = snapshot.read_scope();
    if (!scope.cert_record) return code_e::unauthorized;
    if (scope.cert_record != snapshot.cert_record) return code_e::certificate_changed;
    if (!scope.permission_view) return code_e::permission_required;
    if (scope.unsupported_scope) return code_e::unsupported_scope;
    if (scope.stopping) return code_e::server_stopping;
    if (!scope.sole_active_owner) return code_e::active_owner_required;
    if (scope.app_session_id != snapshot.app_session_id || scope.session_generation != snapshot.session_generation) {
      return code_e::scope_changed;
    }
    const auto finding = finding_of(scope.doctor);
    if (!finding.supported || finding.fingerprint != snapshot.fingerprint ||
        finding.source_result_id != snapshot.source_result_id) {
      return code_e::finding_changed;
    }
    if (scope.controller_revision != snapshot.controller_revision) return code_e::scope_changed;
    const auto config = _hooks.read_config();
    if (!config.enabled) return code_e::host_ai_disabled;
    if (!config.transport_ready || config.generation != snapshot.config_generation) return code_e::provider_not_ready;
    if (identities) *identities = scope.identities;
    return std::nullopt;
  }

  void service_t::finish(snapshot_t &snapshot, code_e code, const std::optional<nlohmann::json> &explanation) {
    response_identity_t identity;
    if (code != code_e::unauthorized) {
      identity = response_identity_t {
        .request_id = snapshot.request.request_id,
        .app_session_id = snapshot.request.app_session_id,
        .session_generation = snapshot.request.session_generation,
        .source_result_id = snapshot.request.source_result_id,
        .finding_fingerprint = snapshot.request.finding_fingerprint,
        .controller_revision = snapshot.request.controller_revision,
        .snapshot_id = snapshot.snapshot_id,
        .snapshot_age_ms = milliseconds_between(snapshot.created, _hooks.clock()),
      };
    }
    const auto age = identity.snapshot_age_ms.value_or(0);
    auto body = response_body(code, identity, k_snapshot_lifetime.count() - age, std::nullopt, explanation);
    if (body.size() > k_max_response_bytes) {
      // Fail closed rather than truncate a JSON object.
      code = code_e::invalid_explanation;
      body = response_body(code, identity, 0, std::nullopt, std::nullopt);
    }
    BOOST_LOG(debug) << "doctor_ai_explanation: paired request finished: "sv << code_name(code);
    snapshot.write(http_status(code), body);
  }

  void service_t::run() {
    std::unique_lock lock(_mutex);
    for (;;) {
      _changed.wait(lock, [this] { return _stop || _job; });
      if (!_job) return;  // stopping with nothing admitted
      auto snapshot = std::move(_job);
      lock.unlock();

      std::optional<code_e> result = recheck(*snapshot);
      std::optional<nlohmann::json> explanation;
      if (!result) {
        {
          std::lock_guard counting(_mutex);
          ++_counters.provider_calls;
        }
        // Exactly one provider attempt per admitted request; no retry and no other provider.
        provider_outcome_t outcome;
        try {
          outcome = _hooks.provider(provider_call_t {snapshot->config, snapshot->evidence, _cancel});
        } catch (...) {
          outcome = {};
          outcome.code = code_e::provider_failed;
        }
        // The caller, finding, controller and configuration must still be the captured ones. This one
        // validated view is the only one publication uses: no later, unchecked read can supply anything.
        std::vector<std::string> identities;
        result = recheck(*snapshot, &identities);
        if (!result) {
          if (outcome.code != code_e::explained || !outcome.explanation) {
            result = outcome.code == code_e::explained ? code_e::invalid_explanation : outcome.code;
          } else {
            identities.insert(identities.end(), snapshot->identities.begin(), snapshot->identities.end());
            explanation = sanitize_explanation(*outcome.explanation, identities);
            result = explanation ? code_e::explained : code_e::invalid_explanation;
          }
        }
      }
      finish(*snapshot, *result, explanation);
      // Evidence and text go before the slot is released.
      snapshot.reset();
      explanation.reset();

      lock.lock();
      _in_flight = false;
      ++_counters.completions;
      _changed.notify_all();
    }
  }

  void service_t::stop_accepting() {
    std::lock_guard lock(_mutex);
    _accepting = false;
    _cancel.store(true, std::memory_order_relaxed);
    _changed.notify_all();
  }

  void service_t::shutdown() {
    stop_accepting();
    {
      std::lock_guard lock(_mutex);
      _stop = true;
      _changed.notify_all();
    }
    if (_worker.joinable() && _worker.get_id() != std::this_thread::get_id()) {
      _worker.join();
    }
  }
}  // namespace doctor_ai_explanation
