/**
 * @file src/adaptive_bitrate.cpp
 * @brief Adaptive bitrate controller using EWMA-based network feedback.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

// local includes
#include "adaptive_bitrate.h"
#include "config.h"
#include "logging.h"

using namespace std::literals;

namespace adaptive_bitrate {

  // Internal state protected by mutex for complex operations,
  // atomics for simple reads from the encoding thread.
  static std::mutex state_mutex;

  static config_t current_config;
  static std::atomic<bool> enabled {false};
  static std::atomic<int> base_bitrate_kbps {0};
  static std::atomic<int> target_bitrate_kbps {0};

  // EWMA-smoothed network metrics
  static double ewma_packet_loss = 0.0;
  static double ewma_rtt = 0.0;
  static double avg_rtt = 0.0;
  static int rtt_sample_count = 0;

  // Timing for recovery and adjustment intervals
  static std::chrono::steady_clock::time_point last_adjustment_time;
  static std::chrono::steady_clock::time_point last_loss_time;

  // Track whether we have received any stats yet
  static bool initialized = false;

  void update_network_stats(double packet_loss_percent, double rtt_ms) {
    if (!enabled.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    auto now = std::chrono::steady_clock::now();

    if (!initialized) {
      ewma_packet_loss = packet_loss_percent;
      ewma_rtt = rtt_ms;
      avg_rtt = rtt_ms;
      rtt_sample_count = 1;
      last_adjustment_time = now;
      last_loss_time = (packet_loss_percent > 0) ? now : (now - 10s);
      initialized = true;
      return;
    }

    // Update EWMA smoothed values
    double alpha = current_config.ewma_alpha;
    ewma_packet_loss = alpha * packet_loss_percent + (1.0 - alpha) * ewma_packet_loss;
    ewma_rtt = alpha * rtt_ms + (1.0 - alpha) * ewma_rtt;

    // Track long-term average RTT for spike detection
    if (rtt_sample_count < 1000) {
      rtt_sample_count++;
    }
    avg_rtt += (rtt_ms - avg_rtt) / rtt_sample_count;

    // Track when we last saw loss
    if (packet_loss_percent > 0.0) {
      last_loss_time = now;
    }

    // Check if enough time has passed for an adjustment
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_adjustment_time);
    if (elapsed.count() < current_config.adjustment_interval_ms) {
      return;
    }
    last_adjustment_time = now;

    int base = base_bitrate_kbps.load(std::memory_order_relaxed);
    int current_target = target_bitrate_kbps.load(std::memory_order_relaxed);
    if (base <= 0) {
      return;
    }

    double max_change = current_config.max_change_rate;
    int new_target = current_target;

    // RTT spike detection: if current RTT is more than 2x the long-term average,
    // treat it as congestion and reduce bitrate immediately
    bool rtt_spike = (rtt_sample_count > 5) && (rtt_ms > avg_rtt * 2.0) && (avg_rtt > 0);

    if (ewma_packet_loss > 1.0 || rtt_spike) {
      // Network is congested -- reduce bitrate

      // Scale reduction by severity of the loss
      double reduction_factor;
      if (rtt_spike && ewma_packet_loss <= 1.0) {
        // RTT spike without packet loss: moderate reduction
        reduction_factor = 0.5 * max_change;
      } else if (ewma_packet_loss > 5.0) {
        // Heavy loss: maximum reduction
        reduction_factor = max_change;
      } else {
        // Moderate loss: proportional reduction (1-5% loss -> proportional within max_change)
        reduction_factor = max_change * std::min(ewma_packet_loss / 5.0, 1.0);
      }

      new_target = static_cast<int>(current_target * (1.0 - reduction_factor));

      BOOST_LOG(debug) << "Adaptive bitrate: reducing "
                       << current_target << " -> " << new_target << " kbps"
                       << " (loss=" << ewma_packet_loss << "%, rtt=" << ewma_rtt << "ms"
                       << (rtt_spike ? ", RTT spike" : "") << ")";

    } else {
      // Network is healthy -- consider increasing bitrate back toward base

      // Only increase if no loss for at least 5 seconds
      auto time_since_loss = std::chrono::duration_cast<std::chrono::seconds>(now - last_loss_time);
      if (time_since_loss.count() >= 5 && current_target < base) {
        // Gradual recovery: increase by a fraction of max_change_rate
        // Use half the max change rate for recovery to be conservative
        double increase_factor = max_change * 0.5;
        new_target = static_cast<int>(current_target * (1.0 + increase_factor));

        // Don't overshoot the base bitrate
        new_target = std::min(new_target, base);

        BOOST_LOG(debug) << "Adaptive bitrate: recovering "
                         << current_target << " -> " << new_target << " kbps"
                         << " (loss=" << ewma_packet_loss << "%, rtt=" << ewma_rtt << "ms)";
      }
    }

    // Clamp to configured bounds
    new_target = std::clamp(new_target, current_config.min_bitrate_kbps, current_config.max_bitrate_kbps);

    // Also clamp to not exceed the base bitrate from the client
    new_target = std::min(new_target, base);

    target_bitrate_kbps.store(new_target, std::memory_order_relaxed);
  }

  int get_target_bitrate_kbps() {
    if (!enabled.load(std::memory_order_relaxed)) {
      return 0;
    }
    return target_bitrate_kbps.load(std::memory_order_relaxed);
  }

  void set_base_bitrate(int kbps) {
    base_bitrate_kbps.store(kbps, std::memory_order_relaxed);

    // Initialize target to base when first set
    int expected = 0;
    target_bitrate_kbps.compare_exchange_strong(expected, kbps, std::memory_order_relaxed);
  }

  void set_enabled(bool enable) {
    enabled.store(enable, std::memory_order_relaxed);
  }

  bool is_enabled() {
    return enabled.load(std::memory_order_relaxed);
  }

  void load_config() {
    std::lock_guard<std::mutex> lock(state_mutex);

    current_config.enabled = config::video.adaptive_bitrate.enabled;
    current_config.min_bitrate_kbps = config::video.adaptive_bitrate.min_bitrate_kbps;
    current_config.max_bitrate_kbps = config::video.adaptive_bitrate.max_bitrate_kbps;

    enabled.store(current_config.enabled, std::memory_order_relaxed);

    BOOST_LOG(info) << "Adaptive bitrate: "
                    << (current_config.enabled ? "enabled" : "disabled")
                    << " (min=" << current_config.min_bitrate_kbps
                    << " kbps, max=" << current_config.max_bitrate_kbps << " kbps)";
  }

  void reset() {
    std::lock_guard<std::mutex> lock(state_mutex);

    ewma_packet_loss = 0.0;
    ewma_rtt = 0.0;
    avg_rtt = 0.0;
    rtt_sample_count = 0;
    initialized = false;

    base_bitrate_kbps.store(0, std::memory_order_relaxed);
    target_bitrate_kbps.store(0, std::memory_order_relaxed);
  }

}  // namespace adaptive_bitrate
