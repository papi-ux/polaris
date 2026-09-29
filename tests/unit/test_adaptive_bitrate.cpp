/**
 * @file tests/unit/test_adaptive_bitrate.cpp
 * @brief Unit tests for adaptive bitrate controller behavior.
 */

#include "src/adaptive_bitrate.h"
#include "src/config.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <future>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
  void enable_controller(int base_bitrate_kbps = 20000) {
    config::video.adaptive_bitrate.enabled = true;
    config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
    config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
    adaptive_bitrate::load_config();
    adaptive_bitrate::reset();
    adaptive_bitrate::set_runtime_update_supported(true);
    adaptive_bitrate::set_base_bitrate(base_bitrate_kbps);
  }
}

TEST(AdaptiveBitrateController, ReducesTargetOnNetworkPressure) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_LT(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_EQ("network_pressure", state.state);
}

TEST(AdaptiveBitrateController, AReadingWithoutLossLeavesTheLossAverageWhereItIs) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  adaptive_bitrate::update_network_stats(6.0, 8.0);
  const double after_loss = adaptive_bitrate::get_state().ewma_packet_loss;
  ASSERT_GT(after_loss, 0.0);

  // A second of control pings carries RTT and nothing about video.
  for (int i = 0; i < 10; ++i) {
    adaptive_bitrate::update_network_stats(std::nullopt, 8.0);
  }
  EXPECT_DOUBLE_EQ(adaptive_bitrate::get_state().ewma_packet_loss, after_loss);
}

namespace {
  /// One client media report a second for `seconds`, its loss given by the rate Live Tuning holds.
  /// Returns the target after each second.
  template<class Loss>
  std::vector<int> live_tuning_seconds(int seconds, Loss loss_at) {
    std::vector<int> targets;
    for (int second = 0; second < seconds; ++second) {
      adaptive_bitrate::age_for_tests(1s);
      adaptive_bitrate::update_network_stats(loss_at(second, adaptive_bitrate::get_state().target_bitrate_kbps), 8.0);
      targets.push_back(adaptive_bitrate::get_state().target_bitrate_kbps);
    }
    return targets;
  }

  std::string targets_text(const std::vector<int> &targets) {
    std::string text;
    for (std::size_t i = 0; i < targets.size(); ++i) {
      text += std::to_string(i) + ":" + std::to_string(targets[i]) + " ";
    }
    return text;
  }

  /// Leave no cut target behind for a later suite in this binary to read as a reduced stream.
  void leave_controller_clean() {
    adaptive_bitrate::reset();
    adaptive_bitrate::set_enabled(false);
    config::video.adaptive_bitrate.enabled = false;
  }
}  // namespace

namespace {
  struct run_t {
    std::size_t start;
    std::size_t length;
  };

  /// Each stretch of seconds the target spent above kbps, from `from` on.
  std::vector<run_t> runs_above(const std::vector<int> &targets, int kbps, std::size_t from = 0) {
    std::vector<run_t> runs;
    for (std::size_t i = from; i < targets.size(); ++i) {
      if (targets[i] <= kbps) {
        continue;
      }
      if (!runs.empty() && runs.back().start + runs.back().length == i) {
        ++runs.back().length;
      } else {
        runs.push_back({i, 1});
      }
    }
    return runs;
  }

  /// Each second the target moved up after moving down, and how long after the move down it came.
  std::vector<std::size_t> waits_after_a_step_back(const std::vector<int> &targets, std::size_t from) {
    std::vector<std::size_t> waits;
    std::optional<std::size_t> back;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < targets.size(); ++i) {
      if (targets[i] < targets[i - 1]) {
        back = i;
      } else if (targets[i] > targets[i - 1] && back) {
        waits.push_back(i - *back);
        back.reset();
      }
    }
    return waits;
  }
}  // namespace

TEST(AdaptiveBitrateController, ModerateLossNoLowerRateLowersProbesDownOnceAndClimbsBack) {
  // 3% of every second's frames lost at any rate. Moderate loss cuts to half the base and holds there,
  // and the steady loss there starts a probe that halves the rate after each dwell, down to the
  // configured minimum. No step lowers the loss, so it does not depend on the rate: back to the hold,
  // up by tested steps to the base, where that loss holds the rate, and no second probe for 5 minutes.
  // A single step below, back and up again left it to be probed every time it came back. Heavy loss is
  // a link too small for the stream and still cuts at once.
  enable_controller(40000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto targets = live_tuning_seconds(400, [](int, int) { return 3.0; });
  const auto trace = targets_text(targets);
  const auto held = std::find(targets.begin(), targets.end(), 20000);
  ASSERT_NE(held, targets.end()) << trace;
  EXPECT_GE(std::find_if(held, targets.end(), [](int target) { return target != 20000; }) - held, 8) << trace;
  // Each probe step is held long enough to judge it, down to the configured minimum.
  auto step = held;
  for (const int probe : {10000, 5000, 2500, 2000}) {
    step = std::find(step, targets.end(), probe);
    ASSERT_NE(step, targets.end()) << probe << " " << trace;
    EXPECT_GE(std::find_if(step, targets.end(), [probe](int target) { return target != probe; }) - step, 8) << trace;
  }
  const auto returned = std::find(step, targets.end(), 20000);
  ASSERT_NE(returned, targets.end()) << trace;
  const auto back = std::find(returned, targets.end(), 40000);
  ASSERT_NE(back, targets.end()) << trace;
  EXPECT_LE(back - returned, 60) << trace;
  // No second probe, for the 5 minutes after the first, and the loss holds the base.
  ASSERT_GE(targets.end() - returned, 300) << trace;
  EXPECT_GE(*std::min_element(returned, targets.end()), 20000) << trace;
  EXPECT_TRUE(std::all_of(back, targets.end(), [](int target) { return target == 40000; })) << trace;
  EXPECT_EQ(adaptive_bitrate::get_state().reason, "packet_loss_holding");

  for (int second = 0; second < 3; ++second) {
    adaptive_bitrate::age_for_tests(1s);
    adaptive_bitrate::update_network_stats(20.0, 8.0);
  }
  const auto state = adaptive_bitrate::get_state();
  EXPECT_LT(state.target_bitrate_kbps, 40000);
  EXPECT_EQ(state.reason, "packet_loss");
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, AProbeThatFindsLossNoRateLowersBlocksTheNextForFiveMinutes) {
  // 3% lost at any rate, probed down to the minimum once and back at the base. The loss then rises to
  // 4.8% at any rate, past what the climb held, and cuts to the hold again. Probing it again within 5
  // minutes of the probe that found loss no rate lowers would halve the stream to the minimum a second
  // time for loss it already knows the rate does not cause: the hold climbs back by tested steps
  // instead. Once the 5 minutes are over, steady loss at the hold probes again.
  enable_controller(40000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto targets = live_tuning_seconds(460, [](int second, int) {
    return second < 90 ? 3.0 : second < 330 ? 4.8 : second < 360 ? 0.0 : 3.0;
  });
  const auto trace = targets_text(targets);
  const auto first_probe_end = std::find(targets.begin(), targets.end(), 2000);
  ASSERT_NE(first_probe_end, targets.end()) << trace;
  ASSERT_LT(first_probe_end - targets.begin(), 60) << trace;
  // The rise cuts to the hold and no lower, and the stream climbs back.
  EXPECT_EQ(*std::min_element(targets.begin() + 90, targets.begin() + 330), 20000) << trace;
  EXPECT_EQ(targets[329], 40000) << trace;
  // Past the 5 minutes, the next steady loss probes again.
  EXPECT_EQ(*std::min_element(targets.begin() + 360, targets.end()), 2000) << trace;
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, ModerateLossStepsUnderALinkBelowHalfTheBase) {
  // The link shrank to 15 Mbps under a 40 Mbps stream, and 3% of the frames are lost while the stream
  // is above it. The hold at half the base stopped every cut at 20 Mbps and kept the stream losing
  // frames for good. The steady loss there starts a probe, whose first step clears it, and the steps up
  // back toward the link are kept only while the loss stays down. A step over the link lasts one dwell.
  enable_controller(40000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto targets = live_tuning_seconds(400, [](int, int target) { return target > 15000 ? 3.0 : 0.0; });
  const auto trace = targets_text(targets);
  const auto first_under = std::find_if(targets.begin(), targets.end(), [](int target) { return target <= 15000; });
  ASSERT_NE(first_under, targets.end()) << trace;
  EXPECT_LE(first_under - targets.begin(), 30) << trace;
  // One step below the hold, and no spiral under it.
  EXPECT_GE(*std::min_element(targets.begin(), targets.end()), 10000) << trace;
  const auto over = runs_above(targets, 15000, static_cast<std::size_t>(first_under - targets.begin()));
  ASSERT_FALSE(over.empty()) << trace;
  for (std::size_t i = 0; i < over.size(); ++i) {
    EXPECT_LE(over[i].length, 8u) << trace;
    if (i > 0) {
      EXPECT_GE(over[i].start - over[i - 1].start - over[i - 1].length, 30u) << trace;
    }
  }
  // Close under the link, not left at the step below.
  EXPECT_LE(targets.back(), 15000) << trace;
  EXPECT_GT(targets.back(), 13500) << trace;
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, ModerateLossProbesPastARateThatDoesNotLowerIt) {
  // The link shrank to 8 Mbps under a 40 Mbps stream, below a quarter of it, with 3% of the frames
  // lost above it. The first step below the hold, 10 Mbps, is still over the link and loses as much,
  // and taken for loss the bitrate does not cause it sent the stream back up to 40 Mbps and the loss.
  // The steady loss keeps the probe halving, to the first rate it is gone at, and the climb back stops
  // under the link.
  enable_controller(40000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto targets = live_tuning_seconds(300, [](int, int target) { return target > 8000 ? 3.0 : 0.0; });
  const auto trace = targets_text(targets);
  const auto first_under = std::find_if(targets.begin(), targets.end(), [](int target) { return target <= 8000; });
  ASSERT_NE(first_under, targets.end()) << trace;
  EXPECT_LE(first_under - targets.begin(), 40) << trace;
  EXPECT_EQ(*std::min_element(targets.begin(), targets.end()), 5000) << trace;
  for (const auto &run : runs_above(targets, 8000, static_cast<std::size_t>(first_under - targets.begin()))) {
    EXPECT_LE(run.length, 8u) << trace;
  }
  EXPECT_LE(targets.back(), 8000) << trace;
  EXPECT_GT(targets.back(), 7200) << trace;
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, ModerateLossFindsTheRateWhereItFallsToABackground) {
  // The link shrank to 15 Mbps under a 40 Mbps stream: 3% of the frames lost above it, and 0.5% at any
  // rate below it. The probe's first step lowers the mean loss by five sixths while every report still
  // loses frames, and read as a step that did not help it sent the stream back over the link. A step
  // helps when the mean loss or the share of lossy reports falls by a third. The next step lowers
  // nothing, so the 0.5% is a background every rate has, and the rate where the loss fell to it is the
  // one found.
  enable_controller(40000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto targets = live_tuning_seconds(300, [](int, int target) { return target > 15000 ? 3.0 : 0.5; });
  const auto trace = targets_text(targets);
  const auto probed = std::find(targets.begin(), targets.end(), 5000);
  ASSERT_NE(probed, targets.end()) << trace;
  EXPECT_EQ(*std::min_element(targets.begin(), targets.end()), 5000) << trace;
  // Back to the rate above the step that lowered nothing, and up from there.
  const auto found = std::find_if(probed, targets.end(), [](int target) { return target != 5000; });
  ASSERT_NE(found, targets.end()) << trace;
  EXPECT_EQ(*found, 10000) << trace;
  for (const auto &run : runs_above(targets, 15000, static_cast<std::size_t>(found - targets.begin()))) {
    EXPECT_LE(run.length, 8u) << trace;
  }
  EXPECT_LE(targets.back(), 15000) << trace;
  EXPECT_GT(targets.back(), 13500) << trace;
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, AClimbStepsUpByEightSixteenAndThirtyTwoPercentAndBacksOffAfterFailures) {
  // From the rate a probe found, the climb steps up 8% after each dwell and doubles the step while each
  // stays clean, to 32%. A step whose loss rises past the held level's bound goes back, and the next
  // waits 30 s, doubling to 120 s while steps fail in a row.
  enable_controller(40000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto targets = live_tuning_seconds(700, [](int, int target) { return target > 15000 ? 3.0 : 0.0; });
  const auto trace = targets_text(targets);
  const auto found = std::find(targets.begin(), targets.end(), 10000);
  ASSERT_NE(found, targets.end()) << trace;
  std::vector<int> rates;
  for (auto it = found; it != targets.end() && rates.size() < 5; ++it) {
    if (rates.empty() || *it != rates.back()) {
      rates.push_back(*it);
    }
  }
  // 10000, +8%, +16%, +32% over the link, and back.
  EXPECT_EQ(rates, (std::vector<int> {10000, 10800, 12528, 16536, 12528})) << trace;
  // Each wait after a failed step, the first three after a kept step reset it.
  const auto waits = waits_after_a_step_back(targets, static_cast<std::size_t>(found - targets.begin()) + 1);
  ASSERT_GE(waits.size(), 7u) << trace;
  EXPECT_EQ(std::vector<std::size_t>(waits.begin(), waits.begin() + 7),
            (std::vector<std::size_t> {30, 30, 30, 60, 120, 120, 120})) << trace;
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, AStepUpIsKeptUpToHalfAgainTheHeldLossOrAPointAboveIt) {
  // A step up is kept unless its loss rises above max(1.5 x the held level, held level + 1 point), the
  // held level being the loss where the climb started. The probe here finds 10 Mbps, where the loss fell
  // from the hold's and a step lower does not lower it, so the level held there is exact. A step that
  // is kept but loses more than the held level is not clean, and the next is no bigger.
  struct case_t {
    double held;
    double above;
    double step_loss;
    bool kept;
  };
  for (const auto c : {case_t {3.0, 4.8, 4.5, true}, case_t {3.0, 4.8, 4.6, false},
                       case_t {0.5, 3.0, 1.5, true}, case_t {0.5, 3.0, 1.6, false}}) {
    SCOPED_TRACE(::testing::Message() << "held " << c.held << ", step " << c.step_loss);
    enable_controller(40000);
    adaptive_bitrate::update_network_stats(0.0, 8.0);
    const auto targets = live_tuning_seconds(70, [c](int, int target) {
      return target >= 20000 ? c.above : target > 10000 ? c.step_loss : c.held;
    });
    const auto trace = targets_text(targets);
    const auto first_step = std::find(targets.begin(), targets.end(), 10800);
    ASSERT_NE(first_step, targets.end()) << trace;
    const auto next = std::find_if(first_step, targets.end(), [](int target) { return target != 10800; });
    ASSERT_NE(next, targets.end()) << trace;
    // Judged after its dwell: kept and not clean, one more 8% step; failed, back to the found rate.
    EXPECT_EQ(*next, c.kept ? 11664 : 10000) << trace;
    EXPECT_GE(next - first_step, 8) << trace;
    leave_controller_clean();
  }
}

TEST(AdaptiveBitrateController, HeavyLossOnAStepUpFailsItAtOnce) {
  // A step up over a link whose loss there is heavy. Heavy loss cut 20% and forgot the climb, so the next
  // step came after one dwell, over the link again. It fails the step at once, back to the rate it
  // left, and the next waits as after any failure.
  enable_controller(40000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto targets = live_tuning_seconds(120, [](int, int target) {
    return target >= 20000 ? 3.0 : target > 15000 ? 8.0 : 0.0;
  });
  const auto trace = targets_text(targets);
  const auto over = std::find(targets.begin(), targets.end(), 16536);
  ASSERT_NE(over, targets.end()) << trace;
  const auto back = std::find_if(over, targets.end(), [](int target) { return target != 16536; });
  ASSERT_NE(back, targets.end()) << trace;
  EXPECT_EQ(*back, 12528) << trace;
  EXPECT_LT(back - over, 8) << trace;
  const auto next = std::find_if(back, targets.end(), [](int target) { return target != 12528; });
  ASSERT_NE(next, targets.end()) << trace;
  EXPECT_GE(next - back, 30) << trace;
  EXPECT_GT(*next, 12528) << trace;
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, LossTheBitrateDoesNotCauseHoldsAndClimbsBackToTheBase) {
  // 8% of the frames lost one second in four, whatever the rate: a Wi-Fi burst. Held at half the base,
  // it was halved for the rest of the stream. Loss that comes and goes gets no probe, and climbs back
  // by tested steps.
  enable_controller(40000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto targets = live_tuning_seconds(180, [](int second, int) { return second % 4 == 3 ? 8.0 : 0.0; });
  const auto trace = targets_text(targets);
  EXPECT_GE(*std::min_element(targets.begin(), targets.end()), 20000) << trace;
  const auto back = std::find(targets.begin() + 20, targets.end(), 40000);
  ASSERT_NE(back, targets.end()) << trace;
  EXPECT_LE(back - targets.begin(), 120) << trace;
  EXPECT_EQ(targets.back(), 40000) << trace;
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, PingsNeverCutForLossAndTheNextReportDoes) {
  // Loss arrives once a second and pings ten times a second. Counted as clean video, the pings pulled
  // the loss average to a few hundredths of a percent before the interval came round. Heard as
  // nothing, they left it alone but took the interval first and cut on it. A ping says nothing about
  // video, so it leaves the loss a report brought to the next report.
  enable_controller();
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  for (int i = 0; i < 3; ++i) {
    adaptive_bitrate::update_network_stats(6.0, 8.0);
  }
  adaptive_bitrate::age_for_tests(1100ms);
  for (int i = 0; i < 10; ++i) {
    adaptive_bitrate::update_network_stats(std::nullopt, 8.0);
  }
  auto state = adaptive_bitrate::get_state();
  EXPECT_GT(state.ewma_packet_loss, 1.0);
  EXPECT_EQ(state.target_bitrate_kbps, state.base_bitrate_kbps);

  adaptive_bitrate::update_network_stats(6.0, 8.0);
  state = adaptive_bitrate::get_state();
  EXPECT_LT(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_EQ(state.state, "network_pressure");
  EXPECT_EQ(state.reason, "packet_loss");
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, ALossAverageWhoseReportsStoppedDecaysAndCutsNothing) {
  // The client's media reports stop while the loss average is high: it stopped posting, or the screen
  // has no new frames. The average stayed where the last report put it and every ping cut on it for as
  // long as the silence lasted. Once the newest report is older than Doctor keeps loss for, the average
  // decays and the pings bring the stream back.
  enable_controller(40000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  for (int second = 0; second < 3; ++second) {
    adaptive_bitrate::age_for_tests(1s);
    adaptive_bitrate::update_network_stats(4.0, 8.0);
  }
  const auto after_loss = adaptive_bitrate::get_state();
  ASSERT_LT(after_loss.target_bitrate_kbps, 40000);
  ASSERT_GT(after_loss.ewma_packet_loss, 1.0);

  std::vector<int> targets;
  for (int second = 0; second < 40; ++second) {
    adaptive_bitrate::age_for_tests(1s);
    for (int ping = 0; ping < 10; ++ping) {
      adaptive_bitrate::update_network_stats(std::nullopt, 8.0);
    }
    targets.push_back(adaptive_bitrate::get_state().target_bitrate_kbps);
  }
  const auto trace = targets_text(targets);
  EXPECT_GE(*std::min_element(targets.begin(), targets.end()), after_loss.target_bitrate_kbps) << trace;
  EXPECT_LT(adaptive_bitrate::get_state().ewma_packet_loss, 0.01);
  EXPECT_EQ(targets.back(), 40000) << trace;
  leave_controller_clean();
}

TEST(AdaptiveBitrateController, IgnoresSubthresholdRelativeRttSpikeOnFastLan) {
  enable_controller();

  // Build enough history to arm relative spike detection. The final sample is
  // three times the baseline, but 3 ms is not actionable network pressure.
  for (int i = 0; i < 6; ++i) {
    adaptive_bitrate::update_network_stats(0.0, 1.0);
  }
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(0.0, 3.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_NE(state.state, "network_pressure");
}

TEST(AdaptiveBitrateController, ReducesTargetOnActionableRelativeRttSpike) {
  enable_controller();

  for (int i = 0; i < 6; ++i) {
    adaptive_bitrate::update_network_stats(0.0, 8.0);
  }
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(0.0, 55.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_LT(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_EQ(state.state, "network_pressure");
  EXPECT_EQ(state.reason, "rtt_spike");
}

TEST(AdaptiveBitrateController, TelemetryMovementInvalidatesAStaleDoctorSnapshot) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  ASSERT_LT(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  ASSERT_GT(after_pressure.revision, before_pressure.revision);
  EXPECT_GT(
    after_pressure.action_authority_revision,
    before_pressure.action_authority_revision
  );
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    before_pressure.revision,
    before_pressure.live_bitrate_kbps,
    before_pressure.max_bitrate_kbps
  ));
}

TEST(AdaptiveBitrateController, NetworkPressureAtFloorInvalidatesAStaleDoctorSnapshot) {
  enable_controller(2000);

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  ASSERT_EQ(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  ASSERT_GT(after_pressure.revision, before_pressure.revision);
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    before_pressure.revision,
    3000,
    before_pressure.max_bitrate_kbps
  ));
}

TEST(AdaptiveBitrateController, HostEvidenceInsideAdjustmentIntervalInvalidatesAStaleDoctorSnapshot) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_observation = adaptive_bitrate::get_doctor_state();
  // Production publishes the host evidence epoch before feeding the adaptive
  // loop. No interval sleep is intentional: this covers the early-return path.
  adaptive_bitrate::note_network_evidence_arrival(true);
  adaptive_bitrate::update_network_stats(0.0, 55.0);

  const auto after_observation = adaptive_bitrate::get_doctor_state();
  ASSERT_EQ(after_observation.live_bitrate_kbps, before_observation.live_bitrate_kbps);
  ASSERT_GT(after_observation.revision, before_observation.revision);
  EXPECT_EQ(
    after_observation.action_authority_revision,
    before_observation.action_authority_revision
  );
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    before_observation.revision,
    25000,
    before_observation.max_bitrate_kbps
  ));
}

TEST(AdaptiveBitrateController, VerificationEvidenceDoesNotSupersedeAnOwnedDoctorTarget) {
  enable_controller();
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());

  adaptive_bitrate::note_network_evidence_arrival(false);

  EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, *doctor_revision);
  EXPECT_TRUE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ).has_value());
  adaptive_bitrate::reset();
}

TEST(AdaptiveBitrateController, VideoRegressionRemainsLatchedForADoctorTransaction) {
  enable_controller();
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());

  adaptive_bitrate::note_doctor_video_policy_evidence(true);
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  EXPECT_TRUE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, *doctor_revision);

  const auto blocked_step =
    adaptive_bitrate::set_doctor_quality_bitrate_if_revision(
      *doctor_revision,
      18000,
      before.max_bitrate_kbps
    );
  EXPECT_EQ(
    blocked_step.status,
    adaptive_bitrate::doctor_bitrate_apply_status_e::quality_policy_blocked
  );
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, *doctor_revision);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 16000);

  EXPECT_TRUE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ).has_value());
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  adaptive_bitrate::reset();
}

TEST(AdaptiveBitrateController, NetworkRegressionBlocksTheNextDoctorQualityStep) {
  enable_controller();
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  const auto doctor_revision =
    adaptive_bitrate::set_doctor_quality_bitrate_if_revision(
      before.revision,
      16000,
      before.max_bitrate_kbps
    );
  ASSERT_EQ(
    doctor_revision.status,
    adaptive_bitrate::doctor_bitrate_apply_status_e::applied
  );

  adaptive_bitrate::note_network_evidence_arrival(true);
  adaptive_bitrate::note_network_evidence_arrival(false);
  EXPECT_TRUE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    doctor_revision.revision
  );

  const auto blocked_step =
    adaptive_bitrate::set_doctor_quality_bitrate_if_revision(
      doctor_revision.revision,
      18000,
      before.max_bitrate_kbps
    );
  EXPECT_EQ(
    blocked_step.status,
    adaptive_bitrate::doctor_bitrate_apply_status_e::quality_policy_blocked
  );
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 16000);

  EXPECT_TRUE(adaptive_bitrate::restore_doctor_state_if_revision(
    doctor_revision.revision,
    before
  ).has_value());
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  adaptive_bitrate::reset();
}

TEST(AdaptiveBitrateController, ReportsFramePacingWithoutLoweringBitrate) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.75, 0.05, 0.02, 3.0, 4.0, 28.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_EQ(state.base_bitrate_kbps, state.target_bitrate_kbps);
  EXPECT_EQ("frame_pacing_observed", state.state);
}

TEST(AdaptiveBitrateController, ReducesTargetOnEncoderPressure) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_LT(state.target_bitrate_kbps, state.base_bitrate_kbps);
  EXPECT_EQ("encoder_pressure", state.state);
}

TEST(AdaptiveBitrateController, HighRefreshEncoderPressureRequiresAnOverrunAndDeliveryShortfall) {
  struct sample_t { double fps, ratio, encode_ms; bool reduce; };
  const double unknown = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const sample_t samples[] = {
    {60.0, 0.80, 9.0, false},       // Existing low-refresh behavior.
    {60.0, 1.00, 11.0, true},
    {120.0, 0.90, 9.0, true},
    {165.0, 0.90, 6.1, true},
    {240.0, 0.80, 4.2, true},       // Old fixed threshold missed this overrun.
    {240.0, 1.00, 6.0, false},      // Isolated slow encode, healthy delivery.
    {240.0, 0.95, 6.0, false},
    {240.0, 0.50, 3.0, false},      // Pacing/capture shortage, encoder within budget.
    {240.0, 0.00, 6.0, false},      // No measured delivery yet.
    {240.0, unknown, 6.0, false},
    {0.0, 0.80, 6.0, false},
    {-240.0, 0.80, 6.0, false},
    {unknown, 0.80, 6.0, false},
    {infinity, 0.80, 6.0, false},
    {240.0, 0.80, unknown, false},
    {240.0, 0.80, infinity, false},
    {120000.0 / 1001.0, 0.80, 8.335, false}, // Do not round fractional targets.
    {120000.0 / 1001.0, 0.80, 8.345, true},
  };
  for (const auto &sample : samples) {
    SCOPED_TRACE(::testing::Message() << sample.fps << " fps, ratio=" << sample.ratio << ", work=" << sample.encode_ms);
    enable_controller();
    adaptive_bitrate::update_network_stats(0.0, 8.0);
    std::this_thread::sleep_for(1100ms);
    const auto before = adaptive_bitrate::get_doctor_state();
    adaptive_bitrate::update_stream_health(sample.ratio, 0.0, 0.0, 0.0, sample.encode_ms, 0.0, sample.fps);
    const auto state = adaptive_bitrate::get_state();
    if (sample.reduce) {
      EXPECT_EQ(state.target_bitrate_kbps, 17600);
      EXPECT_EQ(state.state, "encoder_pressure");
      EXPECT_EQ(state.reason, "encode_load");
      EXPECT_GT(adaptive_bitrate::get_doctor_state().revision, before.revision);
    } else {
      EXPECT_EQ(state.target_bitrate_kbps, state.base_bitrate_kbps);
      EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, before.revision);
    }
  }
  adaptive_bitrate::reset();
}

TEST(AdaptiveBitrateController, EncoderPressureMovementAdvancesControllerRevision) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  EXPECT_LT(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  EXPECT_GT(after_pressure.revision, before_pressure.revision);
}

TEST(AdaptiveBitrateController, EncoderPressureInsideAdjustmentIntervalInvalidatesAStaleDoctorSnapshot) {
  enable_controller();

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  // No interval sleep is intentional: a pressure sample must invalidate a
  // stale Doctor snapshot even when the adaptive target cannot move yet.
  adaptive_bitrate::note_doctor_video_policy_evidence(true);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  EXPECT_GT(after_pressure.revision, before_pressure.revision);
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    before_pressure.revision,
    25000,
    before_pressure.max_bitrate_kbps
  ));
}

TEST(AdaptiveBitrateController, VideoPolicyTransitionsInvalidateOnceWithAdaptiveDisabled) {
  enable_controller();
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  const auto clean_observation = adaptive_bitrate::get_doctor_state();

  // Repeated clean samples retain a still-valid action envelope.
  adaptive_bitrate::note_doctor_video_policy_evidence(false);
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    clean_observation.revision
  );

  // The first host video warning suppresses restore_quality even though the
  // adaptive controller itself is disabled and would ignore this sample.
  adaptive_bitrate::note_doctor_video_policy_evidence(true);
  const auto warning_observation = adaptive_bitrate::get_doctor_state();
  EXPECT_FALSE(warning_observation.enabled);
  EXPECT_GT(warning_observation.revision, clean_observation.revision);
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    clean_observation.revision,
    25000,
    clean_observation.max_bitrate_kbps
  ));

  // Persistent warning samples do not starve an unrelated stable network
  // action by continuously rotating controller authority.
  adaptive_bitrate::note_doctor_video_policy_evidence(true);
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    warning_observation.revision
  );
}

TEST(AdaptiveBitrateController, ProvisionalPacingTransitionInvalidatesWithoutBlockingRestore) {
  enable_controller();
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::note_doctor_video_policy_evidence(false, false);
  const auto clean_observation = adaptive_bitrate::get_doctor_state();

  adaptive_bitrate::note_doctor_video_policy_evidence(false, true);
  const auto provisional_observation = adaptive_bitrate::get_doctor_state();
  EXPECT_GT(provisional_observation.revision, clean_observation.revision);
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    clean_observation.revision,
    25000,
    clean_observation.max_bitrate_kbps
  ));

  adaptive_bitrate::note_doctor_video_policy_evidence(false, true);
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    provisional_observation.revision
  );
}

TEST(AdaptiveBitrateController, EncoderPressureAtFloorAdvancesControllerRevision) {
  enable_controller(2000);

  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before_pressure = adaptive_bitrate::get_doctor_state();
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_stream_health(0.96, 0.0, 0.0, 1.0, 12.0, 20.0);

  const auto after_pressure = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after_pressure.live_bitrate_kbps, before_pressure.live_bitrate_kbps);
  EXPECT_GT(after_pressure.revision, before_pressure.revision);
}

TEST(AdaptiveBitrateController, ConfiguredCeilingNeverCutsTheClientsStartingBitrate) {
  // A PyroWave client asking for 180 Mbps at 1080p60 reaches the encoder at
  // about 161 Mbps, far above the 50 Mbps ceiling configured here.
  enable_controller(161000);

  auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(161000, state.base_bitrate_kbps);
  EXPECT_EQ(161000, state.target_bitrate_kbps);
  // The ceiling the controller holds, and the host reports, is the request.
  EXPECT_EQ(161000, state.max_bitrate_kbps);
  // Adaptive is on, so the encoder polls this request every 30 frames. It has
  // to hold the rate the stream opened at rather than cut it to the ceiling.
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(161000, request->target_bitrate_kbps);

  // Pressure lowers the bitrate from the client's request, not from the ceiling.
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ("network_pressure", state.state);
  EXPECT_LT(state.target_bitrate_kbps, 161000);
  EXPECT_GT(state.target_bitrate_kbps, 50000);

  // The raise belongs to the stream. It never reaches the configured value, so
  // the next stream start reloads 50 Mbps for a smaller stream after this one.
  EXPECT_EQ(50000, config::video.adaptive_bitrate.max_bitrate_kbps);
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_base_bitrate(20000);
  EXPECT_EQ(50000, adaptive_bitrate::get_state().max_bitrate_kbps);
}

TEST(AdaptiveBitrateController, ARequestBelowTheConfiguredCeilingStartsAsBefore) {
  // A guard, not proof of the fix: this passed before it too. It pins that a
  // stream under the configured ceiling starts exactly as it did, and fails if
  // the fix sets the ceiling to the request instead of raising it to the request.
  enable_controller(20000);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(20000, state.base_bitrate_kbps);
  EXPECT_EQ(20000, state.target_bitrate_kbps);
  EXPECT_EQ(50000, state.max_bitrate_kbps);
}

TEST(AdaptiveBitrateController, ARequestBelowTheFloorStartsAtTheFloor) {
  // A guard, not proof of the fix: this held before it too. The floor is the one
  // place the bitrate can sit above what a client asked for, and the Live Tuning
  // docs say so. A client asking 10 Mbps opens the encoder near 7988 kbps; with
  // the floor at 20000 the controller starts at 20000, and the rewritten live
  // write has to keep that floor as well.
  struct restore_adaptive_config_t {
    decltype(config::video.adaptive_bitrate) saved = config::video.adaptive_bitrate;

    ~restore_adaptive_config_t() {
      config::video.adaptive_bitrate = saved;
    }
  } restore;
  config::video.adaptive_bitrate.enabled = true;
  config::video.adaptive_bitrate.min_bitrate_kbps = 20000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 7988);
  adaptive_bitrate::set_base_bitrate(7988);

  auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(20000, state.base_bitrate_kbps);
  EXPECT_EQ(50000, state.max_bitrate_kbps);
  // With Live Tuning on, the encoder's first bitrate check raises it to the floor.
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(20000, request->target_bitrate_kbps);

  adaptive_bitrate::set_live_bitrate(10000);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ(20000, state.base_bitrate_kbps);
  EXPECT_EQ(20000, state.target_bitrate_kbps);
}

TEST(AdaptiveBitrateController, ExplicitLiveRetryCanRaiseSessionCeilingAndTarget) {
  enable_controller(7580);
  const auto configured_ceiling = config::video.adaptive_bitrate.max_bitrate_kbps;
  adaptive_bitrate::set_max_bitrate(7580);
  adaptive_bitrate::set_max_bitrate(20000);
  adaptive_bitrate::set_live_bitrate(9475);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.max_bitrate_kbps, 20000);
  EXPECT_EQ(state.base_bitrate_kbps, 9475);
  EXPECT_EQ(state.target_bitrate_kbps, 9475);
  EXPECT_EQ(state.reason, "paired_client_action");
  EXPECT_EQ(config::video.adaptive_bitrate.max_bitrate_kbps, configured_ceiling);
}

TEST(AdaptiveBitrateController, ExplicitLiveWriteAboveTheConfiguredCeilingIsKept) {
  enable_controller(20000);
  adaptive_bitrate::set_live_bitrate(180000);

  auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(180000, state.base_bitrate_kbps);
  EXPECT_EQ(180000, state.target_bitrate_kbps);
  EXPECT_EQ(180000, state.max_bitrate_kbps);
  EXPECT_EQ(50000, config::video.adaptive_bitrate.max_bitrate_kbps);

  // Adaptive stays on in this controller, so pressure works down from the
  // written 180 Mbps and must not snap to the configured 50 Mbps ceiling.
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(8.0, 8.0);
  state = adaptive_bitrate::get_state();
  EXPECT_LT(state.target_bitrate_kbps, 180000);
  EXPECT_GT(state.target_bitrate_kbps, 50000);
}

TEST(AdaptiveBitrateController, ExplicitLiveWriteStaysUnderTheHostMaxBitrate) {
  const auto host_cap = config::video.max_bitrate;
  enable_controller(20000);
  config::video.max_bitrate = 150000;
  adaptive_bitrate::set_live_bitrate(180000);
  const auto capped = adaptive_bitrate::get_state();
  config::video.max_bitrate = 0;
  adaptive_bitrate::set_live_bitrate(180000);
  const auto uncapped = adaptive_bitrate::get_state();
  config::video.max_bitrate = host_cap;

  EXPECT_EQ(150000, capped.base_bitrate_kbps);
  EXPECT_EQ(150000, capped.target_bitrate_kbps);
  EXPECT_EQ(180000, uncapped.base_bitrate_kbps);
  EXPECT_EQ(180000, uncapped.target_bitrate_kbps);
}

TEST(AdaptiveBitrateController, HidesTargetsWhenEncoderCannotApplyRuntimeUpdates) {
  enable_controller(26000);
  adaptive_bitrate::set_runtime_update_supported(false);

  adaptive_bitrate::update_network_stats(21.8, 12.0);

  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_FALSE(state.active);
  EXPECT_FALSE(state.runtime_update_supported);
  EXPECT_EQ(0, state.target_bitrate_kbps);
  EXPECT_EQ(0, adaptive_bitrate::get_target_bitrate_kbps());
  EXPECT_EQ("unavailable", state.state);
  EXPECT_EQ("encoder_runtime_update_unsupported", state.reason);
}

TEST(AdaptiveBitrateController, NormalizesMaxBelowMinWithoutCuttingTheBase) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 0;

  adaptive_bitrate::load_config();
  EXPECT_EQ(2000, config::video.adaptive_bitrate.max_bitrate_kbps);
  adaptive_bitrate::reset();
  // The controller's own ceiling is normalized up to the floor as well.
  EXPECT_EQ(2000, adaptive_bitrate::get_state().max_bitrate_kbps);
  adaptive_bitrate::set_base_bitrate(30000);

  const auto state = adaptive_bitrate::get_state();
  // The stream then raises that ceiling to its request rather than being cut to it.
  EXPECT_EQ(30000, state.max_bitrate_kbps);
  EXPECT_EQ(30000, state.base_bitrate_kbps);
  EXPECT_EQ(0, state.target_bitrate_kbps);
}

TEST(AdaptiveBitrateController, DoctorRollbackNeverOverwritesANewerExplicitWriter) {
  enable_controller(20000);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    20000
  );
  ASSERT_TRUE(doctor_revision.has_value());
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 15000);

  adaptive_bitrate::set_base_bitrate(10000);
  EXPECT_FALSE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ));

  const auto after = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after.base_bitrate_kbps, 10000);
  EXPECT_EQ(after.live_bitrate_kbps, 10000);
  EXPECT_GT(after.revision, *doctor_revision);
}

TEST(AdaptiveBitrateController, DoctorTransactionRestoresExactOwnedState) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  ASSERT_FALSE(before.enabled);

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    25000
  );
  ASSERT_TRUE(doctor_revision.has_value());
  // Doctor owns one live target without changing the configured/runtime
  // adaptive-controller mode.
  ASSERT_FALSE(adaptive_bitrate::get_doctor_state().enabled);
  ASSERT_TRUE(adaptive_bitrate::is_active());
  ASSERT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 15000);
  adaptive_bitrate::acknowledge_live_bitrate_applied(*doctor_revision, 15000);

  const auto restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  );
  ASSERT_TRUE(restore_revision.has_value());
  const auto rollback_request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(rollback_request.has_value());
  EXPECT_EQ(rollback_request->revision, *restore_revision);
  EXPECT_EQ(rollback_request->target_bitrate_kbps, before.live_bitrate_kbps);
  EXPECT_TRUE(adaptive_bitrate::get_state().active);
  EXPECT_EQ(adaptive_bitrate::get_state().state, "rollback_pending");

  adaptive_bitrate::acknowledge_live_bitrate_applied(
    rollback_request->revision,
    rollback_request->target_bitrate_kbps
  );
  const auto after = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after.enabled, before.enabled);
  EXPECT_EQ(after.base_bitrate_kbps, before.base_bitrate_kbps);
  EXPECT_EQ(after.live_bitrate_kbps, before.live_bitrate_kbps);
  EXPECT_EQ(after.max_bitrate_kbps, before.max_bitrate_kbps);
  EXPECT_FALSE(adaptive_bitrate::is_active());
  EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(
    *restore_revision,
    before.live_bitrate_kbps
  ).has_value());
}

TEST(AdaptiveBitrateController, RecreatedEncoderSessionAcknowledgesTheExactPendingRevision) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *doctor_revision,
    15000
  ).has_value());

  // The FFmpeg path recreates only its encoder session. A successful open at
  // the requested config reports that exact target and current controller
  // revision through this existing session-ready handshake.
  adaptive_bitrate::set_runtime_update_supported(true, {}, 15000);
  EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(
    *doctor_revision,
    15000
  ).has_value());
}

TEST(AdaptiveBitrateController, EncoderSessionLossInvalidatesItsApplicationProof) {
  enable_controller(20000);
  const auto state = adaptive_bitrate::get_doctor_state();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  ASSERT_TRUE(adaptive_bitrate::live_bitrate_applied_at(
    state.revision,
    20000
  ).has_value());

  adaptive_bitrate::set_runtime_update_supported(false, "encoder_session_ended");
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    state.revision,
    20000
  ).has_value());
}

TEST(AdaptiveBitrateController, RollbackDuringEncoderRecreationWaitsForTheReplacementSession) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(
    *doctor_revision,
    15000
  ));

  const auto restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  );
  ASSERT_TRUE(restore_revision.has_value());
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *restore_revision,
    20000
  ).has_value());
  EXPECT_FALSE(adaptive_bitrate::wait_for_live_bitrate_applied(
    *restore_revision,
    20000,
    1ms
  ));

  const auto rollback_request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(rollback_request.has_value());
  EXPECT_EQ(rollback_request->revision, *restore_revision);
  EXPECT_EQ(rollback_request->target_bitrate_kbps, 20000);

  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  EXPECT_TRUE(adaptive_bitrate::wait_for_live_bitrate_applied(
    *restore_revision,
    20000,
    1ms
  ));
}

TEST(AdaptiveBitrateController, SupersededRequestCannotRetireTheCurrentEncoderSession) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());
  const auto restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  );
  ASSERT_TRUE(restore_revision.has_value());

  EXPECT_FALSE(adaptive_bitrate::begin_live_bitrate_session_recreation(
    *doctor_revision,
    15000
  ));
  EXPECT_TRUE(adaptive_bitrate::live_bitrate_applied_at(
    *restore_revision,
    20000
  ).has_value());
}

TEST(AdaptiveBitrateController, RetryBeforeReplacementCannotSynthesizeEncoderProof) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto first_doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    15000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(first_doctor_revision.has_value());
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(
    *first_doctor_revision,
    15000
  ));

  const auto first_restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *first_doctor_revision,
    before
  );
  ASSERT_TRUE(first_restore_revision.has_value());
  ASSERT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *first_restore_revision,
    20000
  ).has_value());

  const auto retry_before = adaptive_bitrate::get_doctor_state();
  const auto retry_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    retry_before.revision,
    16000,
    retry_before.max_bitrate_kbps
  );
  ASSERT_TRUE(retry_revision.has_value());
  const auto retry_restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *retry_revision,
    retry_before
  );
  ASSERT_TRUE(retry_restore_revision.has_value());

  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *retry_restore_revision,
    20000
  ).has_value());
  EXPECT_FALSE(adaptive_bitrate::wait_for_live_bitrate_applied(
    *retry_restore_revision,
    20000,
    1ms
  ));
}

TEST(AdaptiveBitrateController, DoctorTargetCannotDriftFromTelemetry) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    20000
  );
  ASSERT_TRUE(doctor_revision.has_value());

  adaptive_bitrate::update_network_stats(12.0, 120.0);
  adaptive_bitrate::update_stream_health(0.70, 0.10, 0.20, 8.0, 18.0, 50.0);

  const auto during = adaptive_bitrate::get_doctor_state();
  EXPECT_FALSE(during.enabled);
  EXPECT_EQ(during.base_bitrate_kbps, 16000);
  EXPECT_EQ(during.live_bitrate_kbps, 16000);
  EXPECT_EQ(during.revision, *doctor_revision);

  ASSERT_TRUE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ));
}

TEST(AdaptiveBitrateController, RollbackTargetCannotDriftBeforeEncoderAcknowledgement) {
  enable_controller(20000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  const auto before = adaptive_bitrate::get_doctor_state();
  ASSERT_TRUE(before.enabled);

  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    before.max_bitrate_kbps
  );
  ASSERT_TRUE(doctor_revision.has_value());
  adaptive_bitrate::acknowledge_live_bitrate_applied(*doctor_revision, 16000);

  const auto restore_revision = adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  );
  ASSERT_TRUE(restore_revision.has_value());
  ASSERT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 20000);

  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(12.0, 120.0);
  adaptive_bitrate::update_stream_health(0.70, 0.10, 0.20, 8.0, 18.0, 50.0);

  const auto pending = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(pending.live_bitrate_kbps, 20000);
  EXPECT_EQ(pending.revision, *restore_revision);
  EXPECT_FALSE(adaptive_bitrate::live_bitrate_applied_at(
    *restore_revision,
    20000
  ).has_value());

  adaptive_bitrate::acknowledge_live_bitrate_applied(*restore_revision, 20000);
  std::this_thread::sleep_for(1100ms);
  adaptive_bitrate::update_network_stats(12.0, 120.0);

  const auto resumed = adaptive_bitrate::get_doctor_state();
  EXPECT_LT(resumed.live_bitrate_kbps, 20000);
  EXPECT_GT(resumed.revision, *restore_revision);
}

TEST(AdaptiveBitrateController, NewerExplicitIncreaseReplacesDoctorTargetExactly) {
  enable_controller(20000);
  adaptive_bitrate::set_runtime_enabled(false);
  const auto before = adaptive_bitrate::get_doctor_state();
  const auto doctor_revision = adaptive_bitrate::set_doctor_bitrate_if_revision(
    before.revision,
    16000,
    20000
  );
  ASSERT_TRUE(doctor_revision.has_value());

  adaptive_bitrate::set_live_bitrate(30000);

  EXPECT_FALSE(adaptive_bitrate::restore_doctor_state_if_revision(
    *doctor_revision,
    before
  ));
  const auto after = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after.base_bitrate_kbps, 30000);
  EXPECT_EQ(after.live_bitrate_kbps, 30000);
  EXPECT_EQ(after.max_bitrate_kbps, before.max_bitrate_kbps);
  EXPECT_TRUE(after.explicit_live_override_active);
  EXPECT_TRUE(adaptive_bitrate::is_active());
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 30000);
  EXPECT_GT(after.revision, *doctor_revision);
}

TEST(AdaptiveBitrateController, DisableRejectsFetchedButUnappliedRequest) {
  enable_controller();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  adaptive_bitrate::set_enabled(false);
  bool called = false;
  EXPECT_FALSE(adaptive_bitrate::apply_live_bitrate_request(*request, [&] { called = true; return true; }));
  EXPECT_FALSE(called);
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 20000);
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
}

TEST(AdaptiveBitrateController, DisableHoldsCompletedEncoderApplication) {
  enable_controller();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*request, [] { return true; }));
  adaptive_bitrate::set_enabled(false);
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 14000);
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
}

TEST(AdaptiveBitrateController, DisableDuringRecreationReconcilesToLastProvenRate) {
  enable_controller();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request);
  ASSERT_TRUE(adaptive_bitrate::begin_live_bitrate_session_recreation(request->revision, 14000));
  adaptive_bitrate::set_enabled(false);
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 0);
  ASSERT_TRUE(adaptive_bitrate::get_live_bitrate_request());
  EXPECT_EQ(adaptive_bitrate::get_live_bitrate_request()->target_bitrate_kbps, 20000);
  // If creation already began with the old target, its actual rate remains
  // visible until the held target is reconciled, even with tuning disabled.
  adaptive_bitrate::set_runtime_update_supported(true, {}, 14000);
  const auto reconcile = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(reconcile);
  ASSERT_TRUE(adaptive_bitrate::apply_live_bitrate_request(*reconcile, [] { return true; }));
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 20000);
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
}

TEST(AdaptiveBitrateController, DisableSerializesWithInFlightEncoderApplication) {
  enable_controller();
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  adaptive_bitrate::set_live_bitrate(14000);
  const auto request = *adaptive_bitrate::get_live_bitrate_request();
  std::promise<void> entered, finish;
  auto resume = finish.get_future();
  auto update = std::async(std::launch::async, [&] {
    return adaptive_bitrate::apply_live_bitrate_request(request, [&] {
      entered.set_value(); resume.wait(); return true;
    });
  });
  entered.get_future().wait();
  auto disable = std::async(std::launch::async, [] { adaptive_bitrate::set_enabled(false); });
  EXPECT_EQ(disable.wait_for(20ms), std::future_status::timeout);
  finish.set_value();
  EXPECT_TRUE(update.get());
  disable.get();
  EXPECT_EQ(adaptive_bitrate::get_state().applied_bitrate_kbps, 14000);
  EXPECT_FALSE(adaptive_bitrate::get_live_bitrate_request());
}

TEST(AdaptiveBitrateController, PyroWaveFloorStopsLiveTuningAndNeverLiftsARequest) {
  // A request below the codec's floor becomes the floor itself: Live Tuning cannot cut it at all, and
  // the stream is never raised above what the client asked for.
  enable_controller(20000);
  adaptive_bitrate::set_session_floor(76785, "pyrowave_advice");
  auto state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.min_bitrate_kbps, 20000);
  EXPECT_EQ(state.base_bitrate_kbps, 20000);
  EXPECT_EQ(state.target_bitrate_kbps, 20000);
  EXPECT_EQ(state.floor_source, "pyrowave_advice");

  // Above the floor, heavy loss cuts down to it and no further.
  enable_controller(100000);
  adaptive_bitrate::set_session_floor(90000, "pyrowave_advice");
  ASSERT_EQ(adaptive_bitrate::get_state().min_bitrate_kbps, 90000);
  adaptive_bitrate::update_network_stats(0.0, 8.0);
  for (int i = 0; i < 2; ++i) {
    std::this_thread::sleep_for(1100ms);
    adaptive_bitrate::update_network_stats(20.0, 8.0);
  }
  state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.state, "network_pressure");
  EXPECT_EQ(state.target_bitrate_kbps, 90000);

  // The next stream starts from adaptive_bitrate_min again.
  enable_controller(20000);
  state = adaptive_bitrate::get_state();
  EXPECT_EQ(state.min_bitrate_kbps, 2000);
  EXPECT_EQ(state.floor_source, "adaptive_bitrate_min");
}
