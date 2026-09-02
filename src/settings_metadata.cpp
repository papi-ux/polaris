/**
 * @file src/settings_metadata.cpp
 * @brief Shared settings metadata builders for the HTTP servers.
 */
#include "settings_metadata.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "adaptive_bitrate.h"
#include "process.h"
#include "stream_stats.h"
#ifdef __linux__
  #include "platform/linux/stream_display_policy.h"
  #include "platform/linux/virtual_display.h"
#endif

using namespace std::literals;

namespace settings_metadata {
  namespace {
    std::string lower_copy(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return value;
    }
  }  // namespace

  bool ai_auto_quality_enabled() {
    // Legacy compatibility field. Launch policy no longer has an AI-owned
    // mode; adaptive bitrate remains a separate same-stream control.
    return false;
  }

  std::string auto_quality_blocked_reason(const std::string &limiting_factor) {
    const auto normalized = lower_copy(limiting_factor);
    if (normalized == "host_render") {
      return "host_render_limited";
    }
    if (normalized == "network" ||
        normalized == "encoder" ||
        normalized == "decoder") {
      return normalized;
    }
    if (normalized == "pacing" ||
        normalized == "capture" ||
        normalized == "hdr") {
      return "insufficient_signal";
    }
    return "none";
  }

  nlohmann::json build_auto_quality_policy_json(const nlohmann::json &health,
                                                const adaptive_bitrate::state_t &adaptive_state,
                                                int encoder_bitrate_kbps) {
    const bool auto_quality_enabled = ai_auto_quality_enabled();
    const std::string limiting_factor = health.value("limiting_factor", std::string {"none"});
    const std::string adaptive_state_name = lower_copy(adaptive_state.state);
    const bool host_render_limited =
      health.value("host_render_limited", false) ||
      limiting_factor == "host_render" ||
      lower_copy(health.value("primary_issue", std::string {})) == "host_render_limited";
    const bool host_render_recovery =
      host_render_limited &&
      health.value("relaunch_recommended", false) &&
      health.contains("safe_target_fps");
    const bool blocked =
      (!host_render_recovery && host_render_limited) ||
      limiting_factor == "network" ||
      limiting_factor == "encoder" ||
      limiting_factor == "decoder";
    const int live_bitrate_kbps =
      adaptive_state.target_bitrate_kbps > 0 ? adaptive_state.target_bitrate_kbps : encoder_bitrate_kbps;
    const int quality_cap_kbps =
      adaptive_state.base_bitrate_kbps > 0 ? adaptive_state.base_bitrate_kbps : encoder_bitrate_kbps;
    const bool bitrate_recovering =
      !blocked &&
      adaptive_state.enabled &&
      adaptive_state_name == "recovering" &&
      live_bitrate_kbps > 0 &&
      quality_cap_kbps > 0 &&
      live_bitrate_kbps < quality_cap_kbps;

    std::string state = "holding";
    std::string blocked_reason = "none";
    std::string summary = "Auto Quality is holding the current profile.";
    if (!auto_quality_enabled) {
      state = "off";
      summary = "Manual stream tuning is active.";
    } else if (blocked) {
      state = "blocked";
      blocked_reason = auto_quality_blocked_reason(limiting_factor);
      summary =
        blocked_reason == "host_render_limited" ?
          "Holding quality until the host render path reaches the stream FPS target." :
        blocked_reason == "network" ?
          "Holding quality while network pressure clears." :
        blocked_reason == "encoder" ?
          "Holding quality while encoder pressure clears." :
        blocked_reason == "decoder" ?
          "Holding quality while decoder pressure clears." :
          "Holding quality until the stream has enough clean signal.";
    } else if (host_render_recovery) {
      state = "recovery_queued";
      summary = "AI Recovery Profile ready for the next launch.";
    } else if (bitrate_recovering) {
      state = "recovering_bitrate";
      summary = "Recovering bitrate toward the quality cap.";
    }

    nlohmann::json policy;
    policy["enabled"] = auto_quality_enabled;
    policy["state"] = state;
    policy["blocked_reason"] = blocked_reason;
    policy["live_bitrate_kbps"] = live_bitrate_kbps;
    policy["quality_cap_kbps"] = quality_cap_kbps;
    policy["relaunch_required"] =
      auto_quality_enabled && state != "blocked" && health.value("relaunch_recommended", false);
    policy["can_recover_live"] = state == "recovering_bitrate";
    policy["summary"] = summary;
    policy["detail"] = health.value("summary", summary);
    policy["components"] = {
      {"optimizer_active", false},
      {"adaptive_bitrate_active", adaptive_bitrate::is_active()},
      {"adaptive_state", adaptive_state.state},
      {"adaptive_reason", adaptive_state.reason},
      {"target_bitrate_kbps", live_bitrate_kbps}
    };
    if (policy["relaunch_required"].get<bool>()) {
      auto &suggested = policy["suggested_profile"];
      suggested["target_bitrate_kbps"] = health.value("safe_bitrate_kbps", 0);
      suggested["display_mode"] = health.value("safe_display_mode", std::string {});
      if (health.contains("safe_target_fps")) {
        suggested["target_fps"] = health["safe_target_fps"];
      }
      if (health.contains("safe_codec")) {
        suggested["preferred_codec"] = health["safe_codec"];
      }
      if (health.contains("safe_hdr")) {
        suggested["hdr"] = health["safe_hdr"];
      }
    }
    return policy;
  }

  bool host_virtual_display_available() {
#ifdef __linux__
    return virtual_display::is_available();
#elif defined(_WIN32)
    return
      proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK ||
      proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::UNKNOWN;
#else
    return false;
#endif
  }

  std::string configured_stream_display_mode_selection() {
#ifdef __linux__
    return stream_display_policy::resolve_current().selection;
#else
    return "desktop_display";
#endif
  }

  std::string effective_stream_display_mode_selection(
    const stream_stats::stats_t &stats,
    bool session_uses_virtual_display
  ) {
#ifdef __linux__
    return stream_display_policy::resolve_effective(
      stream_display_policy::input_t {
        host_virtual_display_available(),
        false,
        stats.runtime_gpu_native_override_active,
      },
      stats.streaming,
      session_uses_virtual_display,
      stats.runtime_effective_headless
    ).selection;
#else
    (void) stats;
    (void) session_uses_virtual_display;
    return configured_stream_display_mode_selection();
#endif
  }

  std::string effective_stream_display_mode_selection(const stream_stats::stats_t &stats) {
    return effective_stream_display_mode_selection(stats, proc::proc.session_uses_virtual_display());
  }

  std::string stream_display_mode_label_for_selection(const std::string &selection) {
#ifdef __linux__
    const auto label = stream_display_policy::label_for_selection(selection);
    return label.empty() ? "Mirror Desktop" : label;
#else
    if (selection == "headless_stream") {
      return "Private Stream";
    }
    if (selection == "host_virtual_display") {
      return "Host Virtual Display";
    }
    if (selection == "windowed_stream") {
      return "Private Stream (GPU-native)";
    }
    if (selection == "gamescope_stream") {
      return "Gamescope Stream";
    }
    return "Mirror Desktop";
#endif
  }

  std::string stream_display_mode_reason_for_selection(const std::string &selection) {
#ifdef __linux__
    return stream_display_policy::reason_for_selection(selection, host_virtual_display_available());
#else
    (void) selection;
    return "Polaris will mirror the current desktop session.";
#endif
  }

  nlohmann::json stream_display_mode_options_json() {
    nlohmann::json modes = nlohmann::json::array();
#ifdef __linux__
    for (const auto &option : stream_display_policy::mode_options(host_virtual_display_available())) {
      bool available = option.available;
      if (option.value == "host_virtual_display") {
        available = available && host_virtual_display_available();
      }
      std::string unavailable_reason;
      if (!available) {
        unavailable_reason = option.unavailable_reason;
        if (option.value == "host_virtual_display") {
          // The backend probe knows exactly why creation would fail; the
          // policy layer only knows that it would.
          const auto backend_reason = virtual_display::unavailable_reason();
          if (!backend_reason.empty()) {
            unavailable_reason = backend_reason;
          }
        }
        if (unavailable_reason.empty()) {
          unavailable_reason = "This mode is not available on this host right now.";
        }
      }
      modes.push_back({
        {"value", option.value},
        {"label", option.label},
        {"badge", option.badge},
        {"available", available},
        {"unavailable_reason", unavailable_reason},
        {"restart_required", true},
        {"reason", option.reason},
        {"group", option.group},
        {"runtime", option.runtime},
        {"capture", option.capture},
        {"topology", option.topology},
        // Available and session-overridable are different questions. A dongle
        // swap is a perfectly valid host default while still being something a
        // single client must not switch on for one launch, and a client that
        // cannot see the difference offers a choice the host will silently drop.
        {"session_overridable", stream_display_policy::selection_session_overridable(option.value)},
      });
    }
#else
    for (const auto &selection : {
           "headless_stream"s,
           "desktop_display"s,
           "host_virtual_display"s,
           "windowed_stream"s
         }) {
      const bool available = selection != "host_virtual_display" || host_virtual_display_available();
      const bool private_group = selection == "headless_stream" || selection == "windowed_stream";
      const std::string badge =
        selection == "headless_stream" ? "Recommended" :
        selection == "host_virtual_display" ? "Compatibility" :
        selection == "windowed_stream" ? "Advanced capture" :
        "Advanced";
      modes.push_back({
        // Same shape as the Linux branch so clients parse one contract;
        // the runtime/capture/topology vocabulary is Linux-only and empty here.
        {"value", selection},
        {"label", stream_display_mode_label_for_selection(selection)},
        {"badge", badge},
        {"available", available},
        {"unavailable_reason", available ? "" : "Host virtual display is not available on this host."},
        {"restart_required", true},
        {"reason", stream_display_mode_reason_for_selection(selection)},
        {"group", private_group ? "private" : "host"},
        {"runtime", ""},
        {"capture", ""},
        {"topology", ""},
      });
    }
#endif
    return modes;
  }

  nlohmann::json build_tuning_json(const adaptive_bitrate::state_t &adaptive_state,
                                   const stream_stats::stats_t &stats,
                                   bool mangohud_configured) {
    nlohmann::json tuning;
    tuning["adaptive_bitrate_enabled"] = adaptive_bitrate::is_enabled();
    tuning["adaptive_bitrate_active"] = adaptive_state.active;
    tuning["adaptive_runtime_update_supported"] = adaptive_state.runtime_update_supported;
    tuning["adaptive_target_bitrate_kbps"] = stats.adaptive_target_bitrate_kbps;
    tuning["adaptive_base_bitrate_kbps"] = adaptive_state.base_bitrate_kbps;
    tuning["adaptive_min_bitrate_kbps"] = adaptive_state.min_bitrate_kbps;
    tuning["adaptive_max_bitrate_kbps"] = adaptive_state.max_bitrate_kbps;
    tuning["adaptive_bitrate_state"] = adaptive_state.state;
    tuning["adaptive_bitrate_reason"] = adaptive_state.reason;
    tuning["adaptive_packet_loss_ewma"] = adaptive_state.ewma_packet_loss;
    tuning["adaptive_rtt_ewma_ms"] = adaptive_state.ewma_rtt_ms;
    tuning["ai_auto_quality_enabled"] = false;
    tuning["ai_optimizer_enabled"] = false;
    tuning["mangohud_configured"] = mangohud_configured;
    return tuning;
  }
  namespace {
    constexpr std::size_t k_config_write_notes = 32;

    struct config_write_note_t {
      std::chrono::system_clock::time_point at;
      std::string writer;
      std::vector<std::string> keys;
    };

    std::mutex config_write_mutex;
    std::deque<config_write_note_t> config_write_notes;

    std::string iso8601_utc(std::chrono::system_clock::time_point at) {
      const std::time_t seconds = std::chrono::system_clock::to_time_t(at);
      std::tm utc {};
#ifdef _WIN32
      gmtime_s(&utc, &seconds);
#else
      gmtime_r(&seconds, &utc);
#endif
      char buffer[32];
      if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return {};
      }
      return buffer;
    }
  }  // namespace

  void note_config_write(const std::string &writer, std::vector<std::string> keys) {
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    std::lock_guard<std::mutex> lock(config_write_mutex);
    config_write_notes.push_front({std::chrono::system_clock::now(), writer, std::move(keys)});
    while (config_write_notes.size() > k_config_write_notes) {
      config_write_notes.pop_back();
    }
  }

  nlohmann::json config_write_provenance_json() {
    auto notes = nlohmann::json::array();
    std::lock_guard<std::mutex> lock(config_write_mutex);
    for (const auto &note : config_write_notes) {
      notes.push_back({
        {"at", iso8601_utc(note.at)},
        {"writer", note.writer},
        {"keys", note.keys},
      });
    }
    return notes;
  }

  void reset_config_write_provenance_for_tests() {
    std::lock_guard<std::mutex> lock(config_write_mutex);
    config_write_notes.clear();
  }
}  // namespace settings_metadata
