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

namespace adaptive_bitrate {

  struct config_t {
    bool enabled = false;
    int min_bitrate_kbps = 2000;       // 2 Mbps floor
    int max_bitrate_kbps = 100000;     // 100 Mbps ceiling
    double max_change_rate = 0.20;     // Max 20% change per adjustment
    double ewma_alpha = 0.3;           // EWMA smoothing factor (0-1, higher = more responsive)
    int adjustment_interval_ms = 1000; // How often to adjust
  };

  /**
   * @brief Feed network statistics from client loss reports.
   * @param packet_loss_percent Packet loss percentage (0-100).
   * @param rtt_ms Round-trip time in milliseconds.
   */
  void update_network_stats(double packet_loss_percent, double rtt_ms);

  /**
   * @brief Get the current recommended bitrate.
   * @return Target bitrate in kbps, or 0 if adaptive bitrate is disabled.
   */
  int get_target_bitrate_kbps();

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
