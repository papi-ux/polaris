#include "live_tuning.h"
#include "config.h"
#include "configuration_store.h"
#include "settings_metadata.h"
#include "uuid.h"
#include <atomic>

namespace live_tuning {
  nlohmann::json describe(const adaptive_bitrate::state_t &c,
                         const stream_stats::stats_t &s, bool configured) {
    const bool matching_session = c.session_exclusive && c.session_generation == s.session_generation &&
      c.app_session_id == s.app_session_id;
    const bool supported = s.streaming && matching_session && c.runtime_update_supported;
    const bool pending = supported && c.target_bitrate_kbps > 0 &&
      c.target_bitrate_kbps != c.applied_bitrate_kbps;
    std::string state = !configured ? "off" : !s.streaming ? "waiting" :
      !supported ? "unavailable" : pending ? "applying" :
      !c.feedback_initialized ? "measuring" :
      c.applied_bitrate_kbps < c.base_bitrate_kbps ? "adjusting" : "stable";
    return {
      {"version", 1}, {"enabled", configured}, {"scope", "host"},
      {"state", state}, {"supported", supported},
      {"reason", !s.streaming ? "no_stream" : !matching_session ? "session_scope_unavailable" : c.reason},
      {"runtime_enabled", c.enabled}, {"pending", pending},
      {"quality_limit_kbps", matching_session ? c.base_bitrate_kbps : 0},
      {"requested_bitrate_kbps", supported ? c.target_bitrate_kbps : 0},
      {"applied_bitrate_kbps", s.streaming && matching_session ? c.applied_bitrate_kbps : 0},
      {"session_generation", s.session_generation},
      {"app_session_id", s.app_session_id}
    };
  }
  nlohmann::json snapshot(const stream_stats::stats_t &stats) {
    // Serialize preference with its revision; controller telemetry is one snapshot.
    std::lock_guard guard(configuration_store::mutex());
    static const auto instance = uuid_util::uuid_t::generate().string();
    static std::atomic<std::uint64_t> sequence {0};
    const auto controller = adaptive_bitrate::get_state();
    auto value = describe(controller, stats, controller.configured_enabled);
    value["configuration_revision"] = configuration_store::revision(config::sunshine.config_file);
    value["host_instance"] = instance;
    value["sequence"] = ++sequence;
    return value;
  }
  nlohmann::json set_enabled(doctor_actions::paired_global_control_guard_t &authority,
                            bool enabled, const std::optional<std::string> &expected) {
    if (!authority) return {{"status", false}, {"http_status", 403}, {"code", "active_owner_required"}};
    std::lock_guard guard(configuration_store::mutex());
    const auto result = configuration_store::patch(config::sunshine.config_file,
      {{"adaptive_bitrate_enabled", enabled ? "enabled" : "disabled"}}, expected);
    if (result != configuration_store::result::committed) return {
      {"status", false}, {"http_status", result == configuration_store::result::conflict ? 412 : 500},
      {"code", result == configuration_store::result::conflict ? "configuration_changed" : "save_failed"},
      {"live_tuning", snapshot(stream_stats::get_current())}
    };
    // An identical retry acknowledges intent without superseding a Doctor run.
    if (enabled != adaptive_bitrate::get_state().configured_enabled && !authority.set_adaptive_enabled(enabled)) {
      return {{"status", false}, {"http_status", 409}, {"code", "session_changed"}};
    }
    settings_metadata::note_config_write("live_tuning", {"adaptive_bitrate_enabled"});
    return {{"status", true}, {"http_status", 200}, {"live_tuning", snapshot(stream_stats::get_current())}};
  }
}
