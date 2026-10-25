/**
 * @file src/live_tuning_policy.cpp
 * @brief Live Tuning's bitrate decisions from media loss and the control channel's RTT.
 */
#include "live_tuning_policy.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace live_tuning_policy {

  namespace {

    constexpr double MIN_LOSS = 0.002;  // the smallest loss rate any test assumes
    constexpr double REACT_EXCESS = 0.015;  // loss this far over the expected level is worth a probe
    constexpr double MILD_LOSS = 0.045;  // deep-sample loss not clearly this far over the background is rate-independent
    constexpr double ABSORB_EXCESS = 0.02;  // while backing off, loss this close to the background only updates it
    constexpr double CLIMB_LOSSY = 0.03;  // a raise is lossy at this much loss over its floor
    constexpr double FIRST_STEP = 0.6;
    constexpr double STEP = 0.5;
    constexpr double PROBE_DEPTH = 8.0;
    constexpr double BISECT_DONE = 1.06;
    constexpr double RECHECK_CEILING = 1.25;  // a clean step this close under the ceiling rechecks the ceiling
    constexpr double CLIMB_STEP = 1.25;
    constexpr double CLIMB_FAR = 1.5;
    constexpr double CLIMB_PAST_QUEUE = 1.1;  // the first step above a queue ceiling that just held clean
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
    // A queue ceiling needs a few seconds for a queue to show in ENet's smoothed RTT.
    return hi_is_queue_ ? QUEUE_RETRY_REPORTS : 0;
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
    state_ = state_e::normal;
    hi_.reset();
    lo_.reset();
    test_.reset();
    set_target(kbps, why, now_ms);
  }

  void policy_t::rate_independent(double rate, std::int64_t now_ms, std::string_view why) {
    background_ = std::max(background_, rate);
    reset_baseline(background_);
    backoff_ = BACKOFF_REPORTS;
    to_normal(now_ms, std::max(r0_, episode_start_), why);
  }

  double policy_t::probe_rate() const {
    return probe_stats_.first > 0.0 ? probe_stats_.second / probe_stats_.first : l0_;
  }

  void policy_t::begin_descent(std::int64_t now_ms, std::string_view why, double l0) {
    if (state_ == state_e::normal) {
      episode_start_ = target_;
    }
    r0_ = target_;
    l0_ = l0;
    phase_ = phase_e::first;
    blind_step_ = false;
    retries_ = 0;
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
    set_target(target_ * FIRST_STEP, why, now_ms);
    test_ = make_sprt(l0_, std::max(l0_ / 3.0, MIN_LOSS), 0, 0);
  }

  void policy_t::descend_result(bool helped, std::int64_t now_ms) {
    if (helped) {
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
      const double deep = std::max<double>(2.0 * floor_, r0_ / PROBE_DEPTH);
      if (deep >= target_ * 0.9) {
        rate_independent(probe_rate(), now_ms, "rate_independent");
        return;
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
      if (r - 2.0 * se - background_ <= MILD_LOSS) {
        // The same loss far under the rate, not clearly heavier: it does not depend on the rate.
        rate_independent(probe_rate(), now_ms, "rate_independent");
        return;
      }
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
    set_target(target_ * STEP, "descend", now_ms);
    test_ = make_sprt(hi_loss_, std::max(hi_loss_ / 3.0, MIN_LOSS), 0, 0);
  }

  void policy_t::verify_hi_result(bool lossy, std::int64_t now_ms) {
    if (!lossy) {
      lo_ = target_;
      // A queue ceiling that held clean may still be the link: a queue there builds slower than a
      // retry can see, so the first step above it is a small one.
      gentle_climb_ = hi_is_queue_;
      hi_.reset();
      hi_is_queue_ = false;  // the ceiling held; climbs above it need no queue dwell
      state_ = state_e::climb;
      climb_step(now_ms);
      return;
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
    test_ = make_sprt(std::max(lo_loss_, p_background()), std::max(hi_loss_ * 0.6, lo_loss_ + CLIMB_LOSSY));
  }

  void policy_t::bisect_result(bool lossy, std::int64_t now_ms) {
    if (lossy) {
      hi_ = target_;
      hi_loss_ = std::max(test_->rate(), hi_loss_ * 0.5);
      hi_stats_ = {test_->frames(), test_->lost()};
      hi_rechecked_ = false;
    } else {
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

  void policy_t::enter_hold(std::int64_t now_ms, int kbps) {
    state_ = state_e::hold;
    reset_baseline(lo_loss_);
    set_target(kbps, "hold", now_ms);
    lo_ = target_;
    retry_wait_ = retries_ == 0 ? FIRST_RETRY_REPORTS : RETRY_REPORTS;
    hold_stats_ = {0.0, 0.0};
    hold_reports_ = 0;
    test_.reset();
  }

  void policy_t::climb_step(std::int64_t now_ms) {
    if (target_ >= base_) {
      to_normal(now_ms, base_, "base");
      return;
    }
    const double step = gentle_climb_ ? CLIMB_PAST_QUEUE :
      static_cast<double>(target_) * 2.0 < base_ ? CLIMB_FAR : CLIMB_STEP;
    gentle_climb_ = false;
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
    if (gap || first != after_change_e::none) {
      // The first report after either direction still covers the previous rate. Even half of a
      // clean report can approve a retry against severe prior loss before its queue builds.
      // A gap alone leaves the candidate and its snapshot pending: RTT may still confirm a cut.
      return;
    }
    if (backoff_ > 0) {
      --backoff_;
    }
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
      if (loss > std::max(0.2, p_background() + MILD_LOSS)) {
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
        hi_ = rtt_from_;
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
    rtt_from_ = target_;
    ++rtt_cuts_;
    rtt_cut_wait_ = RTT_DRAIN_S;
    set_target(target_ * RTT_CUT, "rtt", now_ms);
  }

  void policy_t::adopt(std::int64_t now_ms, int kbps) {
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
    base_ = std::max(1, base_kbps);
    floor_ = std::min(floor_kbps, base_);
    const int next = std::clamp(target_, floor_, base_);
    if (next != target_) {
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

}  // namespace live_tuning_policy
