/**
 * @file src/doctor_actions.cpp
 * @brief Evidence-gated, reversible one-click Doctor actions shared by web and paired clients.
 */

#include "doctor_actions.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "adaptive_bitrate.h"
#include "logging.h"

using namespace std::literals;

namespace doctor_actions {
  namespace {
    enum class action_kind_e {
      none,
      lower_bitrate,
      restore_quality
    };

    struct action_run_t {
      bool active = false;
      action_kind_e kind = action_kind_e::none;
      std::string run_id;
      bool previous_adaptive_enabled = false;
      int previous_base_bitrate_kbps = 0;
      int previous_live_bitrate_kbps = 0;
      int previous_max_bitrate_kbps = 0;
      int applied_bitrate_kbps = 0;
      int goal_bitrate_kbps = 0;
      std::chrono::steady_clock::time_point applied_at {};
    };

    std::mutex action_mutex;
    action_run_t action_run;

    int current_live_bitrate(const stream_stats::stats_t &stats) {
      return stats.adaptive_runtime_update_supported && stats.adaptive_target_bitrate_kbps > 0 ?
        stats.adaptive_target_bitrate_kbps : stats.bitrate_kbps;
    }

    nlohmann::json network_evidence(const stream_stats::stats_t &stats) {
      return {
        {"streaming", stats.streaming},
        {"network_risk", stats.network_risk},
        {"packet_loss_pct", stats.packet_loss},
        {"latency_ms", stats.latency_ms},
        {"bitrate_kbps", current_live_bitrate(stats)},
        {"paired_target_bitrate_kbps", stats.paired_target_bitrate_kbps},
        {"optimization_source", stats.optimization_source}
      };
    }

    bool network_stable_for_quality_retry(const stream_stats::stats_t &stats) {
      return stats.streaming && !stats.network_risk &&
        stats.packet_loss <= 2.0 && stats.latency_ms < 45.0;
    }

    std::string next_run_id() {
      const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
      return "doctor-run-" + std::to_string(ticks);
    }
  }  // namespace

  bool network_pressure_confirmed(const stream_stats::stats_t &stats) {
    return stats.streaming && stats.network_risk &&
      (stats.packet_loss > 2.0 || stats.latency_ms >= 45.0);
  }

  int guarded_bitrate_target(int current_bitrate_kbps,
                             int requested_bitrate_kbps,
                             int minimum_bitrate_kbps) {
    if (current_bitrate_kbps <= 1000) {
      return current_bitrate_kbps;
    }
    const int one_step_floor_kbps = std::max(
      std::max(1000, minimum_bitrate_kbps),
      static_cast<int>(std::round(current_bitrate_kbps * 0.80))
    );
    const int requested = requested_bitrate_kbps > 0 ? requested_bitrate_kbps : one_step_floor_kbps;
    return std::min(std::max(requested, one_step_floor_kbps), current_bitrate_kbps - 1);
  }

  int guarded_quality_retry_target(int current_bitrate_kbps,
                                   int paired_target_bitrate_kbps) {
    if (current_bitrate_kbps <= 0 || paired_target_bitrate_kbps <= current_bitrate_kbps) {
      return current_bitrate_kbps;
    }
    const int one_step_ceiling_kbps = std::max(
      current_bitrate_kbps + 1,
      static_cast<int>(std::round(current_bitrate_kbps * 1.25))
    );
    return std::min(one_step_ceiling_kbps, paired_target_bitrate_kbps);
  }

  nlohmann::json execute(const nlohmann::json &request) {
    const auto action_id = request.value("action_id", std::string {});

    if (action_id == "undo") {
      const auto run_id = request.value("run_id", std::string {});
      std::lock_guard<std::mutex> lock(action_mutex);
      if (!action_run.active || run_id.empty() || run_id != action_run.run_id) {
        return {{"status", false}, {"changed", false}, {"error", "This Doctor undo is no longer available."}};
      }
      if (!adaptive_bitrate::get_state().runtime_update_supported) {
        return {
          {"status", false},
          {"changed", false},
          {"state", "runtime_update_unavailable"},
          {"error", "The active encoder cannot apply a live bitrate change."}
        };
      }

      const int restore_live_bitrate_kbps = action_run.previous_live_bitrate_kbps > 0 ?
        action_run.previous_live_bitrate_kbps : action_run.previous_base_bitrate_kbps;
      const int temporary_max_bitrate_kbps = std::max({
        action_run.previous_max_bitrate_kbps,
        action_run.previous_base_bitrate_kbps,
        restore_live_bitrate_kbps
      });
      if (temporary_max_bitrate_kbps > 0) {
        adaptive_bitrate::set_max_bitrate(temporary_max_bitrate_kbps);
      }
      if (restore_live_bitrate_kbps > 0) {
        adaptive_bitrate::set_live_bitrate(restore_live_bitrate_kbps);
      }
      if (action_run.previous_base_bitrate_kbps > 0) {
        adaptive_bitrate::set_base_bitrate(action_run.previous_base_bitrate_kbps);
      }
      adaptive_bitrate::set_enabled(action_run.previous_adaptive_enabled);
      if (action_run.previous_max_bitrate_kbps > 0) {
        adaptive_bitrate::set_max_bitrate(action_run.previous_max_bitrate_kbps);
      }
      action_run.active = false;
      return {
        {"status", true},
        {"changed", true},
        {"state", "undone"},
        {"restored_bitrate_kbps", restore_live_bitrate_kbps},
        {"adaptive_bitrate_enabled", action_run.previous_adaptive_enabled}
      };
    }

    const auto stats = stream_stats::get_current();
    const auto evidence = network_evidence(stats);
    if (!stats.streaming) {
      return {
        {"status", false},
        {"changed", false},
        {"state", "needs_stream"},
        {"error", "Start the affected stream before Doctor applies or verifies a live fix."},
        {"evidence", evidence}
      };
    }

    if (action_id == "recheck_network") {
      const bool confirmed = network_pressure_confirmed(stats);
      return {
        {"status", true},
        {"changed", false},
        {"state", confirmed ? "confirmed_pressure" : "stable"},
        {"message", confirmed ?
          "Current telemetry now confirms network pressure. Doctor can safely apply one bitrate step." :
          "Current loss and latency do not justify reducing bitrate."},
        {"evidence", evidence},
        {"doctor", nlohmann::json::parse(stats.to_json()).value("doctor", nlohmann::json::object())}
      };
    }

    if (action_id == "verify") {
      const auto run_id = request.value("run_id", std::string {});
      std::lock_guard<std::mutex> lock(action_mutex);
      if (!action_run.active || run_id.empty() || run_id != action_run.run_id) {
        return {{"status", false}, {"changed", false}, {"state", "expired"}, {"error", "Doctor run not found."}};
      }
      if (!adaptive_bitrate::get_state().runtime_update_supported) {
        return {
          {"status", false},
          {"changed", false},
          {"run_id", run_id},
          {"state", "runtime_update_unavailable"},
          {"error", "The active encoder can no longer apply live bitrate changes."},
          {"evidence", evidence}
        };
      }
      const auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - action_run.applied_at
      ).count();

      if (action_run.kind == action_kind_e::restore_quality) {
        if (!network_stable_for_quality_retry(stats)) {
          return {
            {"status", true},
            {"changed", false},
            {"run_id", run_id},
            {"state", "needs_attention"},
            {"message", "Quality recovery paused because current loss or latency is no longer clean."},
            {"elapsed_seconds", elapsed_seconds},
            {"evidence", evidence},
            {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
          };
        }

        if (action_run.applied_bitrate_kbps < action_run.goal_bitrate_kbps) {
          const int next_bitrate_kbps = guarded_quality_retry_target(
            action_run.applied_bitrate_kbps,
            action_run.goal_bitrate_kbps
          );
          adaptive_bitrate::set_max_bitrate(action_run.goal_bitrate_kbps);
          adaptive_bitrate::set_live_bitrate(next_bitrate_kbps);
          adaptive_bitrate::set_enabled(true);
          action_run.applied_bitrate_kbps = next_bitrate_kbps;
          action_run.applied_at = std::chrono::steady_clock::now();
          BOOST_LOG(info) << "Doctor: stable quality retry advanced to "sv
                          << next_bitrate_kbps << " kbps run=" << run_id;
          return {
            {"status", true},
            {"changed", true},
            {"run_id", run_id},
            {"state", "watching"},
            {"message", "The last quality step stayed stable. Doctor advanced one more guarded step."},
            {"applied", {{"bitrate_kbps", next_bitrate_kbps}, {"target_bitrate_kbps", action_run.goal_bitrate_kbps}}},
            {"verification", {{"delay_seconds", 8}, {"action_id", "verify"}, {"run_id", run_id}}},
            {"evidence", evidence},
            {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
          };
        }

        return {
          {"status", true},
          {"changed", false},
          {"run_id", run_id},
          {"state", "resolved"},
          {"message", "The paired bitrate target is restored and live network evidence remains stable."},
          {"elapsed_seconds", elapsed_seconds},
          {"evidence", evidence},
          {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
        };
      }

      const bool resolved = !stats.network_risk && stats.packet_loss <= 2.0 && stats.latency_ms < 45.0;
      return {
        {"status", true},
        {"changed", false},
        {"run_id", run_id},
        {"state", resolved ? "resolved" : elapsed_seconds >= 8 ? "needs_attention" : "watching"},
        {"elapsed_seconds", elapsed_seconds},
        {"evidence", evidence},
        {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
      };
    }

    if (action_id == "restore_quality") {
      const bool history_safe_source = stats.optimization_source.find("history_safe") != std::string::npos;
      if (!history_safe_source || !network_stable_for_quality_retry(stats)) {
        return {
          {"status", false},
          {"changed", false},
          {"state", "evidence_changed"},
          {"error", "Current session policy and network evidence no longer authorize a quality retry."},
          {"evidence", evidence}
        };
      }

      const auto adaptive_state = adaptive_bitrate::get_state();
      if (!adaptive_state.runtime_update_supported) {
        return {
          {"status", false},
          {"changed", false},
          {"state", "runtime_update_unavailable"},
          {"error", "The active encoder cannot restore bitrate during a live stream."},
          {"evidence", evidence}
        };
      }
      const int current_bitrate_kbps = current_live_bitrate(stats);
      const int goal_bitrate_kbps = stats.paired_target_bitrate_kbps;
      const int target_bitrate_kbps = guarded_quality_retry_target(
        current_bitrate_kbps,
        goal_bitrate_kbps
      );
      if (goal_bitrate_kbps <= 0 || target_bitrate_kbps <= current_bitrate_kbps) {
        return {
          {"status", false},
          {"changed", false},
          {"error", "The paired bitrate target is unavailable or already restored."},
          {"evidence", evidence}
        };
      }

      action_run_t run;
      run.active = true;
      run.kind = action_kind_e::restore_quality;
      run.run_id = next_run_id();
      run.previous_adaptive_enabled = adaptive_state.enabled;
      run.previous_base_bitrate_kbps = adaptive_state.base_bitrate_kbps;
      run.previous_live_bitrate_kbps = current_bitrate_kbps;
      run.previous_max_bitrate_kbps = adaptive_state.max_bitrate_kbps;
      run.applied_bitrate_kbps = target_bitrate_kbps;
      run.goal_bitrate_kbps = goal_bitrate_kbps;
      run.applied_at = std::chrono::steady_clock::now();
      {
        std::lock_guard<std::mutex> lock(action_mutex);
        action_run = run;
      }

      adaptive_bitrate::set_max_bitrate(goal_bitrate_kbps);
      adaptive_bitrate::set_live_bitrate(target_bitrate_kbps);
      adaptive_bitrate::set_enabled(true);
      BOOST_LOG(info) << "Doctor: started guarded quality retry "sv
                      << current_bitrate_kbps << " -> " << target_bitrate_kbps
                      << " kbps toward " << goal_bitrate_kbps
                      << " kbps run=" << run.run_id;

      return {
        {"status", true},
        {"changed", true},
        {"state", "watching"},
        {"message", "Doctor restored one quality step and is watching live loss and latency before continuing."},
        {"run_id", run.run_id},
        {"applied", {{"bitrate_kbps", target_bitrate_kbps}, {"target_bitrate_kbps", goal_bitrate_kbps}, {"adaptive_bitrate_enabled", true}}},
        {"before", {{"bitrate_kbps", current_bitrate_kbps}, {"adaptive_bitrate_enabled", adaptive_state.enabled}}},
        {"verification", {{"delay_seconds", 8}, {"action_id", "verify"}, {"run_id", run.run_id}}},
        {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run.run_id}}},
        {"evidence", evidence}
      };
    }

    if (action_id != "lower_bitrate") {
      return {{"status", false}, {"changed", false}, {"error", "Unsupported Doctor action."}};
    }

    if (!network_pressure_confirmed(stats)) {
      return {
        {"status", false},
        {"changed", false},
        {"state", "evidence_changed"},
        {"error", "Current sustained loss or latency evidence no longer authorizes a bitrate reduction."},
        {"evidence", evidence},
        {"doctor", nlohmann::json::parse(stats.to_json()).value("doctor", nlohmann::json::object())}
      };
    }

    const auto adaptive_state = adaptive_bitrate::get_state();
    if (!adaptive_state.runtime_update_supported) {
      return {
        {"status", false},
        {"changed", false},
        {"state", "runtime_update_unavailable"},
        {"error", "The active encoder cannot lower bitrate during a live stream."},
        {"evidence", evidence}
      };
    }
    const int current_bitrate_kbps = current_live_bitrate(stats);
    const int target_bitrate_kbps = guarded_bitrate_target(
      current_bitrate_kbps,
      request.value("target_bitrate_kbps", 0),
      adaptive_state.min_bitrate_kbps
    );
    if (target_bitrate_kbps <= 0 || target_bitrate_kbps >= current_bitrate_kbps) {
      return {{"status", false}, {"changed", false}, {"error", "The live bitrate is unavailable or already at the safe floor."}};
    }

    action_run_t run;
    run.active = true;
    run.kind = action_kind_e::lower_bitrate;
    run.run_id = next_run_id();
    run.previous_adaptive_enabled = adaptive_state.enabled;
    run.previous_base_bitrate_kbps = adaptive_state.base_bitrate_kbps;
    run.previous_live_bitrate_kbps = current_bitrate_kbps;
    run.previous_max_bitrate_kbps = adaptive_state.max_bitrate_kbps;
    run.applied_bitrate_kbps = target_bitrate_kbps;
    run.goal_bitrate_kbps = target_bitrate_kbps;
    run.applied_at = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(action_mutex);
      action_run = run;
    }

    adaptive_bitrate::set_live_bitrate(target_bitrate_kbps);
    adaptive_bitrate::set_enabled(true);
    BOOST_LOG(info) << "Doctor: applied guarded bitrate step "sv
                    << current_bitrate_kbps << " -> " << target_bitrate_kbps
                    << " kbps run=" << run.run_id;

    return {
      {"status", true},
      {"changed", true},
      {"state", "watching"},
      {"run_id", run.run_id},
      {"applied", {{"bitrate_kbps", target_bitrate_kbps}, {"adaptive_bitrate_enabled", true}}},
      {"before", {{"bitrate_kbps", current_bitrate_kbps}, {"adaptive_bitrate_enabled", adaptive_state.enabled}}},
      {"verification", {{"delay_seconds", 8}, {"action_id", "verify"}, {"run_id", run.run_id}}},
      {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run.run_id}}},
      {"evidence", evidence}
    };
  }

}  // namespace doctor_actions
