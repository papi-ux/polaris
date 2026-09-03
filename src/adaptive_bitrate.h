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
   * revision changes for explicit controller/configuration writers,
   * autonomous target movement, and host network-evidence arrival before a
   * Doctor transaction is acquired. While Doctor owns a transaction,
   * telemetry is observational and cannot move its fixed target. This lets
   * Doctor restore its own change without overwriting a newer user, client,
   * controller, or evidence decision.
   */
  struct doctor_state_t {
    bool enabled = false;
    bool explicit_live_override_active = false;
    bool runtime_update_supported = false;
    int base_bitrate_kbps = 0;
    int live_bitrate_kbps = 0;
    int min_bitrate_kbps = 0;
    int max_bitrate_kbps = 0;
    // Changes only when a user/client/controller decision changes the live
    // bitrate authority. Host telemetry may rotate revision without making a
    // still-identical Doctor button impossible to press.
    std::uint64_t action_authority_revision = 0;
    std::uint64_t revision = 0;
  };

  enum class doctor_bitrate_apply_status_e {
    applied,
    controller_changed,
    quality_policy_blocked,
  };

  struct doctor_bitrate_apply_result_t {
    doctor_bitrate_apply_status_e status =
      doctor_bitrate_apply_status_e::controller_changed;
    std::uint64_t revision = 0;
  };

  /** One encoder-visible bitrate request owned by a controller revision. */
  struct live_bitrate_request_t {
    int target_bitrate_kbps = 0;
    std::uint64_t revision = 0;
  };

  /**
   * @brief Feed network statistics from client loss reports.
   * @param packet_loss_percent Packet loss percentage (0-100).
   * @param rtt_ms Round-trip time in milliseconds.
   */
  void update_network_stats(double packet_loss_percent, double rtt_ms);

  /**
   * @brief Linearize a newly received host network observation with Doctor.
   *
   * Call this while serializing the observation and before publishing its
   * fields. It invalidates a not-yet-applied Doctor controller snapshot even
   * when adaptive feedback is disabled or its adjustment interval has not
   * elapsed. Evidence remains observational once Doctor or its rollback owns
   * the actuator.
   */
  void note_network_evidence_arrival(bool suppresses_quality_restore);

  /**
   * @brief Linearize a transition in host video evidence that can suppress a
   *        deterministic quality restore.
   *
   * Repeated samples in the same policy/evidence class do not rotate action authority.
   * This keeps a stable action usable between status publication and a human
   * click while still rejecting a stale restore after clean evidence becomes
   * an encoder or pacing warning. A nonblocking path observation (such as a
   * healthy SHM compatibility path) or provisional pacing warning rotates the
   * evidence epoch without blocking restore. The class is reset for every stream.
   */
  void note_doctor_video_policy_evidence(
    bool suppresses_quality_restore,
    bool nonblocking_video_observation = false
  );

  /**
   * @brief Whether current or latched post-change video evidence blocks a
   *        guarded quality-restoration step.
   *
   * A warning observed while Doctor owns the actuator remains latched until
   * that reversible transaction ends, even if a later sample looks clean.
   */
  bool doctor_policy_blocks_quality_restore();

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
   * @return Target bitrate in kbps, or 0 if no adaptive, Doctor, or explicit
   *         live target currently owns the runtime encoder actuator.
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
   * Applying the target does not enable adaptive feedback; the returned
   * revision owns one fixed mutation and must be supplied to advance or
   * restore it.
   */
  std::optional<std::uint64_t> set_doctor_bitrate_if_revision(
    std::uint64_t expected_revision,
    int target_bitrate_kbps,
    std::optional<int> max_bitrate_kbps = std::nullopt
  );

  /**
   * Atomically apply one guarded quality-restoration step only if the
   * controller revision and Doctor network/video/capture policy remain
   * eligible.
   *
   * The distinct quality_policy_blocked result lets the caller roll back the
   * Doctor-owned transaction instead of treating fresh warning evidence as
   * an unrelated controller supersession.
   */
  doctor_bitrate_apply_result_t set_doctor_quality_bitrate_if_revision(
    std::uint64_t expected_revision,
    int target_bitrate_kbps,
    std::optional<int> max_bitrate_kbps = std::nullopt
  );

  /**
   * Restore an exact pre-action snapshot only while Doctor still owns it.
   *
   * The returned revision remains encoder-visible as a one-shot update even
   * when the restored adaptive policy is disabled. Callers must confirm that
   * revision before reporting rollback complete.
   */
  std::optional<std::uint64_t> restore_doctor_state_if_revision(
    std::uint64_t expected_revision,
    const doctor_state_t &previous_state
  );

  /** Return the current encoder request, including one-shot rollback work. */
  std::optional<live_bitrate_request_t> get_live_bitrate_request();

  /**
   * Invalidate the retiring encoder session before recreating it for an exact
   * live request. Returns false when the request was superseded before the
   * encoder reached the recreation boundary.
   */
  bool begin_live_bitrate_session_recreation(
    std::uint64_t revision,
    int bitrate_kbps
  );

  /** Record a successful encoder application of an exact controller request. */
  void acknowledge_live_bitrate_applied(
    std::uint64_t revision,
    int bitrate_kbps
  );

  /** Host-monotonic application time for an exact target/revision, if known. */
  std::optional<std::chrono::steady_clock::time_point> live_bitrate_applied_at(
    std::uint64_t revision,
    int bitrate_kbps
  );

  /** Wait for an exact encoder application without changing controller state. */
  bool wait_for_live_bitrate_applied(
    std::uint64_t revision,
    int bitrate_kbps,
    std::chrono::milliseconds timeout
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
  void set_runtime_update_supported(
    bool supported,
    const std::string &reason = {},
    int initial_encoder_bitrate_kbps = 0
  );

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
