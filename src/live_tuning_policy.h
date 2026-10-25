/**
 * @file src/live_tuning_policy.h
 * @brief Live Tuning's bitrate decisions from media loss and the control channel's RTT.
 *
 * The policy is clock-free and owns no locks: the caller passes host steady-clock milliseconds with each
 * observation and reads the target back. adaptive_bitrate.cpp runs one policy per stream under its own lock.
 *
 * Evidence
 * - Media reports, about one a second: frames expected and frames lost. Lost frames are frames FEC could not
 *   save.
 * - RTT readings, about ten a second: the host's ENet-smoothed RTT, the only RTT ENet keeps. It moves 1/8 of the
 *   way to a new sample, rounded up to a whole millisecond, with each acknowledgement, and the host cannot tell
 *   how many came between two readings, so the policy judges the smoothed value alone.
 *
 * Loss
 * - Decisions count reports, never wall time, so a report pause freezes them. A report after a gap of more
 *   than 2.5 s, or the first after a cut, is not judged; the first after a raise counts at half weight.
 * - A CUSUM against a slow running baseline (its level and its burstiness) detects a change in loss, and a probe
 *   also needs two of the last three reports to lose 3% of their frames or more. Loss under 3% never starts
 *   one, and neither does one burst.
 * - A probe steps to 0.6x. If that helped, it rechecks the rate it left (the loss may simply have ended) and
 *   climbs back or bisects. If not, it takes one deep sample at max(2x floor, start/8). Clean there: the link
 *   is in between, so it rechecks and bisects. Loss there that is not clearly heavy: the loss does not depend
 *   on the rate, so the stream goes back to where the episode started, learns that loss as background, and
 *   does not probe again at that level for 1200 reports. Clearly heavy loss there: it halves on to the floor.
 *   When reports stop before the first step is judged, perhaps because the link is overloaded, the probe takes
 *   its deep sample after 1.5 s, once, and goes no further until reports return.
 * - "Did this help" and "is this step lossy" are sequential probability ratio tests on frame counts, scaled by
 *   the measured burstiness. An undecided descent test counts as not having helped.
 * - A hold retries the rate above it after 8 reports, then every 20, and climbs from there when it is clean. A
 *   clean step within 25% under that rate retries it at once. While a hold's loss is not significantly lower
 *   than real loss at the rate above it, the loss is rate-independent after all.
 *
 * Queues
 * - A raise fails when the smoothed RTT is raised (10 ms, or half the minute's floor) and climbing 2 ms/s for 2 s.
 * - A held rate cuts 15% when the smoothed RTT has been raised and not falling for 8 s. A Wi-Fi background
 *   scan only raises it for about 5 s before it falls: a Steam Deck's scans peak at 83 ms for 5 s, every
 *   306.9 s. Cuts go on every 6 s until the queue starts to drain, at most three; the rate that queued is then
 *   the ceiling above the hold, and the first step past it, once it holds clean, is 1.1x: a queue at the link
 *   builds slower than a retry can see. A queue three cuts did not drain, with nothing lost meanwhile, is
 *   someone else's, and is left alone for 5 minutes.
 * - Heavy loss while cutting for a queue hands the stream to the loss probe.
 *
 * tests/unit/test_live_tuning_policy.cpp pins this to its reference model, second for second, and
 * tests/unit/test_live_tuning_acceptance.cpp to the frozen acceptance bench: 22 of its 26 cells pass, and the
 * four that need a congestion signal while reports stop stay named as unmet.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace live_tuning_policy {

  /** Where the policy is. */
  enum class state_e {
    normal,  ///< At the client's request, or climbing back to it on clean reports.
    descend,  ///< Probing down to see whether a lower rate helps.
    verify_hi,  ///< Back at the rate a probe or a hold left, to see whether it is still lossy.
    bisect,  ///< Narrowing in between a clean rate and a lossy one.
    hold,  ///< At the highest rate known to be clean, retrying the one above it now and then.
    climb,  ///< Raising toward the request on clean reports.
    rtt_cut,  ///< Cutting until a standing queue drains.
  };

  std::string_view name(state_e state);

  /** Sequential probability ratio test on frame counts. H0: loss rate p0. H1: loss rate p1. */
  class sprt_t {
  public:
    sprt_t(double p0, double p1, double dispersion, int min_reports, std::optional<int> undecided);

    /// 1 for H1, 0 for H0, nullopt while undecided.
    std::optional<int> add(double expected, double lost, double weight);
    double rate() const;
    double frames() const {
      return frames_;
    }
    double lost() const {
      return lost_;
    }

  private:
    double p0_;
    double p1_;
    double a_;
    double b_;
    double dispersion_;
    int min_reports_;
    int max_reports_;
    std::optional<int> undecided_;
    double v_ = 0.0;
    double n_ = 0.0;
    double frames_ = 0.0;
    double lost_ = 0.0;
  };

  /** One change of the target, for Live Tuning's history. */
  struct change_t {
    std::int64_t at_ms = 0;
    int from_kbps = 0;
    int to_kbps = 0;
    std::string_view why;
  };

  class policy_t {
  public:
    policy_t(int base_kbps, int floor_kbps);

    /// One media report at host time now_ms: frames expected and frames lost since the last one.
    void report(std::int64_t now_ms, double expected, double lost);

    /// One reading of the ENet-smoothed RTT at host time now_ms. It also advances time, as tick() does.
    void rtt(std::int64_t now_ms, double smoothed_rtt_ms);

    /// Host time passed with no reading: a raise no report can judge is taken back.
    void tick(std::int64_t now_ms);

    /// An outside writer (encoder load, Doctor, a manual bitrate) set the rate: carry on from it.
    void adopt(std::int64_t now_ms, int kbps);

    /// The client's request or the floor changed. The target is clamped to them.
    void rebase(int base_kbps, int floor_kbps);

    int target_kbps() const {
      return target_;
    }
    int base_kbps() const {
      return base_;
    }
    int floor_kbps() const {
      return floor_;
    }
    state_e state() const {
      return state_;
    }

    /// Loss the policy learned does not depend on the rate, as a share of frames.
    double background_loss() const {
      return background_;
    }

    /// Target changes so far; the latest is last_change().
    std::uint64_t changes() const {
      return changes_;
    }
    const change_t &last_change() const {
      return last_change_;
    }

    /// The rate above the hold that was lossy or queued, if one is known.
    std::optional<int> ceiling_kbps() const {
      return hi_;
    }
    /// What found that ceiling: loss, or a standing queue.
    bool ceiling_is_queue() const {
      return hi_is_queue_;
    }

  private:
    enum class after_change_e {
      none,
      up,
      down,
    };

    double disp() const;
    sprt_t make_sprt(double p0, double p1, int min_reports = 0, std::optional<int> undecided = std::nullopt) const;
    int upward_min() const;
    double p_background() const;
    void note_baseline(double expected, double lost);
    void reset_baseline(double loss);
    void set_target(double kbps, std::string_view why, std::int64_t now_ms);
    void to_normal(std::int64_t now_ms, int kbps, std::string_view why);
    void rate_independent(double rate, std::int64_t now_ms, std::string_view why);
    double probe_rate() const;

    void begin_descent(std::int64_t now_ms, std::string_view why, double l0);
    void descend_result(bool helped, std::int64_t now_ms);
    void verify_hi_result(bool lossy, std::int64_t now_ms);
    void bisect_step(std::int64_t now_ms);
    void bisect_result(bool lossy, std::int64_t now_ms);
    void enter_hold(std::int64_t now_ms, int kbps);
    void climb_step(std::int64_t now_ms);
    void climb_result(bool lossy, std::int64_t now_ms);
    void step_result(bool lossy, std::int64_t now_ms);
    void watch(std::int64_t now_ms, double expected, double lost);

    bool raising() const;
    void close_second(std::int64_t now_ms);
    void rtt_cut(std::int64_t now_ms);

    int base_;
    int floor_;
    int target_;
    state_e state_ = state_e::normal;
    std::uint64_t changes_ = 0;
    change_t last_change_;

    // Loss evidence.
    after_change_e after_change_ = after_change_e::none;
    std::optional<std::int64_t> last_report_ms_;
    double background_ = 0.0;
    int backoff_ = 0;
    double cusum_ = 0.0;
    std::vector<std::pair<double, double>> run_;
    std::deque<bool> duty_;
    double base_loss_ = 0.0;
    double base_m2_ = 0.0;
    double base_disp_ = 1.0;
    int base_n_ = 0;
    int disp_n_ = 0;
    std::pair<double, double> normal_stats_ {0.0, 0.0};
    int normal_reports_ = 0;

    // Search.
    int episode_start_;
    int r0_;
    double l0_ = 0.0;
    enum class phase_e {
      first,
      deep,
      down,
    } phase_ = phase_e::first;
    std::optional<int> hi_;
    std::optional<int> lo_;
    double hi_loss_ = 0.0;
    double lo_loss_ = 0.0;
    bool hi_is_queue_ = false;
    std::pair<double, double> hi_stats_ {0.0, 0.0};
    std::pair<double, double> probe_stats_ {0.0, 0.0};
    std::pair<double, double> deep_stats_ {0.0, 0.0};
    std::pair<double, double> hold_stats_ {0.0, 0.0};
    int hold_reports_ = 0;
    std::optional<sprt_t> test_;
    int retry_wait_ = 0;
    int retries_ = 0;
    bool blind_step_ = false;
    bool hi_rechecked_ = false;
    bool gentle_climb_ = false;

    // RTT.
    std::deque<double> rtt_min_;
    std::optional<std::int64_t> sec_;
    std::vector<double> sec_vals_;
    std::deque<double> sec_meds_;
    int standing_run_ = 0;
    int building_run_ = 0;
    int rtt_suppress_ = 0;
    int rtt_cut_wait_ = 0;
    int rtt_cuts_ = 0;
    bool rtt_cut_lossy_ = false;
    int rtt_from_;
    int rtt_start_;
  };

}  // namespace live_tuning_policy
