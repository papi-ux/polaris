/**
 * @file src/doctor_actions.cpp
 * @brief Evidence-gated, reversible one-click Doctor actions shared by web and paired clients.
 */

#include "doctor_actions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "adaptive_bitrate.h"
#include "config.h"
#include "configuration_store.h"
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
    constexpr auto encoder_apply_timeout = 3s;
    constexpr auto encoder_rollback_timeout = 3s;
    constexpr auto recheck_window = 3s;
    constexpr auto initial_network_evidence_max_age = 2s;
    constexpr std::size_t max_terminal_actions_per_generation = 128;

    struct action_run_t {
      bool active = false;
      action_kind_e kind = action_kind_e::none;
      std::string run_id;
      std::string request_id;
      std::string owner_uuid;
      std::string app_uuid;
      std::string launch_instance_id;
      std::uint64_t session_generation = 0;
      adaptive_bitrate::doctor_state_t previous_controller;
      std::uint64_t controller_revision = 0;
      int applied_bitrate_kbps = 0;
      int goal_bitrate_kbps = 0;
      // pyrowave_advice, launch_ceiling or launch_bitrate for a quality restore.
      std::string goal_source;
      std::uint64_t verification_step = 0;
      std::uint64_t network_sample_revision_at_apply = 0;
      bool requires_media_sample = false;
      // The step turned Live Tuning off for this stream, as a player's own live bitrate does. Undo and a
      // rollback turn it back on with the rate.
      bool paused_live_tuning = false;
      bool verification_passed = false;
      stream_stats::network_verification_window_t verified_window;
      std::chrono::steady_clock::time_point requested_at {};
      std::chrono::steady_clock::time_point applied_at {};
    };

    std::mutex action_mutex;
    action_run_t action_run;
    struct terminal_action_t {
      nlohmann::json result;
      std::string run_id;
      std::string request_id;
      std::string owner_uuid;
      std::string app_uuid;
      std::string launch_instance_id;
      std::uint64_t session_generation = 0;
    };
    terminal_action_t terminal_action;
    std::deque<terminal_action_t> terminal_action_history;
    std::atomic<std::uint64_t> run_sequence {0};

    struct session_scope_t {
      std::string owner_uuid;
      std::uint64_t session_generation = 0;
      std::string launch_instance_id;
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
        context->owner_uuid == run.owner_uuid && context->app_uuid == run.app_uuid &&
        context->launch_instance_id == run.launch_instance_id;
    }

    bool valid_live_scope_shape(const recovery_action_context_t *context) {
      if (context == nullptr) return true;
      if (!context->active_owner || !context->host_tuning_allowed ||
          !context->stats.streaming || context->session_generation == 0 ||
          context->owner_uuid.empty() || context->app_uuid.empty() ||
          (context->enforce_request_scope && context->launch_instance_id.empty())) {
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
        controller_sessions.front().session_generation == context->session_generation &&
        controller_sessions.front().launch_instance_id == context->launch_instance_id;
    }

    bool valid_live_scope(const recovery_action_context_t *context) {
      std::lock_guard<std::mutex> lock(action_mutex);
      return valid_live_scope_locked(context);
    }

    bool valid_observation_scope_locked(const recovery_action_context_t *context) {
      if (context == nullptr) return true;
      if (!context->active_owner || !context->stats.streaming ||
          context->session_generation == 0 || context->owner_uuid.empty() ||
          context->app_uuid.empty() ||
          (context->enforce_request_scope && context->launch_instance_id.empty())) {
        return false;
      }
      return std::any_of(
        controller_sessions.begin(),
        controller_sessions.end(),
        [context](const session_scope_t &scope) {
          return scope.owner_uuid == context->owner_uuid &&
            scope.session_generation == context->session_generation &&
            scope.launch_instance_id == context->launch_instance_id;
        }
      );
    }

    bool valid_observation_scope(const recovery_action_context_t *context) {
      std::lock_guard<std::mutex> lock(action_mutex);
      return valid_observation_scope_locked(context);
    }

    nlohmann::json action_in_progress(const action_run_t &run) {
      std::string error = "Finish or undo the active same-stream Doctor change before starting another.";
      if (run.kind == action_kind_e::lower_bitrate) {
        // Doctor takes one step at a time. Say which step holds, whether it is verified, and what Undo
        // puts back.
        error = std::string {"Doctor already lowered this stream's bitrate one step"} +
          (run.paused_live_tuning ? " and turned Live Tuning off for it" : "") +
          ", and takes one step at a time. " +
          (run.verification_passed ? "That step is verified and stays until you undo it." : "Doctor is still checking that step.") +
          " Undo puts the bitrate back" + (run.paused_live_tuning ? " and turns Live Tuning on again." : ".");
      }
      return {
        {"status", false}, {"changed", false}, {"state", "action_in_progress"},
        {"run_id", run.run_id},
        {"request_id", run.request_id},
        {"error", error}
      };
    }

    enum class restore_status_e {
      superseded,
      restored,
      encoder_unconfirmed
    };

    struct restore_outcome_t {
      restore_status_e status = restore_status_e::superseded;
      int bitrate_kbps = 0;
    };

    restore_outcome_t restore_bitrate_run_locked(action_run_t &run) {
      const int restore_live_bitrate_kbps = run.previous_controller.live_bitrate_kbps > 0 ?
        run.previous_controller.live_bitrate_kbps : run.previous_controller.base_bitrate_kbps;
      auto expected_revision = run.controller_revision;
      if (run.paused_live_tuning && adaptive_bitrate::get_doctor_state().revision == run.controller_revision) {
        // Live Tuning back on for this stream, then the rate, while the step still owns the controller.
        // A newer writer's choice stands, Live Tuning's state included.
        adaptive_bitrate::end_stream_override();
        expected_revision = adaptive_bitrate::get_doctor_state().revision;
      }
      const auto restored_revision = adaptive_bitrate::restore_doctor_state_if_revision(
        expected_revision,
        run.previous_controller
      );
      run.active = false;
      if (!restored_revision) {
        return {};
      }
      const bool encoder_restored = restore_live_bitrate_kbps <= 0 ||
        adaptive_bitrate::wait_for_live_bitrate_applied(
          *restored_revision,
          restore_live_bitrate_kbps,
          std::chrono::duration_cast<std::chrono::milliseconds>(encoder_rollback_timeout)
        );
      return {
        encoder_restored ? restore_status_e::restored : restore_status_e::encoder_unconfirmed,
        restore_live_bitrate_kbps
      };
    }

#ifdef POLARIS_TESTS
    std::function<void(std::string_view)> live_tuning_step_hook;
#endif

    // Where a newer controller writer can meet Doctor's step with Live Tuning off. A unit test puts a
    // writer there; otherwise nothing runs.
    void live_tuning_step_point([[maybe_unused]] std::string_view stage) {
#ifdef POLARIS_TESTS
      if (live_tuning_step_hook) {
        live_tuning_step_hook(stage);
      }
#endif
    }

    /// What a step with Live Tuning on came to.
    struct live_tuning_step_t {
      /// The step's controller revision, once it is taken.
      std::optional<std::uint64_t> revision;
      /// The step was not taken after Live Tuning was turned off for it, and Live Tuning is back on.
      bool live_tuning_back_on = false;
      /// The rate from before the step is back too.
      bool rate_restored = false;
    };

    // A step with Live Tuning on turns it off for this stream only, the rule a player's own live
    // bitrate follows (set_owner_live_bitrate()), and steps down from there. Nothing is saved:
    // restore_bitrate_run_locked() turns it back on with the rate, and session_ended() puts the saved
    // preference back for the next stream.
    live_tuning_step_t step_with_live_tuning_off(const adaptive_bitrate::doctor_state_t &before,
                                                 int current_kbps,
                                                 int target_kbps) {
      // The check set_doctor_bitrate_if_revision() makes, before anything moves.
      if (adaptive_bitrate::get_doctor_state().revision != before.revision) {
        return {};
      }
      adaptive_bitrate::set_live_bitrate_for_stream(current_kbps);
      const auto paused = adaptive_bitrate::get_doctor_state();
      live_tuning_step_point("paused");
      if (const auto revision = adaptive_bitrate::set_doctor_bitrate_if_revision(paused.revision, target_kbps)) {
        return {revision};
      }
      // A newer writer, most often a network report, moved the controller between the two, so the step
      // is not taken. Put back the rate from before while Live Tuning's feedback is still held, then
      // turn Live Tuning back on for this stream whether or not that won: nothing it was turned off
      // for happened.
      live_tuning_step_t result;
      for (int attempt = 0; attempt < 3 && !result.rate_restored; ++attempt) {
        const auto expected = adaptive_bitrate::get_doctor_state().revision;
        live_tuning_step_point("restoring");
        result.rate_restored = adaptive_bitrate::restore_doctor_state_if_revision(expected, before).has_value();
      }
      adaptive_bitrate::end_stream_override();
      result.live_tuning_back_on = adaptive_bitrate::is_enabled();
      return result;
    }

    bool encoder_application_confirmed_locked(action_run_t &run) {
      const auto applied_at = adaptive_bitrate::live_bitrate_applied_at(
        run.controller_revision,
        run.applied_bitrate_kbps
      );
      if (!applied_at) return false;
      if (run.applied_at == std::chrono::steady_clock::time_point {}) {
        run.applied_at = *applied_at;
      }
      return true;
    }

    nlohmann::json rollback_unconfirmed_result(
        const std::string &run_id,
        int restore_bitrate_kbps) {
      return {
        {"status", false},
        {"changed", true},
        {"state", "rollback_unconfirmed"},
        {"run_id", run_id},
        {"requested_restore_bitrate_kbps", restore_bitrate_kbps},
        {"error", "Doctor restored the controller target but did not receive encoder confirmation; the prior bitrate has not been reported as restored."},
        {"undo", {{"available", false}}}
      };
    }

    void remember_terminal_locked(const action_run_t &run, nlohmann::json result) {
      // A step that turned Live Tuning off for the stream and was put back turned it on again with the
      // rate, and its receipt says so.
      if (run.paused_live_tuning && result.value("state", std::string {}) == "rolled_back" &&
          !result.contains("adaptive_bitrate_enabled")) {
        result["message"] = result.value("message", std::string {}) + " Live Tuning is back on for this stream.";
        result["adaptive_bitrate_enabled"] = true;
      }
      result["request_id"] = run.request_id;
      if (!terminal_action.run_id.empty()) {
        terminal_action_history.push_back(terminal_action);
      }
      terminal_action = {
        std::move(result), run.run_id, run.request_id, run.owner_uuid, run.app_uuid,
        run.launch_instance_id, run.session_generation
      };
    }

    bool matches_terminal_scope_locked(const terminal_action_t &terminal,
                                       const recovery_action_context_t *context) {
      if (terminal.session_generation == 0) return context == nullptr;
      return context != nullptr &&
        context->session_generation == terminal.session_generation &&
        context->owner_uuid == terminal.owner_uuid &&
        context->app_uuid == terminal.app_uuid &&
        context->launch_instance_id == terminal.launch_instance_id;
    }

    terminal_action_t *find_terminal_by_request_locked(std::string_view request_id) {
      if (!request_id.empty() && terminal_action.request_id == request_id) {
        return &terminal_action;
      }
      auto it = std::find_if(
        terminal_action_history.rbegin(), terminal_action_history.rend(),
        [request_id](const terminal_action_t &terminal) {
          return terminal.request_id == request_id;
        }
      );
      return it == terminal_action_history.rend() ? nullptr : &*it;
    }

    terminal_action_t *find_terminal_by_run_locked(std::string_view run_id) {
      if (!run_id.empty() && terminal_action.run_id == run_id) {
        return &terminal_action;
      }
      auto it = std::find_if(
        terminal_action_history.rbegin(), terminal_action_history.rend(),
        [run_id](const terminal_action_t &terminal) {
          return terminal.run_id == run_id;
        }
      );
      return it == terminal_action_history.rend() ? nullptr : &*it;
    }

    nlohmann::json superseded_result(const std::string &run_id) {
      return {
        {"status", true},
        {"changed", false},
        {"state", "superseded"},
        {"run_id", run_id},
        {"message", "A newer explicit bitrate or controller change superseded this Doctor receipt; Doctor left the newer choice untouched."},
        {"undo", {{"available", false}}}
      };
    }

    void set_adaptive_enabled_locked(bool enable) {
      if (action_run.active && !enable) {
        remember_terminal_locked(action_run, superseded_result(action_run.run_id));
        action_run = action_run_t {};
      } else if (action_run.active) {
        const auto run_snapshot = action_run;
        const auto outcome = restore_bitrate_run_locked(action_run);
        if (outcome.status == restore_status_e::encoder_unconfirmed) {
          remember_terminal_locked(
            run_snapshot,
            rollback_unconfirmed_result(run_snapshot.run_id, outcome.bitrate_kbps)
          );
        } else if (outcome.status == restore_status_e::restored && run_snapshot.paused_live_tuning) {
          // Doctor's step turned Live Tuning off for this stream, and the player turned it back on, which
          // ends the step: the rate from before it is back with Live Tuning. No newer choice superseded
          // Doctor here.
          remember_terminal_locked(run_snapshot, {
            {"status", true}, {"changed", true}, {"state", "rolled_back"},
            {"run_id", run_snapshot.run_id},
            {"message", "You turned Live Tuning back on, so Doctor put back the bitrate from before its step and ended it."},
            {"restored_bitrate_kbps", outcome.bitrate_kbps},
            {"adaptive_bitrate_enabled", true},
            {"undo", {{"available", false}}}
          });
        } else {
          remember_terminal_locked(run_snapshot, superseded_result(run_snapshot.run_id));
        }
        action_run = action_run_t {};
      }
      adaptive_bitrate::set_enabled(enable);
    }

    nlohmann::json idempotent_active_receipt_locked(action_run_t &run) {
      const bool encoder_confirmed = encoder_application_confirmed_locked(run);
      return {
        {"status", true}, {"changed", false},
        {"state", encoder_confirmed ? "watching" : "applying"},
        {"run_id", run.run_id},
        {"request_id", run.request_id},
        {"message", encoder_confirmed ?
          "Doctor is collecting the required post-change evidence window." :
          "Doctor is waiting for the live encoder to acknowledge the requested bitrate."},
        {"encoder_application_confirmed", encoder_confirmed},
        {"verification", {{"delay_seconds", 8}, {"action_id", "verify"}, {"run_id", run.run_id}}},
        {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run.run_id}}}
      };
    }

    bool request_uint64_matches(const nlohmann::json &request,
                                std::string_view key,
                                std::uint64_t expected) {
      const auto it = request.find(key);
      if (it == request.end() || !it->is_number_integer()) return false;
      try {
        if (it->is_number_unsigned()) return it->get<std::uint64_t>() == expected;
        const auto value = it->get<std::int64_t>();
        return value > 0 && static_cast<std::uint64_t>(value) == expected;
      } catch (...) {
        return false;
      }
    }

    nlohmann::json invalid_request_scope(const nlohmann::json &request,
                                         const recovery_action_context_t *context) {
      if (context == nullptr || !context->enforce_request_scope) return nullptr;
      const auto token = request.find("app_session_id");
      if (token == request.end() || !token->is_string() ||
          token->get_ref<const std::string &>().empty() ||
          token->get_ref<const std::string &>().size() > 2048 ||
          token->get_ref<const std::string &>() != context->launch_instance_id ||
          !request_uint64_matches(request, "session_generation", context->session_generation)) {
        return {
          {"status", false}, {"changed", false}, {"state", "scope_mismatch"},
          {"code", "stale_stream_generation"},
          {"error", "This Doctor request was not issued for the exact active app and stream generation."}
        };
      }
      return nullptr;
    }

    void bind_current_action_scope(nlohmann::json &doctor,
                                   const stream_stats::stats_t &stats,
                                   const recovery_action_context_t *context,
                                   std::uint64_t controller_revision) {
      if (context == nullptr) return;
      stream_stats::bind_doctor_action_scope(
        doctor,
        context->launch_instance_id,
        context->session_generation,
        controller_revision,
        stats.network_sample_revision,
        stats.video_sample_revision
      );
    }

    bool deterministic_action_envelope_matches(
        const nlohmann::json &request,
        std::string_view action_id,
        const stream_stats::stats_t &stats,
        const recovery_action_context_t *context,
        const adaptive_bitrate::doctor_state_t &controller,
        nlohmann::json *current_doctor_out = nullptr) {
      if (context == nullptr) return true;
      const auto trusted_health = context->health.is_object() ?
        context->health : nlohmann::json::object();
      auto current_doctor = stream_stats::build_doctor_json(
        stats, trusted_health, context->app_uuid
      );
      bind_current_action_scope(
        current_doctor, stats, context, controller.action_authority_revision
      );
      if (current_doctor_out != nullptr) *current_doctor_out = current_doctor;
      const auto current_action = current_doctor.value(
        "safe_recovery_action", nlohmann::json::object()
      );
      const auto expected_payload = current_action.value(
        "payload_preview", nlohmann::json::object()
      );
      if (current_action.value("id", std::string {}) != action_id ||
          !expected_payload.is_object()) {
        return false;
      }
      const bool live_mutation =
        action_id == "lower_bitrate" || action_id == "restore_quality";
      const bool read_only_recheck =
        action_id == "recheck_network" || action_id == "recheck_pacing";
      for (auto it = expected_payload.begin(); it != expected_payload.end(); ++it) {
        // These fields identify the telemetry snapshot that rendered the
        // button, not user authority. A paired click is allowed to cross a
        // newer equivalent observation only when the current host still
        // derives the same action and stream scope. Mutations also recheck the
        // target and controller authority below, then use a fresh internal
        // controller revision atomically; Recheck never mutates the actuator.
        if ((live_mutation || read_only_recheck) &&
            (it.key() == "source_result_id" ||
             it.key() == "evidence_revision")) {
          continue;
        }
        const auto actual = request.find(it.key());
        if (actual == request.end() || *actual != it.value()) return false;
      }
      return true;
    }

    nlohmann::json bind_result_scope(nlohmann::json result,
                                     const recovery_action_context_t &context) {
      if (!result.is_object() || context.launch_instance_id.empty() ||
          context.session_generation == 0) {
        return result;
      }
      result["app_session_id"] = context.launch_instance_id;
      result["session_generation"] = context.session_generation;
      for (const auto *key : {"verification", "undo"}) {
        auto nested = result.find(key);
        if (nested != result.end() && nested->is_object()) {
          (*nested)["app_session_id"] = context.launch_instance_id;
          (*nested)["session_generation"] = context.session_generation;
        }
      }
      return result;
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
        {"last_received_age_ms", stats.network_last_received_age_ms},
        {"media_loss_sample_revision", stats.media_loss_sample_revision},
        {"media_loss_last_received_age_ms", stats.media_loss_last_received_age_ms},
        {"latency_ms", stats.latency_ms},
        {"network_verdict", stream_stats::network_verdict_json(stream_stats::served_network_verdict(stats))},
        {"bitrate_kbps", current_live_bitrate(stats)},
        {"paired_target_bitrate_kbps", stats.paired_target_bitrate_kbps},
        {"effective_launch_bitrate_kbps", stats.effective_launch_bitrate_kbps},
        {"optimization_source", stats.optimization_source}
      };
    }

    // The judged verdict Doctor's headline reads, so a restore it offers is one this accepts, and one it
    // verifies is judged the way it was offered.
    bool network_verdict_clear_for_quality(const stream_stats::stats_t &stats) {
      const auto verdict = stream_stats::served_network_verdict(stats);
      const auto network = stream_stats::judged_network(stats);
      return stats.streaming && (network.loss_judged || network.rtt_judged) && !network.risk &&
        (!network.loss_judged || verdict.loss_pct < stream_stats::network_judge_t::k_loss_enter_pct) &&
        (!network.rtt_judged || verdict.rtt_ms < stream_stats::network_judge_t::k_rtt_fail_ms);
    }

    bool network_stable_for_quality_retry(const stream_stats::stats_t &stats) {
      return network_verdict_clear_for_quality(stats) && !adaptive_bitrate::doctor_policy_blocks_quality_restore();
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
        const action_run_t &run,
        const stream_stats::stats_t &stats) {
      const bool required_media_arrived =
        !run.requires_media_sample || window.media_sample_count > 0;
      const bool restoring_quality = run.kind == action_kind_e::restore_quality;
      if (restoring_quality) {
        // A restore is verified on the window verdict that offered it, and on the network latch, which
        // reads that verdict at every reading since the step. Held to the newest readings instead, the
        // Retroid Pocket 6's light HEVC loss, a 7.4% report every few seconds in a window under 2%, rolled
        // back almost every restore Doctor had offered on it.
        return window.complete && required_media_arrived &&
          !adaptive_bitrate::doctor_policy_blocks_quality_restore() &&
          network_verdict_clear_for_quality(stats);
      }
      // A step for network pressure is verified against the verdict Doctor's headline will read once
      // it verifies: the window judged afresh from the moment the encoder applied the step, counting
      // only media reports whose second began after it. The first report after the step covers the
      // second before it. With enough readings the two cannot disagree. A step that verifies starts the
      // headline's judgement over from the same moment, and a step whose own readings are still
      // pressure rolls back, as the headline would have it. A step taken for loss needs its loss
      // judged; until then the newest readings decide, as they did.
      auto post_step = stats;
      post_step.network_verdict = stream_stats::network_verdict_since(run.applied_at);
      const auto network = stream_stats::judged_network(post_step);
      const bool judged = run.requires_media_sample ? network.loss_judged : network.loss_judged || network.rtt_judged;
      if (judged) {
        return window.complete && required_media_arrived && !network.risk;
      }
      return window.complete && required_media_arrived &&
        !window.network_risk &&
        (!window.packet_loss_available || window.packet_loss <= 2.0) &&
        window.latency_ms < 45.0;
    }

    // A verified step for network pressure hands Doctor's headline the judgement verification read:
    // the window from the moment the encoder applied the step. Judged over a window that still held
    // the readings that asked for the step, a verified step left "Sustained network pressure" and
    // another lower_bitrate on offer for most of 20 seconds. A step that rolls back leaves the
    // judgement as it was, since its readings are why it rolled back.
    void mark_verified_locked(action_run_t &run) {
      if (!run.verification_passed && run.kind == action_kind_e::lower_bitrate) {
        stream_stats::restart_network_judgement(run.applied_at);
      }
      run.verification_passed = true;
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
      const auto doctor_controller = adaptive_bitrate::get_doctor_state();
      if (doctor_controller.revision != action_run.controller_revision) {
        const auto run_snapshot = action_run;
        action_run = action_run_t {};
        remember_terminal_locked(run_snapshot, superseded_result(run_id));
        BOOST_LOG(info) << "Doctor: verification receipt was superseded by a newer explicit controller change run="sv
                        << run_id;
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      if (!encoder_application_confirmed_locked(action_run)) {
        if (now - action_run.requested_at < encoder_apply_timeout) {
          task_pool.pushDelayed(
            [run_id, verification_step, owner_uuid, session_generation]() {
              run_verification_watchdog(
                run_id, verification_step, owner_uuid, session_generation
              );
            },
            250ms
          );
          return;
        }
        const auto run_snapshot = action_run;
        const auto outcome = restore_bitrate_run_locked(action_run);
        if (outcome.status == restore_status_e::restored) {
          auto result = nlohmann::json {
            {"status", true}, {"changed", true}, {"state", "rolled_back"},
            {"run_id", run_id},
            {"message", "The encoder did not confirm Doctor's requested bitrate, so the prior target was restored."},
            {"restored_bitrate_kbps", outcome.bitrate_kbps},
            {"undo", {{"available", false}}}
          };
          remember_terminal_locked(run_snapshot, result);
          BOOST_LOG(warning) << "Doctor: encoder application timed out; restored "sv
                             << outcome.bitrate_kbps << " kbps run=" << run_id;
        } else if (outcome.status == restore_status_e::encoder_unconfirmed) {
          auto result = rollback_unconfirmed_result(run_id, outcome.bitrate_kbps);
          remember_terminal_locked(run_snapshot, result);
          BOOST_LOG(error) << "Doctor: encoder application and rollback acknowledgement both timed out run="sv
                           << run_id;
        } else {
          remember_terminal_locked(run_snapshot, superseded_result(run_id));
        }
        return;
      }
      const auto post_apply_elapsed = now - action_run.applied_at;
      if (post_apply_elapsed < verification_delay) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          verification_delay - post_apply_elapsed
        );
        task_pool.pushDelayed(
          [run_id, verification_step, owner_uuid, session_generation]() {
            run_verification_watchdog(
              run_id, verification_step, owner_uuid, session_generation
            );
          },
          std::max(remaining, 50ms)
        );
        return;
      }
      const auto window = stream_stats::get_network_verification_window(
        action_run.network_sample_revision_at_apply,
        action_run.applied_at,
        verification_delay
      );
      const bool verification_passed =
        adaptive_state.runtime_update_supported &&
        verification_window_stable(window, action_run, stats);
      if (!verification_passed) {
        const auto run_snapshot = action_run;
        const auto outcome = restore_bitrate_run_locked(action_run);
        if (outcome.status == restore_status_e::restored) {
          auto result = nlohmann::json {
            {"status", true}, {"changed", true}, {"state", "rolled_back"},
            {"run_id", run_id},
            {"message", "Verification did not receive a complete clean post-change window, so Doctor restored the prior live target."},
            {"restored_bitrate_kbps", outcome.bitrate_kbps},
            {"verification_window", verification_window_json(window)},
            {"undo", {{"available", false}}}
          };
          remember_terminal_locked(run_snapshot, result);
          BOOST_LOG(warning) << "Doctor: automatic verification failed; restored "sv
                             << outcome.bitrate_kbps << " kbps run=" << run_id;
        } else if (outcome.status == restore_status_e::encoder_unconfirmed) {
          auto result = rollback_unconfirmed_result(run_id, outcome.bitrate_kbps);
          result["verification_window"] = verification_window_json(window);
          remember_terminal_locked(run_snapshot, result);
          BOOST_LOG(error) << "Doctor: controller rollback was not acknowledged by the encoder run="sv
                           << run_id;
        } else {
          remember_terminal_locked(run_snapshot, superseded_result(run_id));
          BOOST_LOG(info) << "Doctor: automatic verification was superseded; left the newer controller target untouched run="sv
                          << run_id;
        }
        return;
      }

      mark_verified_locked(action_run);
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
    // The judged verdict Doctor's headline reads, so the step it offers is the step this takes.
    return stats.streaming && stream_stats::judged_network(stats).fail;
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

  paired_global_control_guard_t acquire_paired_global_control(
      std::string_view owner_uuid,
      std::uint64_t session_generation,
      std::string_view launch_instance_id) {
    std::unique_lock<std::mutex> lock(action_mutex);
    const bool authorized = controller_sessions.empty() ?
      session_generation == 0 && launch_instance_id.empty() :
      (controller_sessions.size() == 1 &&
       session_generation > 0 && !launch_instance_id.empty() &&
       controller_sessions.front().owner_uuid == owner_uuid &&
       controller_sessions.front().session_generation == session_generation &&
       controller_sessions.front().launch_instance_id == launch_instance_id);
    if (!authorized) lock.unlock();
    return {std::move(lock), authorized};
  }

  bool paired_global_control_guard_t::set_adaptive_enabled(bool enable) {
    if (!authorized_ || !lock_.owns_lock()) return false;
    set_adaptive_enabled_locked(enable);
    return true;
  }

  paired_global_control_guard_t acquire_admin_global_control() {
    return {std::unique_lock<std::mutex>(action_mutex), true};
  }

  bool set_owner_live_bitrate(std::string_view owner_uuid,
                              std::uint64_t session_generation,
                              std::string_view launch_instance_id,
                              int bitrate_kbps) {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (controller_sessions.size() != 1 ||
        session_generation == 0 || launch_instance_id.empty() ||
        controller_sessions.front().owner_uuid != owner_uuid ||
        controller_sessions.front().session_generation != session_generation ||
        controller_sessions.front().launch_instance_id != launch_instance_id) {
      return false;
    }
    // A player who picks a bitrate by hand has taken it from Live Tuning for this stream, not for
    // every stream after it. Nothing is saved; session_ended() puts the saved preference back.
    const auto superseded_run = action_run;
    adaptive_bitrate::set_live_bitrate_for_stream(bitrate_kbps);
    if (superseded_run.active) {
      remember_terminal_locked(
        superseded_run,
        superseded_result(superseded_run.run_id)
      );
      action_run = action_run_t {};
    }
    return true;
  }

  void set_adaptive_enabled(bool enable) {
    std::lock_guard<std::mutex> lock(action_mutex);
    set_adaptive_enabled_locked(enable);
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

    if (action_id == "undo" || action_id == "verify" ||
        action_id == "recheck_network" || action_id == "recheck_pacing" ||
        action_id == "lower_bitrate" || action_id == "restore_quality") {
      if (auto invalid_scope = invalid_request_scope(request, trusted_context);
          !invalid_scope.is_null()) {
        return invalid_scope;
      }
    }

    if (action_id == "undo") {
      const auto run_id = request.value("run_id", std::string {});
      std::lock_guard<std::mutex> lock(action_mutex);
      if (action_run.active && !matches_live_scope(action_run, trusted_context)) {
        return {
          {"status", false}, {"changed", false}, {"state", "scope_mismatch"},
          {"error", "This Doctor change belongs to a different active stream owner or generation."}
        };
      }
      if (!action_run.active && !run_id.empty()) {
        if (auto *terminal = find_terminal_by_run_locked(run_id)) {
          if (!matches_terminal_scope_locked(*terminal, trusted_context)) {
            return {
              {"status", false}, {"changed", false}, {"state", "scope_mismatch"},
              {"error", "This Doctor undo receipt belongs to a different stream scope."}
            };
          }
          const auto terminal_state = terminal->result.value("state", std::string {});
          if (terminal_state == "undone" || terminal_state == "superseded" ||
              terminal_state == "rolled_back" ||
              terminal_state == "rollback_unconfirmed") {
            return terminal->result;
          }
        }
      }
      if (!action_run.active || run_id.empty() || run_id != action_run.run_id) {
        return {{"status", false}, {"changed", false}, {"error", "This Doctor undo is no longer available."}};
      }

      const bool previous_adaptive_enabled = action_run.previous_controller.enabled;
      const auto run_snapshot = action_run;
      const auto outcome = restore_bitrate_run_locked(action_run);
      if (outcome.status == restore_status_e::superseded) {
        auto result = superseded_result(run_id);
        remember_terminal_locked(run_snapshot, result);
        return terminal_action.result;
      }
      if (outcome.status == restore_status_e::encoder_unconfirmed) {
        auto result = rollback_unconfirmed_result(run_id, outcome.bitrate_kbps);
        remember_terminal_locked(run_snapshot, result);
        return terminal_action.result;
      }
      auto result = nlohmann::json {
        {"status", true},
        {"changed", true},
        {"state", "undone"},
        {"run_id", run_id},
        {"restored_bitrate_kbps", outcome.bitrate_kbps},
        {"adaptive_bitrate_enabled", previous_adaptive_enabled},
        {"undo", {{"available", false}}}
      };
      remember_terminal_locked(run_snapshot, result);
      return terminal_action.result;
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
        (action_id == "lower_bitrate" || action_id == "restore_quality")) {
      if (!request.contains("request_id") || !request["request_id"].is_string()) {
        return {
          {"status", false}, {"changed", false}, {"state", "invalid_request_id"},
          {"error", "Doctor Auto Fix requires a typed idempotency request_id."}
        };
      }
      const auto request_id = request["request_id"].get<std::string>();
      if (request_id.empty() || request_id.size() > 128) {
        return {
          {"status", false}, {"changed", false}, {"state", "invalid_request_id"},
          {"error", "Doctor request_id must contain 1 to 128 characters."}
        };
      }
      std::lock_guard<std::mutex> lock(action_mutex);
      if (action_run.active && action_run.request_id == request_id) {
        if (!matches_live_scope(action_run, trusted_context)) {
          return {
            {"status", false}, {"changed", false}, {"state", "scope_mismatch"},
            {"error", "This idempotent Doctor request belongs to another stream scope."}
          };
        }
        return idempotent_active_receipt_locked(action_run);
      }
      if (auto *terminal = find_terminal_by_request_locked(request_id)) {
        if (!matches_terminal_scope_locked(*terminal, trusted_context)) {
          return {
            {"status", false}, {"changed", false}, {"state", "scope_mismatch"},
            {"error", "This idempotent Doctor terminal result belongs to another stream scope."}
          };
        }
        return terminal->result;
      }
      const auto terminal_count = terminal_action_history.size() +
        (terminal_action.run_id.empty() ? 0u : 1u);
      if (terminal_count >= max_terminal_actions_per_generation) {
        return {
          {"status", false}, {"changed", false},
          {"state", "generation_action_limit"},
          {"code", "doctor_idempotency_capacity_reached"},
          {"error", "This stream generation has reached its bounded Doctor action limit; reconnect before starting another change."}
        };
      }
    }

    if (trusted_context != nullptr &&
        (action_id == "recheck_network" || action_id == "recheck_pacing" ||
         action_id == "lower_bitrate" || action_id == "restore_quality")) {
      const bool recheck = action_id == "recheck_network" || action_id == "recheck_pacing";
      const bool scope_valid = recheck ?
        valid_observation_scope(trusted_context) : valid_live_scope(trusted_context);
      if (!scope_valid) {
        return {
          {"status", false}, {"changed", false}, {"state", "scope_unavailable"},
          {"error", recheck ?
            "A complete active stream scope is required for this read-only Doctor recheck." :
            "A complete, unshared active stream scope is required for this Doctor Auto Fix."}
        };
      }

      // The paired client echoes the typed payload that Polaris most recently
      // described; it never supplies action policy. Re-derive that payload
      // from the trusted current host snapshot and reject stale, malformed, or
      // client-invented settings before touching the live encoder.
      nlohmann::json scoped_current_doctor;
      const auto controller = adaptive_bitrate::get_doctor_state();
      const bool envelope_matches = deterministic_action_envelope_matches(
        request, action_id, stats, trusted_context, controller, &scoped_current_doctor
      );
      if (!envelope_matches) {
        return {
          {"status", false}, {"changed", false}, {"state", "evidence_changed"},
          {"code", "stale_action_envelope"},
          {"error", "Doctor evidence or its deterministic action changed; recheck before applying it."},
          {"doctor", scoped_current_doctor},
          {"evidence", evidence}
        };
      }
    }

    if (action_id == "recheck_network" || action_id == "recheck_pacing") {
      const bool pacing_recheck = action_id == "recheck_pacing";
      const auto initial_revision = pacing_recheck ?
        stats.video_sample_revision : stats.network_sample_revision;
      std::this_thread::sleep_for(recheck_window);
      const auto refreshed_stats = stream_stats::get_current();
      const auto refreshed_revision = pacing_recheck ?
        refreshed_stats.video_sample_revision : refreshed_stats.network_sample_revision;
      if (!refreshed_stats.streaming || refreshed_revision <= initial_revision ||
          !valid_observation_scope(trusted_context)) {
        return {
          {"status", false},
          {"changed", false},
          {"state", "no_fresh_sample"},
          {"message", "Doctor did not receive a fresh matching telemetry sample during the read-only recheck window."},
          {"evidence", network_evidence(refreshed_stats)}
        };
      }
      const bool confirmed = network_pressure_confirmed(refreshed_stats);
      const auto refreshed_health = trusted_context != nullptr && trusted_context->health.is_object() ?
        trusted_context->health : nlohmann::json::object();
      const auto refreshed_doctor = stream_stats::build_doctor_json(
        refreshed_stats,
        refreshed_health,
        trusted_context != nullptr ? trusted_context->app_uuid : std::string {}
      );
      return {
        {"status", true},
        {"changed", false},
        {"state", action_id == "recheck_pacing" ? "observed" : confirmed ? "confirmed_pressure" : "stable"},
        {"message", pacing_recheck ?
          "Collected a fresh read-only pacing observation; no launch or game-process settings were changed." : confirmed ?
          "Current telemetry now confirms network pressure. Doctor can safely apply one bitrate step." :
          "Current loss and latency do not justify reducing bitrate."},
        {"evidence", network_evidence(refreshed_stats)},
        {"doctor", refreshed_doctor}
      };
    }

    if (action_id == "verify") {
      const auto run_id = request.value("run_id", std::string {});
      std::lock_guard<std::mutex> lock(action_mutex);
      if (!action_run.active && !run_id.empty()) {
        auto *terminal = find_terminal_by_run_locked(run_id);
        if (terminal && !matches_terminal_scope_locked(*terminal, trusted_context)) {
          return {
            {"status", false}, {"changed", false}, {"state", "scope_mismatch"},
            {"error", "This Doctor terminal receipt belongs to a different stream owner or generation."}
          };
        }
        if (terminal) return terminal->result;
      }
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
      const auto doctor_controller = adaptive_bitrate::get_doctor_state();
      if (doctor_controller.revision != action_run.controller_revision) {
        const auto run_snapshot = action_run;
        action_run = action_run_t {};
        auto result = superseded_result(run_id);
        remember_terminal_locked(run_snapshot, result);
        return terminal_action.result;
      }
      if (!doctor_controller.runtime_update_supported) {
        const auto run_snapshot = action_run;
        const auto outcome = restore_bitrate_run_locked(action_run);
        if (outcome.status == restore_status_e::superseded) {
          auto result = superseded_result(run_id);
          remember_terminal_locked(run_snapshot, result);
          return terminal_action.result;
        }
        if (outcome.status == restore_status_e::encoder_unconfirmed) {
          auto result = rollback_unconfirmed_result(run_id, outcome.bitrate_kbps);
          remember_terminal_locked(run_snapshot, result);
          return terminal_action.result;
        }
        auto result = nlohmann::json {
          {"status", true},
          {"changed", true},
          {"run_id", run_id},
          {"state", "rolled_back"},
          {"message", "Live encoder updates became unavailable during verification, so Doctor restored the prior target and ended the fix."},
          {"restored_bitrate_kbps", outcome.bitrate_kbps},
          {"evidence", verification_evidence},
          {"undo", {{"available", false}}}
        };
        remember_terminal_locked(run_snapshot, result);
        return terminal_action.result;
      }
      const auto now = std::chrono::steady_clock::now();
      if (!encoder_application_confirmed_locked(action_run)) {
        const auto requested_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          now - action_run.requested_at
        );
        if (requested_elapsed < encoder_apply_timeout) {
          return {
            {"status", true}, {"changed", false}, {"run_id", run_id},
            {"state", "applying"},
            {"message", "Doctor is waiting for the live encoder to acknowledge the requested bitrate."},
            {"retry_after_seconds", 1},
            {"encoder_application_confirmed", false},
            {"evidence", verification_evidence},
            {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
          };
        }
        const auto run_snapshot = action_run;
        const auto outcome = restore_bitrate_run_locked(action_run);
        if (outcome.status == restore_status_e::superseded) {
          auto result = superseded_result(run_id);
          remember_terminal_locked(run_snapshot, result);
          return terminal_action.result;
        }
        if (outcome.status == restore_status_e::encoder_unconfirmed) {
          auto result = rollback_unconfirmed_result(run_id, outcome.bitrate_kbps);
          remember_terminal_locked(run_snapshot, result);
          return terminal_action.result;
        }
        auto result = nlohmann::json {
          {"status", true}, {"changed", true}, {"run_id", run_id},
          {"state", "rolled_back"},
          {"message", "The encoder did not confirm Doctor's requested bitrate, so the prior target was restored."},
          {"restored_bitrate_kbps", outcome.bitrate_kbps},
          {"encoder_application_confirmed", false},
          {"evidence", verification_evidence},
          {"undo", {{"available", false}}}
        };
        remember_terminal_locked(run_snapshot, result);
        return terminal_action.result;
      }
      const auto current_verification_window = stream_stats::get_network_verification_window(
        action_run.network_sample_revision_at_apply,
        action_run.applied_at,
        verification_delay
      );
      const auto response_verification_window = action_run.verification_passed ?
        action_run.verified_window : current_verification_window;
      const auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(
        now - action_run.applied_at
      ).count();
      if (elapsed_seconds < verification_delay.count()) {
        return {
          {"status", true},
          {"changed", false},
          {"run_id", run_id},
          {"state", "watching"},
          {"message", "Doctor is still collecting the required post-change evidence window."},
          {"encoder_application_confirmed", true},
          {"elapsed_seconds", elapsed_seconds},
          {"retry_after_seconds", verification_delay.count() - elapsed_seconds},
          {"evidence", verification_evidence},
          {"verification_window", verification_window_json(response_verification_window)},
          {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
        };
      }
      const bool current_verification_stable = verification_window_stable(
        current_verification_window,
        action_run,
        verification_stats
      );
      const bool verification_stable = action_run.verification_passed ||
        current_verification_stable;

      if (action_run.kind == action_kind_e::restore_quality) {
        const bool another_mutation_required =
          action_run.applied_bitrate_kbps < action_run.goal_bitrate_kbps;
        const auto rollback_quality_restore = [&](std::string message) {
          const auto run_snapshot = action_run;
          const auto outcome = restore_bitrate_run_locked(action_run);
          if (outcome.status == restore_status_e::superseded) {
            auto result = superseded_result(run_id);
            remember_terminal_locked(run_snapshot, result);
            return terminal_action.result;
          }
          if (outcome.status == restore_status_e::encoder_unconfirmed) {
            auto result = rollback_unconfirmed_result(run_id, outcome.bitrate_kbps);
            remember_terminal_locked(run_snapshot, result);
            return terminal_action.result;
          }
          auto result = nlohmann::json {
            {"status", true},
            {"changed", true},
            {"run_id", run_id},
            {"state", "rolled_back"},
            {"message", std::move(message)},
            {"restored_bitrate_kbps", outcome.bitrate_kbps},
            {"elapsed_seconds", elapsed_seconds},
            {"evidence", verification_evidence},
            {"verification_window", verification_window_json(current_verification_window)},
            {"undo", {{"available", false}}}
          };
          remember_terminal_locked(run_snapshot, result);
          return terminal_action.result;
        };
        // A cached watchdog pass is receipt history, not current authority.
        // Every quality-restoration decision requires a complete, fresh,
        // currently clean host-received window.
        if (!current_verification_stable) {
          return rollback_quality_restore(
            "Verification did not receive enough clean post-change loss, latency, and host video evidence, so Doctor automatically restored the prior live target."
          );
        }

        if (another_mutation_required) {
          const int next_bitrate_kbps = guarded_quality_retry_target(
            action_run.applied_bitrate_kbps,
            action_run.goal_bitrate_kbps
          );
          const auto next_apply = adaptive_bitrate::set_doctor_quality_bitrate_if_revision(
            action_run.controller_revision,
            next_bitrate_kbps,
            action_run.goal_bitrate_kbps
          );
          if (next_apply.status ==
              adaptive_bitrate::doctor_bitrate_apply_status_e::quality_policy_blocked) {
            return rollback_quality_restore(
              "Fresh network, host video, or capture evidence no longer supports a quality increase, so Doctor automatically restored the prior live target."
            );
          }
          if (next_apply.status !=
              adaptive_bitrate::doctor_bitrate_apply_status_e::applied) {
            const auto run_snapshot = action_run;
            action_run = action_run_t {};
            auto result = superseded_result(run_id);
            remember_terminal_locked(run_snapshot, result);
            return terminal_action.result;
          }
          action_run.controller_revision = next_apply.revision;
          action_run.applied_bitrate_kbps = next_bitrate_kbps;
          action_run.requested_at = std::chrono::steady_clock::now();
          action_run.applied_at = {};
          action_run.network_sample_revision_at_apply =
            stream_stats::get_current().network_sample_revision;
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
            {"state", "applying"},
            {"message", "The last quality step stayed stable. Doctor requested one more guarded encoder step."},
            {"requested", {{"bitrate_kbps", next_bitrate_kbps}, {"target_bitrate_kbps", action_run.goal_bitrate_kbps}}},
            {"encoder_application_confirmed", false},
            {"verification", {{"delay_seconds", 8}, {"action_id", "verify"}, {"run_id", run_id}}},
            {"evidence", verification_evidence},
            {"verification_window", verification_window_json(current_verification_window)},
            {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
          };
        }

        return {
          {"status", true},
          {"changed", false},
          {"run_id", run_id},
          {"state", "resolved"},
          {"message", action_run.goal_source == "pyrowave_advice" ?
            "PyroWave's advised bitrate is reached and live network evidence remains stable." :
            "The capability-validated launch bitrate ceiling is restored and live network evidence remains stable."},
          {"goal_source", action_run.goal_source},
          {"elapsed_seconds", elapsed_seconds},
          {"evidence", verification_evidence},
          {"verification_window", verification_window_json(response_verification_window)},
          {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
        };
      }

      const bool resolved = verification_stable;
      if (!resolved) {
        const auto run_snapshot = action_run;
        const auto outcome = restore_bitrate_run_locked(action_run);
        if (outcome.status == restore_status_e::superseded) {
          auto result = superseded_result(run_id);
          remember_terminal_locked(run_snapshot, result);
          return terminal_action.result;
        }
        if (outcome.status == restore_status_e::encoder_unconfirmed) {
          auto result = rollback_unconfirmed_result(run_id, outcome.bitrate_kbps);
          remember_terminal_locked(run_snapshot, result);
          return terminal_action.result;
        }
        auto result = nlohmann::json {
          {"status", true},
          {"changed", true},
          {"run_id", run_id},
          {"state", "rolled_back"},
          {"message", "Verification did not clear the network evidence, so Doctor automatically restored the prior live target."},
          {"restored_bitrate_kbps", outcome.bitrate_kbps},
          {"elapsed_seconds", elapsed_seconds},
          {"evidence", verification_evidence},
          {"verification_window", verification_window_json(current_verification_window)},
          {"undo", {{"available", false}}}
        };
        remember_terminal_locked(run_snapshot, result);
        return terminal_action.result;
      }
      mark_verified_locked(action_run);
      return {
        {"status", true},
        {"changed", false},
        {"run_id", run_id},
        {"state", resolved ? "resolved" : "watching"},
        {"message", "Doctor verified the guarded bitrate step against fresh stream and encoder evidence."},
        {"elapsed_seconds", elapsed_seconds},
        {"evidence", verification_evidence},
        {"verification_window", verification_window_json(response_verification_window)},
        {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
      };
    }

    if (action_id == "restore_quality") {
      action_run_t run;
      adaptive_bitrate::doctor_state_t adaptive_state;
      int current_bitrate_kbps = 0;
      int target_bitrate_kbps = 0;
      int goal_bitrate_kbps = 0;
      stream_stats::doctor_quality_goal_t goal;
      nlohmann::json mutation_evidence;
      {
        std::lock_guard<std::mutex> lock(action_mutex);
        if (!valid_live_scope_locked(trusted_context)) {
          return {
            {"status", false}, {"changed", false}, {"state", "scope_unavailable"},
            {"error", "Doctor Auto Fix requires one fresh, unshared stream generation owning the live bitrate controller."}
          };
        }
        if (action_run.active) return action_in_progress(action_run);
        adaptive_state = adaptive_bitrate::get_doctor_state();
        const auto mutation_stats = stream_stats::get_current();
        mutation_evidence = network_evidence(mutation_stats);
        if (!deterministic_action_envelope_matches(
              request, action_id, mutation_stats, trusted_context, adaptive_state
            )) {
          return {
            {"status", false}, {"changed", false}, {"state", "evidence_changed"},
            {"code", "stale_action_envelope"},
            {"error", "Doctor evidence or controller ownership changed before this quality step could be applied."},
            {"evidence", mutation_evidence}
          };
        }
        current_bitrate_kbps = adaptive_state.live_bitrate_kbps > 0 ?
          adaptive_state.live_bitrate_kbps : current_live_bitrate(mutation_stats);
        // The envelope above already held goal_source to what the host derives now. PyroWave's goal
        // may sit above the player's own request; the launch goal never does.
        const auto source = request.contains("goal_source") && request["goal_source"].is_string() ?
          request["goal_source"].get<std::string>() : std::string {};
        goal = stream_stats::doctor_quality_goal(mutation_stats, source);
        goal_bitrate_kbps = goal.encoder_kbps;
        target_bitrate_kbps = guarded_quality_retry_target(
          current_bitrate_kbps, goal_bitrate_kbps
        );
        if (!network_stable_for_quality_retry(mutation_stats) ||
            goal_bitrate_kbps <= current_bitrate_kbps ||
            target_bitrate_kbps <= current_bitrate_kbps) {
          return {
            {"status", false}, {"changed", false}, {"state", "evidence_changed"},
            {"error", "Current session policy and network evidence no longer authorize a quality retry."},
            {"evidence", mutation_evidence}
          };
        }
        if (!adaptive_state.runtime_update_supported) {
          return {
            {"status", false}, {"changed", false}, {"state", "runtime_update_unavailable"},
            {"error", "The active encoder cannot restore bitrate during a live stream."},
            {"evidence", mutation_evidence}
          };
        }
        run.active = true;
        run.kind = action_kind_e::restore_quality;
        run.run_id = next_run_id();
        run.request_id = request.value("request_id", std::string {});
        if (trusted_context != nullptr) {
          run.owner_uuid = trusted_context->owner_uuid;
          run.app_uuid = trusted_context->app_uuid;
          run.launch_instance_id = trusted_context->launch_instance_id;
          run.session_generation = trusted_context->session_generation;
        }
        run.previous_controller = adaptive_state;
        run.applied_bitrate_kbps = target_bitrate_kbps;
        run.goal_bitrate_kbps = goal_bitrate_kbps;
        run.goal_source = goal.source;
        run.verification_step = 1;
        run.requires_media_sample = false;
        const auto applied = adaptive_bitrate::set_doctor_quality_bitrate_if_revision(
          adaptive_state.revision,
          target_bitrate_kbps,
          goal_bitrate_kbps
        );
        if (applied.status !=
            adaptive_bitrate::doctor_bitrate_apply_status_e::applied) {
          const bool quality_policy_blocked = applied.status ==
            adaptive_bitrate::doctor_bitrate_apply_status_e::quality_policy_blocked;
          return {
            {"status", false}, {"changed", false},
            {"state", quality_policy_blocked ? "evidence_changed" : "controller_changed"},
            {"error", quality_policy_blocked ?
              "Network, host video, or capture evidence changed while Doctor prepared this step. Recheck before applying it." :
              "The live bitrate controller changed while Doctor prepared this step. Recheck before applying it."}
          };
        }
        run.controller_revision = applied.revision;
        run.requested_at = std::chrono::steady_clock::now();
        run.applied_at = {};
        run.network_sample_revision_at_apply =
          mutation_stats.network_sample_revision;
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
                      << " kbps (" << goal.source << ") run=" << run.run_id;

      return {
        {"status", true},
        {"changed", true},
        {"state", "applying"},
        {"message", "Doctor requested one quality step and will begin verification after the encoder acknowledges it."},
        {"run_id", run.run_id},
        {"request_id", run.request_id},
        {"requested", {{"bitrate_kbps", target_bitrate_kbps}, {"target_bitrate_kbps", goal_bitrate_kbps},
                       {"goal_source", goal.source}, {"goal_request_kbps", goal.target_kbps},
                       {"adaptive_bitrate_enabled", adaptive_state.enabled}}},
        {"encoder_application_confirmed", false},
        {"before", {{"bitrate_kbps", current_bitrate_kbps}, {"adaptive_bitrate_enabled", adaptive_state.enabled}}},
        {"verification", {{"delay_seconds", 8}, {"action_id", "verify"}, {"run_id", run.run_id}}},
        {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run.run_id}}},
        {"evidence", mutation_evidence}
      };
    }

    if (action_id != "lower_bitrate") {
      return {{"status", false}, {"changed", false}, {"error", "Unsupported Doctor action."}};
    }

    action_run_t run;
    adaptive_bitrate::doctor_state_t adaptive_state;
    int current_bitrate_kbps = 0;
    int target_bitrate_kbps = 0;
    nlohmann::json mutation_evidence;
    {
      std::lock_guard<std::mutex> lock(action_mutex);
      if (!valid_live_scope_locked(trusted_context)) {
        return {
          {"status", false}, {"changed", false}, {"state", "scope_unavailable"},
          {"error", "Doctor Auto Fix requires one fresh, unshared stream generation owning the live bitrate controller."}
        };
      }
      if (action_run.active) return action_in_progress(action_run);
      adaptive_state = adaptive_bitrate::get_doctor_state();
      const auto mutation_stats = stream_stats::get_current();
      mutation_evidence = network_evidence(mutation_stats);
      if (!deterministic_action_envelope_matches(
            request, action_id, mutation_stats, trusted_context, adaptive_state
          )) {
        return {
          {"status", false}, {"changed", false}, {"state", "evidence_changed"},
          {"code", "stale_action_envelope"},
          {"error", "Doctor evidence or controller ownership changed before this bitrate step could be applied."},
          {"evidence", mutation_evidence}
        };
      }
      if (!network_pressure_confirmed(mutation_stats)) {
        return {
          {"status", false}, {"changed", false}, {"state", "evidence_changed"},
          {"error", "Network evidence changed before Doctor could apply this bitrate step."},
          {"evidence", mutation_evidence}
        };
      }
      // With Live Tuning on, Doctor steps in for sustained video frame loss only. Round trip time alone
      // is Live Tuning's to cut for.
      const bool pause_live_tuning = adaptive_state.enabled;
      if (pause_live_tuning && !stream_stats::judged_network(mutation_stats).loss_pressure) {
        return {
          {"status", false}, {"changed", false}, {"state", "evidence_changed"},
          {"error", "Live Tuning adjusts this stream's bitrate for round trip time. Doctor steps in only for sustained video frame loss."},
          {"evidence", mutation_evidence}
        };
      }
      if (!adaptive_state.runtime_update_supported) {
        return {
          {"status", false}, {"changed", false}, {"state", "runtime_update_unavailable"},
          {"error", "The active encoder cannot lower bitrate during a live stream."},
          {"evidence", mutation_evidence}
        };
      }
      current_bitrate_kbps = adaptive_state.live_bitrate_kbps > 0 ?
        adaptive_state.live_bitrate_kbps : current_live_bitrate(mutation_stats);
      target_bitrate_kbps = guarded_bitrate_target(
        current_bitrate_kbps,
        request.value("target_bitrate_kbps", 0),
        adaptive_state.min_bitrate_kbps
      );
      if (target_bitrate_kbps <= 0 || target_bitrate_kbps >= current_bitrate_kbps) {
        return {
          {"status", false}, {"changed", false},
          {"error", "The live bitrate is unavailable or already at the safe floor."}
        };
      }
      run.active = true;
      run.kind = action_kind_e::lower_bitrate;
      run.run_id = next_run_id();
      run.request_id = request.value("request_id", std::string {});
      if (trusted_context != nullptr) {
        run.owner_uuid = trusted_context->owner_uuid;
        run.app_uuid = trusted_context->app_uuid;
        run.launch_instance_id = trusted_context->launch_instance_id;
        run.session_generation = trusted_context->session_generation;
      }
      run.previous_controller = adaptive_state;
      run.applied_bitrate_kbps = target_bitrate_kbps;
      run.goal_bitrate_kbps = target_bitrate_kbps;
      run.verification_step = 1;
      // A step taken for loss is verified only by a client media report from after it.
      run.requires_media_sample = stream_stats::judged_network(mutation_stats).loss_pressure;
      run.paused_live_tuning = pause_live_tuning;
      live_tuning_step_t live_tuning_step;
      if (pause_live_tuning) {
        live_tuning_step = step_with_live_tuning_off(adaptive_state, current_bitrate_kbps, target_bitrate_kbps);
      }
      const auto applied_revision = pause_live_tuning ?
        live_tuning_step.revision :
        adaptive_bitrate::set_doctor_bitrate_if_revision(
          adaptive_state.revision,
          target_bitrate_kbps
        );
      if (!applied_revision && live_tuning_step.live_tuning_back_on) {
        BOOST_LOG(info) << "Doctor: the live bitrate controller changed while Doctor took its step; Live Tuning is back on for this stream"sv
                        << (live_tuning_step.rate_restored ? " at the prior bitrate"sv : ""sv);
        return {
          {"status", false}, {"changed", false}, {"state", "controller_changed"},
          {"adaptive_bitrate_enabled", true},
          {"error", std::string {"The live bitrate controller changed while Doctor was taking this step, so Doctor turned Live Tuning back on for this stream"} +
            (live_tuning_step.rate_restored ? " and left the bitrate as it was." : ".") +
            " Recheck before applying it."}
        };
      }
      if (!applied_revision) {
        return {
          {"status", false}, {"changed", false}, {"state", "controller_changed"},
          {"error", "The live bitrate controller changed while Doctor prepared this step. Recheck before applying it."}
        };
      }
      run.controller_revision = *applied_revision;
      run.requested_at = std::chrono::steady_clock::now();
      run.applied_at = {};
      run.network_sample_revision_at_apply =
        mutation_stats.network_sample_revision;
      action_run = run;
      if (run.paused_live_tuning) {
        // Doctor offers this step's Undo, rather than a quality restore it would refuse, while it holds.
        stream_stats::set_doctor_live_tuning_step(run.run_id, run.controller_revision);
      }
      if (trusted_context != nullptr) {
        schedule_verification_watchdog(
          action_run.run_id,
          action_run.verification_step,
          action_run.owner_uuid,
          action_run.session_generation
        );
      }
    }
    BOOST_LOG(info) << "Doctor: requested guarded bitrate step "sv
                    << current_bitrate_kbps << " -> " << target_bitrate_kbps << " kbps"
                    << (run.paused_live_tuning ? " with Live Tuning off for this stream"sv : ""sv)
                    << " run=" << run.run_id;

    return {
      {"status", true},
      {"changed", true},
      {"state", "applying"},
      {"run_id", run.run_id},
      {"request_id", run.request_id},
      {"message", run.paused_live_tuning ?
        "Doctor turned Live Tuning off for this stream and requested one bitrate step. Verification begins after the encoder acknowledges it, and Undo turns Live Tuning back on." :
        "Doctor requested one bitrate step and will begin verification after the encoder acknowledges it."},
      {"requested", {{"bitrate_kbps", target_bitrate_kbps}, {"adaptive_bitrate_enabled", adaptive_state.enabled && !run.paused_live_tuning}}},
      {"encoder_application_confirmed", false},
      {"before", {{"bitrate_kbps", current_bitrate_kbps}, {"adaptive_bitrate_enabled", adaptive_state.enabled}}},
      {"verification", {{"delay_seconds", 8}, {"action_id", "verify"}, {"run_id", run.run_id}}},
      {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run.run_id}}},
      {"evidence", mutation_evidence}
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
    return bind_result_scope(
      execute_live_action(request, &recovery_context), recovery_context
    );
  }

  int http_status_code(const nlohmann::json &result) {
    const auto status = result.find("status");
    return status != result.end() && status->is_boolean() && status->get<bool>() ?
      200 : 409;
  }

  void session_started(std::string_view owner_uuid,
                       std::uint64_t session_generation,
                       std::string_view launch_instance_id,
                       int base_bitrate_kbps) {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (controller_sessions.empty()) {
      // Keep every request idempotent for the full stream generation. Once a
      // new isolated generation begins, exact request scope rejects old
      // retries and the prior generation's receipt history can be discarded.
      terminal_action = terminal_action_t {};
      terminal_action_history.clear();
    }
    if (action_run.active) {
      const auto run_id = action_run.run_id;
      const auto outcome = restore_bitrate_run_locked(action_run);
      BOOST_LOG(info) << "Doctor: new stream boundary "sv
                      << (outcome.status == restore_status_e::restored ? "restored "sv :
                          outcome.status == restore_status_e::encoder_unconfirmed ? "requested rollback without encoder acknowledgement "sv :
                          "retired superseded run without restoring "sv)
                      << (outcome.status != restore_status_e::superseded ?
                          std::to_string(outcome.bitrate_kbps) + " kbps" : std::string {})
                      << " run=" << run_id;
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
      std::string {launch_instance_id},
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
    adaptive_bitrate::set_session_scope(session_generation, std::string {launch_instance_id}, unshared_at_start);
    stream_stats::set_doctor_live_action_scope_available(
      controller_sessions.size() == 1 && controller_sessions.front().auto_fix_eligible
    );
  }

  void session_ended(std::string_view owner_uuid, std::uint64_t session_generation) {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (action_run.active && action_run.session_generation == session_generation &&
        action_run.owner_uuid == owner_uuid) {
      const auto run_id = action_run.run_id;
      const auto outcome = restore_bitrate_run_locked(action_run);
      BOOST_LOG(info) << "Doctor: stream ended; "sv
                      << (outcome.status == restore_status_e::restored ? "restored "sv :
                          outcome.status == restore_status_e::encoder_unconfirmed ? "requested rollback without encoder acknowledgement "sv :
                          "retired superseded run without restoring "sv)
                      << (outcome.status != restore_status_e::superseded ?
                          std::to_string(outcome.bitrate_kbps) + " kbps" : std::string {})
                      << " run=" << run_id;
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
    if (controller_sessions.empty()) {
      terminal_action = terminal_action_t {};
      terminal_action_history.clear();
    }

    adaptive_bitrate::set_session_scope(0, {}, false);
    // A manual live bitrate turned Live Tuning off for the stream that just ended, and only for it.
    if (controller_sessions.empty()) {
      adaptive_bitrate::end_stream_override();
    }

    // Encoder teardown already publishes runtime support loss. Do not change
    // the configured adaptive policy here; the next stream start reloads it,
    // while Doctor remains unavailable for any contaminated survivor.
    stream_stats::set_doctor_live_action_scope_available(
      controller_sessions.size() == 1 && controller_sessions.front().auto_fix_eligible
    );
  }

#ifdef POLARIS_TESTS
  void session_started(std::string_view owner_uuid,
                       std::uint64_t session_generation,
                       int base_bitrate_kbps) {
    session_started(owner_uuid, session_generation, {}, base_bitrate_kbps);
  }

  void make_verification_due_for_tests() {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (!action_run.active) return;
    if (const auto request = adaptive_bitrate::get_live_bitrate_request()) {
      adaptive_bitrate::acknowledge_live_bitrate_applied(
        request->revision,
        request->target_bitrate_kbps
      );
    }
    (void) encoder_application_confirmed_locked(action_run);
    action_run.requested_at -= verification_delay;
    action_run.applied_at -= verification_delay;
    // The network judge's readings live through the same seconds.
    stream_stats::age_network_judge_for_tests(verification_delay);
  }

  void confirm_encoder_application_for_tests() {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (!action_run.active) return;
    if (const auto request = adaptive_bitrate::get_live_bitrate_request()) {
      adaptive_bitrate::acknowledge_live_bitrate_applied(
        request->revision,
        request->target_bitrate_kbps
      );
    }
    (void) encoder_application_confirmed_locked(action_run);
  }

  void make_verification_window_complete_for_tests() {
    std::lock_guard<std::mutex> lock(action_mutex);
    if (!action_run.active) return;
    if (const auto request = adaptive_bitrate::get_live_bitrate_request()) {
      adaptive_bitrate::acknowledge_live_bitrate_applied(
        request->revision,
        request->target_bitrate_kbps
      );
    }
    (void) encoder_application_confirmed_locked(action_run);
    action_run.requested_at -= verification_delay;
    action_run.applied_at -= verification_delay;
    stream_stats::age_network_judge_for_tests(verification_delay);
    stream_stats::spread_network_verification_window_for_tests(
      action_run.network_sample_revision_at_apply,
      action_run.applied_at,
      std::chrono::steady_clock::now()
    );
  }

  void set_live_tuning_step_hook_for_tests(std::function<void(std::string_view)> hook) {
    std::lock_guard<std::mutex> lock(action_mutex);
    live_tuning_step_hook = std::move(hook);
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
