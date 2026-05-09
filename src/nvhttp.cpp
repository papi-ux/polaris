/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <string>

// lib includes (JSON for last-launched persistence)
#include <nlohmann/json.hpp>

// lib includes
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "stream.h"
#include "system_tray.h"
#include "utility.h"
#include "adaptive_bitrate.h"
#include "ai_optimizer.h"
#include "device_db.h"
#include "confighttp.h"
#include "stream_stats.h"
#include "video.h"
#ifdef __linux__
  #include "platform/linux/cage_display_router.h"
  #include "platform/linux/session_manager.h"
  #include "platform/linux/stream_display_policy.h"
  #include "platform/linux/virtual_display.h"
#endif
#include "uuid.h"
#include "zwpad.h"

#ifdef _WIN32
  #include "platform/windows/virtual_display.h"
#endif

using namespace std::literals;

namespace nvhttp {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  namespace {
    bool ipv6_prefix_matches(const boost::asio::ip::address_v6 &client,
                             const boost::asio::ip::address_v6 &network,
                             unsigned short prefix) {
      if (prefix > 128) {
        return false;
      }

      const auto client_bytes = client.to_bytes();
      const auto network_bytes = network.to_bytes();
      const auto full_bytes = static_cast<std::size_t>(prefix / 8);
      const auto remaining_bits = static_cast<unsigned short>(prefix % 8);

      if (!std::equal(client_bytes.begin(), client_bytes.begin() + full_bytes, network_bytes.begin())) {
        return false;
      }

      if (remaining_bits == 0) {
        return true;
      }

      const auto mask = static_cast<unsigned char>(0xFFu << (8u - remaining_bits));
      return (client_bytes[full_bytes] & mask) == (network_bytes[full_bytes] & mask);
    }

    void append_optimization_json(nlohmann::json &output, const device_db::optimization_t &optimization) {
      if (optimization.display_mode) {
        output["display_mode"] = *optimization.display_mode;
      }
      if (optimization.color_range) {
        output["color_range"] = *optimization.color_range;
      }
      if (optimization.hdr.has_value()) {
        output["hdr"] = *optimization.hdr;
      }
      if (optimization.virtual_display.has_value()) {
        output["virtual_display"] = *optimization.virtual_display;
      }
      if (optimization.target_bitrate_kbps) {
        output["target_bitrate_kbps"] = *optimization.target_bitrate_kbps;
      }
      if (optimization.nvenc_tune) {
        output["nvenc_tune"] = *optimization.nvenc_tune;
      }
      if (optimization.preferred_codec) {
        output["preferred_codec"] = *optimization.preferred_codec;
      }
      output["reasoning"] = optimization.reasoning;
      output["reasoning_summary"] = optimization.reasoning;
      output["source"] = optimization.source;
      output["cache_status"] = optimization.cache_status;
      output["confidence"] = optimization.confidence;
      output["signals_used"] = optimization.signals_used;
      output["normalization_reason"] = optimization.normalization_reason;
      output["recommendation_version"] = optimization.recommendation_version;
      output["generated_at"] = optimization.generated_at;
      output["expires_at"] = optimization.expires_at;
    }

    std::string lower_copy(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return value;
    }

    bool is_poor_quality_grade(const std::string &grade) {
      const auto normalized = lower_copy(grade);
      return normalized == "d" || normalized == "f";
    }

    std::string latest_quality_grade(const std::optional<ai_optimizer::session_history_t> &history) {
      if (!history) {
        return {};
      }
      if (!history->last_quality_grade.empty()) {
        return history->last_quality_grade;
      }
      return history->quality_grade;
    }

    bool host_virtual_display_available() {
#ifdef __linux__
      return virtual_display::is_available();
#elif defined(_WIN32)
      return
        vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK ||
        vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::UNKNOWN;
#else
      return false;
#endif
    }

    bool host_prefers_headless() {
#ifdef __linux__
      return stream_display_policy::resolve(stream_display_policy::input_t {}).mode == stream_display_policy::mode_e::HEADLESS;
#else
      return false;
#endif
    }

    bool build_has_cuda() {
#ifdef POLARIS_BUILD_CUDA
      return true;
#else
      return false;
#endif
    }

    bool is_mobile_client_type(const std::optional<device_db::device_t> &device_profile);

    std::string bool_config_value(bool enabled) {
      return enabled ? "enabled"s : "disabled"s;
    }

    bool persist_config_values(const std::unordered_map<std::string, std::string> &updates) {
      if (updates.empty()) {
        return true;
      }

      auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      for (const auto &[key, value] : updates) {
        vars[key] = value;
      }

      std::stringstream config_stream;
      for (const auto &[key, value] : vars) {
        if (value.empty()) {
          continue;
        }
        config_stream << key << " = " << value << std::endl;
      }

      if (file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str()) != 0) {
        BOOST_LOG(error) << "client_settings: failed to write config file: "sv << config::sunshine.config_file;
        return false;
      }

      return true;
    }

    std::string configured_stream_display_mode_selection() {
      const auto &linux_display = config::video.linux_display;
      if (!linux_display.headless_mode) {
        return "desktop_display";
      }
      if (!linux_display.use_cage_compositor) {
        return "host_virtual_display";
      }
      if (linux_display.prefer_gpu_native_capture) {
        return "windowed_stream";
      }
      return "headless_stream";
    }

    std::string effective_stream_display_mode_selection(const stream_stats::stats_t &stats) {
      if (!stats.streaming) {
        return configured_stream_display_mode_selection();
      }
      if (stats.runtime_gpu_native_override_active) {
        return "windowed_stream";
      }
      if (proc::proc.virtual_display) {
        return "host_virtual_display";
      }
      if (stats.runtime_effective_headless) {
        return "headless_stream";
      }
      return "desktop_display";
    }

    std::string stream_display_mode_label_for_selection(const std::string &selection) {
      if (selection == "headless_stream") {
        return "Headless Stream";
      }
      if (selection == "host_virtual_display") {
        return "Host Virtual Display";
      }
      if (selection == "windowed_stream") {
        return "GPU-Native Test";
      }
      return "Desktop Display";
    }

    std::string stream_display_mode_reason_for_selection(const std::string &selection) {
      if (selection == "headless_stream") {
        return "Polaris will stream from the private headless compositor runtime.";
      }
      if (selection == "host_virtual_display") {
        return host_virtual_display_available() ?
          "Polaris will create or use a host virtual display for this stream." :
          "Polaris requested a host virtual display, but no backend is currently available.";
      }
      if (selection == "windowed_stream") {
        return "Polaris may run the private compositor windowed when that is needed to keep capture GPU-native.";
      }
      return "Polaris will stream from the current desktop session.";
    }

    bool apply_stream_display_mode_selection(const std::string &selection,
                                             std::string &error) {
      bool headless = false;
      bool cage = false;
      bool gpu_native = false;

      if (selection == "headless_stream") {
        headless = true;
        cage = true;
      } else if (selection == "host_virtual_display") {
        headless = true;
      } else if (selection == "desktop_display") {
        // Defaults already describe desktop display.
      } else if (selection == "windowed_stream") {
        headless = true;
        cage = true;
        gpu_native = true;
      } else {
        error = "stream_display_mode must be headless_stream, desktop_display, host_virtual_display, or windowed_stream";
        return false;
      }

      config::video.linux_display.headless_mode = headless;
      config::video.linux_display.use_cage_compositor = cage;
      config::video.linux_display.prefer_gpu_native_capture = gpu_native;

      if (!persist_config_values({
            {"headless_mode", bool_config_value(headless)},
            {"linux_use_cage_compositor", bool_config_value(cage)},
            {"linux_prefer_gpu_native_capture", bool_config_value(gpu_native)}
          })) {
        error = "failed to persist stream display mode";
        return false;
      }

      return true;
    }

    nlohmann::json stream_display_mode_options_json() {
      nlohmann::json modes = nlohmann::json::array();
      for (const auto &selection : {
             "headless_stream"s,
             "desktop_display"s,
             "host_virtual_display"s,
             "windowed_stream"s
           }) {
        modes.push_back({
          {"value", selection},
          {"label", stream_display_mode_label_for_selection(selection)},
          {"available", selection != "host_virtual_display" || host_virtual_display_available()},
          {"restart_required", true},
          {"reason", stream_display_mode_reason_for_selection(selection)}
        });
      }
      return modes;
    }

    nlohmann::json build_client_settings_sync_status(const crypto::named_cert_t &client,
                                                     const stream_stats::stats_t &stats,
                                                     const nlohmann::json &policy,
                                                     const std::string &configured_mode,
                                                     const std::string &effective_mode,
                                                     bool relaunch_required) {
      const bool paired_display_override = !client.display_mode.empty();
      const bool paired_bitrate_override = client.target_bitrate_kbps > 0;
      const auto effective_display_mode = policy.value("selected_display_mode", std::string {});
      const auto effective_bitrate_kbps = policy.value("target_bitrate_kbps", 0);
      const bool adaptive_active = adaptive_bitrate::is_enabled() && stats.adaptive_target_bitrate_kbps > 0;

      nlohmann::json fields = nlohmann::json::object();
      fields["stream_display_mode"] = {
        {"direction", "read_write"},
        {"scope", "host"},
        {"desired", configured_mode},
        {"effective", effective_mode},
        {"status", relaunch_required ? "pending_relaunch" : "synced"},
        {"live", false},
        {"requires_relaunch", relaunch_required}
      };
      fields["display_mode"] = {
        {"direction", "read_write"},
        {"scope", "paired_client"},
        {"desired", client.display_mode},
        {"effective", effective_display_mode},
        {"status", paired_display_override && stats.streaming && effective_display_mode != client.display_mode ? "pending_next_launch" : "synced"},
        {"live", false},
        {"requires_relaunch", paired_display_override && stats.streaming && effective_display_mode != client.display_mode}
      };
      fields["target_bitrate_kbps"] = {
        {"direction", "read_write"},
        {"scope", "paired_client"},
        {"desired", client.target_bitrate_kbps},
        {"effective", effective_bitrate_kbps},
        {"status", adaptive_active ? "adaptive_active" : "synced"},
        {"live", true},
        {"requires_relaunch", false},
        {"adaptive_target_bitrate_kbps", stats.adaptive_target_bitrate_kbps},
        {"paired_override_active", paired_bitrate_override}
      };
      fields["adaptive_bitrate_enabled"] = {
        {"direction", "read_write"},
        {"scope", "host"},
        {"desired", adaptive_bitrate::is_enabled()},
        {"effective", adaptive_bitrate::is_enabled()},
        {"status", "synced"},
        {"live", true},
        {"requires_relaunch", false}
      };
      fields["ai_optimizer_enabled"] = {
        {"direction", "read_write"},
        {"scope", "host"},
        {"desired", ai_optimizer::is_enabled()},
        {"effective", ai_optimizer::is_enabled()},
        {"status", "synced"},
        {"live", true},
        {"requires_relaunch", false}
      };

      nlohmann::json status;
      status["available"] = true;
      status["version"] = 1;
      status["direction"] = "bidirectional";
      status["endpoint"] = "/polaris/v1/client-settings";
      status["source_of_truth"] = "polaris_effective_runtime";
      status["state"] =
        relaunch_required ? "pending_relaunch" :
        adaptive_active ? "adaptive_active" :
        "synced";
      status["message"] =
        relaunch_required ?
          "Desired settings are saved and will become effective after the active stream relaunches." :
        adaptive_active ?
          "Adaptive bitrate is enabled; Polaris is adjusting the effective bitrate in real time under the saved paired-client limit." :
          "Desired settings match the current Polaris runtime state.";
      status["fields"] = std::move(fields);
      return status;
    }

    double stream_policy_target_fps(const stream_stats::stats_t &stats) {
      if (stats.session_target_fps > 0.0) {
        return stats.session_target_fps;
      }
      if (stats.encode_target_fps > 0.0) {
        return stats.encode_target_fps;
      }
      if (stats.requested_client_fps > 0.0) {
        return stats.requested_client_fps;
      }
      return stats.fps;
    }

    std::string format_stream_policy_fps(double fps) {
      if (fps <= 0.0) {
        return {};
      }
      const auto rounded = std::round(fps);
      if (std::abs(fps - rounded) < 0.01) {
        return std::to_string(static_cast<int>(rounded));
      }
      return std::format("{:.2f}", fps);
    }

    std::string format_stream_policy_display_mode(int width, int height, double fps) {
      if (width <= 0 || height <= 0) {
        return {};
      }

      const auto fps_text = format_stream_policy_fps(fps);
      if (fps_text.empty()) {
        return std::to_string(width) + "x" + std::to_string(height);
      }

      return std::to_string(width) + "x" + std::to_string(height) + "x" + fps_text;
    }

    bool parse_stream_policy_display_mode(const std::string &value, int &width, int &height, double &fps) {
      std::stringstream mode(value);
      std::string segment;
      int index = 0;
      width = 0;
      height = 0;
      fps = 0.0;

      while (std::getline(mode, segment, 'x')) {
        if (segment.empty()) {
          return false;
        }

        try {
          if (index == 0) {
            width = std::stoi(segment);
          } else if (index == 1) {
            height = std::stoi(segment);
          } else if (index == 2) {
            fps = std::stod(segment);
          } else {
            return false;
          }
        } catch (...) {
          return false;
        }
        ++index;
      }

      return index == 3 && width > 0 && height > 0 && fps > 0.0;
    }

    std::string stream_policy_source_label(const std::string &source) {
      const auto normalized = lower_copy(source);
      if (normalized == "paired_client") {
        return "Paired client override";
      }
      if (normalized.find("ai_live") != std::string::npos) {
        return "Live AI recommendation";
      }
      if (normalized.find("ai_cached") != std::string::npos) {
        return "Cached AI recommendation";
      }
      if (normalized.find("device_db") != std::string::npos) {
        return "Device profile";
      }
      if (normalized.find("runtime_policy") != std::string::npos) {
        return "Runtime policy";
      }
      if (normalized == "default") {
        return "Server default";
      }
      return source.empty() ? "Server default" : source;
    }

    void append_stream_policy_warning(nlohmann::json &warnings,
                                      const std::string &code,
                                      const std::string &message) {
      warnings.push_back({
        {"code", code},
        {"message", message}
      });
    }

    std::string capture_path_reason_message(const std::string &reason) {
      if (reason == "gpu_native") {
        return "Capture and encoder conversion are GPU-resident.";
      }
      if (reason == "headless_extcopy_dmabuf") {
        return "True-headless DMA-BUF capture is active; frames stay GPU-resident through the encoder path.";
      }
      if (reason == "windowed_dmabuf_override") {
        return "Polaris is using a windowed private compositor so DMA-BUF/CUDA capture can stay GPU-resident.";
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
        return "Headless Stream is using the conservative SHM/system-memory path; the stream can be healthy, but high-FPS NVIDIA testing should use a CUDA-enabled GPU-native path.";
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

    nlohmann::json build_stream_policy_json(const crypto::named_cert_t &client,
                                            const stream_stats::stats_t &stats,
                                            const nlohmann::json &health) {
      const auto device_profile = device_db::get_device(client.name);
      const bool paired_display_override = !client.display_mode.empty();
      const bool paired_bitrate_override = client.target_bitrate_kbps > 0;
      const double target_fps = stream_policy_target_fps(stats);
      const int selected_width = stats.width > 0 ? stats.width : 0;
      const int selected_height = stats.height > 0 ? stats.height : 0;
      std::string selected_display_mode =
        format_stream_policy_display_mode(selected_width, selected_height, target_fps);

      if (selected_display_mode.empty() && paired_display_override) {
        selected_display_mode = client.display_mode;
      }
      if (selected_display_mode.empty() && device_profile && !device_profile->display_mode.empty()) {
        selected_display_mode = device_profile->display_mode;
      }

      int policy_width = selected_width;
      int policy_height = selected_height;
      double policy_fps = target_fps;
      if ((policy_width <= 0 || policy_height <= 0 || policy_fps <= 0.0) && !selected_display_mode.empty()) {
        parse_stream_policy_display_mode(selected_display_mode, policy_width, policy_height, policy_fps);
      }

      const auto capture_path = stream_stats::capture_path_summary(stats);
      const auto capture_reason = stream_stats::capture_path_reason(stats);
      const bool capture_cpu_copy = stream_stats::capture_path_uses_cpu_copy(stats);
      const bool capture_gpu_native = stream_stats::capture_path_is_gpu_native(stats);
      const int target_bitrate_kbps =
        stats.adaptive_target_bitrate_kbps > 0 ? stats.adaptive_target_bitrate_kbps :
        stats.bitrate_kbps > 0 ? stats.bitrate_kbps :
        paired_bitrate_override ? client.target_bitrate_kbps :
        device_profile ? device_profile->ideal_bitrate_kbps : 0;
      const std::string source = (paired_display_override || paired_bitrate_override) ? "paired_client" :
        (!stats.optimization_source.empty() ? stats.optimization_source :
          (device_profile ? "device_db" : "default"));

      nlohmann::json warnings = nlohmann::json::array();
      if (paired_display_override) {
        append_stream_policy_warning(
          warnings,
          "paired_display_mode_override",
          "This paired client has a saved display-mode override; it wins over device profile display-mode recommendations until cleared."
        );
      }
      if (capture_cpu_copy) {
        append_stream_policy_warning(
          warnings,
          capture_reason,
          capture_path_reason_message(capture_reason)
        );
      }
      if (stats.dynamic_range > 0 && !stats.stream_hdr_enabled) {
        append_stream_policy_warning(
          warnings,
          "hdr_downgraded",
          "The client requested HDR, but the active capture path is not exposing HDR metadata; Polaris is reporting the stream as SDR."
        );
      }
      if (device_profile && is_mobile_client_type(device_profile) &&
          policy_width > 1920 && policy_height > 1080 && policy_fps >= 100.0) {
        append_stream_policy_warning(
          warnings,
          "high_resolution_120_mobile",
          "This handheld/phone policy is above 1080p at 100+ FPS; 1080p120 is the safer target for steady true-headless testing."
        );
      }
      const auto health_primary_issue = health.value("primary_issue", std::string {"steady"});
      if (health_primary_issue != "steady" && health_primary_issue != capture_reason) {
        append_stream_policy_warning(
          warnings,
          health_primary_issue.empty() ? "session_health" : health_primary_issue,
          health.value("summary", std::string {"The active session has a stream-health warning."})
        );
      }

      nlohmann::json policy;
      policy["version"] = 1;
      const auto policy_stream_display_mode = effective_stream_display_mode_selection(stats);
      policy["mode"] = policy_stream_display_mode;
      policy["mode_label"] = stream_display_mode_label_for_selection(policy_stream_display_mode);
      policy["mode_reason"] = stream_display_mode_reason_for_selection(policy_stream_display_mode);
      policy["source"] = source;
      policy["source_label"] = stream_policy_source_label(source);
      policy["optimization_source"] = stats.optimization_source;
      policy["selected_display_mode"] = selected_display_mode;
      policy["requested_display_mode"] = format_stream_policy_display_mode(
        stats.width > 0 ? stats.width : policy_width,
        stats.height > 0 ? stats.height : policy_height,
        stats.requested_client_fps > 0.0 ? stats.requested_client_fps : policy_fps
      );
      policy["paired_display_mode_override"] = client.display_mode;
      policy["paired_display_mode_locked"] = paired_display_override;
      policy["paired_target_bitrate_kbps"] = client.target_bitrate_kbps;
      policy["paired_target_bitrate_locked"] = paired_bitrate_override;
      policy["target_fps"] = policy_fps;
      policy["target_bitrate_kbps"] = target_bitrate_kbps;
      policy["preferred_codec"] = stats.codec;
      policy["hdr_requested"] = stats.dynamic_range > 0;
      policy["hdr_active"] = stats.stream_hdr_enabled;
      policy["hdr_metadata_available"] = stats.hdr_metadata_available;
      policy["capture_path"] = capture_path;
      policy["capture_path_reason"] = capture_reason;
      policy["capture_path_reason_message"] = capture_path_reason_message(capture_reason);
      policy["capture_cpu_copy"] = capture_cpu_copy;
      policy["capture_gpu_native"] = capture_gpu_native;
      policy["capture_transport"] = platf::from_frame_transport(stats.capture_transport);
      policy["capture_residency"] = platf::from_frame_residency(stats.capture_residency);
      policy["capture_format"] = platf::from_frame_format(stats.capture_format);
      policy["warnings"] = std::move(warnings);
      policy["has_warnings"] = !policy["warnings"].empty();
      return policy;
    }

    nlohmann::json build_client_settings_json(const crypto::named_cert_t &client,
                                              const stream_stats::stats_t &stats,
                                              const nlohmann::json &health) {
      const auto policy = build_stream_policy_json(client, stats, health);
      const auto configured_mode = configured_stream_display_mode_selection();
      const auto effective_mode = effective_stream_display_mode_selection(stats);
      const bool relaunch_required =
        rtsp_stream::session_count() != 0 && configured_mode != effective_mode;

      nlohmann::json desired;
      desired["stream_display_mode"] = configured_mode;
      desired["stream_display_mode_label"] = stream_display_mode_label_for_selection(configured_mode);
      desired["stream_display_mode_reason"] = stream_display_mode_reason_for_selection(configured_mode);
      desired["display_mode"] = client.display_mode;
      desired["target_bitrate_kbps"] = client.target_bitrate_kbps;
      desired["adaptive_bitrate_enabled"] = adaptive_bitrate::is_enabled();
      desired["ai_optimizer_enabled"] = ai_optimizer::is_enabled();

      nlohmann::json effective;
      effective["stream_display_mode"] = effective_mode;
      effective["stream_display_mode_label"] = stream_display_mode_label_for_selection(effective_mode);
      effective["stream_display_mode_reason"] = stream_display_mode_reason_for_selection(effective_mode);
      effective["display_mode"] = policy.value("selected_display_mode", std::string {});
      effective["target_bitrate_kbps"] = policy.value("target_bitrate_kbps", 0);
      effective["adaptive_bitrate_enabled"] = adaptive_bitrate::is_enabled();
      effective["adaptive_target_bitrate_kbps"] = stats.adaptive_target_bitrate_kbps;
      effective["ai_optimizer_enabled"] = ai_optimizer::is_enabled();
      effective["capture_path"] = policy.value("capture_path", std::string {});
      effective["capture_gpu_native"] = policy.value("capture_gpu_native", false);

      const std::string revision_seed =
        configured_mode + "|" + client.display_mode + "|" +
        std::to_string(client.target_bitrate_kbps) + "|" +
        (adaptive_bitrate::is_enabled() ? "1" : "0") + "|" +
        (ai_optimizer::is_enabled() ? "1" : "0") + "|" +
        effective_mode;

      nlohmann::json settings;
      settings["version"] = 1;
      settings["revision"] = std::to_string(std::hash<std::string> {}(revision_seed));
      settings["desired"] = std::move(desired);
      settings["effective"] = std::move(effective);
      settings["policy"] = policy;
      settings["health"] = health;
      settings["capabilities"] = {
        {"modes", stream_display_mode_options_json()},
        {"display_mode_override", true},
        {"target_bitrate_override", true},
        {"adaptive_bitrate_control", true},
        {"ai_optimizer_control", true}
      };
      settings["sync_status"] = build_client_settings_sync_status(
        client,
        stats,
        policy,
        configured_mode,
        effective_mode,
        relaunch_required
      );
      settings["relaunch_required"] = relaunch_required;
      return settings;
    }

    bool is_mobile_client_type(const std::optional<device_db::device_t> &device_profile) {
      if (!device_profile) {
        return false;
      }

      return device_profile->type == "handheld" || device_profile->type == "phone";
    }

    std::string codec_family(const std::string &codec) {
      const auto normalized = lower_copy(codec);
      if (normalized.find("av1") != std::string::npos) {
        return "av1";
      }
      if (normalized.find("hevc") != std::string::npos || normalized.find("h265") != std::string::npos) {
        return "hevc";
      }
      if (normalized.find("avc") != std::string::npos || normalized.find("h264") != std::string::npos) {
        return "h264";
      }
      return normalized;
    }

    int derive_safe_bitrate_kbps(int baseline_kbps,
                                 const std::optional<device_db::device_t> &device_profile,
                                 bool degraded_history) {
      int safe_kbps = baseline_kbps > 0 ? baseline_kbps : 15000;

      if (device_profile) {
        if (device_profile->type == "handheld") {
          safe_kbps = std::min(safe_kbps, 16000);
        } else if (device_profile->type == "phone") {
          safe_kbps = std::min(safe_kbps, 18000);
        } else if (device_profile->type == "tablet") {
          safe_kbps = std::min(safe_kbps, 22000);
        } else if (device_profile->type == "desktop") {
          safe_kbps = std::min(safe_kbps, 30000);
        }
      }

      if (degraded_history) {
        safe_kbps = static_cast<int>(std::lround(static_cast<double>(safe_kbps) * 0.75));
      }

      return std::clamp(safe_kbps, 6000, std::max(6000, baseline_kbps > 0 ? baseline_kbps : safe_kbps));
    }

    nlohmann::json build_stability_plan_json(const std::string &device_name,
                                             const std::string &app_name,
                                             const std::optional<device_db::device_t> &device_profile,
                                             const device_db::optimization_t &effective_optimization,
                                             const std::optional<ai_optimizer::session_history_t> &history,
                                             const std::optional<std::string> &normalized_codec,
                                             const std::optional<int> &target_bitrate_kbps,
                                             bool hdr_requested) {
      const bool virtual_display_available = host_virtual_display_available();
      const bool prefers_headless = host_prefers_headless();
      const bool mobile_client = is_mobile_client_type(device_profile);
      const std::string quality_grade = latest_quality_grade(history);
      const bool degraded_history =
        (history && (history->consecutive_poor_outcomes > 0 || history->poor_outcome_count > 0)) ||
        is_poor_quality_grade(quality_grade);

      const int baseline_bitrate_kbps =
        target_bitrate_kbps.value_or(device_profile ? device_profile->ideal_bitrate_kbps : 15000);
      const int safe_bitrate_kbps = derive_safe_bitrate_kbps(
        baseline_bitrate_kbps,
        device_profile,
        degraded_history
      );

      std::optional<std::string> safe_codec = normalized_codec;
      if (degraded_history || (mobile_client && normalized_codec && *normalized_codec == "av1")) {
        safe_codec = device_db::normalize_preferred_codec(
          device_name,
          app_name,
          std::optional<std::string> {std::string {"hevc"}},
          safe_bitrate_kbps,
          false
        );
        if (!safe_codec) {
          safe_codec = "hevc";
        }
      }

      const bool current_virtual_display =
        effective_optimization.virtual_display.value_or(device_profile ? device_profile->virtual_display : false);
      const bool safe_virtual_display =
        virtual_display_available &&
        current_virtual_display &&
        !degraded_history &&
        !prefers_headless &&
        !mobile_client;
      const bool safe_hdr =
        hdr_requested &&
        device_profile &&
        device_profile->hdr_capable &&
        !degraded_history;

      auto discouraged_features = nlohmann::json::array();
      auto reasons = nlohmann::json::array();
      auto relaunch_notes = nlohmann::json::array();

      if (degraded_history) {
        reasons.push_back(
          history && history->consecutive_poor_outcomes > 1 ?
            "Recent sessions degraded repeatedly on this device, so Polaris is staging a safer fallback profile." :
            "This device saw a rough recent session, so Polaris is keeping a safer next-launch path ready."
        );
      } else if (mobile_client) {
        reasons.push_back("Handheld and phone clients usually stay steadier when the host path stays simple.");
      } else {
        reasons.push_back("Balanced mode keeps the current optimization, with a safer fallback ready if the session turns rough.");
      }

      if (hdr_requested && !safe_hdr) {
        discouraged_features.push_back({
          {"feature", "hdr"},
          {"reason", "Keep HDR off until this device logs a clean session again."}
        });
        relaunch_notes.push_back("HDR changes apply on the next launch.");
      }

      if (normalized_codec && *normalized_codec == "av1" && safe_codec && *safe_codec != *normalized_codec) {
        discouraged_features.push_back({
          {"feature", "av1"},
          {"reason", "AV1 can amplify decoder or pacing instability on handheld-style clients when sessions already degrade."}
        });
        relaunch_notes.push_back("Codec changes apply on the next launch.");
      }

      if (current_virtual_display && !safe_virtual_display) {
        discouraged_features.push_back({
          {"feature", "virtual_display"},
          {"reason", prefers_headless ?
            "This Polaris host already prefers a headless compositor path for the steadiest sessions." :
            "Headless is the safer fallback when virtual display starts introducing frame pacing trouble."}
        });
        relaunch_notes.push_back("Display-mode changes apply on the next launch.");
      }

      nlohmann::json stability;
      stability["mode"] = degraded_history ? "stability_first" : "balanced";
      stability["summary"] = degraded_history ?
        "Safer next launch available for this device." :
        "Balanced profile ready with a safer fallback if you hit hitching.";
      stability["relaunch_required"] =
        !relaunch_notes.empty() &&
        (
          !safe_hdr ||
          (safe_codec && normalized_codec && *safe_codec != *normalized_codec) ||
          safe_virtual_display != current_virtual_display
        );
      stability["reasons"] = std::move(reasons);
      stability["discouraged_features"] = std::move(discouraged_features);
      stability["relaunch_notes"] = std::move(relaunch_notes);

      auto &safe_profile = stability["safe_profile"];
      safe_profile["display_mode"] = safe_virtual_display ? "virtual_display" : "headless";
      safe_profile["target_bitrate_kbps"] = safe_bitrate_kbps;
      safe_profile["hdr"] = safe_hdr;
      if (safe_codec) {
        safe_profile["preferred_codec"] = *safe_codec;
      }

      if (history) {
        stability["last_quality_grade"] = quality_grade;
        stability["poor_outcome_count"] = history->poor_outcome_count;
        stability["consecutive_poor_outcomes"] = history->consecutive_poor_outcomes;
      }

      return stability;
    }

    nlohmann::json build_session_health_json(const stream_stats::stats_t &stats,
                                             bool current_virtual_display,
                                             const std::string &device_name,
                                             const std::string &app_name) {
      const auto device_profile = device_db::get_device(device_name);
      const bool mobile_client = is_mobile_client_type(device_profile);
      const std::string active_codec_family = codec_family(stats.codec);
      const double target_fps =
        stats.encode_target_fps > 0 ? stats.encode_target_fps :
        stats.session_target_fps > 0 ? stats.session_target_fps :
        stats.requested_client_fps > 0 ? stats.requested_client_fps :
        stats.fps;
      const double fps_gap = target_fps > 0 ? std::max(0.0, target_fps - stats.fps) : 0.0;

      const bool network_risk = stats.packet_loss >= 0.35 || stats.latency_ms >= 28.0;
      const bool pacing_risk =
        stats.frame_jitter_ms >= 2.2 ||
        stats.duplicate_frame_ratio >= 0.10 ||
        stats.dropped_frame_ratio >= 0.04 ||
        fps_gap >= 4.0;
      const bool capture_fallback =
        stream_stats::capture_path_uses_cpu_copy(stats);
      const auto capture_path = stream_stats::capture_path_summary(stats);
      const auto capture_reason = stream_stats::capture_path_reason(stats);
      const auto active_encoder_name = video::active_encoder_name();
      const bool nvenc_cuda_disabled_path =
        active_encoder_name == "nvenc" &&
        !build_has_cuda() &&
        capture_fallback;
      const bool encoder_risk = stats.encode_time_ms >= 11.0 || stats.avg_frame_age_ms >= 18.0;
      const bool hdr_source_missing = stats.dynamic_range > 0 && !stats.stream_hdr_enabled;
      const bool hdr_risk = stats.stream_hdr_enabled && (pacing_risk || encoder_risk);
      const bool decoder_risk =
        (active_codec_family == "av1" || stats.encode_target_format == platf::frame_format_e::p010) &&
        (pacing_risk || fps_gap >= 4.0) &&
        !network_risk;
      const bool virtual_display_risk =
        current_virtual_display &&
        (pacing_risk || capture_fallback || hdr_risk);

      auto issues = nlohmann::json::array();
      auto recommendations = nlohmann::json::array();

      if (network_risk) {
        issues.push_back("network_jitter");
        recommendations.push_back("Lower bitrate or keep Adaptive Bitrate enabled.");
      }
      if (pacing_risk) {
        issues.push_back("frame_pacing");
        recommendations.push_back("Match the game frame cap to the stream FPS and avoid VRR-style sync on the streaming display.");
      }
      if (capture_fallback) {
        issues.push_back(capture_reason);
        recommendations.push_back(capture_path_reason_message(capture_reason));
      }
      if (nvenc_cuda_disabled_path) {
        issues.push_back("nvenc_cuda_disabled");
        recommendations.push_back("Use a CUDA-enabled Polaris build/package before judging NVIDIA headless performance.");
      }
      if (encoder_risk) {
        issues.push_back("encoder_load");
        recommendations.push_back("Trim bitrate a bit to give the encoder more margin.");
      }
      if (hdr_risk) {
        issues.push_back("hdr_path");
        recommendations.push_back("Disable HDR on the next launch if the hitching keeps returning.");
      }
      if (decoder_risk) {
        issues.push_back("decoder_path");
        recommendations.push_back("Use HEVC for the next launch if AV1 keeps hitching on this client.");
      }
      if (virtual_display_risk) {
        issues.push_back("virtual_display_path");
        recommendations.push_back("Relaunch in headless mode if this title does not need a dedicated display.");
      }

      std::string primary_issue = "steady";
      if (network_risk) primary_issue = "network_jitter";
      else if (hdr_risk) primary_issue = "hdr_path";
      else if (virtual_display_risk) primary_issue = "virtual_display_path";
      else if (decoder_risk) primary_issue = "decoder_path";
      else if (nvenc_cuda_disabled_path) primary_issue = "nvenc_cuda_disabled";
      else if (capture_fallback) primary_issue = capture_reason;
      else if (pacing_risk) primary_issue = "frame_pacing";
      else if (encoder_risk) primary_issue = "encoder_load";

      const int concern_count =
        static_cast<int>(network_risk) +
        static_cast<int>(pacing_risk) +
        static_cast<int>(capture_fallback) +
        static_cast<int>(nvenc_cuda_disabled_path) +
        static_cast<int>(encoder_risk) +
        static_cast<int>(hdr_risk) +
        static_cast<int>(decoder_risk) +
        static_cast<int>(virtual_display_risk);

      std::string grade = "good";
      if (concern_count >= 2 || (network_risk && pacing_risk) || hdr_risk || decoder_risk) {
        grade = "degraded";
      } else if (concern_count == 1) {
        grade = "watch";
      }

      const int current_bitrate_kbps =
        stats.adaptive_target_bitrate_kbps > 0 ? stats.adaptive_target_bitrate_kbps : stats.bitrate_kbps;
      const int safe_bitrate_kbps = derive_safe_bitrate_kbps(
        current_bitrate_kbps > 0 ? current_bitrate_kbps :
          (device_profile ? device_profile->ideal_bitrate_kbps : 15000),
        device_profile,
        grade != "good"
      );

      auto safe_codec = active_codec_family.empty() ? std::optional<std::string> {} : std::optional<std::string> {active_codec_family};
      if (decoder_risk || (mobile_client && active_codec_family == "av1")) {
        safe_codec = device_db::normalize_preferred_codec(
          device_name,
          app_name,
          std::optional<std::string> {std::string {"hevc"}},
          safe_bitrate_kbps,
          false
        );
        if (!safe_codec) {
          safe_codec = "hevc";
        }
      }

      nlohmann::json health;
      health["grade"] = grade;
      health["primary_issue"] = primary_issue;
      health["summary"] =
        grade == "good" ? "Session looks steady." :
        network_risk ? "Network jitter is the most likely source of the hitching." :
        hdr_risk ? "The current HDR path looks unstable." :
        virtual_display_risk ? "The virtual display path is likely adding pacing overhead." :
        decoder_risk ? "The current codec path looks harder on this client than expected." :
        nvenc_cuda_disabled_path ? "The NVIDIA path is using a CUDA-disabled CPU copy fallback." :
        capture_fallback ? capture_path_reason_message(capture_reason) :
        "The stream needs a safer pacing or encode path.";
      health["issues"] = std::move(issues);
      health["recommendations"] = std::move(recommendations);
      health["safe_bitrate_kbps"] = safe_bitrate_kbps;
      health["safe_display_mode"] = (virtual_display_risk || host_prefers_headless()) ? "headless" : (current_virtual_display ? "virtual_display" : "headless");
      health["safe_hdr"] = stats.stream_hdr_enabled && !hdr_risk;
      health["decoder_risk"] = decoder_risk ? "elevated" : "normal";
      health["hdr_risk"] = hdr_risk ? "elevated" : "normal";
      health["hdr_source"] = hdr_source_missing ? "missing" : (stats.stream_hdr_enabled ? "metadata" : "sdr");
      health["network_risk"] = network_risk ? "elevated" : "normal";
      health["capture_path"] = capture_path;
      health["capture_path_reason"] = capture_reason;
      health["capture_path_reason_message"] = capture_path_reason_message(capture_reason);
      health["capture_cpu_copy"] = capture_fallback;
      health["capture_gpu_native"] = stream_stats::capture_path_is_gpu_native(stats);
      health["active_encoder"] = active_encoder_name.empty() ? "unknown" : active_encoder_name;
      health["cuda_build"] = build_has_cuda();
      health["relaunch_recommended"] = hdr_risk || decoder_risk || virtual_display_risk || nvenc_cuda_disabled_path;
      if (safe_codec) {
        health["safe_codec"] = *safe_codec;
      }
      return health;
    }
  }  // namespace

#ifdef __linux__
  namespace {
    std::mutex deferred_cage_capability_probe_mutex;

    std::optional<std::string> copy_env_var(const char *key) {
      if (const char *value = getenv(key)) {
        return std::string {value};
      }

      return std::nullopt;
    }

    void restore_env_var(const char *key, const std::optional<std::string> &value) {
      if (value) {
        platf::set_env(key, *value);
      } else {
        unsetenv(key);
      }
    }

    bool should_defer_encoder_probe_until_cage() {
      return config::video.linux_display.use_cage_compositor;
    }

    bool prime_deferred_headless_codec_capabilities() {
      if (!config::video.linux_display.use_cage_compositor ||
          !config::video.linux_display.headless_mode ||
          video::advertised_codec_capability_state_ready()) {
        return true;
      }

      if (rtsp_stream::session_count() != 0 || cage_display_router::is_running()) {
        return false;
      }

      std::scoped_lock lock {deferred_cage_capability_probe_mutex};
      if (video::advertised_codec_capability_state_ready()) {
        return true;
      }

      if (rtsp_stream::session_count() != 0 || cage_display_router::is_running()) {
        return false;
      }

      BOOST_LOG(info) << "nvhttp: Priming deferred headless encoder capabilities using a temporary cage runtime"sv;
      if (!cage_display_router::start()) {
        BOOST_LOG(warning) << "nvhttp: Temporary cage runtime failed to start for deferred capability probe"sv;
        return false;
      }

      auto stop_guard = util::fail_guard([]() {
        cage_display_router::stop();
        video::reset_encoder_probe_state();
      });

      const auto cage_socket = cage_display_router::get_wayland_socket();
      if (cage_socket.empty()) {
        BOOST_LOG(warning) << "nvhttp: Temporary cage runtime did not expose a WAYLAND_DISPLAY for deferred capability probe"sv;
        return false;
      }

      const auto original_wayland_display = copy_env_var("WAYLAND_DISPLAY");
      const auto original_at_spi_bus_address = copy_env_var("AT_SPI_BUS_ADDRESS");

      platf::set_env("WAYLAND_DISPLAY", cage_socket);
      platf::set_env("AT_SPI_BUS_ADDRESS", "");

      const int probe_status = video::probe_encoders();

      restore_env_var("AT_SPI_BUS_ADDRESS", original_at_spi_bus_address);
      restore_env_var("WAYLAND_DISPLAY", original_wayland_display);

      if (probe_status != 0) {
        BOOST_LOG(warning) << "nvhttp: Deferred headless capability probe failed"sv;
        return false;
      }

      BOOST_LOG(info) << "nvhttp: Deferred headless capability probe primed encoder cache"sv;
      return true;
    }
  }  // namespace
#endif

  namespace {
    video::codec_capability_state_t advertised_codec_support_for_http(bool allow_deferred_headless_prime = false) {
#ifdef __linux__
      if (allow_deferred_headless_prime) {
        (void) prime_deferred_headless_codec_capabilities();
      }
#endif
      return video::advertised_codec_capability_state();
    }

    std::optional<int> rounded_refresh_rate_hz(const display_device::FloatingPoint &value) {
      return std::visit(
        [](const auto &refresh_rate) -> std::optional<int> {
          using value_t = std::decay_t<decltype(refresh_rate)>;
          if constexpr (std::is_same_v<value_t, display_device::Rational>) {
            if (refresh_rate.m_denominator == 0) {
              return std::nullopt;
            }

            return static_cast<int>(std::lround(static_cast<double>(refresh_rate.m_numerator) /
                                                static_cast<double>(refresh_rate.m_denominator)));
          } else {
            return static_cast<int>(std::lround(refresh_rate));
          }
        },
        value
      );
    }

    std::optional<int> active_output_refresh_rate_hz_hint() {
      const auto configured_display_name = display_device::map_output_name(config::video.output_name);
      const auto enumerated_devices = display_device::enumerate_devices();

      std::optional<int> primary_refresh_rate;
      std::optional<int> any_active_refresh_rate;

      for (const auto &device : enumerated_devices) {
        if (!device.m_info) {
          continue;
        }

        const auto refresh_rate_hz = rounded_refresh_rate_hz(device.m_info->m_refresh_rate);
        if (!refresh_rate_hz || *refresh_rate_hz <= 0) {
          continue;
        }

        const bool matches_configured_output = !configured_display_name.empty() &&
                                              (device.m_display_name == configured_display_name ||
                                               device.m_device_id == config::video.output_name);
        if (matches_configured_output) {
          return refresh_rate_hz;
        }

        if (device.m_info->m_primary &&
            (!primary_refresh_rate || *refresh_rate_hz > *primary_refresh_rate)) {
          primary_refresh_rate = refresh_rate_hz;
        }

        if (!any_active_refresh_rate || *refresh_rate_hz > *any_active_refresh_rate) {
          any_active_refresh_rate = refresh_rate_hz;
        }
      }

      return primary_refresh_rate ? primary_refresh_rate : any_active_refresh_rate;
    }

    int advertised_max_launch_refresh_rate_for_http() {
#ifdef __linux__
      if (config::video.linux_display.use_cage_compositor &&
          config::video.linux_display.headless_mode) {
        // Headless cage sessions aren't tied to a physical panel refresh, so advertise the
        // highest stock launch rate Nova currently exposes without custom entry.
        return 120;
      }
#endif

      if (const auto refresh_rate_hz = active_output_refresh_rate_hz_hint()) {
        return *refresh_rate_hz;
      }

      return 60;
    }

    std::string_view codec_name_for_video_format(int video_format) {
      switch (video_format) {
        case 1:
          return "hevc"sv;
        case 2:
          return "av1"sv;
        default:
          return "h264"sv;
      }
    }

    std::string format_profile_fps(int fps) {
      if (fps <= 0) {
        return "0"s;
      }
      if (fps % 1000 == 0) {
        return std::to_string(fps / 1000);
      }
      return std::format("{:.3f}", static_cast<double>(fps) / 1000.0);
    }

    std::string format_watch_profile(const stream::session_profile_t &profile) {
      return std::format("{}x{}@{} {} {}-bit {}kbps",
                         profile.width,
                         profile.height,
                         format_profile_fps(profile.session_target_fps),
                         codec_name_for_video_format(profile.video_format),
                         profile.dynamic_range > 0 ? 10 : 8,
                         profile.bitrate_kbps);
    }

    std::optional<stream::session_profile_t> active_owner_watch_profile() {
      const auto owner_uuid = proc::proc.get_session_owner_unique_id();
      if (owner_uuid.empty()) {
        return std::nullopt;
      }

      const auto owner_session = rtsp_stream::find_session(owner_uuid);
      if (!owner_session || stream::session::is_watch_only(*owner_session)) {
        return std::nullopt;
      }

      return stream::session::profile(*owner_session);
    }

    std::optional<std::pair<int, std::string>> pin_watch_session_to_active_profile(rtsp_stream::launch_session_t &launch_session) {
      if (!launch_session.watch_only) {
        return std::nullopt;
      }

      const auto owner_profile = active_owner_watch_profile();
      if (!owner_profile) {
        return std::make_pair(409, "No active owner stream is available to watch"s);
      }

      const int requested_dynamic_range = launch_session.enable_hdr ? 1 : 0;
      if (launch_session.requested_width != owner_profile->width ||
          launch_session.requested_height != owner_profile->height ||
          launch_session.requested_fps != owner_profile->session_target_fps ||
          requested_dynamic_range != owner_profile->dynamic_range) {
        return std::make_pair(
          412,
          std::format("Watch mode must match the active stream profile ({})", format_watch_profile(*owner_profile))
        );
      }

      launch_session.requested_width = owner_profile->width;
      launch_session.requested_height = owner_profile->height;
      launch_session.requested_fps = owner_profile->session_target_fps;
      launch_session.width = owner_profile->width;
      launch_session.height = owner_profile->height;
      launch_session.fps = owner_profile->session_target_fps;
      launch_session.enable_hdr = owner_profile->dynamic_range > 0;
      launch_session.target_bitrate_kbps = owner_profile->bitrate_kbps;
      launch_session.preferred_codec = std::string {codec_name_for_video_format(owner_profile->video_format)};
      launch_session.optimization_source = "watch_owner_profile";

      BOOST_LOG(info) << "Watch session pinned to active owner profile ["sv
                      << owner_profile->device_name << "]: "sv
                      << format_watch_profile(*owner_profile);

      return std::nullopt;
    }

    nlohmann::json launch_mode_contract_for_app(const proc::ctx_t &app) {
      const bool app_prefers_virtual_display = app.virtual_display;
      const std::string preferred_mode = app_prefers_virtual_display ? "host_virtual_display" : "headless_stream";
      std::string recommended_mode = preferred_mode;
      std::string mode_reason;

      auto allowed_modes = nlohmann::json::array();
      allowed_modes.push_back("headless_stream");
      allowed_modes.push_back("desktop_display");
      allowed_modes.push_back("windowed_stream");
      if (host_virtual_display_available()) {
        allowed_modes.push_back("host_virtual_display");
      }

      const bool steam_big_picture = boost::iequals(boost::trim_copy(app.name), "Steam Big Picture");

      if (app_prefers_virtual_display && host_virtual_display_available()) {
        recommended_mode = "host_virtual_display";
        mode_reason = steam_big_picture ?
          "Steam Big Picture is configured to prefer a dedicated virtual display on this host." :
          "This app is configured to prefer a dedicated virtual display on the host.";
      } else if (app_prefers_virtual_display && !host_virtual_display_available()) {
        recommended_mode = "headless_stream";
        mode_reason =
          "This app prefers Host Virtual Display, but Polaris does not currently have a virtual display backend available, so Headless Stream is recommended.";
      } else if (host_prefers_headless()) {
        recommended_mode = "headless_stream";
        mode_reason =
          "Headless Stream is recommended because this Polaris host is already configured for headless streaming.";
      } else if (host_virtual_display_available()) {
        recommended_mode = "headless_stream";
        mode_reason =
          "This app defaults to Headless Stream. Host Virtual Display is available when you want a dedicated display for the session.";
      } else {
        recommended_mode = "headless_stream";
        mode_reason =
          "This app defaults to Headless Stream on this host.";
      }

      nlohmann::json launch_mode;
      launch_mode["preferred_mode"] = preferred_mode;
      launch_mode["recommended_mode"] = recommended_mode;
      launch_mode["allowed_modes"] = std::move(allowed_modes);
      launch_mode["mode_reason"] = mode_reason;
      return launch_mode;
    }
  }  // namespace

  using p_named_cert_t = crypto::p_named_cert_t;
  using PERM = crypto::PERM;

  struct client_t {
    std::vector<p_named_cert_t> named_devices;
  };

  struct pair_session_t;

  crypto::cert_chain_t cert_chain;
  static std::string one_time_pin;
  static std::string otp_passphrase;
  static std::string otp_device_name;
  static std::chrono::time_point<std::chrono::steady_clock> otp_creation_time;

  class PolarisHTTPSServer: public SimpleWeb::ServerBase<PolarisHTTPS> {
  public:
    PolarisHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<PolarisHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<bool(std::shared_ptr<Request>, SSL*)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              if (verify && !verify(session->request, session->connection->socket->native_handle())) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = PolarisHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<PolarisHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<PolarisHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  namespace {
    bool watch_requested(const args_t &args) {
      return util::from_view(get_arg(args, "watch", "0"));
    }

    bool session_token_matches_request(const args_t &args) {
      const auto expected_token = get_arg(args, "sessiontoken", "");
      const auto active_token = proc::proc.get_session_token();
      return expected_token.empty() || active_token.empty() || crypto::constant_time_equals(expected_token, active_token);
    }

    void append_current_game_session_fields(pt::ptree &tree, const crypto::named_cert_t *named_cert_p) {
      const auto current_session_token = proc::proc.get_session_token();
      const auto current_session_owner = proc::proc.get_session_owner_unique_id();
      const bool has_current_owner = !current_session_token.empty() && !current_session_owner.empty();

      tree.put("root.currentgamesessiontoken", current_session_token);
      tree.put("root.currentgameowner", current_session_owner);
      tree.put("root.currentgameviewercount", rtsp_stream::viewer_count());
      tree.put(
        "root.currentgameowned",
        has_current_owner && named_cert_p && proc::proc.is_session_owner(named_cert_p->uuid) ? 1 : 0
      );
    }
  }  // namespace

  /**
   * @brief Check if an IP address falls within any configured trusted subnet.
   * Used for TOFU (Trust-on-First-Use) LAN pairing.
   */
  bool is_in_trusted_subnet(const boost::asio::ip::address &addr) {
    if (config::nvhttp.trusted_subnets.empty()) {
      return false;
    }

    for (const auto &subnet_str : config::nvhttp.trusted_subnets) {
      auto slash = subnet_str.find('/');
      if (slash == std::string::npos) {
        continue;
      }

      try {
        auto net_str = subnet_str.substr(0, slash);
        auto prefix = static_cast<unsigned short>(std::stoi(subnet_str.substr(slash + 1)));
        auto network_addr = boost::asio::ip::make_address(net_str);

        if (addr.is_v4()) {
          if (!network_addr.is_v4() || prefix > 32) {
            continue;
          }

          auto network = boost::asio::ip::make_network_v4(
            network_addr.to_v4(), prefix);
          auto client_v4 = addr.to_v4();
          auto net_addr = network.network().to_uint();
          auto client_uint = client_v4.to_uint();
          auto mask = network.netmask().to_uint();
          if ((client_uint & mask) == (net_addr & mask)) {
            return true;
          }
        }
        else if (addr.is_v6()) {
          // Handle IPv4-mapped IPv6 addresses (e.g., ::ffff:10.0.0.1)
          auto v6 = addr.to_v6();
          if (v6.is_v4_mapped()) {
            if (!network_addr.is_v4() || prefix > 32) {
              continue;
            }

            auto v4 = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
            auto network = boost::asio::ip::make_network_v4(
              network_addr.to_v4(), prefix);
            auto net_addr = network.network().to_uint();
            auto client_uint = v4.to_uint();
            auto mask = network.netmask().to_uint();
            if ((client_uint & mask) == (net_addr & mask)) {
              return true;
            }
          } else if (network_addr.is_v6() && ipv6_prefix_matches(v6, network_addr.to_v6(), prefix)) {
            return true;
          }
        }
      }
      catch (...) {
        BOOST_LOG(warning) << "TOFU: Failed to parse trusted subnet: " << subnet_str;
        continue;
      }
    }

    return false;
  }

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  std::string get_arg(const args_t &args, const char *name, const char *default_value) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  // Helper function to extract command entries from a JSON object.
  cmd_list_t extract_command_entries(const nlohmann::json& j, const std::string& key) {
    cmd_list_t commands;

    // Check if the key exists in the JSON.
    if (j.contains(key)) {
      // Ensure that the value for the key is an array.
      try {
        for (const auto& item : j.at(key)) {
          try {
            // Extract "cmd" and "elevated" fields from the JSON object.
            std::string cmd = item.at("cmd").get<std::string>();
            bool elevated = util::get_non_string_json_value<bool>(item, "elevated", false);

            // Add the command entry to the list.
            commands.push_back({cmd, elevated});
          } catch (const std::exception& e) {
            BOOST_LOG(warning) << "Error parsing command entry: " << e.what();
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Error retrieving key \"" << key << "\": " << e.what();
      }
    } else {
      BOOST_LOG(debug) << "Key \"" << key << "\" not found in the JSON.";
    }

    return commands;
  }

  void save_state() {
    nlohmann::json root = nlohmann::json::object();
    // If the state file exists, try to read it.
    if (fs::exists(config::nvhttp.file_state)) {
      try {
        std::ifstream in(config::nvhttp.file_state);
        in >> root;
      } catch (std::exception &e) {
        BOOST_LOG(warning) << "Couldn't read existing state "sv << config::nvhttp.file_state
                           << ": "sv << e.what() << "; rewriting from in-memory state";
        root = nlohmann::json::object();
      }
    }

    // Erase any previous "root" key.
    root.erase("root");

    // Create a new "root" object and set the unique id.
    root["root"] = nlohmann::json::object();
    root["root"]["uniqueid"] = http::unique_id;

    client_t &client = client_root;
    nlohmann::json named_cert_nodes = nlohmann::json::array();

    std::unordered_set<std::string> unique_certs;
    std::unordered_map<std::string, int> name_counts;

    for (auto &named_cert_p : client.named_devices) {
      // Only add each unique certificate once.
      if (unique_certs.insert(named_cert_p->cert).second) {
        nlohmann::json named_cert_node = nlohmann::json::object();
        std::string base_name = named_cert_p->name;
        // Remove any pending id suffix (e.g., " (2)") if present.
        size_t pos = base_name.find(" (");
        if (pos != std::string::npos) {
          base_name = base_name.substr(0, pos);
        }
        int count = name_counts[base_name]++;
        std::string final_name = base_name;
        if (count > 0) {
          final_name += " (" + std::to_string(count + 1) + ")";
        }
        named_cert_node["name"] = final_name;
        named_cert_node["cert"] = named_cert_p->cert;
        named_cert_node["uuid"] = named_cert_p->uuid;
        named_cert_node["client_family"] = named_cert_p->client_family;
        named_cert_node["display_mode"] = named_cert_p->display_mode;
        named_cert_node["target_bitrate_kbps"] = named_cert_p->target_bitrate_kbps;
        named_cert_node["perm"] = static_cast<uint32_t>(named_cert_p->perm);
        named_cert_node["enable_legacy_ordering"] = named_cert_p->enable_legacy_ordering;
        named_cert_node["allow_client_commands"] = named_cert_p->allow_client_commands;
        named_cert_node["always_use_virtual_display"] = named_cert_p->always_use_virtual_display;

        // Add "do" commands if available.
        if (!named_cert_p->do_cmds.empty()) {
          nlohmann::json do_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert_p->do_cmds) {
            do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["do"] = do_cmds_node;
        }

        // Add "undo" commands if available.
        if (!named_cert_p->undo_cmds.empty()) {
          nlohmann::json undo_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert_p->undo_cmds) {
            undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["undo"] = undo_cmds_node;
        }

        named_cert_nodes.push_back(named_cert_node);
      }
    }

    root["root"]["named_devices"] = named_cert_nodes;

    try {
      std::ofstream out(config::nvhttp.file_state);
      out << root.dump(4);  // Pretty-print with an indent of 4 spaces.
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }
  }

  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    nlohmann::json tree;
    try {
      std::ifstream in(config::nvhttp.file_state);
      in >> tree;
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    // Check that the file contains a "root.uniqueid" value.
    if (!tree.contains("root") || !tree["root"].contains("uniqueid")) {
      http::uuid = uuid_util::uuid_t::generate();
      http::unique_id = http::uuid.string();
      return;
    }

    std::string uid = tree["root"]["uniqueid"];
    http::uuid = uuid_util::uuid_t::parse(uid);
    http::unique_id = uid;

    nlohmann::json root = tree["root"];
    client_t client;  // Local client to load into

    // Import from the old format if available.
    if (root.contains("devices")) {
      for (auto &device_node : root["devices"]) {
        // For each device, if there is a "certs" array, add a named certificate.
        if (device_node.contains("certs")) {
          for (auto &el : device_node["certs"]) {
            auto named_cert_p = std::make_shared<crypto::named_cert_t>();
            named_cert_p->name = "";
            named_cert_p->cert = el.get<std::string>();
            named_cert_p->uuid = uuid_util::uuid_t::generate().string();
            named_cert_p->client_family = "";
            named_cert_p->display_mode = "";
            named_cert_p->target_bitrate_kbps = 0;
            named_cert_p->perm = PERM::_all;
            named_cert_p->enable_legacy_ordering = true;
            named_cert_p->allow_client_commands = true;
            named_cert_p->always_use_virtual_display = false;
            client.named_devices.emplace_back(named_cert_p);
          }
        }
      }
    }

    // Import from the new format.
    if (root.contains("named_devices")) {
      for (auto &el : root["named_devices"]) {
        auto named_cert_p = std::make_shared<crypto::named_cert_t>();
        named_cert_p->name = el.value("name", "");
        named_cert_p->cert = el.value("cert", "");
        named_cert_p->uuid = el.value("uuid", "");
        named_cert_p->client_family = el.value("client_family", "");
        named_cert_p->display_mode = el.value("display_mode", "");
        named_cert_p->target_bitrate_kbps = util::get_non_string_json_value<int>(el, "target_bitrate_kbps", 0);
        named_cert_p->perm = (PERM)(util::get_non_string_json_value<uint32_t>(el, "perm", (uint32_t)PERM::_all)) & PERM::_all;
        named_cert_p->enable_legacy_ordering = el.value("enable_legacy_ordering", true);
        named_cert_p->allow_client_commands = el.value("allow_client_commands", true);
        named_cert_p->always_use_virtual_display = el.value("always_use_virtual_display", false);
        // Load command entries for "do" and "undo" keys.
        named_cert_p->do_cmds = extract_command_entries(el, "do");
        named_cert_p->undo_cmds = extract_command_entries(el, "undo");
        client.named_devices.emplace_back(named_cert_p);
      }
    }

    // Clear any existing certificate chain and add the imported certificates.
    cert_chain.clear();
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(named_cert);
    }

    client_root = client;
  }

  void add_authorized_client(const p_named_cert_t& named_cert_p) {
    client_t &client = client_root;
    client.named_devices.push_back(named_cert_p);
    auto live_named_cert_p = named_cert_p;
    cert_chain.add(live_named_cert_p);

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
    system_tray::update_tray_paired(named_cert_p->name);
#endif

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
      load_state();
    }
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, bool input_only, const args_t &args, const crypto::named_cert_t* named_cert_p) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    // If launched from client
    if (named_cert_p->uuid != http::unique_id) {
      auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
      std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

      launch_session->host_audio = host_audio;

      // Encrypted RTSP is enabled with client reported corever >= 1
      auto corever = util::from_view(get_arg(args, "corever", "0"));
      if (corever >= 1) {
        launch_session->rtsp_cipher = crypto::cipher::gcm_t {
          launch_session->gcm_key, false
        };
        launch_session->rtsp_iv_counter = 0;
      }
      launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

      // Generate the unique identifiers for this connection that we will send later during RTSP handshake
      unsigned char raw_payload[8];
      RAND_bytes(raw_payload, sizeof(raw_payload));
      launch_session->av_ping_payload = util::hex_vec(raw_payload);
      RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

      launch_session->iv.resize(16);
      uint32_t prepend_iv = util::endian::big<uint32_t>(util::from_view(get_arg(args, "rikeyid")));
      auto prepend_iv_p = (uint8_t *) &prepend_iv;
      std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    }

    std::stringstream mode;
    if (named_cert_p->display_mode.empty()) {
      auto mode_str = get_arg(args, "mode", config::video.fallback_mode.c_str());
      mode = std::stringstream(mode_str);
      BOOST_LOG(info) << "Display mode for client ["sv << named_cert_p->name <<"] requested to ["sv << mode_str << ']';
    } else {
      mode = std::stringstream(named_cert_p->display_mode);
      BOOST_LOG(info) << "Display mode for client ["sv << named_cert_p->name <<"] overriden to ["sv << named_cert_p->display_mode << ']';
    }

    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        auto fps = atof(segment.c_str());
        if (fps < 1000) {
          fps *= 1000;
        };
        launch_session->fps = (int)fps;
        break;
      }
      x++;
    }

    // Parsing have failed or missing components
    if (x != 2) {
      launch_session->width = 1920;
      launch_session->height = 1080;
      launch_session->fps = 60000; // 60fps * 1000 denominator
    }

    launch_session->requested_width = launch_session->width;
    launch_session->requested_height = launch_session->height;
    launch_session->requested_fps = launch_session->fps;

    launch_session->device_name = named_cert_p->name.empty() ? "PolarisDisplay"s : named_cert_p->name;
    launch_session->unique_id = named_cert_p->uuid;
    launch_session->watch_only = watch_requested(args);
    launch_session->perm = launch_session->watch_only ? PERM::view : named_cert_p->perm;
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->gcmap = util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));
    const bool client_display_mode_explicit = util::from_view(get_arg(args, "displayModeExplicit", "0"));
    const bool client_requested_virtual_display = util::from_view(get_arg(args, "virtualDisplay", "0"));
    launch_session->virtual_display = client_requested_virtual_display || named_cert_p->always_use_virtual_display;
    launch_session->user_locked_display_mode = !named_cert_p->display_mode.empty();
    launch_session->user_locked_virtual_display = client_display_mode_explicit || named_cert_p->always_use_virtual_display;
    launch_session->scale_factor = util::from_view(get_arg(args, "scaleFactor", "100"));
    if (named_cert_p->target_bitrate_kbps > 0) {
      launch_session->paired_target_bitrate_kbps = named_cert_p->target_bitrate_kbps;
    } else {
      launch_session->paired_target_bitrate_kbps.reset();
    }

    if (!launch_session->watch_only) {
      launch_session->client_do_cmds = named_cert_p->do_cmds;
      launch_session->client_undo_cmds = named_cert_p->undo_cmds;
    }

    launch_session->input_only = input_only;

    return launch_session;
  }

  void remove_session(const pair_session_t &sess) {
    map_id_sess.erase(sess.client.uniqueID);
  }

  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    remove_session(sess);  // Security measure, delete the session when something went wrong and force a re-pair
    BOOST_LOG(warning) << "Pair attempt failed due to " << status_msg;
  }

  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      tree.put("root.paired", 1);

      auto named_cert_p = std::make_shared<crypto::named_cert_t>();
      named_cert_p->name = client.name;
      for (char& c : named_cert_p->name) {
        if (c == '(') c = '[';
        else if (c == ')') c = ']';
      }
      named_cert_p->cert = std::move(client.cert);
      named_cert_p->uuid = uuid_util::uuid_t::generate().string();
      named_cert_p->client_family = client.family_hint;
      // If the device is the first one paired with the server, assign full permission.
      if (client_root.named_devices.empty()) {
        named_cert_p->perm = PERM::_all;
      } else {
        named_cert_p->perm = PERM::_default;
      }

      named_cert_p->enable_legacy_ordering = true;
      named_cert_p->allow_client_commands = true;
      named_cert_p->always_use_virtual_display = false;

      auto it = map_id_sess.find(client.uniqueID);
      map_id_sess.erase(it);

      add_authorized_client(named_cert_p);
    } else {
      tree.put("root.paired", 0);
      BOOST_LOG(warning) << "Pair attempt failed due to same_hash: " << same_hash << ", verify: " << verify;
    }

    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  template<class T>
  struct tunnel;

  template<>
  struct tunnel<PolarisHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  inline crypto::named_cert_t* get_verified_cert(req_https_t request) {
    return (crypto::named_cert_t*)request->userp.get();
  }

  void remember_client_family(crypto::named_cert_t *named_cert_p, std::string_view family) {
    if (!named_cert_p || family.empty() || named_cert_p->client_family == family) {
      return;
    }

    named_cert_p->client_family = std::string(family);
    save_state();
  }

  crypto::p_named_cert_t verify_client_cert(SSL *ssl, bool log_errors) {
    if (!ssl) {
      return {};
    }

    crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
      SSL_get1_peer_certificate(ssl)
#else
      SSL_get_peer_certificate(ssl)
#endif
    };

    if (!x509) {
      return {};
    }

    p_named_cert_t named_cert_p;
    auto err_str = cert_chain.verify(x509.get(), named_cert_p);
    if (err_str) {
      if (log_errors) {
        BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;
      }
      return {};
    }

    return named_cert_p;
  }

  template <class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    std::ostringstream query_string;
    bool first = true;
    for (const auto &[name, val] : request->parse_query_string()) {
      if (!first) {
        query_string << '&';
      }
      query_string << name << '=' << val;
      first = false;
    }

    BOOST_LOG(warning) << "nvhttp: 404 "sv
                       << request->method << ' ' << request->path
                       << (query_string.str().empty() ? "" : "?")
                       << query_string.str();

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(SimpleWeb::StatusCode::client_error_not_found, data.str());
    response->close_connection_after_response = true;
  }

  template<class T>
  void unpair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    const auto unique_id_it = args.find("uniqueid"s);
    if (unique_id_it == std::end(args)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");
      return;
    }

    const auto unique_id = unique_id_it->second;
    const bool removed_session = map_id_sess.erase(unique_id) > 0;

    bool removed_paired_client = false;
    if constexpr (std::is_same_v<PolarisHTTPS, T>) {
      if (auto named_cert_p = get_verified_cert(request)) {
        removed_paired_client = unpair_client(named_cert_p->uuid);
      }
    }

    BOOST_LOG(info) << "pair: unpair cleanup for uniqueid "sv << unique_id
                    << " session_removed="sv << removed_session
                    << " paired_client_removed="sv << removed_paired_client;

    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.<xmlattr>.status_message", "Unpaired");
  }

  template <class T>
  void pair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    if (!config::sunshine.enable_pairing) {
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Pairing is disabled for this instance");

      return;
    }

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        auto existing_session = map_id_sess.find(uniqID);
        if (existing_session != std::end(map_id_sess)) {
          BOOST_LOG(info) << "pair: restarting in-flight session for uniqueid "sv << uniqID;
          map_id_sess.erase(existing_session);
        }

        pair_session_t sess;

        auto deviceName { get_arg(args, "devicename") };
        const bool trusted_pair_requested = util::from_view(get_arg(args, "trustedpair", "0"));
        const auto remote_addr = request->remote_endpoint().address();
        const auto remote_addr_str = net::addr_to_normalized_string(remote_addr);
        const bool remote_in_trusted_subnet = is_in_trusted_subnet(remote_addr);

        if (deviceName == "roth"sv) {
          deviceName = "Legacy Moonlight Client";
        }

        sess.client.uniqueID = std::move(uniqID);
        sess.client.name = std::move(deviceName);
        sess.client.family_hint.clear();
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);

        BOOST_LOG(debug) << sess.client.cert;
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;

        ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));
        BOOST_LOG(info) << "pair: getservercert uniqueid="sv << uniqID
                        << " device=\""sv << ptr->second.client.name << "\""
                        << " remote="sv << remote_addr_str
                        << " trustedpair="sv << trusted_pair_requested
                        << " trusted_subnet_match="sv << remote_in_trusted_subnet;

        auto it = args.find("otpauth");
        if (it != std::end(args)) {
          if (one_time_pin.empty() || (std::chrono::steady_clock::now() - otp_creation_time > OTP_EXPIRE_DURATION)) {
            one_time_pin.clear();
            otp_passphrase.clear();
            otp_device_name.clear();
            tree.put("root.<xmlattr>.status_code", 503);
            tree.put("root.<xmlattr>.status_message", "OTP auth not available.");
          } else {
            auto hash = util::hex(crypto::hash(one_time_pin + ptr->second.async_insert_pin.salt + otp_passphrase), true);

            if (hash.to_string_view() == it->second) {

              if (!otp_device_name.empty()) {
                ptr->second.client.name = std::move(otp_device_name);
              }

              getservercert(ptr->second, tree, one_time_pin);

              one_time_pin.clear();
              otp_passphrase.clear();
              otp_device_name.clear();
              return;
            }
          }

          // Always return positive, attackers will fail in the next steps.
          getservercert(ptr->second, tree, crypto::rand(16));
          return;
        }

        if (config::sunshine.flags[config::flag::PIN_STDIN]) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          getservercert(ptr->second, tree, pin);
          return;
        } else {
          if (trusted_pair_requested) {
            ptr->second.client.family_hint = "nova";
          }
          if (trusted_pair_requested &&
              config::nvhttp.trusted_subnet_auto_pairing &&
              remote_in_trusted_subnet)
          {
            // TOFU: Auto-approve pairing from trusted subnet with well-known PIN,
            // but only when the client explicitly opts into the trusted flow.
            BOOST_LOG(info) << "TOFU: Auto-approving pairing from trusted subnet: "sv
                            << remote_addr_str;
            getservercert(ptr->second, tree, "0000");
            return;
          }

          if (trusted_pair_requested && !config::nvhttp.trusted_subnet_auto_pairing) {
            BOOST_LOG(info) << "TOFU: Trusted Pair requested but disabled in host config"sv;
          } else if (trusted_pair_requested && !remote_in_trusted_subnet) {
            BOOST_LOG(info) << "TOFU: Trusted Pair requested from untrusted subnet: "sv
                            << remote_addr_str;
          }

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
          system_tray::update_tray_require_pin();
#endif
          ptr->second.async_insert_pin.response = std::move(response);

          fg.disable();
          return;
        }
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, tree, pairingsecret);
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  bool pin(std::string pin, std::string name) {
    pt::ptree tree;
    if (map_id_sess.empty()) {
      return false;
    }

    // ensure pin is 4 digits
    if (pin.size() != 4) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message",
        std::format("Pin must be 4 digits, {} provided", pin.size())
      );
      return false;
    }

    // ensure all pin characters are numeric
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
      return false;
    }

    auto &sess = std::begin(map_id_sess)->second;
    getservercert(sess, tree, pin);

    if (!name.empty()) {
      sess.client.name = name;
    }

    // response to the request for pin
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &async_response = sess.async_insert_pin.response;
    if (async_response.has_left() && async_response.left()) {
      async_response.left()->write(data.str());
    } else if (async_response.has_right() && async_response.right()) {
      async_response.right()->write(data.str());
    } else {
      return false;
    }

    // reset async_response
    async_response = std::decay_t<decltype(async_response.left())>();
    // response to the current request
    return true;
  }

  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    auto local_endpoint = request->local_endpoint();
    const crypto::named_cert_t *named_cert_p = nullptr;
    if constexpr (std::is_same_v<PolarisHTTPS, T>) {
      named_cert_p = get_verified_cert(request);
    }
    const int pair_status = named_cert_p ? 1 : 0;
    const auto advertised_codec_support = advertised_codec_support_for_http(std::is_same_v<PolarisHTTPS, T>);

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", advertised_codec_support.hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<PolarisHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
      if (named_cert_p && !!(named_cert_p->perm & PERM::server_cmd)) {
        pt::ptree& root_node = tree.get_child("root");

        if (config::sunshine.server_cmds.size() > 0) {
          // Broadcast server_cmds
          for (const auto& cmd : config::sunshine.server_cmds) {
            pt::ptree cmd_node;
            cmd_node.put_value(cmd.cmd_name);
            root_node.push_back(std::make_pair("ServerCommand", cmd_node));
          }
        }
      } else if (named_cert_p) {
        BOOST_LOG(debug) << "Permission Get ServerCommand denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";
      }

      tree.put("root.Permission", std::to_string(named_cert_p ? (uint32_t) named_cert_p->perm : 0U));

    #ifdef _WIN32
      tree.put("root.VirtualDisplayCapable", true);
      if (named_cert_p && !!(named_cert_p->perm & PERM::_all_actions)) {
        tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK);
      } else {
        tree.put("root.VirtualDisplayDriverReady", true);
      }
    #endif
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
      tree.put("root.Permission", "0");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    // Only advertise trusted-subnet pairing when the host actually allows it.
    if (config::nvhttp.trusted_subnet_auto_pairing && is_in_trusted_subnet(request->remote_endpoint().address())) {
      tree.put("root.TofuEnabled", 1);
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (advertised_codec_support.yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (advertised_codec_support.hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (advertised_codec_support.yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (advertised_codec_support.hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (advertised_codec_support.yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (advertised_codec_support.av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (advertised_codec_support.yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (advertised_codec_support.av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (advertised_codec_support.yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);
    tree.put("root.ServerMaxLaunchRefreshRate", advertised_max_launch_refresh_rate_for_http());

    tree.put("root.PairStatus", pair_status);

    if constexpr (std::is_same_v<PolarisHTTPS, T>) {
      int current_appid = proc::proc.running();
      // When input only mode is enabled, the only resume method should be launching the same app again.
      if (config::input.enable_input_only_mode && current_appid != proc::input_only_app_id) {
        current_appid = 0;
      }
      tree.put("root.currentgame", current_appid);
      tree.put("root.currentgameuuid", proc::proc.get_running_app_uuid());
      tree.put("root.state", current_appid > 0 ? "POLARIS_SERVER_BUSY" : "POLARIS_SERVER_FREE");
      append_current_game_session_fields(tree, named_cert_p);
    } else {
      tree.put("root.currentgame", 0);
      tree.put("root.currentgameuuid", "");
      tree.put("root.state", "POLARIS_SERVER_FREE");
      tree.put("root.currentgamesessiontoken", "");
      tree.put("root.currentgameowner", "");
      tree.put("root.currentgameviewercount", 0);
      tree.put("root.currentgameowned", 0);
    }

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    client_t &client = client_root;
    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    for (auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert->name;
      named_cert_node["friendly_name"] = device_db::friendly_name(named_cert->name);
      named_cert_node["uuid"] = named_cert->uuid;
      named_cert_node["client_family"] = named_cert->client_family;
      named_cert_node["display_mode"] = named_cert->display_mode;
      named_cert_node["target_bitrate_kbps"] = named_cert->target_bitrate_kbps;
      named_cert_node["perm"] = static_cast<uint32_t>(named_cert->perm);
      named_cert_node["enable_legacy_ordering"] = named_cert->enable_legacy_ordering;
      named_cert_node["allow_client_commands"] = named_cert->allow_client_commands;
      named_cert_node["always_use_virtual_display"] = named_cert->always_use_virtual_display;

      // Add "do" commands if available
      if (!named_cert->do_cmds.empty()) {
        nlohmann::json do_cmds_node = nlohmann::json::array();
        for (const auto &cmd : named_cert->do_cmds) {
          do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
        }
        named_cert_node["do"] = do_cmds_node;
      }

      // Add "undo" commands if available
      if (!named_cert->undo_cmds.empty()) {
        nlohmann::json undo_cmds_node = nlohmann::json::array();
        for (const auto &cmd : named_cert->undo_cmds) {
          undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
        }
        named_cert_node["undo"] = undo_cmds_node;
      }

      // Determine connection status
      bool connected = false;
      if (connected_uuids.empty()) {
        connected = false;
      } else {
        for (auto it = connected_uuids.begin(); it != connected_uuids.end(); ++it) {
          if (*it == named_cert->uuid) {
            connected = true;
            connected_uuids.erase(it);
            break;
          }
        }
      }
      named_cert_node["connected"] = connected;

      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  void applist(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);
    const auto advertised_codec_support = advertised_codec_support_for_http(true);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    auto named_cert_p = get_verified_cert(request);
    if (!named_cert_p) {
      apps.put("<xmlattr>.status_code", 401);
      apps.put("<xmlattr>.status_message", "The client is not authorized. Certificate verification failed.");
      return;
    }

    apps.put("<xmlattr>.status_code", 200);

    if (!!(named_cert_p->perm & PERM::_all_actions)) {
      auto current_appid = proc::proc.running();
      auto should_hide_inactive_apps = config::input.enable_input_only_mode && current_appid > 0 && current_appid != proc::input_only_app_id;

      auto app_list = proc::proc.get_apps();

      bool enable_legacy_ordering = config::sunshine.legacy_ordering && named_cert_p->enable_legacy_ordering;
      size_t bits;
      if (enable_legacy_ordering) {
        bits = zwpad::pad_width_for_count(app_list.size());
      }

      for (size_t i = 0; i < app_list.size(); i++) {
        auto& app = app_list[i];
        auto appid = util::from_view(app.id);
        if (should_hide_inactive_apps) {
          if (
            appid != current_appid
            && appid != proc::input_only_app_id
            && appid != proc::terminate_app_id
          ) {
            continue;
          }
        } else {
          if (appid == proc::terminate_app_id) {
            continue;
          }
        }

        std::string app_name;
        if (enable_legacy_ordering) {
          app_name = zwpad::pad_for_ordering(app.name, bits, i);
        } else {
          app_name = app.name;
        }

        pt::ptree app_node;

        app_node.put("IsHdrSupported"s, advertised_codec_support.hevc_mode == 3 ? 1 : 0);
        app_node.put("AppTitle"s, app_name);
        app_node.put("UUID", app.uuid);
        app_node.put("IDX", app.idx);
        app_node.put("ID", app.id);

        apps.push_back(std::make_pair("App", std::move(app_node)));
      }
    } else {
      BOOST_LOG(debug) << "Permission ListApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      pt::ptree app_node;

      app_node.put("IsHdrSupported"s, 0);
      app_node.put("AppTitle"s, "Permission Denied");
      app_node.put("UUID", "");
      app_node.put("IDX", "0");
      app_node.put("ID", "114514");

      apps.push_back(std::make_pair("App", std::move(app_node)));

      return;
    }

  }

  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();

    auto appid_str = get_arg(args, "appid", "0");
    auto appuuid_str = get_arg(args, "appuuid", "");
    auto appid = util::from_view(appid_str);
    auto current_appid = proc::proc.running();
    auto current_app_uuid = proc::proc.get_running_app_uuid();
    bool is_input_only = config::input.enable_input_only_mode && (appid == proc::input_only_app_id || (appuuid_str == REMOTE_INPUT_UUID));

    auto named_cert_p = get_verified_cert(request);
    if (!named_cert_p) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 401);
      tree.put("root.<xmlattr>.status_message", "The client is not authorized. Certificate verification failed.");
      return;
    }

    auto perm = PERM::launch;

    BOOST_LOG(verbose) << "Launching app [" << appid_str << "] with UUID [" << appuuid_str << "]";
    // BOOST_LOG(verbose) << "QS: " << request->query_string;

    // If we have already launched an app, we should allow clients with view permission to join the input only or current app's session.
    if (
      current_appid > 0
      && (appuuid_str != TERMINATE_APP_UUID || appid != proc::terminate_app_id)
      && (is_input_only || appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid))
    ) {
      perm = PERM::_allow_view;
    }

    if (!(named_cert_p->perm & perm)) {
      BOOST_LOG(debug) << "Permission LaunchApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      (args.find("appid"s) == std::end(args) && args.find("appuuid"s) == std::end(args))
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    if (!is_input_only) {
      // Special handling for the "terminate" app
      if (
        (config::input.enable_input_only_mode && appid == proc::terminate_app_id)
        || appuuid_str == TERMINATE_APP_UUID
      ) {
        proc::proc.terminate();

        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 410);
        tree.put("root.<xmlattr>.status_message", "App terminated.");

        return;
      }

      if (
        current_appid > 0
        && current_appid != proc::input_only_app_id
        && (
          (appid > 0 && appid != current_appid)
          || (!appuuid_str.empty() && appuuid_str != current_app_uuid)
        )
      ) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

        return;
      }
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, is_input_only, args, named_cert_p);
    const bool watch_only = launch_session->watch_only;

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    bool no_active_sessions = rtsp_stream::session_count() == 0;
    if (watch_only && (current_appid == 0 || no_active_sessions)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "No active stream is available to watch");

      return;
    }

    if (const auto watch_error = pin_watch_session_to_active_profile(*launch_session)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", watch_error->first);
      tree.put("root.<xmlattr>.status_message", watch_error->second);

      return;
    }

    if (is_input_only) {
      BOOST_LOG(info) << "Launching input only session..."sv;

      launch_session->client_do_cmds.clear();
      launch_session->client_undo_cmds.clear();

      if (current_appid != 0) {
        if (!watch_only && !proc::proc.is_session_owner(named_cert_p->uuid)) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 470);
          tree.put("root.<xmlattr>.status_message", "The current session belongs to another client");

          return;
        }
        if (!watch_only && !session_token_matches_request(args)) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 470);
          tree.put("root.<xmlattr>.status_message", "The requested session token does not match the active session");

          return;
        }

        launch_session->session_token = proc::proc.get_session_token();
      }

      // Still probe encoders once, if input only session is launched first
      // But we're ignoring if it's successful or not
      if (no_active_sessions && !proc::proc.virtual_display) {
#ifdef __linux__
        if (should_defer_encoder_probe_until_cage()) {
          BOOST_LOG(info) << "nvhttp: Deferring input-only encoder probe until the cage runtime is available"sv;
        } else
#endif
        {
          video::probe_encoders();
        }
        if (current_appid == 0) {
          proc::proc.launch_input_only(launch_session);
        }
      }
    } else if (appid > 0 || !appuuid_str.empty()) {
      if (appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid)) {
        // We're basically resuming the same app

        BOOST_LOG(debug) << "Resuming app [" << proc::proc.get_last_run_app_name() << "] from launch app path...";

        if (!watch_only && !proc::proc.is_session_owner(named_cert_p->uuid)) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 470);
          tree.put("root.<xmlattr>.status_message", "The current session belongs to another client");

          return;
        }
        if (!watch_only && !session_token_matches_request(args)) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 470);
          tree.put("root.<xmlattr>.status_message", "The requested session token does not match the active session");

          return;
        }

        launch_session->session_token = proc::proc.get_session_token();

        if (watch_only || !proc::proc.allow_client_commands || !named_cert_p->allow_client_commands) {
          launch_session->client_do_cmds.clear();
          launch_session->client_undo_cmds.clear();
        }

        if (current_appid == proc::input_only_app_id) {
          launch_session->input_only = true;
        }

        if (no_active_sessions && !proc::proc.virtual_display) {
          display_device::configure_display(config::video, *launch_session);
#ifdef __linux__
          if (should_defer_encoder_probe_until_cage()) {
            BOOST_LOG(info) << "nvhttp: Deferring resume-time encoder probe until the cage runtime is available"sv;
          } else
#endif
          if (video::probe_encoders()) {
            tree.put("root.resume", 0);
            tree.put("root.<xmlattr>.status_code", 503);
            tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

            return;
          }
        }
      } else {
        const auto& apps = proc::proc.get_apps();
        auto app_iter = std::find_if(apps.begin(), apps.end(), [&appid_str, &appuuid_str](const auto _app) {
          return _app.id == appid_str || _app.uuid == appuuid_str;
        });

        if (app_iter == apps.end()) {
          BOOST_LOG(error) << "Couldn't find app with ID ["sv << appid_str << "] or UUID ["sv << appuuid_str << ']';
          tree.put("root.<xmlattr>.status_code", 404);
          tree.put("root.<xmlattr>.status_message", "Cannot find requested application");
          tree.put("root.gamesession", 0);
          return;
        }

        if (!app_iter->allow_client_commands) {
          launch_session->client_do_cmds.clear();
          launch_session->client_undo_cmds.clear();
        }

        // Update last_launched timestamp
        try {
          std::string content = file_handler::read_file(config::stream.file_apps.c_str());
          auto file_tree = nlohmann::json::parse(content);
          if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
            for (auto &app_node : file_tree["apps"]) {
              if (app_node.value("uuid", "") == app_iter->uuid) {
                app_node["last-launched"] = std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
                break;
              }
            }
            file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
          }
        } catch (...) {}

        auto err = proc::proc.execute(*app_iter, launch_session);
        if (err) {
          tree.put("root.<xmlattr>.status_code", err);
          tree.put(
            "root.<xmlattr>.status_message",
            err == 503
            ? "Failed to initialize video capture/encoding. Is a display connected and turned on?"
            : "Failed to start the specified application");
          tree.put("root.gamesession", 0);

          return;
        }
      }
    } else {
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "How did you get here?");
      tree.put("root.gamesession", 0);
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.sessionToken", launch_session->session_token);
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto named_cert_p = get_verified_cert(request);
    if (!named_cert_p) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 401);
      tree.put("root.<xmlattr>.status_message", "The client is not authorized. Certificate verification failed.");
      return;
    }

    if (!(named_cert_p->perm & PERM::_allow_view)) {
      BOOST_LOG(debug) << "Permission ViewApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    auto launch_session = make_launch_session(host_audio, false, args, named_cert_p);
    const bool watch_only = launch_session->watch_only;

    if (watch_only && no_active_sessions) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "No active stream is available to watch");

      return;
    }

    if (const auto watch_error = pin_watch_session_to_active_profile(*launch_session)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", watch_error->first);
      tree.put("root.<xmlattr>.status_message", watch_error->second);

      return;
    }

    if (!watch_only && !proc::proc.is_session_owner(named_cert_p->uuid)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 470);
      tree.put("root.<xmlattr>.status_message", "The current session belongs to another client");

      return;
    }
    if (!watch_only && !session_token_matches_request(args)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 470);
      tree.put("root.<xmlattr>.status_message", "The requested session token does not match the active session");

      return;
    }
    launch_session->session_token = proc::proc.get_session_token();

    if (watch_only || !proc::proc.allow_client_commands || !named_cert_p->allow_client_commands) {
      launch_session->client_do_cmds.clear();
      launch_session->client_undo_cmds.clear();
    }

    if (config::input.enable_input_only_mode && current_appid == proc::input_only_app_id) {
      launch_session->input_only = true;
    }

    if (no_active_sessions && !proc::proc.virtual_display) {
      // We want to prepare display only if there are no active sessions
      // and the current session isn't virtual display at the moment.
      // This should be done before probing encoders as it could change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
#ifdef __linux__
      if (should_defer_encoder_probe_until_cage()) {
        BOOST_LOG(info) << "nvhttp: Deferring launch-time encoder probe until the cage runtime is available"sv;
      } else
#endif
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.sessionToken", launch_session->session_token);
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
    system_tray::update_tray_client_connected(named_cert_p->name);
#endif
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    auto named_cert_p = get_verified_cert(request);
    if (!named_cert_p) {
      tree.put("root.cancel", 0);
      tree.put("root.<xmlattr>.status_code", 401);
      tree.put("root.<xmlattr>.status_message", "The client is not authorized. Certificate verification failed.");
      return;
    }

    if (!(named_cert_p->perm & PERM::launch)) {
      BOOST_LOG(debug) << "Permission CancelApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }

    if (proc::proc.running() > 0 && !proc::proc.is_session_owner(named_cert_p->uuid)) {
      tree.put("root.cancel", 0);
      tree.put("root.<xmlattr>.status_code", 470);
      tree.put("root.<xmlattr>.status_message", "The current session belongs to another client");

      return;
    }
    if (proc::proc.running() > 0 && !session_token_matches_request(args)) {
      tree.put("root.cancel", 0);
      tree.put("root.<xmlattr>.status_code", 470);
      tree.put("root.<xmlattr>.status_message", "The requested session token does not match the active session");

      return;
    }

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    proc::proc.set_session_shutdown_requested(true);
    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    } else {
      proc::proc.set_session_shutdown_requested(false);
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    auto fg = util::fail_guard([&]() {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
    });

    auto named_cert_p = get_verified_cert(request);
    if (!named_cert_p) {
      fg.disable();
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    if (!(named_cert_p->perm & PERM::_all_actions)) {
      BOOST_LOG(debug) << "Permission Get AppAsset denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      fg.disable();
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image(util::from_view(get_arg(args, "appid")));

    fg.disable();

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void getClipboard(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);
    if (!named_cert_p) {
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    if (
      !(named_cert_p->perm & PERM::_allow_view)
      || !(named_cert_p->perm & PERM::clipboard_read)
    ) {
      BOOST_LOG(debug) << "Permission Read Clipboard denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    if (clipboard_type != "text"sv) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), named_cert_p->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client ["<< named_cert_p->name << "] trying to get clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    std::string content = platf::get_clipboard();
    response->write(content);
    return;
  }

  void
  setClipboard(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);
    if (!named_cert_p) {
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    if (
      !(named_cert_p->perm & PERM::_allow_view)
      || !(named_cert_p->perm & PERM::clipboard_set)
    ) {
      BOOST_LOG(debug) << "Permission Write Clipboard denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    if (clipboard_type != "text"sv) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), named_cert_p->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client ["<< named_cert_p->name << "] trying to set clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    std::string content = request->content.string();

    bool success = platf::set_clipboard(content);

    if (!success) {
      BOOST_LOG(debug) << "Setting clipboard failed!";

      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
    }

    response->write();
    return;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  void start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [](req_https_t req, SSL *ssl) {
      if (auto named_cert_p = verify_client_cert(ssl, false)) {
        req->userp = named_cert_p;
        if (req->path.rfind("/polaris/v1/", 0) == 0) {
          remember_client_family(named_cert_p.get(), "nova");
        }
        BOOST_LOG(debug) << named_cert_p->name << " -- verified"sv;
      }

      return true;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<PolarisHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<PolarisHTTPS>;
    https_server.resource["^/pair$"]["GET"] = pair<PolarisHTTPS>;
    https_server.resource["^/unpair$"]["GET"] = unpair<PolarisHTTPS>;
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;
    https_server.resource["^/actions/clipboard$"]["GET"] = getClipboard;
    https_server.resource["^/actions/clipboard$"]["POST"] = setClipboard;

    // -----------------------------------------------------------------------
    // Polaris v1 API — client cert auth (same TLS as Moonlight pairing)
    // -----------------------------------------------------------------------

    auto polarisCapabilities = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);

      auto named_cert_p = get_verified_cert(request);
      if (!named_cert_p) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      nlohmann::json output;
      output["server"] = "polaris";
      output["version"] = PROJECT_VERSION;

      auto &build = output["build"];
      build["cuda"] = build_has_cuda();

      // Feature flags
      auto &features = output["features"];
      features["ai_optimizer"] = ai_optimizer::is_enabled();
      features["ai_optimizer_control"] = true;
      features["adaptive_bitrate_control"] = true;
      features["game_library"] = true;
      features["session_lifecycle"] = true;
      features["device_profiles"] = true;
      features["stream_policy_v1"] = true;
      features["client_settings_v1"] = true;
      features["cursor_visibility_control"] = true;
      features["lock_screen_control"] = false;
#ifdef __linux__
      features["lock_screen_control"] = true;
#endif

      output["client_settings"] = {
        {"version", 1},
        {"endpoint", "/polaris/v1/client-settings"},
        {"direction", "bidirectional"},
        {"source_of_truth", "polaris_effective_runtime"},
        {"fields", nlohmann::json::array({
          "stream_display_mode",
          "display_mode",
          "target_bitrate_kbps",
          "adaptive_bitrate_enabled",
          "ai_optimizer_enabled"
        })}
      };

      // Capture info
      auto &capture = output["capture"];
#ifdef __linux__
      capture["backend"] = cage_display_router::is_running() ? "cage-screencopy" : "portal";
      capture["compositor"] = "cage";
#else
      capture["backend"] = "platform";
      capture["compositor"] = "none";
#endif
      capture["max_resolution"] = "3840x2160";
      capture["max_fps"] = 120;

      auto &codecs = capture["codecs"];
      codecs = nlohmann::json::array({"h264"});
      if (config::video.hevc_mode > 1) codecs.push_back("hevc");
      if (config::video.av1_mode > 1) codecs.push_back("av1");

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    auto polarisSessionStatus = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);

      const auto named_cert_p = get_verified_cert(request);
      if (!named_cert_p) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      nlohmann::json output;

      // Session state from the state machine
      auto stats = stream_stats::get_current();
      const auto session_state = confighttp::get_session_state();
      const auto session_token = proc::proc.get_session_token();
      const bool owned_by_client =
        !session_token.empty() && proc::proc.is_session_owner(named_cert_p->uuid);
      const bool shutdown_requested = proc::proc.session_shutdown_requested();
      output["state"] = session_state;
      output["streaming_active"] = stats.streaming;
      output["shutdown_requested"] = shutdown_requested;
      auto &build = output["build"];
      build["cuda"] = build_has_cuda();
#ifdef __linux__
      output["cage_pid"] = cage_display_router::get_pid();
      output["screen_locked"] = session_manager::is_screen_locked();
#endif
      output["owned_by_client"] = owned_by_client;
      output["session_token"] = session_token;
      output["owner_unique_id"] = proc::proc.get_session_owner_unique_id();
      output["owner_device_name"] = proc::proc.get_session_owner_device_name();
      output["viewer_count"] = rtsp_stream::viewer_count();

      std::string client_role = "none";
      if (const auto session = rtsp_stream::find_session(named_cert_p->uuid)) {
        client_role = stream::session::is_watch_only(*session) ? "viewer" : "owner";
      }
      output["client_role"] = client_role;
      const bool host_tuning_allowed =
        owned_by_client &&
        client_role != "viewer" &&
        !shutdown_requested &&
        stats.streaming;
      const bool quit_allowed =
        host_tuning_allowed &&
        proc::proc.allow_client_commands &&
        named_cert_p->allow_client_commands &&
        proc::proc.running() > 0;

      // Game info
      output["game_id"] = proc::proc.running();
      output["game"] = proc::proc.get_last_run_app_name();
      output["game_uuid"] = proc::proc.get_running_app_uuid();
      output["cursor_visible"] = cursor::visible();
      output["dynamic_range"] = stats.dynamic_range;
      auto &hdr = output["hdr"];
      hdr["client_dynamic_range"] = stats.dynamic_range;
      hdr["display_hdr"] = stats.display_hdr;
      hdr["metadata_available"] = stats.hdr_metadata_available;
      hdr["stream_hdr_enabled"] = stats.stream_hdr_enabled;
      hdr["color_coding"] = stats.color_coding;
      output["adaptive_bitrate_enabled"] = adaptive_bitrate::is_enabled();
      output["adaptive_target_bitrate_kbps"] = stats.adaptive_target_bitrate_kbps;
      output["ai_optimizer_enabled"] = ai_optimizer::is_enabled();
      output["mangohud_configured"] = proc::proc.current_app_has_mangohud();

      auto &controls = output["controls"];
      controls["host_tuning_allowed"] = host_tuning_allowed;
      controls["quit_allowed"] = quit_allowed;
      controls["shutdown_in_progress"] = shutdown_requested;
      controls["client_commands_enabled"] = proc::proc.allow_client_commands;
      controls["device_commands_enabled"] = named_cert_p->allow_client_commands;

      auto &tuning = output["tuning"];
      tuning["adaptive_bitrate_enabled"] = adaptive_bitrate::is_enabled();
      tuning["adaptive_target_bitrate_kbps"] = stats.adaptive_target_bitrate_kbps;
      tuning["ai_optimizer_enabled"] = ai_optimizer::is_enabled();
      tuning["mangohud_configured"] = proc::proc.current_app_has_mangohud();

      auto &display_mode = output["display_mode"];
      const auto stream_display_mode = effective_stream_display_mode_selection(stats);
      display_mode["virtual_display"] = proc::proc.virtual_display;
      display_mode["requested_headless"] = stats.runtime_requested_headless;
      display_mode["effective_headless"] = stats.runtime_effective_headless;
      display_mode["gpu_native_override_active"] = stats.runtime_gpu_native_override_active;
      display_mode["selection"] = stream_display_mode;
      display_mode["stream_display_mode"] = stream_display_mode;
      display_mode["explicit_choice"] = proc::proc.session_display_mode_is_explicit();
      display_mode["requested"] =
        session_token.empty() ? "" :
        proc::proc.session_display_mode_is_explicit() ?
          (proc::proc.virtual_display ? "virtual_display" : "headless") :
          "auto";
      display_mode["label"] = stream_display_mode_label_for_selection(stream_display_mode);
      display_mode["reason"] = stream_display_mode_reason_for_selection(stream_display_mode);
      display_mode["paired_display_mode_override"] = named_cert_p->display_mode;
      display_mode["paired_display_mode_locked"] = !named_cert_p->display_mode.empty();
      display_mode["paired_target_bitrate_kbps"] = named_cert_p->target_bitrate_kbps;
      display_mode["paired_target_bitrate_locked"] = named_cert_p->target_bitrate_kbps > 0;

      // Capture info
      auto &capture_info = output["capture"];
      capture_info["backend"] = stats.runtime_backend.empty() ? "screencopy" : stats.runtime_backend;
      capture_info["resolution"] = std::to_string(stats.width) + "x" + std::to_string(stats.height);
      capture_info["transport"] = platf::from_frame_transport(stats.capture_transport);
      capture_info["residency"] = platf::from_frame_residency(stats.capture_residency);
      capture_info["format"] = platf::from_frame_format(stats.capture_format);
      capture_info["path"] = stream_stats::capture_path_summary(stats);
      const auto capture_reason = stream_stats::capture_path_reason(stats);
      capture_info["reason"] = capture_reason;
      capture_info["reason_message"] = capture_path_reason_message(capture_reason);
      capture_info["cpu_copy"] = stream_stats::capture_path_uses_cpu_copy(stats);
      capture_info["gpu_native"] = stream_stats::capture_path_is_gpu_native(stats);

      // Encoder info
      auto &encoder = output["encoder"];
      encoder["active_backend"] = video::active_encoder_name().empty() ? "unknown" : video::active_encoder_name();
      encoder["codec"] = stats.codec;
      encoder["bitrate_kbps"] = stats.bitrate_kbps;
      encoder["fps"] = stats.fps;
      encoder["requested_client_fps"] = stats.requested_client_fps;
      encoder["session_target_fps"] = stats.session_target_fps;
      encoder["encode_target_fps"] = stats.encode_target_fps;
      encoder["target_device"] = stats.encode_target_device;
      encoder["target_residency"] = platf::from_frame_residency(stats.encode_target_residency);
      encoder["target_format"] = platf::from_frame_format(stats.encode_target_format);
      encoder["pacing_policy"] = stats.pacing_policy;
      encoder["optimization_source"] = stats.optimization_source;
      encoder["optimization_confidence"] = stats.optimization_confidence;
      encoder["optimization_cache_status"] = stats.optimization_cache_status;
      encoder["optimization_reasoning"] = stats.optimization_reasoning;
      encoder["optimization_normalization_reason"] = stats.optimization_normalization_reason;
      encoder["recommendation_version"] = stats.recommendation_version;
      const auto health = build_session_health_json(
        stats,
        proc::proc.virtual_display,
        named_cert_p->name,
        proc::proc.get_last_run_app_name()
      );
      output["health"] = health;
      output["stream_policy"] = build_stream_policy_json(
        *named_cert_p,
        stats,
        health
      );
      output["client_settings"] = build_client_settings_json(
        *named_cert_p,
        stats,
        health
      );

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    auto polarisStreamPolicy = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);

      const auto named_cert_p = get_verified_cert(request);
      if (!named_cert_p) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        auto query = request->parse_query_string();
        std::string app_name = proc::proc.get_last_run_app_name();
        for (auto &[key, val] : query) {
          if (key == "game") {
            app_name = val;
          }
        }

        if (request->method == "POST") {
          std::string body_str(std::istreambuf_iterator<char>(request->content), {});
          const auto body = body_str.empty() ? nlohmann::json::object() : nlohmann::json::parse(body_str);

          std::string display_mode = named_cert_p->display_mode;
          if (body.value("clear_display_mode", false)) {
            display_mode.clear();
          } else if (body.contains("display_mode")) {
            if (!body["display_mode"].is_string()) {
              nlohmann::json err;
              err["error"] = "display_mode must be a string";
              SimpleWeb::CaseInsensitiveMultimap headers;
              headers.emplace("Content-Type", "application/json");
              response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
              return;
            }
            display_mode = body["display_mode"].get<std::string>();
          }

          int width = 0;
          int height = 0;
          double fps = 0.0;
          if (!display_mode.empty() && !parse_stream_policy_display_mode(display_mode, width, height, fps)) {
            nlohmann::json err;
            err["error"] = "display_mode must use WIDTHxHEIGHTxFPS, for example 1920x1080x120";
            SimpleWeb::CaseInsensitiveMultimap headers;
            headers.emplace("Content-Type", "application/json");
            response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
            return;
          }

          const bool updated = update_device_info(
            named_cert_p->uuid,
            named_cert_p->name,
            display_mode,
            named_cert_p->target_bitrate_kbps,
            named_cert_p->do_cmds,
            named_cert_p->undo_cmds,
            named_cert_p->perm,
            named_cert_p->enable_legacy_ordering,
            named_cert_p->allow_client_commands,
            named_cert_p->always_use_virtual_display
          );

          if (!updated) {
            nlohmann::json err;
            err["error"] = "paired client not found";
            SimpleWeb::CaseInsensitiveMultimap headers;
            headers.emplace("Content-Type", "application/json");
            response->write(SimpleWeb::StatusCode::client_error_not_found, err.dump(), headers);
            return;
          }
        }

        const auto stats = stream_stats::get_current();
        const auto health = build_session_health_json(
          stats,
          proc::proc.virtual_display,
          named_cert_p->name,
          app_name
        );

        nlohmann::json output;
        output["status"] = true;
        output["stream_policy"] = build_stream_policy_json(*named_cert_p, stats, health);
        output["health"] = health;

        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    auto polarisClientSettings = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);

      const auto named_cert_p = get_verified_cert(request);
      if (!named_cert_p) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      auto write_json = [&](const nlohmann::json &body,
                            SimpleWeb::StatusCode status = SimpleWeb::StatusCode::success_ok) {
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(status, body.dump(), headers);
      };

      try {
        if (request->method == "POST") {
          std::string body_str(std::istreambuf_iterator<char>(request->content), {});
          const auto body = body_str.empty() ? nlohmann::json::object() : nlohmann::json::parse(body_str);

          std::optional<std::string> stream_display_mode;
          if (body.contains("stream_display_mode")) {
            if (!body["stream_display_mode"].is_string()) {
              write_json({{"error", "stream_display_mode must be a string"}}, SimpleWeb::StatusCode::client_error_bad_request);
              return;
            }

            std::string error;
            stream_display_mode = body["stream_display_mode"].get<std::string>();
            if (*stream_display_mode != "headless_stream" &&
                *stream_display_mode != "desktop_display" &&
                *stream_display_mode != "host_virtual_display" &&
                *stream_display_mode != "windowed_stream") {
              error = "stream_display_mode must be headless_stream, desktop_display, host_virtual_display, or windowed_stream";
              write_json({{"error", error}}, SimpleWeb::StatusCode::client_error_bad_request);
              return;
            }
          }

          std::string display_mode = named_cert_p->display_mode;
          if (body.value("clear_display_mode", false)) {
            display_mode.clear();
          } else if (body.contains("display_mode")) {
            if (!body["display_mode"].is_string()) {
              write_json({{"error", "display_mode must be a string"}}, SimpleWeb::StatusCode::client_error_bad_request);
              return;
            }
            display_mode = body["display_mode"].get<std::string>();
          }

          int width = 0;
          int height = 0;
          double fps = 0.0;
          if (!display_mode.empty() && !parse_stream_policy_display_mode(display_mode, width, height, fps)) {
            write_json(
              {{"error", "display_mode must use WIDTHxHEIGHTxFPS, for example 1920x1080x120"}},
              SimpleWeb::StatusCode::client_error_bad_request
            );
            return;
          }

          int target_bitrate_kbps = named_cert_p->target_bitrate_kbps;
          if (body.value("clear_target_bitrate", false)) {
            target_bitrate_kbps = 0;
          } else if (body.contains("target_bitrate_kbps")) {
            if (!body["target_bitrate_kbps"].is_number_integer()) {
              write_json({{"error", "target_bitrate_kbps must be an integer"}}, SimpleWeb::StatusCode::client_error_bad_request);
              return;
            }
            target_bitrate_kbps = body["target_bitrate_kbps"].get<int>();
            if (target_bitrate_kbps != 0 && (target_bitrate_kbps < 1000 || target_bitrate_kbps > 300000)) {
              write_json({{"error", "target_bitrate_kbps must be 0 or between 1000 and 300000"}}, SimpleWeb::StatusCode::client_error_bad_request);
              return;
            }
          }

          if (body.contains("adaptive_bitrate_enabled")) {
            if (!body["adaptive_bitrate_enabled"].is_boolean()) {
              write_json({{"error", "adaptive_bitrate_enabled must be a boolean"}}, SimpleWeb::StatusCode::client_error_bad_request);
              return;
            }
            const bool enabled = body["adaptive_bitrate_enabled"].get<bool>();
            if (!persist_config_values({{"adaptive_bitrate_enabled", bool_config_value(enabled)}})) {
              write_json({{"error", "failed to persist adaptive bitrate setting"}}, SimpleWeb::StatusCode::server_error_internal_server_error);
              return;
            }
            adaptive_bitrate::set_enabled(enabled);
          }

          if (body.contains("ai_optimizer_enabled")) {
            if (!body["ai_optimizer_enabled"].is_boolean()) {
              write_json({{"error", "ai_optimizer_enabled must be a boolean"}}, SimpleWeb::StatusCode::client_error_bad_request);
              return;
            }
            const bool enabled = body["ai_optimizer_enabled"].get<bool>();
            if (!persist_config_values({{"ai_enabled", bool_config_value(enabled)}})) {
              write_json({{"error", "failed to persist AI Optimizer setting"}}, SimpleWeb::StatusCode::server_error_internal_server_error);
              return;
            }
            ai_optimizer::set_enabled(enabled);
          }

          if (stream_display_mode) {
            std::string error;
            if (!apply_stream_display_mode_selection(*stream_display_mode, error)) {
              write_json({{"error", error}}, SimpleWeb::StatusCode::client_error_bad_request);
              return;
            }
          }

          if (target_bitrate_kbps > 0) {
            adaptive_bitrate::set_base_bitrate(target_bitrate_kbps);
          }

          const bool updated = update_device_info(
            named_cert_p->uuid,
            named_cert_p->name,
            display_mode,
            target_bitrate_kbps,
            named_cert_p->do_cmds,
            named_cert_p->undo_cmds,
            named_cert_p->perm,
            named_cert_p->enable_legacy_ordering,
            named_cert_p->allow_client_commands,
            named_cert_p->always_use_virtual_display
          );

          if (!updated) {
            write_json({{"error", "paired client not found"}}, SimpleWeb::StatusCode::client_error_not_found);
            return;
          }
        }

        const auto stats = stream_stats::get_current();
        const auto health = build_session_health_json(
          stats,
          proc::proc.virtual_display,
          named_cert_p->name,
          proc::proc.get_last_run_app_name()
        );

        nlohmann::json output;
        output["status"] = true;
        output["client_settings"] = build_client_settings_json(*named_cert_p, stats, health);
        output["sync_status"] = output["client_settings"]["sync_status"];
        output["stream_policy"] = output["client_settings"]["policy"];
        output["health"] = health;
        write_json(output);
      } catch (std::exception &e) {
        write_json({{"error", e.what()}}, SimpleWeb::StatusCode::server_error_internal_server_error);
      }
    };

    // Game library — returns games with metadata, covers, categories
    auto polarisGames = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }
      const auto advertised_codec_support = advertised_codec_support_for_http(true);

      // Parse query params
      auto query = request->parse_query_string();
      std::string search_query;
      std::string source_filter;
      int limit = 50, offset = 0;
      for (auto &[key, val] : query) {
        if (key == "search") search_query = val;
        else if (key == "source") source_filter = val;
        else if (key == "limit") try { limit = std::stoi(val); } catch (...) {}
        else if (key == "offset") try { offset = std::stoi(val); } catch (...) {}
      }

      auto apps = proc::proc.get_apps();
      nlohmann::json games = nlohmann::json::array();

      int idx = 0;
      for (auto &app : apps) {
        // Skip non-game entries (Desktop, Lutris launcher)
        if (app.name == "Desktop") continue;

        // Search filter
        if (!search_query.empty()) {
          std::string name_lower = app.name;
          std::string query_lower = search_query;
          std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
          std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
          if (name_lower.find(query_lower) == std::string::npos) continue;
        }

        // Source filter
        if (!source_filter.empty()) {
          bool is_steam = !app.steam_appid.empty();
          if (source_filter == "steam" && !is_steam) continue;
          if (source_filter == "other" && is_steam) continue;
        }

        // Pagination
        if (idx < offset) { idx++; continue; }
        if ((int)games.size() >= limit) break;

        nlohmann::json game;
        game["id"] = app.uuid;
        game["app_id"] = app.id;
        game["name"] = app.name;
        game["source"] = app.steam_appid.empty() ? "other" : "steam";
        game["steam_appid"] = app.steam_appid;
        game["category"] = app.game_category;
        game["source"] = app.source;
        game["installed"] = true;
        game["hdr_supported"] = advertised_codec_support.hevc_mode == 3;
        game["cover_url"] = "/polaris/v1/games/" + app.uuid + "/cover";
        game["last_launched"] = app.last_launched;
        game["launch_mode"] = launch_mode_contract_for_app(app);

        nlohmann::json genre_arr = nlohmann::json::array();
        for (const auto &g : app.genres) genre_arr.push_back(g);
        game["genres"] = genre_arr;
        game["mangohud"] = app.env_vars.count("MANGOHUD") > 0 && app.env_vars.at("MANGOHUD") == "1";

        games.push_back(game);
        idx++;
      }

      nlohmann::json output;
      output["games"] = games;
      output["total"] = idx;

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    // Game cover art
    auto polarisGameCover = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      // Extract UUID from path: /polaris/v1/games/{uuid}/cover
      auto path = request->path;
      auto start = path.find("/games/") + 7;
      auto end = path.find("/cover");
      if (start == std::string::npos || end == std::string::npos) {
        response->write(SimpleWeb::StatusCode::client_error_not_found);
        return;
      }
      std::string uuid = path.substr(start, end - start);

      // Find the app and its image
      auto apps = proc::proc.get_apps();
      for (auto &app : apps) {
        if (app.uuid == uuid) {
          std::string image_path = proc::validate_app_image_path(app.image_path);

          // Check polaris covers directory first
          if (!app.steam_appid.empty()) {
            const std::string cover_stem = config::sunshine.config_file.substr(
              0, config::sunshine.config_file.rfind('/')) + "/covers/steam_" + app.steam_appid;
            for (const auto &ext : {".png", ".jpg", ".jpeg", ".webp"}) {
              const std::string cover_path = cover_stem + ext;
              if (access(cover_path.c_str(), F_OK) == 0) {
                image_path = cover_path;
                break;
              }
            }
          }

          // Try reading the image file
          try {
            auto data = file_handler::read_file(image_path.c_str());
            SimpleWeb::CaseInsensitiveMultimap headers;
            std::string content_type = "image/png";
            auto ext = fs::path(image_path).extension().string();
            boost::algorithm::to_lower(ext);
            if (ext == ".jpg" || ext == ".jpeg") {
              content_type = "image/jpeg";
            } else if (ext == ".webp") {
              content_type = "image/webp";
            }
            headers.emplace("Content-Type", content_type);
            headers.emplace("Cache-Control", "max-age=86400");
            response->write(data, headers);
            return;
          } catch (...) {}
          break;
        }
      }

      response->write(SimpleWeb::StatusCode::client_error_not_found);
    };

    // Game launch via Polaris API
    auto polarisLaunchGame = [&host_audio](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);

        std::string game_id = body.value("game_id", "");
        if (game_id.empty()) {
          nlohmann::json err;
          err["error"] = "game_id required";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        // Smart Launch: read client display info for resolution matching
        int client_width = body.value("client_width", 0);
        int client_height = body.value("client_height", 0);
        int client_fps = body.value("client_fps", 0);

        if (client_width > 0 && client_height > 0) {
          BOOST_LOG(info) << "Smart Launch: client display " << client_width << "x" << client_height
                          << " @ " << (client_fps > 0 ? client_fps : 60) << " fps";

#ifdef __linux__
          // Pre-configure labwc resolution to match client device
          if (config::video.linux_display.use_cage_compositor && cage_display_router::is_running()) {
            BOOST_LOG(info) << "Smart Launch: adjusting labwc resolution to " << client_width << "x" << client_height;
            // Note: labwc will be restarted with the correct resolution when the game launches
            // via the cage_display_router::start() call in process.cpp.
            // Store the preferred resolution for the session.
          }
#endif
        }

        // Find the app by UUID
        auto apps = proc::proc.get_apps();
        int app_id = -1;
        std::string app_name;
        for (size_t i = 0; i < apps.size(); i++) {
          if (apps[i].uuid == game_id) {
            app_id = i + 1;  // 1-indexed
            app_name = apps[i].name;
            break;
          }
        }

        if (app_id < 0) {
          nlohmann::json err;
          err["error"] = "Game not found";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_not_found, err.dump(), headers);
          return;
        }

        // Update last_launched timestamp in apps.json
        try {
          std::string content = file_handler::read_file(config::stream.file_apps.c_str());
          auto file_tree = nlohmann::json::parse(content);
          if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
            for (auto &app_node : file_tree["apps"]) {
              if (app_node.value("uuid", "") == game_id) {
                app_node["last-launched"] = std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
                break;
              }
            }
            file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
          }
        } catch (...) {
          BOOST_LOG(warning) << "Failed to update last-launched timestamp";
        }

        nlohmann::json output;
        output["status"] = "launching";
        output["game"] = app_name;
        output["game_id"] = game_id;
        if (client_width > 0 && client_height > 0) {
          output["smart_launch"] = {
            {"client_width", client_width},
            {"client_height", client_height},
            {"client_fps", client_fps > 0 ? client_fps : 60}
          };
        }

        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);

      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    // Toggle MangoHud for a game (sets/clears MANGOHUD=1 in env)
    auto polarisToggleMangoHud = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);

        std::string game_id = body.value("game_id", "");
        bool enabled = body.value("mangohud", false);

        if (game_id.empty()) {
          nlohmann::json err;
          err["error"] = "game_id required";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        // Update apps.json
        std::string content = file_handler::read_file(config::stream.file_apps.c_str());
        auto file_tree = nlohmann::json::parse(content);
        bool found = false;
        if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
          for (auto &app_node : file_tree["apps"]) {
            if (app_node.value("uuid", "") == game_id) {
              if (!app_node.contains("env") || !app_node["env"].is_object()) {
                app_node["env"] = nlohmann::json::object();
              }
              if (enabled) {
                app_node["env"]["MANGOHUD"] = "1";
              } else {
                app_node["env"].erase("MANGOHUD");
                if (app_node["env"].empty()) app_node.erase("env");
              }
              found = true;
              break;
            }
          }
          if (found) {
            file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
            proc::proc.set_app_mangohud_configured(game_id, enabled);
            // Do NOT call proc::refresh() here — it would terminate the running stream.
            // The env change takes effect on the next game launch.
          }
        }

        nlohmann::json output;
        output["status"] = found;
        output["mangohud"] = enabled;
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    // Real-time bitrate adjustment from client
    auto polarisSetBitrate = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      const auto named_cert_p = get_verified_cert(request);
      if (!named_cert_p) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }
      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);
        int bitrate_kbps = body.value("bitrate_kbps", 0);
        if (bitrate_kbps < 1000 || bitrate_kbps > 300000) {
          nlohmann::json err;
          err["error"] = "bitrate_kbps must be between 1000 and 300000";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }
        adaptive_bitrate::set_base_bitrate(bitrate_kbps);
        BOOST_LOG(info) << "Client requested bitrate change: " << bitrate_kbps << " kbps";

        nlohmann::json output;
        output["status"] = true;
        output["bitrate_kbps"] = bitrate_kbps;
        const auto stats = stream_stats::get_current();
        const auto health = build_session_health_json(
          stats,
          proc::proc.virtual_display,
          named_cert_p->name,
          proc::proc.get_last_run_app_name()
        );
        output["client_settings"] = build_client_settings_json(*named_cert_p, stats, health);
        output["sync_status"] = output["client_settings"]["sync_status"];
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    auto polarisSetAdaptiveBitrate = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      const auto named_cert_p = get_verified_cert(request);
      if (!named_cert_p) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);
        if (!body.contains("enabled") || !body["enabled"].is_boolean()) {
          nlohmann::json err;
          err["error"] = "enabled must be a boolean";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        const bool enabled = body["enabled"].get<bool>();
        if (!persist_config_values({{"adaptive_bitrate_enabled", bool_config_value(enabled)}})) {
          nlohmann::json err;
          err["error"] = "failed to persist adaptive bitrate setting";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
          return;
        }
        adaptive_bitrate::set_enabled(enabled);
        BOOST_LOG(info) << "Adaptive bitrate toggled via Polaris API: " << (enabled ? "enabled" : "disabled");

        nlohmann::json output;
        output["status"] = true;
        output["adaptive_bitrate_enabled"] = adaptive_bitrate::is_enabled();
        const auto stats = stream_stats::get_current();
        output["adaptive_target_bitrate_kbps"] = stats.adaptive_target_bitrate_kbps;
        const auto health = build_session_health_json(
          stats,
          proc::proc.virtual_display,
          named_cert_p->name,
          proc::proc.get_last_run_app_name()
        );
        output["client_settings"] = build_client_settings_json(*named_cert_p, stats, health);
        output["sync_status"] = output["client_settings"]["sync_status"];
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    auto polarisSetAiOptimizer = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      const auto named_cert_p = get_verified_cert(request);
      if (!named_cert_p) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);
        if (!body.contains("enabled") || !body["enabled"].is_boolean()) {
          nlohmann::json err;
          err["error"] = "enabled must be a boolean";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        const bool enabled = body["enabled"].get<bool>();
        if (!persist_config_values({{"ai_enabled", bool_config_value(enabled)}})) {
          nlohmann::json err;
          err["error"] = "failed to persist AI Optimizer setting";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
          return;
        }
        ai_optimizer::set_enabled(enabled);

        nlohmann::json output;
        output["status"] = true;
        output["ai_optimizer_enabled"] = ai_optimizer::is_enabled();
        output["effective"] = ai_optimizer::is_enabled();
        const auto stats = stream_stats::get_current();
        const auto health = build_session_health_json(
          stats,
          proc::proc.virtual_display,
          named_cert_p->name,
          proc::proc.get_last_run_app_name()
        );
        output["client_settings"] = build_client_settings_json(*named_cert_p, stats, health);
        output["sync_status"] = output["client_settings"]["sync_status"];
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    auto polarisSetCursorVisibility = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);
        if (!body.contains("visible") || !body["visible"].is_boolean()) {
          nlohmann::json err;
          err["error"] = "visible must be a boolean";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        const bool visible = body["visible"].get<bool>();
        cursor::set_visible(visible);
        BOOST_LOG(info) << "Client requested cursor visibility change: " << (visible ? "visible" : "hidden");

        nlohmann::json output;
        output["status"] = true;
        output["visible"] = visible;
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    // Client session report — Nova sends quality summary at session end
    auto polarisSessionReport = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }
      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);

        std::string device = body.value("device", "");
        std::string game = body.value("game", "");
        std::string unique_id = body.value("unique_id", "");
        double avg_fps = body.value("avg_fps", 0.0);
        double avg_latency = body.value("avg_latency_ms", 0.0);
        int avg_bitrate = body.value("avg_bitrate_kbps", 0);
        double packet_loss = body.value("packet_loss_pct", 0.0);
        std::string codec = body.value("codec", "");
        double target_fps = body.value("target_fps", 0.0);
        int duration_s = body.value("duration_s", 0);
        std::string end_reason = body.value("end_reason", "disconnect");
        std::string optimization_source = body.value("optimization_source", "");
        std::string optimization_confidence = body.value("optimization_confidence", "");
        int recommendation_version = body.value("recommendation_version", 0);
        std::string health_grade = body.value("health_grade", "");
        std::string primary_issue = body.value("primary_issue", "");
        std::vector<std::string> issues;
        if (body.contains("issues") && body["issues"].is_array()) {
          for (const auto &item : body["issues"]) {
            if (item.is_string()) {
              issues.push_back(item.get<std::string>());
            }
          }
        }
        std::string decoder_risk = body.value("decoder_risk", "");
        std::string hdr_risk = body.value("hdr_risk", "");
        std::string network_risk = body.value("network_risk", "");
        std::string capture_path = body.value("capture_path", "");
        int safe_bitrate_kbps = body.value("safe_bitrate_kbps", 0);
        std::string safe_codec = body.value("safe_codec", "");
        std::string safe_display_mode = body.value("safe_display_mode", "");
        std::optional<bool> safe_hdr;
        if (body.contains("safe_hdr") && body["safe_hdr"].is_boolean()) {
          safe_hdr = body["safe_hdr"].get<bool>();
        }
        bool relaunch_recommended = body.value("relaunch_recommended", false);

        if (!device.empty() && !game.empty() && ai_optimizer::is_enabled()) {
          const auto canonical_device = device_db::canonicalize_name(device);
          const auto owner_device_name = proc::proc.get_session_owner_device_name();
          const auto canonical_owner_device = device_db::canonicalize_name(owner_device_name);
          const bool device_matches_owner =
            !canonical_device.empty() &&
            !canonical_owner_device.empty() &&
            canonical_device == canonical_owner_device;
          const bool app_matches_active_session = proc::proc.get_last_run_app_name() == game;
          const bool matches_active_owner =
            app_matches_active_session &&
            ((!unique_id.empty() && proc::proc.is_session_owner(unique_id)) || device_matches_owner);
          if (matches_active_owner) {
            if (!owner_device_name.empty()) {
              device = owner_device_name;
            }
            proc::proc.mark_client_session_report_recorded(unique_id);
            BOOST_LOG(info) << "Client session report matched active session owner; host-side duplicate recording disabled for ["
                            << device_db::canonicalize_name(device) << ":" << game << "]";
          } else {
            device = canonical_device;
          }

          ai_optimizer::session_history_t session;
          session.avg_fps = avg_fps;
          session.avg_latency_ms = avg_latency;
          session.avg_bitrate_kbps = avg_bitrate;
          session.packet_loss_pct = packet_loss;
          session.last_fps = avg_fps;
          session.last_target_fps = target_fps > 0.0 ? target_fps : avg_fps;
          session.last_latency_ms = avg_latency;
          session.last_bitrate_kbps = avg_bitrate;
          session.last_packet_loss_pct = packet_loss;
          session.last_codec = codec;
          session.session_count = 1;

          const double effective_target_fps = session.last_target_fps > 0.0 ? session.last_target_fps : avg_fps;
          const double fps_ratio =
            (effective_target_fps > 0.0 && avg_fps > 0.0) ? std::clamp(avg_fps / effective_target_fps, 0.0, 1.5) : 0.0;
          if (avg_fps <= 0.0)
            session.last_quality_grade = "F";
          else if (fps_ratio >= 0.95 && packet_loss < 0.5 && avg_latency < 20.0)
            session.last_quality_grade = "A";
          else if (fps_ratio >= 0.85 && packet_loss < 2.0 && avg_latency < 40.0)
            session.last_quality_grade = "B";
          else if (fps_ratio >= 0.70 && packet_loss < 5.0)
            session.last_quality_grade = "C";
          else if (fps_ratio >= 0.50)
            session.last_quality_grade = "D";
          else
            session.last_quality_grade = "F";
          session.quality_grade = session.last_quality_grade;
          session.codec = session.last_codec;

          session.last_optimization_source = optimization_source;
          session.last_optimization_confidence = optimization_confidence;
          session.last_recommendation_version = recommendation_version;
          session.last_health_grade = health_grade;
          session.last_primary_issue = primary_issue;
          session.last_issues = std::move(issues);
          session.last_decoder_risk = decoder_risk;
          session.last_hdr_risk = hdr_risk;
          session.last_network_risk = network_risk;
          session.last_capture_path = capture_path;
          session.last_safe_bitrate_kbps = safe_bitrate_kbps;
          session.last_safe_codec = safe_codec;
          session.last_safe_display_mode = safe_display_mode;
          session.last_safe_hdr = safe_hdr;
          session.last_relaunch_recommended = relaunch_recommended;

          ai_optimizer::record_session(device, game, session);
          BOOST_LOG(info) << "Client session report: " << device << " + " << game
                          << " → grade " << session.quality_grade
                          << " (fps=" << avg_fps << ", lat=" << avg_latency << "ms, dur=" << duration_s << "s)";
        }

        nlohmann::json output;
        output["status"] = true;
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    // AI optimization query — Nova asks for recommended settings before launching
    auto polarisOptimize = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      auto args = request->parse_query_string();
      std::string device = args.count("device") ? args.find("device")->second : "";
      std::string game = args.count("game") ? args.find("game")->second : "";

      nlohmann::json output;
      device_db::optimization_t effective_optimization;
      std::optional<std::string> suggested_codec;
      std::optional<int> target_bitrate_kbps;
      bool hdr_requested = false;
      const auto session_history = ai_optimizer::get_session_history(device, game);
      const auto device_profile = device_db::get_device(device);
      const auto gpu_info = config::video.adapter_name.empty()
        ? "NVIDIA GPU (NVENC)"s
        : config::video.adapter_name;

      // Try AI cache first, then device_db
      auto ai_opt = ai_optimizer::get_cached(device, game);
      if (ai_opt) {
        effective_optimization = *ai_opt;
        append_optimization_json(output, *ai_opt);
        if (ai_opt->target_bitrate_kbps) {
          target_bitrate_kbps = *ai_opt->target_bitrate_kbps;
        }
        suggested_codec = ai_opt->preferred_codec;
        hdr_requested = ai_opt->hdr.value_or(false);
      } else {
        auto opt = device_db::get_optimization(device, game);
        if (session_history && session_history->last_invalidated_at > 0) {
          opt.cache_status = "invalidated";
        }
        effective_optimization = opt;
        append_optimization_json(output, opt);
        target_bitrate_kbps = opt.target_bitrate_kbps;
        suggested_codec = opt.preferred_codec;
        hdr_requested = opt.hdr.value_or(false);

        if (ai_optimizer::is_enabled()) {
          if (ai_optimizer::should_sync_on_cache_miss()) {
            if (auto sync_opt = ai_optimizer::request_sync(device, game, gpu_info, "", session_history)) {
              effective_optimization = *sync_opt;
              output = nlohmann::json::object();
              append_optimization_json(output, *sync_opt);
              if (sync_opt->target_bitrate_kbps) {
                target_bitrate_kbps = *sync_opt->target_bitrate_kbps;
              }
              suggested_codec = sync_opt->preferred_codec;
              hdr_requested = sync_opt->hdr.value_or(false);
              BOOST_LOG(info) << "ai_optimizer: Sync-prewarmed optimization for \""sv << device
                              << "\" + \""sv << game << "\" before launch"sv;
            } else {
              ai_optimizer::request_async(device, game, gpu_info, "", session_history);
            }
          } else {
            ai_optimizer::request_async(device, game, gpu_info, "", session_history);
          }
        }
      }

      suggested_codec = device_db::normalize_preferred_codec(
        device,
        game,
        suggested_codec,
        target_bitrate_kbps,
        hdr_requested
      );
      if (suggested_codec) {
        output["preferred_codec"] = *suggested_codec;
      }
      output["stability"] = build_stability_plan_json(
        device,
        game,
        device_profile,
        effective_optimization,
        session_history,
        suggested_codec,
        target_bitrate_kbps,
        hdr_requested
      );
      if (session_history) {
        output["last_quality_grade"] = session_history->quality_grade;
        output["poor_outcome_count"] = session_history->poor_outcome_count;
        output["consecutive_poor_outcomes"] = session_history->consecutive_poor_outcomes;
        output["last_invalidated_at"] = session_history->last_invalidated_at;
      }

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    https_server.resource["^/polaris/v1/session/report$"]["POST"] = polarisSessionReport;
    https_server.resource["^/polaris/v1/optimize$"]["GET"] = polarisOptimize;
    https_server.resource["^/polaris/v1/capabilities$"]["GET"] = polarisCapabilities;
    https_server.resource["^/polaris/v1/session/status$"]["GET"] = polarisSessionStatus;
    https_server.resource["^/polaris/v1/client-settings$"]["GET"] = polarisClientSettings;
    https_server.resource["^/polaris/v1/client-settings$"]["POST"] = polarisClientSettings;
    https_server.resource["^/polaris/v1/stream-policy$"]["GET"] = polarisStreamPolicy;
    https_server.resource["^/polaris/v1/stream-policy$"]["POST"] = polarisStreamPolicy;
    https_server.resource["^/polaris/v1/session/bitrate$"]["POST"] = polarisSetBitrate;
    https_server.resource["^/polaris/v1/session/adaptive-bitrate$"]["POST"] = polarisSetAdaptiveBitrate;
    https_server.resource["^/polaris/v1/session/ai-optimizer$"]["POST"] = polarisSetAiOptimizer;
    https_server.resource["^/polaris/v1/session/cursor$"]["POST"] = polarisSetCursorVisibility;
    https_server.resource["^/polaris/v1/games$"]["GET"] = polarisGames;
    https_server.resource["^/polaris/v1/games/.+/cover$"]["GET"] = polarisGameCover;
    https_server.resource["^/polaris/v1/games/.+/mangohud$"]["POST"] = polarisToggleMangoHud;
    https_server.resource["^/polaris/v1/session/launch$"]["POST"] = polarisLaunchGame;

    https_server.config.reuse_address = true;
    https_server.config.address = net::af_to_any_address_string(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = pair<SimpleWeb::HTTP>;
    http_server.resource["^/unpair$"]["GET"] = unpair<SimpleWeb::HTTP>;

    http_server.config.reuse_address = true;
    http_server.config.address = net::af_to_any_address_string(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    map_id_sess.clear();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  std::string request_otp(const std::string& passphrase, const std::string& deviceName) {
    if (passphrase.size() < 4) {
      return "";
    }

    one_time_pin = crypto::rand_alphabet(4, "0123456789"sv);
    otp_passphrase = passphrase;
    otp_device_name = deviceName;
    otp_creation_time = std::chrono::steady_clock::now();

    return one_time_pin;
  }

  void
  erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
    load_state();
  }

  void stop_session(stream::session_t& session, bool graceful) {
    if (graceful) {
      stream::session::graceful_stop(session);
    } else {
      stream::session::stop(session);
    }
  }

  bool find_and_stop_session(const std::string& uuid, bool graceful) {
    auto session = rtsp_stream::find_session(uuid);
    if (session) {
      stop_session(*session, graceful);
      return true;
    }
    return false;
  }

  void update_session_info(stream::session_t& session, const std::string& name, const crypto::PERM newPerm) {
    stream::session::update_device_info(session, name, newPerm);
  }

  bool find_and_udpate_session_info(const std::string& uuid, const std::string& name, const crypto::PERM newPerm) {
    auto session = rtsp_stream::find_session(uuid);
    if (session) {
      update_session_info(*session, name, newPerm);
      return true;
    }
    return false;
  }

  bool update_device_info(
    const std::string& uuid,
    const std::string& name,
    const std::string& display_mode,
    const int target_bitrate_kbps,
    const cmd_list_t& do_cmds,
    const cmd_list_t& undo_cmds,
    const crypto::PERM newPerm,
    const bool enable_legacy_ordering,
    const bool allow_client_commands,
    const bool always_use_virtual_display
  ) {
    find_and_udpate_session_info(uuid, name, newPerm);

    client_t &client = client_root;
    auto it = client.named_devices.begin();
    for (; it != client.named_devices.end(); ++it) {
      auto named_cert_p = *it;
      if (named_cert_p->uuid == uuid) {
        named_cert_p->name = name;
        named_cert_p->display_mode = display_mode;
        named_cert_p->target_bitrate_kbps = target_bitrate_kbps;
        named_cert_p->perm = newPerm;
        named_cert_p->do_cmds = do_cmds;
        named_cert_p->undo_cmds = undo_cmds;
        named_cert_p->enable_legacy_ordering = enable_legacy_ordering;
        named_cert_p->allow_client_commands = allow_client_commands;
        named_cert_p->always_use_virtual_display = always_use_virtual_display;
        save_state();
        return true;
      }
    }

    return false;
  }

  bool unpair_client(const std::string_view uuid) {
    bool removed = false;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it)->uuid == uuid) {
        it = client.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    save_state();
    load_state();

    if (removed) {
      auto session = rtsp_stream::find_session(uuid);
      if (session) {
        stop_session(*session, true);
      }

      if (client.named_devices.empty()) {
        proc::proc.terminate();
      }
    }

    return removed;
  }
}  // namespace nvhttp
