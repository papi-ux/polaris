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
 *   than 2.5 s, or the first after any rate change, is not judged because it covers the previous rate.
 * - A CUSUM against a slow running baseline (its level and its burstiness) detects a change in loss, and a probe
 *   also needs two of the last three reports to lose 3% of their frames or more. Loss under 3% never starts
 *   one, and neither does one burst. Reports keep teaching the baseline while a change is only a candidate.
 *   An actual cut restores the level and burstiness from before that candidate; if the detector clears,
 *   the learned baseline stays. A skipped report gap alone does not resolve the candidate.
 *   A due hold retry or climb waits at most one judged duty window for a new candidate; outside writers own their changes.
 * - A probe steps to 0.6x. If that helped, it rechecks the rate it left (the loss may simply have ended) and
 *   climbs back or bisects. If not, it takes one deep sample at max(2x floor, start/8). Clean there: the link
 *   is in between, so it rechecks and bisects. Loss there that is mild, or no lighter than at the first step and
 *   not clearly over the 10% background can cost: the loss does not depend on the rate. Loss that is lighter there
 *   but not mild, or clearly over 10% whatever the first step lost: it halves on, until halving no longer lightens
 *   it. A sample at the floor has nowhere to halve on to, and is the link's only when clearly over 20%.
 *   When reports stop before the first step is judged, perhaps because the link is overloaded, the probe takes
 *   its deep sample after 1.5 s, once, and goes no further until reports return. A return to normal that no
 *   judged report has covered yet is taken back to the rate it left when reports stop for 2.5 s, as an unjudged
 *   raise is, and tried again three reports after they return.
 * - A few light reports between clustered losses can pass for help far down. A help at or below a deep sample is
 *   therefore checked while the rates above it keep failing. If its verification fails and nothing measured above
 *   it lost significantly more, it was chance: the loss does not depend on the rate. If the verification and then
 *   two bisect steps fail with no clean step between, and the helped rate lost frames itself and was not lighter
 *   beyond doubt (four standard errors), that rate is sampled again for exactly six judged reports, a fixed
 *   sample and not a sequential test, and judged alone against the rate it was verified against. Significantly
 *   lighter again: the search carries on, and that help is not sampled a third time in this descent. If not, the
 *   first sample is dropped, the loss does not depend on the rate, and the stream goes back without consulting
 *   the pooled measurements that called for the sample.
 * - While a learned background backs off and has held at the request for twelve reports, a new probe's first step
 *   that is not helped, not heavy, and not clearly over that background plus 2% has found it again: it raises the
 *   background by at most 2%, as a run that close does, without another deep sample and without starting the
 *   back-off again.
 * - A probe step is judged at the whole rate it would set. A fresh descent's first step, with nothing measured
 *   above it, takes the floor when it would reach it, as it always did: a request close over its floor steps
 *   onto it. Within an episode, a probe that would reach the floor stops a climb step above it first, or halfway
 *   to the floor from within a bisect step of that stop, where so short a step tells nothing, and takes
 *   the floor only for loss clearly over 20%, or loss that still falls with the rate there: the nearest rate
 *   above lost significantly more. With no room left for its deep sample, a first step under clearly heavy loss
 *   samples the floor that way before the rate is judged where it is. A probe step at most a climb step over the
 *   floor that helped but still loses clearly over 10%, more than background under 3% costs there, samples the
 *   floor before it can bound a hold, and so does a loss hold there whose own twelve reports do.
 * - Every judged report since the stream left its request is kept by rate. When a hold that lost at most 3% there,
 *   over twelve reports or more, starts losing significantly more at the same rate, the link has changed: the
 *   episode starts again from that hold, keeping only the higher rates that lost significantly more than it now
 *   does. So does a hold clean over fewer reports whose probe started on a link's loss, over 20% and clearly over
 *   the 10% background can cost (#659). A rate-independent result goes back to where the episode started, learns
 *   that loss as background, and does not probe again at that level for 1200 reports, only if no higher rate
 *   measured in the episode lost significantly more. Otherwise the loss depends on the rate above: the stream holds
 *   at the highest measured rate that lost no more, under the lowest one that did, but never at or above a hold the
 *   episode restarted from on a link's loss (#659). When that lowest rate is at or above such a hold, nothing above
 *   is known on the new link: the stream climbs back in judged steps of at most 1.5x rather than retrying a rate far
 *   above. Loss over 20% is learned only once the episode has looked
 *   under it, at the floor or at a rate that lost 20% or less: until then the floor is sampled first. At the floor
 *   itself, loss in the evidence for the verdict (the test that ended there, a hold's own reports, or the run that
 *   started a probe with no room under it) that is over 20% and clearly over the 10% background can cost there is
 *   never learned: it is the link's, and the stream holds at the floor with nothing above it known to fail, so it
 *   climbs back only in judged steps of at most 1.5x. One report a little over 20% is not clearly over 10%, and goes
 *   back as before. A background that has gone is judged only on reports after
 *   it was learned.
 * - "Did this help" and "is this step lossy" are sequential probability ratio tests on frame counts, scaled by
 *   the measured burstiness. An undecided descent test counts as not having helped.
 * - A hold retries the rate above it after 8 reports, then every 20. Loss ceilings climb from there when
 *   clean; queue ceilings use the staircase below. A clean step within 25% under that rate retries it at once.
 *   While a hold's loss is not significantly lower than real loss at the rate above it, the loss is
 *   rate-independent after all. A hold whose own loss stays clearly over 20% probes under it.
 *   After a hold fails, clean verify/bisect promotions need the existing four-report queue dwell until
 *   a rate at or above that failed hold passes. A higher queue ceiling survives a loss descent from its
 *   lower hold: verifying the lower rate resumes that queue bracket instead of retiring the higher bound.
 *   Ordinary climbs keep their frame test without this dwell.
 *
 * Queues
 * - A raise fails when the smoothed RTT rises at two changes in a row to a raised level, 10 ms or half the
 *   minute's floor over that floor, whichever is more, or is raised and climbing 2 ms/s for 2 s.
 * - A held rate cuts 15% when the smoothed RTT has been raised and not falling for 8 s. A Wi-Fi background
 *   scan only raises it for about 5 s before it falls: a Steam Deck's scans peak at 83 ms for 5 s, every
 *   306.9 s. Cuts go on every 6 s until the queue starts to drain, at most three; the rate that queued is then
 *   retried ceiling above the hold, capped at one existing 1.06x bracket-completion step. A queue ceiling
 *   verified clean becomes a real hold before the next completion-size probe, while below 1.25x the first
 *   verified queue rate: a queue at the link builds slower than a retry can see. Consecutive failed retries
 *   in a closed queue bracket wait 20, then 40 reports, doubling up to the existing 1200-report back-off.
 *   Clean verification, a new descent, a cut or an outside writer resets that queue-only back-off.
 *   A queue three cuts did not drain, with nothing lost meanwhile, is someone else's, and is left alone
 *   for 5 minutes.
 * - Heavy loss while cutting for a queue hands the stream to the loss probe.
 *
 * FEC
 * - Unconfigured by default, which leaves every other output as it was. A trusted new scope configures the
 *   stream's FEC percentage; from 0 to 49 the policy may ask for more parity, up to 50, and 50 or more stays fixed.
 * - Only a rate-independent result asks, once: up by max(5, F/2). The actuator publishes the percentage, or refuses;
 *   the policy never counts a request as applied. After a raise is published, the first report is not judged, and
 *   the descent's "did this help" test on post-FEC frames against the learned background keeps or reverts it.
 * - Either outcome, and a refusal, starts the 1200-report back-off; a kept raise then asks for the configured
 *   percentage again. A new probe, an outside writer or a new request asks for the configured percentage at once.
 *   Like every loss decision, this counts reports, so RTT and a report pause change nothing. The rate decisions
 *   never read any of it.
 *
 * tests/unit/test_live_tuning_policy.cpp pins this to its reference model, second for second, and replays two
 * RP6 loss runs and two release candidates' RP6 cap steps to 6.5 Mbit/s (#228, #659);
 * tests/unit/test_live_tuning_acceptance.cpp to the frozen acceptance bench: 22 of its 26 cells pass, and the four
 * that need a congestion signal while reports stop stay named as unmet. Under its wire layer,
 * none of 40 seeded 400 s runs at a 25987 kbps request and a 2000 kbps floor reaches the floor, with whole sends
 * dropped, the pacer off or on, or single packets dropped, at 1, 2 and 2.9%. That holds for those modelled runs,
 * not for other requests, floors, clients or loss. Over 141 synthetic links close to the floor and the 5111 static
 * links of a wider search it does no worse than the policy at staging 457eeef9, but for two named rows: floor holds
 * reached 6 s later, with one retry more in the judged window. With the pacer off, over 400 seeded 1200 s runs at
 * each of those three loss levels, it spends under 1% of the time below half its request; over 1000 seeds of a second
 * wire model the 1% level reads 1.07%, and over 1000 other seeds both models read 1.00% there.
 */
#pragma once

#include <cstdint>
#include <deque>
#include <map>
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
    hold,  ///< At a selected rate, retrying the one above it now and then.
    climb,  ///< Raising toward the request on clean reports.
    rtt_cut,  ///< Cutting until a standing queue drains.
    native_hold,  ///< Experimental native downward request; no automatic exit or legacy judgments.
  };

  std::string_view name(state_e state);

  /** Sequential probability ratio test on frame counts. H0: loss rate p0. H1: loss rate p1. */
  class sprt_t {
  public:
    sprt_t(double p0, double p1, double dispersion, int min_reports, std::optional<int> undecided);

    /// 1 for H1, 0 for H0, nullopt while undecided.
    std::optional<int> add(double expected, double lost, double weight);
    double rate() const;
    double reports() const {
      return n_;
    }
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

    /// Host time passed with no reading: a raise, or a return to the request, that no report can judge is taken back.
    void tick(std::int64_t now_ms);

    /// Dedicated downward-only entry. No legacy sample, failed hold or learned baseline is cleared.
    /// Missing/flat native or ENet input cannot resume adaptation. An outside writer retains its
    /// existing adopt/rebase authority; a same-target writer leaves this conservative hold intact.
    bool native_downward(std::int64_t now_ms, int expected_kbps, int next_kbps);

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

    /// The rate above the hold to retry, if known: a lossy/queued rate, or the next completion-size
    /// queue probe, without measured loss assigned to it.
    std::optional<int> ceiling_kbps() const {
      return hi_;
    }
    /// Whether that ceiling belongs to the queue search rather than a loss measurement.
    bool ceiling_is_queue() const {
      return hi_is_queue_;
    }

    /// A trusted new scope's configured FEC percentage. Outside 0 to 255 it leaves FEC unconfigured.
    void configure_fec(int configured_percentage);

    /// The nominal FEC percentage the policy asks the actuator to publish. Not applied until fec_applied().
    std::optional<int> fec_request() const;

    /// The last FEC percentage the actuator confirmed it published; 0 while unconfigured.
    int fec_percentage() const {
      return fec_pct_;
    }

    /// The actuator published this percentage at a frame boundary. A percentage the policy did not ask for is
    /// ignored, except the configured one, which is always taken back.
    void fec_applied(std::int64_t now_ms, int percentage);

    /// The actuator could not act on the request. The confirmed percentage holds, and nothing is asked again until
    /// the back-off ends. A soft evidence/ACK wait expiry may clear the
    /// request without starting a parity-outcome backoff.
    void fec_refused(std::int64_t now_ms, bool backoff = true);

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
    void clear_loss_run(bool restore_baseline);
    void note_baseline(double expected, double lost);
    void reset_baseline(double loss);
    void set_target(double kbps, std::string_view why, std::int64_t now_ms);
    void to_normal(std::int64_t now_ms, int kbps, std::string_view why);
    void rate_independent(double rate, std::int64_t now_ms, std::string_view why);
    void learn_background(double rate, std::int64_t now_ms, std::string_view why);
    void hold_at_floor(std::int64_t now_ms);
    std::optional<int> lossier_above(int kbps, std::pair<double, double> &above) const;
    void note_measured(int kbps, double expected, double lost);
    bool clearly_over(double lost, double frames, double level) const;
    std::optional<int> loss_step(double kbps, double lost, double frames) const;
    double probe_rate() const;
    void sample_floor(std::int64_t now_ms, std::pair<double, double> here);
    void far_down_reset();
    void far_down_resample_result(std::int64_t now_ms);

    void begin_descent(std::int64_t now_ms, std::string_view why, double l0);
    void descend_result(bool helped, std::int64_t now_ms);
    void verify_hi_result(bool lossy, std::int64_t now_ms);
    void bisect_step(std::int64_t now_ms);
    void bisect_result(bool lossy, std::int64_t now_ms);
    void clean_promotion();
    void queue_hold(std::int64_t now_ms, int kbps);
    void enter_hold(std::int64_t now_ms, int kbps);
    void climb_step(std::int64_t now_ms);
    void climb_result(bool lossy, std::int64_t now_ms);
    void step_result(bool lossy, std::int64_t now_ms);
    void watch(std::int64_t now_ms, double expected, double lost);

    bool raising() const;
    void close_second(std::int64_t now_ms);
    void rtt_cut(std::int64_t now_ms);

    void fec_open_episode();
    void fec_revert();
    void fec_report(double expected, double lost, bool first_after_raise);

    int base_;
    int floor_;
    int target_;
    state_e state_ = state_e::normal;
    std::uint64_t changes_ = 0;
    change_t last_change_;

    // Loss evidence.
    after_change_e after_change_ = after_change_e::none;
    std::optional<std::int64_t> last_report_ms_;
    // The rate a return to normal left, until a judged report covers the rate it went back to.
    std::optional<int> unjudged_return_from_;
    double background_ = 0.0;
    // Judged reports at the request since the background was last learned.
    int background_reports_ = 0;
    int backoff_ = 0;
    double cusum_ = 0.0;
    std::vector<std::pair<double, double>> run_;
    std::deque<bool> duty_;
    double base_loss_ = 0.0;
    double base_m2_ = 0.0;
    double base_disp_ = 1.0;
    int base_n_ = 0;
    int disp_n_ = 0;
    struct baseline_t {
      double loss;
      double m2;
      double dispersion;
      int reports;
      int dispersion_reports;
    };
    std::optional<baseline_t> baseline_before_run_;
    std::pair<double, double> normal_stats_ {0.0, 0.0};
    int normal_reports_ = 0;

    // Search.
    // Judged reports by rate since the stream left its request: what the rate-independent verdict must agree with.
    std::map<int, std::pair<double, double>> measured_;
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
    // A failed hold must survive a full upward dwell before it can erase its loss ceiling.
    std::optional<int> hold_failed_kbps_;
    // The lowest hold this episode restarted from on a link's loss: the link fell under it. Until a rate there passes.
    std::optional<int> link_fell_under_;
    // A queue ceiling displaced by a loss probe; proving its lower hold clean cannot retire it.
    std::optional<int> retained_queue_ceiling_;
    // The validated lower queue hold from which this staircase hold was promoted.
    std::optional<int> queue_hold_below_;
    struct failed_queue_hold_t {
      int failed;
      int fallback;
      int losses;
      bool verifying;
    };
    // A measured hold failure survives its loss probe's short same-rate verification.
    std::optional<failed_queue_hold_t> failed_queue_hold_;
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
    // Consecutive closed queue-bracket failures; saturates once well beyond the capped retry wait.
    int queue_retry_fails_ = 0;
    bool blind_step_ = false;
    // A help at or below a deep sample is checked again while the rates above it keep failing: the rate it came at
    // and the rate it was verified against, the lossy bisect steps since, and whether the second sample is running
    // or has been taken.
    std::optional<int> far_down_lo_;
    int far_down_hi_ = 0;
    int far_down_lossy_steps_ = 0;
    bool far_down_verifying_ = false;
    bool far_down_resampling_ = false;
    bool far_down_resampled_ = false;
    bool hi_rechecked_ = false;
    // First clean queue rate anchoring the staircase's existing recheck radius across real holds.
    std::optional<int> queue_continuation_from_;

    // RTT.
    std::deque<double> rtt_min_;
    std::optional<std::int64_t> sec_;
    std::vector<double> sec_vals_;
    std::deque<double> sec_meds_;
    int standing_run_ = 0;
    int building_run_ = 0;
    std::optional<double> last_rtt_;
    int rises_ = 0;
    int rtt_suppress_ = 0;
    int rtt_cut_wait_ = 0;
    int rtt_cuts_ = 0;
    bool rtt_cut_lossy_ = false;
    int rtt_from_;
    int rtt_start_;

    // FEC, in nominal percent. Nothing here is read by the rate decisions above.
    std::optional<int> fec_configured_;
    int fec_pct_ = 0;
    std::optional<int> fec_wanted_;
    std::optional<sprt_t> fec_test_;
    int fec_backoff_ = 0;
    bool fec_skip_ = false;
  };

}  // namespace live_tuning_policy
