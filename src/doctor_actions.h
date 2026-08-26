/**
 * @file src/doctor_actions.h
 * @brief Evidence-gated, reversible one-click Doctor actions shared by web and paired clients.
 */
#pragma once

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "stream_stats.h"

namespace doctor_actions {

  struct recovery_action_context_t {
    bool active_owner = false;
    bool host_tuning_allowed = false;
    bool caller_is_viewer = false;
    bool require_owner_scope = true;
    std::string owner_uuid;
    std::string device_name;
    std::string app_uuid;
    std::string app_name;
    std::string launch_instance_id;
    std::uint64_t session_generation = 0;
    std::string effective_stream_display_mode;
    std::filesystem::path state_path;
    stream_stats::stats_t stats;
    nlohmann::json health;
  };

  /** Current telemetry must pass this guard before Doctor may reduce quality. */
  bool network_pressure_confirmed(const stream_stats::stats_t &stats);

  /** Paired-route authorization, including owner-scoped Undo after disconnect. */
  bool paired_route_allowed(std::string_view action_id,
                            bool active_owner_present,
                            bool caller_is_active_owner);

  /** Clamp a proposed bitrate to one guarded reduction step. */
  int guarded_bitrate_target(int current_bitrate_kbps,
                             int requested_bitrate_kbps,
                             int minimum_bitrate_kbps);

  /** Clamp a quality retry to one 25% increase toward the paired target. */
  int guarded_quality_retry_target(int current_bitrate_kbps,
                                   int paired_target_bitrate_kbps);

  /** Execute recheck, apply, verify, or undo and return the Doctor action result contract. */
  nlohmann::json execute(const nlohmann::json &request);

  /** Execute with trusted current owner/app/telemetry for durable recovery actions. */
  nlohmann::json execute(const nlohmann::json &request,
                         const recovery_action_context_t &recovery_context);

}  // namespace doctor_actions
