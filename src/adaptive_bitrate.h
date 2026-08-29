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
#include <cstdint>
#include <optional>
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
    bool active = false;
    bool runtime_update_supported = false;
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
   * @brief Exact operator-owned controller state used by a reversible Doctor
   *        transaction.
   *
   * revision changes for explicit controller/configuration writers, but not
   * for ordinary telemetry-driven adaptive adjustments. This lets Doctor
   * restore its own change without overwriting a newer user or client choice.
   */
  struct doctor_state_t {
    bool enabled = false;
    bool runtime_update_supported = false;
    int base_bitrate_kbps = 0;
    int live_bitrate_kbps = 0;
    int min_bitrate_kbps = 0;
    int max_bitrate_kbps = 0;
    std::uint64_t revision = 0;
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

  /** Return a coherent snapshot for a conditional Doctor transaction. */
  doctor_state_t get_doctor_state();

  /**
   * Atomically apply a Doctor-owned bitrate target when no explicit writer
   * has changed the controller since expected_revision.
   *
   * max_bitrate_kbps is a temporary in-memory ceiling for this stream only.
   * The returned revision owns the mutation and must be supplied to advance
   * or restore it.
   */
  std::optional<std::uint64_t> set_doctor_bitrate_if_revision(
    std::uint64_t expected_revision,
    int target_bitrate_kbps,
    std::optional<int> max_bitrate_kbps = std::nullopt
  );

  /** Restore an exact pre-action snapshot only while Doctor still owns it. */
  bool restore_doctor_state_if_revision(
    std::uint64_t expected_revision,
    const doctor_state_t &previous_state
  );

  /**
   * @brief Set the base bitrate from client request.
   * @param kbps Base bitrate in kilobits per second.
   */
  void set_base_bitrate(int kbps);

  /**
   * @brief Set both the live target and its base immediately.
   *
   * Unlike set_base_bitrate(), this is an explicit operator action and does
   * not preserve a previously reduced target. The value is still clamped to
   * the configured adaptive bitrate bounds.
   */
  void set_live_bitrate(int kbps);

  /**
   * @brief Change the in-memory adaptive bitrate ceiling for this session.
   *
   * Doctor uses this for a temporary same-stream quality retry. Verification
   * or session teardown restores the target and the pre-action clamp.
   */
  void set_max_bitrate(int kbps);

  /**
   * @brief Enable or disable adaptive bitrate control.
   * @param enabled True to enable, false to disable.
   */
  void set_enabled(bool enabled);

  /**
   * @brief Change adaptive bitrate only for the current in-memory controller.
   *
   * Unlike set_enabled(), this never changes the saved configuration value.
   * Doctor uses it for a reversible same-stream action so a temporary enable
   * cannot become policy for a later stream generation.
   */
  void set_runtime_enabled(bool enabled);

  /**
   * @brief Check if adaptive bitrate control is enabled.
   * @return True if enabled.
   */
  bool is_enabled();

  /**
   * @brief Report whether the active encoder can apply bitrate changes live.
   *
   * The controller remains configured when this is false, but it must not
   * consume telemetry or publish an encoder target that was never applied.
   */
  void set_runtime_update_supported(bool supported, const std::string &reason = {});

  /**
   * @brief Check whether adaptive bitrate is configured and usable live.
   */
  bool is_active();

  /**
   * @brief Load configuration from polaris config system.
   */
  void load_config();

  /**
   * @brief Reset all state (call when a new stream session starts).
   */
  void reset();

}  // namespace adaptive_bitrate
