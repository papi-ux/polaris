/** @file src/vrr_pacing_receipt.h
 * Value-only encode-loop observations. These never carry live capture authority.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace vrr_pacing {
  enum class mode_e { waiting, source_time, latched_fallback, unsupported };
  enum class event_e {
    accepted_source, readiness, idr_recovery, liveness,
    routine_repeat_suppressed, rate_ceiling, receipt_not_mapped, authority_lost,
    stale_or_replayed, regressed_vs_emitted, tick_collision, source_stale,
    unsupported_route, unsupported_codec, unsupported_loop,
  };

  inline constexpr std::size_t k_event_count = static_cast<std::size_t>(event_e::unsupported_loop) + 1;

  inline constexpr std::string_view name(mode_e value) {
    switch (value) {
      case mode_e::waiting: return "waiting";
      case mode_e::source_time: return "source_time";
      case mode_e::latched_fallback: return "latched_fallback";
      case mode_e::unsupported: return "unsupported";
    }
    return "invalid_mode";
  }

  inline constexpr std::string_view name(event_e value) {
    switch (value) {
      case event_e::accepted_source: return "accepted_source";
      case event_e::readiness: return "readiness";
      case event_e::idr_recovery: return "idr_recovery";
      case event_e::liveness: return "liveness";
      case event_e::routine_repeat_suppressed: return "routine_repeat_suppressed";
      case event_e::rate_ceiling: return "rate_ceiling";
      case event_e::receipt_not_mapped: return "receipt_not_mapped";
      case event_e::authority_lost: return "authority_lost";
      case event_e::stale_or_replayed: return "stale_or_replayed";
      case event_e::regressed_vs_emitted: return "regressed_vs_emitted";
      case event_e::tick_collision: return "tick_collision";
      case event_e::source_stale: return "source_stale";
      case event_e::unsupported_route: return "unsupported_route";
      case event_e::unsupported_codec: return "unsupported_codec";
      case event_e::unsupported_loop: return "unsupported_loop";
    }
    return "invalid_event";
  }

  struct receipt_t {
    std::uint64_t session_generation = 0;
    std::uint64_t capture_generation = 0;
    std::uint64_t capture_epoch = 0;
    std::uint64_t source_sequence = 0;
    mode_e mode = mode_e::waiting;
    event_e event = event_e::receipt_not_mapped;
    // Internal measurement inputs, omitted from public diagnostics and history JSON.
    std::optional<std::int64_t> emitted_source_ns;
    std::int64_t observed_steady_ns = -1;
  };
}  // namespace vrr_pacing
