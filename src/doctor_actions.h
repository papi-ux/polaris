/**
 * @file src/doctor_actions.h
 * @brief Evidence-gated, reversible one-click Doctor actions shared by web and paired clients.
 */
#pragma once

#include <nlohmann/json_fwd.hpp>

#include "stream_stats.h"

namespace doctor_actions {

  /** Current telemetry must pass this guard before Doctor may reduce quality. */
  bool network_pressure_confirmed(const stream_stats::stats_t &stats);

  /** Clamp a proposed bitrate to one guarded reduction step. */
  int guarded_bitrate_target(int current_bitrate_kbps,
                             int requested_bitrate_kbps,
                             int minimum_bitrate_kbps);

  /** Clamp a quality retry to one 25% increase toward the paired target. */
  int guarded_quality_retry_target(int current_bitrate_kbps,
                                   int paired_target_bitrate_kbps);

  /** Execute recheck, apply, verify, or undo and return the Doctor action result contract. */
  nlohmann::json execute(const nlohmann::json &request);

}  // namespace doctor_actions
