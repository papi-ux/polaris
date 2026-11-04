/**
 * @file src/adaptive_bitrate.h
 * @brief Live Tuning's bitrate actuator: the policy's target, Doctor's transactions and the encoder's proof.
 *
 * live_tuning_policy.h decides the bitrate from client media reports and the control channel's RTT. This
 * controller owns everything around that decision: the client's request and the floor, explicit and Doctor
 * writers, revisions that keep a stale writer from winning, and what the encoder actually applied.
 */
#pragma once

#include <functional>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include "stream_bitrate.h"
#include "live_tuning_fec.h"
#include "vrr_trace.h"
#include "native_video_signal.h"
#include "live_tuning_native.h"

namespace stream_native_reception { class scope_t; }

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
    bool native_downward = false;     // Separate experimental opt-in; preserve aggregate field order.
  };

  /// Live Tuning history keeps at most this many entries per stream scope.
  inline constexpr std::size_t HISTORY_CAPACITY = 64;

  /** Nominal actuator allocation state around a real operation, not wire delivery or FEC benefit. */
  struct fec_transition_t {
    int configured_percentage = 0;
    int percentage_before = 0;
    int percentage_after = 0;
    std::string phase_before;
    std::string phase_after;
    std::optional<int> pending_percentage_before;
    std::optional<int> pending_percentage_after;
    /// Present only for an actual raise publication at its accepted positive next-frame boundary.
    std::optional<std::int64_t> from_frame_index;
  };

  /**
   * One observed controller or encoder transition, recorded under the controller's own lock.
   *
   * Times are host steady-clock milliseconds since the history's scope began. They are never client,
   * media or wall-clock times, and no entry says which bitrate any client media interval was encoded
   * at. kind is an open diagnostic string. Supported events include:
   * - decision: the requested target, controller state or reason changed;
   * - encoder_applied: the encoder backend reported it applied the exact current request;
   * - encoder_update_not_applied: the backend was asked to apply the exact current request and did not;
   * - stale_encoder_request: an encoder callback named a request that was no longer current, so the
   *   backend was not asked;
   * - applied_proof_invalidated: the encoder is being recreated, so no applied rate is proven;
   * - recreation_superseded: a recreation was refused because a newer request won;
   * - encoder_acknowledged: the encoder confirmed it runs at the exact current request without a
   *   backend update in that call (for example after recreation);
   * - stale_acknowledgement: a confirmation for a request that was no longer current was ignored;
   * - revision_reaffirmed: a newer controller revision adopted the already applied rate; nothing
   *   reached the encoder backend;
   * - encoder_initialized: a new encoder reported its initial rate;
   * - applied_unknown: live updates stopped or the controller reset, so no applied rate is proven;
   * - scope_started: this history began for its stream scope. It is a snapshot of the controller as the
   *   scope found it; nothing recorded before the scope began is kept or attributed to it;
   * - fec_target, fec_published: an allocation request, or its actual frame-boundary publication;
   * - fec_retired, fec_cancelled: an actual percentage retirement, or cancellation without one;
   * - fec_restored: the encoder confirmed the higher rate after F0 was already published;
   * - fec_refused, fec_deferred, fec_unavailable: existing transaction/evidence diagnostics.
   * Typed fec_transition evidence, when present, distinguishes allocation changes from requests and
   * confirmations. Absence means unavailable, never a configured or published percentage of zero.
   */
  struct history_entry_t {
    std::uint64_t sequence = 0;
    std::int64_t host_elapsed_ms = 0;
    /// Host steady-clock time of the latest repeat folded into this entry.
    std::int64_t last_host_elapsed_ms = 0;
    /// Identical consecutive observations are folded into one entry and counted here.
    std::uint64_t repeat_count = 1;
    std::string kind;
    std::string cause;
    std::string state;
    std::string reason;
    /// The controller's requested encoder target after the event.
    int requested_bitrate_kbps = 0;
    /// The encoder-confirmed rate after the event, meaningful only when applied_known.
    int applied_bitrate_kbps = 0;
    bool applied_known = false;
    std::uint64_t operator_revision = 0;
    std::uint64_t applied_revision = 0;
    /// The request an encoder callback named, for encoder entries; zero otherwise.
    int callback_bitrate_kbps = 0;
    std::uint64_t callback_revision = 0;
    bool feedback_initialized = false;
    double ewma_packet_loss = 0.0;
    double ewma_rtt_ms = 0.0;
    /// The newest network feedback sample the controller had received, when it had one.
    bool input_available = false;
    double input_packet_loss_percent = 0.0;
    double input_rtt_ms = 0.0;
    std::optional<fec_transition_t> fec_transition;
    /// Experimental native evidence; separate from RTT/loss and never a restore capability.
    std::optional<live_tuning_native::evidence_t> native_transition;
  };

  /** A bounded, oldest-first copy of one stream scope's history. */
  struct history_t {
    struct trace_scope_t {
      // Opaque host observation identity, independent of the private app/session bearer.
      std::uint64_t id = 0;
      std::optional<std::int64_t> origin_steady_ns;
      std::optional<std::uint64_t> first_sequence;
    };
    /// The stream scope these entries belong to. A history that never had one, or whose stream ended,
    /// is unscoped and must not be attributed to any stream.
    bool scoped = false;
    std::uint64_t session_generation = 0;
    /// Used only to match a stats snapshot's scope; never exported.
    std::string app_session_id;
    std::size_t capacity = HISTORY_CAPACITY;
    /// Entries this scope recorded that no longer fit.
    std::uint64_t dropped = 0;
    /// Host steady-clock milliseconds since the scope began, when this copy was taken.
    std::int64_t host_elapsed_ms_at_copy = 0;
    std::vector<history_entry_t> entries;
    std::optional<trace_scope_t> trace_scope;
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
    /// Diagnostic averages of the media reports' loss and the RTT readings. No decision reads them.
    double ewma_packet_loss = 0.0;
    double ewma_rtt_ms = 0.0;
    /// The lowest rate the policy found lossy or queueing above its hold, or 0 when it knows none.
    int ceiling_bitrate_kbps = 0;
    /// True when that ceiling was found by a standing queue rather than by loss.
    bool ceiling_is_queue = false;
    /// Loss the policy learned does not depend on the bitrate, in percent of frames.
    double background_loss_pct = 0.0;
    std::string state = "disabled";
    std::string reason = "disabled";
    /// Filled only by get_state_with_history(), from the same locked snapshot as the fields above.
    history_t history;
    /// FEC's nominal allocation. Policy rates remain in the configured-FEC
    /// encoder units; target_bitrate_kbps is the separately actuated rate.
    bool fec_budget_available = false;
    bool runtime_bitrate_in_place = false;
    int policy_target_bitrate_kbps = 0;
    int configured_fec_percentage = 0;
    int current_fec_percentage = 0;
    std::string fec_phase = "fixed";
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
   * @brief Feed one reading of the control channel's ENet-smoothed RTT, about ten a second.
   *
   * Live Tuning's policy (live_tuning_policy.h) acts on a standing queue, never on a single high
   * reading: a Wi-Fi background scan lifts the reading for seconds and is not congestion.
   * @param rtt_ms ENet's smoothed round-trip time in milliseconds.
   */
  void update_rtt(double rtt_ms);

  /**
   * @brief Feed one client media report: the frames its interval expected and the frames it lost.
   *
   * Lost frames are frames FEC could not save. The policy counts reports, not wall time, so a pause in
   * reports freezes its decisions.
   * @param frames_expected Frames the interval expected.
   * @param frames_lost Frames that never arrived whole.
   * @param rtt_ms The host's RTT reading when the report arrived.
   */
  void update_media_report(double frames_expected, double frames_lost, double rtt_ms);

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

  /**
   * @brief get_state() plus a copy of the current Live Tuning history, taken under the same lock.
   *
   * The history is observational: recording it never changes a controller decision, and it does not
   * say which bitrate any client media interval was encoded at.
   */
  state_t get_state_with_history();

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

  /// Passive nonblocking native diagnostic inspection only; never a policy input or write capability.
  native_video_signal::context_t inspect_native_context(std::uint64_t expected_generation,
                                                       std::string_view expected_app_session) noexcept;

  /// Immediate callback only. No queued observation or read-only context is action authority.
  /// True means a current observation was consumed, not that a write occurred or is permitted later.
  bool update_native_signal(const std::shared_ptr<stream_native_reception::scope_t> &owner,
                            std::string_view app_session, std::string_view launch_nonce,
                            const native_video_signal::signal_t &signal, bool published,
                            const std::function<bool()> &current_caller);

  // Historical origin for an encoder about to open at this exact rate. Unlike
  // a live update, initialization precedes runtime-support publication. This
  // snapshot neither authorizes an SDK operation nor acknowledges application.
  std::optional<live_bitrate_request_t> get_encoder_open_request(
    std::uint64_t generation, int configured_encoder_kbps);

  // A read-only ceiling for an already submission-bound frame, never an
  // applied-setting receipt. Missing/contaminated/busy scopes keep legacy for
  // new frames; already admitted frames must freeze or tighten.
  struct live_pacing_cap_t {
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    int target_encoder_kbps = 0;
    // Nominal policy target in F0 units, before a trial's encoder allocation.
    // Without a trial the nominal and actual targets are the same.
    int policy_encoder_kbps = target_encoder_kbps;
  };
  std::optional<live_pacing_cap_t> get_live_pacing_cap(std::uint64_t generation);

  // The callback may only update the encoder; it must not call this controller.
  // Validate, apply and acknowledge under one boundary with operator changes.
  bool apply_live_bitrate_request(const live_bitrate_request_t &request,
                                  const std::function<bool()> &apply,
                                  vrr_trace::bitrate_observation_t *trace_observation = nullptr,
                                  std::int64_t next_frame_index = 0);
  void set_session_scope(std::uint64_t generation, const std::string &app_session_id,
                         bool exclusive);

  /// Trusted handshake arithmetic for the sole stream, frozen before its
  /// encoder starts. Watchers, absent requests and unsupported clients stay fixed.
  void configure_fec_budget(std::uint64_t generation,
                            const stream_bitrate::request_t &request,
                            bool client_advertised_fec);
  /// Only an accepted selective block report with a positive shard-rank deficit
  /// calls this. It is neither a whole-stream loss rate nor proof of recovery.
  void note_fec_partial_deficit(std::uint64_t generation,
                               std::chrono::steady_clock::time_point received_at);
  /// Packetizer-only: one coherent immutable publication, scoped to this frame.
  live_tuning_fec::selection_t frame_fec_selection(std::uint64_t generation, std::int64_t frame_index,
                                                 int configured_fallback);
  /// Allocation-only compatibility wrapper over the same selection operation.
  std::pair<int, int> frame_fec_allocation(std::uint64_t generation, std::int64_t frame_index,
                                          int configured_fallback);

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
    int bitrate_kbps,
    std::int64_t next_frame_index = 0
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
    int initial_encoder_bitrate_kbps = 0,
    bool in_place = false,
    int in_place_floor_kbps = 1,
    std::weak_ptr<const video::bitrate_provenance::current_setting_t> current_setting = {}
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
