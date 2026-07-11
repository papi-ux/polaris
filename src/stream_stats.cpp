/**
 * @file src/stream_stats.cpp
 * @brief Thread-safe stream statistics collector for real-time monitoring.
 *
 * Supports tracking multiple simultaneous streaming clients.
 */

// standard includes
#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "adaptive_bitrate.h"
#include "config.h"
#include "logging.h"
#include "stream_stats.h"
#ifdef __linux__
  #include "platform/linux/cage_display_router.h"
  #include "platform/linux/stream_display_policy.h"
#endif

namespace stream_stats {

  static std::mutex stats_mutex;
  static stats_t current_stats;

  namespace {
    constexpr std::size_t CAPTURE_PROFILE_SUMMARY_FRAMES = 600;
    constexpr std::size_t CAPTURE_PROFILE_BUCKET_COUNT =
      static_cast<std::size_t>(platf::frame_transport_e::unknown) + 1;

    struct capture_profile_bucket_t {
      std::vector<long long> dispatch_us;
      std::vector<long long> ingest_us;
      std::vector<long long> total_us;
    };

    std::array<capture_profile_bucket_t, CAPTURE_PROFILE_BUCKET_COUNT> capture_profile_buckets;

    capture_profile_bucket_t &capture_profile_bucket(platf::frame_transport_e transport) {
      auto index = static_cast<std::size_t>(transport);
      if (index >= capture_profile_buckets.size()) {
        index = static_cast<std::size_t>(platf::frame_transport_e::unknown);
      }
      return capture_profile_buckets[index];
    }

    void clear_capture_profile_buckets() {
      for (auto &bucket : capture_profile_buckets) {
        bucket.dispatch_us.clear();
        bucket.ingest_us.clear();
        bucket.total_us.clear();
      }
    }

    const char *stream_display_mode_label() {
#ifdef __linux__
      static thread_local std::string label;
      label = stream_display_policy::resolve(stream_display_policy::input_t {
        false,
        false,
        cage_display_router::runtime_state().gpu_native_override_active,
      }).label;
      return label.c_str();
#else
      const auto &linux_display = config::video.linux_display;
      if (!linux_display.headless_mode) {
        return "Mirror Desktop";
      }
      if (!linux_display.use_cage_compositor) {
        return "Host Virtual Display";
      }
      if (linux_display.prefer_gpu_native_capture) {
        return "Private Stream (GPU-native)";
      }
      return "Private Stream";
#endif
    }

    long long percentile_value(std::vector<long long> values, double percentile) {
      if (values.empty()) {
        return 0;
      }

      std::sort(values.begin(), values.end());
      auto pos = static_cast<std::size_t>(percentile * static_cast<double>(values.size() - 1));
      return values[pos];
    }
  }  // namespace

  std::string stats_t::to_json() const {
    nlohmann::json j;

    j["streaming"] = streaming;
    j["client_name"] = client_name;
    j["client_ip"] = client_ip;
    j["runtime_backend"] = runtime_backend;
    j["runtime_requested_headless"] = runtime_requested_headless;
    j["runtime_effective_headless"] = runtime_effective_headless;
    j["runtime_gpu_native_override_active"] = runtime_gpu_native_override_active;
    j["stream_display_mode"] = stream_display_mode_label();
    j["capture_transport"] = platf::from_frame_transport(capture_transport);
    j["capture_residency"] = platf::from_frame_residency(capture_residency);
    j["capture_format"] = platf::from_frame_format(capture_format);
    j["capture_device"] = capture_device;
    const auto capture_path = capture_path_summary(*this);
    const auto capture_reason = capture_path_reason(*this);
    const auto capture_reason_message = capture_path_reason_message(capture_reason);
    const bool capture_cpu_copy = capture_path_uses_cpu_copy(*this);
    const bool capture_gpu_native = capture_path_is_gpu_native(*this);
    j["capture_path"] = capture_path;
    j["capture_path_reason"] = capture_reason;
    j["capture_path_reason_message"] = capture_reason_message;
    j["capture_cpu_copy"] = capture_cpu_copy;
    j["capture_gpu_native"] = capture_gpu_native;
    j["capture_decision"] = {
      {"path", capture_path},
      {"reason", capture_reason},
      {"reason_message", capture_reason_message},
      {"transport", platf::from_frame_transport(capture_transport)},
      {"residency", platf::from_frame_residency(capture_residency)},
      {"format", platf::from_frame_format(capture_format)},
      {"capture_device", capture_device},
      {"encoder_adapter", config::video.adapter_name},
      {"cross_gpu_dmabuf_risk", capture_path_has_cross_gpu_dmabuf_risk(*this)},
      {"cpu_copy", capture_cpu_copy},
      {"gpu_native", capture_gpu_native},
      {"runtime_backend", runtime_backend},
      {"requested_headless", runtime_requested_headless},
      {"effective_headless", runtime_effective_headless},
      {"gpu_native_override_active", runtime_gpu_native_override_active}
    };
    j["linux_gpu_profile"] = linux_gpu_profile_json(*this);
    j["encode_target_device"] = encode_target_device;
    j["encode_target_residency"] = platf::from_frame_residency(encode_target_residency);
    j["encode_target_format"] = platf::from_frame_format(encode_target_format);
    j["dynamic_range"] = dynamic_range;
    j["display_hdr"] = display_hdr;
    j["hdr_metadata_available"] = hdr_metadata_available;
    j["stream_hdr_enabled"] = stream_hdr_enabled;
    j["color_coding"] = color_coding;
    j["hdr_effective_mode"] = hdr_effective_mode(*this);
    j["hdr_downgrade_reason"] = hdr_downgrade_reason(*this);
    j["hdr_downgrade_message"] = hdr_downgrade_message(*this);
    j["fps"] = fps;
    j["requested_client_fps"] = requested_client_fps;
    j["session_target_fps"] = session_target_fps;
    j["encode_target_fps"] = encode_target_fps;
    j["bitrate_kbps"] = bitrate_kbps;
    j["encode_time_ms"] = encode_time_ms;
    j["duplicate_frame_ratio"] = duplicate_frame_ratio;
    j["dropped_frame_ratio"] = dropped_frame_ratio;
    j["avg_frame_age_ms"] = avg_frame_age_ms;
    j["frame_jitter_ms"] = frame_jitter_ms;
    j["codec"] = codec;
    j["pacing_policy"] = pacing_policy;
    j["optimization_source"] = optimization_source;
    j["optimization_confidence"] = optimization_confidence;
    j["optimization_cache_status"] = optimization_cache_status;
    j["optimization_reasoning"] = optimization_reasoning;
    j["optimization_normalization_reason"] = optimization_normalization_reason;
    j["recommendation_version"] = recommendation_version;
    j["width"] = width;
    j["height"] = height;
    j["latency_ms"] = latency_ms;
    j["packet_loss"] = packet_loss;
    j["bytes_sent"] = bytes_sent;
    j["gpu_usage"] = gpu_usage;
    j["adaptive_target_bitrate_kbps"] = adaptive_target_bitrate_kbps;
    j["headless_mode"] = config::video.linux_display.headless_mode;
    j["ai_enabled"] = config::video.ai_optimizer.enabled;
    j["controller_input"] = {
      {"virtual_controller_created", input_virtual_controller_created},
      {"virtual_controller_number", input_virtual_controller_number},
      {"virtual_controller_kind", input_virtual_controller_kind},
      {"virtual_controller_error", input_virtual_controller_error},
      {"host_controller_isolation", input_host_controller_isolation},
      {"host_controller_isolation_detail", input_host_controller_isolation_detail},
      {"haptics_supported", input_haptics_supported},
      {"haptics_detail", input_haptics_detail}
    };

    // Multi-client data
    nlohmann::json clients_json = nlohmann::json::array();
    for (const auto &c : clients) {
      nlohmann::json cj;
      cj["name"] = c.name;
      cj["ip"] = c.ip;
      cj["fps"] = c.fps;
      cj["bitrate_kbps"] = c.bitrate_kbps;
      cj["encode_time_ms"] = c.encode_time_ms;
      cj["codec"] = c.codec;
      cj["width"] = c.width;
      cj["height"] = c.height;
      cj["latency_ms"] = c.latency_ms;
      cj["packet_loss"] = c.packet_loss;
      cj["bytes_sent"] = c.bytes_sent;
      cj["adaptive_target_bitrate_kbps"] = c.adaptive_target_bitrate_kbps;
      clients_json.push_back(cj);
    }
    j["clients"] = clients_json;
    j["active_sessions"] = static_cast<int>(clients.size());
    j["doctor"] = build_doctor_json(*this, nlohmann::json::object());

    return j.dump();
  }

  bool capture_path_uses_cpu_copy(const stats_t &stats) {
    return
      stats.capture_transport == platf::frame_transport_e::shm ||
      stats.capture_residency == platf::frame_residency_e::cpu ||
      stats.encode_target_residency == platf::frame_residency_e::cpu;
  }

  bool capture_path_is_gpu_native(const stats_t &stats) {
    return
      stats.capture_transport == platf::frame_transport_e::dmabuf &&
      stats.capture_residency == platf::frame_residency_e::gpu &&
      stats.encode_target_residency == platf::frame_residency_e::gpu;
  }

  bool capture_path_has_cross_gpu_dmabuf_risk(const stats_t &stats) {
#ifdef __linux__
    return
      stats.runtime_effective_headless &&
      config::video.linux_display.use_cage_compositor &&
      stats.capture_transport == platf::frame_transport_e::dmabuf &&
      stats.capture_residency == platf::frame_residency_e::gpu &&
      !stats.capture_device.empty() &&
      !config::video.adapter_name.empty() &&
      stats.capture_device != config::video.adapter_name;
#else
    return false;
#endif
  }

  nlohmann::json linux_gpu_profile_json(const stats_t &stats) {
    const auto &linux_display = config::video.linux_display;
    const bool gpu_native_requested =
      stats.runtime_gpu_native_override_active ||
      linux_display.prefer_gpu_native_capture;
    const bool adapter_matches_capture_device =
      stats.capture_device.empty() ||
      config::video.adapter_name.empty() ||
      stats.capture_device == config::video.adapter_name;
    const bool capture_metadata_reported =
      stats.capture_transport != platf::frame_transport_e::unknown ||
      stats.capture_residency != platf::frame_residency_e::unknown ||
      stats.encode_target_residency != platf::frame_residency_e::unknown;

    nlohmann::json configuration_warnings = nlohmann::json::array();
#ifdef __linux__
    if (
      linux_display.headless_mode &&
      linux_display.use_cage_compositor &&
      !linux_display.prefer_gpu_native_capture &&
      config::video.encoder == "nvenc"
    ) {
      configuration_warnings.push_back({
        {"id", "nvidia_headless_gpu_native_disabled"},
        {"severity", "warning"},
        {"message", "NVIDIA true-headless labwc is configured with GPU-native capture disabled; cold or missing encoder cache can fail launch with 503 encoder initialization even when NVENC is healthy."},
        {"action", "Set linux_prefer_gpu_native_capture = enabled, restart Polaris, and retry Private Stream (GPU-native) before chasing CUDA/NVENC driver issues."}
      });
    }
#endif

    nlohmann::json profile = {
      {"encoder_api", stats.encode_target_device},
      {"encoder_adapter", config::video.adapter_name},
      {"capture_device", stats.capture_device},
      {"adapter_matches_capture_device", adapter_matches_capture_device},
      {"cross_gpu_dmabuf_risk", capture_path_has_cross_gpu_dmabuf_risk(stats)},
      {"gpu_native_requested", gpu_native_requested},
      {"gpu_native_attempted", gpu_native_requested && capture_metadata_reported},
      {"gpu_native_succeeded", capture_path_is_gpu_native(stats)},
      {"configuration_warnings", std::move(configuration_warnings)}
    };

    return profile;
  }

  std::string capture_path_summary(const stats_t &stats) {
    const bool capture_unknown =
      stats.capture_transport == platf::frame_transport_e::unknown &&
      stats.capture_residency == platf::frame_residency_e::unknown &&
      stats.encode_target_residency == platf::frame_residency_e::unknown;
    if (capture_unknown) {
      return "unknown";
    }
    if (stats.capture_transport == platf::frame_transport_e::shm) {
      return "shm_cpu_capture";
    }
    if (stats.capture_residency == platf::frame_residency_e::cpu) {
      return "cpu_capture";
    }
    if (stats.encode_target_residency == platf::frame_residency_e::cpu) {
      return "cpu_encode_upload";
    }
    if (capture_path_is_gpu_native(stats)) {
      return "gpu_native";
    }
    if (stats.capture_transport == platf::frame_transport_e::dmabuf &&
        stats.capture_residency == platf::frame_residency_e::gpu) {
      return "gpu_capture";
    }
    return "mixed_or_unknown";
  }

  std::string capture_path_reason(const stats_t &stats) {
    const bool capture_unknown =
      stats.capture_transport == platf::frame_transport_e::unknown &&
      stats.capture_residency == platf::frame_residency_e::unknown &&
      stats.encode_target_residency == platf::frame_residency_e::unknown;
    if (capture_unknown) {
      return "no_capture_metadata";
    }

    if (capture_path_is_gpu_native(stats)) {
#ifdef __linux__
      const auto &linux_display = config::video.linux_display;
      if (stats.runtime_effective_headless && linux_display.use_cage_compositor) {
        if (capture_path_has_cross_gpu_dmabuf_risk(stats)) {
          return "headless_extcopy_dmabuf_cross_gpu_risk";
        }
        return "headless_extcopy_dmabuf";
      }
      if (stats.runtime_gpu_native_override_active) {
        return "windowed_dmabuf_override";
      }
#endif
      return "gpu_native";
    }

    const auto &linux_display = config::video.linux_display;
    const bool gpu_native_requested =
      stats.runtime_gpu_native_override_active ||
      linux_display.prefer_gpu_native_capture;

    if (stats.capture_transport == platf::frame_transport_e::shm) {
      if (gpu_native_requested) {
        return "gpu_native_requested_shm_fallback";
      }
      if (stats.runtime_effective_headless && linux_display.use_cage_compositor) {
        return "headless_shm_fallback";
      }
      return "shm_capture";
    }

    if (stats.capture_residency == platf::frame_residency_e::cpu) {
      return gpu_native_requested ? "gpu_native_requested_cpu_capture" : "cpu_capture";
    }

    if (stats.encode_target_residency == platf::frame_residency_e::cpu) {
      return gpu_native_requested ? "gpu_native_requested_cpu_encode_upload" : "encoder_upload_cpu";
    }

    if (stats.capture_transport == platf::frame_transport_e::dmabuf &&
        stats.capture_residency == platf::frame_residency_e::gpu) {
      return "dmabuf_gpu_capture";
    }

    return "mixed_or_unknown";
  }

  std::string capture_path_reason_message(const std::string &reason) {
    if (reason == "gpu_native") {
      return "Capture and encoder conversion are GPU-resident.";
    }
    if (reason == "headless_extcopy_dmabuf") {
      return "True-headless DMA-BUF capture is active; frames stay GPU-resident through the encoder path.";
    }
    if (reason == "headless_extcopy_dmabuf_cross_gpu_risk") {
      return "True-headless DMA-BUF capture is using a different DRM render node than the configured encoder adapter; Polaris should fall back to SHM/system memory to avoid known cross-GPU black video.";
    }
    if (reason == "windowed_dmabuf_override") {
      return "Polaris is using a windowed private compositor so GPU-native capture can stay GPU-resident.";
    }
    if (reason == "gpu_native_requested_shm_fallback") {
      return "GPU-native capture was requested, but the active Wayland capture fell back to SHM/system-memory frames.";
    }
    if (reason == "gpu_native_requested_cpu_capture") {
      return "GPU-native capture was requested, but the active capture frames are CPU-resident.";
    }
    if (reason == "gpu_native_requested_cpu_encode_upload") {
      return "GPU-native capture was requested, but encoder upload/conversion is still CPU-resident.";
    }
    if (reason == "headless_shm_fallback" || reason == "headless_shm_default") {
      return "Private Stream is using the conservative SHM/system-memory path; the stream can be healthy, but capable high-FPS hosts should use a GPU-native path when available.";
    }
    if (reason == "encoder_upload_cpu") {
      return "Capture is GPU-resident, but encoder upload/conversion crosses system memory.";
    }
    if (reason == "cpu_capture" || reason == "shm_capture") {
      return "The active capture path is CPU-resident.";
    }
    if (reason == "dmabuf_gpu_capture") {
      return "Capture is using DMA-BUF/GPU frames, but the encoder path is not fully GPU-native.";
    }
    if (reason == "no_capture_metadata") {
      return "No capture metadata has been reported yet.";

    }
    return "The active capture and encoder path is mixed or not fully classified.";
  }

  std::string hdr_effective_mode(const stats_t &stats) {
    if (stats.dynamic_range > 0 && stats.stream_hdr_enabled) {
      return "hdr10";
    }
    if (stats.dynamic_range > 0) {
      return "sdr_10bit";
    }
    return "sdr_8bit";
  }

  std::string hdr_downgrade_reason(const stats_t &stats) {
    if (stats.dynamic_range <= 0 || stats.stream_hdr_enabled) {
      return "none";
    }
    if (!stats.display_hdr) {
      if (stats.runtime_effective_headless) {
        return "headless_hdr_unavailable";
      }
      return "display_not_hdr";
    }
    if (!stats.hdr_metadata_available) {
      return "hdr_metadata_missing";
    }
    return "stream_hdr_disabled";
  }

  std::string hdr_downgrade_message(const stats_t &stats) {
    const auto reason = hdr_downgrade_reason(stats);
    if (reason == "none") {
      return {};
    }
    if (reason == "headless_hdr_unavailable") {
      return "The client requested HDR, but Private Stream is using a compositor output that does not report HDR. Polaris is streaming 10-bit SDR, not HDR; use a physical or virtual HDR-capable display path for true HDR.";
    }
    if (reason == "display_not_hdr") {
      return "The client requested HDR, but the active capture display is not reporting HDR. Polaris is streaming 10-bit SDR, not HDR.";
    }
    if (reason == "hdr_metadata_missing") {
      return "The client requested HDR, but the active capture display did not expose HDR10 metadata. Polaris is streaming 10-bit SDR, not HDR.";
    }
    return "The client requested HDR, but Polaris did not advertise HDR for this stream. Polaris is streaming 10-bit SDR, not HDR.";
  }

  namespace {
    bool doctor_has_capture_metadata(const stats_t &stats) {
      return stats.capture_transport != platf::frame_transport_e::unknown ||
        stats.capture_residency != platf::frame_residency_e::unknown ||
        stats.capture_format != platf::frame_format_e::unknown ||
        !stats.capture_device.empty();
    }

    double doctor_target_fps(const stats_t &stats) {
      if (stats.encode_target_fps > 0.0) return stats.encode_target_fps;
      if (stats.session_target_fps > 0.0) return stats.session_target_fps;
      if (stats.requested_client_fps > 0.0) return stats.requested_client_fps;
      return stats.fps;
    }

    void append_doctor_evidence(nlohmann::json &evidence,
                                const std::string &id,
                                const std::string &label,
                                const nlohmann::json &value,
                                const std::string &unit,
                                const std::string &status,
                                const std::string &source,
                                const std::string &detail) {
      evidence.push_back({
        {"id", id},
        {"label", label},
        {"value", value},
        {"unit", unit},
        {"status", status},
        {"source", source},
        {"redacted", false},
        {"detail", detail}
      });
    }

    nlohmann::json doctor_recommendation(const std::string &primary_issue,
                                         const std::string &summary,
                                         const nlohmann::json &health) {
      std::string title = "Try this first";
      std::string body = "Start a stream, reproduce the issue, then export diagnostics with this Doctor result attached.";
      std::string next_step = "Export diagnostics";
      std::string expected = "Support gets deterministic telemetry instead of guesswork.";

      if (primary_issue == "none") {
        title = "Keep playing";
        body = "Streaming telemetry looks ready. Keep this page open if you are trying to catch an intermittent problem.";
        next_step = "Keep monitoring";
        expected = "No recovery action should be needed right now.";
      } else if (primary_issue == "network_jitter") {
        body = "Lower bitrate one step or keep Adaptive Bitrate enabled before changing encoder settings.";
        next_step = "Lower bitrate";
        expected = "Packet loss and latency spikes should calm down if bandwidth is the bottleneck.";
      } else if (primary_issue == "encoder_load") {
        body = "Trim bitrate, resolution, or FPS to give the active encoder more frame time.";
        next_step = "Lower stream load";
        expected = "Encode time should fall back under the low-latency budget.";
      } else if (primary_issue == "host_render_limited") {
        body = "Lower the game render preset, render resolution, or stream FPS before tuning bitrate.";
        next_step = "Lower game/FPS target";
        expected = "Game frames should arrive on pace with the stream target.";
      } else if (primary_issue == "capture_missing" || primary_issue == "no_active_stream") {
        body = "Start or resume the affected stream so Polaris can classify the real capture and encode path.";
        next_step = "Start stream";
        expected = "Doctor can switch from readiness hints to live telemetry evidence.";
      } else if (primary_issue.find("capture") != std::string::npos ||
                 primary_issue.find("shm") != std::string::npos ||
                 primary_issue.find("dmabuf") != std::string::npos ||
                 primary_issue == "nvenc_cuda_disabled") {
        body = health.value("summary", summary);
        next_step = "Review capture evidence";
        expected = "Advanced evidence will show whether the host is on GPU-native, DMA-BUF, SHM/system-memory, or CPU-copy fallback.";
      } else {
        body = health.value("summary", summary);
        next_step = health.value("relaunch_recommended", false) ? "Plan safe relaunch" : "Open Advanced";
        expected = "The recommended change should target the loudest deterministic signal first.";
      }

      return {
        {"title", title},
        {"body", body},
        {"why", summary},
        {"next_step_label", next_step},
        {"expected_effect", expected}
      };
    }

    nlohmann::json doctor_safe_action(const std::string &primary_issue, const nlohmann::json &health) {
      std::string id = "none";
      std::string label = "No automatic action";
      std::string kind = "none";
      std::string endpoint;
      std::string method;
      nlohmann::json payload = nlohmann::json::object();
      std::string rollback = "No change is applied by Doctor.";

      if (primary_issue == "network_jitter" || primary_issue == "encoder_load") {
        id = "lower_bitrate";
        label = "Apply safer bitrate";
        kind = "live_tuning";
        endpoint = "/polaris/v1/client-settings";
        method = "POST";
        payload["target_bitrate_kbps"] = health.value("safe_bitrate_kbps", 0);
        rollback = "Raise bitrate again from the client or Polaris stream settings.";
      } else if (primary_issue == "no_active_stream" || primary_issue == "capture_missing") {
        id = "export_support_bundle";
        label = "Export diagnostics";
        kind = "export";
        rollback = "Export only; no host settings are changed.";
      } else if (health.value("relaunch_recommended", false)) {
        id = "apply_recovery_profile_next_launch";
        label = "Use safer next launch";
        kind = "next_launch_profile";
        rollback = "Restore the previous display, HDR, codec, FPS, or bitrate settings before launching again.";
        if (health.contains("safe_display_mode")) payload["display_mode"] = health["safe_display_mode"];
        if (health.contains("safe_bitrate_kbps")) payload["target_bitrate_kbps"] = health["safe_bitrate_kbps"];
        if (health.contains("safe_target_fps")) payload["target_fps"] = health["safe_target_fps"];
        if (health.contains("safe_codec")) payload["preferred_codec"] = health["safe_codec"];
        if (health.contains("safe_hdr")) payload["hdr"] = health["safe_hdr"];
      }

      return {
        {"id", id},
        {"label", label},
        {"kind", kind},
        {"destructive", false},
        {"requires_confirmation", id != "none"},
        {"requires_owner", id != "none"},
        {"allowed_in_viewer_mode", id == "export_support_bundle"},
        {"endpoint", endpoint},
        {"method", method},
        {"payload_preview", payload},
        {"rollback", rollback}
      };
    }
  }  // namespace

  nlohmann::json build_doctor_json(const stats_t &stats, const nlohmann::json &health) {
    const auto capture_reason = capture_path_reason(stats);
    const auto capture_path = capture_path_summary(stats);
    const bool capture_cpu_copy = capture_path_uses_cpu_copy(stats);
    const bool capture_gpu_native = capture_path_is_gpu_native(stats);
    const bool capture_known = doctor_has_capture_metadata(stats);
    const double target_fps = doctor_target_fps(stats);
    const double fps_gap = target_fps > 0.0 ? std::max(0.0, target_fps - stats.fps) : 0.0;
    const bool network_fail = stats.packet_loss > 2.0 || stats.latency_ms >= 45.0;
    const bool network_watch = !network_fail && (stats.packet_loss > 0.5 || stats.latency_ms >= 28.0);
    const bool encoder_fail = stats.encode_time_ms >= 12.0 || stats.avg_frame_age_ms >= 22.0;
    const bool encoder_watch = !encoder_fail && (stats.encode_time_ms >= 8.0 || stats.avg_frame_age_ms >= 18.0);
    const bool pacing_watch = stats.frame_jitter_ms >= 2.2 || stats.duplicate_frame_ratio >= 0.10 || stats.dropped_frame_ratio >= 0.04 || fps_gap >= 4.0;

    std::string primary_issue = health.value("primary_issue", std::string {});
    if (primary_issue == "steady") primary_issue = "none";
    if (primary_issue.empty()) {
      if (!stats.streaming) primary_issue = "no_active_stream";
      else if (network_fail || network_watch) primary_issue = "network_jitter";
      else if (encoder_fail || encoder_watch) primary_issue = "encoder_load";
      else if (capture_cpu_copy) primary_issue = capture_reason;
      else if (!capture_known) primary_issue = "capture_missing";
      else if (pacing_watch) primary_issue = "frame_pacing";
      else primary_issue = "none";
    }

    std::string traffic = "green";
    std::string status = "ok";
    std::string severity = "info";
    std::string simple_state = "Streaming ready";
    const auto health_grade = health.value("grade", std::string {});
    if (primary_issue == "no_active_stream" || primary_issue == "capture_missing") {
      traffic = "amber";
      status = "unknown";
      severity = "warning";
      simple_state = "Needs attention";
    } else if (health_grade == "degraded" || network_fail || encoder_fail) {
      traffic = "red";
      status = "needs_action";
      severity = "critical";
      simple_state = "Needs attention";
    } else if (primary_issue != "none" || health_grade == "watch" || capture_cpu_copy || pacing_watch) {
      traffic = "amber";
      status = capture_cpu_copy ? "watch" : "needs_action";
      severity = "warning";
      simple_state = capture_cpu_copy ? "Advanced issue detected" : "Needs attention";
    }

    const std::string summary =
      primary_issue == "none" ? "Streaming telemetry looks ready." :
      health.contains("summary") ? health.value("summary", std::string {}) :
      primary_issue == "no_active_stream" ? "No active stream is running, so Doctor cannot verify the live path yet." :
      primary_issue == "capture_missing" ? "Capture metadata has not arrived yet; start a stream before tuning advanced settings." :
      primary_issue == "network_jitter" ? "Network jitter is the most likely stream issue." :
      primary_issue == "encoder_load" ? "Encoder load is above the low-latency budget." :
      primary_issue == "frame_pacing" ? "Frame pacing telemetry needs attention." :
      capture_path_reason_message(capture_reason);

    nlohmann::json evidence = nlohmann::json::array();
    append_doctor_evidence(evidence, "streaming", "Active stream", stats.streaming, "", stats.streaming ? "pass" : "unknown", "stream_stats", stats.streaming ? "A stream is active." : "No active stream is reporting live telemetry.");
    append_doctor_evidence(evidence, "capture_path", "Capture path", capture_path, "", !capture_known ? "unknown" : capture_cpu_copy ? "watch" : capture_gpu_native ? "pass" : "watch", "stream_stats", capture_path_reason_message(capture_reason));
    append_doctor_evidence(evidence, "encoder", "Encoder", stats.encode_target_device, "", encoder_fail ? "fail" : encoder_watch ? "watch" : "pass", "stream_stats", stats.encode_time_ms > 0.0 ? "Encode timing is reported by stream telemetry." : "Encoder timing has not been reported yet.");
    append_doctor_evidence(evidence, "packet_loss", "Packet loss", stats.packet_loss, "%", network_fail ? "fail" : network_watch ? "watch" : "pass", "stream_stats", "Packet loss reported by current stream telemetry.");
    append_doctor_evidence(evidence, "frame_pacing", "Frame pacing", stats.frame_jitter_ms, "ms jitter", pacing_watch ? "watch" : "pass", "stream_stats", "Frame jitter, duplicate/drop ratios, and target FPS gap classify pacing risk.");

    auto advanced = nlohmann::json::object();
    advanced["stream_stats_keys"] = nlohmann::json::array({"capture_path", "capture_path_reason", "capture_transport", "capture_residency", "capture_format", "encode_target_device", "encode_target_residency", "fps", "encode_time_ms", "packet_loss", "frame_jitter_ms"});
    advanced["capture_decision"] = {
      {"path", capture_path},
      {"reason", capture_reason},
      {"reason_message", capture_path_reason_message(capture_reason)},
      {"transport", platf::from_frame_transport(stats.capture_transport)},
      {"residency", platf::from_frame_residency(stats.capture_residency)},
      {"format", platf::from_frame_format(stats.capture_format)},
      {"capture_device", stats.capture_device},
      {"encoder_adapter", config::video.adapter_name},
      {"cross_gpu_dmabuf_risk", capture_path_has_cross_gpu_dmabuf_risk(stats)},
      {"cpu_copy", capture_cpu_copy},
      {"gpu_native", capture_gpu_native}
    };
    advanced["linux_gpu_profile"] = linux_gpu_profile_json(stats);
    advanced["health"] = health;
    advanced["recent_issue_codes"] = nlohmann::json::array();
    advanced["raw_fields_redacted"] = true;

    double confidence_score = 0.35;
    std::string confidence_level = "low";
    std::string basis = "insufficient_data";
    if (health.contains("primary_issue")) {
      confidence_score = 0.92;
      confidence_level = "high";
      basis = "direct_telemetry";
    } else if (stats.streaming && capture_known) {
      confidence_score = 0.78;
      confidence_level = "medium";
      basis = "direct_telemetry";
    } else if (!stats.streaming) {
      confidence_score = 0.25;
      confidence_level = "unknown";
      basis = "insufficient_data";
    }

    nlohmann::json doctor;
    doctor["version"] = 1;
    doctor["result_id"] = "doctor-v1-" + status + "-" + primary_issue + "-" + capture_reason;
    doctor["scope"] = "stream";
    doctor["status"] = status;
    doctor["traffic_light"] = traffic;
    doctor["status_color"] = traffic;
    doctor["severity"] = severity;
    doctor["simple_state"] = simple_state;
    doctor["primary_issue"] = primary_issue;
    doctor["confidence"] = {{"level", confidence_level}, {"score", confidence_score}, {"basis", basis}, {"sample_window", {{"samples", stats.streaming ? 1 : 0}, {"seconds", stats.streaming ? 1 : 0}}}};
    doctor["summary"] = summary;
    doctor["recommendation"] = doctor_recommendation(primary_issue, summary, health);
    doctor["evidence"] = std::move(evidence);
    doctor["advanced_evidence"] = std::move(advanced);
    doctor["safe_recovery_action"] = doctor_safe_action(primary_issue, health);
    doctor["redaction"] = {{"policy", "polaris-diagnostics-redaction-v1"}, {"applied", true}, {"redacted_fields", nlohmann::json::array()}, {"notice", "Tokens, cookies, credentials, auth headers, client IPs, and sensitive config fields are redacted before export or AI explanation."}};
    doctor["ai_explanation"] = {{"enabled", false}, {"provider", "none"}, {"model", ""}, {"generated_at", nullptr}, {"input_redacted", true}, {"source_result_id", doctor["result_id"]}, {"summary", ""}, {"limits", nlohmann::json::array({"AI can explain only; deterministic Doctor owns status, evidence, and actions."})}, {"error", ""}};
    return doctor;
  }

  void update_stream_active(bool active, const std::string &client_name, const std::string &client_ip) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    current_stats.streaming = active;
    if (active) {
      current_stats.client_name = client_name;
      current_stats.client_ip = client_ip;
    } else {
      // Reset all stats when stream ends
      current_stats = stats_t {};
      clear_capture_profile_buckets();
    }
  }

  void add_client(const std::string &client_ip, const std::string &client_name) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    // Check if client already exists
    auto it = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [&client_ip](const client_stats_t &c) { return c.ip == client_ip; });

    if (it == current_stats.clients.end()) {
      client_stats_t client;
      client.name = client_name;
      client.ip = client_ip;
      current_stats.clients.push_back(std::move(client));
    }

    // Always mark streaming as active and update primary client info
    current_stats.streaming = true;
    if (current_stats.clients.size() == 1) {
      current_stats.client_name = client_name;
      current_stats.client_ip = client_ip;
    }
  }

  void remove_client(const std::string &client_ip) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    current_stats.clients.erase(
      std::remove_if(current_stats.clients.begin(), current_stats.clients.end(),
        [&client_ip](const client_stats_t &c) { return c.ip == client_ip; }),
      current_stats.clients.end());

    if (current_stats.clients.empty()) {
      current_stats.streaming = false;
      current_stats.client_name.clear();
      current_stats.client_ip.clear();
    } else {
      // Update primary client info to first remaining client
      current_stats.client_name = current_stats.clients.front().name;
      current_stats.client_ip = current_stats.clients.front().ip;
    }
  }

  void update_video_stats(double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    current_stats.fps = fps;
    current_stats.bitrate_kbps = bitrate_kbps;
    current_stats.encode_time_ms = encode_time_ms;
    current_stats.codec = codec;
    current_stats.width = width;
    current_stats.height = height;

    // Also update first client if any
    if (!current_stats.clients.empty()) {
      auto &c = current_stats.clients.front();
      c.fps = fps;
      c.bitrate_kbps = bitrate_kbps;
      c.encode_time_ms = encode_time_ms;
      c.codec = codec;
      c.width = width;
      c.height = height;
    }
  }

  void update_video_stats(const std::string &client_ip, double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    auto it = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [&client_ip](const client_stats_t &c) { return c.ip == client_ip; });

    if (it != current_stats.clients.end()) {
      it->fps = fps;
      it->bitrate_kbps = bitrate_kbps;
      it->encode_time_ms = encode_time_ms;
      it->codec = codec;
      it->width = width;
      it->height = height;
    }

    // Also update top-level stats (use first client for backward compat)
    if (!current_stats.clients.empty() && current_stats.clients.front().ip == client_ip) {
      current_stats.fps = fps;
      current_stats.bitrate_kbps = bitrate_kbps;
      current_stats.encode_time_ms = encode_time_ms;
      current_stats.codec = codec;
      current_stats.width = width;
      current_stats.height = height;
    }
  }

  void update_session_targets(double requested_client_fps,
                              double session_target_fps,
                              double encode_target_fps,
                              const std::string &pacing_policy,
                              const std::string &optimization_source,
                              const std::string &optimization_confidence,
                              const std::string &optimization_cache_status,
                              const std::string &optimization_reasoning,
                              const std::string &optimization_normalization_reason,
                              int recommendation_version) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    current_stats.requested_client_fps = requested_client_fps;
    current_stats.session_target_fps = session_target_fps;
    current_stats.encode_target_fps = encode_target_fps;
    current_stats.pacing_policy = pacing_policy;
    current_stats.optimization_source = optimization_source;
    current_stats.optimization_confidence = optimization_confidence;
    current_stats.optimization_cache_status = optimization_cache_status;
    current_stats.optimization_reasoning = optimization_reasoning;
    current_stats.optimization_normalization_reason = optimization_normalization_reason;
    current_stats.recommendation_version = recommendation_version;
  }

  void update_frame_delivery(double duplicate_frame_ratio,
                             double dropped_frame_ratio,
                             double avg_frame_age_ms,
                             double frame_jitter_ms) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    current_stats.duplicate_frame_ratio = duplicate_frame_ratio;
    current_stats.dropped_frame_ratio = dropped_frame_ratio;
    current_stats.avg_frame_age_ms = avg_frame_age_ms;
    current_stats.frame_jitter_ms = frame_jitter_ms;
  }

  void update_network_stats(double latency_ms, double packet_loss, uint64_t bytes_sent) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    current_stats.latency_ms = latency_ms;
    current_stats.packet_loss = packet_loss;
    current_stats.bytes_sent = bytes_sent;
  }

  void update_network_stats(const std::string &client_ip, double latency_ms, double packet_loss, uint64_t bytes_sent) {
    std::lock_guard<std::mutex> lock(stats_mutex);

    auto it = std::find_if(current_stats.clients.begin(), current_stats.clients.end(),
      [&client_ip](const client_stats_t &c) { return c.ip == client_ip; });

    if (it != current_stats.clients.end()) {
      it->latency_ms = latency_ms;
      it->packet_loss = packet_loss;
      it->bytes_sent = bytes_sent;
    }

    // Also update top-level stats (use first client for backward compat)
    if (!current_stats.clients.empty() && current_stats.clients.front().ip == client_ip) {
      current_stats.latency_ms = latency_ms;
      current_stats.packet_loss = packet_loss;
      current_stats.bytes_sent = bytes_sent;
    }
  }

  void update_runtime_state(const platf::runtime_state_t &state) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.runtime_backend = state.backend_name;
    current_stats.runtime_requested_headless = state.requested_headless;
    current_stats.runtime_effective_headless = state.effective_headless;
    current_stats.runtime_gpu_native_override_active = state.gpu_native_override_active;
  }

  void update_capture_metadata(const platf::frame_metadata_t &metadata) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.capture_transport = metadata.transport;
    current_stats.capture_residency = metadata.residency;
    current_stats.capture_format = metadata.format;
    current_stats.capture_device = metadata.device;
  }

  void update_encode_path_metadata(const std::string &target_device,
                                   platf::frame_residency_e target_residency,
                                   platf::frame_format_e target_format) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.encode_target_device = target_device;
    current_stats.encode_target_residency = target_residency;
    current_stats.encode_target_format = target_format;
  }

  void update_dynamic_range(int dynamic_range) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.dynamic_range = dynamic_range;
  }

  void update_hdr_state(bool display_hdr,
                        bool hdr_metadata_available,
                        bool stream_hdr_enabled,
                        const std::string &color_coding) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.display_hdr = display_hdr;
    current_stats.hdr_metadata_available = hdr_metadata_available;
    current_stats.stream_hdr_enabled = stream_hdr_enabled;
    current_stats.color_coding = color_coding;
  }

  void update_controller_input_state(bool virtual_controller_created,
                                     int virtual_controller_number,
                                     const std::string &virtual_controller_kind,
                                     const std::string &virtual_controller_error,
                                     const std::string &host_controller_isolation,
                                     const std::string &host_controller_isolation_detail,
                                     bool haptics_supported,
                                     const std::string &haptics_detail) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.input_virtual_controller_created = virtual_controller_created;
    current_stats.input_virtual_controller_number = virtual_controller_number;
    current_stats.input_virtual_controller_kind = virtual_controller_kind;
    current_stats.input_virtual_controller_error = virtual_controller_error;
    current_stats.input_host_controller_isolation = host_controller_isolation.empty() ? "unknown" : host_controller_isolation;
    current_stats.input_host_controller_isolation_detail = host_controller_isolation_detail;
    current_stats.input_haptics_supported = haptics_supported;
    current_stats.input_haptics_detail = haptics_detail;
  }

  void update_capture_profile(const capture_profile_sample_t &sample) {
    std::lock_guard<std::mutex> lock(stats_mutex);
    auto &bucket = capture_profile_bucket(sample.transport);
    bucket.dispatch_us.push_back(sample.dispatch_time.count());
    bucket.ingest_us.push_back(sample.ingest_time.count());
    bucket.total_us.push_back(sample.total_time.count());

    if (bucket.total_us.size() < CAPTURE_PROFILE_SUMMARY_FRAMES) {
      return;
    }

    BOOST_LOG(info) << "capture_telemetry: transport="sv << platf::from_frame_transport(sample.transport)
                    << " frames="sv << bucket.total_us.size()
                    << " dispatch_us_p50="sv << percentile_value(bucket.dispatch_us, 0.50)
                    << " dispatch_us_p99="sv << percentile_value(bucket.dispatch_us, 0.99)
                    << " ingest_us_p50="sv << percentile_value(bucket.ingest_us, 0.50)
                    << " ingest_us_p99="sv << percentile_value(bucket.ingest_us, 0.99)
                    << " total_us_p50="sv << percentile_value(bucket.total_us, 0.50)
                    << " total_us_p99="sv << percentile_value(bucket.total_us, 0.99);

    bucket.dispatch_us.clear();
    bucket.ingest_us.clear();
    bucket.total_us.clear();
  }

  int active_client_count() {
    std::lock_guard<std::mutex> lock(stats_mutex);
    return static_cast<int>(current_stats.clients.size());
  }

  stats_t get_current() {
    std::lock_guard<std::mutex> lock(stats_mutex);
    current_stats.adaptive_target_bitrate_kbps = adaptive_bitrate::get_target_bitrate_kbps();

    // Also update adaptive bitrate for all clients
    for (auto &c : current_stats.clients) {
      c.adaptive_target_bitrate_kbps = adaptive_bitrate::get_target_bitrate_kbps();
    }

    return current_stats;
  }

}  // namespace stream_stats
