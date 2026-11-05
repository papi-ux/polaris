/** @file
 * Read-only fields from the packet's immutable historical records.
 * Existing opt-in trace only; no setting, authority, clock or delivery claim.
 */
#pragma once

#include "video_bitrate_provenance.h"
#include "video_pyrowave_work.h"
#include "vrr_trace.h"

namespace video::packet_trace_fields {
  inline std::string_view receipt_origin(bitrate_provenance::receipt_origin_e origin) noexcept {
    switch (origin) {
      case bitrate_provenance::receipt_origin_e::successful_open: return "successful_open";
      case bitrate_provenance::receipt_origin_e::backend_reconfiguration: return "backend_reconfiguration";
      default: return {};
    }
  }

  inline std::string_view work_kind(pyrowave_work::kind_e kind) noexcept {
    switch (kind) {
      case pyrowave_work::kind_e::imported: return "imported";
      case pyrowave_work::kind_e::blank_primer: return "blank_primer";
      case pyrowave_work::kind_e::blank_hdr: return "blank_hdr";
      case pyrowave_work::kind_e::packed: return "packed";
      case pyrowave_work::kind_e::retained: return "retained";
      default: return {};
    }
  }

  // Every key/text value has static lifetime. Adds 19 fixed fields; the actual
  // packet_dequeue event's existing eight fields leave 13 of 40 slots unused.
  // Caller retains the existing enabled() predicate and one record() call.
  inline void append(vrr_trace::event_t &event,
                     const std::optional<const bitrate_provenance::record_t> &setting,
                     const pyrowave_work::receipt_value &work) noexcept {
    event.s("record_stage", "sender_packet_dequeue").u("frame01_available", !!setting);
    if (setting) {
      event.u("frame01_encoder_lifetime", setting->scope().encoder_lifetime)
        .u("frame01_stream_generation", setting->scope().stream_generation)
        .u("frame01_application_sequence", setting->application_sequence())
        .u("frame01_origin_revision", setting->authority_revision())
        .i("frame01_configured_encoder_kbps", setting->configured_encoder_kbps())
        .i("frame01_input_pts", setting->input_pts())
        .u("frame01_submission_sequence", setting->submission_sequence());
      const auto origin = receipt_origin(setting->origin());
      if (origin.empty()) event.n("frame01_receipt_origin");
      else event.s("frame01_receipt_origin", origin);
    } else {
      event.n("frame01_encoder_lifetime").n("frame01_stream_generation")
        .n("frame01_application_sequence").n("frame01_origin_revision")
        .n("frame01_configured_encoder_kbps").n("frame01_input_pts")
        .n("frame01_submission_sequence").n("frame01_receipt_origin");
    }
    event.u("pyrowork_available", !!work);
    if (work) {
      // The record's only authority/provenance enums are deliberately narrow.
      if (work->authority() == pyrowave_work::authority_e::unknown) event.s("pyrowork_authority", "unknown");
      else event.n("pyrowork_authority");
      if (work->provenance() == pyrowave_work::provenance_e::wrapper_call_success_nonempty) {
        event.s("pyrowork_provenance", "wrapper_call_success_nonempty");
      } else event.n("pyrowork_provenance");
      const auto kind = work_kind(work->work_kind());
      if (kind.empty()) event.n("pyrowork_kind");
      else event.s("pyrowork_kind", kind);
      event.u("pyrowork_sequence", work->local_work_sequence())
        .i("pyrowork_handoff_frame_index", work->handoff_frame_index())
        .u("pyrowork_budget_argument_bytes", work->effective_budget_argument_bytes())
        .u("pyrowork_completed_bitstream_bytes", work->completed_bitstream_bytes())
        .u("pyrowork_packet_copy_bytes", work->generic_packet_copy_bytes());
    } else {
      event.n("pyrowork_authority").n("pyrowork_provenance").n("pyrowork_kind")
        .n("pyrowork_sequence").n("pyrowork_handoff_frame_index")
        .n("pyrowork_budget_argument_bytes").n("pyrowork_completed_bitstream_bytes")
        .n("pyrowork_packet_copy_bytes");
    }
  }
}  // namespace video::packet_trace_fields
