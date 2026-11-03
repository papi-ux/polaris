/** @file Optional own-current paired native reception status. No identities or raw host clocks. */
#pragma once
#include "stream_native_reception.h"
#include "generated/polaris_v1_contract.h"
#include <nlohmann/json.hpp>

namespace stream_native_reception {
  namespace v1 = polaris::contract::v1;
  inline nlohmann::json report_json(const report_t &r) {
    nlohmann::json output {
      {v1::native_reception_report::sequence, r.sequence}, {v1::native_reception_report::flags, r.flags}, {v1::native_reception_report::interval_us, r.interval_us},
      {v1::native_reception_report::receive_loop_tick, r.loop_tick}, {v1::native_reception_report::accepted_data_packets, r.data_packets},
      {v1::native_reception_report::accepted_parity_packets, r.parity_packets}, {v1::native_reception_report::accepted_data_udp_bytes, r.data_bytes},
      {v1::native_reception_report::accepted_parity_udp_bytes, r.parity_bytes}, {v1::native_reception_report::last_accepted_age_us, r.last_accepted_age_us},
      {v1::native_reception_report::first_frame, r.first_frame}, {v1::native_reception_report::last_frame, r.last_frame},
    };
    return output;
  }
  inline nlohmann::json setting_json(const stream_frame_feedback::encoder_setting_t &s) {
    nlohmann::json output {
      {v1::native_reception_setting::encoder_lifetime, s.encoder_lifetime}, {v1::native_reception_setting::stream_generation, s.stream_generation},
      {v1::native_reception_setting::application_sequence, s.application_sequence}, {v1::native_reception_setting::origin_revision, s.origin_revision},
      {v1::native_reception_setting::configured_encoder_kbps, s.configured_encoder_kbps},
      {v1::native_reception_setting::receipt_origin, stream_frame_feedback::receipt_origin_name(s.receipt_origin)},
    };
    return output;
  }
  inline nlohmann::json sample_json(const sample_t &wire, const correlated_sample_t &s) {
    nlohmann::json output {
      {v1::native_reception_sample::frame_index, wire.frame}, {v1::native_reception_sample::rtp_sequence, wire.sequence}, {v1::native_reception_sample::socket_read_age_us, wire.age_us},
    };
    if (s.matched) {
      output[v1::native_reception_sample::packet_kind] = s.parity ? "parity" : "data";
      output[v1::native_reception_sample::udp_bytes] = s.udp_bytes;
      output[v1::native_reception_sample::handoff_duration_ns] = s.handoff_duration_ns;
      if (s.dispersion_available) {
        output[v1::native_reception_sample::dispersion_lower_ns] = s.dispersion_lower_ns;
        output[v1::native_reception_sample::dispersion_upper_ns] = s.dispersion_upper_ns;
      }
    }
    return output;
  }
  inline nlohmann::json readback_json(const readback_t &s) {
    nlohmann::json output {
      {v1::native_reception::version, 1}, {v1::native_reception::negotiated, s.negotiated}, {v1::native_reception::available, s.available}, {v1::native_reception::reason, name(s.reason)},
      {v1::native_reception::media_authenticated, s.media_authenticated},
      {v1::native_reception::control_authenticated, s.control_authenticated},
      {v1::native_reception::coverage, "queue_accepted_original"},
    };
    if (!s.has_report) return output;
    output[v1::native_reception::host_report_age_ms] = s.host_report_age_ms;
    output[v1::native_reception::missing_reports] = s.missing_reports;
    output[v1::native_reception::receive_loop] = s.receive_loop == loop_e::advancing ? "advancing" :
      s.receive_loop == loop_e::unchanged ? "unchanged" : "baseline";
    output[v1::native_reception::frame_reason] = s.frame_reason;
    output[v1::native_reception::report] = report_json(s.report);
    if (s.available && s.setting) output[v1::native_reception::setting] = setting_json(*s.setting);
    auto samples = nlohmann::json::array();
    for (std::size_t i = 0; i < s.report.count; ++i) samples.push_back(sample_json(s.report.samples[i], s.samples[i]));
    output[v1::native_reception::samples] = std::move(samples);
    return output;
  }
}  // namespace stream_native_reception
