/**
 * @file src/doctor_actions.cpp
 * @brief Evidence-gated, reversible one-click Doctor actions shared by web and paired clients.
 */

#include "doctor_actions.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "adaptive_bitrate.h"
#include "game_library_scanner.h"
#include "logging.h"
#include "recovery_profile.h"

#ifdef __linux__
  #include "process.h"
#endif

using namespace std::literals;

namespace doctor_actions {
  namespace {
    enum class action_kind_e {
      none,
      lower_bitrate,
      restore_quality,
      disable_steam_input_xbox
    };

    /** One profile this run rewrote, and the value it held beforehand. */
    struct steam_profile_edit_t {
      std::filesystem::path path;
      bool previously_enabled = true;
    };

    struct action_run_t {
      bool active = false;
      action_kind_e kind = action_kind_e::none;
      std::string run_id;
      std::vector<steam_profile_edit_t> steam_edits;
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
        {"packet_loss_available", stats.packet_loss_available},
        {"packet_loss_source", stats.packet_loss_source},
        {"control_channel_packet_loss_pct", stats.control_channel_packet_loss},
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

    /**
     * Rewrite one profile's Xbox opt-in through a sibling temp file.
     *
     * A half-written localconfig.vdf is a corrupted Steam profile, so the new
     * payload is only ever swapped in by rename, which is atomic within the
     * directory. Returns false when nothing was written, including the case
     * where the value already held the requested setting.
     */
    bool rewrite_steam_profile(const std::filesystem::path &path, bool enabled) {
      std::ifstream input {path, std::ios::binary};
      if (!input) {
        return false;
      }
      std::ostringstream buffer;
      buffer << input.rdbuf();
      input.close();

      const auto updated = game_library::set_steam_input_xbox_support(buffer.str(), enabled);
      if (!updated) {
        return false;
      }

      auto temporary = path;
      temporary += ".polaris-doctor";
      {
        std::ofstream output {temporary, std::ios::binary | std::ios::trunc};
        if (!output) {
          return false;
        }
        output << *updated;
        if (!output) {
          return false;
        }
      }

      std::error_code rename_ec;
      std::filesystem::rename(temporary, path, rename_ec);
      if (rename_ec) {
        std::error_code discard_ec;
        std::filesystem::remove(temporary, discard_ec);
        return false;
      }
      return true;
    }

    /**
     * Read the profiles again, deliberately bypassing the 30s snapshot cache.
     *
     * Verification runs seconds after the write, well inside that cache, so the
     * cached snapshot would report the state this action just changed and call
     * its own success a failure.
     */
    game_library::steam_input_snapshot_t fresh_steam_input_state() {
      return game_library::inspect_steam_input_configs(
        game_library::steam_localconfig_paths(
          game_library::steam_data_roots(game_library::library_home_roots())
        )
      );
    }

    nlohmann::json steam_input_evidence() {
      const auto snapshot = fresh_steam_input_state();
      return {
        {"steam_input_status", snapshot.status},
        {"profiles_checked", snapshot.profiles_checked},
        {"profiles_with_xbox_support", snapshot.profiles_with_xbox_support},
        {"forced_app_count", snapshot.forced_app_count}
      };
    }

    /**
     * Whether the last run was the Steam Input one, active or finished.
     *
     * `verify` carries only a run id, so the shared verb has to ask what it is
     * verifying before the streaming gate decides a stream is required. The
     * check deliberately ignores `active`: verifying a Steam Input run that has
     * already been undone must answer "expired", not send the caller off to
     * start a stream this action never needed.
     */
    bool steam_input_run_selected() {
      std::lock_guard<std::mutex> lock(action_mutex);
      return action_run.kind == action_kind_e::disable_steam_input_xbox;
    }

    std::string next_run_id() {
      const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
      ).count();
      return "doctor-run-" + std::to_string(ticks);
    }

    std::string normalized_codec_family(std::string codec) {
      std::transform(codec.begin(), codec.end(), codec.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
      });
      if (codec.find("av1") != std::string::npos) return "av1";
      if (codec.find("hevc") != std::string::npos || codec.find("h265") != std::string::npos ||
          codec.find("h.265") != std::string::npos) return "hevc";
      if (codec.find("h264") != std::string::npos || codec.find("h.264") != std::string::npos ||
          codec.find("avc") != std::string::npos) return "h264";
      return {};
    }

    int trusted_target_fps(const stream_stats::stats_t &stats) {
      const double fps =
        stats.encode_target_fps > 0.0 ? stats.encode_target_fps :
        stats.session_target_fps > 0.0 ? stats.session_target_fps :
        stats.requested_client_fps > 0.0 ? stats.requested_client_fps :
        stats.fps;
      return static_cast<int>(std::round(fps));
    }

    int trusted_bitrate(const stream_stats::stats_t &stats) {
      return stats.adaptive_target_bitrate_kbps > 0 ?
        stats.adaptive_target_bitrate_kbps : stats.bitrate_kbps;
    }

    std::optional<recovery_profile::safe_profile_t> derive_safe_recovery_profile(
      const recovery_action_context_t &context
    ) {
      recovery_profile::safe_profile_t profile;
      const auto safe_mode = context.health.value("safe_display_mode", std::string {});
      profile.stream_display_mode = safe_mode == "virtual_display" ? "host_virtual_display" :
        safe_mode == "headless" ? "headless_stream" : context.effective_stream_display_mode;
      profile.width = context.stats.width;
      profile.height = context.stats.height;
      profile.target_fps = context.health.value("safe_target_fps", trusted_target_fps(context.stats));
      profile.target_bitrate_kbps = context.health.value("safe_bitrate_kbps", trusted_bitrate(context.stats));
      profile.preferred_codec = normalized_codec_family(
        context.health.value("safe_codec", context.stats.codec)
      );
      if (profile.preferred_codec.empty()) profile.preferred_codec = "h264";
      profile.hdr = context.health.value("safe_hdr", false);

      if (profile.width <= 0 || profile.height <= 0 || profile.target_fps <= 0 ||
          profile.target_bitrate_kbps <= 0 || profile.stream_display_mode.empty()) {
        return std::nullopt;
      }
      return profile;
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
      if (action_id != "apply_recovery_profile_next_launch" &&
          action_id != "verify_recovery_profile_next_launch" &&
          action_id != "undo") {
        return nlohmann::json();
      }

      const auto run_id = request.value("run_id", std::string {});
      if (action_id == "undo") {
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
        if (!result.value("status", false) &&
            result.value("error", std::string {}).find("no longer available") != std::string::npos) {
          // This was not an owned queued recovery run. Preserve the existing
          // live-action Undo namespace for its own run identifiers.
          return nlohmann::json();
        }
        return result;
      }

      if (!context.active_owner || !context.host_tuning_allowed || context.owner_uuid.empty() ||
          context.app_uuid.empty() || context.state_path.empty() || !context.stats.streaming) {
        return recovery_rejected(
          "A matching active owner and game stream are required for this recovery action.",
          "evidence_changed"
        );
      }

      if (action_id == "verify_recovery_profile_next_launch") {
        if (run_id.empty()) {
          return recovery_rejected("Recovery verification requires a run_id.");
        }
        recovery_profile::observed_launch_t observed {
          .streaming = context.stats.streaming,
          .owner_uuid = context.owner_uuid,
          .app_uuid = context.app_uuid,
          .stream_display_mode = context.effective_stream_display_mode,
          .width = context.stats.width,
          .height = context.stats.height,
          .target_fps = trusted_target_fps(context.stats),
          .bitrate_kbps = trusted_bitrate(context.stats),
          .codec = normalized_codec_family(context.stats.codec),
          .hdr = context.stats.stream_hdr_enabled,
          .launch_instance_id = context.launch_instance_id,
          .session_generation = context.session_generation,
        };
        return recovery_profile::verify(
          context.state_path, context.owner_uuid, context.app_uuid, run_id, observed
        );
      }

      if (!request.value("confirmed", false)) {
        return recovery_rejected(
          "Confirm that the current stream stays unchanged and the safer profile applies only to the next launch of this game on this paired device.",
          "confirmation_required"
        );
      }

      const auto doctor = context.health.value("doctor", nlohmann::json::object());
      const auto current_action = doctor.value("safe_recovery_action", nlohmann::json::object());
      const auto source_result_id = doctor.value("result_id", std::string {});
      const auto requested_result_id = request.value("source_result_id", std::string {});
      const auto requested_app_uuid = request.value("app_uuid", std::string {});
      const auto preview = current_action.value("payload_preview", nlohmann::json::object());
      const bool exact_current_action =
        context.health.value("relaunch_recommended", false) &&
        current_action.value("id", std::string {}) == "apply_recovery_profile_next_launch" &&
        current_action.value("kind", std::string {}) == "next_launch_profile" &&
        current_action.value("requires_confirmation", false) &&
        current_action.value("requires_owner", false) &&
        current_action.value("owner_tuning_allowed", false) &&
        current_action.value("undo", nlohmann::json::object()).value("supported", false) &&
        !source_result_id.empty() && requested_result_id == source_result_id &&
        preview.value("source_result_id", std::string {}) == source_result_id &&
        !requested_app_uuid.empty() && requested_app_uuid == context.app_uuid &&
        preview.value("app_uuid", std::string {}) == context.app_uuid &&
        !context.launch_instance_id.empty() && context.session_generation > 0;
      if (!exact_current_action) {
        return recovery_rejected(
          "Current Doctor evidence no longer authorizes this next-launch recovery profile.",
          "evidence_changed"
        );
      }

      const auto profile = derive_safe_recovery_profile(context);
      if (!profile) {
        return recovery_rejected("Current host evidence cannot produce a complete safe profile.");
      }
      auto receipt = recovery_profile::queue(
        context.state_path,
        context.owner_uuid,
        context.app_uuid,
        source_result_id,
        *profile,
        context.launch_instance_id,
        context.session_generation
      );
      if (receipt.value("status", false)) {
        receipt["action_id"] = "apply_recovery_profile_next_launch";
        receipt["kind"] = "next_launch_profile";
        const auto recovery_state = receipt.value("recovery_state", std::string {});
        receipt["message"] = recovery_state == "applied" ?
          "This confirmed recovery action was already applied by its matching one-shot launch." :
          "The current stream is unchanged. The safer profile is queued only for the next launch of this game on this paired device.";
        receipt["verification"] = {
          {"mode", "post_connect"},
          {"action_id", "verify_recovery_profile_next_launch"},
          {"endpoint", "/polaris/v1/doctor/action"},
          {"run_id", receipt.value("run_id", std::string {})}
        };
      }
      return receipt;
    }
  }  // namespace

  bool network_pressure_confirmed(const stream_stats::stats_t &stats) {
    return stats.streaming && stats.network_risk &&
      ((stats.packet_loss_available && stats.packet_loss > 2.0) || stats.latency_ms >= 45.0);
  }

  bool paired_route_allowed(std::string_view action_id,
                            bool active_owner_present,
                            bool caller_is_active_owner) {
    static_cast<void>(active_owner_present);
    if (caller_is_active_owner) return true;
    // The core action resolves Undo under the caller certificate's owner UUID,
    // so this cannot remove the active owner's or another client's record.
    return action_id == "undo";
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

  nlohmann::json execute(const nlohmann::json &request) {
    const auto action_id = request.value("action_id", std::string {});

    if (action_id == "undo") {
      const auto run_id = request.value("run_id", std::string {});
      std::lock_guard<std::mutex> lock(action_mutex);
      if (!action_run.active || run_id.empty() || run_id != action_run.run_id) {
        return {{"status", false}, {"changed", false}, {"error", "This Doctor undo is no longer available."}};
      }

      if (action_run.kind == action_kind_e::disable_steam_input_xbox) {
        return steam_vdf_read_only_response();
        int restored = 0;
        for (const auto &edit : action_run.steam_edits) {
          if (rewrite_steam_profile(edit.path, edit.previously_enabled)) {
            ++restored;
          }
        }
        action_run.active = false;
        BOOST_LOG(info) << "Doctor: restored Steam Input opt-in on "sv << restored
                        << " profile(s) run=" << run_id;
        return {
          {"status", true},
          {"changed", restored > 0},
          {"state", "undone"},
          {"message", "Restored the Steam Input setting each profile held before this Doctor run."},
          {"restored_profiles", restored},
          {"evidence", steam_input_evidence()}
        };
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

    // Legacy Steam VDF action IDs remain recognized only so stale clients and
    // receipts fail closed before Steam shutdown or filesystem work.
    if (action_id == "disable_steam_input_xbox" || (action_id == "verify" && steam_input_run_selected())) {
      return steam_vdf_read_only_response();
#ifndef __linux__
      return {
        {"status", false},
        {"changed", false},
        {"error", "Closing desktop Steam is only supported on Linux hosts."}
      };
#else
      if (action_id == "verify") {
        const auto run_id = request.value("run_id", std::string {});
        std::lock_guard<std::mutex> lock(action_mutex);
        if (!action_run.active || run_id.empty() || run_id != action_run.run_id) {
          return {{"status", false}, {"changed", false}, {"state", "expired"}, {"error", "Doctor run not found."}};
        }
        const auto state = fresh_steam_input_state();
        const bool cleared = state.profiles_with_xbox_support == 0;
        // A per-app Force On outranks the host-wide opt-in this action owns, so
        // clearing the opt-in while overrides remain is a real partial result,
        // not a success and not a failure.
        const bool overrides_remain = state.forced_app_count > 0;
        return {
          {"status", true},
          {"changed", false},
          {"run_id", run_id},
          {"state", cleared && !overrides_remain ? "resolved" : "needs_attention"},
          {"message", !cleared ?
             "A Steam profile still opts the emulated controller into Steam Input." :
             overrides_remain ?
               "The host-wide opt-in is cleared, but per-game Force On overrides remain. Set those games to Default or Disable in their Steam controller properties." :
               "No Steam profile opts the emulated controller into Steam Input any more."},
          {"evidence", steam_input_evidence()},
          {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run_id}}}
        };
      }

      const auto before = fresh_steam_input_state();
      if (before.profiles_with_xbox_support == 0) {
        return {
          {"status", false},
          {"changed", false},
          {"state", "evidence_changed"},
          {"error", before.forced_app_count > 0 ?
             "No profile opts in host-wide. The remaining conflict is per-game Force On, which must be changed in each game's Steam controller properties." :
             "No Steam profile opts the emulated controller into Steam Input."},
          {"evidence", steam_input_evidence()}
        };
      }

      // Steam rewrites this file when it exits, so an edit under a live client
      // is reverted the moment that client closes. Closing Steam first is the
      // whole reason this action asks for confirmation.
      if (!proc::ensure_steam_client_quiescent_for_doctor()) {
        return {
          {"status", false},
          {"changed", false},
          {"state", "steam_still_running"},
          {"error", "Steam did not close, so the change would be reverted when it exits. Close Steam and try again."},
          {"evidence", steam_input_evidence()}
        };
      }

      action_run_t run;
      run.active = true;
      run.kind = action_kind_e::disable_steam_input_xbox;
      run.run_id = next_run_id();
      run.applied_at = std::chrono::steady_clock::now();
      for (const auto &path : game_library::steam_localconfig_paths(
             game_library::steam_data_roots(game_library::library_home_roots()))) {
        if (rewrite_steam_profile(path, false)) {
          run.steam_edits.push_back({path, true});
        }
      }

      if (run.steam_edits.empty()) {
        return {
          {"status", false},
          {"changed", false},
          {"error", "No Steam profile could be updated."},
          {"evidence", steam_input_evidence()}
        };
      }

      {
        std::lock_guard<std::mutex> lock(action_mutex);
        action_run = run;
      }
      BOOST_LOG(info) << "Doctor: cleared the Xbox Steam Input opt-in on "sv
                      << run.steam_edits.size() << " profile(s) run=" << run.run_id;

      return {
        {"status", true},
        {"changed", true},
        {"state", "watching"},
        {"message", "Closed Steam and cleared the Xbox Steam Input opt-in. Launch the game again to pick up the change."},
        {"run_id", run.run_id},
        {"applied", {{"profiles_updated", static_cast<int>(run.steam_edits.size())}}},
        {"before", {{"profiles_with_xbox_support", before.profiles_with_xbox_support}, {"forced_app_count", before.forced_app_count}}},
        {"verification", {{"delay_seconds", 2}, {"action_id", "verify"}, {"run_id", run.run_id}}},
        {"undo", {{"available", true}, {"action_id", "undo"}, {"run_id", run.run_id}}},
        {"evidence", steam_input_evidence()}
      };
#endif
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

  nlohmann::json execute(const nlohmann::json &request,
                         const recovery_action_context_t &recovery_context) {
    auto recovery_result = execute_recovery_action(request, recovery_context);
    if (!recovery_result.is_null()) {
      return recovery_result;
    }
    return execute(request);
  }

}  // namespace doctor_actions
