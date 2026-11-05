/** @file Passive, rate-local native signal values. No policy, target, estimator or capability. */
#pragma once

#include "stream_frame_feedback.h"
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace native_video_signal {
  inline constexpr std::size_t max_samples = 16;

  enum class context_reason_e {
    busy, scope_mismatch, shared_scope, disabled, runtime_unavailable, not_in_place,
    doctor_override, explicit_writer, pending_update, applied_unknown, rate_mismatch,
    at_floor, ready,
  };
  inline const char *name(context_reason_e reason) noexcept {
    switch (reason) {
#define NATIVE_CONTEXT_REASON(x) case context_reason_e::x: return #x
      NATIVE_CONTEXT_REASON(busy); NATIVE_CONTEXT_REASON(scope_mismatch); NATIVE_CONTEXT_REASON(shared_scope);
      NATIVE_CONTEXT_REASON(disabled); NATIVE_CONTEXT_REASON(runtime_unavailable); NATIVE_CONTEXT_REASON(not_in_place);
      NATIVE_CONTEXT_REASON(doctor_override); NATIVE_CONTEXT_REASON(explicit_writer); NATIVE_CONTEXT_REASON(pending_update);
      NATIVE_CONTEXT_REASON(applied_unknown); NATIVE_CONTEXT_REASON(rate_mismatch); NATIVE_CONTEXT_REASON(at_floor);
      NATIVE_CONTEXT_REASON(ready);
#undef NATIVE_CONTEXT_REASON
    }
    return "unavailable";
  }

  /// One coherent, read-only inspection. No token, address, or reusable action authority.
  struct context_t {
    context_reason_e reason = context_reason_e::busy;
    bool observed = false, generation_matches = false, app_matches = false, exclusive = false;
    bool configured_enabled = false, enabled = false, paused = false, runtime_supported = false, in_place = false;
    bool doctor_override = false, explicit_writer = false, pending_update = false, applied_known = false;
    int base_kbps = 0, requested_kbps = 0, floor_kbps = 0, applied_kbps = 0;
    std::uint64_t operator_revision = 0, action_revision = 0, applied_revision = 0;
    std::int64_t inspected_steady_ns = -1;
    std::optional<std::int64_t> applied_age_ns;
  };
  struct caller_t {
    std::uint64_t generation = 0;
    bool running = false, watch_or_input_only = false, negotiated = false;
    bool control_authenticated = false, media_authenticated = false;
  };
  enum class reason_e {
    not_admitted, generation_mismatch, native_unavailable, invalid_samples, invalid_time,
    negative_interval, caller_unready, context_unready, source_rate_mismatch, scope_changed,
    report_stale, sender_stale, frame_unavailable, ready,
  };
  inline const char *name(reason_e reason) noexcept {
    switch (reason) {
#define NATIVE_SIGNAL_REASON(x) case reason_e::x: return #x
      NATIVE_SIGNAL_REASON(not_admitted); NATIVE_SIGNAL_REASON(generation_mismatch);
      NATIVE_SIGNAL_REASON(native_unavailable); NATIVE_SIGNAL_REASON(invalid_samples);
      NATIVE_SIGNAL_REASON(invalid_time); NATIVE_SIGNAL_REASON(negative_interval);
      NATIVE_SIGNAL_REASON(caller_unready); NATIVE_SIGNAL_REASON(context_unready);
      NATIVE_SIGNAL_REASON(source_rate_mismatch); NATIVE_SIGNAL_REASON(ready);
      NATIVE_SIGNAL_REASON(scope_changed);
      NATIVE_SIGNAL_REASON(report_stale); NATIVE_SIGNAL_REASON(sender_stale); NATIVE_SIGNAL_REASON(frame_unavailable);
#undef NATIVE_SIGNAL_REASON
    }
    return "unavailable";
  }
  struct sample_t {
    std::uint32_t frame = 0, udp_bytes = 0;
    std::uint16_t rtp_sequence = 0;
    bool parity = false;
    reason_e reason = reason_e::native_unavailable;
    // A negative interval stays signed and unavailable; absence means no valid arithmetic.
    std::optional<std::int64_t> elapsed_lower_ns, elapsed_upper_ns;
  };
  struct signal_t {
    reason_e reason = reason_e::not_admitted;
    bool admitted = false, measurement_available = false, inspection_ready = false, sparse = false;
    std::uint64_t generation = 0;
    std::uint32_t sequence = 0, interval_us = 0, data_packets = 0, parity_packets = 0;
    std::uint32_t data_bytes = 0, parity_bytes = 0, missing_reports = 0;
    std::int64_t received_steady_ns = -1;
    const char *native_availability = "unavailable";
    caller_t caller;
    context_t context;
    std::optional<stream_frame_feedback::encoder_setting_t> setting;
    std::uint8_t sample_count = 0;
    std::array<sample_t, max_samples> samples {};
  };

  /// Accept's value copy is the only input: never poll a retained last-good report or absolute client time.
  /// The dependent admission type keeps this pure adapter independent of its native scope's locking.
  template<class Admission>
  signal_t project(const Admission &a, caller_t caller, context_t context) noexcept {
    signal_t out;
    out.generation = a.generation; out.received_steady_ns = a.received_steady_ns;
    out.caller = caller;
    if (!a.admitted) return out;
    if (!a.generation || a.generation != caller.generation || !a.report.sequence ||
        (a.setting && a.setting->stream_generation != a.generation)) {
      out.reason = reason_e::generation_mismatch; return out;
    }
    out.admitted = true;
    out.context = context;
    out.sequence = a.report.sequence; out.interval_us = a.report.interval_us;
    out.data_packets = a.report.data_packets; out.parity_packets = a.report.parity_packets;
    out.data_bytes = a.report.data_bytes; out.parity_bytes = a.report.parity_bytes;
    out.missing_reports = a.missing_reports;
    out.sparse = (a.report.flags & 8) != 0; // Existing TIMING_SAMPLE_LIMIT; not incomplete or clean.
    out.native_availability = name(a.availability); // ADL: original native availability, not admission.
    if (a.availability != decltype(a.availability)::joined || !a.setting) {
      out.reason = reason_e::native_unavailable; return out;
    }
    if (!a.sample_count || a.sample_count > max_samples || a.sample_count != a.report.count) {
      out.reason = reason_e::invalid_samples; return out;
    }
    out.setting = a.setting; out.sample_count = a.sample_count;
    out.measurement_available = true;
    out.reason = reason_e::ready;
    for (std::size_t i = 0; i < out.sample_count; ++i) {
      const auto &p = a.samples[i]; auto &s = out.samples[i];
      s.frame = p.frame; s.rtp_sequence = p.rtp_sequence; s.parity = p.parity; s.udp_bytes = p.udp_bytes;
      if (p.sender_api_begin_ns < 0 || p.sender_api_end_ns < p.sender_api_begin_ns ||
          a.received_steady_ns < p.sender_api_end_ns) {
        s.reason = reason_e::invalid_time; out.reason = reason_e::invalid_time; out.measurement_available = false;
        continue;
      }
      const auto age_ns = static_cast<std::int64_t>(p.socket_read_age_us) * 1000;
      s.elapsed_lower_ns = (a.received_steady_ns - p.sender_api_end_ns) - age_ns;
      s.elapsed_upper_ns = (a.received_steady_ns - p.sender_api_begin_ns) - age_ns;
      s.reason = *s.elapsed_lower_ns < 0 ? reason_e::negative_interval : reason_e::ready;
      if (s.reason != reason_e::ready) {
        out.measurement_available = false;
        if (out.reason != reason_e::invalid_time) out.reason = s.reason;
      }
    }
    if (!out.measurement_available) return out;
    if (!caller.running || caller.watch_or_input_only || !caller.negotiated || !caller.control_authenticated) {
      out.reason = reason_e::caller_unready; return out;
    }
    if (!context.observed || context.reason != context_reason_e::ready ||
        context.inspected_steady_ns < a.received_steady_ns) {
      out.reason = reason_e::context_unready; return out;
    }
    const auto retention_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Admission::freshness_bound).count();
    if (context.inspected_steady_ns - a.received_steady_ns > retention_ns) {
      out.reason = reason_e::report_stale; return out;
    }
    for (std::size_t i = 0; i < out.sample_count; ++i) {
      if (context.inspected_steady_ns - a.samples[i].sender_api_begin_ns > retention_ns) {
        out.reason = reason_e::sender_stale; return out;
      }
    }
    // A receipt's historical origin revision is NOT the controller's newest evidence revision.
    if (a.setting->configured_encoder_kbps != context.applied_kbps) {
      out.reason = reason_e::source_rate_mismatch; return out;
    }
    out.inspection_ready = true; // Observation only. A future writer must serialize and recheck itself.
    return out;
  }
} // namespace native_video_signal
