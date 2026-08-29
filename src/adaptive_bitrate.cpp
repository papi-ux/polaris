/**
 * @file src/adaptive_bitrate.cpp
 * @brief Adaptive bitrate controller using EWMA-based network feedback.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
  static std::condition_variable state_changed;

  static config_t current_config;
  static std::atomic<bool> enabled {false};
  static std::atomic<bool> runtime_update_supported {false};
  static std::atomic<int> base_bitrate_kbps {0};
  static std::atomic<int> target_bitrate_kbps {0};
  // A Doctor Auto Fix owns one fixed live target until it is undone,
  // superseded, or the stream generation ends. It must not implicitly enable
  // the adaptive feedback controller or permit telemetry to make additional
  // bitrate mutations during that reversible transaction.
  static std::atomic<bool> doctor_override_active {false};
  // A direct paired-client bitrate write must also reach the live encoder when
  // adaptive feedback is disabled. Unlike Doctor, this is not a reversible
  // receipt; it remains the explicit live target until another controller
  // writer or the next stream generation replaces it.
  static std::atomic<bool> explicit_live_override_active {false};
  // A rollback to an otherwise inactive policy still has to reach the live
  // encoder once. The encoder acknowledgement clears this flag without
  // changing the restored policy.
  static std::atomic<bool> pending_live_update_active {false};
  // Doctor may install a temporary ceiling while restoring quality in guarded
  // steps. Keep the pre-transaction ceiling inside the actuator so any newer
  // explicit writer can retire Doctor completely before it clamps its value.
  // Protected by state_mutex.
  static int doctor_previous_max_bitrate_kbps = 0;
  // Protected by state_mutex. Every target writer, autonomous telemetry
  // decision, host network-evidence arrival, and host video-policy transition
  // before Doctor acquisition bumps this value. Doctor snapshots it and
  // performs an exact compare-and-set so a newer controller or evidence
  // decision cannot be overwritten by a stale quality increase.
  static std::uint64_t operator_revision = 1;
  // The encoder thread is the only authority for what was actually applied.
  // Protected by state_mutex.
  static int encoder_applied_bitrate_kbps = 0;
  static std::uint64_t encoder_applied_revision = 0;
  static std::chrono::steady_clock::time_point encoder_applied_at {};
  // -1 is unknown for a new stream, 0 allows quality restoration, and 1 is a
  // host video warning that suppresses it. Protected by state_mutex.
  static int doctor_video_policy_class = -1;

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
  static std::string runtime_update_reason = "encoder_not_initialized";

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

  static void retire_doctor_override_locked() {
    if (doctor_override_active.load(std::memory_order_relaxed) &&
        doctor_previous_max_bitrate_kbps > 0) {
      current_config.max_bitrate_kbps = std::max(
        doctor_previous_max_bitrate_kbps,
        current_config.min_bitrate_kbps
      );
    }
    doctor_override_active.store(false, std::memory_order_relaxed);
    doctor_previous_max_bitrate_kbps = 0;
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

  void note_network_evidence_arrival() {
    std::lock_guard<std::mutex> lock(state_mutex);
    // Post-change telemetry verifies the fixed Doctor target; it must not
    // supersede the transaction or its one-shot encoder rollback. Before
    // ownership begins, however, every host-received network observation
    // invalidates a controller snapshot so the initial mutation and evidence
    // publication share one monotonic transaction epoch.
    if (doctor_override_active.load(std::memory_order_relaxed) ||
        pending_live_update_active.load(std::memory_order_relaxed)) {
      return;
    }
    ++operator_revision;
    state_changed.notify_all();
  }

  void note_doctor_video_policy_evidence(bool suppresses_quality_restore) {
    std::lock_guard<std::mutex> lock(state_mutex);
    const int policy_class = suppresses_quality_restore ? 1 : 0;
    if (doctor_video_policy_class == policy_class) {
      return;
    }
    doctor_video_policy_class = policy_class;

    // Video samples collected during a Doctor-owned change are verification
    // evidence, not a competing controller writer. The next generation resets
    // this class before it can authorize another action.
    if (doctor_override_active.load(std::memory_order_relaxed) ||
        pending_live_update_active.load(std::memory_order_relaxed)) {
      return;
    }
    ++operator_revision;
    state_changed.notify_all();
  }

  void update_network_stats(double packet_loss_percent, double rtt_ms) {
    if (!enabled.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    if (doctor_override_active.load(std::memory_order_relaxed)) {
      return;
    }
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

    const bool network_pressure = ewma_packet_loss > 1.0 || rtt_spike;
    if (network_pressure) {
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

    const int clamped_target = clamp_target(new_target, base);
    if (clamped_target != current_target) {
      target_bitrate_kbps.store(clamped_target, std::memory_order_relaxed);
    }
    // Keep direct controller callers safe at the floor too. Production network
    // observations already advance the common evidence/controller epoch before
    // this callback; this additional bump records a controller pressure
    // decision that could not move the clamped target.
    if (clamped_target != current_target || network_pressure) {
      ++operator_revision;
      state_changed.notify_all();
    }
  }

  void update_stream_health(double fps_ratio,
                            double dropped_frame_ratio,
                            double duplicate_frame_ratio,
                            double frame_jitter_ms,
                            double encode_time_ms,
                            double avg_frame_age_ms) {
    if (!enabled.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    if (doctor_override_active.load(std::memory_order_relaxed)) {
      return;
    }
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
    // Encoder pressure is still a newer controller decision at the bitrate
    // floor. Advance the revision so Doctor cannot raise quality from a clean
    // snapshot taken before this sample was published.
    ++operator_revision;
    state_changed.notify_all();
  }

  int get_target_bitrate_kbps() {
    if (!is_active()) {
      return 0;
    }
    return target_bitrate_kbps.load(std::memory_order_relaxed);
  }

  state_t get_state() {
    std::lock_guard<std::mutex> lock(state_mutex);
    state_t state;
    state.enabled = enabled.load(std::memory_order_relaxed);
    state.runtime_update_supported = runtime_update_supported.load(std::memory_order_relaxed);
    const bool doctor_override = doctor_override_active.load(std::memory_order_relaxed);
    const bool explicit_live_override =
      explicit_live_override_active.load(std::memory_order_relaxed);
    const bool rollback_pending =
      pending_live_update_active.load(std::memory_order_relaxed);
    state.active = (state.enabled || doctor_override || explicit_live_override || rollback_pending) &&
      state.runtime_update_supported;
    state.base_bitrate_kbps = base_bitrate_kbps.load(std::memory_order_relaxed);
    state.target_bitrate_kbps = state.active ? target_bitrate_kbps.load(std::memory_order_relaxed) : 0;
    state.min_bitrate_kbps = current_config.min_bitrate_kbps;
    state.max_bitrate_kbps = current_config.max_bitrate_kbps;
    state.ewma_packet_loss = ewma_packet_loss;
    state.ewma_rtt_ms = ewma_rtt;
    state.state = doctor_override && state.active ? "doctor_override" :
      explicit_live_override && state.active ? "explicit_live_target" :
      rollback_pending && state.active ? "rollback_pending" :
      !state.enabled ? "disabled" : state.active ? controller_state : "unavailable";
    state.reason = doctor_override && state.active ? "doctor_action" :
      explicit_live_override && state.active ? "paired_client_action" :
      rollback_pending && state.active ? "doctor_rollback" :
      !state.enabled ? "disabled" : state.active ? controller_reason : runtime_update_reason;
    return state;
  }

  doctor_state_t get_doctor_state() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return {
      enabled.load(std::memory_order_relaxed),
      explicit_live_override_active.load(std::memory_order_relaxed),
      runtime_update_supported.load(std::memory_order_relaxed),
      base_bitrate_kbps.load(std::memory_order_relaxed),
      target_bitrate_kbps.load(std::memory_order_relaxed),
      current_config.min_bitrate_kbps,
      current_config.max_bitrate_kbps,
      operator_revision,
    };
  }

  std::optional<std::uint64_t> set_doctor_bitrate_if_revision(
      std::uint64_t expected_revision,
      int target,
      std::optional<int> temporary_max_bitrate_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (operator_revision != expected_revision ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
      return std::nullopt;
    }

    // Production encoder startup registers the launch bitrate explicitly.
    // Preserve a coherent baseline for embedders/tests that enabled runtime
    // support after constructing an already-running controller.
    if (encoder_applied_bitrate_kbps <= 0) {
      const int prior_target = target_bitrate_kbps.load(std::memory_order_relaxed);
      if (prior_target > 0) {
        encoder_applied_bitrate_kbps = prior_target;
        encoder_applied_revision = operator_revision;
        encoder_applied_at = std::chrono::steady_clock::now();
      }
    }

    if (!doctor_override_active.load(std::memory_order_relaxed)) {
      doctor_previous_max_bitrate_kbps = current_config.max_bitrate_kbps;
    }
    if (temporary_max_bitrate_kbps) {
      current_config.max_bitrate_kbps = std::max(
        *temporary_max_bitrate_kbps,
        current_config.min_bitrate_kbps
      );
    }
    const int clamped = std::clamp(
      target,
      current_config.min_bitrate_kbps,
      current_config.max_bitrate_kbps
    );
    base_bitrate_kbps.store(clamped, std::memory_order_relaxed);
    target_bitrate_kbps.store(clamped, std::memory_order_relaxed);
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    doctor_override_active.store(true, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);
    set_controller_status("steady", "doctor_action");
    const auto revision = ++operator_revision;
    state_changed.notify_all();
    return revision;
  }

  std::optional<std::uint64_t> restore_doctor_state_if_revision(
      std::uint64_t expected_revision,
      const doctor_state_t &previous_state) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (operator_revision != expected_revision) {
      return std::nullopt;
    }

    retire_doctor_override_locked();
    explicit_live_override_active.store(
      previous_state.explicit_live_override_active,
      std::memory_order_relaxed
    );
    current_config.max_bitrate_kbps = std::max(
      previous_state.max_bitrate_kbps,
      current_config.min_bitrate_kbps
    );
    const int restored_base = previous_state.base_bitrate_kbps > 0 ?
      std::clamp(
        previous_state.base_bitrate_kbps,
        current_config.min_bitrate_kbps,
        current_config.max_bitrate_kbps
      ) : 0;
    const int restored_live = previous_state.live_bitrate_kbps > 0 ?
      std::clamp(
        previous_state.live_bitrate_kbps,
        current_config.min_bitrate_kbps,
        current_config.max_bitrate_kbps
      ) : 0;
    base_bitrate_kbps.store(restored_base, std::memory_order_relaxed);
    const int restored_target = restored_base > 0 ? std::min(restored_live, restored_base) : 0;
    target_bitrate_kbps.store(restored_target, std::memory_order_relaxed);
    enabled.store(previous_state.enabled, std::memory_order_relaxed);
    set_controller_status(
      previous_state.enabled ? "steady" : "disabled",
      "doctor_rollback"
    );
    const auto revision = ++operator_revision;
    const bool already_applied = restored_target > 0 &&
      encoder_applied_bitrate_kbps == restored_target;
    pending_live_update_active.store(
      restored_target > 0 && !already_applied,
      std::memory_order_relaxed
    );
    if (already_applied) {
      encoder_applied_revision = revision;
      encoder_applied_at = std::chrono::steady_clock::now();
    }
    state_changed.notify_all();
    return revision;
  }

  std::optional<live_bitrate_request_t> get_live_bitrate_request() {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!runtime_update_supported.load(std::memory_order_relaxed)) {
      return std::nullopt;
    }
    const bool actuator_active =
      enabled.load(std::memory_order_relaxed) ||
      doctor_override_active.load(std::memory_order_relaxed) ||
      explicit_live_override_active.load(std::memory_order_relaxed) ||
      pending_live_update_active.load(std::memory_order_relaxed);
    const int target = target_bitrate_kbps.load(std::memory_order_relaxed);
    if (!actuator_active || target <= 0) {
      return std::nullopt;
    }
    if (encoder_applied_bitrate_kbps == target &&
        encoder_applied_revision != operator_revision) {
      encoder_applied_revision = operator_revision;
      encoder_applied_at = std::chrono::steady_clock::now();
      pending_live_update_active.store(false, std::memory_order_relaxed);
      state_changed.notify_all();
    }
    return live_bitrate_request_t {target, operator_revision};
  }

  void acknowledge_live_bitrate_applied(
      std::uint64_t revision,
      int bitrate_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (revision != operator_revision ||
        bitrate_kbps != target_bitrate_kbps.load(std::memory_order_relaxed)) {
      return;
    }
    encoder_applied_bitrate_kbps = bitrate_kbps;
    encoder_applied_revision = revision;
    encoder_applied_at = std::chrono::steady_clock::now();
    pending_live_update_active.store(false, std::memory_order_relaxed);
    state_changed.notify_all();
  }

  std::optional<std::chrono::steady_clock::time_point> live_bitrate_applied_at(
      std::uint64_t revision,
      int bitrate_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (encoder_applied_revision != revision ||
        encoder_applied_bitrate_kbps != bitrate_kbps) {
      return std::nullopt;
    }
    return encoder_applied_at;
  }

  bool wait_for_live_bitrate_applied(
      std::uint64_t revision,
      int bitrate_kbps,
      std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(state_mutex);
    return state_changed.wait_for(lock, timeout, [&] {
      return (encoder_applied_revision == revision &&
              encoder_applied_bitrate_kbps == bitrate_kbps) ||
        operator_revision != revision ||
        !runtime_update_supported.load(std::memory_order_relaxed);
    }) && encoder_applied_revision == revision &&
      encoder_applied_bitrate_kbps == bitrate_kbps;
  }

  void set_base_bitrate(int kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    retire_doctor_override_locked();
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);
    const int clamped = std::clamp(kbps, current_config.min_bitrate_kbps, current_config.max_bitrate_kbps);
    base_bitrate_kbps.store(clamped, std::memory_order_relaxed);

    // Initialize target to base when first set
    int expected = 0;
    if (!target_bitrate_kbps.compare_exchange_strong(expected, clamped, std::memory_order_relaxed)) {
      int current = target_bitrate_kbps.load(std::memory_order_relaxed);
      target_bitrate_kbps.store(clamp_target(current, clamped), std::memory_order_relaxed);
    }
    ++operator_revision;
    state_changed.notify_all();
  }

  void set_live_bitrate(int kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    retire_doctor_override_locked();
    explicit_live_override_active.store(
      !enabled.load(std::memory_order_relaxed),
      std::memory_order_relaxed
    );
    const int clamped = std::clamp(kbps, current_config.min_bitrate_kbps, current_config.max_bitrate_kbps);
    base_bitrate_kbps.store(clamped, std::memory_order_relaxed);
    target_bitrate_kbps.store(clamped, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);
    set_controller_status("steady", "paired_client_action");
    ++operator_revision;
    state_changed.notify_all();
  }

  void set_max_bitrate(int kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    retire_doctor_override_locked();
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);
    current_config.max_bitrate_kbps = std::max(kbps, current_config.min_bitrate_kbps);

    const int base = std::min(
      base_bitrate_kbps.load(std::memory_order_relaxed),
      current_config.max_bitrate_kbps
    );
    const int target = std::min(
      target_bitrate_kbps.load(std::memory_order_relaxed),
      current_config.max_bitrate_kbps
    );
    base_bitrate_kbps.store(base, std::memory_order_relaxed);
    target_bitrate_kbps.store(std::min(target, base), std::memory_order_relaxed);
    ++operator_revision;
    state_changed.notify_all();
  }

  void set_enabled(bool enable) {
    std::lock_guard<std::mutex> lock(state_mutex);
    retire_doctor_override_locked();
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    config::video.adaptive_bitrate.enabled = enable;
    enabled.store(enable, std::memory_order_relaxed);
    const auto revision = ++operator_revision;
    const int target = target_bitrate_kbps.load(std::memory_order_relaxed);
    const bool needs_encoder_update = target > 0 && encoder_applied_bitrate_kbps != target;
    pending_live_update_active.store(needs_encoder_update, std::memory_order_relaxed);
    if (!needs_encoder_update && target > 0) {
      encoder_applied_revision = revision;
      encoder_applied_at = std::chrono::steady_clock::now();
    }
    state_changed.notify_all();
  }

  void set_runtime_enabled(bool enable) {
    std::lock_guard<std::mutex> lock(state_mutex);
    retire_doctor_override_locked();
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);
    enabled.store(enable, std::memory_order_relaxed);
    ++operator_revision;
    state_changed.notify_all();
  }

  bool is_enabled() {
    return enabled.load(std::memory_order_relaxed);
  }

  void set_runtime_update_supported(bool supported,
                                    const std::string &reason,
                                    int initial_encoder_bitrate_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    runtime_update_supported.store(supported, std::memory_order_relaxed);
    runtime_update_reason = supported ? "supported" :
      (reason.empty() ? "encoder_runtime_update_unsupported" : reason);
    if (supported) {
      if (initial_encoder_bitrate_kbps > 0) {
        encoder_applied_bitrate_kbps = initial_encoder_bitrate_kbps;
        encoder_applied_revision = operator_revision;
        encoder_applied_at = std::chrono::steady_clock::now();
      }
      set_controller_status("steady", "encoder_ready");
    }
    state_changed.notify_all();
  }

  bool is_active() {
    return (enabled.load(std::memory_order_relaxed) ||
            doctor_override_active.load(std::memory_order_relaxed) ||
            explicit_live_override_active.load(std::memory_order_relaxed) ||
            pending_live_update_active.load(std::memory_order_relaxed)) &&
      runtime_update_supported.load(std::memory_order_relaxed);
  }

  void load_config() {
    std::lock_guard<std::mutex> lock(state_mutex);
    doctor_override_active.store(false, std::memory_order_relaxed);
    doctor_previous_max_bitrate_kbps = 0;
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);

    current_config.enabled = config::video.adaptive_bitrate.enabled;
    current_config.min_bitrate_kbps = config::video.adaptive_bitrate.min_bitrate_kbps;
    current_config.max_bitrate_kbps = config::video.adaptive_bitrate.max_bitrate_kbps;
    normalize_config_bounds(current_config);
    config::video.adaptive_bitrate.max_bitrate_kbps = current_config.max_bitrate_kbps;

    enabled.store(current_config.enabled, std::memory_order_relaxed);
    ++operator_revision;
    state_changed.notify_all();

    BOOST_LOG(info) << "Adaptive bitrate: "
                    << (current_config.enabled ? "enabled" : "disabled")
                    << " (min=" << current_config.min_bitrate_kbps
                    << " kbps, max=" << current_config.max_bitrate_kbps << " kbps)";
  }

  void reset() {
    std::lock_guard<std::mutex> lock(state_mutex);
    doctor_override_active.store(false, std::memory_order_relaxed);
    doctor_previous_max_bitrate_kbps = 0;
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);

    ewma_packet_loss = 0.0;
    ewma_rtt = 0.0;
    avg_rtt = 0.0;
    rtt_sample_count = 0;
    initialized = false;
    runtime_update_supported.store(false, std::memory_order_relaxed);
    runtime_update_reason = "encoder_not_initialized";
    set_controller_status(enabled.load(std::memory_order_relaxed) ? "steady" : "disabled",
                          enabled.load(std::memory_order_relaxed) ? "reset" : "disabled");

    base_bitrate_kbps.store(0, std::memory_order_relaxed);
    target_bitrate_kbps.store(0, std::memory_order_relaxed);
    encoder_applied_bitrate_kbps = 0;
    encoder_applied_revision = 0;
    encoder_applied_at = {};
    doctor_video_policy_class = -1;
    ++operator_revision;
    state_changed.notify_all();
  }

}  // namespace adaptive_bitrate
