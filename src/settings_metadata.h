/**
 * @file src/settings_metadata.h
 * @brief Shared settings metadata builders for the HTTP servers.
 */
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "adaptive_bitrate.h"
#include "stream_stats.h"

namespace settings_metadata {
  /**
   * @brief Legacy compatibility field. Launch policy no longer has an AI-owned mode.
   */
  bool ai_auto_quality_enabled();

  /**
   * @brief Map a session-health limiting factor onto the served blocked reason.
   */
  std::string auto_quality_blocked_reason(const std::string &limiting_factor);

  /**
   * @brief Build the auto-quality policy JSON served with session health.
   */
  nlohmann::json build_auto_quality_policy_json(const nlohmann::json &health,
                                                const adaptive_bitrate::state_t &adaptive_state,
                                                int encoder_bitrate_kbps);

  /**
   * @brief Whether this host can create or use a virtual display right now.
   */
  bool host_virtual_display_available();

  /**
   * @brief The configured (desired) stream display mode selection id.
   */
  std::string configured_stream_display_mode_selection();

  /**
   * @brief The effective stream display mode selection for a live session.
   */
  std::string effective_stream_display_mode_selection(
    const stream_stats::stats_t &stats,
    bool session_uses_virtual_display
  );

  /**
   * @brief Convenience overload reading the live session virtual-display flag.
   */
  std::string effective_stream_display_mode_selection(const stream_stats::stats_t &stats);

  /**
   * @brief Human label for a stream display mode selection id.
   */
  std::string stream_display_mode_label_for_selection(const std::string &selection);

  /**
   * @brief Reason copy for a stream display mode selection id.
   */
  std::string stream_display_mode_reason_for_selection(const std::string &selection);

  /**
   * @brief The capabilities modes catalog served to clients.
   */
  nlohmann::json stream_display_mode_options_json();

  /**
   * @brief Build the session-status tuning JSON block.
   */
  nlohmann::json build_tuning_json(const adaptive_bitrate::state_t &adaptive_state,
                                   const stream_stats::stats_t &stats,
                                   bool mangohud_configured);
}  // namespace settings_metadata
