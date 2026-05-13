/**
 * @file src/adaptive_bitrate.h
 * @brief Adaptive bitrate controller using EWMA-based network feedback.
 *
 * Dynamically adjusts encoding bitrate based on packet loss and RTT
 * statistics reported by the streaming client, similar to Parsec and
 * Steam Remote Play adaptive streaming.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <string>

namespace adaptive_bitrate {

  struct config_t {
    bool enabled = false;
    int min_bitrate_kbps = 2000;       // 2 Mbps floor
    int max_bitrate_kbps = 100000;     // 100 Mbps ceiling
    double max_change_rate = 0.20;     // Max 20% change per adjustment
    double ewma_alpha = 0.3;           // EWMA smoothing factor (0-1, higher = more responsive)
    int adjustment_interval_ms = 1000; // How often to adjust
  };

  struct state_t {
    bool enabled = false;
    int base_bitrate_kbps = 0;
    int target_bitrate_kbps = 0;
    int min_bitrate_kbps = 0;
    int max_bitrate_kbps = 0;
    double ewma_packet_loss = 0.0;
    double ewma_rtt_ms = 0.0;
    std::string state = "disabled";
    std::string reason = "disabled";
  };

  /**
   * @brief Feed network statistics from client loss reports.
   * @param packet_loss_percent Packet loss percentage (0-100).
   * @param rtt_ms Round-trip time in milliseconds.
   */
  void update_network_stats(double packet_loss_percent, double rtt_ms);

  /**
   * @brief Feed local stream health so bitrate can react to host pacing pressure.
   */
  void update_stream_health(double fps_ratio,
                            double dropped_frame_ratio,
                            double duplicate_frame_ratio,
                            double frame_jitter_ms,
                            double encode_time_ms,
                            double avg_frame_age_ms);

  /**
   * @brief Get the current recommended bitrate.
   * @return Target bitrate in kbps, or 0 if adaptive bitrate is disabled.
   */
  int get_target_bitrate_kbps();

  /**
   * @brief Get the current controller state for status APIs and HUDs.
   */
  state_t get_state();

  /**
   * @brief Set the base bitrate from client request.
   * @param kbps Base bitrate in kilobits per second.
   */
  void set_base_bitrate(int kbps);

  /**
   * @brief Enable or disable adaptive bitrate control.
   * @param enabled True to enable, false to disable.
   */
  void set_enabled(bool enabled);

  /**
   * @brief Check if adaptive bitrate control is enabled.
   * @return True if enabled.
   */
  bool is_enabled();

  /**
   * @brief Load configuration from polaris config system.
   */
  void load_config();

  /**
   * @brief Reset all state (call when a new stream session starts).
   */
  void reset();

}  // namespace adaptive_bitrate
