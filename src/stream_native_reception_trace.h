/** @file src/stream_native_reception_trace.h
 * Private native admission and passive signal rows for the opt-in recorder (schema 2.5).
 *
 * Values only: no controller, status, wire or policy reads them. A row copies one 0x7F10 callback's
 * admission result and, for a newly admitted joined report of the callback's own generation, its
 * samples' retained sender API bounds.
 * Host steady nanoseconds and client connection-relative microseconds stay in separate fields and
 * are never subtracted in the original admission/sample rows. New companion rows copy the pure
 * adapter's signed elapsed outcome and coherent diagnostic context, never a policy decision.
 * No address, session token, client identity, name, certificate, key or packet buffer enters a row.
 * Every key and text value is a static literal.
 */
#pragma once

#include "stream_native_reception.h"
#include "vrr_trace.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace stream_native_reception::trace {
  inline constexpr const char *admission_kind = "native_admission";
  inline constexpr const char *sample_kind = "native_admission_sample";
  inline constexpr std::size_t admission_fields = 34, sample_fields = 12;
  inline constexpr const char *signal_kind = "native_signal_context", *signal_sample_kind = "native_signal_sample";
  inline constexpr std::size_t signal_fields = 40, signal_sample_fields = 12;
  static_assert(signal_fields <= vrr_trace::k_fields && signal_sample_fields <= vrr_trace::k_fields);

  /// Facts the control callback itself owns, read in that callback.
  struct callback_t {
    bool dispatched = false;  ///< The existing RUNNING/peer/scope/negotiation guard passed.
    std::uint64_t generation = 0;  ///< The session's generation.
    std::uint64_t trace_scope = 0;  ///< Opaque vrr_trace id for this native scope; 0 when unknown.
    std::uint64_t callback = 0;  ///< Opaque vrr_trace id for this callback; 0 when unknown.
    std::optional<std::uint32_t> enet_smoothed_rtt_ms;  ///< ENet peer roundTripTime in this callback.
  };

  inline const char *loop_name(loop_e loop) noexcept {
    return loop == loop_e::advancing ? "advancing" : loop == loop_e::unchanged ? "unchanged" : "baseline";
  }

  /// Report facts belong to this callback only for a dispatched, newly admitted report of a nonzero
  /// scope generation equal to the callback's own. Anything else never relabels retained facts.
  inline bool attributed(const admission_trace_t &a, const callback_t &c) noexcept {
    return c.dispatched && a.admitted && a.report.sequence != 0 && a.generation != 0 && a.generation == c.generation;
  }
  inline const char *attribution_name(const admission_trace_t &a, const callback_t &c) noexcept {
    if (!c.dispatched || !a.admitted) return "none";
    return attributed(a, c) ? "admitted_payload" : "generation_mismatch";
  }

  /// Sample rows follow only an attributed report whose availability joined.
  inline std::size_t sample_rows(const admission_trace_t &a, const callback_t &c) noexcept {
    if (!attributed(a, c) || a.availability != reason_e::joined || !a.setting) return 0;
    return a.sample_count < max_samples ? a.sample_count : max_samples;
  }

  inline vrr_trace::event_t admission_event(const admission_trace_t &a, const callback_t &c) noexcept {
    vrr_trace::event_t e(admission_kind);
    const auto id = [&e](const char *key, std::uint64_t value) { if (value) e.u(key, value); else e.n(key); };
    const bool report = attributed(a, c);
    const auto u = [&e, report](const char *key, std::uint64_t value) { if (report) e.u(key, value); else e.n(key); };
    id("trace_scope", c.trace_scope);
    e.u("session_generation", c.generation);
    id("scope_generation", c.dispatched ? a.generation : 0);
    id("callback", c.callback);
    // Refusal is the host's own admission decision: never a wire-loss or delivery statement.
    e.s("admission", c.dispatched ? name(a.admission) : "not_dispatched");
    e.u("admitted", c.dispatched && a.admitted);
    e.s("report_attribution", attribution_name(a, c));
    e.i("received_steady_ns", a.received_steady_ns);
    if (c.enet_smoothed_rtt_ms) e.u("enet_smoothed_rtt_ms", *c.enet_smoothed_rtt_ms);
    else e.n("enet_smoothed_rtt_ms");
    u("report_sequence", a.report.sequence);
    u("report_end_us", a.report.end_us);
    u("report_interval_us", a.report.interval_us);
    u("report_flags", a.report.flags);
    u("report_receive_loop_tick", a.report.loop_tick);
    if (report) e.s("receive_loop", loop_name(a.receive_loop)); else e.n("receive_loop");
    u("missing_reports", a.missing_reports);
    u("report_data_packets", a.report.data_packets);
    u("report_parity_packets", a.report.parity_packets);
    u("report_data_udp_bytes", a.report.data_bytes);
    u("report_parity_udp_bytes", a.report.parity_bytes);
    u("report_last_accepted_age_us", a.report.last_accepted_age_us);
    u("report_first_frame", a.report.first_frame);
    u("report_last_frame", a.report.last_frame);
    u("report_sample_count", a.report.count);
    if (report) e.s("availability", name(a.availability)).s("frame_reason", a.frame_reason);
    else e.n("availability").n("frame_reason");
    const auto rows = sample_rows(a, c);
    if (rows) {
      const auto &s = *a.setting;
      e.u("encoder_lifetime", s.encoder_lifetime).u("encoder_stream_generation", s.stream_generation)
        .u("encoder_application_sequence", s.application_sequence).u("encoder_origin_revision", s.origin_revision)
        .i("configured_encoder_kbps", s.configured_encoder_kbps)
        .s("encoder_receipt_origin", stream_frame_feedback::receipt_origin_name(s.receipt_origin));
    } else {
      e.n("encoder_lifetime").n("encoder_stream_generation").n("encoder_application_sequence")
        .n("encoder_origin_revision").n("configured_encoder_kbps").n("encoder_receipt_origin");
    }
    e.u("sample_rows", rows);
    e.s("clock_domains", "host_steady_ns;client_connection_us");
    return e;
  }

  inline vrr_trace::event_t sample_event(const admission_trace_t &a, std::size_t index, const callback_t &c) noexcept {
    vrr_trace::event_t e(sample_kind);
    const auto &s = a.samples[index < max_samples ? index : max_samples - 1];
    if (c.trace_scope) e.u("trace_scope", c.trace_scope); else e.n("trace_scope");
    e.u("session_generation", c.generation);
    if (c.callback) e.u("callback", c.callback); else e.n("callback");
    e.u("report_sequence", a.report.sequence).u("sample_index", index).u("frame_index", s.frame)
      .u("rtp_sequence", s.rtp_sequence).u("socket_read_age_us", s.socket_read_age_us)
      .i("sender_api_begin_ns", s.sender_api_begin_ns).i("sender_api_end_ns", s.sender_api_end_ns)
      .s("packet_kind", s.parity ? "parity" : "data").u("udp_bytes", s.udp_bytes);
    return e;
  }

  /// One header row, then at most sixteen sample rows of this same admission. Returns the row count.
  template<class Sink>
  std::size_t emit(const admission_trace_t &a, const callback_t &c, Sink &&sink) noexcept {
    sink(admission_event(a, c));
    const auto rows = sample_rows(a, c);
    for (std::size_t i = 0; i < rows; ++i) sink(sample_event(a, i, c));
    return 1 + rows;
  }

  inline vrr_trace::event_t signal_event(const native_video_signal::signal_t &s, bool published,
                                        const callback_t &c) noexcept {
    vrr_trace::event_t e(signal_kind);
    const bool own = c.dispatched && s.admitted && s.sequence && s.generation && s.generation == c.generation;
    native_video_signal::signal_t empty;
    empty.received_steady_ns = s.received_steady_ns;
    const auto &value = own ? s : empty;
    if (c.trace_scope) e.u("trace_scope", c.trace_scope); else e.n("trace_scope");
    e.u("session_generation", c.generation);
    if (c.callback) e.u("callback", c.callback); else e.n("callback");
    if (own) e.u("report_sequence", value.sequence); else e.n("report_sequence");
    e.i("received_steady_ns", value.received_steady_ns).s("signal_reason", native_video_signal::name(value.reason))
      .u("measurement_available", value.measurement_available).u("inspection_ready", value.inspection_ready)
      .u("sparse", value.sparse).u("running", value.caller.running).u("watch_or_input_only", value.caller.watch_or_input_only)
      .u("negotiated", value.caller.negotiated).u("control_authenticated", value.caller.control_authenticated)
      .u("media_authenticated", value.caller.media_authenticated);
    const auto &x = value.context;
    e.s("context_reason", native_video_signal::name(x.reason)).u("context_observed", x.observed)
      .i("inspection_steady_ns", x.inspected_steady_ns).u("generation_matches", x.generation_matches)
      .u("app_matches", x.app_matches).u("exclusive", x.exclusive).u("configured_enabled", x.configured_enabled)
      .u("runtime_enabled", x.enabled).u("paused", x.paused).u("runtime_supported", x.runtime_supported)
      .u("in_place", x.in_place).u("doctor_override", x.doctor_override).u("explicit_writer", x.explicit_writer)
      .u("pending_update", x.pending_update).u("applied_known", x.applied_known)
      .i("base_kbps", x.base_kbps).i("requested_kbps", x.requested_kbps).i("floor_kbps", x.floor_kbps)
      .i("applied_kbps", x.applied_kbps).u("operator_revision", x.operator_revision)
      .u("action_revision", x.action_revision).u("applied_revision", x.applied_revision)
      .o("applied_age_ns", x.applied_age_ns).s("native_availability", value.native_availability)
      .u("sample_count", value.sample_count).u("published", own && published);
    return e;
  }
  inline vrr_trace::event_t signal_sample_event(const native_video_signal::signal_t &s, std::size_t index,
                                               const callback_t &c) noexcept {
    vrr_trace::event_t e(signal_sample_kind);
    const auto &p = s.samples[index < native_video_signal::max_samples ? index : native_video_signal::max_samples - 1];
    if (c.trace_scope) e.u("trace_scope", c.trace_scope); else e.n("trace_scope");
    e.u("session_generation", c.generation);
    if (c.callback) e.u("callback", c.callback); else e.n("callback");
    e.u("report_sequence", s.sequence).u("sample_index", index).u("frame_index", p.frame)
      .u("rtp_sequence", p.rtp_sequence).s("packet_kind", p.parity ? "parity" : "data")
      .u("udp_bytes", p.udp_bytes).s("elapsed_reason", native_video_signal::name(p.reason))
      .o("elapsed_lower_ns", p.elapsed_lower_ns).o("elapsed_upper_ns", p.elapsed_upper_ns);
    return e;
  }
  template<class Sink>
  std::size_t emit_signal(const native_video_signal::signal_t &s, bool published, const callback_t &c, Sink &&sink) noexcept {
    sink(signal_event(s, published, c));
    const auto count = c.dispatched && s.admitted && s.generation && s.generation == c.generation && s.setting ?
      std::min<std::size_t>(s.sample_count, native_video_signal::max_samples) : 0;
    for (std::size_t i = 0; i < count; ++i) sink(signal_sample_event(s, i, c));
    return 1 + count;
  }
}  // namespace stream_native_reception::trace
