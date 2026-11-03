/**
 * @file src/adaptive_bitrate.cpp
 * @brief Live Tuning's bitrate actuator: the policy's target, Doctor's transactions and the encoder's proof.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>

// local includes
#include "adaptive_bitrate.h"
#include <cmath>
#include "config.h"
#include "live_tuning_policy.h"
#include "live_tuning_fec.h"
#include "logging.h"

using namespace std::literals;

namespace adaptive_bitrate {

  // Internal state protected by mutex for complex operations,
  // atomics for simple reads from the encoding thread.
  static std::mutex state_mutex;
  static std::condition_variable state_changed;

  static config_t current_config;
  static std::uint64_t scope_generation = 0;
  static std::string scope_app_session_id;
  static bool scope_exclusive = false;
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
  static std::uint64_t action_authority_revision = 1;
  // The encoder thread is the only authority for what was actually applied.
  // Protected by state_mutex.
  static int encoder_applied_bitrate_kbps = 0;
  static int last_confirmed_bitrate_kbps = 0;
  static std::uint64_t encoder_applied_revision = 0;
  static std::chrono::steady_clock::time_point encoder_applied_at {};
  // -1 is unknown for a new stream, 0 allows quality restoration, and 1 is a
  // host video warning that suppresses it. Protected by state_mutex.
  static int doctor_video_policy_class = -1;
  // -1 is unknown, 0 is clean, 1 is a nonblocking path observation or a
  // provisional pacing warning still inside its confirmation window, and 2
  // is policy-blocking evidence. This keeps a stale Doctor action from crossing
  // new capture/pacing evidence without treating one startup sample as a
  // confirmed fault. Protected by state_mutex.
  static int doctor_video_evidence_class = -1;
  static bool doctor_video_policy_regressed_during_override = false;
  // A post-change network regression is verification evidence, not a
  // competing controller writer. Keep it latched for the complete reversible
  // transaction so a later clean sample cannot authorize another quality
  // increase over an intervening bad observation. Protected by state_mutex.
  static bool doctor_network_policy_regressed_during_override = false;

  // Diagnostic averages of the media reports' loss and the RTT readings, for status and history. Live
  // Tuning's decisions come from the policy below, which never reads them.
  static double ewma_packet_loss = 0.0;
  static double ewma_rtt = 0.0;
  // The first media report starts the loss average.
  static bool loss_average_started = false;

  // This stream's loss and queue policy. Protected by state_mutex. Created when feedback first arrives
  // with a base bitrate, and dropped by reset().
  static std::optional<live_tuning_policy::policy_t> stream_policy;
  static live_tuning_fec::actuator_t fec_actuator;
  static bool runtime_bitrate_in_place = false;
  static int runtime_bitrate_floor_kbps = 1;
  // The policy does not adopt its own reduced encoder allocation as an
  // outside writer. This target always has the handshake's F0 units.
  static int policy_target_kbps = 0;
  static std::optional<std::chrono::steady_clock::time_point> fec_partial_deficit_at;
  static std::optional<std::chrono::steady_clock::time_point> fec_media_at;
  static std::optional<int> fec_waiting_percentage;
  static int fec_wait_reports = 0;
  static void retire_fec_for_writer_locked(const char *cause);

  // Timing for encoder-pressure adjustments
  static std::chrono::steady_clock::time_point last_adjustment_time;
  static std::chrono::steady_clock::time_point last_pressure_time;

  // Track whether we have received any stats yet
  static bool initialized = false;
  static std::string controller_state = "disabled";
  static std::string controller_reason = "disabled";
  static std::string runtime_update_reason = "encoder_not_initialized";
  // What set current_config.min_bitrate_kbps for this stream. Protected by state_mutex.
  static std::string floor_source = "adaptive_bitrate_min";
  // A manual live bitrate turned the controller's feedback off for this stream while the saved
  // preference, current_config.enabled, stayed on. Protected by state_mutex.
  static bool paused_for_stream = false;

  static void set_controller_status(const std::string &state, const std::string &reason) {
    controller_state = state;
    controller_reason = reason;
  }

  struct effective_status_t {
    bool enabled = false;
    bool active = false;
    std::string state;
    std::string reason;
  };

  // The state and reason get_state() reports. Requires state_mutex.
  static effective_status_t effective_status_locked() {
    effective_status_t status;
    status.enabled = enabled.load(std::memory_order_relaxed);
    const bool supported = runtime_update_supported.load(std::memory_order_relaxed);
    const bool doctor_override = doctor_override_active.load(std::memory_order_relaxed);
    const bool explicit_live_override =
      explicit_live_override_active.load(std::memory_order_relaxed);
    const bool rollback_pending =
      pending_live_update_active.load(std::memory_order_relaxed);
    status.active = (status.enabled || doctor_override || explicit_live_override || rollback_pending) &&
      supported;
    status.state = doctor_override && status.active ? "doctor_override" :
      explicit_live_override && status.active ? "explicit_live_target" :
      rollback_pending && status.active ? "rollback_pending" :
      !status.enabled ? "disabled" : status.active ? controller_state : "unavailable";
    status.reason = doctor_override && status.active ? "doctor_action" :
      explicit_live_override && status.active ? "paired_client_action" :
      rollback_pending && status.active ? "doctor_rollback" :
      !status.enabled ? "disabled" : status.active ? controller_reason : runtime_update_reason;
    return status;
  }

  // Live Tuning history: an observational record of target/reason transitions and encoder
  // acknowledgements, so a once-a-second reader cannot miss a short-lived one. Nothing here feeds
  // a controller decision. Protected by state_mutex.
  static history_t history;
  static std::uint64_t history_next_sequence = 1;
  static std::chrono::steady_clock::time_point history_started_at = std::chrono::steady_clock::now();
  struct decision_key_t {
    int requested_bitrate_kbps = 0;
    std::string state;
    std::string reason;
  };
  static std::optional<decision_key_t> last_history_decision;
  // The newest network feedback sample that reached the controller, for diagnosing a decision.
  static bool last_input_available = false;
  static double last_input_packet_loss_percent = 0.0;
  static double last_input_rtt_ms = 0.0;

  static std::int64_t history_elapsed_ms_locked(std::chrono::steady_clock::time_point now) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - history_started_at).count();
  }

  // Forget every entry. Requires state_mutex. The sequence keeps counting across scopes.
  static void clear_history_locked() {
    history.entries.clear();
    history.dropped = 0;
    history.scoped = false;
    history.session_generation = 0;
    history.app_session_id.clear();
    history.trace_scope.reset();
    history_started_at = std::chrono::steady_clock::now();
    last_history_decision.reset();
  }

  static bool folds_into(const history_entry_t &last, const history_entry_t &next) {
    if (last.fec_transition || next.fec_transition ||
        last.kind != next.kind || next.kind == "decision" || next.kind == "scope_started") {
      return false;
    }
    return last.cause == next.cause && last.state == next.state && last.reason == next.reason &&
      last.requested_bitrate_kbps == next.requested_bitrate_kbps &&
      last.applied_known == next.applied_known &&
      last.applied_bitrate_kbps == next.applied_bitrate_kbps &&
      last.callback_bitrate_kbps == next.callback_bitrate_kbps &&
      // A reaffirmation only moves the revision. Encoder callbacks for a newer revision stay apart.
      (next.kind == "revision_reaffirmed" || last.callback_revision == next.callback_revision);
  }

  // Append one entry from the current locked state. Requires state_mutex. The history is
  // observational, so a failure to record it never reaches the caller.
  static void append_history_locked(history_entry_t entry) noexcept {
    try {
      const auto elapsed = history_elapsed_ms_locked(std::chrono::steady_clock::now());
      auto status = effective_status_locked();
      entry.state = std::move(status.state);
      entry.reason = std::move(status.reason);
      entry.requested_bitrate_kbps = target_bitrate_kbps.load(std::memory_order_relaxed);
      entry.applied_known = encoder_applied_bitrate_kbps > 0;
      entry.applied_bitrate_kbps = entry.applied_known ? encoder_applied_bitrate_kbps : 0;
      entry.operator_revision = operator_revision;
      entry.applied_revision = encoder_applied_revision;
      entry.feedback_initialized = initialized;
      entry.ewma_packet_loss = ewma_packet_loss;
      entry.ewma_rtt_ms = ewma_rtt;
      entry.input_available = last_input_available;
      entry.input_packet_loss_percent = last_input_packet_loss_percent;
      entry.input_rtt_ms = last_input_rtt_ms;
      entry.host_elapsed_ms = elapsed;
      entry.last_host_elapsed_ms = elapsed;
      if (!history.entries.empty() && folds_into(history.entries.back(), entry)) {
        auto &last = history.entries.back();
        ++last.repeat_count;
        last.last_host_elapsed_ms = elapsed;
        last.operator_revision = entry.operator_revision;
        last.applied_revision = entry.applied_revision;
        last.callback_revision = entry.callback_revision;
        last.feedback_initialized = entry.feedback_initialized;
        last.ewma_packet_loss = entry.ewma_packet_loss;
        last.ewma_rtt_ms = entry.ewma_rtt_ms;
        last.input_available = entry.input_available;
        last.input_packet_loss_percent = entry.input_packet_loss_percent;
        last.input_rtt_ms = entry.input_rtt_ms;
        return;
      }
      entry.sequence = history_next_sequence++;
      if (history.entries.size() >= HISTORY_CAPACITY) {
        history.entries.erase(history.entries.begin());
        ++history.dropped;
      }
      history.entries.push_back(std::move(entry));
    } catch (...) {
    }
  }

  static void record_history_locked(const char *kind, const char *cause,
                                    int callback_bitrate_kbps = 0,
                                    std::uint64_t callback_revision = 0) noexcept {
    try {
      history_entry_t entry;
      entry.kind = kind;
      entry.cause = cause;
      entry.callback_bitrate_kbps = callback_bitrate_kbps;
      entry.callback_revision = callback_revision;
      append_history_locked(std::move(entry));
    } catch (...) {
    }
  }

  // Scalar-only snapshots are captured under state_mutex without allocating before an actuator operation.
  struct fec_snapshot_t {
    bool configured;
    int configured_percentage;
    int percentage;
    live_tuning_fec::actuator_t::phase_e phase;
    std::optional<int> pending_percentage;
  };

  static fec_snapshot_t fec_snapshot_locked() noexcept {
    return {fec_actuator.configured(), fec_actuator.configured_percentage(), fec_actuator.percentage(),
      fec_actuator.phase(), fec_actuator.pending_percentage()};
  }

  static const char *fec_phase_name(live_tuning_fec::actuator_t::phase_e phase) noexcept {
    using phase_e = live_tuning_fec::actuator_t::phase_e;
    return phase == phase_e::lowering_rate ? "lowering_rate" :
      phase == phase_e::raising_rate ? "raising_rate" : "fixed";
  }

  // All allocating conversion, including phase names, belongs inside this observational catch boundary.
  static void record_fec_transition_locked(const char *kind, const char *cause,
                                           const fec_snapshot_t &before, const fec_snapshot_t &after,
                                           int callback_bitrate_kbps = 0,
                                           std::uint64_t callback_revision = 0,
                                           std::optional<std::int64_t> from_frame_index = std::nullopt) noexcept {
    if (!before.configured || !after.configured) return;
    try {
      history_entry_t entry;
      entry.kind = kind;
      entry.cause = cause;
      entry.callback_bitrate_kbps = callback_bitrate_kbps;
      entry.callback_revision = callback_revision;
      entry.fec_transition = fec_transition_t {
        before.configured_percentage, before.percentage, after.percentage,
        fec_phase_name(before.phase), fec_phase_name(after.phase),
        before.pending_percentage, after.pending_percentage, from_frame_index
      };
      append_history_locked(std::move(entry));
    } catch (...) {
    }
  }

  // Record a decision when the requested target, state or reason differs from the last recorded
  // decision. Requires state_mutex.
  static void note_decision_locked(const char *cause) noexcept {
    try {
      auto status = effective_status_locked();
      const int requested = target_bitrate_kbps.load(std::memory_order_relaxed);
      if (last_history_decision && last_history_decision->requested_bitrate_kbps == requested &&
          last_history_decision->state == status.state && last_history_decision->reason == status.reason) {
        return;
      }
      last_history_decision = decision_key_t {requested, std::move(status.state), std::move(status.reason)};
      record_history_locked("decision", cause);
    } catch (...) {
    }
  }

  // Declared after a writer's lock_guard so it records the writer's outcome before the lock is released.
  struct decision_note_t {
    const char *cause;
    ~decision_note_t() {
      note_decision_locked(cause);
    }
  };

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
    doctor_video_policy_regressed_during_override = false;
    doctor_network_policy_regressed_during_override = false;
  }

  // adaptive_bitrate_max never cuts what a client asked for. Nothing in this
  // controller moves the target above its base, so raising the session's
  // ceiling to the client's request leaves every clamp below it (adaptive
  // feedback, Doctor and its Undo) working from the bitrate the encoder was
  // given, not from a smaller configured number.
  static void admit_client_bitrate_locked(int kbps) {
    current_config.max_bitrate_kbps = std::max(current_config.max_bitrate_kbps, kbps);
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

  void note_network_evidence_arrival(bool suppresses_quality_restore) {
    std::lock_guard<std::mutex> lock(state_mutex);
    // Post-change telemetry verifies the fixed Doctor target; it must not
    // supersede the transaction or its one-shot encoder rollback. Before
    // ownership begins, however, every host-received network observation
    // invalidates a controller snapshot so the initial mutation and evidence
    // publication share one monotonic transaction epoch.
    if (doctor_override_active.load(std::memory_order_relaxed)) {
      if (suppresses_quality_restore) {
        doctor_network_policy_regressed_during_override = true;
      }
      return;
    }
    if (pending_live_update_active.load(std::memory_order_relaxed)) {
      return;
    }
    ++operator_revision;
    state_changed.notify_all();
  }

  void note_doctor_video_policy_evidence(
      bool suppresses_quality_restore,
      bool nonblocking_video_observation) {
    std::lock_guard<std::mutex> lock(state_mutex);
    const int policy_class = suppresses_quality_restore ? 1 : 0;
    const int evidence_class = suppresses_quality_restore ? 2 :
      nonblocking_video_observation ? 1 : 0;
    if (doctor_video_policy_class == policy_class &&
        doctor_video_evidence_class == evidence_class) {
      return;
    }
    doctor_video_policy_class = policy_class;
    doctor_video_evidence_class = evidence_class;

    // Video samples collected during a Doctor-owned change are verification
    // evidence, not a competing controller writer. Latch a regression so a
    // later clean sample cannot hide it from the host verification window.
    if (doctor_override_active.load(std::memory_order_relaxed)) {
      if (suppresses_quality_restore) {
        doctor_video_policy_regressed_during_override = true;
      }
      return;
    }
    if (pending_live_update_active.load(std::memory_order_relaxed)) {
      return;
    }
    ++operator_revision;
    state_changed.notify_all();
  }

  bool doctor_policy_blocks_quality_restore() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return doctor_video_policy_class == 1 ||
      doctor_video_policy_regressed_during_override ||
      doctor_network_policy_regressed_during_override;
  }

  static std::int64_t policy_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()
    ).count();
  }

  static bool fec_media_live_locked() {
    const auto now = std::chrono::steady_clock::now();
    return fec_media_at && *fec_media_at <= now && *fec_media_at >= now - 2500ms;
  }

  static bool fec_deficit_recent_locked() {
    const auto now = std::chrono::steady_clock::now();
    // Selective reports are sparse: no new deficit is not a dead client.
    // Require bounded recent block evidence when opening the transaction;
    // freeze that evidence there, and check only media liveness at its ACK.
    return fec_partial_deficit_at && *fec_partial_deficit_at <= now &&
      *fec_partial_deficit_at >= now - 30s;
  }

  // A policy or outside writer owns a new F0-unit target. Publish the baseline before
  // it can raise the encoder rate, and invalidate even a same-value/ABA trial.
  static void retire_fec_for_writer_locked(const char *cause) {
    if (fec_actuator.configured()) {
      const auto before = fec_snapshot_locked();
      fec_actuator.restore();
      const auto after = fec_snapshot_locked();
      if (before.percentage != after.percentage) {
        record_fec_transition_locked("fec_retired", cause, before, after);
      } else if (before.phase != after.phase || before.pending_percentage != after.pending_percentage) {
        record_fec_transition_locked("fec_cancelled", cause, before, after);
      }
      if (stream_policy) {
        stream_policy->fec_applied(policy_now_ms(), fec_actuator.configured_percentage());
      }
    }
    policy_target_kbps = 0;
    fec_waiting_percentage.reset();
    fec_wait_reports = 0;
  }

  static void apply_fec_policy_locked(live_tuning_policy::policy_t &policy, bool media_report = false) {
    if (!fec_actuator.configured() || fec_actuator.pending()) {
      return;
    }
    const auto wanted = policy.fec_request();
    if (!wanted) {
      fec_waiting_percentage.reset();
      fec_wait_reports = 0;
      return;
    }
    if (wanted != fec_waiting_percentage) {
      fec_waiting_percentage = wanted;
      fec_wait_reports = 0;
    }
    const bool restoring = *wanted == fec_actuator.configured_percentage();
    const bool capable = scope_exclusive && scope_generation == fec_actuator.generation() &&
      enabled.load(std::memory_order_relaxed) && runtime_bitrate_in_place &&
      runtime_update_supported.load(std::memory_order_relaxed) &&
      !doctor_override_active.load(std::memory_order_relaxed) &&
      !explicit_live_override_active.load(std::memory_order_relaxed) &&
      !pending_live_update_active.load(std::memory_order_relaxed);
    if (!restoring && !capable) {
      policy.fec_refused(policy_now_ms());
      record_history_locked("fec_unavailable", "in_place_scope_unavailable");
      return;
    }
    const bool eligible = capable && fec_media_live_locked() && fec_deficit_recent_locked();
    const int actual_target = target_bitrate_kbps.load(std::memory_order_relaxed);
    // The rate-independent result may first restore the policy's base. Wait
    // for its real encoder acknowledgement rather than dropping that trial
    // or treating the requested base as applied.
    if ((!restoring && !eligible) || encoder_applied_bitrate_kbps != actual_target) {
      // Bound an evidence/ACK wait by valid host media receipts, not polling
      // frequency. It is not a failed parity trial and earns no FEC backoff.
      if (media_report && ++fec_wait_reports >= 8) {
        policy.fec_refused(policy_now_ms(), false);
        record_history_locked("fec_deferred", "evidence_or_encoder_wait_expired");
        fec_waiting_percentage.reset();
        fec_wait_reports = 0;
      }
      return;
    }
    const auto expected = fec_actuator.encoder_kbps(policy.target_kbps(), fec_actuator.percentage());
    if (!expected || *expected != actual_target) {
      policy.fec_refused(policy_now_ms());
      return;
    }
    const auto before = fec_snapshot_locked();
    const auto next = fec_actuator.request(policy.target_kbps(), *wanted,
      std::max(current_config.min_bitrate_kbps, runtime_bitrate_floor_kbps),
      eligible, action_authority_revision + 1);
    const auto after = fec_snapshot_locked();
    if (!next) {
      policy.fec_refused(policy_now_ms());
      return;
    }
    fec_waiting_percentage.reset();
    fec_wait_reports = 0;
    policy_target_kbps = policy.target_kbps();
    // request() has already published a lower F, if this restores F0.
    if (restoring) {
      policy.fec_applied(policy_now_ms(), fec_actuator.percentage());
    }
    target_bitrate_kbps.store(*next, std::memory_order_relaxed);
    ++action_authority_revision;
    ++operator_revision;
    record_fec_transition_locked("fec_target", restoring ? "restore_configured_parity" : "parity_trial",
      before, after);
    state_changed.notify_all();
  }

  // Called only after the existing exact request/revision guard and an actual
  // backend acknowledgement. The encoder names the next not-yet-encoded frame.
  static void confirm_fec_locked(int bitrate_kbps, std::int64_t next_frame_index) {
    const auto before = fec_snapshot_locked();
    const auto result = fec_actuator.confirm(scope_generation, action_authority_revision,
      bitrate_kbps, next_frame_index, runtime_bitrate_in_place, fec_media_live_locked());
    const auto after = fec_snapshot_locked();
    using result_e = live_tuning_fec::actuator_t::confirmation_e;
    if (result == result_e::published && stream_policy) {
      stream_policy->fec_applied(policy_now_ms(), fec_actuator.percentage());
      record_fec_transition_locked("fec_published", "in_place_encoder_confirmed", before, after,
        bitrate_kbps, operator_revision, next_frame_index);
    } else if (result == result_e::restored) {
      // F0 was already published by request(); this only confirms the restored encoder rate.
      record_fec_transition_locked("fec_restored", "configured_parity_encoder_confirmed", before, after,
        bitrate_kbps, operator_revision);
    } else if (result == result_e::refused || result == result_e::stale) {
      // Parity was never published. Hold the known encoder rate at F0,
      // including when the backend rejected the lower-rate request. A report
      // gap must not itself request an increase or pretend one was applied.
      const int known = encoder_applied_bitrate_kbps;
      if (known > 0) {
        policy_target_kbps = known;
        target_bitrate_kbps.store(known, std::memory_order_relaxed);
        if (stream_policy) {
          stream_policy->fec_refused(policy_now_ms(), false);
          stream_policy->adopt(policy_now_ms(), known);
        }
      }
      ++action_authority_revision;
      ++operator_revision;
      record_fec_transition_locked("fec_refused", result == result_e::stale ?
        "stale_fec_authority" : "encoder_boundary_or_feedback_unavailable", before, after);
    }
  }

  void configure_fec_budget(std::uint64_t generation,
                            const stream_bitrate::request_t &request,
                            bool client_advertised_fec) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (generation != scope_generation || !scope_exclusive || !client_advertised_fec) {
      return;
    }
    retire_fec_for_writer_locked("fec_budget_reconfigured");
    fec_partial_deficit_at.reset();
    fec_media_at.reset();
    const bool configured = fec_actuator.configure(generation, request);
    if (stream_policy) {
      stream_policy->configure_fec(configured ? request.fec_percentage : -1);
    }
  }

  void note_fec_partial_deficit(std::uint64_t generation,
                               std::chrono::steady_clock::time_point received_at) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (generation && generation == scope_generation && scope_exclusive &&
        generation == fec_actuator.generation() &&
        (!fec_partial_deficit_at || received_at > *fec_partial_deficit_at)) {
      fec_partial_deficit_at = received_at;
    }
  }

  live_tuning_fec::selection_t frame_fec_selection(std::uint64_t generation, std::int64_t frame_index,
                                                 int configured_fallback) {
    return fec_actuator.selection_for_frame(generation, frame_index, configured_fallback);
  }

  std::pair<int, int> frame_fec_allocation(std::uint64_t generation, std::int64_t frame_index,
                                          int configured_fallback) {
    return frame_fec_selection(generation, frame_index, configured_fallback).allocation;
  }

  // This stream's policy, following every writer outside it. Requires state_mutex and a base bitrate.
  // A client request, a floor or a ceiling the policy has not seen is a rebase; a target it did not set
  // (encoder load, Doctor, a manual bitrate, a clamp) is where it carries on from.
  static live_tuning_policy::policy_t &policy_locked(std::int64_t now_ms) {
    const int base = std::min(base_bitrate_kbps.load(std::memory_order_relaxed), current_config.max_bitrate_kbps);
    const int floor = std::min(current_config.min_bitrate_kbps, base);
    const int target = policy_target_kbps > 0 ? policy_target_kbps :
      target_bitrate_kbps.load(std::memory_order_relaxed);
    if (!stream_policy) {
      stream_policy.emplace(base, floor);
      if (fec_actuator.configured()) {
        stream_policy->configure_fec(fec_actuator.configured_percentage());
      }
    }
    if (stream_policy->base_kbps() != base || stream_policy->floor_kbps() != floor) {
      stream_policy->rebase(base, floor);
    }
    if (target > 0 && stream_policy->target_kbps() != target) {
      stream_policy->adopt(now_ms, target);
    }
    return *stream_policy;
  }

  // The controller state and reason Live Tuning reports for where the policy is. The states are the ones
  // clients already read; the reasons say what the policy is doing.
  static void set_policy_status_locked(const live_tuning_policy::policy_t &p) {
    using live_tuning_policy::state_e;
    switch (p.state()) {
      case state_e::descend:
        set_controller_status("network_pressure", "packet_loss");
        return;
      case state_e::rtt_cut:
        set_controller_status("network_pressure", "standing_queue");
        return;
      case state_e::bisect:
        set_controller_status("network_pressure", "finding_the_link");
        return;
      case state_e::hold:
        if (p.ceiling_kbps()) {
          // Settled under a link or a queue: holding, not recovering, though it retries above now and then.
          set_controller_status("network_pressure", p.ceiling_is_queue() ? "holding_under_a_queue" : "holding_under_the_link");
        } else {
          set_controller_status("recovering", "waiting_to_climb");
        }
        return;
      case state_e::verify_hi:
        set_controller_status("recovering", "testing_a_step_up");
        return;
      case state_e::climb:
        set_controller_status("recovering", "climbing");
        return;
      case state_e::normal:
        // Loss that does not depend on the bitrate is left to the stream: cutting would not help it.
        set_controller_status("steady", p.background_loss() >= 0.005 ? "loss_independent_of_bitrate" : "healthy");
        return;
    }
  }

  // Publish what the policy decided. Requires state_mutex. A decision that could not move a clamped target,
  // such as a probe that starts at the floor, still advances the revision, so Doctor cannot raise quality
  // from a clean snapshot taken before it.
  static void apply_policy_locked(live_tuning_policy::policy_t &p,
                                  std::uint64_t changes_before,
                                  live_tuning_policy::state_e state_before,
                                  bool media_report = false) {
    if (p.changes() == changes_before && p.state() == state_before) {
      // Nothing decided: leave the status, which encoder load or frame pacing may have set since.
      apply_fec_policy_locked(p, media_report);
      return;
    }
    set_policy_status_locked(p);
    const int base = base_bitrate_kbps.load(std::memory_order_relaxed);
    const int current = target_bitrate_kbps.load(std::memory_order_relaxed);
    const int next = clamp_target(p.target_kbps(), base);
    const bool parity_reverted = fec_actuator.configured() &&
      (p.fec_request() == fec_actuator.configured_percentage() ||
       (fec_actuator.phase() == live_tuning_fec::actuator_t::phase_e::lowering_rate &&
        p.fec_request() != fec_actuator.pending_percentage()));
    const bool allocated = fec_actuator.percentage() != fec_actuator.configured_percentage() ||
      fec_actuator.pending();
    if (allocated && ((policy_target_kbps > 0 && next != policy_target_kbps) || parity_reverted)) {
      retire_fec_for_writer_locked(policy_target_kbps > 0 && next != policy_target_kbps ?
        "policy_target_changed" : "policy_parity_reverted");
    }
    policy_target_kbps = next;
    // A state-only decision does not undo the pending rate allocation or
    // rotate its authority. The next exact encoder ACK can still complete it.
    const bool keep_pending = fec_actuator.phase() == live_tuning_fec::actuator_t::phase_e::lowering_rate &&
      p.fec_request() == fec_actuator.pending_percentage();
    const int encoder_next = keep_pending ? current : fec_actuator.configured() ?
      fec_actuator.encoder_kbps(next, fec_actuator.percentage()).value_or(next) : next;
    if (encoder_next != current) {
      target_bitrate_kbps.store(encoder_next, std::memory_order_relaxed);
      ++action_authority_revision;
      BOOST_LOG(debug) << "Live Tuning: " << current << " -> " << encoder_next << " kbps ("
                       << p.last_change().why << ", " << live_tuning_policy::name(p.state()) << ')';
    }
    ++operator_revision;
    apply_fec_policy_locked(p, media_report);
    state_changed.notify_all();
  }

  // Requires state_mutex. False when nothing may move the bitrate from feedback now.
  static bool feedback_accepted_locked() {
    return !doctor_override_active.load(std::memory_order_relaxed) &&
      !pending_live_update_active.load(std::memory_order_relaxed) &&
      base_bitrate_kbps.load(std::memory_order_relaxed) > 0;
  }

  // The first feedback starts the encoder-pressure interval, as it always has.
  static void initialize_feedback_locked() {
    if (initialized) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    last_adjustment_time = now;
    last_pressure_time = now - 10s;
    initialized = true;
  }

  static void note_rtt_locked(double rtt_ms) {
    if (!initialized) {
      ewma_rtt = rtt_ms;
      initialize_feedback_locked();
    }
    ewma_rtt = current_config.ewma_alpha * rtt_ms + (1.0 - current_config.ewma_alpha) * ewma_rtt;
    last_input_available = true;
    last_input_rtt_ms = rtt_ms;
  }

  void update_rtt(double rtt_ms) {
    if (!enabled.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed) ||
        !std::isfinite(rtt_ms) || rtt_ms < 0.0) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"network_feedback"};
    if (!feedback_accepted_locked()) {
      return;
    }
    note_rtt_locked(rtt_ms);
    const auto now_ms = policy_now_ms();
    auto &p = policy_locked(now_ms);
    const auto changes_before = p.changes();
    const auto state_before = p.state();
    p.rtt(now_ms, rtt_ms);
    apply_policy_locked(p, changes_before, state_before);
  }

  void update_media_report(double frames_expected, double frames_lost, double rtt_ms) {
    if (!enabled.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed) ||
        !std::isfinite(frames_expected) || !std::isfinite(frames_lost) ||
        frames_expected <= 0.0 || frames_lost < 0.0 || frames_lost > frames_expected) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"network_feedback"};
    if (!feedback_accepted_locked()) {
      return;
    }
    fec_media_at = std::chrono::steady_clock::now();
    const double loss_pct = 100.0 * frames_lost / frames_expected;
    ewma_packet_loss = !loss_average_started ? loss_pct :
      current_config.ewma_alpha * loss_pct + (1.0 - current_config.ewma_alpha) * ewma_packet_loss;
    loss_average_started = true;
    last_input_packet_loss_percent = loss_pct;
    if (std::isfinite(rtt_ms) && rtt_ms >= 0.0) {
      note_rtt_locked(rtt_ms);
    } else {
      initialize_feedback_locked();
      last_input_available = true;
    }
    const auto now_ms = policy_now_ms();
    auto &p = policy_locked(now_ms);
    const auto changes_before = p.changes();
    const auto state_before = p.state();
    p.report(now_ms, frames_expected, frames_lost);
    apply_policy_locked(p, changes_before, state_before, true);
  }

  void update_stream_health(double fps_ratio,
                            double dropped_frame_ratio,
                            double duplicate_frame_ratio,
                            double frame_jitter_ms,
                            double encode_time_ms,
                            double avg_frame_age_ms,
                            double target_fps) {
    if (!enabled.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"stream_health"};
    if (doctor_override_active.load(std::memory_order_relaxed) ||
        pending_live_update_active.load(std::memory_order_relaxed)) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    if (!initialized) {
      last_adjustment_time = now;
      last_pressure_time = now - 10s;
      initialized = true;
    }

    // Preserve the existing low-refresh/unknown-target policy. At high refresh,
    // an encoder can miss every deadline well below 11 ms (240 Hz = 4.17 ms).
    // Require measured delivery loss as well as an over-budget encode sample
    // before using that lower threshold; an isolated slow frame in an otherwise
    // healthy stream is not enough evidence to reduce picture quality.
    const double frame_budget_ms = std::isfinite(target_fps) && target_fps > 0.0 ?
      1000.0 / target_fps : 11.0;
    const bool delivery_shortfall = std::isfinite(fps_ratio) && fps_ratio > 0.0 && fps_ratio < 0.95;
    const bool encoder_pressure = std::isfinite(encode_time_ms) &&
      (encode_time_ms >= 11.0 || (delivery_shortfall && encode_time_ms >= frame_budget_ms));
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
    retire_fec_for_writer_locked("encoder_pressure");
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
                       << "ms, target_fps=" << target_fps
                       << ", frame_budget=" << frame_budget_ms
                       << "ms, age=" << avg_frame_age_ms << "ms)";
      target_bitrate_kbps.store(new_target, std::memory_order_relaxed);
      ++action_authority_revision;
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

  static state_t get_state_locked() {
    state_t state;
    state.session_generation = scope_generation;
    state.app_session_id = scope_app_session_id;
    state.session_exclusive = scope_exclusive;
    state.configured_enabled = current_config.enabled;
    state.feedback_initialized = initialized;
    state.applied_bitrate_kbps = encoder_applied_bitrate_kbps;
    state.runtime_update_supported = runtime_update_supported.load(std::memory_order_relaxed);
    auto status = effective_status_locked();
    state.enabled = status.enabled;
    state.active = status.active;
    state.base_bitrate_kbps = base_bitrate_kbps.load(std::memory_order_relaxed);
    state.target_bitrate_kbps = state.active ? target_bitrate_kbps.load(std::memory_order_relaxed) : 0;
    state.min_bitrate_kbps = current_config.min_bitrate_kbps;
    state.max_bitrate_kbps = current_config.max_bitrate_kbps;
    state.floor_source = floor_source;
    state.paused_for_stream = paused_for_stream;
    state.ewma_packet_loss = ewma_packet_loss;
    state.ewma_rtt_ms = ewma_rtt;
    if (stream_policy) {
      state.ceiling_bitrate_kbps = stream_policy->ceiling_kbps().value_or(0);
      state.ceiling_is_queue = stream_policy->ceiling_kbps() && stream_policy->ceiling_is_queue();
      state.background_loss_pct = 100.0 * stream_policy->background_loss();
    }
    state.state = std::move(status.state);
    state.reason = std::move(status.reason);
    state.fec_budget_available = fec_actuator.configured();
    state.runtime_bitrate_in_place = runtime_bitrate_in_place;
    state.policy_target_bitrate_kbps = policy_target_kbps > 0 ? policy_target_kbps :
      target_bitrate_kbps.load(std::memory_order_relaxed);
    state.configured_fec_percentage = fec_actuator.configured_percentage();
    state.current_fec_percentage = fec_actuator.percentage();
    using phase_e = live_tuning_fec::actuator_t::phase_e;
    state.fec_phase = fec_actuator.phase() == phase_e::lowering_rate ? "lowering_rate" :
      fec_actuator.phase() == phase_e::raising_rate ? "raising_rate" : "fixed";
    return state;
  }

  state_t get_state() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return get_state_locked();
  }

  state_t get_state_with_history() {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto state = get_state_locked();
    state.history = history;
    state.history.host_elapsed_ms_at_copy = history_elapsed_ms_locked(std::chrono::steady_clock::now());
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
      action_authority_revision,
      operator_revision,
    };
  }

  static doctor_bitrate_apply_result_t apply_doctor_bitrate_if_revision_locked(
      std::uint64_t expected_revision,
      int target,
      std::optional<int> temporary_max_bitrate_kbps,
      bool require_quality_policy_clear) {
    decision_note_t note {"doctor_action"};
    if (operator_revision != expected_revision ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
      return {
        doctor_bitrate_apply_status_e::controller_changed,
        0,
      };
    }
    if (require_quality_policy_clear &&
        (doctor_video_policy_class == 1 ||
         doctor_video_policy_regressed_during_override ||
         doctor_network_policy_regressed_during_override)) {
      return {
        doctor_bitrate_apply_status_e::quality_policy_blocked,
        0,
      };
    }

    retire_fec_for_writer_locked("doctor_apply");
    if (!doctor_override_active.load(std::memory_order_relaxed)) {
      doctor_previous_max_bitrate_kbps = current_config.max_bitrate_kbps;
      doctor_video_policy_regressed_during_override = false;
      doctor_network_policy_regressed_during_override = false;
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
    ++action_authority_revision;
    const auto revision = ++operator_revision;
    state_changed.notify_all();
    return {
      doctor_bitrate_apply_status_e::applied,
      revision,
    };
  }

  std::optional<std::uint64_t> set_doctor_bitrate_if_revision(
      std::uint64_t expected_revision,
      int target,
      std::optional<int> temporary_max_bitrate_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    const auto result = apply_doctor_bitrate_if_revision_locked(
      expected_revision,
      target,
      temporary_max_bitrate_kbps,
      false
    );
    return result.status == doctor_bitrate_apply_status_e::applied ?
      std::optional<std::uint64_t> {result.revision} : std::nullopt;
  }

  doctor_bitrate_apply_result_t set_doctor_quality_bitrate_if_revision(
      std::uint64_t expected_revision,
      int target,
      std::optional<int> temporary_max_bitrate_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    return apply_doctor_bitrate_if_revision_locked(
      expected_revision,
      target,
      temporary_max_bitrate_kbps,
      true
    );
  }

  std::optional<std::uint64_t> restore_doctor_state_if_revision(
      std::uint64_t expected_revision,
      const doctor_state_t &previous_state) {
    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"doctor_rollback"};
    if (operator_revision != expected_revision) {
      return std::nullopt;
    }

    retire_fec_for_writer_locked("doctor_rollback");
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
    ++action_authority_revision;
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
      record_history_locked("revision_reaffirmed", "doctor_rollback");
    }
    state_changed.notify_all();
    return revision;
  }

  std::optional<live_bitrate_request_t> get_live_bitrate_request() {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!runtime_update_supported.load(std::memory_order_relaxed)) {
      return std::nullopt;
    }
    if (stream_policy) apply_fec_policy_locked(*stream_policy);
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
      record_history_locked("revision_reaffirmed", "encoder_poll");
      note_decision_locked("encoder_poll");
      state_changed.notify_all();
    }
    return live_bitrate_request_t {target, operator_revision};
  }

  std::optional<live_bitrate_request_t> get_encoder_open_request(
      std::uint64_t generation, int configured_encoder_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    const int target = target_bitrate_kbps.load(std::memory_order_relaxed);
    if (!generation || generation != scope_generation || !scope_exclusive ||
        !operator_revision || configured_encoder_kbps <= 0 || target != configured_encoder_kbps) {
      return std::nullopt;
    }
    // In particular, do not reaffirm encoder_applied_revision or create a
    // history event. A later successful SDK open must supply its own receipt.
    return live_bitrate_request_t {target, operator_revision};
  }

  std::optional<live_pacing_cap_t> get_live_pacing_cap(std::uint64_t generation) {
    // The shared sender must not wait behind an encoder/SDK reconfiguration.
    // No field is read until this coherent, non-blocking snapshot is owned.
    std::unique_lock<std::mutex> lock(state_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return std::nullopt;
    // Unlike get_live_bitrate_request(), this never advances policy, reaffirms
    // an ACK or publishes parity. It can only restrict an immutable old frame.
    const int target = target_bitrate_kbps.load(std::memory_order_relaxed);
    if (!generation || generation != scope_generation || !scope_exclusive ||
        !enabled.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed) ||
        doctor_override_active.load(std::memory_order_relaxed) ||
        explicit_live_override_active.load(std::memory_order_relaxed) ||
        pending_live_update_active.load(std::memory_order_relaxed) || target <= 0) {
      return std::nullopt;
    }
    return live_pacing_cap_t {generation, operator_revision, target,
      policy_target_kbps > 0 ? policy_target_kbps : target};
  }

  bool apply_live_bitrate_request(const live_bitrate_request_t &request,
                                  const std::function<bool()> &apply,
                                  vrr_trace::bitrate_observation_t *trace_observation,
                                  std::int64_t next_frame_index) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (trace_observation) {
      *trace_observation = {scope_generation, history.trace_scope ? history.trace_scope->id : 0,
        target_bitrate_kbps.load(std::memory_order_relaxed),
        operator_revision, encoder_applied_bitrate_kbps, encoder_applied_revision};
    }
    if (request.revision != operator_revision ||
        request.target_bitrate_kbps != target_bitrate_kbps.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
      record_history_locked("stale_encoder_request", "encoder_callback",
        request.target_bitrate_kbps, request.revision);
      return false;
    }
    if (apply()) {
      encoder_applied_bitrate_kbps = request.target_bitrate_kbps;
      last_confirmed_bitrate_kbps = request.target_bitrate_kbps;
      encoder_applied_revision = request.revision;
      encoder_applied_at = std::chrono::steady_clock::now();
      pending_live_update_active.store(false, std::memory_order_relaxed);
      record_history_locked("encoder_applied", "runtime_update",
        request.target_bitrate_kbps, request.revision);
      confirm_fec_locked(request.target_bitrate_kbps, next_frame_index);
      state_changed.notify_all();
    } else {
      record_history_locked("encoder_update_not_applied", "runtime_update",
        request.target_bitrate_kbps, request.revision);
      if (fec_actuator.phase() == live_tuning_fec::actuator_t::phase_e::lowering_rate) {
        // An advertised in-place backend did not apply this request. Do not
        // let a recreation turn that refusal into an adaptive-parity ACK.
        runtime_bitrate_in_place = false;
        confirm_fec_locked(request.target_bitrate_kbps, next_frame_index);
      }
    }
    note_decision_locked("encoder_callback");
    return true;
  }

  void set_session_scope(std::uint64_t generation, const std::string &app_session_id,
                         bool exclusive) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (generation != scope_generation || app_session_id != scope_app_session_id || !exclusive) {
      retire_fec_for_writer_locked("session_scope_changed");
      fec_actuator.reset();
      if (stream_policy) stream_policy->configure_fec(-1);
      fec_partial_deficit_at.reset();
      fec_media_at.reset();
    }
    scope_generation = generation;
    scope_app_session_id = app_session_id;
    scope_exclusive = exclusive;
    if (history.scoped && generation != 0 && history.session_generation == generation &&
        history.app_session_id == app_session_id) {
      return;
    }
    // Any other scope identity starts over. Nothing recorded before it, unscoped or under another
    // scope (including another owner of the same generation), is proven to belong to it, so none of it
    // is kept, and neither is the diagnostic copy of the newest network input. Controller state is not
    // touched. An ended stream (scope 0) keeps no history.
    clear_history_locked();
    last_input_available = false;
    last_input_packet_loss_percent = 0.0;
    last_input_rtt_ms = 0.0;
    if (generation == 0) {
      return;
    }
    history.scoped = true;
    history.session_generation = generation;
    history.app_session_id = app_session_id;
    // One explicit snapshot of the controller as this scope finds it, then only changes after it.
    const auto status = effective_status_locked();
    last_history_decision = decision_key_t {target_bitrate_kbps.load(std::memory_order_relaxed), status.state, status.reason};
    record_history_locked("scope_started", "initial_scope_snapshot");
    if (vrr_trace::enabled()) {
      auto &proof = history.trace_scope.emplace();
      proof.id = vrr_trace::next_id();
      proof.origin_steady_ns = vrr_trace::ns(history_started_at);
      if (!history.entries.empty() && history.entries.front().kind == "scope_started") {
        proof.first_sequence = history.entries.front().sequence;
      }
      auto row = vrr_trace::event_t("live_tuning_scope").u("session_generation", generation)
        .u("trace_scope", proof.id).u("exclusive_at_scope_start", exclusive).o("origin_steady_ns", proof.origin_steady_ns);
      if (proof.first_sequence) row.u("first_sequence", *proof.first_sequence);
      else row.n("first_sequence");
      vrr_trace::record(row);
      if (!proof.id || !proof.origin_steady_ns || !proof.first_sequence) {
        vrr_trace::record(vrr_trace::event_t("instrumentation_refused").s("reason", "live_tuning_scope_unavailable")
          .u("session_generation", generation));
      }
    }
  }

  bool begin_live_bitrate_session_recreation(
      std::uint64_t revision,
      int bitrate_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (revision != operator_revision ||
        bitrate_kbps != target_bitrate_kbps.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
      record_history_locked("recreation_superseded", "encoder_recreation", bitrate_kbps, revision);
      return false;
    }

    // The retiring encoder no longer proves any bitrate, including a later
    // rollback to the value it used before Doctor acted. Keep runtime support
    // available so an in-flight rollback can wait for the replacement session
    // instead of failing immediately as unsupported.
    encoder_applied_bitrate_kbps = 0;
    encoder_applied_revision = 0;
    encoder_applied_at = {};
    pending_live_update_active.store(true, std::memory_order_relaxed);
    record_history_locked("applied_proof_invalidated", "encoder_recreation", bitrate_kbps, revision);
    note_decision_locked("encoder_recreation");
    state_changed.notify_all();
    return true;
  }

  void acknowledge_live_bitrate_applied(
      std::uint64_t revision,
      int bitrate_kbps,
      std::int64_t next_frame_index) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (revision != operator_revision ||
        bitrate_kbps != target_bitrate_kbps.load(std::memory_order_relaxed)) {
      record_history_locked("stale_acknowledgement", "encoder_callback", bitrate_kbps, revision);
      return;
    }
    // The encoder thread confirms an unchanged rate every frame. Only a confirmation that proves
    // something new becomes history.
    const bool already_proven = encoder_applied_bitrate_kbps == bitrate_kbps &&
      encoder_applied_revision == revision;
    const bool proof_was_unknown = encoder_applied_bitrate_kbps <= 0;
    encoder_applied_bitrate_kbps = bitrate_kbps;
    last_confirmed_bitrate_kbps = bitrate_kbps;
    encoder_applied_revision = revision;
    encoder_applied_at = std::chrono::steady_clock::now();
    pending_live_update_active.store(false, std::memory_order_relaxed);
    confirm_fec_locked(bitrate_kbps, next_frame_index);
    if (!already_proven) {
      record_history_locked("encoder_acknowledged",
        proof_was_unknown ? "applied_proof_was_unknown" : "encoder_running_at_rate", bitrate_kbps, revision);
      note_decision_locked("encoder_callback");
    }
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
    decision_note_t note {"base_bitrate"};
    retire_fec_for_writer_locked("base_bitrate_changed");
    retire_doctor_override_locked();
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);
    admit_client_bitrate_locked(kbps);
    const int clamped = std::clamp(kbps, current_config.min_bitrate_kbps, current_config.max_bitrate_kbps);
    base_bitrate_kbps.store(clamped, std::memory_order_relaxed);

    // Initialize target to base when first set
    int expected = 0;
    if (!target_bitrate_kbps.compare_exchange_strong(expected, clamped, std::memory_order_relaxed)) {
      int current = target_bitrate_kbps.load(std::memory_order_relaxed);
      target_bitrate_kbps.store(clamp_target(current, clamped), std::memory_order_relaxed);
    }
    ++operator_revision;
    ++action_authority_revision;
    state_changed.notify_all();
  }

  static void apply_live_bitrate_locked(int kbps);

  void set_live_bitrate(int kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"live_bitrate"};
    apply_live_bitrate_locked(kbps);
  }

  void set_live_bitrate_for_stream(int kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"live_bitrate_for_stream"};
    // Only a stream that had Live Tuning running has anything to put back when it ends.
    paused_for_stream = paused_for_stream || enabled.load(std::memory_order_relaxed);
    enabled.store(false, std::memory_order_relaxed);
    apply_live_bitrate_locked(kbps);
  }

  void end_stream_override() {
    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"stream_override_ended"};
    if (!paused_for_stream) {
      return;
    }
    paused_for_stream = false;
    enabled.store(current_config.enabled, std::memory_order_relaxed);
    ++operator_revision;
    ++action_authority_revision;
    state_changed.notify_all();
  }

  static void apply_live_bitrate_locked(int kbps) {
    retire_fec_for_writer_locked("explicit_live_bitrate");
    retire_doctor_override_locked();
    explicit_live_override_active.store(
      !enabled.load(std::memory_order_relaxed),
      std::memory_order_relaxed
    );
    // The paired endpoints already hold this to stream_bitrate::request_in_range(), 1000 to
    // 500000 kbps. The host cap, max_bitrate, bounds it as it bounds RTSP and launch requests;
    // adaptive_bitrate_max does not.
    const int requested = config::video.max_bitrate > 0 ?
      std::min(kbps, config::video.max_bitrate) : kbps;
    admit_client_bitrate_locked(requested);
    const int clamped = std::clamp(requested, current_config.min_bitrate_kbps, current_config.max_bitrate_kbps);
    base_bitrate_kbps.store(clamped, std::memory_order_relaxed);
    target_bitrate_kbps.store(clamped, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);
    set_controller_status("steady", "paired_client_action");
    ++operator_revision;
    ++action_authority_revision;
    state_changed.notify_all();
  }

  void set_session_floor(int kbps, std::string_view source) {
    if (kbps <= 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"session_floor"};
    retire_fec_for_writer_locked("session_floor_changed");
    const int base = base_bitrate_kbps.load(std::memory_order_relaxed);
    const int floor = std::max(
      current_config.min_bitrate_kbps,
      base > 0 ? std::min(kbps, base) : kbps
    );
    current_config.min_bitrate_kbps = floor;
    current_config.max_bitrate_kbps = std::max(current_config.max_bitrate_kbps, floor);
    floor_source = source;
    const int target = target_bitrate_kbps.load(std::memory_order_relaxed);
    if (target > 0 && target < floor) {
      target_bitrate_kbps.store(floor, std::memory_order_relaxed);
    }
    ++operator_revision;
    ++action_authority_revision;
    state_changed.notify_all();
  }

  void set_max_bitrate(int kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"max_bitrate"};
    retire_fec_for_writer_locked("max_bitrate_changed");
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
    ++action_authority_revision;
    state_changed.notify_all();
  }

  void set_enabled(bool enable) {
    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"enabled_changed"};
    retire_fec_for_writer_locked("enabled_changed");
    retire_doctor_override_locked();
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    // Disabling holds what the encoder actually applied, never an unacknowledged
    // automatic increase. Re-enabling starts from that same live bitrate.
    if (!enable && last_confirmed_bitrate_kbps > 0) {
      target_bitrate_kbps.store(last_confirmed_bitrate_kbps, std::memory_order_relaxed);
    }
    current_config.enabled = enable;
    config::video.adaptive_bitrate.enabled = enable;
    enabled.store(enable, std::memory_order_relaxed);
    paused_for_stream = false;
    ++action_authority_revision;
    const auto revision = ++operator_revision;
    const int target = target_bitrate_kbps.load(std::memory_order_relaxed);
    const bool needs_encoder_update = target > 0 && encoder_applied_bitrate_kbps != target;
    pending_live_update_active.store(needs_encoder_update, std::memory_order_relaxed);
    if (!needs_encoder_update && target > 0) {
      encoder_applied_revision = revision;
      encoder_applied_at = std::chrono::steady_clock::now();
      record_history_locked("revision_reaffirmed", "enabled_changed");
    }
    state_changed.notify_all();
  }

  void set_runtime_enabled(bool enable) {
    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"runtime_enabled"};
    retire_fec_for_writer_locked("runtime_enabled_changed");
    retire_doctor_override_locked();
    paused_for_stream = false;
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);
    enabled.store(enable, std::memory_order_relaxed);
    ++operator_revision;
    ++action_authority_revision;
    state_changed.notify_all();
  }

  bool is_enabled() {
    return enabled.load(std::memory_order_relaxed);
  }

  void set_runtime_update_supported(bool supported,
                                    const std::string &reason,
                                    int initial_encoder_bitrate_kbps,
                                    bool in_place,
                                    int in_place_floor_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    decision_note_t note {"runtime_support"};
    runtime_bitrate_in_place = supported && in_place;
    runtime_bitrate_floor_kbps = std::max(in_place_floor_kbps, 1);
    if (initial_encoder_bitrate_kbps > 0 && fec_actuator.configured() &&
        fec_actuator.percentage() != fec_actuator.configured_percentage() &&
        (fec_actuator.encoder_kbps(policy_target_kbps, fec_actuator.percentage()) !=
           initial_encoder_bitrate_kbps ||
         initial_encoder_bitrate_kbps < runtime_bitrate_floor_kbps)) {
      // A capture/backend reopen may start at a different rate. It is not an
      // ACK of the old allocation: publish F0 before this encoder's first frame.
      retire_fec_for_writer_locked("encoder_reopened");
      ++action_authority_revision;
      ++operator_revision;
    }
    if (!runtime_bitrate_in_place &&
        fec_actuator.phase() == live_tuning_fec::actuator_t::phase_e::lowering_rate) {
      confirm_fec_locked(target_bitrate_kbps.load(std::memory_order_relaxed), 0);
    }
    runtime_update_supported.store(supported, std::memory_order_relaxed);
    runtime_update_reason = supported ? "supported" :
      (reason.empty() ? "encoder_runtime_update_unsupported" : reason);
    if (supported) {
      if (initial_encoder_bitrate_kbps > 0) {
        encoder_applied_bitrate_kbps = initial_encoder_bitrate_kbps;
        last_confirmed_bitrate_kbps = initial_encoder_bitrate_kbps;
        encoder_applied_revision = operator_revision;
        encoder_applied_at = std::chrono::steady_clock::now();
      }
      set_controller_status("steady", "encoder_ready");
      if (initial_encoder_bitrate_kbps > 0) {
        record_history_locked("encoder_initialized", "encoder_ready", initial_encoder_bitrate_kbps);
      }
    } else {
      const bool was_known = encoder_applied_bitrate_kbps > 0;
      encoder_applied_bitrate_kbps = 0;
      encoder_applied_revision = 0;
      encoder_applied_at = {};
      if (was_known) {
        record_history_locked("applied_unknown", "runtime_update_stopped");
      }
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
    decision_note_t note {"load_config"};
    retire_fec_for_writer_locked("config_reloaded");
    doctor_override_active.store(false, std::memory_order_relaxed);
    doctor_previous_max_bitrate_kbps = 0;
    doctor_video_policy_regressed_during_override = false;
    doctor_network_policy_regressed_during_override = false;
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);

    current_config.enabled = config::video.adaptive_bitrate.enabled;
    current_config.min_bitrate_kbps = config::video.adaptive_bitrate.min_bitrate_kbps;
    floor_source = "adaptive_bitrate_min";
    paused_for_stream = false;
    current_config.max_bitrate_kbps = config::video.adaptive_bitrate.max_bitrate_kbps;
    normalize_config_bounds(current_config);
    config::video.adaptive_bitrate.max_bitrate_kbps = current_config.max_bitrate_kbps;

    enabled.store(current_config.enabled, std::memory_order_relaxed);
    ++operator_revision;
    ++action_authority_revision;
    state_changed.notify_all();

    BOOST_LOG(info) << "Adaptive bitrate: "
                    << (current_config.enabled ? "enabled" : "disabled")
                    << " (min=" << current_config.min_bitrate_kbps
                    << " kbps, max=" << current_config.max_bitrate_kbps << " kbps)";
  }

  void reset() {
    std::lock_guard<std::mutex> lock(state_mutex);
    fec_actuator.reset();
    runtime_bitrate_in_place = false;
    runtime_bitrate_floor_kbps = 1;
    policy_target_kbps = 0;
    fec_partial_deficit_at.reset();
    fec_media_at.reset();
    fec_waiting_percentage.reset();
    fec_wait_reports = 0;
    fec_waiting_percentage.reset();
    fec_wait_reports = 0;
    doctor_override_active.store(false, std::memory_order_relaxed);
    doctor_previous_max_bitrate_kbps = 0;
    explicit_live_override_active.store(false, std::memory_order_relaxed);
    pending_live_update_active.store(false, std::memory_order_relaxed);

    ewma_packet_loss = 0.0;
    ewma_rtt = 0.0;
    loss_average_started = false;
    stream_policy.reset();
    initialized = false;
    runtime_update_supported.store(false, std::memory_order_relaxed);
    runtime_update_reason = "encoder_not_initialized";
    set_controller_status(enabled.load(std::memory_order_relaxed) ? "steady" : "disabled",
                          enabled.load(std::memory_order_relaxed) ? "reset" : "disabled");

    base_bitrate_kbps.store(0, std::memory_order_relaxed);
    target_bitrate_kbps.store(0, std::memory_order_relaxed);
    encoder_applied_bitrate_kbps = 0;
    last_confirmed_bitrate_kbps = 0;
    encoder_applied_revision = 0;
    encoder_applied_at = {};
    doctor_video_policy_class = -1;
    doctor_video_evidence_class = -1;
    doctor_video_policy_regressed_during_override = false;
    doctor_network_policy_regressed_during_override = false;
    ++operator_revision;
    ++action_authority_revision;
    // A reset starts a new stream's controller. Nothing recorded before it may be read as that
    // stream's history, so the history starts over, unscoped until set_session_scope() names it.
    last_input_available = false;
    last_input_packet_loss_percent = 0.0;
    last_input_rtt_ms = 0.0;
    clear_history_locked();
    note_decision_locked("reset");
    state_changed.notify_all();
  }

}  // namespace adaptive_bitrate
