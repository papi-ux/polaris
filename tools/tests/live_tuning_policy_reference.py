#!/usr/bin/env python3
"""Reference model of src/live_tuning_policy.cpp, and the replays that pin the C++ port to it.

    python3 tools/tests/live_tuning_policy_reference.py --write tests/fixtures/live-tuning-policy-replays.json

regenerates the golden per-second targets tests/unit/test_live_tuning_policy.cpp replays. A change to the policy
changes both files: edit the C++ and this model the same way, regenerate, and read the diff of the fixture.

Live Tuning's loss and queue policy: the reference model the C++ port follows.

Evidence
- Media reports, about one a second: frames expected and frames lost. Lost frames are frames FEC could not save.
- Pings, about ten a second: the host's ENet-smoothed RTT, the only RTT ENet keeps. It moves 1/8 of the way to a
  new sample, rounded up to a whole millisecond, with each acknowledgement, and the host cannot tell how many
  came between two readings, so the policy judges the smoothed value alone.

Loss
- Decisions count reports, never wall time, so a report pause freezes them. A report after a gap of more than
  2.5 s, or the first after any rate change, is not judged because it covers the previous rate.
- A CUSUM against a slow running baseline (its level and its burstiness) detects a change in loss, and a probe
  also needs two of the last three reports to lose 3% of their frames or more. Loss under 3% never starts one,
  and neither does one burst. Reports keep teaching the baseline while a change is only a candidate. An actual
  cut restores the level and burstiness from before that candidate; if the detector clears, the learned
  baseline stays. A skipped report gap alone does not resolve the candidate.
- A due hold retry or climb waits at most one judged duty window for a new candidate; outside writers own their changes.
- A probe steps to 0.6x. If that helped, it rechecks the rate it left (the loss may simply have ended) and
  climbs back or bisects. If not, it takes one deep sample at max(2x floor, start/8). Clean there: the link is
  in between, so it rechecks and bisects. Loss there that is not clearly heavy: the loss does not depend on
  the rate, so the stream goes back to where the episode started, learns that loss as background, and does
  not probe again at that level for 1200 reports. Clearly heavy loss there: it halves on down to the floor.
  When reports stop before the first step is judged, the probe takes its deep sample after 1.5 s, once.
- "Did this help" and "is this step lossy" are sequential probability ratio tests on frame counts, scaled by
  the measured burstiness. An undecided descent test counts as not having helped.
- A hold retries the rate above it after 8 reports, then every 20, and climbs from there when it is clean. A
  clean step within 25% under that rate retries it at once. While a hold's loss is not significantly lower
  than real loss at the rate above it, the loss is rate-independent after all.
  After a hold fails, clean verify/bisect promotions need the existing four-report queue dwell until
  a rate at or above that failed hold passes. A higher queue ceiling survives a loss descent from its
  lower hold: verifying the lower rate resumes that queue bracket instead of retiring the higher bound.
  Ordinary climbs keep their frame test without this dwell.

Queues
- A raise fails when the smoothed RTT rises at two changes in a row to a raised level, 10 ms or half the minute's
  floor over that floor, whichever is more, or is raised and climbing 2 ms/s for 2 s.
- A held rate cuts 15% when the smoothed RTT has been raised and not falling for 8 s. A Wi-Fi background scan
  only raises it for about 5 s before it falls (the Deck's peak at 83 ms for 5 s). Cuts go on every 6 s until
  the queue starts to drain, at most three; then the rate that queued is the ceiling above the hold. The
  first promotion past that rate, once verified clean, is bounded by the existing 1.06x completion step.
  A queue three cuts did not drain, with nothing lost meanwhile, is someone else's, and is left alone for 5 minutes.
- Heavy loss while cutting for a queue hands the stream to the loss probe.
"""
import argparse
import json
import math
from collections import deque
from pathlib import Path

NORMAL, DESCEND, VERIFY_HI, BISECT, HOLD, CLIMB, RTT_CUT = (
    'normal', 'descend', 'verify_hi', 'bisect', 'hold', 'climb', 'rtt_cut')
UPWARD = (VERIFY_HI, BISECT, CLIMB)


def llr(expected, lost, p0, p1, dispersion):
    return (lost * math.log(p1 / p0) - expected * (p1 - p0)) / dispersion


def z_lower(a_lost, a_frames, b_lost, b_frames, dispersion):
    """How many standard errors rate a sits under rate b, allowing for bursty loss."""
    if a_frames <= 0 or b_frames <= 0:
        return 0.0
    pa, pb = a_lost / a_frames, b_lost / b_frames
    p = (a_lost + b_lost) / (a_frames + b_frames)
    se = math.sqrt(max(p, 1e-4) * dispersion * (1 / a_frames + 1 / b_frames))
    return (pb - pa) / se


class Sprt:
    """H0: loss rate p0. H1: loss rate p1. add() returns 1 for H1, 0 for H0, None while undecided."""

    def __init__(self, p0, p1, dispersion, min_reports=0, undecided=None, alpha=0.05, beta=0.05):
        self.p0, self.p1 = max(p0, 1e-3), max(p1, 2e-3)
        if self.p0 / 1.2 < self.p1 < self.p0 * 1.2:
            self.p1 = self.p0 * 1.2 if self.p1 >= self.p0 else self.p0 / 1.2
        self.a, self.b = math.log((1 - beta) / alpha), math.log(beta / (1 - alpha))
        self.d = dispersion
        self.min = min_reports
        self.max = min(20, max(6, int(round(1.5 * dispersion))))
        self.undecided = undecided
        self.v = self.n = self.frames = self.lost = 0.0

    def add(self, expected, lost, weight):
        self.n += weight
        self.frames += expected * weight
        self.lost += lost * weight
        self.v += weight * llr(expected, lost, self.p0, self.p1, self.d)
        if self.v >= self.a:
            return 1
        if self.v <= self.b and self.n >= self.min:
            return 0
        if self.n >= max(self.max, self.min):
            if self.undecided is not None:
                return self.undecided
            return 1 if self.v > 0 else 0
        return None

    def rate(self):
        return self.lost / self.frames if self.frames else 0.0


class Controller:
    MIN_LOSS = 0.002          # the smallest loss rate any test assumes
    REACT_EXCESS = 0.015      # loss this far over the expected level is worth a probe
    MILD_LOSS = 0.045         # deep-sample loss not clearly this far over the background is rate-independent
    ABSORB_EXCESS = 0.02      # while backing off, loss this close to the background only updates it
    CLIMB_LOSSY = 0.03        # a raise is lossy at this much loss over its floor
    FIRST_STEP, STEP = 0.6, 0.5
    PROBE_DEPTH = 8
    BISECT_DONE = 1.06
    CLIMB_STEP, CLIMB_FAR = 1.25, 1.5
    RETRY_REPORTS = 20
    FIRST_RETRY_REPORTS = 8   # the first retry after a hold begins, for a link that recovered meanwhile
    LOSSY_MIN = 0.03          # loss under 3% never starts a probe
    DUTY_WINDOW, DUTY_NEEDED = 3, 2   # a probe needs two lossy reports among the last three
    MISSING_MS = 1500         # no report this long while probing down: step to the deep sample
    RECHECK_CEILING = 1.25    # a clean step this close under the ceiling rechecks the ceiling
    HOLD_CHECK_REPORTS = 12
    BACKOFF_REPORTS = 1200
    BACKGROUND_DECAY_REPORTS = 60
    CUSUM_H = 4.0
    BASELINE_REPORTS = 40
    WARMUP_REPORTS = 10
    REPORT_GAP_MS = 2500
    PAUSE_TAKE_BACK_MS = 2500
    QUEUE_RETRY_REPORTS = 4
    RTT_WINDOW_S = 60
    RTT_STANDING_S = 8
    RTT_BUILD_MS_S = 2.0
    RTT_BUILD_FAIL_S = 2
    RTT_RISES = 2
    RTT_DRAIN_S = 6
    RTT_CUT = 0.85
    RTT_MAX_CUTS = 3
    RTT_SUPPRESS_S = 300

    def __init__(self, base, floor=2000):
        self.base, self.floor, self.target = base, floor, base
        self.state = NORMAL
        self.log = []
        # loss evidence
        self.after_change = None
        self.last_report_ms = None
        self.background = 0.0
        self.backoff = 0
        self.cusum = 0.0
        self.run = []
        self.duty = deque(maxlen=self.DUTY_WINDOW)
        self.base_loss = self.base_m2 = 0.0
        self.base_disp = 1.0
        self.base_n = self.disp_n = 0
        self._baseline_before_run = None
        self.normal_stats = [0, 0]
        self.normal_reports = 0
        # search
        self.episode_start = self.r0 = base
        self.l0 = 0.0
        self.phase = 'first'
        self.hi = self.lo = None
        self.hold_failed_kbps = None
        # A queue ceiling displaced by a loss probe; its lower hold cannot retire it.
        self.retained_queue_ceiling = None
        self.hi_loss = self.lo_loss = 0.0
        self.hi_kind = 'loss'
        self.hi_stats = [0, 0]
        self.probe_stats = [0, 0]
        self.deep_stats = [0, 0]
        self.hold_stats = [0, 0]
        self.hold_reports = 0
        self.test = None
        self.retry_wait = 0
        self.retries = 0
        self.blind_step = False
        self.hi_rechecked = False
        self.gentle_climb = False
        # rtt
        self.rtt_min = deque(maxlen=self.RTT_WINDOW_S)
        self.sec = None
        self.sec_vals = []
        self.sec_meds = deque(maxlen=4)
        self.standing_run = self.building_run = 0
        self.last_rtt = None
        self.rises = 0
        self.rtt_suppress = 0
        self.rtt_cut_wait = self.rtt_cuts = 0
        self.rtt_cut_lossy = False
        self.rtt_from = self.rtt_start = base

    # ------------------------------------------------------------ helpers

    def disp(self):
        return min(10.0, max(1.0, self.base_disp))

    def sprt(self, p0, p1, min_reports=0, undecided=None):
        return Sprt(p0, p1, max(2.0, self.disp()), min_reports, undecided)

    def upward_min(self):
        # A queue or a just-failed hold can appear clean before its queue refills and loss arrives.
        return self.QUEUE_RETRY_REPORTS if self.hi_kind == 'queue' or self.hold_failed_kbps is not None else 0

    def p_background(self):
        return max(self.background, self.MIN_LOSS)

    def clear_loss_run(self, restore_baseline):
        if restore_baseline and self._baseline_before_run is not None:
            (self.base_loss, self.base_m2, self.base_disp,
             self.base_n, self.disp_n) = self._baseline_before_run
        self._baseline_before_run = None
        self.cusum = 0.0
        self.run = []
        self.duty.clear()

    def note_baseline(self, expected, lost):
        a = 1.0 / min(self.BASELINE_REPORTS, self.base_n + 1)
        self.base_n += 1
        x = lost / expected
        self.base_loss = (1 - a) * self.base_loss + a * x
        self.base_m2 = (1 - a) * self.base_m2 + a * x * x
        self.disp_n = min(self.disp_n + 1, self.BASELINE_REPORTS)
        if self.disp_n >= self.WARMUP_REPORTS:
            var = max(0.0, self.base_m2 - self.base_loss ** 2)
            self.base_disp = var * expected / max(self.base_loss, 0.5 / expected)

    def reset_baseline(self, loss):
        # A selected hold or rate-independent result resolves the candidate, retaining its burstiness.
        self.clear_loss_run(False)
        # Keep the measured burstiness; move only the level.
        var = max(0.0, self.base_m2 - self.base_loss ** 2)
        self.base_loss = loss
        self.base_m2 = var + loss * loss
        self.base_n = self.WARMUP_REPORTS

    def set_target(self, kbps, why, now):
        kbps = int(max(self.floor, min(self.base, kbps)))
        if kbps == self.target:
            return
        # Only an actual cut rejects the candidate's baseline samples. A raise retains what was learned.
        self.clear_loss_run(kbps < self.target)
        self.log.append((now // 1000, self.target // 1000, kbps // 1000, why))
        self.after_change = 'up' if kbps > self.target else 'down'
        if kbps < self.target:
            self.standing_run = self.building_run = 0
        self.target = kbps

    def to_normal(self, now, kbps, why):
        self.retained_queue_ceiling = None
        self.gentle_climb = False
        if kbps >= self.base:
            self.hold_failed_kbps = None
        self.state = NORMAL
        self.hi = self.lo = None
        self.test = None
        self.set_target(kbps, why, now)

    def rate_independent(self, rate, now, why):
        self.hold_failed_kbps = None
        self.background = max(self.background, rate)
        self.reset_baseline(self.background)
        self.backoff = self.BACKOFF_REPORTS
        self.to_normal(now, max(self.r0, self.episode_start), why)

    def probe_rate(self):
        frames, lost = self.probe_stats
        return lost / frames if frames else self.l0

    # ------------------------------------------------------------ loss search

    def begin_descent(self, now, why, l0):
        if self.state == NORMAL:
            self.episode_start = self.target
        if self.state == HOLD:
            self.hold_failed_kbps = self.target
            # Keep the higher queue bound separate from this loss probe's verification ceiling.
            if (self.hi_kind == 'queue' and self.hi is not None and self.hi > self.target
                    and (self.retained_queue_ceiling is None or self.hi < self.retained_queue_ceiling)):
                self.retained_queue_ceiling = self.hi
        self.r0, self.l0 = self.target, l0
        self.phase = 'first'
        self.blind_step = False
        self.retries = 0
        self.hi, self.hi_loss, self.hi_kind = self.target, l0, 'loss'
        self.probe_stats = [0, 0]
        self.hi_stats = [sum(e for e, _ in self.run), sum(l for _, l in self.run)]
        self.state = DESCEND
        self.set_target(self.target * self.FIRST_STEP, f'descend:{why}', now)
        self.test = self.sprt(self.l0, max(self.l0 / 3, self.MIN_LOSS), undecided=0)

    def descend_result(self, helped, now):
        if helped:
            self.lo, self.lo_loss = self.target, self.test.rate()
            self.state = VERIFY_HI
            self.set_target(self.hi, 'verify_hi', now)
            self.test = self.sprt(max(self.lo_loss, self.p_background()),
                                  max(self.hi_loss * 0.6, self.lo_loss + self.CLIMB_LOSSY), self.upward_min())
            return
        rate = self.test.rate()
        if self.phase == 'first':
            self.hi, self.hi_loss = self.target, max(rate, self.l0 * 0.5)
            self.hi_stats = [self.test.frames, self.test.lost]
            deep = max(2 * self.floor, self.r0 / self.PROBE_DEPTH)
            if deep >= self.target * 0.9:
                self.rate_independent(self.probe_rate(), now, 'rate_independent')
                return
            self.phase = 'deep'
            self.deep_stats = [0, 0]
            self.set_target(deep, 'descend_deep', now)
            self.test = self.sprt(self.hi_loss, max(self.hi_loss / 3, self.MIN_LOSS), undecided=0)
            return
        if self.phase == 'deep':
            self.deep_stats[0] += self.test.frames
            self.deep_stats[1] += self.test.lost
            frames, lost = self.deep_stats
            r = lost / frames
            se = math.sqrt(max(r, 1e-4) * max(2.0, self.disp()) / frames)
            if r - 2 * se - self.background <= self.MILD_LOSS:
                # The same loss far under the rate, not clearly heavier: it does not depend on the rate.
                self.rate_independent(self.probe_rate(), now, 'rate_independent')
                return
        # Heavy loss even far down: keep halving, as far as the floor.
        self.phase = 'down'
        self.hi, self.hi_loss = self.target, max(rate, self.hi_loss * 0.5)
        self.hi_stats = [self.test.frames, self.test.lost]
        if self.target <= self.floor:
            self.enter_hold(now, self.target)
            return
        self.set_target(self.target * self.STEP, 'descend', now)
        self.test = self.sprt(self.hi_loss, max(self.hi_loss / 3, self.MIN_LOSS), undecided=0)

    def verify_hi_result(self, lossy, now):
        if not lossy:
            self.clean_promotion()
            self.lo = self.target
            if self.retained_queue_ceiling is not None and self.target < self.retained_queue_ceiling:
                # Resume the still-unverified queue bracket without inventing loss samples.
                # The failed-hold dwell keeps its own existing lifetime.
                self.hi, self.retained_queue_ceiling = self.retained_queue_ceiling, None
                self.hi_kind = 'queue'
                self.hi_loss = self.CLIMB_LOSSY
                self.hi_stats = [0, 0]
                self.hi_rechecked = False
                self.state = BISECT
                self.bisect_step(now)
                return
            # A queue ceiling that held clean may still be the link: a queue there builds slower than a
            # retry can see, so the first step above it is a small one.
            self.gentle_climb = self.hi_kind == 'queue' or self.retained_queue_ceiling is not None
            self.retained_queue_ceiling = None
            self.hi = None
            self.hi_kind = 'loss'   # the ceiling held; climbs above it need no queue dwell
            self.state = CLIMB
            self.climb_step(now)
            return
        self.hi_loss = max(self.test.rate(), self.hi_loss)
        self.hi_stats = [self.hi_stats[0] + self.test.frames, self.hi_stats[1] + self.test.lost]
        self.hi_rechecked = True
        self.state = BISECT
        self.bisect_step(now)

    def bisect_step(self, now):
        if self.hi is None or self.hi / self.lo <= self.BISECT_DONE:
            self.enter_hold(now, self.lo)
            return
        self.set_target(math.sqrt(self.lo * self.hi), 'bisect', now)
        self.test = self.sprt(max(self.lo_loss, self.p_background()),
                              max(self.hi_loss * 0.6, self.lo_loss + self.CLIMB_LOSSY),
                              self.QUEUE_RETRY_REPORTS if self.hold_failed_kbps is not None else 0)

    def bisect_result(self, lossy, now):
        if lossy:
            self.hi, self.hi_loss = self.target, max(self.test.rate(), self.hi_loss * 0.5)
            self.hi_stats = [self.test.frames, self.test.lost]
            self.hi_rechecked = False
        else:
            self.clean_promotion()
            self.lo = self.target
            if (self.hi is not None and not self.hi_rechecked
                    and self.BISECT_DONE < self.hi / self.lo < self.RECHECK_CEILING):
                # Clean this close under the ceiling: the ceiling may have gone, so try it before narrowing in.
                self.hi_rechecked = True
                self.state = VERIFY_HI
                self.set_target(self.hi, 'recheck_ceiling', now)
                self.test = self.sprt(max(self.lo_loss, self.p_background()),
                                      max(self.hi_loss * 0.6, self.lo_loss + self.CLIMB_LOSSY), self.upward_min())
                return
        self.bisect_step(now)

    def clean_promotion(self):
        # Clean verify/bisect tests require the full dwell while this is set. Lower rates cannot clear it.
        if self.hold_failed_kbps is not None and self.target >= self.hold_failed_kbps:
            self.hold_failed_kbps = None

    def enter_hold(self, now, kbps):
        self.state = HOLD
        self.reset_baseline(self.lo_loss)
        self.set_target(kbps, 'hold', now)
        self.lo = self.target
        self.retry_wait = self.FIRST_RETRY_REPORTS if self.retries == 0 else self.RETRY_REPORTS
        self.hold_stats = [0, 0]
        self.hold_reports = 0
        self.test = None

    def climb_step(self, now):
        if self.target >= self.base:
            self.to_normal(now, self.base, 'base')
            return
        step = (self.BISECT_DONE if self.gentle_climb else
                self.CLIMB_FAR if self.target * 2 < self.base else self.CLIMB_STEP)
        self.gentle_climb = False
        self.set_target(self.target * step, 'climb', now)
        p0 = max(self.lo_loss, self.p_background())
        self.test = self.sprt(p0, p0 + self.CLIMB_LOSSY)

    def climb_result(self, lossy, now):
        if lossy:
            self.hi, self.hi_loss = self.target, self.test.rate()
            self.hi_stats = [self.test.frames, self.test.lost]
            self.state = BISECT
            self.bisect_step(now)
        else:
            self.lo = self.target
            self.climb_step(now)

    def step_result(self, lossy, now):
        {VERIFY_HI: self.verify_hi_result, BISECT: self.bisect_result,
         CLIMB: self.climb_result}[self.state](lossy, now)

    # ------------------------------------------------------------ reports

    def report(self, t_ms, expected, lost):
        gap = self.last_report_ms is not None and t_ms - self.last_report_ms > self.REPORT_GAP_MS
        self.last_report_ms = t_ms
        if expected <= 0:
            return
        first = self.after_change
        self.after_change = None
        if gap or first is not None:
            # The first report after either direction still covers the previous rate. Even half of a
            # clean report can approve a retry against severe prior loss before its queue builds.
            # A gap alone leaves the candidate and its snapshot pending: RTT may still confirm a cut.
            return
        if self.backoff:
            self.backoff -= 1
        st = self.state
        if st == DESCEND:
            self.probe_stats[0] += expected
            self.probe_stats[1] += lost
        if st == DESCEND or st in UPWARD:
            res = self.test.add(expected, lost, 1.0)
            if res is not None:
                if st == DESCEND:
                    self.descend_result(res == 1, t_ms)
                else:
                    self.step_result(res == 1, t_ms)
            return
        if st == RTT_CUT:
            loss = lost / expected
            if loss > self.p_background() + self.REACT_EXCESS:
                self.rtt_cut_lossy = True
            if loss > max(0.2, self.p_background() + self.MILD_LOSS):
                # Heavy loss is stronger evidence than the queue: the loss probe takes it from here.
                self.state = HOLD
                self.run = [(expected, lost), (expected, lost)]
                self.begin_descent(t_ms, 'loss_in_rtt_cut', loss)
            return
        self.watch(t_ms, expected, lost)

    def watch(self, t_ms, expected, lost):
        """NORMAL or HOLD: look for a change in loss, keep the background honest, retry what is above."""
        st = self.state
        warm = self.base_n >= self.WARMUP_REPORTS
        p0 = max(self.base_loss if warm else 0.0, self.p_background())
        if st == HOLD:
            p0 = max(p0, self.lo_loss)
        p1 = max(2.5 * p0, p0 + self.REACT_EXCESS)
        lossy_at = max(self.LOSSY_MIN, p0 + self.REACT_EXCESS / 2.0)
        lossy_report = lost / expected >= lossy_at
        self.cusum = max(0.0, self.cusum + llr(expected, lost, p0, p1, max(2.0, self.disp())))
        if self.cusum == 0.0:
            self._baseline_before_run = None
            self.run = []
        else:
            if self._baseline_before_run is None:
                self._baseline_before_run = (self.base_loss, self.base_m2, self.base_disp,
                                             self.base_n, self.disp_n)
            self.run.append((expected, lost))
        # Recurring bursts must teach both the level and dispersion. A real cut restores the snapshot.
        self.note_baseline(expected, lost)
        if st == NORMAL:
            self.normal_stats[0] += expected
            self.normal_stats[1] += lost
            self.normal_reports += 1
            if self.normal_reports >= self.BACKGROUND_DECAY_REPORTS:
                rate = self.normal_stats[1] / self.normal_stats[0]
                if rate * 1.2 < self.background:
                    self.background = rate * 1.2   # background loss that has gone
                self.normal_stats = [0, 0]
                self.normal_reports = 0
        else:
            self.hold_stats[0] += expected
            self.hold_stats[1] += lost
            self.hold_reports += 1
            if self.hold_reports >= self.HOLD_CHECK_REPORTS:
                frames, lst = self.hold_stats
                rate = lst / frames
                hf, hl = self.hi_stats
                real_loss = self.hi_kind == 'loss' and hf > 0 and hl / hf >= self.background + self.REACT_EXCESS
                if self.hi is not None and real_loss and z_lower(lst, frames, hl, hf, max(2.0, self.disp())) < 2.0:
                    # The lower rate is no better: the loss does not depend on the rate after all.
                    self.rate_independent(rate, t_ms, 'hold_no_better')
                    return
                self.lo_loss = rate
                if rate * 1.2 < self.background:
                    self.background = rate * 1.2
                self.hold_stats = [0, 0]
                self.hold_reports = 0
        self.duty.append(lossy_report)
        lossy = sum(self.duty)
        if self.cusum >= self.CUSUM_H and lossy >= self.DUTY_NEEDED:
            frames = sum(e for e, _ in self.run)
            mean = sum(l for _, l in self.run) / frames
            if self.backoff and mean <= self.background + self.ABSORB_EXCESS:
                self.background = max(self.background, mean)
                self._baseline_before_run = None
                self.cusum = 0.0
                self.run = []
                return
            self.begin_descent(t_ms, 'loss' if st == NORMAL else 'loss_at_hold', mean)
            return
        if st == NORMAL and self.target < self.base and self.cusum == 0.0:
            self.lo, self.hi, self.lo_loss = self.target, None, max(self.base_loss, self.p_background())
            self.state = CLIMB
            self.climb_step(t_ms)
            return
        # One existing duty window can confirm a new candidate before a due automatic raise.
        probe_deferred = self.cusum > 0.0 and len(self.run) < self.DUTY_WINDOW
        if st == HOLD and self.hi is None and self.target < self.base:
            # Nothing above is known to fail: carry on climbing.
            if self.retry_wait > 0:
                self.retry_wait -= 1
            if self.retry_wait == 0 and not probe_deferred:
                self.state = CLIMB
                self.climb_step(t_ms)
            return
        if st == HOLD and self.hi is not None:
            if self.retry_wait > 0:
                self.retry_wait -= 1
            if self.retry_wait == 0 and not probe_deferred:
                self.retries += 1
                self.state = VERIFY_HI
                self.set_target(self.hi, 'retry', t_ms)
                p0 = max(self.lo_loss, self.p_background())
                self.test = self.sprt(p0, p0 + self.CLIMB_LOSSY, self.upward_min())

    # ------------------------------------------------------------ rtt

    def raising(self):
        return self.state in UPWARD and self.lo is not None and self.target > self.lo

    def ping(self, t_ms, rtt):
        if self.last_rtt is not None and rtt != self.last_rtt:
            self.rises = self.rises + 1 if rtt > self.last_rtt else 0
        self.last_rtt = rtt
        floor = min(self.rtt_min) if self.rtt_min else None
        if (self.raising() and self.rises >= self.RTT_RISES and floor is not None and
                rtt >= floor + max(10.0, 0.5 * floor)):
            # The smoothed RTT rose at its last two changes, to a queue's height: this step is building one.
            self.rises = 0
            self.hi_kind = 'queue'
            self.step_result(True, t_ms)
        self.tick(t_ms)
        sec = t_ms // 1000
        if self.sec is None:
            self.sec = sec
        if sec != self.sec:
            self.close_second(t_ms)
            self.sec, self.sec_vals = sec, []
        self.sec_vals.append(rtt)

    def tick(self, t_ms):
        """Host time passed with no reading: a raise no report can judge is taken back."""
        if (self.state == DESCEND and self.phase == 'first' and not self.blind_step
                and self.last_report_ms is not None and t_ms - self.last_report_ms > self.MISSING_MS):
            # The probe's first step cannot be judged: the reports stopped, maybe because the link is
            # overloaded. Take the deep sample now, once, rather than ride the rate for the whole pause.
            deep = max(2 * self.floor, self.r0 / self.PROBE_DEPTH)
            if deep < self.target * 0.9:
                self.blind_step = True
                self.hi, self.hi_loss = self.target, max(self.hi_loss, self.l0 * 0.5)
                self.phase = 'deep'
                self.deep_stats = [0, 0]
                self.set_target(deep, 'descend_deep_unjudged', t_ms)
                self.test = self.sprt(self.hi_loss, max(self.hi_loss / 3, self.MIN_LOSS), undecided=0)
        if (self.raising() and self.last_report_ms is not None
                and t_ms - self.last_report_ms > self.PAUSE_TAKE_BACK_MS):
            # No report can judge this raise: take it back without counting it as a failure,
            # and try it again once reports are back.
            tried = self.target
            if self.hi is None or tried < self.hi:
                self.hi, self.hi_kind, self.hi_stats = tried, 'loss', [0, 0]
            self.enter_hold(t_ms, self.lo)
            self.retry_wait = 3

    def close_second(self, now):
        if not self.sec_vals:
            return
        vals = sorted(self.sec_vals)
        med = vals[len(vals) // 2]
        floor_rtt = min(self.rtt_min) if self.rtt_min else None
        prev = list(self.sec_meds)
        self.sec_meds.append(med)
        raised = floor_rtt is not None and med >= floor_rtt + max(10.0, 0.5 * floor_rtt)
        if not raised:
            self.rtt_min.append(vals[0])
        # At ENet's idle cadence, an acknowledgement about every 500 ms, a drained queue still reads high for 15-20 s
        # through its smoothing. A raised RTT that is not falling is a standing queue; one climbing 2 ms/s or more is
        # a queue being built now.
        standing = raised and len(prev) >= 3 and med >= prev[-3] - 0.5
        building = raised and len(prev) >= 2 and (med - prev[-2]) / 2 >= self.RTT_BUILD_MS_S
        self.standing_run = self.standing_run + 1 if standing else 0
        self.building_run = self.building_run + 1 if building else 0
        if self.rtt_suppress:
            self.rtt_suppress -= 1
        if self.raising() and self.building_run >= self.RTT_BUILD_FAIL_S:
            # This raise is building a queue: it fails, whatever the loss says.
            self.building_run = 0
            self.hi_kind = 'queue'
            self.step_result(True, now)
            return
        if self.state == RTT_CUT:
            if not standing:
                # The queue is draining: the rate before the cut was over the link.
                self.lo, self.lo_loss = self.target, max(self.lo_loss, self.p_background())
                self.hi, self.hi_loss, self.hi_kind = self.rtt_from, self.CLIMB_LOSSY, 'queue'
                self.hi_stats = [0, 0]
                self.enter_hold(now, self.target)
                return
            self.rtt_cut_wait -= 1
            if self.rtt_cut_wait <= 0:
                if self.rtt_cuts >= self.RTT_MAX_CUTS and self.rtt_cut_lossy:
                    # Our own queue three cuts did not drain: the loss probe takes it from here.
                    self.lo, self.lo_loss = self.target, max(self.lo_loss, self.p_background())
                    self.enter_hold(now, self.target)
                    return
                if self.rtt_cuts >= self.RTT_MAX_CUTS:
                    # Cutting did not drain it and nothing was lost: someone else's queue.
                    self.rtt_suppress = self.RTT_SUPPRESS_S
                    self.to_normal(now, self.rtt_start, 'rtt_not_ours')
                    return
                self.rtt_cut(now)
            return
        if self.standing_run >= self.RTT_STANDING_S and not self.rtt_suppress and self.state in (NORMAL, HOLD, CLIMB):
            self.standing_run = 0
            self.rtt_start = self.target if self.state != HOLD else self.r0
            self.rtt_cuts = 0
            self.rtt_cut_lossy = False
            self.state = RTT_CUT
            self.rtt_cut(now)

    def rtt_cut(self, now):
        self.rtt_from = self.target
        self.rtt_cuts += 1
        self.rtt_cut_wait = self.RTT_DRAIN_S
        self.set_target(self.target * self.RTT_CUT, 'rtt', now)

    def adopt(self, now, kbps):
        self.hold_failed_kbps = None
        previous = self.target
        kbps = max(min(self.floor, self.base), min(self.base, kbps))
        self.clear_loss_run(kbps < self.target)
        if kbps != self.target:
            self.after_change = 'up' if kbps > self.target else 'down'
            self.target = kbps
        self.test = None
        self.standing_run = self.building_run = 0
        if self.target >= self.base:
            self.state = NORMAL
            self.hi = self.lo = None
            return
        self.state = HOLD
        self.lo = self.target
        self.lo_loss = max(self.lo_loss, self.p_background())
        self.hi = previous if previous > self.target else None
        self.hi_kind = 'loss'
        self.hi_stats = [0, 0]
        self.reset_baseline(self.lo_loss)
        self.retry_wait = self.RETRY_REPORTS
        self.hold_stats = [0, 0]
        self.hold_reports = 0
        self.r0 = max(previous, self.target)

    def rebase(self, base, floor):
        self.base = max(1, base)
        self.floor = min(floor, self.base)
        target = max(self.floor, min(self.base, self.target))
        if target != self.target:
            self.hold_failed_kbps = None
            self.clear_loss_run(target < self.target)
            self.after_change = 'up' if target > self.target else 'down'
            if target < self.target:
                self.standing_run = self.building_run = 0
            self.target = target
        self.episode_start = min(self.episode_start, self.base)
        self.r0 = min(self.r0, self.base)
        self.rtt_start = min(self.rtt_start, self.base)
        if self.hi is not None and self.hi > self.base:
            self.hi = self.base
        if self.state == HOLD and self.hi is not None and self.hi <= self.target:
            self.hi = None


# ---------------------------------------------------------------- replays
#
# One simulated second carries ten RTT readings, at 50, 150, ... 950 ms, and one media report at 500 ms. The
# feed sees the target the second starts with. tests/unit/test_live_tuning_policy.cpp runs the same harness.

BASE = 268988
FLOOR = 2000
HEVC = [(120, 0), (122, 9), (120, 0), (120, 0), (120, 0), (120, 0), (121, 9), (120, 0)]


def enet_smooth(s, raw):
    """enet/protocol.c: roundTripTime moves 1/8 of the way to the sample, in whole milliseconds."""
    s, r = int(round(s)), max(int(round(raw)), 1)
    if r >= s:
        return s + (r - s + 7) // 8
    return s - (s - r + 7) // 8


def run(seconds, feed, rtt=None, quiet=lambda i: False):
    policy = Controller(BASE, FLOOR)
    rtt = rtt or (lambda t_ms, target: 3.0)
    targets = []
    for i in range(seconds):
        t0 = i * 1000
        expected, lost = feed(i, policy.target)
        for k in range(10):
            at = t0 + 100 * k + 50
            if k == 5 and not quiet(i):
                policy.report(t0 + 500, expected, lost)
            policy.ping(at, rtt(at, policy.target))
        targets.append(policy.target)
    return targets


def link(kbps, over=(120, 6), under=(120, 0), start=25, recover_at=None):
    def feed(i, target):
        lk = kbps if i >= start and (recover_at is None or i < recover_at) else 10 ** 9
        return over if target > lk else under
    return feed


def phases(*spans):
    seq = []
    for n, expected, lost in spans:
        seq += [(expected, lost)] * n
    return lambda i, target: seq[i] if i < len(seq) else (120, 0)


def queue(kbps, start=25):
    """Tail drop behind a 120 ms buffer: loss grows with the overload, and so does the queue."""
    state = {'q': 0.0, 's': 3.0, 'last': -1000}

    def feed(i, target):
        overload = max(0.0, 1.0 - kbps / target) if i >= start else 0.0
        return 120, int(120 * min(1.0, 6.0 * overload))

    def rtt(t_ms, target):
        lk = kbps if t_ms // 1000 >= start else 10 ** 9
        state['q'] = min(120.0, max(0.0, state['q'] + 100.0 * (target - lk) / lk))
        if t_ms - state['last'] >= 500:
            state['s'] = enet_smooth(state['s'], 3.0 + state['q'])
            state['last'] = t_ms
        return state['s']
    return feed, rtt


def scenarios(deck_scan):
    pauses = set(range(150, 170)) | set(range(300, 335))
    scan = lambda t_ms, target: deck_scan[(t_ms // 100) % len(deck_scan)]
    q100, q30 = queue(100000), queue(30000)
    return {
        'link_100': (600, link(100000), None, None),
        'link_50': (600, link(50000), None, None),
        'link_100_with_background': (600, link(100000, over=(120, 7), under=(120, 1)), None, None),
        'link_200': (600, link(200000), None, None),
        'rp6_hevc': (1200, lambda i, target: HEVC[i % 8], None, None),
        'burst_10s_at_5_percent': (90, phases((20, 120, 0), (10, 120, 6), (60, 120, 0)), None, None),
        'burst_3s_at_20_percent': (90, phases((20, 120, 0), (3, 120, 24), (60, 120, 0)), None, None),
        'link_100_recovers_at_200': (400, link(100000, recover_at=200), None, None),
        'link_100_with_report_pauses': (600, link(100000), None, lambda i: i in pauses),
        'deck_scan': (1230, lambda i, target: (120, 0), scan, None),
        'queue_link_100': (600, q100[0], q100[1], None),
        'queue_link_30': (600, q30[0], q30[1], None),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--write', type=Path, required=True, help='fixture to write')
    parser.add_argument('--deck-scan', type=Path, help='ENet-smoothed RTT per 100 ms over one scan period')
    args = parser.parse_args()
    deck_scan = json.loads(args.deck_scan.read_text()) if args.deck_scan else json.loads(args.write.read_text())['deck_scan_rtt_ms']
    replays = {}
    for name, (seconds, feed, rtt, quiet) in scenarios(deck_scan).items():
        replays[name] = run(seconds, feed, rtt, quiet or (lambda i: False))
    fixture = {
        'comment': 'Generated by tools/tests/live_tuning_policy_reference.py; do not edit by hand.',
        'base_kbps': BASE,
        'floor_kbps': FLOOR,
        'deck_scan_rtt_ms': deck_scan,
        'targets_kbps': replays,
    }
    args.write.write_text(json.dumps(fixture, separators=(',', ':')) + '\n')


if __name__ == '__main__':
    main()
