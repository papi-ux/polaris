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
      const auto &linux_display = config::video.linux_display;
      if (!linux_display.headless_mode) {
        return "Desktop Display";
      }
      if (!linux_display.use_cage_compositor) {
        return "Host Virtual Display";
      }
      if (linux_display.prefer_gpu_native_capture) {
        return "Windowed Stream";
      }
      return "Headless Stream";
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
    j["capture_path"] = capture_path_summary(*this);
    j["capture_path_reason"] = capture_path_reason(*this);
    j["capture_cpu_copy"] = capture_path_uses_cpu_copy(*this);
    j["capture_gpu_native"] = capture_path_is_gpu_native(*this);
    j["encode_target_device"] = encode_target_device;
    j["encode_target_residency"] = platf::from_frame_residency(encode_target_residency);
    j["encode_target_format"] = platf::from_frame_format(encode_target_format);
    j["dynamic_range"] = dynamic_range;
    j["display_hdr"] = display_hdr;
    j["hdr_metadata_available"] = hdr_metadata_available;
    j["stream_hdr_enabled"] = stream_hdr_enabled;
    j["color_coding"] = color_coding;
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
        return "headless_shm_default";
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
