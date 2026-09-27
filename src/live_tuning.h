#pragma once
#include "adaptive_bitrate.h"
#include "configuration_store.h"
#include "doctor_actions.h"
#include "stream_stats.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace live_tuning {
  nlohmann::json describe(const adaptive_bitrate::state_t &controller,
                         const stream_stats::stats_t &stats,
                         bool configured_enabled);
  nlohmann::json snapshot(const stream_stats::stats_t &stats);
  // Caller must retain the authorized guard across persistence and application.
  // Legacy paired endpoints may omit expected; new clients must supply it.
  // A settings file the store refuses answers 503 with code config_unreadable,
  // and says why in @p refusal when the caller asks. Only the console's own
  // route asks, so a paired client never receives the file's path.
  nlohmann::json set_enabled(doctor_actions::paired_global_control_guard_t &authority,
                            bool enabled, const std::optional<std::string> &expected = std::nullopt,
                            configuration_store::refusal_t *refusal = nullptr);
}

