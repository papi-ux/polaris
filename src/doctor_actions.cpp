/**
 * @file src/doctor_actions.cpp
 * @brief Evidence-gated, reversible one-click Doctor actions shared by web and paired clients.
 */

#include "doctor_actions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "adaptive_bitrate.h"
#include "globals.h"
#include "logging.h"
#include "recovery_profile.h"

using namespace std::literals;

namespace doctor_actions {
  namespace {
    enum class action_kind_e {
      none,
      lower_bitrate,
      restore_quality
    };

    constexpr auto verification_delay = 8s;

    struct action_run_t {
      bool active = false;
      action_kind_e kind = action_kind_e::none;
      std::string run_id;
      std::string owner_uuid;
      std::string app_uuid;
      std::uint64_t session_generation = 0;
      bool previous_adaptive_enabled = false;
      int previous_base_bitrate_kbps = 0;
      int previous_live_bitrate_kbps = 0;
      int previous_max_bitrate_kbps = 0;
      int applied_bitrate_kbps = 0;
      int goal_bitrate_kbps = 0;
      std::uint64_t verification_step = 0;
      std::uint64_t network_sample_revision_at_apply = 0;
      bool requires_media_sample = false;
      bool verification_passed = false;
      stream_stats::network_verification_window_t verified_window;
      std::chrono::steady_clock::time_point applied_at {};
    };

    std::mutex action_mutex;
    action_run_t action_run;
    std::atomic<std::uint64_t> run_sequence {0};

    struct session_scope_t {
      std::string owner_uuid;
      std::uint64_t session_generation = 0;
      int base_bitrate_kbps = 0;
      bool auto_fix_eligible = true;
    };
    std::vector<session_scope_t> controller_sessions;

    bool matches_live_scope(const action_run_t &run,
                            const recovery_action_context_t *context) {
      if (run.session_generation == 0) {
        // Unit-level callers of the legacy one-argument API have no trusted
        // session context. Production routes always use the scoped overload.
        return context == nullptr;
      }
      return context != nullptr && context->stats.streaming &&
        context->session_generation == run.session_generation &&
        context->owner_uuid == run.owner_uuid && context->app_uuid == run.app_uuid;
    }

    bool valid_live_scope_shape(const recovery_action_context_t *context) {
      if (context == nullptr) return true;
      if (!context->active_owner || !context->host_tuning_allowed ||
          !context->stats.streaming || context->session_generation == 0 ||
          context->owner_uuid.empty() || context->app_uuid.empty()) {
        return false;
      }
      return true;
    }

    bool valid_live_scope_locked(const recovery_action_context_t *context) {
      if (!valid_live_scope_shape(context)) return false;
      if (context == nullptr) return true;
      return controller_sessions.size() == 1 &&
        controller_sessions.front().auto_fix_eligible &&
        controller_sessions.front().owner_uuid == context->owner_uuid &&
        controller_sessions.front().session_generation == context->session_generation;
    }

    bool valid_live_scope(const recovery_action_context_t *context) {
      std::lock_guard<std::mutex> lock(action_mutex);
      return valid_live_scope_locked(context);
    }

    nlohmann::json action_in_progress(const action_run_t &run) {
      return {
        {"status", false}, {"changed", false}, {"state", "action_in_progress"},
        {"run_id", run.run_id},
        {"error", "Finish or undo the active same-stream Doctor change before starting another."}
      };
    }

    int restore_bitrate_run_locked(action_run_t &run) {
      const int restore_live_bitrate_kbps = run.previous_live_bitrate_kbps > 0 ?
        run.previous_live_bitrate_kbps : run.previous_base_bitrate_kbps;
      const int temporary_max_bitrate_kbps = std::max({
        run.previous_max_bitrate_kbps,
        run.previous_base_bitrate_kbps,
        restore_live_bitrate_kbps
      });
      if (temporary_max_bitrate_kbps > 0) {
        adaptive_bitrate::set_max_bitrate(temporary_max_bitrate_kbps);
      }
      if (restore_live_bitrate_kbps > 0) {
        adaptive_bitrate::set_live_bitrate(restore_live_bitrate_kbps);
      }
      if (run.previous_base_bitrate_kbps > 0) {
        adaptive_bitrate::set_base_bitrate(run.previous_base_bitrate_kbps);
      }
      adaptive_bitrate::set_runtime_enabled(run.previous_adaptive_enabled);
      if (run.previous_max_bitrate_kbps > 0) {
        adaptive_bitrate::set_max_bitrate(run.previous_max_bitrate_kbps);
      }
      run.active = false;
      return restore_live_bitrate_kbps;
    }

    int current_live_bitrate(const stream_stats::stats_t &stats) {
      return stats.adaptive_runtime_update_supported && stats.adaptive_target_bitrate_kbps > 0 ?
        stats.adaptive_target_bitrate_kbps : stats.bitrate_kbps;
    }

    nlohmann::json network_evidence(const stream_stats::stats_t &stats) {
      return {
        {"streaming", stats.streaming},
        {"network_risk", stats.network_risk},
        {"packet_loss_pct", stats.packet_loss},
        {"packet_loss_available", stats.packet_loss_available},
        {"packet_loss_source", stats.packet_loss_source},
        {"control_channel_packet_loss_pct", stats.control_channel_packet_loss},
        {"network_sample_revision", stats.network_sample_revision},
        {"latency_ms", stats.latency_ms},
        {"bitrate_kbps", current_live_bitrate(stats)},
        {"paired_target_bitrate_kbps", stats.paired_target_bitrate_kbps},
        {"effective_launch_bitrate_kbps", stats.effective_launch_bitrate_kbps},
        {"optimization_source", stats.optimization_source}
      };
    }

    bool network_stable_for_quality_retry(const stream_stats::stats_t &stats) {
      const bool network_evidence_available =
        stats.packet_loss_available || stats.control_channel_samples > 0;
      return stats.streaming && network_evidence_available && !stats.network_risk &&
        stats.packet_loss <= 2.0 && stats.latency_ms < 45.0;
    }

    nlohmann::json verification_window_json(
        const stream_stats::network_verification_window_t &window) {
      return {
        {"complete", window.complete},
        {"samples", window.sample_count},
        {"media_samples", window.media_sample_count},
        {"first_delay_ms", window.first_delay_ms},
        {"last_delay_ms", window.last_delay_ms},
        {"span_ms", window.span_ms},
        {"last_age_ms", window.last_age_ms},
        {"last_revision", window.last_revision},
        {"last_latency_ms", window.latency_ms},
        {"last_packet_loss_pct", window.packet_loss_available ?
          nlohmann::json(window.packet_loss) : nlohmann::json(nullptr)},
        {"peak_latency_ms", window.max_latency_ms},
        {"peak_packet_loss_pct", window.any_packet_loss_available ?
          nlohmann::json(window.max_packet_loss) : nlohmann::json(nullptr)},
        {"any_network_risk", window.any_network_risk}
      };
    }

    bool verification_window_stable(
        const stream_stats::network_verification_window_t &window,
        const action_run_t &run) {
      const bool required_media_arrived =
        !run.requires_media_sample || window.media_sample_count > 0;
      const bool restoring_quality = run.kind == action_kind_e::restore_quality;
      const bool network_risk = restoring_quality ? window.any_network_risk : window.network_risk;
      const bool packet_loss_available = restoring_quality ?
        window.any_packet_loss_available : window.packet_loss_available;
      const double packet_loss = restoring_quality ? window.max_packet_loss : window.packet_loss;
      const double latency_ms = restoring_quality ? window.max_latency_ms : window.latency_ms;
      return window.complete && required_media_arrived &&
        !network_risk &&
        (!packet_loss_available || packet_loss <= 2.0) &&
        latency_ms < 45.0;
    }

    int effective_quality_restore_target(const stream_stats::stats_t &stats) {
      if (stats.paired_target_bitrate_kbps <= 0) return 0;
      if (stats.effective_launch_bitrate_kbps <= 0) return 0;
      return std::min(
        stats.paired_target_bitrate_kbps,
        stats.effective_launch_bitrate_kbps
      );
    }

    void run_verification_watchdog(const std::string &run_id,
                                   std::uint64_t verification_step,
                                   const std::string &owner_uuid,
                                   std::uint64_t session_generation) {
      std::lock_guard<std::mutex> lock(action_mutex);
      if (!action_run.active || action_run.run_id != run_id ||
          action_run.verification_step != verification_step ||
          action_run.owner_uuid != owner_uuid ||
          action_run.session_generation != session_generation) {
        return;
      }

      if (session_generation > 0 &&
          (controller_sessions.size() != 1 ||
           controller_sessions.front().owner_uuid != owner_uuid ||
           controller_sessions.front().session_generation != session_generation)) {
        // Session start/end owns controller restoration under this same lock.
        // If the scope is no longer exact, never write the old receipt into a
        // different encoder generation.
        action_run = action_run_t {};
        BOOST_LOG(info) << "Doctor: retired stale verification watchdog without restoring into a newer stream run="sv
                        << run_id;
        return;
      }

      const auto stats = stream_stats::get_current();
      const auto adaptive_state = adaptive_bitrate::get_state();
      const auto window = stream_stats::get_network_verification_window(
        action_run.network_sample_revision_at_apply,
        action_run.applied_at,
        verification_delay
      );
      const bool verification_passed =
        adaptive_state.runtime_update_supported &&
        verification_window_stable(window, action_run);
      if (!verification_passed) {
        const int restored_bitrate_kbps = restore_bitrate_run_locked(action_run);
        BOOST_LOG(warning) << "Doctor: automatic verification failed; restored "sv
                           << restored_bitrate_kbps << " kbps run=" << run_id;
        return;
      }

      action_run.verification_passed = true;
      action_run.verified_window = window;
      BOOST_LOG(info) << "Doctor: automatic verification passed run="sv << run_id
                      << " step=" << verification_step;
    }

    void schedule_verification_watchdog(const std::string &run_id,
                                        std::uint64_t verification_step,
                                        const std::string &owner_uuid,
                                        std::uint64_t session_generation) {
      task_pool.pushDelayed([run_id, verification_step, owner_uuid, session_generation]() {
        run_verification_watchdog(
          run_id, verification_step, owner_uuid, session_generation
        );
      }, verification_delay);
    }

    std::string next_run_id() {
      const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
      return "doctor-run-" + std::to_string(ticks) + "-" +
        std::to_string(run_sequence.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    nlohmann::json recovery_rejected(std::string error, std::string state = "rejected") {
      return {
        {"status", false},
        {"changed", false},
        {"state", state},
        {"recovery_state", state},
        {"error", std::move(error)}
      };
    }

    nlohmann::json execute_recovery_action(const nlohmann::json &request,
                                            const recovery_action_context_t &context) {
      const auto action_id = request.value("action_id", std::string {});
      const bool recovery_undo = action_id == "undo" ||
        action_id == "undo_recovery_profile_next_launch";
      if (action_id != "apply_recovery_profile_next_launch" &&
          action_id != "verify_recovery_profile_next_launch" &&
          !recovery_undo) {
        return nlohmann::json();
      }

      if (!recovery_undo) {
        return {
          {"status", false},
          {"changed", false},
          {"state", "deprecated"},
          {"recovery_state", "deprecated"},
          {"code", "unsupported_deprecated"},
          {"deprecated", true},
          {"applicable", false},
          {"cancellable", true},
          {"error", "Next-launch recovery profiles are observational legacy records and can no longer be applied or verified."}
        };
      }

      const auto run_id = request.value("run_id", std::string {});
      if (recovery_undo) {
        if (run_id.starts_with("doctor-run-")) {
          // Live receipts are authenticated against the active stream scope
          // below; never probe the deprecated on-disk namespace first.
          return nlohmann::json();
        }
        if (!run_id.starts_with("recovery-run-")) {
          return recovery_rejected(
            "Unknown Doctor undo receipt namespace.",
            "scope_mismatch"
          );
        }
        if (context.state_path.empty() ||
            (context.require_owner_scope && context.owner_uuid.empty())) {
          return recovery_rejected(
            "Only the paired owner or authenticated host can undo this recovery run.",
            "evidence_changed"
          );
        }
        const std::optional<std::string> owner_scope = context.require_owner_scope ?
          std::optional<std::string> {context.owner_uuid} : std::nullopt;
        auto result = recovery_profile::undo_run(
          context.state_path, run_id, owner_scope
        );
        return result;
      }
      return nlohmann::json();
    }
  }  // namespace

  bool network_pressure_confirmed(const stream_stats::stats_t &stats) {
    return stats.streaming && stats.network_risk &&
      ((stats.packet_loss_available && stats.packet_loss > 2.0) || stats.latency_ms >= 45.0);
  }

  bool paired_route_allowed(std::string_view action_id,
                            std::string_view run_id,
                            bool active_owner_present,
                            bool caller_is_active_owner) {
    if (caller_is_active_owner) return true;
    if (action_id == "apply_recovery_profile_next_launch" ||
        action_id == "verify_recovery_profile_next_launch") {
      // These operations are informationally deprecated and cannot mutate.
      return true;
    }
    if ((action_id == "undo" || action_id == "undo_recovery_profile_next_launch") &&
        run_id.starts_with("recovery-run-")) {
      // Core dispatch still enforces the paired certificate's owner scope.
      return true;
    }
    (void) active_owner_present;
    return false;
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

  nlohmann::json steam_vdf_read_only_response() {
    return {
      {"status", false},
      {"changed", false},
      {"state", "read_only"},
      {"error", "Automatic Steam profile changes and their Undo are disabled in this release. Review Steam Input settings manually."}
    };
  }

  static nlohmann::json execute_live_action(
      const nlohmann::json &request,
      const recovery_action_context_t *trusted_context) {
    const auto action_id = request.value("action_id", std::string {});

    if (action_id == "undo") {
      const auto run_id = request.value("run_id", std::string {});
      std::lock_guard<std::mutex> lock(action_mutex);
      if (action_run.active && !matches_live_scope(action_run, trusted_context)) {
        return {
          {"status", false}, {"changed", false}, {"state", "scope_mismatch"},
          {"error", "This Doctor change belongs to a different active stream owner or generation."}
        };
      }
      if (!action_run.active || run_id.empty() || run_id != action_run.run_id) {
        return {{"status", false}, {"changed", false}, {"error", "This Doctor undo is no longer available."}};
      }

      const int restore_live_bitrate_kbps = restore_bitrate_run_locked(action_run);
      return {
        {"status", true},
        {"changed", true},
        {"state", "undone"},
        {"run_id", run_id},
        {"restored_bitrate_kbps", restore_live_bitrate_kbps},
        {"adaptive_bitrate_enabled", action_run.previous_adaptive_enabled}
      };
    }

    // Legacy Steam VDF action IDs remain recognized only so stale clients and
    // receipts fail closed before Steam shutdown or filesystem work.
    if (action_id == "disable_steam_input_xbox") {
      return steam_vdf_read_only_response();
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

    if (trusted_context != nullptr &&
        (action_id == "recheck_network" || action_id == "recheck_pacing" ||
         action_id == "lower_bitrate" || action_id == "restore_quality")) {
      if (!valid_live_scope(trusted_context)) {
        return {
          {"status", false}, {"changed", false}, {"state", "scope_unavailable"},
          {"error", "A complete active stream scope is required for this Doctor action."}
        };
      }

      // The paired client echoes the typed payload that Polaris most recently
      // described; it never supplies action policy. Re-derive that payload
      // from the trusted current host snapshot and reject stale, malformed, or
      // client-invented settings before touching the live encoder.
      const auto trusted_health = trusted_context->health.is_object() ?
        trusted_context->health : nlohmann::json::object();
      const auto current_doctor = stream_stats::build_doctor_json(
        trusted_context->stats,
        trusted_health,
        trusted_context->app_uuid
      );
      const auto current_action = current_doctor.value(
        "safe_recovery_action", nlohmann::json::object()
      );
      const auto expected_payload = current_action.value(
        "payload_preview", nlohmann::json::object()
      );
      const bool target_expected = expected_payload.contains("target_bitrate_kbps");
      const bool target_matches = target_expected ?
        request.contains("target_bitrate_kbps") &&
          request["target_bitrate_kbps"].is_number_integer() &&
          request["target_bitrate_kbps"] == expected_payload["target_bitrate_kbps"] :
        !request.contains("target_bitrate_kbps");
      const bool envelope_matches =
        current_action.value("id", std::string {}) == action_id &&
        request.contains("source_result_id") &&
        request["source_result_id"].is_string() &&
        request["source_result_id"] == current_doctor.value("result_id", std::string {}) &&
        target_matches;
      if (!envelope_matches) {
        return {
          {"status", false}, {"changed", false}, {"state", "evidence_changed"},
          {"code", "stale_action_envelope"},
          {"error", "Doctor evidence or its deterministic action changed; recheck before applying it."},
          {"doctor", current_doctor},
          {"evidence", evidence}
        };
      }
    }

    if (action_id == "recheck_network" || action_id == "recheck_pacing") {
      const bool confirmed = network_pressure_confirmed(stats);
      return {
        {"status", true},
        {"changed", false},
        {"state", action_id == "recheck_pacing" ? "observed" : confirmed ? "confirmed_pressure" : "stable"},
        {"message", action_id == "recheck_pacing" ?
          "Collected a fresh read-only pacing observation; no launch or game-process settings were changed." : confirmed ?
          "Current telemetry now confirms network pressure. Doctor can safely apply one bitrate step." :
          "Current loss and latency do not justify reducing bitrate."},
        {"evidence", evidence},
        {"doctor", nlohmann::json::parse(stats.to_json()).value("doctor", nlohmann::json::object())}
      };
    }

    if (action_id == "verify") {
      const auto run_id = request.value("run_id", std::string {});
      std::lock_guard<std::mutex> lock(action_mutex);
      if (action_run.active && !matches_live_scope(action_run, trusted_context)) {
        return {
          {"status", false}, {"changed", false}, {"state", "scope_mismatch"},
          {"error", "This Doctor verification belongs to a different active stream owner or generation."}
        };
      }
      if (!action_run.active || run_id.empty() || run_id != action_run.run_id) {
        return {{"status", false}, {"changed", false}, {"state", "expired"}, {"error", "Doctor run not found."}};
      }
      if (!valid_live_scope_locked(trusted_context)) {
        return {
          {"status", false}, {"changed", false}, {"state", "scope_unavailable"},
          {"error", "Doctor Auto Fix requires one fresh, unshared stream generation owning the live bitrate controller."}
        };
      }
      const auto verification_stats = stream_stats::get_current();
      const auto verification_evidence = network_evidence(verification_stats);
      auto verification_window = stream_stats::get_network_verification_window(
        action_run.network_sample_revision_at_apply,
        action_run.applied_at,
        verification_delay
      );
      if (action_run.verification_passed) {
        verification_window = action_run.verified_window;
      }
      if (!adaptive_bitrate::get_state().runtime_update_supported) {
        const int restored_bitrate_kbps = restore_bitrate_run_locked(action_run);
        return {
          {"status", true},
          {"changed", true},
          {"run_id", run_id},
          {"state", "rolled_back"},
          {"message", "Live encoder updates became unavailable during verification, so Doctor restored the prior target and ended the fix."},
          {"restored_bitrate_kbps", restored_bitrate_kbps},
          {"evidence", verification_evidence},
          {"verification_window", verification_window_json(verification_window)}
        };
      }
      const auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - action_run.applied_at
      ).count();
      if (elapsed_seconds < verification_delay.count()) {
        return {
          {"status", true},
          {"changed", false},
          {"run_id", run_id},
          {"state", "watching"},
          {"message", "Doctor is still collecting the required post-change evidence window."},
          {"elapsed_seconds", elapsed_seconds},
          {"retry_after_seconds", verification_delay.count() - elapsed_seconds},
          {"evidence", verification_evidence},
          {"verification_window", verification_window_json(verification_window)},
          {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
        };
      }
      const bool verification_stable = action_run.verification_passed ||
        verification_window_stable(verification_window, action_run);

      if (action_run.kind == action_kind_e::restore_quality) {
        if (!verification_stable) {
          const int restored_bitrate_kbps = restore_bitrate_run_locked(action_run);
          return {
            {"status", true},
            {"changed", true},
            {"run_id", run_id},
            {"state", "rolled_back"},
            {"message", "Verification did not receive enough clean post-change loss and latency evidence, so Doctor automatically restored the prior live target."},
            {"restored_bitrate_kbps", restored_bitrate_kbps},
            {"elapsed_seconds", elapsed_seconds},
            {"evidence", verification_evidence},
            {"verification_window", verification_window_json(verification_window)},
            {"undo", {{"available", false}}}
          };
        }

        if (action_run.applied_bitrate_kbps < action_run.goal_bitrate_kbps) {
          const int next_bitrate_kbps = guarded_quality_retry_target(
            action_run.applied_bitrate_kbps,
            action_run.goal_bitrate_kbps
          );
          adaptive_bitrate::set_max_bitrate(action_run.goal_bitrate_kbps);
          adaptive_bitrate::set_live_bitrate(next_bitrate_kbps);
          adaptive_bitrate::set_runtime_enabled(true);
          action_run.applied_bitrate_kbps = next_bitrate_kbps;
          action_run.applied_at = std::chrono::steady_clock::now();
          action_run.network_sample_revision_at_apply = verification_stats.network_sample_revision;
          action_run.verification_passed = false;
          action_run.verified_window = {};
          ++action_run.verification_step;
          if (trusted_context != nullptr) {
            schedule_verification_watchdog(
              action_run.run_id,
              action_run.verification_step,
              action_run.owner_uuid,
              action_run.session_generation
            );
          }
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
            {"evidence", verification_evidence},
            {"verification_window", verification_window_json(verification_window)},
            {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
          };
        }

        return {
          {"status", true},
          {"changed", false},
          {"run_id", run_id},
          {"state", "resolved"},
          {"message", "The capability-validated launch bitrate ceiling is restored and live network evidence remains stable."},
          {"elapsed_seconds", elapsed_seconds},
          {"evidence", verification_evidence},
          {"verification_window", verification_window_json(verification_window)},
          {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
        };
      }

      const bool resolved = verification_stable;
      if (!resolved) {
        const int restored_bitrate_kbps = restore_bitrate_run_locked(action_run);
        return {
          {"status", true},
          {"changed", true},
          {"run_id", run_id},
          {"state", "rolled_back"},
          {"message", "Verification did not clear the network evidence, so Doctor automatically restored the prior live target."},
          {"restored_bitrate_kbps", restored_bitrate_kbps},
          {"elapsed_seconds", elapsed_seconds},
          {"evidence", verification_evidence},
          {"verification_window", verification_window_json(verification_window)},
          {"undo", {{"available", false}}}
        };
      }
      action_run.verification_passed = true;
      return {
        {"status", true},
        {"changed", false},
        {"run_id", run_id},
        {"state", resolved ? "resolved" : "watching"},
        {"elapsed_seconds", elapsed_seconds},
        {"evidence", verification_evidence},
        {"verification_window", verification_window_json(verification_window)},
        {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
      };
    }

    if (action_id == "restore_quality") {
      const int current_bitrate_kbps = current_live_bitrate(stats);
      const int effective_target_bitrate_kbps = effective_quality_restore_target(stats);
      const bool reversible_live_reduction =
        effective_target_bitrate_kbps > current_bitrate_kbps;
      if (!reversible_live_reduction || !network_stable_for_quality_retry(stats)) {
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
      const int goal_bitrate_kbps = effective_target_bitrate_kbps;
      const int target_bitrate_kbps = guarded_quality_retry_target(
        current_bitrate_kbps,
        goal_bitrate_kbps
      );
      if (goal_bitrate_kbps <= 0 || target_bitrate_kbps <= current_bitrate_kbps) {
        return {
          {"status", false},
          {"changed", false},
          {"error", "The capability-validated launch bitrate ceiling is unavailable or already restored."},
          {"evidence", evidence}
        };
      }

      action_run_t run;
      run.active = true;
      run.kind = action_kind_e::restore_quality;
      run.run_id = next_run_id();
      if (trusted_context != nullptr) {
        run.owner_uuid = trusted_context->owner_uuid;
        run.app_uuid = trusted_context->app_uuid;
        run.session_generation = trusted_context->session_generation;
      }
      run.previous_adaptive_enabled = adaptive_state.enabled;
      run.previous_base_bitrate_kbps = adaptive_state.base_bitrate_kbps;
      run.previous_live_bitrate_kbps = current_bitrate_kbps;
      run.previous_max_bitrate_kbps = adaptive_state.max_bitrate_kbps;
      run.applied_bitrate_kbps = target_bitrate_kbps;
      run.goal_bitrate_kbps = goal_bitrate_kbps;
      run.verification_step = 1;
      run.network_sample_revision_at_apply = stats.network_sample_revision;
      run.requires_media_sample = false;
      run.applied_at = std::chrono::steady_clock::now();
      {
        std::lock_guard<std::mutex> lock(action_mutex);
        if (!valid_live_scope_locked(trusted_context)) {
          return {
            {"status", false}, {"changed", false}, {"state", "scope_unavailable"},
            {"error", "Doctor Auto Fix requires one fresh, unshared stream generation owning the live bitrate controller."}
          };
        }
        if (action_run.active) return action_in_progress(action_run);
        adaptive_bitrate::set_max_bitrate(goal_bitrate_kbps);
        adaptive_bitrate::set_live_bitrate(target_bitrate_kbps);
        adaptive_bitrate::set_runtime_enabled(true);
        action_run = run;
        if (trusted_context != nullptr) {
          schedule_verification_watchdog(
            action_run.run_id,
            action_run.verification_step,
            action_run.owner_uuid,
            action_run.session_generation
          );
        }
      }
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
    if (trusted_context != nullptr) {
      run.owner_uuid = trusted_context->owner_uuid;
      run.app_uuid = trusted_context->app_uuid;
      run.session_generation = trusted_context->session_generation;
    }
    run.previous_adaptive_enabled = adaptive_state.enabled;
    run.previous_base_bitrate_kbps = adaptive_state.base_bitrate_kbps;
    run.previous_live_bitrate_kbps = current_bitrate_kbps;
    run.previous_max_bitrate_kbps = adaptive_state.max_bitrate_kbps;
    run.applied_bitrate_kbps = target_bitrate_kbps;
    run.goal_bitrate_kbps = target_bitrate_kbps;
    run.verification_step = 1;
    run.network_sample_revision_at_apply = stats.network_sample_revision;
    run.requires_media_sample = stats.packet_loss_available && stats.packet_loss > 2.0;
    run.applied_at = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(action_mutex);
      if (!valid_live_scope_locked(trusted_context)) {
        return {
          {"status", false}, {"changed", false}, {"state", "scope_unavailable"},
          {"error", "Doctor Auto Fix requires one fresh, unshared stream generation owning the live bitrate controller."}
        };
      }
      if (action_run.active) return action_in_progress(action_run);
      adaptive_bitrate::set_live_bitrate(target_bitrate_kbps);
      adaptive_bitrate::set_runtime_enabled(true);
      action_run = run;
      if (trusted_context != nullptr) {
        schedule_verification_watchdog(
          action_run.run_id,
          action_run.verification_step,
          action_run.owner_uuid,
          action_run.session_generation
        );
      }
    }
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

  nlohmann::json execute(const nlohmann::json &request) {
    return execute_live_action(request, nullptr);
  }

  nlohmann::json execute(const nlohmann::json &request,
                         const recovery_action_context_t &recovery_context) {
    auto recovery_result = execute_recovery_action(request, recovery_context);
    if (!recovery_result.is_null()) {
      return recovery_result;
    }
    return execute_live_action(request, &recovery_context);
  }

  void session_started(std::string_view owner_uuid,
                       std::uint64_t session_generation,
                       int base_bitrate_kbps) {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (action_run.active) {
      const auto run_id = action_run.run_id;
      const int restored_bitrate_kbps = restore_bitrate_run_locked(action_run);
      BOOST_LOG(info) << "Doctor: new stream boundary restored "sv
                      << restored_bitrate_kbps << " kbps and retired run=" << run_id;
    }

    controller_sessions.erase(
      std::remove_if(
        controller_sessions.begin(), controller_sessions.end(),
        [owner_uuid, session_generation](const session_scope_t &scope) {
          return scope.owner_uuid == owner_uuid &&
            scope.session_generation == session_generation;
        }
      ),
      controller_sessions.end()
    );
    const bool unshared_at_start = controller_sessions.empty();
    for (auto &scope : controller_sessions) {
      scope.auto_fix_eligible = false;
    }
    controller_sessions.push_back({
      std::string {owner_uuid},
      session_generation,
      base_bitrate_kbps,
      unshared_at_start
    });

    // Preserve the pre-existing stream-start policy for the process-global
    // controller. Multi-session tracking only removes Doctor authority; it
    // must not silently disable a user-configured adaptive policy merely
    // because a viewer or second encoder attached.
    adaptive_bitrate::load_config();
    adaptive_bitrate::reset();
    adaptive_bitrate::set_base_bitrate(base_bitrate_kbps);
    stream_stats::set_doctor_live_action_scope_available(
      controller_sessions.size() == 1 && controller_sessions.front().auto_fix_eligible
    );
  }

  void session_ended(std::string_view owner_uuid, std::uint64_t session_generation) {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (action_run.active && action_run.session_generation == session_generation &&
        action_run.owner_uuid == owner_uuid) {
      const auto run_id = action_run.run_id;
      const int restored_bitrate_kbps = restore_bitrate_run_locked(action_run);
      BOOST_LOG(info) << "Doctor: stream ended; restored "sv << restored_bitrate_kbps
                      << " kbps and retired run=" << run_id;
    }

    controller_sessions.erase(
      std::remove_if(
        controller_sessions.begin(), controller_sessions.end(),
        [owner_uuid, session_generation](const session_scope_t &scope) {
          return scope.owner_uuid == owner_uuid &&
            scope.session_generation == session_generation;
        }
      ),
      controller_sessions.end()
    );

    // Encoder teardown already publishes runtime support loss. Do not change
    // the configured adaptive policy here; the next stream start reloads it,
    // while Doctor remains unavailable for any contaminated survivor.
    stream_stats::set_doctor_live_action_scope_available(
      controller_sessions.size() == 1 && controller_sessions.front().auto_fix_eligible
    );
  }

#ifdef POLARIS_TESTS
  void make_verification_due_for_tests() {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (action_run.active) action_run.applied_at -= verification_delay;
  }

  void make_verification_window_complete_for_tests() {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (!action_run.active) return;
    action_run.applied_at -= verification_delay;
    stream_stats::spread_network_verification_window_for_tests(
      action_run.network_sample_revision_at_apply,
      action_run.applied_at,
      std::chrono::steady_clock::now()
    );
  }

  void run_verification_watchdog_for_tests() {
    std::string run_id;
    std::string owner_uuid;
    std::uint64_t verification_step = 0;
    std::uint64_t session_generation = 0;
    {
      std::lock_guard<std::mutex> lock(action_mutex);
      if (!action_run.active) return;
      run_id = action_run.run_id;
      owner_uuid = action_run.owner_uuid;
      verification_step = action_run.verification_step;
      session_generation = action_run.session_generation;
    }
    run_verification_watchdog(
      run_id, verification_step, owner_uuid, session_generation
    );
  }
#endif

}  // namespace doctor_actions
