/**
 * @file src/stream_stats.h
 * @brief Thread-safe stream statistics collector for real-time monitoring.
 *
 * Supports tracking multiple simultaneous streaming clients.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// local includes
#include "platform/common.h"

namespace stream_stats {

  /**
   * @brief Per-client statistics for multi-session tracking.
   */
  struct client_stats_t {
    std::string name;
    std::string ip;

    // Video
    double fps = 0;
    int bitrate_kbps = 0;
    double encode_time_ms = 0;
    std::string codec;
    int width = 0;
    int height = 0;

    // Network
    double latency_ms = 0;
    double packet_loss = 0;
    uint64_t bytes_sent = 0;

    // Adaptive bitrate
    int adaptive_target_bitrate_kbps = 0;
  };

  struct capture_profile_sample_t {
    platf::frame_transport_e transport = platf::frame_transport_e::unknown;
    std::chrono::microseconds dispatch_time {};
    std::chrono::microseconds ingest_time {};
    std::chrono::microseconds total_time {};
  };

  struct stats_t {
    // Stream state
    bool streaming = false;
    std::string client_name;
    std::string client_ip;
    std::string runtime_backend;
    bool runtime_requested_headless = false;
    bool runtime_effective_headless = false;
    bool runtime_gpu_native_override_active = false;
    platf::frame_transport_e capture_transport = platf::frame_transport_e::unknown;
    platf::frame_residency_e capture_residency = platf::frame_residency_e::unknown;
    platf::frame_format_e capture_format = platf::frame_format_e::unknown;
    std::string encode_target_device;
    platf::frame_residency_e encode_target_residency = platf::frame_residency_e::unknown;
    platf::frame_format_e encode_target_format = platf::frame_format_e::unknown;
    int dynamic_range = 0;
    bool display_hdr = false;
    bool hdr_metadata_available = false;
    bool stream_hdr_enabled = false;
    std::string color_coding;

    // Video (primary/first client for backward compatibility)
    double fps = 0;
    double requested_client_fps = 0;
    double session_target_fps = 0;
    double encode_target_fps = 0;
    int bitrate_kbps = 0;
    double encode_time_ms = 0;
    double duplicate_frame_ratio = 0;
    double dropped_frame_ratio = 0;
    double avg_frame_age_ms = 0;
    double frame_jitter_ms = 0;
    std::string codec;
    std::string pacing_policy;
    std::string optimization_source;
    std::string optimization_confidence;
    std::string optimization_cache_status;
    std::string optimization_reasoning;
    std::string optimization_normalization_reason;
    int recommendation_version = 0;
    int width = 0;
    int height = 0;

    // Network
    double latency_ms = 0;
    double packet_loss = 0;
    uint64_t bytes_sent = 0;

    // Adaptive bitrate
    int adaptive_target_bitrate_kbps = 0;

    // System
    double gpu_usage = 0;

    // Multi-client
    std::vector<client_stats_t> clients;

    /**
     * @brief Serialize stats to a JSON string.
     * @return JSON string representation.
     */
    std::string to_json() const;
  };

  /**
   * @brief Whether the current capture/encode path requires a CPU-side copy.
   * @param stats Current stream statistics snapshot.
   */
  bool capture_path_uses_cpu_copy(const stats_t &stats);

  /**
   * @brief Whether both capture and encode conversion are GPU-resident.
   * @param stats Current stream statistics snapshot.
   */
  bool capture_path_is_gpu_native(const stats_t &stats);

  /**
   * @brief Human-readable capture path classification for diagnostics.
   * @param stats Current stream statistics snapshot.
   */
  std::string capture_path_summary(const stats_t &stats);

  /**
   * @brief Update stream active state.
   * @param active Whether streaming is active.
   * @param client_name Name of the connected client.
   * @param client_ip IP address of the connected client.
   */
  void update_stream_active(bool active, const std::string &client_name = "", const std::string &client_ip = "");

  /**
   * @brief Add a new client session to the stats tracker.
   * @param client_ip IP address used as session key.
   * @param client_name Display name of the client.
   */
  void add_client(const std::string &client_ip, const std::string &client_name);

  /**
   * @brief Remove a client session from the stats tracker.
   * @param client_ip IP address of the client to remove.
   */
  void remove_client(const std::string &client_ip);

  /**
   * @brief Update video statistics (backward-compatible single-client API).
   * @param fps Current frames per second.
   * @param bitrate_kbps Current bitrate in kbps.
   * @param encode_time_ms Current encode time in milliseconds.
   * @param codec Current codec name.
   * @param width Current video width.
   * @param height Current video height.
   */
  void update_video_stats(double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height);

  /**
   * @brief Update video statistics for a specific client.
   * @param client_ip IP address of the client.
   * @param fps Current frames per second.
   * @param bitrate_kbps Current bitrate in kbps.
   * @param encode_time_ms Current encode time in milliseconds.
   * @param codec Current codec name.
   * @param width Current video width.
   * @param height Current video height.
   */
  void update_video_stats(const std::string &client_ip, double fps, int bitrate_kbps, double encode_time_ms, const std::string &codec, int width, int height);

  /**
   * @brief Update static session targets for pacing and optimization telemetry.
   * @param requested_client_fps FPS originally requested by the client session.
   * @param session_target_fps FPS Polaris selected for the session before encode pacing.
   * @param encode_target_fps FPS the encoder loop is targeting.
   * @param pacing_policy Human-readable pacing policy label.
   * @param optimization_source Optimization layer(s) that influenced the session.
   * @param optimization_confidence Confidence attached to the chosen recommendation.
   * @param optimization_cache_status Whether the recommendation came from cache or a fresh request.
   * @param optimization_reasoning Human-readable summary for the chosen recommendation.
   * @param optimization_normalization_reason Explanation for any server-side correction.
   * @param recommendation_version Optimization schema version used to produce the result.
   */
  void update_session_targets(double requested_client_fps,
                              double session_target_fps,
                              double encode_target_fps,
                              const std::string &pacing_policy,
                              const std::string &optimization_source,
                              const std::string &optimization_confidence,
                              const std::string &optimization_cache_status,
                              const std::string &optimization_reasoning,
                              const std::string &optimization_normalization_reason,
                              int recommendation_version);

  /**
   * @brief Update frame delivery telemetry derived from the encode loop.
   * @param duplicate_frame_ratio Ratio of encoded frames that reused a prior image.
   * @param dropped_frame_ratio Ratio of capture frames dropped before encode.
   * @param avg_frame_age_ms Mean frame age at encode submission time.
   * @param frame_jitter_ms Mean absolute inter-frame timing error.
   */
  void update_frame_delivery(double duplicate_frame_ratio,
                             double dropped_frame_ratio,
                             double avg_frame_age_ms,
                             double frame_jitter_ms);

  /**
   * @brief Update network statistics.
   * @param latency_ms Current latency in milliseconds.
   * @param packet_loss Current packet loss percentage (0-100).
   * @param bytes_sent Total bytes sent.
   */
  void update_network_stats(double latency_ms, double packet_loss, uint64_t bytes_sent);

  /**
   * @brief Update network statistics for a specific client.
   * @param client_ip IP address of the client.
   * @param latency_ms Current latency in milliseconds.
   * @param packet_loss Current packet loss percentage (0-100).
   * @param bytes_sent Total bytes sent.
   */
  void update_network_stats(const std::string &client_ip, double latency_ms, double packet_loss, uint64_t bytes_sent);

  /**
   * @brief Update runtime mode metadata exposed to the dashboard.
   * @param state Current runtime state for the active backend.
   */
  void update_runtime_state(const platf::runtime_state_t &state);

  /**
   * @brief Update current capture path metadata exposed to the dashboard.
   * @param metadata Current frame transport/residency/format metadata.
   */
  void update_capture_metadata(const platf::frame_metadata_t &metadata);

  /**
   * @brief Update the encode conversion path metadata exposed to the dashboard.
   * @param target_device Human-readable target device label for the converter.
   * @param target_residency Residency of the converter output.
   * @param target_format Pixel format of the converter output.
   */
  void update_encode_path_metadata(const std::string &target_device,
                                   platf::frame_residency_e target_residency,
                                   platf::frame_format_e target_format);

  /**
   * @brief Update the currently negotiated dynamic range mode.
   * @param dynamic_range Dynamic range mode from the active RTSP session.
   */
  void update_dynamic_range(int dynamic_range);

  /**
   * @brief Update the display HDR state and negotiated stream color coding.
   * @param display_hdr Whether the active display path reports HDR.
   * @param hdr_metadata_available Whether HDR metadata was available from the display path.
   * @param stream_hdr_enabled Whether Polaris is advertising true HDR for the stream.
   * @param color_coding Human-readable color coding label.
   */
  void update_hdr_state(bool display_hdr,
                        bool hdr_metadata_available,
                        bool stream_hdr_enabled,
                        const std::string &color_coding);

  /**
   * @brief Record a single capture timing sample into the shared telemetry sink.
   * @param sample Timing sample tagged with the frame transport used.
   */
  void update_capture_profile(const capture_profile_sample_t &sample);

  /**
   * @brief Get the number of active client sessions.
   * @return Count of active clients.
   */
  int active_client_count();

  /**
   * @brief Get a snapshot of the current stats.
   * @return Copy of the current stats.
   */
  stats_t get_current();

}  // namespace stream_stats
