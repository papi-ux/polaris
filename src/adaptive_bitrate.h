/**
 * @file src/adaptive_bitrate.h
 * @brief Adaptive bitrate controller using EWMA-based network feedback.
 *
 * Dynamically adjusts encoding bitrate based on packet loss and RTT
 * statistics reported by the streaming client, similar to Parsec and
 * Steam Remote Play adaptive streaming.
 */
#pragma once

#include <functional>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace adaptive_bitrate {

  struct config_t {
    bool enabled = false;
    int min_bitrate_kbps = 2000;       // 2 Mbps floor
    // adaptive_bitrate_max. A session lifts it to the client's own request,
    // so it never cuts the bitrate a client asked for.
    int max_bitrate_kbps = 100000;
    double max_change_rate = 0.20;     // Max 20% change per adjustment
    double ewma_alpha = 0.3;           // EWMA smoothing factor (0-1, higher = more responsive)
    int adjustment_interval_ms = 1000; // How often to adjust
  };

  struct state_t {
    std::uint64_t session_generation = 0;
    std::string app_session_id;
    bool session_exclusive = false;
    bool configured_enabled = false;
    bool feedback_initialized = false;
    int applied_bitrate_kbps = 0;
    bool enabled = false;
    bool active = false;
    bool runtime_update_supported = false;
    int base_bitrate_kbps = 0;
    int target_bitrate_kbps = 0;
    int min_bitrate_kbps = 0;
    // The ceiling the controller holds: adaptive_bitrate_max, raised to the
    // stream's own request, so it never limits what a client asked for. Like
    // base_bitrate_kbps it keeps the last stream's value until the next starts.
    int max_bitrate_kbps = 0;
    /// What set min_bitrate_kbps: adaptive_bitrate_min, or pyrowave_advice for a PyroWave stream.
    std::string floor_source = "adaptive_bitrate_min";
    /// A manual live bitrate turned Live Tuning off for this stream; the saved preference stands.
    bool paused_for_stream = false;
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
   * How old the newest client media report may be before Live Tuning stops acting on the loss it
   * brought, as the host's network verdict stops judging it (judged_network_t::k_media_report_max_age_ms).
   */
  inline constexpr std::chrono::milliseconds k_media_report_max_age {5000};

  /**
   * @brief Feed one network reading to Live Tuning.
   *
   * Heavy loss, over 5%, and RTT spikes cut down to the floor. Moderate loss, 5% or less with no RTT
   * spike beside it, cuts no lower than half the stream's base until the rate is tested. A rate held
   * there for about 8 seconds is tested with the reports heard at it: steady loss, in most reports,
   * gets a step below, and the steps go on only while each lowers the loss, where one that does not
   * returns to the rate before it. Loss that comes and goes, or that a step below did not lower, is
   * not the bitrate's doing, so the rate holds and climbs back one step up at a time, each kept only if
   * the loss does not rise. Where a step below did lower it, the rate climbs back toward the rate that
   * lost frames the same way.
   *
   * Only a client media report acts on loss. A ping acts on an RTT spike, and while the newest report
   * is no older than k_media_report_max_age it does nothing else. Past that the loss is stale: it is no
   * pressure, its average decays with each ping as clean readings would pull it, and pings bring the
   * ordinary recovery.
   *
   * @param packet_loss_percent Video frame loss, 0 to 100, from one client media report, the report's
   *        own figure. std::nullopt for a reading that says nothing about video, such as a
   *        control-channel ping: while reports arrive it leaves the loss average where it is instead of
   *        pulling it toward zero.
   * @param rtt_ms Round-trip time in milliseconds.
   * @param loss_is_pressure Whether the host's network verdict calls loss pressure. Live Tuning hears a
   *        report's loss only while it does, so it acts when Doctor does, and its average falls within
   *        seconds of clean reports, where the verdict's 20 second window holds for most of its
   *        length. The report's own figure still judges the rate it arrived at.
   */
  void update_network_stats(std::optional<double> packet_loss_percent, double rtt_ms, bool loss_is_pressure = true);

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
   * @brief Feed local stream health so bitrate can react to encoder pressure.
   * High-refresh overruns additionally require a measured delivery shortfall;
   * pacing alone never lowers quality. An unknown target retains the legacy threshold.
   */
  void update_stream_health(double fps_ratio,
                            double dropped_frame_ratio,
                            double duplicate_frame_ratio,
                            double frame_jitter_ms,
                            double encode_time_ms,
                            double avg_frame_age_ms,
                            double target_fps = 0.0);

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

  // The callback may only update the encoder; it must not call this controller.
  // Validate, apply and acknowledge under one boundary with operator changes.
  bool apply_live_bitrate_request(const live_bitrate_request_t &request,
                                  const std::function<bool()> &apply);
  void set_session_scope(std::uint64_t generation, const std::string &app_session_id,
                         bool exclusive);

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
   *
   * The session's ceiling rises to at least kbps: adaptive_bitrate_max never
   * cuts the bitrate a client asked for. The adaptive floor still applies.
   * @param kbps Base bitrate in kilobits per second.
   */
  void set_base_bitrate(int kbps);

  /**
   * @brief Set both the live target and its base immediately.
   *
   * Unlike set_base_bitrate(), this is an explicit operator action and does
   * not preserve a previously reduced target. The host cap, max_bitrate, and
   * the adaptive floor bound it; adaptive_bitrate_max does not, and the
   * session's ceiling rises to the written value.
   */
  void set_live_bitrate(int kbps);

  /**
   * @brief Apply a player's own live bitrate and turn Live Tuning off for this stream only.
   *
   * set_live_bitrate(), with the controller's feedback turned off for the rest of the stream. Nothing
   * is saved: adaptive_bitrate_enabled keeps its value, and end_stream_override() or the next stream
   * puts the controller back to it.
   */
  void set_live_bitrate_for_stream(int kbps);

  /**
   * @brief Put back the saved Live Tuning preference after a stream a manual bitrate turned it off for.
   *
   * Does nothing when no manual bitrate did.
   */
  void end_stream_override();

  /**
   * @brief Raise the controller's floor for this stream, never above the stream's own request.
   *
   * PyroWave's picture falls apart well above adaptive_bitrate_min, so a PyroWave stream gets a floor
   * of its own, half what its model advises. The floor is never set above the stream's base, which is
   * the client's request: a client that asked for less than the codec's floor gets a stream Live
   * Tuning cannot cut at all. The next stream's load_config() puts adaptive_bitrate_min back.
   * @param kbps The floor at the encoder.
   * @param source What set it, reported as state_t::floor_source.
   */
  void set_session_floor(int kbps, std::string_view source);

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

#ifdef POLARIS_TESTS
  /** Move the controller's last adjustment and last pressure this much further into the past. */
  void age_for_tests(std::chrono::steady_clock::duration age);
#endif

}  // namespace adaptive_bitrate
