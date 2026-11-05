/**
 * @file src/stream_applied_pacing.h
 * @brief Bound packet pacing by a frame's own encoder setting and the current sole owner's cap.
 */
#pragma once

#include "adaptive_bitrate.h"
#include "stream_pacing.h"
#include "video_bitrate_provenance.h"

namespace stream_applied_pacing {
  enum class source_e { off, frame_unavailable, owner_unavailable, frame_mismatch, split_unavailable, cost_unavailable, applied };

  struct selection_t {
    stream_pacing::rate_t rate;
    source_e source;
    bool applied() const noexcept { return source == source_e::applied; }
    // A legacy start refused only because no owner cap was readable (busy
    // try_lock or no current owner) may later tighten under a coherent cap.
    // Every other refusal keeps the legacy rate for the whole frame.
    bool refreshable() const noexcept { return applied() || source == source_e::owner_unavailable; }
  };

  // Invert an encoder allocation in its own nominal FEC domain. The current
  // policy uses F0; a frame uses the percentage its allocation was published
  // at, before the packetizer's envelope fit or oversized zero-parity fallback.
  // Audio/control keep the handshake deductions in that domain, and the
  // total remains bounded by split_kbps (already warped and launch-capped).
  inline std::optional<std::int64_t> packet_budget_kbps(const stream_bitrate::request_t &request, int video_kbps,
                                                      std::optional<int> frame_fec_percentage = std::nullopt) {
    const int percentage = frame_fec_percentage.value_or(request.fec_percentage);
    if (request.split_kbps <= 0 || request.split_kbps > stream_pacing::k_max_negotiated_kbps ||
        request.encoder_kbps <= 0 || request.audio_kbps < 0 ||
        request.fec_percentage < 0 || request.fec_percentage > stream_bitrate::k_max_adjusted_fec_percentage ||
        percentage < 0 || percentage > stream_bitrate::k_max_adjusted_fec_percentage ||
        request.warp_factor <= 0 || video_kbps <= 0 ||
        stream_bitrate::encoder_kbps_for_wire(request.split_kbps, request.fec_percentage, request.audio_kbps) != request.encoder_kbps) {
      return std::nullopt;
    }
    const auto total = std::min(request.split_kbps, stream_bitrate::wire_kbps_for_encoder(
      std::min(video_kbps, request.encoder_kbps), percentage, request.audio_kbps));
    std::int64_t after_fec = total;
    after_fec /= 100.f / (100 - percentage);
    const auto reserved_audio_control = after_fec - stream_bitrate::encoder_kbps_for_wire(
      total, percentage, request.audio_kbps);
    const auto packets = total - reserved_audio_control;
    return packets > 0 ? std::optional {packets} : std::nullopt;
  }

  // block/prefix sizes come from the actual constructed FEC object, including
  // padded data and actual parity packets. Every constructed shard consumes
  // one full datagram charge through send_offset(); no frame-size denominator
  // or nominal parity multiplier can increase this admitted rate. IP/UDP/link
  // headers and possible duplicate fallback sends remain outside this ledger.
  inline selection_t select_rate(bool enabled, const stream_bitrate::request_t &request, int headroom,
                                 std::size_t blocksize, std::size_t prefixsize,
                                 std::uint64_t generation, std::int64_t frame_index,
                                 const video::bitrate_provenance::record_t *frame,
                                 const std::optional<adaptive_bitrate::live_pacing_cap_t> &cap,
                                 std::optional<int> frame_fec_percentage = std::nullopt) {
    const auto legacy = stream_pacing::select_rate(enabled, request.split_kbps, headroom, blocksize);
    const auto refuse = [&](source_e reason) { return selection_t {legacy, reason}; };
    if (!enabled) return refuse(source_e::off);
    if (!frame) return refuse(source_e::frame_unavailable);
    if (!cap) return refuse(source_e::owner_unavailable);
    if (!generation || frame->scope().stream_generation != generation || cap->generation != generation ||
        !frame->scope().encoder_lifetime || frame->input_pts() != frame_index ||
        !frame->submission_sequence() || !frame->application_sequence() || !frame->authority_revision() ||
        frame->authority_revision() > cap->revision || frame->configured_encoder_kbps() <= 0 ||
        cap->target_encoder_kbps <= 0 ||
        (frame->authority_revision() == cap->revision && frame->configured_encoder_kbps() != cap->target_encoder_kbps)) {
      return refuse(source_e::frame_mismatch);
    }
    const auto frame_budget = packet_budget_kbps(request, frame->configured_encoder_kbps(), frame_fec_percentage);
    // The current lower policy ceiling is independent of an old frame's repair
    // percentage. Keep the actual target above only for exact revision identity.
    const auto current_cap = packet_budget_kbps(request, cap->policy_encoder_kbps);
    if (!frame_budget || !current_cap) return refuse(source_e::split_unavailable);
    // The sender retains its existing nonzero block/legacy-divisor precondition.
    // Check the constructed UDP payload size before adding the prefix.
    if (blocksize > 65535 || prefixsize > 65535 - blocksize ||
        headroom <= 0 || headroom > stream_pacing::k_max_headroom_percent) {
      return refuse(source_e::cost_unavailable);
    }
    // Keep both wire-domain bounds explicit, including the inverse's integer
    // rounding, rather than relying on an idealized proportional conversion.
    const auto budget = std::min(*frame_budget, *current_cap);
    if (static_cast<std::uint64_t>(budget) * 10 * headroom < (blocksize + prefixsize) * 8) {
      // The existing pacer cannot represent less than one packet/second.
      // Keep that path explicitly unsupported instead of claiming admission.
      return refuse(source_e::cost_unavailable);
    }
    return {stream_pacing::select_rate(true, budget, headroom, blocksize + prefixsize), source_e::applied};
  }

  // The active frame may only slow down. This also preserves its accrued debt
  // when a newer UP or proof loss arrives after a lower cap. Products fit in
  // uint64: selected rates are bounded by the existing legacy rate and UDP size.
  inline void tighten(stream_pacing::rate_t &current, const selection_t &candidate) {
    if (!candidate.applied()) return;
    const auto numerator = [](const auto &rate) { return rate.session_rate ? rate.bits_per_packet : std::uint64_t {1}; };
    const auto denominator = [](const auto &rate) { return rate.session_rate ? rate.bits_per_second : rate.packets_per_second; };
    if (numerator(candidate.rate) * denominator(current) > numerator(current) * denominator(candidate.rate)) {
      current = candidate.rate;
    }
  }
}  // namespace stream_applied_pacing
