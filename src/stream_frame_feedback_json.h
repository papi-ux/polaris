/**
 * @file src/stream_frame_feedback_json.h
 * @brief Wire parser and receipt serializer for the optional frame-submission section (#611).
 *
 * The section is sample.frame_submission on the existing paired session telemetry request. It is
 * parsed strictly and independently of the legacy counters: a malformed section only makes the new
 * evidence unavailable; it never changes how the legacy counters are accepted or answered.
 */
#pragma once

#include "stream_frame_feedback.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace stream_frame_feedback {
  /// Advertised in the host's features; a strict positive boolean means the host parses the section.
  inline constexpr const char *k_capability = "live_media_frame_submission_v1";
  inline constexpr const char *k_section_key = "frame_submission";
  inline constexpr const char *k_receipt_key = "frame_submission_receipt";

  namespace detail {
    inline constexpr std::uint64_t k_int64_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    inline constexpr std::uint64_t k_uint32_max = std::numeric_limits<std::uint32_t>::max();

    /// An exact JSON integer in [1, max]. Floats, booleans, strings and negatives are not.
    inline std::optional<std::uint64_t> positive_integer(const nlohmann::json &value, std::uint64_t max) {
      if (!value.is_number_integer()) return std::nullopt;
      std::uint64_t result = 0;
      if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
      } else {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 1) return std::nullopt;
        result = static_cast<std::uint64_t>(signed_value);
      }
      if (result < 1 || result > max) return std::nullopt;
      return result;
    }

    inline bool valid_nonce(const nlohmann::json &value) {
      return value.is_string() && stream_frame_feedback::valid_nonce(value.get_ref<const std::string &>());
    }
  }  // namespace detail

  /// Parse sample.frame_submission. Never throws; anything unexpected is malformed.
  inline section_t parse_section(const nlohmann::json &sample) noexcept {
    section_t out;
    const auto refuse = [&out](section_t::state_e state, const char *reason) {
      out.state = state;
      out.reason = reason;
      out.snapshot = {};
      return out;
    };
    try {
      if (!sample.is_object()) return out;
      const auto found = sample.find(k_section_key);
      if (found == sample.end()) return out;
      const auto &section = *found;
      if (!section.is_object()) return refuse(section_t::state_e::malformed, "malformed_section");

      const auto version = section.find("version");
      if (version == section.end() || !version->is_number_integer()) {
        return refuse(section_t::state_e::malformed, "malformed_version");
      }
      const bool version_one = version->is_number_unsigned() ? version->get<std::uint64_t>() == 1 :
                                                               version->get<std::int64_t>() == 1;
      if (!version_one) return refuse(section_t::state_e::unsupported_version, "unsupported_version");

      static constexpr const char *fields[] = {
        "version", "launch_nonce", "connection_nonce", "native_epoch", "snapshot_sequence", "last_frame_index",
        "decode_units_submitted_total", "payload_bytes_total", "monotonic_timestamp_ms",
        "last_submission_timestamp_ms",
      };
      for (const auto &item : section.items()) {
        bool known = false;
        for (const char *field : fields) known = known || item.key() == field;
        if (!known) return refuse(section_t::state_e::malformed, "malformed_unexpected_field");
      }

      const auto launch_nonce = section.find("launch_nonce");
      if (launch_nonce == section.end() || !detail::valid_nonce(*launch_nonce)) {
        return refuse(section_t::state_e::malformed, "malformed_launch_nonce");
      }
      out.snapshot.launch_nonce = launch_nonce->get<std::string>();
      const auto nonce = section.find("connection_nonce");
      if (nonce == section.end() || !detail::valid_nonce(*nonce)) {
        return refuse(section_t::state_e::malformed, "malformed_connection_nonce");
      }
      out.snapshot.connection_nonce = nonce->get<std::string>();

      struct integer_field_t {
        const char *name;
        const char *reason;
        std::uint64_t max;
      };
      static constexpr integer_field_t integers[] = {
        {"native_epoch", "malformed_native_epoch", detail::k_int64_max},
        {"snapshot_sequence", "malformed_snapshot_sequence", detail::k_int64_max},
        {"last_frame_index", "malformed_last_frame_index", detail::k_uint32_max},
        {"decode_units_submitted_total", "malformed_decode_units_submitted_total", detail::k_int64_max},
        {"payload_bytes_total", "malformed_payload_bytes_total", detail::k_int64_max},
        {"monotonic_timestamp_ms", "malformed_monotonic_timestamp_ms", detail::k_int64_max},
        {"last_submission_timestamp_ms", "malformed_last_submission_timestamp_ms", detail::k_int64_max},
      };
      std::uint64_t values[std::size(integers)] {};
      for (std::size_t index = 0; index < std::size(integers); ++index) {
        const auto value = section.find(integers[index].name);
        const auto parsed = value == section.end() ? std::nullopt : detail::positive_integer(*value, integers[index].max);
        if (!parsed) return refuse(section_t::state_e::malformed, integers[index].reason);
        values[index] = *parsed;
      }
      out.snapshot.native_epoch = values[0];
      out.snapshot.snapshot_sequence = values[1];
      out.snapshot.last_frame_index = static_cast<std::uint32_t>(values[2]);
      out.snapshot.decode_units_submitted_total = values[3];
      out.snapshot.payload_bytes_total = values[4];
      out.snapshot.monotonic_timestamp_ms = values[5];
      out.snapshot.last_submission_timestamp_ms = values[6];
      if (out.snapshot.last_submission_timestamp_ms > out.snapshot.monotonic_timestamp_ms) {
        return refuse(section_t::state_e::malformed, "malformed_last_submission_timestamp_ms");
      }
      // Absolute coherence, before any baseline: each counted unit has its own distinct positive frame
      // number and a positive payload, so neither total can exceed what the cursor and bytes allow.
      if (out.snapshot.decode_units_submitted_total > out.snapshot.last_frame_index ||
          out.snapshot.payload_bytes_total < out.snapshot.decode_units_submitted_total) {
        return refuse(section_t::state_e::malformed, "malformed_totals_incoherent");
      }
      out.state = section_t::state_e::valid;
      out.reason = "valid";
      return out;
    } catch (...) {
      return refuse(section_t::state_e::malformed, "malformed_section");
    }
  }

  /// The versioned receipt served in the telemetry response. No scope token is ever included.
  inline nlohmann::json receipt_json(const receipt_t &receipt) {
    nlohmann::json out {
      {"version", k_version},
      {"available", receipt.available},
      {"reason", receipt.reason},
    };
    if (receipt.snapshot_echo) {
      out["native_epoch"] = receipt.native_epoch;
      out["snapshot_sequence"] = receipt.snapshot_sequence;
    }
    if (!receipt.available) return out;
    out["from_frame_exclusive"] = receipt.from_frame_exclusive;
    out["to_frame_inclusive"] = receipt.to_frame_inclusive;
    out["frames_emitted"] = receipt.frames_emitted;
    out["decode_units_submitted"] = receipt.decode_units_submitted;
    out["frames_absent_at_submission"] = receipt.frames_absent_at_submission;
    out["payload_bytes"] = receipt.payload_bytes;
    out["client_interval_ms"] = receipt.client_interval_ms;
    const auto &setting = receipt.encoder_setting;
    out["encoder_setting"] = {
      {"encoder_lifetime", setting.encoder_lifetime},
      {"application_sequence", setting.application_sequence},
      {"origin_revision", setting.origin_revision},
      {"configured_encoder_kbps", setting.configured_encoder_kbps},
      {"receipt_origin", receipt_origin_name(setting.receipt_origin)},
    };
    return out;
  }

  /// The as-of diagnostic readback of a scope's latest receipt: the same receipt plus its host age and,
  /// when unavailable because of one frame, that frame's index.
  inline nlohmann::json readback_json(const receipt_t &receipt, clock::time_point now) {
    auto out = receipt_json(receipt);
    out["host_receipt_age_ms"] =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - receipt.host_received_at).count();
    if (!receipt.available && receipt.unavailable_frame_index) {
      out["unavailable_frame_index"] = *receipt.unavailable_frame_index;
    }
    return out;
  }
}  // namespace stream_frame_feedback
