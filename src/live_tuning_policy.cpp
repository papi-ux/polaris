/**
 * @file src/live_tuning_policy.cpp
 * @brief Live Tuning's bitrate decisions from media loss and the control channel's RTT.
 */
#include "live_tuning_policy.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace live_tuning_policy {

  namespace {

    constexpr double MIN_LOSS = 0.002;  // the smallest loss rate any test assumes
    constexpr double REACT_EXCESS = 0.015;  // loss this far over the expected level is worth a probe
    constexpr double MILD_LOSS = 0.045;  // deep-sample loss not clearly this far over the background is rate-independent
    constexpr double HEAVY_LOSS = 0.2;  // loss this heavy is the link's, whatever a lower rate shows
    // Background under 3% on the wire costs about three frames a loss: at most about 9% of frames near the floor,
    // where the RP6 measured 4.4-6.3% at 2-4 Mbps. Loss clearly over this there is more than background; a hold's
    // twelve reports of 9% at burstiness 3 are clearly over only about 6.3%.
    constexpr double NEAR_FLOOR_LOSS = 0.1;
    constexpr double ABSORB_EXCESS = 0.02;  // while backing off, loss this close to the background only updates it
    constexpr double CLIMB_LOSSY = 0.03;  // a raise is lossy at this much loss over its floor
    constexpr double FIRST_STEP = 0.6;
    constexpr double STEP = 0.5;
    constexpr double PROBE_DEPTH = 8.0;
    constexpr double BISECT_DONE = 1.06;
    constexpr double RECHECK_CEILING = 1.25;  // a clean step this close under the ceiling rechecks the ceiling
    constexpr double CLIMB_STEP = 1.25;
    constexpr double CLIMB_FAR = 1.5;
    constexpr int RETRY_REPORTS = 20;
    constexpr int FIRST_RETRY_REPORTS = 8;  // the first retry after a hold begins, for a link that recovered meanwhile
    constexpr double LOSSY_MIN = 0.03;  // loss under 3% never starts a probe: the bench's line for background loss
    constexpr int DUTY_WINDOW = 3;  // a probe needs two lossy reports among the last three
    constexpr int DUTY_NEEDED = 2;
    constexpr std::int64_t MISSING_MS = 1500;  // no report this long while probing down: step to the deep sample
    constexpr int HOLD_CHECK_REPORTS = 12;
    constexpr int BACKOFF_REPORTS = 1200;
    constexpr int BACKGROUND_DECAY_REPORTS = 60;
    constexpr double CUSUM_H = 4.0;
    constexpr int BASELINE_REPORTS = 40;
    constexpr int WARMUP_REPORTS = 10;
    constexpr std::int64_t REPORT_GAP_MS = 2500;
    constexpr std::int64_t PAUSE_TAKE_BACK_MS = 2500;
    constexpr int QUEUE_RETRY_REPORTS = 4;
    constexpr std::size_t RTT_WINDOW_S = 60;
    constexpr int RTT_STANDING_S = 8;
    constexpr double RTT_BUILD_MS_S = 2.0;
    constexpr int RTT_BUILD_FAIL_S = 2;
    constexpr int RTT_RISES = 2;
    constexpr int RTT_DRAIN_S = 6;
    constexpr double RTT_CUT = 0.85;
    constexpr int RTT_MAX_CUTS = 3;
    constexpr int RTT_SUPPRESS_S = 300;
    constexpr std::size_t SEC_MEDS = 4;
    constexpr int FEC_MAX = 50;  // the most parity the policy asks for
    constexpr int FEC_MIN_STEP = 5;  // a raise is max(5, F/2)

    double llr(double expected, double lost, double p0, double p1, double dispersion) {
      return (lost * std::log(p1 / p0) - expected * (p1 - p0)) / dispersion;
    }

    /// How many standard errors rate a sits under rate b, allowing for bursty loss.
    double z_lower(double a_lost, double a_frames, double b_lost, double b_frames, double dispersion) {
      if (a_frames <= 0.0 || b_frames <= 0.0) {
        return 0.0;
      }
      const double pa = a_lost / a_frames;
      const double pb = b_lost / b_frames;
      const double p = (a_lost + b_lost) / (a_frames + b_frames);
      const double se = std::sqrt(std::max(p, 1e-4) * dispersion * (1.0 / a_frames + 1.0 / b_frames));
      return (pb - pa) / se;
    }

    template<class T>
    void push_bounded(std::deque<T> &values, T value, std::size_t bound) {
      values.push_back(value);
      while (values.size() > bound) {
        values.pop_front();
      }
    }

  }  // namespace

  std::string_view name(state_e state) {
    switch (state) {
      case state_e::normal:
        return "normal";
      case state_e::descend:
        return "descend";
      case state_e::verify_hi:
        return "verify_hi";
      case state_e::bisect:
        return "bisect";
      case state_e::hold:
        return "hold";
      case state_e::climb:
        return "climb";
      case state_e::rtt_cut:
        return "rtt_cut";
    }
    return "normal";
  }

  sprt_t::sprt_t(double p0, double p1, double dispersion, int min_reports, std::optional<int> undecided):
      p0_ {std::max(p0, 1e-3)},
      p1_ {std::max(p1, 2e-3)},
      a_ {std::log((1.0 - 0.05) / 0.05)},
      b_ {std::log(0.05 / (1.0 - 0.05))},
      dispersion_ {dispersion},
      min_reports_ {min_reports},
      // Round half to even, as the reference model does.
      max_reports_ {std::min(20, std::max(6, static_cast<int>(std::nearbyint(1.5 * dispersion))))},
      undecided_ {undecided} {
    if (p0_ / 1.2 < p1_ && p1_ < p0_ * 1.2) {
      p1_ = p1_ >= p0_ ? p0_ * 1.2 : p0_ / 1.2;
    }
  }

  std::optional<int> sprt_t::add(double expected, double lost, double weight) {
    n_ += weight;
    frames_ += expected * weight;
    lost_ += lost * weight;
    v_ += weight * llr(expected, lost, p0_, p1_, dispersion_);
    if (v_ >= a_) {
      return 1;
    }
    if (v_ <= b_ && n_ >= min_reports_) {
      return 0;
    }
    if (n_ >= std::max(max_reports_, min_reports_)) {
      if (undecided_) {
        return *undecided_;
      }
      return v_ > 0.0 ? 1 : 0;
    }
    return std::nullopt;
  }

  double sprt_t::rate() const {
    return frames_ > 0.0 ? lost_ / frames_ : 0.0;
  }

  policy_t::policy_t(int base_kbps, int floor_kbps):
      base_ {base_kbps},
      floor_ {floor_kbps},
      target_ {base_kbps},
      episode_start_ {base_kbps},
      r0_ {base_kbps},
      rtt_from_ {base_kbps},
      rtt_start_ {base_kbps} {
  }

  double policy_t::disp() const {
    return std::min(10.0, std::max(1.0, base_disp_));
  }

  sprt_t policy_t::make_sprt(double p0, double p1, int min_reports, std::optional<int> undecided) const {
    return sprt_t {p0, p1, std::max(2.0, disp()), min_reports, undecided};
  }

  int policy_t::upward_min() const {
    // A queue or a just-failed hold can appear clean before its queue refills and loss arrives.
    return hi_is_queue_ || hold_failed_kbps_ ? QUEUE_RETRY_REPORTS : 0;
  }

  double policy_t::p_background() const {
    return std::max(background_, MIN_LOSS);
  }

  void policy_t::clear_loss_run(bool restore_baseline) {
    if (restore_baseline && baseline_before_run_) {
      const auto &before = *baseline_before_run_;
      base_loss_ = before.loss;
      base_m2_ = before.m2;
      base_disp_ = before.dispersion;
      base_n_ = before.reports;
      disp_n_ = before.dispersion_reports;
    }
    baseline_before_run_.reset();
    cusum_ = 0.0;
    run_.clear();
    duty_.clear();
  }

  void policy_t::note_baseline(double expected, double lost) {
    const double a = 1.0 / std::min(BASELINE_REPORTS, base_n_ + 1);
    base_n_ += 1;
    const double x = lost / expected;
    base_loss_ = (1.0 - a) * base_loss_ + a * x;
    base_m2_ = (1.0 - a) * base_m2_ + a * x * x;
    disp_n_ = std::min(disp_n_ + 1, BASELINE_REPORTS);
    if (disp_n_ >= WARMUP_REPORTS) {
      const double var = std::max(0.0, base_m2_ - base_loss_ * base_loss_);
      base_disp_ = var * expected / std::max(base_loss_, 0.5 / expected);
    }
  }

  void policy_t::reset_baseline(double loss) {
    // A selected hold or rate-independent result resolves the candidate, retaining its burstiness.
    clear_loss_run(false);
    // Keep the measured burstiness; move only the level.
    const double var = std::max(0.0, base_m2_ - base_loss_ * base_loss_);
    base_loss_ = loss;
    base_m2_ = var + loss * loss;
    base_n_ = WARMUP_REPORTS;
  }

  void policy_t::set_target(double kbps, std::string_view why, std::int64_t now_ms) {
    const int next = static_cast<int>(std::max<double>(floor_, std::min<double>(base_, kbps)));
    if (next == target_) {
      return;
    }
    // Only an actual cut rejects the candidate's baseline samples. A raise retains what was learned.
    clear_loss_run(next < target_);
    ++changes_;
    last_change_ = change_t {now_ms, target_, next, why};
    after_change_ = next > target_ ? after_change_e::up : after_change_e::down;
    if (next < target_) {
      standing_run_ = 0;
      building_run_ = 0;
    }
    target_ = next;
  }

  void policy_t::to_normal(std::int64_t now_ms, int kbps, std::string_view why) {
    measured_.clear();
    retained_queue_ceiling_.reset();
    queue_continuation_from_.reset();
    queue_hold_below_.reset();
    failed_queue_hold_.reset();
    queue_retry_fails_ = 0;
    if (kbps >= base_) {
      hold_failed_kbps_.reset();
    }
    state_ = state_e::normal;
    hi_.reset();
    lo_.reset();
    test_.reset();
    set_target(kbps, why, now_ms);
  }

  void policy_t::note_measured(int kbps, double expected, double lost) {
    auto &[frames, lst] = measured_[kbps];
    frames += expected;
    lst += lost;
  }

  // The lowest rate from which everything measured above kbps, pooled from the top, lost significantly more.
  std::optional<int> policy_t::lossier_above(int kbps, std::pair<double, double> &above) const {
    const auto here = measured_.find(kbps);
    if (here == measured_.end() || here->second.first <= 0.0) {
      return std::nullopt;
    }
    std::optional<int> ceiling;
    std::pair<double, double> pooled {0.0, 0.0};
    for (auto it = measured_.rbegin(); it != measured_.rend() && it->first > kbps; ++it) {
      pooled.first += it->second.first;
      pooled.second += it->second.second;
      if (z_lower(here->second.second, here->second.first, pooled.second, pooled.first, std::max(2.0, disp())) >= 2.0) {
        ceiling = it->first;
        above = pooled;
      }
    }
    return ceiling;
  }

  bool policy_t::clearly_over(double lost, double frames, double level) const {
    const double r = frames > 0.0 ? lost / frames : 0.0;
    const double se = std::sqrt(std::max(r, 1e-4) * std::max(2.0, disp()) / std::max(frames, 1.0));
    return r - 2.0 * se > level;
  }

  // Where a loss probe may step from here, given what it lost here; nullopt when it may not go lower.
  std::optional<int> policy_t::loss_step(double kbps, double lost, double frames) const {
    // Judge the rate the step would set: half of 4001 is 2000.5, which is the floor.
    const int step = static_cast<int>(kbps);
    if (step > floor_) {
      return step;
    }
    // A fresh descent has nothing measured above it to compare a stop with, as when the request sits close over its
    // floor: it takes the floor.
    const auto above = measured_.upper_bound(target_);
    if (above == measured_.end()) {
      return step;
    }
    // Within an episode, a probe that would reach the floor first stops a climb step above it, to compare with the
    // rate above.
    const int near_floor = static_cast<int>(floor_ * CLIMB_STEP);
    if (target_ > near_floor) {
      return near_floor;
    }
    // The floor itself takes clearly heavy loss, or loss that still falls with the rate here: the nearest rate above
    // lost significantly more.
    if (clearly_over(lost, frames, HEAVY_LOSS) ||
        z_lower(lost, frames, above->second.second, above->second.first, std::max(2.0, disp())) >= 2.0) {
      return step;
    }
    return std::nullopt;
  }

  void policy_t::rate_independent(double rate, std::int64_t now_ms, std::string_view why) {
    std::pair<double, double> above {0.0, 0.0};
    if (const auto ceiling = lossier_above(target_, above)) {
      // A higher rate this episode lost significantly more: the loss depends on the rate above here.
      // Settle on the highest measured rate under that ceiling that loses no more than this one.
      const auto &here = measured_.at(target_);
      int settle = target_;
      for (const auto &[kbps, stats] : measured_) {
        if (kbps > target_ && kbps < *ceiling &&
            z_lower(here.second, here.first, stats.second, stats.first, std::max(2.0, disp())) < 2.0) {
          settle = kbps;
        }
      }
      const auto &at = measured_.at(settle);
      lo_loss_ = at.second / at.first;
      hi_ = *ceiling;
      hi_is_queue_ = false;
      hi_loss_ = above.second / above.first;
      hi_stats_ = above;
      hi_rechecked_ = false;
      enter_hold(now_ms, settle);
      return;
    }
    // Heavy loss is learned only once the episode has looked under it, at the floor or at a rate that lost no more
    // than 20%. Observed, not clearly over: 30 frames a report cannot clear two standard errors at 35%.
    const bool looked_under = std::any_of(measured_.begin(), measured_.end(), [&](const auto &rate_stats) {
      const auto &[kbps, stats] = rate_stats;
      return kbps <= floor_ || stats.second / stats.first <= HEAVY_LOSS;
    });
    if (rate > HEAVY_LOSS && target_ > floor_ && !looked_under) {
      std::pair<double, double> here {0.0, 0.0};
      for (auto it = measured_.lower_bound(target_); it != measured_.end(); ++it) {
        here.first += it->second.first;
        here.second += it->second.second;
      }
      sample_floor(now_ms, here);
      return;
    }
    hold_failed_kbps_.reset();
    background_ = std::max(background_, rate);
    // Whether this background goes again is judged on the reports after it was learned.
    normal_stats_ = {0.0, 0.0};
    normal_reports_ = 0;
    reset_baseline(background_);
    backoff_ = BACKOFF_REPORTS;
    to_normal(now_ms, std::max(r0_, episode_start_), why);
    fec_open_episode();
  }

  void policy_t::sample_floor(std::int64_t now_ms, std::pair<double, double> here) {
    // Judged as a deep sample is, against what the rate it left lost.
    state_ = state_e::descend;
    phase_ = phase_e::deep;
    hi_ = target_;
    hi_is_queue_ = false;
    hi_stats_ = here;
    hi_loss_ = here.first > 0.0 ? here.second / here.first : l0_;
    deep_stats_ = {0.0, 0.0};
    set_target(floor_, "descend_floor", now_ms);
    test_ = make_sprt(hi_loss_, std::max(hi_loss_ / 3.0, MIN_LOSS), 0, 0);
  }

  double policy_t::probe_rate() const {
    return probe_stats_.first > 0.0 ? probe_stats_.second / probe_stats_.first : l0_;
  }

  void policy_t::begin_descent(std::int64_t now_ms, std::string_view why, double l0) {
    // Loss that depends on the rate: parity would only add load.
    fec_revert();
    if (state_ == state_e::normal) {
      episode_start_ = target_;
      measured_.clear();
      for (const auto &[expected, lost] : run_) {
        note_measured(target_, expected, lost);
      }
    }
    if (state_ == state_e::hold) {
      // R5-D2: a hold that had been clean here now loses significantly more at the same rate: the link changed.
      // What the episode measured before is another link's, except a higher rate that lost significantly more than
      // this rate loses now, which a worse link cannot have made better. Restart the ledger from this run and judge
      // from this rate.
      if (const auto held = measured_.find(target_); held != measured_.end()) {
        double run_frames = 0.0;
        double run_lost = 0.0;
        for (const auto &[expected, lost] : run_) {
          run_frames += expected;
          run_lost += lost;
        }
        const double prior_frames = held->second.first - run_frames;
        const double prior_lost = held->second.second - run_lost;
        const double dispersion = std::max(2.0, disp());
        if (prior_frames >= HOLD_CHECK_REPORTS * (run_frames / std::max<std::size_t>(run_.size(), 1)) &&
            prior_lost / prior_frames <= background_ + LOSSY_MIN &&
            z_lower(prior_lost, prior_frames, run_lost, run_frames, dispersion) >= 2.0) {
          std::map<int, std::pair<double, double>> kept;
          for (auto it = measured_.upper_bound(target_); it != measured_.end(); ++it) {
            if (z_lower(run_lost, run_frames, it->second.second, it->second.first, dispersion) >= 2.0) {
              kept.insert(*it);
            }
          }
          measured_ = std::move(kept);
          for (const auto &[expected, lost] : run_) {
            note_measured(target_, expected, lost);
          }
          episode_start_ = target_;
        }
      }
      hold_failed_kbps_ = target_;
      if (hi_is_queue_ && queue_hold_below_ && *queue_hold_below_ < target_) {
        const int losses = failed_queue_hold_ && failed_queue_hold_->failed == target_ ?
                             std::min(failed_queue_hold_->losses, BACKOFF_REPORTS - 1) + 1 :
                             1;
        failed_queue_hold_ = failed_queue_hold_t {target_, *queue_hold_below_, losses, true};
      }
      // The loss probe needs its own verification ceiling. Keep the higher queue bound separate:
      // four clean reports at this lower hold do not test the rate that previously queued.
      if (hi_is_queue_ && hi_ && *hi_ > target_ &&
          (!retained_queue_ceiling_ || *hi_ < *retained_queue_ceiling_)) {
        retained_queue_ceiling_ = hi_;
      }
    }
    r0_ = target_;
    l0_ = l0;
    phase_ = phase_e::first;
    blind_step_ = false;
    retries_ = 0;
    queue_continuation_from_.reset();
    queue_hold_below_.reset();
    queue_retry_fails_ = 0;
    hi_ = target_;
    hi_loss_ = l0;
    hi_is_queue_ = false;
    probe_stats_ = {0.0, 0.0};
    hi_stats_ = {0.0, 0.0};
    for (const auto &[expected, lost] : run_) {
      hi_stats_.first += expected;
      hi_stats_.second += lost;
    }
    state_ = state_e::descend;
    const auto step = loss_step(target_ * FIRST_STEP, hi_stats_.second, hi_stats_.first);
    if (!step) {
      // No room to probe under this rate: judge it here.
      rate_independent(l0_, now_ms, "rate_independent");
      return;
    }
    set_target(*step, why, now_ms);
    test_ = make_sprt(l0_, std::max(l0_ / 3.0, MIN_LOSS), 0, 0);
  }

  void policy_t::descend_result(bool helped, std::int64_t now_ms) {
    if (helped) {
      const bool floor_measured = !measured_.empty() && measured_.begin()->first <= floor_;
      if (target_ > floor_ && target_ <= static_cast<int>(floor_ * CLIMB_STEP) && !floor_measured &&
          clearly_over(test_->lost(), test_->frames(), NEAR_FLOOR_LOSS)) {
        // Lighter here, but this close over the floor still losing more than background costs: the floor under it
        // may be clean, so sample it before this rate bounds a hold.
        sample_floor(now_ms, {test_->frames(), test_->lost()});
        return;
      }
      lo_ = target_;
      lo_loss_ = test_->rate();
      state_ = state_e::verify_hi;
      set_target(*hi_, "verify_hi", now_ms);
      test_ = make_sprt(std::max(lo_loss_, p_background()), std::max(hi_loss_ * 0.6, lo_loss_ + CLIMB_LOSSY), upward_min());
      return;
    }
    const double rate = test_->rate();
    if (phase_ == phase_e::first) {
      hi_ = target_;
      hi_loss_ = std::max(rate, l0_ * 0.5);
      hi_stats_ = {test_->frames(), test_->lost()};
      double deep = std::max<double>(2.0 * floor_, r0_ / PROBE_DEPTH);
      if (deep >= target_ * 0.9) {
        // No room for the deep sample. Clearly heavy loss here may be a link's that the floor is under: sample the
        // floor, as far as a probe may go, before judging the rate here.
        const auto step = target_ > floor_ && clearly_over(hi_stats_.second, hi_stats_.first, HEAVY_LOSS) ?
                            loss_step(floor_, hi_stats_.second, hi_stats_.first) :
                            std::nullopt;
        if (!step) {
          rate_independent(probe_rate(), now_ms, "rate_independent");
          return;
        }
        deep = *step;
      }
      phase_ = phase_e::deep;
      deep_stats_ = {0.0, 0.0};
      set_target(deep, "descend_deep", now_ms);
      test_ = make_sprt(hi_loss_, std::max(hi_loss_ / 3.0, MIN_LOSS), 0, 0);
      return;
    }
    if (phase_ == phase_e::deep) {
      deep_stats_.first += test_->frames();
      deep_stats_.second += test_->lost();
      const auto [frames, lost] = deep_stats_;
      const double r = frames > 0.0 ? lost / frames : 0.0;
      const double se = std::sqrt(std::max(r, 1e-4) * std::max(2.0, disp()) / std::max(frames, 1.0));
      // Heavy loss far down that is no lighter than at the first step does not depend on the rate either.
      const bool lighter = z_lower(lost, frames, hi_stats_.second, hi_stats_.first, std::max(2.0, disp())) >= 2.0;
      if (r - 2.0 * se - background_ <= MILD_LOSS || (!lighter && !clearly_over(lost, frames, HEAVY_LOSS))) {
        // The same loss far under the rate, not clearly heavier: it does not depend on the rate.
        rate_independent(probe_rate(), now_ms, "rate_independent");
        return;
      }
    }
    if (phase_ == phase_e::down && !clearly_over(test_->lost(), test_->frames(), HEAVY_LOSS) &&
        z_lower(test_->lost(), test_->frames(), hi_stats_.second, hi_stats_.first, std::max(2.0, disp())) < 2.0) {
      // Halving no longer lightens the loss: below here it does not depend on the rate.
      rate_independent(probe_rate(), now_ms, "rate_independent");
      return;
    }
    // Heavy loss even far down: keep halving, as far as the floor.
    phase_ = phase_e::down;
    hi_ = target_;
    hi_loss_ = std::max(rate, hi_loss_ * 0.5);
    hi_stats_ = {test_->frames(), test_->lost()};
    if (target_ <= floor_) {
      enter_hold(now_ms, target_);
      return;
    }
    const auto step = loss_step(target_ * STEP, hi_stats_.second, hi_stats_.first);
    if (!step) {
      enter_hold(now_ms, target_);
      return;
    }
    set_target(*step, "descend", now_ms);
    test_ = make_sprt(hi_loss_, std::max(hi_loss_ / 3.0, MIN_LOSS), 0, 0);
  }

  void policy_t::verify_hi_result(bool lossy, std::int64_t now_ms) {
    const bool loss_probe_verify = failed_queue_hold_ && std::exchange(failed_queue_hold_->verifying, false);
    if (!lossy) {
      const auto previous_queue_hold = hi_is_queue_ ? lo_ : std::nullopt;
      clean_promotion();
      if (loss_probe_verify && failed_queue_hold_ && target_ == failed_queue_hold_->failed &&
          failed_queue_hold_->fallback < target_) {
        // A short verify cannot rehabilitate the queue hold that just failed on loss.
        retained_queue_ceiling_.reset();
        hi_ = failed_queue_hold_->failed;
        hi_is_queue_ = true;
        hi_loss_ = CLIMB_LOSSY;
        hi_stats_ = {0.0, 0.0};
        hi_rechecked_ = false;
        queue_retry_fails_ = std::min(failed_queue_hold_->losses, BACKOFF_REPORTS);
        enter_hold(now_ms, failed_queue_hold_->fallback);
        return;
      }
      lo_ = target_;
      if (retained_queue_ceiling_ && target_ < *retained_queue_ceiling_) {
        // This only verified the lower rate. Preserve the queue provenance without reopening
        // unrestricted discovery; any failed-hold dwell keeps its own existing lifetime.
        hi_ = std::exchange(retained_queue_ceiling_, std::nullopt);
        hi_is_queue_ = true;
        hi_loss_ = CLIMB_LOSSY;
        hi_stats_ = {0.0, 0.0};
        hi_rechecked_ = false;
        if (static_cast<double>(*hi_) / *lo_ <= BISECT_DONE) {
          // Keep a closed bracket's tighter ceiling. Wider bounds use the staircase below.
          enter_hold(now_ms, target_);
          return;
        }
      }
      // A queue ceiling that held clean becomes a real hold before the next completion-size probe.
      // Keep the staircase's original anchor until its existing recheck radius is cleared.
      if (hi_is_queue_ || retained_queue_ceiling_) {
        if (!queue_continuation_from_) {
          queue_continuation_from_ = target_;
        }
        if (target_ < base_ && target_ < *queue_continuation_from_ * RECHECK_CEILING) {
          retained_queue_ceiling_.reset();
          queue_hold_below_ = previous_queue_hold && *previous_queue_hold < target_ ? previous_queue_hold : std::nullopt;
          queue_hold(now_ms, target_);
          return;
        }
      }
      queue_continuation_from_.reset();
      retained_queue_ceiling_.reset();
      hi_.reset();
      hi_is_queue_ = false;  // the ceiling held; climbs above it need no queue dwell
      state_ = state_e::climb;
      climb_step(now_ms);
      return;
    }
    if (hi_is_queue_ && hi_ && lo_ && static_cast<double>(*hi_) / *lo_ <= BISECT_DONE &&
        queue_retry_fails_ < BACKOFF_REPORTS) {
      ++queue_retry_fails_;
    }
    hi_loss_ = std::max(test_->rate(), hi_loss_);
    hi_stats_ = {hi_stats_.first + test_->frames(), hi_stats_.second + test_->lost()};
    hi_rechecked_ = true;
    state_ = state_e::bisect;
    bisect_step(now_ms);
  }

  void policy_t::bisect_step(std::int64_t now_ms) {
    if (!lo_) {
      enter_hold(now_ms, target_);
      return;
    }
    if (!hi_ || static_cast<double>(*hi_) / *lo_ <= BISECT_DONE) {
      enter_hold(now_ms, *lo_);
      return;
    }
    set_target(std::sqrt(static_cast<double>(*lo_) * *hi_), "bisect", now_ms);
    // Keep queue-only bisects unchanged; only a failed hold requires this extra loss-onset dwell.
    test_ = make_sprt(std::max(lo_loss_, p_background()), std::max(hi_loss_ * 0.6, lo_loss_ + CLIMB_LOSSY), hold_failed_kbps_ ? QUEUE_RETRY_REPORTS : 0);
  }

  void policy_t::bisect_result(bool lossy, std::int64_t now_ms) {
    if (lossy) {
      hi_ = target_;
      hi_loss_ = std::max(test_->rate(), hi_loss_ * 0.5);
      hi_stats_ = {test_->frames(), test_->lost()};
      hi_rechecked_ = false;
    } else {
      clean_promotion();
      lo_ = target_;
      if (hi_ && !hi_rechecked_ && static_cast<double>(*hi_) / *lo_ < RECHECK_CEILING &&
          static_cast<double>(*hi_) / *lo_ > BISECT_DONE) {
        // Clean this close under the ceiling: the ceiling may have gone, so try it before narrowing in.
        hi_rechecked_ = true;
        state_ = state_e::verify_hi;
        set_target(*hi_, "recheck_ceiling", now_ms);
        test_ = make_sprt(std::max(lo_loss_, p_background()), std::max(hi_loss_ * 0.6, lo_loss_ + CLIMB_LOSSY), upward_min());
        return;
      }
    }
    bisect_step(now_ms);
  }

  void policy_t::clean_promotion() {
    queue_retry_fails_ = 0;
    // Called only by clean verify/bisect tests, which require the full dwell while this is set.
    // A clean lower rate does not prove the failed rate safe, and ordinary climbs add no dwell.
    if (hold_failed_kbps_ && target_ >= *hold_failed_kbps_) {
      hold_failed_kbps_.reset();
    }
  }

  void policy_t::queue_hold(std::int64_t now_ms, int kbps) {
    hi_ = static_cast<int>(kbps * BISECT_DONE);
    hi_is_queue_ = true;
    hi_loss_ = CLIMB_LOSSY;
    hi_stats_ = {0.0, 0.0};
    hi_rechecked_ = false;
    enter_hold(now_ms, kbps);
  }

  void policy_t::enter_hold(std::int64_t now_ms, int kbps) {
    state_ = state_e::hold;
    reset_baseline(lo_loss_);
    set_target(kbps, "hold", now_ms);
    lo_ = target_;
    retry_wait_ = retries_ == 0 ? FIRST_RETRY_REPORTS : RETRY_REPORTS;
    if (hi_is_queue_ && queue_retry_fails_ > 0) {
      retry_wait_ = RETRY_REPORTS;
      for (int failure = 1; failure < queue_retry_fails_ && retry_wait_ < BACKOFF_REPORTS; ++failure) {
        // Check the cap before doubling; neither the counter nor the wait can overflow.
        retry_wait_ = retry_wait_ >= BACKOFF_REPORTS - retry_wait_ ? BACKOFF_REPORTS : retry_wait_ * 2;
      }
    }
    hold_stats_ = {0.0, 0.0};
    hold_reports_ = 0;
    test_.reset();
  }

  void policy_t::climb_step(std::int64_t now_ms) {
    if (target_ >= base_) {
      to_normal(now_ms, base_, "base");
      return;
    }
    const double step = static_cast<double>(target_) * 2.0 < base_ ? CLIMB_FAR : CLIMB_STEP;
    set_target(target_ * step, "climb", now_ms);
    const double p0 = std::max(lo_loss_, p_background());
    test_ = make_sprt(p0, p0 + CLIMB_LOSSY);
  }

  void policy_t::climb_result(bool lossy, std::int64_t now_ms) {
    if (lossy) {
      hi_ = target_;
      hi_loss_ = test_->rate();
      hi_stats_ = {test_->frames(), test_->lost()};
      state_ = state_e::bisect;
      bisect_step(now_ms);
    } else {
      lo_ = target_;
      climb_step(now_ms);
    }
  }

  void policy_t::step_result(bool lossy, std::int64_t now_ms) {
    switch (state_) {
      case state_e::verify_hi:
        verify_hi_result(lossy, now_ms);
        break;
      case state_e::bisect:
        bisect_result(lossy, now_ms);
        break;
      case state_e::climb:
        climb_result(lossy, now_ms);
        break;
      default:
        break;
    }
  }

  void policy_t::report(std::int64_t now_ms, double expected, double lost) {
    const bool gap = last_report_ms_ && now_ms - *last_report_ms_ > REPORT_GAP_MS;
    last_report_ms_ = now_ms;
    if (expected <= 0.0) {
      return;
    }
    const auto first = after_change_;
    after_change_ = after_change_e::none;
    // Likewise the first report after a published FEC raise still covers the old percentage.
    const bool fec_first = std::exchange(fec_skip_, false);
    if (gap || first != after_change_e::none) {
      // The first report after either direction still covers the previous rate. Even half of a
      // clean report can approve a retry against severe prior loss before its queue builds.
      // A gap alone leaves the candidate and its snapshot pending: RTT may still confirm a cut.
      return;
    }
    if (backoff_ > 0) {
      --backoff_;
    }
    if (state_ != state_e::normal) {
      note_measured(target_, expected, lost);
    }
    fec_report(expected, lost, fec_first);
    const auto st = state_;
    if (st == state_e::descend) {
      probe_stats_.first += expected;
      probe_stats_.second += lost;
    }
    if (st == state_e::descend || st == state_e::verify_hi || st == state_e::bisect || st == state_e::climb) {
      if (!test_) {
        return;
      }
      const auto res = test_->add(expected, lost, 1.0);
      if (res) {
        if (st == state_e::descend) {
          descend_result(*res == 1, now_ms);
        } else {
          step_result(*res == 1, now_ms);
        }
      }
      return;
    }
    if (st == state_e::rtt_cut) {
      const double loss = lost / expected;
      if (loss > p_background() + REACT_EXCESS) {
        rtt_cut_lossy_ = true;
      }
      if (loss > std::max(HEAVY_LOSS, p_background() + MILD_LOSS)) {
        // Heavy loss is stronger evidence than the queue: the loss probe takes it from here.
        state_ = state_e::hold;
        run_ = {{expected, lost}, {expected, lost}};
        begin_descent(now_ms, "descend_loss_in_rtt_cut", loss);
      }
      return;
    }
    watch(now_ms, expected, lost);
  }

  void policy_t::watch(std::int64_t now_ms, double expected, double lost) {
    const auto st = state_;
    const bool warm = base_n_ >= WARMUP_REPORTS;
    double p0 = std::max(warm ? base_loss_ : 0.0, p_background());
    if (st == state_e::hold) {
      p0 = std::max(p0, lo_loss_);
    }
    const double p1 = std::max(2.5 * p0, p0 + REACT_EXCESS);
    const double lossy_at = std::max(LOSSY_MIN, p0 + REACT_EXCESS / 2.0);
    const bool lossy_report = lost / expected >= lossy_at;
    cusum_ = std::max(0.0, cusum_ + llr(expected, lost, p0, p1, std::max(2.0, disp())));
    if (cusum_ == 0.0) {
      baseline_before_run_.reset();
      run_.clear();
    } else {
      if (!baseline_before_run_) {
        baseline_before_run_ = baseline_t {base_loss_, base_m2_, base_disp_, base_n_, disp_n_};
      }
      run_.emplace_back(expected, lost);
    }
    // Recurring bursts must teach both the level and the dispersion. If this candidate instead
    // ends in a real cut, clear_loss_run() restores the baseline from before its first report.
    note_baseline(expected, lost);
    if (st == state_e::normal) {
      normal_stats_.first += expected;
      normal_stats_.second += lost;
      if (++normal_reports_ >= BACKGROUND_DECAY_REPORTS) {
        const double rate = normal_stats_.second / normal_stats_.first;
        if (rate * 1.2 < background_) {
          background_ = rate * 1.2;  // background loss that has gone
        }
        normal_stats_ = {0.0, 0.0};
        normal_reports_ = 0;
      }
    } else {
      hold_stats_.first += expected;
      hold_stats_.second += lost;
      if (++hold_reports_ >= HOLD_CHECK_REPORTS) {
        const auto [frames, lst] = hold_stats_;
        const double rate = lst / frames;
        if (target_ > floor_ && clearly_over(lst, frames, HEAVY_LOSS)) {
          // Heavy loss that stays at a hold is the link's: probe under it, on this hold's evidence.
          run_ = {hold_stats_};
          begin_descent(now_ms, "descend_heavy_at_hold", rate);
          return;
        }
        if (!hi_is_queue_ && target_ > floor_ && target_ <= static_cast<int>(floor_ * CLIMB_STEP) &&
            (measured_.empty() || measured_.begin()->first > floor_) && clearly_over(lst, frames, NEAR_FLOOR_LOSS)) {
          // This close over the floor, a hold losing more than background costs: the floor under it may be clean.
          sample_floor(now_ms, hold_stats_);
          return;
        }
        const auto [hf, hl] = hi_stats_;
        const bool real_loss = !hi_is_queue_ && hf > 0.0 && hl / hf >= background_ + REACT_EXCESS;
        if (hi_ && real_loss && z_lower(lst, frames, hl, hf, std::max(2.0, disp())) < 2.0) {
          // The lower rate is no better: the loss does not depend on the rate after all.
          rate_independent(rate, now_ms, "hold_no_better");
          return;
        }
        lo_loss_ = rate;
        if (rate * 1.2 < background_) {
          background_ = rate * 1.2;
        }
        hold_stats_ = {0.0, 0.0};
        hold_reports_ = 0;
      }
    }
    push_bounded(duty_, lossy_report, DUTY_WINDOW);
    const auto lossy = std::count(duty_.begin(), duty_.end(), true);
    if (cusum_ >= CUSUM_H && lossy >= DUTY_NEEDED) {
      double frames = 0.0;
      double run_lost = 0.0;
      for (const auto &[e, l] : run_) {
        frames += e;
        run_lost += l;
      }
      const double mean = run_lost / frames;
      if (backoff_ > 0 && mean <= background_ + ABSORB_EXCESS) {
        background_ = std::max(background_, mean);
        baseline_before_run_.reset();
        cusum_ = 0.0;
        run_.clear();
        return;
      }
      begin_descent(now_ms, st == state_e::normal ? "descend_loss" : "descend_loss_at_hold", mean);
      return;
    }
    if (st == state_e::normal && target_ < base_ && cusum_ == 0.0) {
      lo_ = target_;
      hi_.reset();
      lo_loss_ = std::max(base_loss_, p_background());
      state_ = state_e::climb;
      climb_step(now_ms);
      return;
    }
    // Allow one existing duty window to confirm a new candidate before retrying. Two lossy
    // reports already probe above; one isolated burst must not defer a due retry indefinitely.
    const bool probe_deferred = cusum_ > 0.0 && run_.size() < DUTY_WINDOW;
    if (st == state_e::hold && !hi_ && target_ < base_) {
      // Nothing above is known to fail: carry on climbing.
      if (retry_wait_ > 0) {
        --retry_wait_;
      }
      // Judge the existing loss window before a due probe commits an isolated burst.
      if (retry_wait_ == 0 && !probe_deferred) {
        state_ = state_e::climb;
        climb_step(now_ms);
      }
      return;
    }
    if (st == state_e::hold && hi_) {
      if (retry_wait_ > 0) {
        --retry_wait_;
      }
      // Judge the existing loss window before a due probe commits an isolated burst.
      if (retry_wait_ == 0 && !probe_deferred) {
        if (failed_queue_hold_ && lo_ && *lo_ >= failed_queue_hold_->failed) {
          failed_queue_hold_.reset();
        }
        ++retries_;
        state_ = state_e::verify_hi;
        set_target(*hi_, "retry", now_ms);
        const double retry_p0 = std::max(lo_loss_, p_background());
        test_ = make_sprt(retry_p0, retry_p0 + CLIMB_LOSSY, upward_min());
      }
    }
  }

  bool policy_t::raising() const {
    const bool upward = state_ == state_e::verify_hi || state_ == state_e::bisect || state_ == state_e::climb;
    return upward && lo_ && target_ > *lo_;
  }

  void policy_t::rtt(std::int64_t now_ms, double smoothed_rtt_ms) {
    if (last_rtt_ && smoothed_rtt_ms != *last_rtt_) {
      rises_ = smoothed_rtt_ms > *last_rtt_ ? rises_ + 1 : 0;
    }
    last_rtt_ = smoothed_rtt_ms;
    if (raising() && rises_ >= RTT_RISES && !rtt_min_.empty()) {
      const double floor_rtt = *std::min_element(rtt_min_.begin(), rtt_min_.end());
      if (smoothed_rtt_ms >= floor_rtt + std::max(10.0, 0.5 * floor_rtt)) {
        // The smoothed RTT rose at its last two changes, to a queue's height: this step is building one.
        rises_ = 0;
        hi_is_queue_ = true;
        step_result(true, now_ms);
      }
    }
    tick(now_ms);
    const std::int64_t sec = now_ms / 1000;
    if (!sec_) {
      sec_ = sec;
    }
    if (sec != *sec_) {
      close_second(now_ms);
      sec_ = sec;
      sec_vals_.clear();
    }
    sec_vals_.push_back(smoothed_rtt_ms);
  }

  void policy_t::tick(std::int64_t now_ms) {
    if (state_ == state_e::descend && phase_ == phase_e::first && !blind_step_ && last_report_ms_ &&
        now_ms - *last_report_ms_ > MISSING_MS) {
      // The probe's first step cannot be judged: the reports stopped, maybe because the link is overloaded.
      // Take the deep sample now, once, rather than ride the rate for the whole pause.
      const double deep = std::max<double>(2.0 * floor_, r0_ / PROBE_DEPTH);
      if (deep < target_ * 0.9) {
        blind_step_ = true;
        hi_ = target_;
        hi_loss_ = std::max(hi_loss_, l0_ * 0.5);
        phase_ = phase_e::deep;
        deep_stats_ = {0.0, 0.0};
        set_target(deep, "descend_deep_unjudged", now_ms);
        test_ = make_sprt(hi_loss_, std::max(hi_loss_ / 3.0, MIN_LOSS), 0, 0);
      }
    }
    if (raising() && last_report_ms_ && now_ms - *last_report_ms_ > PAUSE_TAKE_BACK_MS) {
      // No report can judge this raise: take it back without counting it as a failure,
      // and try it again once reports are back.
      const int tried = target_;
      if (!hi_ || tried < *hi_) {
        hi_ = tried;
        hi_is_queue_ = false;
        hi_stats_ = {0.0, 0.0};
      }
      enter_hold(now_ms, *lo_);
      retry_wait_ = 3;
    }
  }

  void policy_t::close_second(std::int64_t now_ms) {
    if (sec_vals_.empty()) {
      return;
    }
    auto vals = sec_vals_;
    std::sort(vals.begin(), vals.end());
    const double med = vals[vals.size() / 2];
    std::optional<double> floor_rtt;
    if (!rtt_min_.empty()) {
      floor_rtt = *std::min_element(rtt_min_.begin(), rtt_min_.end());
    }
    const std::vector<double> prev(sec_meds_.begin(), sec_meds_.end());
    push_bounded(sec_meds_, med, SEC_MEDS);
    const bool raised = floor_rtt && med >= *floor_rtt + std::max(10.0, 0.5 * *floor_rtt);
    if (!raised) {
      push_bounded(rtt_min_, vals.front(), RTT_WINDOW_S);
    }
    // At ENet's idle cadence, an acknowledgement about every 500 ms, a drained queue still reads high for 15-20 s
    // through its smoothing. A raised RTT that is not falling is a standing queue; one climbing 2 ms/s or more is
    // a queue being built now.
    const bool standing = raised && prev.size() >= 3 && med >= prev[prev.size() - 3] - 0.5;
    const bool building = raised && prev.size() >= 2 && (med - prev[prev.size() - 2]) / 2.0 >= RTT_BUILD_MS_S;
    standing_run_ = standing ? standing_run_ + 1 : 0;
    building_run_ = building ? building_run_ + 1 : 0;
    if (rtt_suppress_ > 0) {
      --rtt_suppress_;
    }
    if (raising() && building_run_ >= RTT_BUILD_FAIL_S) {
      // This raise is building a queue: it fails, whatever the loss says.
      building_run_ = 0;
      hi_is_queue_ = true;
      step_result(true, now_ms);
      return;
    }
    if (state_ == state_e::rtt_cut) {
      if (!standing) {
        // The queue is draining: the rate before the cut was over the link.
        lo_ = target_;
        lo_loss_ = std::max(lo_loss_, p_background());
        hi_ = std::min(rtt_from_, static_cast<int>(target_ * BISECT_DONE));
        hi_loss_ = CLIMB_LOSSY;
        hi_is_queue_ = true;
        hi_stats_ = {0.0, 0.0};
        enter_hold(now_ms, target_);
        return;
      }
      if (--rtt_cut_wait_ <= 0) {
        if (rtt_cuts_ >= RTT_MAX_CUTS && rtt_cut_lossy_) {
          // Our own queue three cuts did not drain: the loss probe takes it from here.
          lo_ = target_;
          lo_loss_ = std::max(lo_loss_, p_background());
          enter_hold(now_ms, target_);
          return;
        }
        if (rtt_cuts_ >= RTT_MAX_CUTS) {
          // Cutting did not drain it and nothing was lost: someone else's queue.
          rtt_suppress_ = RTT_SUPPRESS_S;
          to_normal(now_ms, rtt_start_, "rtt_not_ours");
          return;
        }
        rtt_cut(now_ms);
      }
      return;
    }
    const bool steady = state_ == state_e::normal || state_ == state_e::hold || state_ == state_e::climb;
    if (standing_run_ >= RTT_STANDING_S && rtt_suppress_ == 0 && steady) {
      standing_run_ = 0;
      rtt_start_ = state_ != state_e::hold ? target_ : r0_;
      rtt_cuts_ = 0;
      rtt_cut_lossy_ = false;
      state_ = state_e::rtt_cut;
      rtt_cut(now_ms);
    }
  }

  void policy_t::rtt_cut(std::int64_t now_ms) {
    queue_continuation_from_.reset();
    queue_hold_below_.reset();
    queue_retry_fails_ = 0;
    rtt_from_ = target_;
    ++rtt_cuts_;
    rtt_cut_wait_ = RTT_DRAIN_S;
    set_target(target_ * RTT_CUT, "rtt", now_ms);
  }

  void policy_t::adopt(std::int64_t now_ms, int kbps) {
    fec_revert();
    measured_.clear();
    retained_queue_ceiling_.reset();
    queue_continuation_from_.reset();
    queue_hold_below_.reset();
    failed_queue_hold_.reset();
    queue_retry_fails_ = 0;
    hold_failed_kbps_.reset();
    const int previous = target_;
    const int next = std::clamp(kbps, std::min(floor_, base_), base_);
    // Use the clamped rate, including a no-op adoption which resolves the run without a cut.
    clear_loss_run(next < target_);
    if (next != target_) {
      after_change_ = next > target_ ? after_change_e::up : after_change_e::down;
      target_ = next;
    }
    (void) now_ms;
    test_.reset();
    standing_run_ = 0;
    building_run_ = 0;
    if (target_ >= base_) {
      state_ = state_e::normal;
      hi_.reset();
      lo_.reset();
      return;
    }
    // Hold where the writer left it; the rate it came from is the one to try again.
    state_ = state_e::hold;
    lo_ = target_;
    lo_loss_ = std::max(lo_loss_, p_background());
    if (previous > target_) {
      hi_ = previous;
    } else {
      hi_.reset();
    }
    hi_is_queue_ = false;
    hi_stats_ = {0.0, 0.0};
    reset_baseline(lo_loss_);
    retry_wait_ = RETRY_REPORTS;
    hold_stats_ = {0.0, 0.0};
    hold_reports_ = 0;
    r0_ = std::max(previous, target_);
  }

  void policy_t::rebase(int base_kbps, int floor_kbps) {
    fec_revert();
    measured_.clear();
    retained_queue_ceiling_.reset();
    queue_continuation_from_.reset();
    queue_hold_below_.reset();
    failed_queue_hold_.reset();
    queue_retry_fails_ = 0;
    base_ = std::max(1, base_kbps);
    floor_ = std::min(floor_kbps, base_);
    const int next = std::clamp(target_, floor_, base_);
    if (next != target_) {
      hold_failed_kbps_.reset();
      clear_loss_run(next < target_);
      after_change_ = next > target_ ? after_change_e::up : after_change_e::down;
      if (next < target_) {
        standing_run_ = 0;
        building_run_ = 0;
      }
      target_ = next;
    }
    episode_start_ = std::min(episode_start_, base_);
    r0_ = std::min(r0_, base_);
    rtt_start_ = std::min(rtt_start_, base_);
    if (hi_ && *hi_ > base_) {
      hi_ = base_;
    }
    if (state_ == state_e::hold && hi_ && *hi_ <= target_) {
      hi_.reset();
    }
  }

  void policy_t::configure_fec(int configured_percentage) {
    fec_wanted_.reset();
    fec_test_.reset();
    fec_backoff_ = 0;
    fec_skip_ = false;
    if (configured_percentage < 0 || configured_percentage > 255) {
      fec_configured_.reset();
      fec_pct_ = 0;
      return;
    }
    fec_configured_ = configured_percentage;
    fec_pct_ = configured_percentage;
  }

  std::optional<int> policy_t::fec_request() const {
    if (fec_wanted_ && *fec_wanted_ != fec_pct_) {
      return fec_wanted_;
    }
    return std::nullopt;
  }

  void policy_t::fec_applied(std::int64_t now_ms, int percentage) {
    (void) now_ms;
    if (!fec_configured_) {
      return;
    }
    const int configured = *fec_configured_;
    if (percentage == configured) {
      // Back at the configured percentage, asked for or not: whatever was on trial or held is over.
      fec_pct_ = configured;
      fec_wanted_.reset();
      fec_test_.reset();
      fec_skip_ = false;
      return;
    }
    if (!fec_wanted_ || percentage != *fec_wanted_ || percentage < configured || percentage > FEC_MAX) {
      return;  // not what the policy asked for
    }
    const bool raise = percentage > fec_pct_;
    fec_pct_ = percentage;
    fec_wanted_.reset();
    if (raise) {
      // Did the parity help? The same test a descent uses: H0 the learned background, H1 a third of it.
      fec_skip_ = true;
      fec_test_ = make_sprt(p_background(), std::max(p_background() / 3.0, MIN_LOSS), 0, 0);
    }
  }

  void policy_t::fec_refused(std::int64_t now_ms, bool backoff) {
    (void) now_ms;
    if (!fec_configured_ || !fec_wanted_) {
      return;
    }
    // The confirmed percentage holds. Asking again waits out the back-off, which counts judged reports.
    fec_wanted_.reset();
    if (backoff) fec_backoff_ = BACKOFF_REPORTS;
  }

  void policy_t::fec_open_episode() {
    if (!fec_configured_ || fec_wanted_ || fec_test_ || fec_backoff_ > 0) {
      return;
    }
    const int configured = *fec_configured_;
    if (configured >= FEC_MAX || fec_pct_ != configured) {
      return;
    }
    fec_wanted_ = std::min(FEC_MAX, fec_pct_ + std::max(FEC_MIN_STEP, fec_pct_ / 2));
  }

  void policy_t::fec_revert() {
    if (!fec_configured_) {
      return;
    }
    fec_test_.reset();
    fec_skip_ = false;
    if (fec_pct_ != *fec_configured_) {
      fec_wanted_ = *fec_configured_;
    } else {
      fec_wanted_.reset();  // a raise not yet published is no longer wanted
    }
  }

  void policy_t::fec_report(double expected, double lost, bool first_after_raise) {
    if (!fec_configured_) {
      return;
    }
    if (fec_test_) {
      // Judge the parity only at a steady rate; a search judges the rate.
      if (first_after_raise || state_ != state_e::normal) {
        return;
      }
      const auto res = fec_test_->add(expected, lost, 1.0);
      if (!res) {
        return;
      }
      fec_test_.reset();
      fec_backoff_ = BACKOFF_REPORTS;
      if (*res != 1) {
        fec_wanted_ = *fec_configured_;  // no clear help, undecided included: back to the configured parity
      }
      return;
    }
    if (fec_backoff_ > 0 && --fec_backoff_ == 0 && fec_pct_ != *fec_configured_ && !fec_wanted_) {
      fec_wanted_ = *fec_configured_;  // held long enough: see whether the configured parity does again
    }
  }

}  // namespace live_tuning_policy
