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
  static std::chrono::steady_clock::time_point last_pressure_time;

  // Track whether we have received any stats yet
  static bool initialized = false;
  static std::string controller_state = "disabled";
  static std::string controller_reason = "disabled";

  static void set_controller_status(const std::string &state, const std::string &reason) {
    controller_state = state;
    controller_reason = reason;
  }

  static void normalize_config_bounds(config_t &config) {
    if (config.max_bitrate_kbps >= config.min_bitrate_kbps) {
      return;
    }

    BOOST_LOG(warning) << "Adaptive bitrate: max bitrate "
                       << config.max_bitrate_kbps << " kbps is below min bitrate "
                       << config.min_bitrate_kbps << " kbps; using min as max";
    config.max_bitrate_kbps = config.min_bitrate_kbps;
  }

  static int clamp_target(int target, int base) {
    target = std::clamp(target, current_config.min_bitrate_kbps, current_config.max_bitrate_kbps);
    return std::min(target, base);
  }

  static bool interval_elapsed(std::chrono::steady_clock::time_point now) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_adjustment_time);
    if (elapsed.count() < current_config.adjustment_interval_ms) {
      return false;
    }
    last_adjustment_time = now;
    return true;
  }

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
      last_pressure_time = (packet_loss_percent > 0) ? now : (now - 10s);
      set_controller_status("steady", "warming_up");
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
      last_pressure_time = now;
    }

    // Check if enough time has passed for an adjustment
    if (!interval_elapsed(now)) {
      return;
    }

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
      last_pressure_time = now;
      set_controller_status("network_pressure", rtt_spike ? "rtt_spike" : "packet_loss");

      BOOST_LOG(debug) << "Adaptive bitrate: reducing "
                       << current_target << " -> " << new_target << " kbps"
                       << " (loss=" << ewma_packet_loss << "%, rtt=" << ewma_rtt << "ms"
                       << (rtt_spike ? ", RTT spike" : "") << ")";

    } else {
      // Network is healthy -- consider increasing bitrate back toward base

      // Only increase if the stream has been healthy for at least 10 seconds
      auto time_since_pressure = std::chrono::duration_cast<std::chrono::seconds>(now - last_pressure_time);
      if (time_since_pressure.count() >= 10 && current_target < base) {
        // Gradual recovery: increase by a fraction of max_change_rate
        // Use a smaller recovery step so reconnects and transient spikes do not bounce.
        double increase_factor = max_change * 0.4;
        new_target = static_cast<int>(current_target * (1.0 + increase_factor));

        // Don't overshoot the base bitrate
        new_target = std::min(new_target, base);
        set_controller_status("recovering", "healthy_window");

        BOOST_LOG(debug) << "Adaptive bitrate: recovering "
                         << current_target << " -> " << new_target << " kbps"
                         << " (loss=" << ewma_packet_loss << "%, rtt=" << ewma_rtt << "ms)";
      }
    }

    if (new_target == current_target && controller_state != "network_pressure") {
      set_controller_status("steady", "healthy");
    }

    target_bitrate_kbps.store(clamp_target(new_target, base), std::memory_order_relaxed);
  }

  void update_stream_health(double fps_ratio,
                            double dropped_frame_ratio,
                            double duplicate_frame_ratio,
                            double frame_jitter_ms,
                            double encode_time_ms,
                            double avg_frame_age_ms) {
    if (!enabled.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    auto now = std::chrono::steady_clock::now();
    if (!initialized) {
      last_adjustment_time = now;
      last_pressure_time = now - 10s;
      initialized = true;
    }

    const bool encoder_pressure = encode_time_ms >= 11.0;
    const bool pacing_pressure =
      (fps_ratio > 0.0 && fps_ratio < 0.88) ||
      dropped_frame_ratio >= 0.04 ||
      duplicate_frame_ratio >= 0.10 ||
      frame_jitter_ms >= 2.2 ||
      encoder_pressure;
    if (!pacing_pressure) {
      return;
    }
    if (!encoder_pressure) {
      set_controller_status("frame_pacing_observed", "frame_pacing");
      return;
    }
    if (!interval_elapsed(now)) {
      last_pressure_time = now;
      return;
    }

    int base = base_bitrate_kbps.load(std::memory_order_relaxed);
    int current_target = target_bitrate_kbps.load(std::memory_order_relaxed);
    if (base <= 0 || current_target <= 0) {
      return;
    }

    const int new_target = clamp_target(static_cast<int>(current_target * 0.88), base);
    last_pressure_time = now;
    set_controller_status("encoder_pressure", "encode_load");
    if (new_target != current_target) {
      BOOST_LOG(debug) << "Adaptive bitrate: reducing "
                       << current_target << " -> " << new_target << " kbps"
                       << " (fps_ratio=" << fps_ratio
                       << ", dropped=" << dropped_frame_ratio
                       << ", duplicate=" << duplicate_frame_ratio
                       << ", jitter=" << frame_jitter_ms
                       << "ms, encode=" << encode_time_ms
                       << "ms, age=" << avg_frame_age_ms << "ms)";
      target_bitrate_kbps.store(new_target, std::memory_order_relaxed);
    }
  }

  int get_target_bitrate_kbps() {
    if (!enabled.load(std::memory_order_relaxed)) {
      return 0;
    }
    return target_bitrate_kbps.load(std::memory_order_relaxed);
  }

  state_t get_state() {
    std::lock_guard<std::mutex> lock(state_mutex);
    state_t state;
    state.enabled = enabled.load(std::memory_order_relaxed);
    state.base_bitrate_kbps = base_bitrate_kbps.load(std::memory_order_relaxed);
    state.target_bitrate_kbps = state.enabled ? target_bitrate_kbps.load(std::memory_order_relaxed) : 0;
    state.min_bitrate_kbps = current_config.min_bitrate_kbps;
    state.max_bitrate_kbps = current_config.max_bitrate_kbps;
    state.ewma_packet_loss = ewma_packet_loss;
    state.ewma_rtt_ms = ewma_rtt;
    state.state = state.enabled ? controller_state : "disabled";
    state.reason = state.enabled ? controller_reason : "disabled";
    return state;
  }

  void set_base_bitrate(int kbps) {
    const int clamped = std::clamp(kbps, current_config.min_bitrate_kbps, current_config.max_bitrate_kbps);
    base_bitrate_kbps.store(clamped, std::memory_order_relaxed);

    // Initialize target to base when first set
    int expected = 0;
    if (!target_bitrate_kbps.compare_exchange_strong(expected, clamped, std::memory_order_relaxed)) {
      int current = target_bitrate_kbps.load(std::memory_order_relaxed);
      target_bitrate_kbps.store(clamp_target(current, clamped), std::memory_order_relaxed);
    }
  }

  void set_enabled(bool enable) {
    config::video.adaptive_bitrate.enabled = enable;
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
    normalize_config_bounds(current_config);
    config::video.adaptive_bitrate.max_bitrate_kbps = current_config.max_bitrate_kbps;

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
    set_controller_status(enabled.load(std::memory_order_relaxed) ? "steady" : "disabled",
                          enabled.load(std::memory_order_relaxed) ? "reset" : "disabled");

    base_bitrate_kbps.store(0, std::memory_order_relaxed);
    target_bitrate_kbps.store(0, std::memory_order_relaxed);
  }

}  // namespace adaptive_bitrate
