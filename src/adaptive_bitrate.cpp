/**
 * @file src/adaptive_bitrate.cpp
 * @brief Adaptive bitrate controller using EWMA-based network feedback.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

// local includes
#include "adaptive_bitrate.h"
#include <cmath>
#include "config.h"
#include "logging.h"

using namespace std::literals;

namespace adaptive_bitrate {

  // Keep autonomous bitrate changes behind the same actionable RTT boundary
  // Doctor uses. A relative-only spike check turns normal LAN jitter such as
  // 1 ms -> 3 ms into "congestion" even though the path is still excellent.
  static constexpr double ACTIONABLE_RTT_MS = 45.0;
  // Loss above this share of frames is pressure, and above HEAVY_LOSS_PCT it is heavy: the controller
  // cuts by its whole max_change_rate.
  static constexpr double LOSS_PRESSURE_PCT = 1.0;
  static constexpr double HEAVY_LOSS_PCT = 5.0;
  // Moderate loss with no RTT spike beside it cuts the target no lower than this share of the
  // stream's base until a test says the bitrate is the cause. See update_network_stats().
  static constexpr double MODERATE_LOSS_FLOOR_SHARE = 0.5;
  // How long Live Tuning holds a rate, over at least RATE_TEST_MIN_REPORTS client media reports,
  // before it tests the rate or judges a test of it.
  static constexpr auto RATE_TEST_DWELL = 8s;
  static constexpr std::size_t RATE_TEST_MIN_REPORTS = 5;
  // The newest reports at a rate that judge it.
  static constexpr std::size_t RATE_TEST_MAX_REPORTS = 20;
  // Loss in at least this share of a rate's reports is steady. A link too small for the stream loses
  // frames every second, while Wi-Fi loses them in bursts at any rate.
  static constexpr double STEADY_LOSS_SHARE = 0.75;
  // A test below the hold halves the rate. A smaller step can land above the link still and show
  // nothing.
  static constexpr double RATE_TEST_STEP_DOWN = 0.5;
  // After a step up raised the loss, the next one waits this long, twice as long each time one does
  // again, up to RATE_TEST_RETRY_MAX.
  static constexpr std::chrono::steady_clock::duration RATE_TEST_RETRY = 30s;
  static constexpr std::chrono::steady_clock::duration RATE_TEST_RETRY_MAX = 120s;
  // Pressure gone this long ends a hold a test called not the bitrate's doing, as it starts ordinary
  // recovery.
  static constexpr auto RECOVERY_WAIT = 10s;

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
  // What set current_config.min_bitrate_kbps for this stream. Protected by state_mutex.
  static std::string floor_source = "adaptive_bitrate_min";
  // A manual live bitrate turned the controller's feedback off for this stream while the saved
  // preference, current_config.enabled, stayed on. Protected by state_mutex.
  static bool paused_for_stream = false;

  // Live Tuning's tests of a rate it holds for moderate loss, see update_network_stats(). All
  // protected by state_mutex.
  enum class rate_test_e {
    none,
    /// A step below the rate held for moderate loss, to see whether the loss falls.
    step_down,
    /// A step up from a held rate, kept only if the loss does not rise.
    step_up,
  };

  /// The client media reports heard at the rate Live Tuning holds now, each with its own loss.
  struct rate_evidence_t {
    int kbps = 0;
    std::chrono::steady_clock::time_point since {};
    /// The first report after a change covers time before it, so it does not count.
    bool skip_next = true;
    std::deque<double> loss_pct;
  };

  static rate_evidence_t rate_evidence;
  static rate_test_e rate_test = rate_test_e::none;
  // The rate a running test set, and the rate it returns to with the loss and steady share heard there.
  static int rate_test_kbps = 0;
  static int rate_test_return_kbps = 0;
  static double rate_test_return_loss_pct = 0.0;
  static double rate_test_return_steady_share = 0.0;
  // A test found the loss is not the bitrate's doing: moderate loss up to this figure holds the rate
  // instead of cutting it, and the rate climbs back by steps up.
  static std::optional<double> loss_not_the_bitrates_up_to_pct;
  // The lowest rate a test found the link loses frames at. Live Tuning climbs back toward it only by
  // steps up, and one that reaches it with the loss no higher forgets it.
  static int lossy_rate_kbps = 0;
  static std::chrono::steady_clock::duration step_up_wait = RATE_TEST_DWELL;
  // The base those findings were made against. A new base starts them over.
  static int rate_tests_base_kbps = 0;

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

  static void forget_rate_tests_locked() {
    rate_test = rate_test_e::none;
    rate_test_kbps = 0;
    loss_not_the_bitrates_up_to_pct.reset();
    lossy_rate_kbps = 0;
    step_up_wait = RATE_TEST_DWELL;
  }

  static void restart_rate_evidence_locked(int kbps, std::chrono::steady_clock::time_point now) {
    rate_evidence.kbps = kbps;
    rate_evidence.since = now;
    rate_evidence.skip_next = true;
    rate_evidence.loss_pct.clear();
  }

  static void note_rate_evidence_locked(double loss_pct) {
    if (rate_evidence.skip_next) {
      rate_evidence.skip_next = false;
      return;
    }
    rate_evidence.loss_pct.push_back(loss_pct);
    if (rate_evidence.loss_pct.size() > RATE_TEST_MAX_REPORTS) {
      rate_evidence.loss_pct.pop_front();
    }
  }

  static bool rate_evidence_complete(std::chrono::steady_clock::time_point now,
                                     std::chrono::steady_clock::duration dwell = RATE_TEST_DWELL) {
    return now - rate_evidence.since >= dwell && rate_evidence.loss_pct.size() >= RATE_TEST_MIN_REPORTS;
  }

  static double rate_evidence_loss_pct() {
    if (rate_evidence.loss_pct.empty()) {
      return 0.0;
    }
    double sum = 0.0;
    for (const double loss : rate_evidence.loss_pct) {
      sum += loss;
    }
    return sum / static_cast<double>(rate_evidence.loss_pct.size());
  }

  /// The share of the rate's reports that lost any frames.
  static double rate_evidence_steady_share() {
    if (rate_evidence.loss_pct.empty()) {
      return 0.0;
    }
    const auto lossy = std::count_if(rate_evidence.loss_pct.begin(), rate_evidence.loss_pct.end(), [](double loss) {
      return loss > 0.0;
    });
    return static_cast<double>(lossy) / static_cast<double>(rate_evidence.loss_pct.size());
  }

  /// Loss up to twice what a test heard, and a point more, is still the loss it heard.
  static double not_the_bitrates_bound(double loss_pct) {
    return loss_pct + std::max(LOSS_PRESSURE_PCT, loss_pct);
  }

  /// Start a test of to_kbps from from_kbps, remembering what was heard at from_kbps. Returns the rate
  /// the test runs at, or from_kbps when the bounds leave no step to take.
  static int start_rate_test_locked(rate_test_e test, int from_kbps, int to_kbps, int base) {
    const int clamped = clamp_target(to_kbps, base);
    if (clamped == from_kbps) {
      return from_kbps;
    }
    rate_test = test;
    rate_test_kbps = clamped;
    rate_test_return_kbps = from_kbps;
    rate_test_return_loss_pct = rate_evidence_loss_pct();
    rate_test_return_steady_share = rate_evidence_steady_share();
    return clamped;
  }

  // Moderate loss: a cut in proportion, no lower than half the base. At the hold the rate is tested once
  // it has been held long enough to judge. Steady loss there gets a step below, and loss that comes and
  // goes is what the hold is for.
  static int cut_for_moderate_loss_locked(std::chrono::steady_clock::time_point now, int current_target, int base) {
    if (loss_not_the_bitrates_up_to_pct &&
        (rate_evidence.loss_pct.size() < RATE_TEST_MIN_REPORTS || rate_evidence_loss_pct() <= *loss_not_the_bitrates_up_to_pct)) {
      set_controller_status("network_pressure", "packet_loss_holding");
      return current_target;
    }
    // Loss well past what a test found is new, and cut for as any other.
    loss_not_the_bitrates_up_to_pct.reset();
    const double reduction = current_config.max_change_rate * std::min(ewma_packet_loss / HEAVY_LOSS_PCT, 1.0);
    const int hold = static_cast<int>(base * MODERATE_LOSS_FLOOR_SHARE);
    const int new_target = std::max(static_cast<int>(current_target * (1.0 - reduction)), std::min(current_target, hold));
    if (new_target != current_target) {
      set_controller_status("network_pressure", "packet_loss");
      return new_target;
    }
    set_controller_status("network_pressure", "packet_loss_holding");
    if (!rate_evidence_complete(now) || rate_evidence_loss_pct() < LOSS_PRESSURE_PCT) {
      return current_target;
    }
    if (rate_evidence_steady_share() >= STEADY_LOSS_SHARE) {
      const int step = start_rate_test_locked(
        rate_test_e::step_down, current_target, static_cast<int>(current_target * RATE_TEST_STEP_DOWN), base
      );
      if (step != current_target) {
        set_controller_status("network_pressure", "packet_loss_testing");
        BOOST_LOG(debug) << "Adaptive bitrate: steady " << rate_test_return_loss_pct << "% loss at "
                         << current_target << " kbps, testing " << step << " kbps";
        return step;
      }
    }
    loss_not_the_bitrates_up_to_pct = not_the_bitrates_bound(rate_evidence_loss_pct());
    step_up_wait = RATE_TEST_DWELL;
    BOOST_LOG(debug) << "Adaptive bitrate: " << rate_evidence_loss_pct() << "% loss at " << current_target
                     << " kbps comes and goes, holding and testing steps up";
    return current_target;
  }

  static int judge_step_down_locked(std::chrono::steady_clock::time_point now, int current_target, int base) {
    set_controller_status("network_pressure", "packet_loss_testing");
    if (!rate_evidence_complete(now)) {
      return current_target;
    }
    const double loss = rate_evidence_loss_pct();
    const double steady_share = rate_evidence_steady_share();
    if (loss > rate_test_return_loss_pct / 2.0 || steady_share > rate_test_return_steady_share / 2.0) {
      // The step did not help, so the loss is not the bitrate's doing: back to the rate the test left.
      BOOST_LOG(debug) << "Adaptive bitrate: " << loss << "% loss at " << current_target << " kbps against "
                       << rate_test_return_loss_pct << "% at " << rate_test_return_kbps << " kbps, returning";
      rate_test = rate_test_e::none;
      loss_not_the_bitrates_up_to_pct = not_the_bitrates_bound(rate_test_return_loss_pct);
      step_up_wait = RATE_TEST_DWELL;
      set_controller_status("network_pressure", "packet_loss_holding");
      return rate_test_return_kbps;
    }
    // The step helped: the loss was the bitrate's doing. Live Tuning climbs back toward the rate it
    // left only by steps up that keep the loss down.
    BOOST_LOG(debug) << "Adaptive bitrate: " << loss << "% loss at " << current_target << " kbps against "
                     << rate_test_return_loss_pct << "% at " << rate_test_return_kbps << " kbps, keeping it";
    lossy_rate_kbps = rate_test_return_kbps;
    step_up_wait = RATE_TEST_DWELL;
    rate_test = rate_test_e::none;
    set_controller_status("network_pressure", "packet_loss");
    if (loss >= LOSS_PRESSURE_PCT && steady_share >= STEADY_LOSS_SHARE) {
      const int step = start_rate_test_locked(
        rate_test_e::step_down, current_target, static_cast<int>(current_target * RATE_TEST_STEP_DOWN), base
      );
      if (step != current_target) {
        set_controller_status("network_pressure", "packet_loss_testing");
      }
      return step;
    }
    return current_target;
  }

  static int judge_step_up_locked(std::chrono::steady_clock::time_point now, int current_target) {
    set_controller_status("recovering", "testing_a_step_up");
    if (!rate_evidence_complete(now)) {
      return current_target;
    }
    const double loss = rate_evidence_loss_pct();
    // Loss the test found is not the bitrate's doing counts as risen only well past what it heard. Where a
    // test found the bitrate was the cause, steady loss coming back is the mark of the link again.
    const bool rose = loss_not_the_bitrates_up_to_pct ?
      loss > *loss_not_the_bitrates_up_to_pct :
      loss >= LOSS_PRESSURE_PCT && rate_evidence_steady_share() >= STEADY_LOSS_SHARE;
    rate_test = rate_test_e::none;
    if (!rose) {
      step_up_wait = RATE_TEST_DWELL;
      if (lossy_rate_kbps > 0 && current_target >= lossy_rate_kbps) {
        lossy_rate_kbps = 0;
      }
      return current_target;
    }
    BOOST_LOG(debug) << "Adaptive bitrate: " << loss << "% loss at " << current_target << " kbps, back to "
                     << rate_test_return_kbps << " kbps";
    lossy_rate_kbps = lossy_rate_kbps > 0 ? std::min(lossy_rate_kbps, current_target) : current_target;
    step_up_wait = std::min(std::max(step_up_wait * 2, RATE_TEST_RETRY), RATE_TEST_RETRY_MAX);
    set_controller_status("network_pressure", "packet_loss");
    return rate_test_return_kbps;
  }

  // Where a test left a finding, the rate climbs back one step up at a time, each held long enough to
  // judge and kept only if the loss does not rise.
  static int start_step_up_locked(std::chrono::steady_clock::time_point now, int current_target, int base) {
    if (current_target >= base) {
      lossy_rate_kbps = 0;
      return current_target;
    }
    if (!rate_evidence_complete(now, step_up_wait) ||
        (loss_not_the_bitrates_up_to_pct && rate_evidence_loss_pct() > *loss_not_the_bitrates_up_to_pct)) {
      return current_target;
    }
    const int step = start_rate_test_locked(
      rate_test_e::step_up, current_target,
      static_cast<int>(current_target * (1.0 + current_config.max_change_rate * 0.4)), base
    );
    if (step != current_target) {
      set_controller_status("recovering", "testing_a_step_up");
    }
    return step;
  }

  void update_network_stats(std::optional<double> packet_loss_percent, double rtt_ms, bool loss_is_pressure) {
    if (!enabled.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
    if (doctor_override_active.load(std::memory_order_relaxed) ||
        pending_live_update_active.load(std::memory_order_relaxed)) {
      return;
    }
    auto now = std::chrono::steady_clock::now();
    // Live Tuning cuts only for loss the host's verdict calls pressure, so it hears any other report as
    // clean. The report's own figure still judges the rate it arrived at.
    const std::optional<double> heard_loss = packet_loss_percent ?
      std::optional<double> {loss_is_pressure ? *packet_loss_percent : 0.0} :
      std::nullopt;

    if (!initialized) {
      ewma_packet_loss = heard_loss.value_or(0.0);
      ewma_rtt = rtt_ms;
      avg_rtt = rtt_ms;
      rtt_sample_count = 1;
      last_adjustment_time = now;
      last_pressure_time = (heard_loss.value_or(0.0) > 0) ? now : (now - 10s);
      set_controller_status("steady", "warming_up");
      initialized = true;
      return;
    }

    const int held_target = target_bitrate_kbps.load(std::memory_order_relaxed);
    if (held_target != rate_evidence.kbps) {
      restart_rate_evidence_locked(held_target, now);
    }
    if (packet_loss_percent) {
      note_rate_evidence_locked(*packet_loss_percent);
    }

    // Update EWMA smoothed values. A reading with no loss figure moves only RTT: ten control pings a
    // second, each counted as clean video, diluted a real 7% frame loss to under a tenth of a percent
    // before Live Tuning could see it.
    double alpha = current_config.ewma_alpha;
    if (heard_loss) {
      ewma_packet_loss = alpha * *heard_loss + (1.0 - alpha) * ewma_packet_loss;
    }
    ewma_rtt = alpha * rtt_ms + (1.0 - alpha) * ewma_rtt;

    // Track long-term average RTT for spike detection
    if (rtt_sample_count < 1000) {
      rtt_sample_count++;
    }
    avg_rtt += (rtt_ms - avg_rtt) / rtt_sample_count;

    // Track when we last saw loss
    if (heard_loss.value_or(0.0) > 0.0) {
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
    if (rate_tests_base_kbps != base) {
      forget_rate_tests_locked();
      rate_tests_base_kbps = base;
    }
    if (rate_test != rate_test_e::none && current_target != rate_test_kbps) {
      // Something else moved the rate under the test, so there is nothing left to judge.
      rate_test = rate_test_e::none;
    }

    double max_change = current_config.max_change_rate;
    int new_target = current_target;

    // RTT spike detection: if current RTT is more than 2x the long-term average,
    // treat it as congestion and reduce bitrate immediately
    bool rtt_spike = (rtt_sample_count > 5) &&
                     (rtt_ms >= ACTIONABLE_RTT_MS) &&
                     (rtt_ms > avg_rtt * 2.0) &&
                     (avg_rtt > 0);

    const bool loss_pressure = ewma_packet_loss > LOSS_PRESSURE_PCT;
    const bool heavy_loss = ewma_packet_loss > HEAVY_LOSS_PCT;
    const bool network_pressure = loss_pressure || rtt_spike;
    if (rtt_spike || heavy_loss) {
      // Heavy loss is the mark of a link too small for the stream, and an RTT spike the mark of a
      // queue, so both cut at once, down to the floor, and end any test.
      forget_rate_tests_locked();
      double reduction_factor;
      if (rtt_spike && ewma_packet_loss <= LOSS_PRESSURE_PCT) {
        // RTT spike without packet loss: moderate reduction
        reduction_factor = 0.5 * max_change;
      } else if (heavy_loss) {
        // Heavy loss: maximum reduction
        reduction_factor = max_change;
      } else {
        // An RTT spike beside moderate loss: proportional reduction
        reduction_factor = max_change * std::min(ewma_packet_loss / HEAVY_LOSS_PCT, 1.0);
      }
      new_target = static_cast<int>(current_target * (1.0 - reduction_factor));
      set_controller_status("network_pressure", rtt_spike ? "rtt_spike" : "packet_loss");
    } else if (rate_test == rate_test_e::step_down) {
      new_target = judge_step_down_locked(now, current_target, base);
    } else if (rate_test == rate_test_e::step_up) {
      new_target = judge_step_up_locked(now, current_target);
    } else if (loss_pressure) {
      // Moderate loss, 5% or less with no RTT spike beside it. Replayed with each report's loss, the
      // Retroid Pocket 6's recorded HEVC run, about 2% lost in bursts every few seconds, took a
      // controller that cut on for it from 269 Mbps to 4 in a minute and on to the 2 Mbps floor: a
      // Wi-Fi burst loses the same frames at any rate. So the cuts stop at half the base and the hold
      // tests itself. Steady loss there, in most reports, gets a step below, and the steps go on only
      // while each lowers the loss; one that does not returns to the rate before it. Loss that comes and
      // goes, or that a step below did not lower, holds the rate, and after a while a step up is tried,
      // kept only if the loss does not rise.
      new_target = cut_for_moderate_loss_locked(now, current_target, base);
    }
    if (!rtt_spike && !heavy_loss && rate_test == rate_test_e::none && new_target == current_target &&
        (loss_not_the_bitrates_up_to_pct || lossy_rate_kbps > 0)) {
      new_target = start_step_up_locked(now, current_target, base);
    }

    if (network_pressure) {
      last_pressure_time = now;
    } else if (loss_not_the_bitrates_up_to_pct && now - last_pressure_time >= RECOVERY_WAIT) {
      // The loss a test said was not the bitrate's has gone.
      loss_not_the_bitrates_up_to_pct.reset();
    }
    if (!network_pressure && rate_test == rate_test_e::none && !loss_not_the_bitrates_up_to_pct &&
        lossy_rate_kbps == 0 && new_target == current_target) {
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
      }
    }

    if (new_target == current_target && controller_state != "network_pressure" && rate_test == rate_test_e::none) {
      set_controller_status("steady", "healthy");
    }

    const int clamped_target = clamp_target(new_target, base);
    if (clamped_target != current_target) {
      BOOST_LOG(debug) << "Adaptive bitrate: " << current_target << " -> " << clamped_target << " kbps ("
                       << controller_reason << ", loss=" << ewma_packet_loss << "%, rtt=" << ewma_rtt << "ms)";
      target_bitrate_kbps.store(clamped_target, std::memory_order_relaxed);
      ++action_authority_revision;
      restart_rate_evidence_locked(clamped_target, now);
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
                            double avg_frame_age_ms,
                            double target_fps) {
    if (!enabled.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard<std::mutex> lock(state_mutex);
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

  state_t get_state() {
    std::lock_guard<std::mutex> lock(state_mutex);
    state_t state;
    state.session_generation = scope_generation;
    state.app_session_id = scope_app_session_id;
    state.session_exclusive = scope_exclusive;
    state.configured_enabled = current_config.enabled;
    state.feedback_initialized = initialized;
    state.applied_bitrate_kbps = encoder_applied_bitrate_kbps;
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
    state.floor_source = floor_source;
    state.paused_for_stream = paused_for_stream;
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
      action_authority_revision,
      operator_revision,
    };
  }

  static doctor_bitrate_apply_result_t apply_doctor_bitrate_if_revision_locked(
      std::uint64_t expected_revision,
      int target,
      std::optional<int> temporary_max_bitrate_kbps,
      bool require_quality_policy_clear) {
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

  bool apply_live_bitrate_request(const live_bitrate_request_t &request,
                                  const std::function<bool()> &apply) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (request.revision != operator_revision ||
        request.target_bitrate_kbps != target_bitrate_kbps.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed)) return false;
    if (apply()) {
      encoder_applied_bitrate_kbps = request.target_bitrate_kbps;
      last_confirmed_bitrate_kbps = request.target_bitrate_kbps;
      encoder_applied_revision = request.revision;
      encoder_applied_at = std::chrono::steady_clock::now();
      pending_live_update_active.store(false, std::memory_order_relaxed);
      state_changed.notify_all();
    }
    return true;
  }

  void set_session_scope(std::uint64_t generation, const std::string &app_session_id,
                         bool exclusive) {
    std::lock_guard<std::mutex> lock(state_mutex);
    scope_generation = generation;
    scope_app_session_id = app_session_id;
    scope_exclusive = exclusive;
  }

  bool begin_live_bitrate_session_recreation(
      std::uint64_t revision,
      int bitrate_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (revision != operator_revision ||
        bitrate_kbps != target_bitrate_kbps.load(std::memory_order_relaxed) ||
        !runtime_update_supported.load(std::memory_order_relaxed)) {
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
    state_changed.notify_all();
    return true;
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
    last_confirmed_bitrate_kbps = bitrate_kbps;
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
    apply_live_bitrate_locked(kbps);
  }

  void set_live_bitrate_for_stream(int kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
    // Only a stream that had Live Tuning running has anything to put back when it ends.
    paused_for_stream = paused_for_stream || enabled.load(std::memory_order_relaxed);
    enabled.store(false, std::memory_order_relaxed);
    apply_live_bitrate_locked(kbps);
  }

  void end_stream_override() {
    std::lock_guard<std::mutex> lock(state_mutex);
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
    }
    state_changed.notify_all();
  }

  void set_runtime_enabled(bool enable) {
    std::lock_guard<std::mutex> lock(state_mutex);
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
                                    int initial_encoder_bitrate_kbps) {
    std::lock_guard<std::mutex> lock(state_mutex);
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
    } else {
      encoder_applied_bitrate_kbps = 0;
      encoder_applied_revision = 0;
      encoder_applied_at = {};
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

#ifdef POLARIS_TESTS
  void age_for_tests(std::chrono::steady_clock::duration age) {
    std::lock_guard<std::mutex> lock(state_mutex);
    last_adjustment_time -= age;
    last_pressure_time -= age;
    rate_evidence.since -= age;
  }
#endif

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
    forget_rate_tests_locked();
    rate_evidence = rate_evidence_t {};
    rate_tests_base_kbps = 0;
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
    state_changed.notify_all();
  }

}  // namespace adaptive_bitrate
