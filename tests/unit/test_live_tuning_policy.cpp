/**
 * @file tests/unit/test_live_tuning_policy.cpp
 * @brief Replays of Live Tuning's loss and queue policy against its replay bench.
 *
 * The golden replays come from tools/tests/live_tuning_policy_reference.py. The scenario tests below say what
 * each replay is for, and the statistical ones run seeds the reference never saw.
 */

#include "src/live_tuning_policy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr int BASE = 268988;
  constexpr int FLOOR = 2000;

  using feed_t = std::function<std::pair<double, double>(int second, int target)>;
  using rtt_t = std::function<double(std::int64_t at_ms, int target)>;

  /// One simulated second: RTT readings at 50, 150, ... 950 ms and one media report at 500 ms. The feed sees
  /// the target the second starts with. tools/tests/live_tuning_policy_reference.py runs the same harness.
  std::vector<int> run(
    int seconds,
    const feed_t &feed,
    const rtt_t &rtt = {},
    const std::function<bool(int)> &quiet = {},
    live_tuning_policy::policy_t *out = nullptr
  ) {
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    std::vector<int> targets;
    targets.reserve(seconds);
    for (int i = 0; i < seconds; ++i) {
      const std::int64_t t0 = std::int64_t {i} * 1000;
      const auto [expected, lost] = feed(i, policy.target_kbps());
      for (int k = 0; k < 10; ++k) {
        const std::int64_t at = t0 + 100 * k + 50;
        if (k == 5 && !(quiet && quiet(i))) {
          policy.report(t0 + 500, expected, lost);
        }
        policy.rtt(at, rtt ? rtt(at, policy.target_kbps()) : 3.0);
      }
      targets.push_back(policy.target_kbps());
    }
    if (out) {
      *out = policy;
    }
    return targets;
  }

  feed_t link(int kbps, std::pair<double, double> over = {120, 6}, std::pair<double, double> under = {120, 0}, int start = 25, int recover_at = -1) {
    return [=](int i, int target) {
      const bool limited = i >= start && (recover_at < 0 || i < recover_at);
      return limited && target > kbps ? over : under;
    };
  }

  feed_t clean() {
    return [](int, int) {
      return std::pair<double, double> {120, 0};
    };
  }

  int enet_smooth(double s, double raw) {
    // enet/protocol.c: roundTripTime moves 1/8 of the way to the sample, in whole milliseconds.
    const int si = static_cast<int>(std::nearbyint(s));
    const int r = std::max(static_cast<int>(std::nearbyint(raw)), 1);
    if (r >= si) {
      return si + (r - si + 7) / 8;
    }
    return si - (si - r + 7) / 8;
  }

  /// Tail drop behind a 120 ms buffer: loss grows with the overload, and so does the queue.
  struct queue_t {
    int kbps;
    int start = 25;
    double q = 0.0;
    double s = 3.0;
    std::int64_t last = -1000;
    double raw = 3.0;

    feed_t feed() {
      return [this](int i, int target) {
        const double overload = i >= start ? std::max(0.0, 1.0 - static_cast<double>(kbps) / target) : 0.0;
        return std::pair<double, double> {120, std::floor(120 * std::min(1.0, 6.0 * overload))};
      };
    }

    rtt_t rtt() {
      return [this](std::int64_t at, int target) {
        const double lk = at / 1000 >= start ? kbps : 1e9;
        q = std::min(120.0, std::max(0.0, q + 100.0 * (target - lk) / lk));
        raw = 3.0 + q;
        if (at - last >= 500) {
          s = enet_smooth(s, raw);
          last = at;
        }
        return s;
      };
    }
  };

  const nlohmann::json &fixture() {
    static const nlohmann::json value = [] {
      std::ifstream input(std::filesystem::path(POLARIS_SOURCE_DIR) / "tests/fixtures/live-tuning-policy-replays.json");
      std::stringstream text;
      text << input.rdbuf();
      return nlohmann::json::parse(text.str());
    }();
    return value;
  }

  std::vector<double> deck_scan() {
    return fixture().at("deck_scan_rtt_ms").get<std::vector<double>>();
  }

  rtt_t deck_scan_rtt() {
    auto period = std::make_shared<std::vector<double>>(deck_scan());
    return [period](std::int64_t at, int) {
      return (*period)[static_cast<std::size_t>((at / 100) % static_cast<std::int64_t>(period->size()))];
    };
  }

  int median(std::vector<int> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
  }

  int seconds_over(const std::vector<int> &targets, int from, int kbps) {
    return static_cast<int>(std::count_if(targets.begin() + from, targets.end(), [&](int t) {
      return t > kbps;
    }));
  }

  /// Each frame is lost with probability p, independently: Wi-Fi loss that does not depend on the rate.
  feed_t background(double p, std::uint64_t seed, int start = 20) {
    auto rng = std::make_shared<std::mt19937_64>(seed);
    return [=](int i, int) {
      if (i < start) {
        return std::pair<double, double> {120, 0};
      }
      std::binomial_distribution<int> frames {120, p};
      return std::pair<double, double> {120, static_cast<double>(frames(*rng))};
    };
  }

  // RP6-S2-COMPLETED/DENSE-TRACE.jsonl, SHA256 ef95864bce0c62e29c7fdf5d9d8d8b7ab3774c546e47ce718aea3a9f96a08824.
  // Times are milliseconds from cap start. Report receipts are snapshot time minus host_receipt_age_ms,
  // rounded to milliseconds; repeated interval sequences are omitted. These are the retained 1 Hz RTT
  // observations, not a reconstruction of every ENet poll or encoder acknowledgement.
  live_tuning_policy::policy_t rp6_s2_prefix(int through_ms) {
    constexpr int origin = 700000;
    live_tuning_policy::policy_t policy {25987, FLOOR};
    // The recorded 692 s clean condition: zero loss and a 3 ms RTT floor. Counts within the clean
    // interval vary; 120 is the observed nominal report size, with the actual congested counts below.
    for (int i = -692; i < 0; ++i) {
      policy.report(origin + i * 1000, 120, 0);
      policy.rtt(origin + i * 1000, 3.0);
    }
    struct report_t {
      int at;
      int expected;
      int lost;
    };
    const std::vector<report_t> reports {
      {-25, 121, 0},
      {1136, 163, 132},
      {4209, 542, 541},
      {8566, 753, 752},
      {10699, 375, 374},
      {14204, 620, 618},
      {15963, 303, 297},
      {17088, 201, 197},
      {18103, 158, 144},
      {19418, 232, 224},
      {20414, 144, 29},
      {21411, 120, 0},
      {22423, 118, 0},
      {23438, 129, 91},
    };
    const std::vector<std::pair<int, double>> rtts {
      {630, 58.0},
      {1636, 68.0},
      {2639, 72.0},
      {3645, 78.0},
      {4647, 82.0},
      {5654, 83.0},
      {6656, 88.0},
      {7663, 91.0},
      {8666, 89.0},
      {9674, 90.0},
      {10678, 91.0},
      {11687, 94.0},
      {12690, 93.0},
      {13697, 96.0},
      {14700, 94.0},
      {15708, 95.0},
      {16711, 95.0},
      {17717, 96.0},
      {18721, 96.0},
      {19728, 96.0},
      {20732, 73.0},
      {21739, 56.0},
      {22743, 38.0},
      {23749, 57.0},
    };
    std::size_t report = 0;
    std::size_t rtt = 0;
    while (report < reports.size() || rtt < rtts.size()) {
      if (report < reports.size() && (rtt == rtts.size() || reports[report].at < rtts[rtt].first)) {
        const auto &row = reports[report++];
        if (row.at > through_ms) {
          break;
        }
        policy.report(origin + row.at, row.expected, row.lost);
      } else {
        const auto &[at, ms] = rtts[rtt++];
        if (at > through_ms) {
          break;
        }
        policy.rtt(origin + at, ms);
      }
    }
    return policy;
  }

  const std::vector<std::pair<int, int>> &rp6_s2_clean_reports() {
    static const std::vector<std::pair<int, int>> clean_reports {
      {181127, 120}, {182137, 120}, {183163, 121}, {184139, 120},
      {185155, 121}, {186155, 120}, {187155, 121}, {188167, 118},
      {189172, 121}, {190168, 120}, {191183, 121}, {192201, 121},
      {193190, 120}, {194191, 121}, {195210, 121}, {196204, 120},
      {197209, 119}, {198225, 121}, {199224, 121}, {200221, 120},
      {201239, 121}, {202238, 120}, {203244, 121}, {204247, 121},
    };
    return clean_reports;
  }

  live_tuning_policy::policy_t rp6_s2_failed_hold_window(int through_ms) {
    // Warm clean prefix with its observed 3 ms RTT floor. Adopt artificially seeds the captured
    // 9946 hold; this does not assert an outside writer existed or replay the preceding cap cycle.
    auto policy = rp6_s2_prefix(-1);
    policy.adopt(700000 + 55858, 9946);
    // Literal report receipts/counts and RTT samples from CLAUDE-CAP-SETTLING-SOURCE-DISPOSITION/
    // FACTS.json (SHA256 f32fcaed8e48b21c5f8e7704d97da2626f6fc72cc19a8d40a306a75ec70961d8).
    struct report_t {
      int at;
      int expected;
      int lost;
    };
    const std::vector<report_t> reports {
      {56943, 190, 180}, {58175, 202, 192}, {59294, 181, 171}, {60303, 142, 39},
      {61309, 121, 0}, {62313, 118, 0}, {63316, 120, 0}, {64344, 126, 81},
    };
    const std::vector<std::pair<int, double>> rtts {
      {56978, 84}, {57987, 86}, {58992, 92}, {59999, 84},
      {61003, 65}, {62010, 50}, {63014, 38}, {64022, 38},
    };
    std::size_t report = 0;
    std::size_t rtt = 0;
    while (report < reports.size() || rtt < rtts.size()) {
      if (report < reports.size() && (rtt == rtts.size() || reports[report].at < rtts[rtt].first)) {
        const auto &row = reports[report++];
        if (row.at > through_ms) {
          break;
        }
        policy.report(700000 + row.at, row.expected, row.lost);
      } else {
        const auto &[at, ms] = rtts[rtt++];
        if (at > through_ms) {
          break;
        }
        policy.rtt(700000 + at, ms);
      }
    }
    return policy;
  }

  void promoted_queue_hold(live_tuning_policy::policy_t &policy) {
    constexpr std::int64_t origin = 700000;
    // Reuse the WideRetainedQueueCeiling prefix through its 14546 promotion from 13952.
    // These are fixed public-API component inputs, not a native replay or queue model.
    for (int second = 84; second <= 103; ++second) {
      policy.report(origin + second * 1000, 90, 0);
      policy.rtt(origin + second * 1000 + 1, 2);
    }
    for (int second = 104; second <= 112; ++second) {
      policy.rtt(origin + second * 1000 + 1, 19);
    }
    policy.rtt(origin + 113001, 2);
    policy.rtt(origin + 114001, 2);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    ASSERT_EQ(policy.target_kbps(), 29739);
    ASSERT_EQ(policy.ceiling_kbps(), 31523);
    ASSERT_TRUE(policy.ceiling_is_queue());
    for (int second = 115; second <= 123; ++second) {
      policy.rtt(origin + second * 1000 + 1, 85);
    }
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::rtt_cut);
    ASSERT_EQ(policy.target_kbps(), 25278);
    for (int second = 124; second <= 127; ++second) {
      policy.report(origin + second * 1000, 100, 100);
    }
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::descend);
    ASSERT_EQ(policy.target_kbps(), 4000);
    ASSERT_EQ(policy.ceiling_kbps(), 15166);
    ASSERT_FALSE(policy.ceiling_is_queue());
    policy.report(origin + 128000, 90, 0);
    policy.report(origin + 129000, 90, 0);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
    ASSERT_EQ(policy.target_kbps(), 15166);
    policy.rtt(origin + 129001, 2);
    policy.rtt(origin + 129100, 19);
    policy.rtt(origin + 129200, 27);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::bisect);
    ASSERT_EQ(policy.target_kbps(), 7788);
    for (int second = 130; second <= 149; ++second) {
      policy.report(origin + second * 1000, 90, 0);
      policy.rtt(origin + second * 1000 + 1, 2);
    }
    ASSERT_EQ(policy.target_kbps(), 14546);
    policy.rtt(origin + 149100, 19);
    policy.rtt(origin + 149200, 27);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    ASSERT_EQ(policy.target_kbps(), 13952);
    ASSERT_EQ(policy.ceiling_kbps(), 14546);
    ASSERT_TRUE(policy.ceiling_is_queue());
    for (int second = 150; second <= 158; ++second) {
      policy.report(origin + second * 1000, 90, 0);
      policy.rtt(origin + second * 1000 + 1, 2);
    }
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
    ASSERT_EQ(policy.target_kbps(), 14546);
    for (int second = 159; second <= 163; ++second) {
      policy.report(origin + second * 1000, 90, 0);
      policy.rtt(origin + second * 1000 + 1, 2);
    }
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    ASSERT_EQ(policy.target_kbps(), 14546);
    ASSERT_EQ(policy.ceiling_kbps(), 15418);
    ASSERT_TRUE(policy.ceiling_is_queue());
  }

  void queue_hold_report(live_tuning_policy::policy_t &policy, int &second, int expected = 90, int lost = 0) {
    policy.report(700000 + ++second * 1000, expected, lost);
    policy.rtt(700000 + second * 1000 + 1, 2);
  }

  void fail_promoted_queue_hold(live_tuning_policy::policy_t &policy, int &second) {
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    ASSERT_EQ(policy.target_kbps(), 14546);
    ASSERT_EQ(policy.ceiling_kbps(), 15418);
    ASSERT_TRUE(policy.ceiling_is_queue());
    queue_hold_report(policy, second, 100, 100);
    queue_hold_report(policy, second, 100, 100);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::descend);
    ASSERT_EQ(policy.target_kbps(), 8727);  // floor(0.6 * 14546).
    ASSERT_EQ(policy.ceiling_kbps(), 14546);
    ASSERT_FALSE(policy.ceiling_is_queue());
    queue_hold_report(policy, second);  // Mixed after the cut.
    queue_hold_report(policy, second);  // Clean descent rechecks the failed hold itself.
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
    ASSERT_EQ(policy.target_kbps(), 14546);
    for (int report = 0; report < 4; ++report) {
      queue_hold_report(policy, second);  // Mixed, then only three judged verification reports.
      ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
      ASSERT_EQ(policy.target_kbps(), 14546);
    }
    queue_hold_report(policy, second);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    ASSERT_EQ(policy.target_kbps(), 13952);  // The lower validated queue hold remains the fallback.
    ASSERT_EQ(policy.ceiling_kbps(), 14546);  // Measured failure, not the synthetic 15418 rung.
    ASSERT_TRUE(policy.ceiling_is_queue());
  }

  void retry_failed_queue_hold(live_tuning_policy::policy_t &policy, int &second, int wait) {
    queue_hold_report(policy, second);  // Mixed after falling back from 14546 to 13952.
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    ASSERT_EQ(policy.target_kbps(), 13952);
    for (int judged = 1; judged < wait; ++judged) {
      queue_hold_report(policy, second);
      ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold) << judged;
      ASSERT_EQ(policy.target_kbps(), 13952) << judged;
      ASSERT_EQ(policy.ceiling_kbps(), 14546);
      ASSERT_TRUE(policy.ceiling_is_queue());
    }
    queue_hold_report(policy, second);  // The existing backoff expires on this judged report.
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
    ASSERT_EQ(policy.target_kbps(), 14546);
    ASSERT_EQ(policy.ceiling_kbps(), 14546);
    ASSERT_TRUE(policy.ceiling_is_queue());
    ASSERT_EQ(policy.last_change().why, "retry");
  }

}  // namespace

TEST(LiveTuningPolicy, FailedQueueHoldSameRateVerifyRestoresLowerHoldAndWaitsTwentyReports) {
  live_tuning_policy::policy_t policy {34988, FLOOR};
  ASSERT_NO_FATAL_FAILURE(promoted_queue_hold(policy));
  int second = 163;
  ASSERT_NO_FATAL_FAILURE(fail_promoted_queue_hold(policy, second));
  // The short same-rate verify must neither re-hold 14546 nor retry its synthetic 15418 ceiling.
  ASSERT_NO_FATAL_FAILURE(retry_failed_queue_hold(policy, second, 20));
}

TEST(LiveTuningPolicy, FailedQueueHoldCanBeReprovedAndItsRepeatLossWaitsFortyReports) {
  live_tuning_policy::policy_t policy {34988, FLOOR};
  ASSERT_NO_FATAL_FAILURE(promoted_queue_hold(policy));
  int second = 163;
  ASSERT_NO_FATAL_FAILURE(fail_promoted_queue_hold(policy, second));
  ASSERT_NO_FATAL_FAILURE(retry_failed_queue_hold(policy, second, 20));
  for (int report = 0; report < 4; ++report) {
    queue_hold_report(policy, second);  // Mixed, then only three judged retry verification reports.
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
    ASSERT_EQ(policy.target_kbps(), 14546);
  }
  queue_hold_report(policy, second);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  ASSERT_EQ(policy.target_kbps(), 14546);  // A later hold retry can prove the staircase hold again.
  ASSERT_EQ(policy.ceiling_kbps(), 15418);
  ASSERT_TRUE(policy.ceiling_is_queue());
  // Fail before that re-proved hold survives a full retry wait: retain the prior loss count.
  ASSERT_NO_FATAL_FAILURE(fail_promoted_queue_hold(policy, second));
  ASSERT_NO_FATAL_FAILURE(retry_failed_queue_hold(policy, second, 40));
}

TEST(LiveTuningPolicy, QueueCeilingSurvivesLowerHoldLossAndBoundsItsFirstPromotion) {
  constexpr std::int64_t origin = 700000;
  constexpr int hold_kbps = 14088;
  constexpr int queue_kbps = 14775;
  live_tuning_policy::policy_t policy {34988, FLOOR};
  // Component setup, not an exact native replay: adopt seeds the two adjacent observed rates,
  // without asserting that an outside writer existed. The fixed clean 90-frame prefix supplies
  // the observed nominal cadence and 2 ms floor; no input depends on the policy's target.
  for (int second = 84; second <= 103; ++second) {
    policy.report(origin + second * 1000, 90, 0);
    policy.rtt(origin + second * 1000 + 1, 2);
  }
  policy.adopt(origin + 104000, queue_kbps);
  policy.adopt(origin + 104000, hold_kbps);
  for (int second = 105; second <= 125; ++second) {
    policy.report(origin + second * 1000, 90, 0);
    policy.rtt(origin + second * 1000 + 1, 2);
  }
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  ASSERT_EQ(policy.target_kbps(), queue_kbps);
  // Observed rising RTT values reject that public retry and establish real queue provenance.
  policy.rtt(origin + 125100, 19);
  policy.rtt(origin + 125200, 27);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  ASSERT_EQ(policy.target_kbps(), hold_kbps);
  ASSERT_EQ(policy.ceiling_kbps(), queue_kbps);
  ASSERT_TRUE(policy.ceiling_is_queue());
  const auto queue_hold = policy;

  // Actual epoch 4 intervals 105..115 from DECK-CAP976-228-FIRST/STATS.jsonl,
  // SHA256 ea99d3faf30496630ce785b8e66db31473970ac232a3ed0177be7a1708e31cc8.
  // Receipt times are sampler request-end minus host_receipt_age_ms, rounded to ms.
  // RTT values/times are the retained 1 Hz observations, not all subsecond ENet polls.
  struct row_t {
    int receipt_ms;
    int expected;
    int lost;
    int rtt_at_ms;
    int rtt_ms;
  };
  const std::vector<row_t> rows {
    {126207, 84, 33, 126360, 48},
    {127202, 102, 19, 127365, 52},
    {128199, 90, 0, 128373, 54},
    {129196, 94, 73, 129377, 62},
    {130210, 104, 29, 130385, 47},
    {131211, 90, 0, 131391, 36},
    {132230, 90, 0, 132400, 28},
    {133204, 90, 0, 133404, 21},
    {134218, 90, 0, 134413, 16},
    {135235, 90, 0, 135420, 12},
    {136230, 90, 0, 136430, 9},
  };
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto &row = rows[i];
    SCOPED_TRACE(row.receipt_ms);
    policy.report(origin + row.receipt_ms, row.expected, row.lost);
    policy.rtt(origin + row.rtt_at_ms, row.rtt_ms);
    if (i == 3) {
      ASSERT_EQ(policy.state(), live_tuning_policy::state_e::descend);
      ASSERT_EQ(policy.target_kbps(), 8452);
      // The descent's temporary ceiling is distinct from the saved higher queue bound.
      ASSERT_EQ(policy.ceiling_kbps(), hold_kbps);
      ASSERT_FALSE(policy.ceiling_is_queue());
    }
    if (i >= 5 && i < 10) {
      // Interval 111 is mixed; intervals 112..114 are only three clean judged reports.
      ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
      ASSERT_EQ(policy.target_kbps(), hold_kbps);
      ASSERT_EQ(policy.ceiling_kbps(), hold_kbps);
    }
  }
  // The fourth clean judged interval verifies 14088, not the un-retested 14775 queue ceiling.
  // Exact 976 instead retires both bounds and climbs to 21132 here.
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  EXPECT_EQ(policy.target_kbps(), hold_kbps);
  EXPECT_EQ(policy.ceiling_kbps(), queue_kbps);
  EXPECT_TRUE(policy.ceiling_is_queue());

  // Independent sequence in this same component case: a queue rate that passes its four-report
  // dwell becomes a real hold before any further completion-size probe.
  // This fixed clean continuation is a unit input, not another captured native observation.
  auto promotion = queue_hold;
  for (int second = 126; second <= 146; ++second) {
    promotion.report(origin + second * 1000, 90, 0);
    promotion.rtt(origin + second * 1000 + 1, 2);
  }
  ASSERT_EQ(promotion.state(), live_tuning_policy::state_e::verify_hi);
  ASSERT_EQ(promotion.target_kbps(), queue_kbps);
  ASSERT_EQ(promotion.ceiling_kbps(), queue_kbps);
  ASSERT_TRUE(promotion.ceiling_is_queue());
  for (int second = 147; second <= 150; ++second) {
    promotion.report(origin + second * 1000, 90, 0);  // Mixed, then three judged reports.
    ASSERT_EQ(promotion.state(), live_tuning_policy::state_e::verify_hi);
    ASSERT_EQ(promotion.target_kbps(), queue_kbps);
  }
  promotion.report(origin + 151000, 90, 0);
  EXPECT_EQ(promotion.state(), live_tuning_policy::state_e::hold);
  EXPECT_EQ(promotion.target_kbps(), queue_kbps);
  EXPECT_EQ(promotion.ceiling_kbps(), 15661);
  EXPECT_TRUE(promotion.ceiling_is_queue());
}

TEST(LiveTuningPolicy, WideRetainedQueueCeilingHoldsTheVerifiedRetryBeforeItsNextStep) {
  constexpr std::int64_t origin = 700000;
  live_tuning_policy::policy_t policy {34988, FLOOR};
  // Fixed component inputs, not a native replay or a target-dependent queue plant.
  // The drained first cut establishes the actual D3 queue ceiling, 31523.
  for (int second = 84; second <= 103; ++second) {
    policy.report(origin + second * 1000, 90, 0);
    policy.rtt(origin + second * 1000 + 1, 2);
  }
  for (int second = 104; second <= 112; ++second) {
    policy.rtt(origin + second * 1000 + 1, 19);
  }
  policy.rtt(origin + 113001, 2);
  policy.rtt(origin + 114001, 2);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  ASSERT_EQ(policy.target_kbps(), 29739);
  ASSERT_EQ(policy.ceiling_kbps(), 31523);
  ASSERT_TRUE(policy.ceiling_is_queue());

  // A second standing queue cuts to 25278 while the higher queue ceiling survives.
  for (int second = 115; second <= 123; ++second) {
    policy.rtt(origin + second * 1000 + 1, 85);
  }
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::rtt_cut);
  ASSERT_EQ(policy.target_kbps(), 25278);
  ASSERT_EQ(policy.ceiling_kbps(), 31523);
  for (int second = 124; second <= 127; ++second) {
    policy.report(origin + second * 1000, 100, 100);
  }
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::descend);
  ASSERT_EQ(policy.target_kbps(), 4000);
  ASSERT_EQ(policy.ceiling_kbps(), 15166);
  ASSERT_FALSE(policy.ceiling_is_queue());
  policy.report(origin + 128000, 90, 0);  // Mixed interval after the deep cut.
  policy.report(origin + 129000, 90, 0);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  ASSERT_EQ(policy.target_kbps(), 15166);

  // Reject the temporary loss ceiling on RTT, then discover the lower bracket.
  policy.rtt(origin + 129001, 2);
  policy.rtt(origin + 129100, 19);
  policy.rtt(origin + 129200, 27);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::bisect);
  ASSERT_EQ(policy.target_kbps(), 7788);
  for (int second = 130; second <= 149; ++second) {
    policy.report(origin + second * 1000, 90, 0);
    policy.rtt(origin + second * 1000 + 1, 2);
    if (second == 134 || second == 139 || second == 144 || second == 149) {
      ASSERT_EQ(policy.state(), live_tuning_policy::state_e::bisect);
    }
  }
  ASSERT_EQ(policy.target_kbps(), 14546);
  policy.rtt(origin + 149100, 19);
  policy.rtt(origin + 149200, 27);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  ASSERT_EQ(policy.target_kbps(), 13952);
  ASSERT_EQ(policy.ceiling_kbps(), 14546);
  ASSERT_TRUE(policy.ceiling_is_queue());

  int last_hold = 13952;
  auto changes = policy.changes();
  const auto observe = [&] {
    if (policy.changes() != changes) {
      const auto &change = policy.last_change();
      if (change.to_kbps > change.from_kbps) {
        EXPECT_LE(std::int64_t {change.to_kbps} * 100, std::int64_t {last_hold} * 106)
          << change.why << " at " << change.at_ms;
      }
      changes = policy.changes();
    }
    if (policy.state() == live_tuning_policy::state_e::hold) {
      last_hold = policy.target_kbps();
    }
  };
  const auto clean_report = [&](int second) {
    policy.report(origin + second * 1000, 90, 0);
    observe();
    policy.rtt(origin + second * 1000 + 1, 2);
    observe();
  };
  for (int second = 150; second <= 157; ++second) {
    clean_report(second);  // Mixed, then only seven judged hold reports.
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    ASSERT_EQ(policy.target_kbps(), 13952);
  }
  clean_report(158);  // The existing eighth judged report permits the 14546 retry.
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  ASSERT_EQ(policy.target_kbps(), 14546);
  for (int second = 159; second <= 162; ++second) {
    clean_report(second);  // Mixed, then only three judged verification reports.
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
    ASSERT_EQ(policy.target_kbps(), 14546);
  }
  clean_report(163);
  // Actual D3 sequence47 restores 31523 here and bisects to 21413. A verified retry
  // must instead become a real hold before the next completion-size queue probe.
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  ASSERT_EQ(policy.target_kbps(), 14546);
  ASSERT_EQ(policy.ceiling_kbps(), 15418);
  ASSERT_TRUE(policy.ceiling_is_queue());
  ASSERT_EQ(last_hold, 14546);
  for (int second = 164; second <= 182; ++second) {
    clean_report(second);  // A state-only hold does not create a mixed rate interval.
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    ASSERT_EQ(policy.target_kbps(), 14546);
  }
  clean_report(183);  // Existing 20-report retry cadence, with the new real hold anchor.
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  EXPECT_EQ(policy.target_kbps(), 15418);
  EXPECT_TRUE(policy.ceiling_is_queue());
}

TEST(LiveTuningPolicy, LossOnlyVerificationStillResumesOrdinaryDiscovery) {
  constexpr std::int64_t origin = 700000;
  live_tuning_policy::policy_t policy {34988, FLOOR};
  for (int second = 84; second <= 103; ++second) {
    policy.report(origin + second * 1000, 90, 0);
    policy.rtt(origin + second * 1000 + 1, 2);
  }
  // Public outside-writer setup, with no queue provenance or retained queue ceiling.
  policy.adopt(origin + 104000, 25278);
  for (int second = 105; second <= 107; ++second) {
    policy.report(origin + second * 1000, 100, 100);
  }
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::descend);
  ASSERT_EQ(policy.target_kbps(), 15166);
  ASSERT_FALSE(policy.ceiling_is_queue());
  policy.report(origin + 108000, 90, 0);  // Mixed interval after the cut.
  policy.report(origin + 109000, 90, 0);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  ASSERT_EQ(policy.target_kbps(), 25278);
  for (int second = 110; second <= 113; ++second) {
    policy.report(origin + second * 1000, 90, 0);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  }
  policy.report(origin + 114000, 90, 0);
  // A clean loss-only verification still resumes the existing ordinary climb.
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::climb);
  EXPECT_EQ(policy.target_kbps(), 31597);
  EXPECT_FALSE(policy.ceiling_kbps());
}

TEST(LiveTuningPolicy, QueueStaircaseHoldsEachVerifiedRateAndBacksOffClosedBracketFailures) {
  constexpr std::int64_t origin = 700000;
  live_tuning_policy::policy_t policy {34988, FLOOR};
  // Fixed component inputs, not a plant or native replay. As in the preceding case, adopt
  // seeds adjacent rates; the 19/27 ms pair then establishes queue provenance through rtt().
  for (int second = 84; second <= 103; ++second) {
    policy.report(origin + second * 1000, 90, 0);
    policy.rtt(origin + second * 1000 + 1, 2);
  }
  policy.adopt(origin + 104000, 13587);
  policy.adopt(origin + 104000, 13197);

  int last_hold = 13197;
  auto changes = policy.changes();
  const auto observe = [&] {
    if (policy.changes() != changes) {
      const auto &change = policy.last_change();
      if (change.to_kbps > change.from_kbps) {
        EXPECT_LE(std::int64_t {change.to_kbps} * 100, std::int64_t {last_hold} * 106)
          << change.why << " at " << change.at_ms;
      }
      changes = policy.changes();
    }
    if (policy.state() == live_tuning_policy::state_e::hold) {
      last_hold = policy.target_kbps();
    }
    EXPECT_NE(policy.target_kbps(), 13586);
    EXPECT_NE(policy.target_kbps(), 13988);
  };
  const auto clean_report = [&](int second) {
    policy.report(origin + second * 1000, 90, 0);
    observe();
    policy.rtt(origin + second * 1000 + 1, 2);
    observe();
  };
  const auto reject = [&](int second) {
    policy.rtt(origin + second * 1000 + 100, 19);
    observe();
    policy.rtt(origin + second * 1000 + 200, 27);
    observe();
  };
  const auto expect_hold = [&](int target, int ceiling) {
    EXPECT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    EXPECT_EQ(policy.target_kbps(), target);
    EXPECT_EQ(policy.ceiling_kbps(), ceiling);
    EXPECT_TRUE(policy.ceiling_is_queue());
    EXPECT_EQ(last_hold, target);
  };

  for (int second = 105; second <= 125; ++second) {
    clean_report(second);  // First mixed report, then the existing 20 judged hold reports.
  }
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  ASSERT_EQ(policy.target_kbps(), 13587);
  reject(125);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  expect_hold(13197, 13587);
  for (int second = 126; second <= 146; ++second) {
    clean_report(second);
  }
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  ASSERT_EQ(policy.target_kbps(), 13587);

  int second = 146;
  for (const auto &[held, ceiling] : std::vector<std::pair<int, int>> {{13587, 14402}, {14402, 15266}}) {
    SCOPED_TRACE(held);
    for (int report = 0; report < 4; ++report) {
      clean_report(++second);  // Mixed, then only three judged verification reports.
      ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
      ASSERT_EQ(policy.target_kbps(), held);
    }
    clean_report(++second);  // Fourth judged report: hold this rate, do not climb past it.
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    expect_hold(held, ceiling);
    for (int judged = 1; judged < 20; ++judged) {
      clean_report(++second);  // A state-only hold entry does not create a mixed rate interval.
      expect_hold(held, ceiling);
    }
    clean_report(++second);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
    ASSERT_EQ(policy.target_kbps(), ceiling);
    ASSERT_TRUE(policy.ceiling_is_queue());
  }

  // At the real 14402 hold, 15266 is a closed-bracket retry. Its first failure waits 20
  // judged hold reports and the next waits 40; a mixed report after each cut counts as neither.
  for (const int wait : {20, 40}) {
    SCOPED_TRACE(wait);
    reject(second);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    expect_hold(14402, 15266);
    clean_report(++second);  // Mixed after the rejection's downward rate change.
    expect_hold(14402, 15266);
    for (int judged = 1; judged < wait; ++judged) {
      clean_report(++second);
      expect_hold(14402, 15266);
    }
    clean_report(++second);
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
    ASSERT_EQ(policy.target_kbps(), 15266);
    ASSERT_EQ(policy.ceiling_kbps(), 15266);
    ASSERT_TRUE(policy.ceiling_is_queue());
  }
}

TEST(LiveTuningPolicy, ADrainedRttCutRetriesAtMostOneCompletionStepAboveItsHold) {
  constexpr std::int64_t origin = 700000;
  live_tuning_policy::policy_t policy {34988, FLOOR};
  for (int second = 84; second <= 103; ++second) {
    policy.report(origin + second * 1000, 90, 0);
    policy.rtt(origin + second * 1000 + 1, 2);
  }
  // Fixed standing RTT, then fixed drained RTT: no target-dependent queue model.
  for (int second = 104; second <= 112; ++second) {
    policy.rtt(origin + second * 1000 + 1, 19);
  }
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::rtt_cut);
  ASSERT_EQ(policy.target_kbps(), 29739);  // Existing floor(34988 * 0.85) cut.
  ASSERT_EQ(policy.last_change().why, "rtt");
  policy.rtt(origin + 113001, 2);
  policy.rtt(origin + 114001, 2);  // Close the first drained second.
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  ASSERT_EQ(policy.target_kbps(), 29739);
  ASSERT_EQ(policy.ceiling_kbps(), 31523);  // floor(29739 * 1.06), not the pre-cut 34988.
  ASSERT_TRUE(policy.ceiling_is_queue());
  for (int second = 115; second <= 122; ++second) {
    policy.report(origin + second * 1000, 90, 0);  // Mixed, then seven judged hold reports.
    policy.rtt(origin + second * 1000 + 1, 2);
    EXPECT_EQ(policy.state(), live_tuning_policy::state_e::hold);
    EXPECT_EQ(policy.target_kbps(), 29739);
  }
  policy.report(origin + 123000, 90, 0);  // The existing eighth judged report permits a retry.
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  EXPECT_EQ(policy.target_kbps(), 31523);
  EXPECT_EQ(policy.ceiling_kbps(), 31523);
  EXPECT_TRUE(policy.ceiling_is_queue());
  EXPECT_LE(std::int64_t {policy.last_change().to_kbps} * 100, std::int64_t {29739} * 106);
}

TEST(LiveTuningPolicy, Rp6S2FailedHoldWaitsForDelayedLossBeforePromotion) {
  const auto descent = rp6_s2_failed_hold_window(59294);
  ASSERT_EQ(descent.state(), live_tuning_policy::state_e::descend);
  ASSERT_EQ(descent.target_kbps(), 5967);
  const auto retry = rp6_s2_failed_hold_window(61309);
  ASSERT_EQ(retry.state(), live_tuning_policy::state_e::verify_hi);
  ASSERT_EQ(retry.target_kbps(), 9946);
  auto policy = rp6_s2_failed_hold_window(63316);
  // 53c prematurely erased the ceiling and climbed to 14919 on this first counted clean report.
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  ASSERT_EQ(policy.target_kbps(), 9946);
  ASSERT_EQ(policy.ceiling_kbps(), 9946);
  policy.rtt(700000 + 64022, 38);
  policy.report(700000 + 64344, 126, 81);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::bisect);
  EXPECT_EQ(policy.ceiling_kbps(), 9946);
  EXPECT_EQ(policy.target_kbps(), 7703);
}

TEST(LiveTuningPolicy, FailedHoldDwellCountsNeitherMixedNorGapReports) {
  auto policy = rp6_s2_failed_hold_window(62313);  // First report after the verify raise was mixed.
  policy.report(700000 + 66316, 120, 0);  // A >2.5 s gap cannot count as a clean dwell report.
  for (int i = 1; i <= 3; ++i) {
    policy.report(700000 + 66316 + i * 1000, 120, 0);
    EXPECT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
    EXPECT_EQ(policy.target_kbps(), 9946);
  }
  policy.report(700000 + 70316, 120, 0);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::climb);
  EXPECT_EQ(policy.target_kbps(), 14919);
  // Ordinary climbs keep their existing frame test, with no extra report dwell.
  policy.report(700000 + 71316, 240, 0);  // Mixed after the climb.
  policy.report(700000 + 72316, 240, 0);
  EXPECT_EQ(policy.target_kbps(), 18648);
}

TEST(LiveTuningPolicy, CleanFailedRatePromotionReleasesTheLaterBisectDwell) {
  auto policy = rp6_s2_failed_hold_window(63316);
  for (int i = 1; i <= 3; ++i) {
    policy.report(700000 + 63316 + i * 1000, 120, 0);
  }
  ASSERT_EQ(policy.target_kbps(), 14919);  // Four judged reports have now proved 9946 clean.
  policy.report(700000 + 67316, 120, 120);  // Mixed after the climb.
  policy.report(700000 + 68316, 120, 120);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::bisect);
  policy.report(700000 + 69316, 120, 0);  // Mixed after the bisect cut.
  policy.report(700000 + 70316, 120, 0);
  // One sufficient clean report can again recheck the ceiling, rather than waiting on the old failure.
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  EXPECT_EQ(policy.target_kbps(), 14919);
  EXPECT_EQ(policy.last_change().why, "recheck_ceiling");
}

TEST(LiveTuningPolicy, CleanBisectBelowAFailedHoldKeepsTheLaterDwell) {
  auto policy = rp6_s2_failed_hold_window(64344);
  ASSERT_EQ(policy.target_kbps(), 7703);
  policy.report(700000 + 65344, 120, 0);  // Mixed after the cut.
  for (int i = 1; i <= 3; ++i) {
    policy.report(700000 + 65344 + i * 1000, 120, 0);
    EXPECT_EQ(policy.target_kbps(), 7703);
  }
  policy.report(700000 + 69344, 120, 0);
  ASSERT_EQ(policy.target_kbps(), 8752);
  policy.report(700000 + 70344, 120, 0);  // Mixed after the raise.
  policy.report(700000 + 71344, 120, 0);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::bisect);
  EXPECT_EQ(policy.target_kbps(), 8752);  // Proving 7703 clean did not rehabilitate failed 9946.
}

TEST(LiveTuningPolicy, NoOpOutsideAdoptionClearsTheFailedHoldDwell) {
  auto policy = rp6_s2_failed_hold_window(63316);
  const auto changes = policy.changes();
  policy.adopt(700000 + 63317, 9946);
  ASSERT_EQ(policy.changes(), changes);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  ASSERT_FALSE(policy.ceiling_kbps());
  for (int i = 1; i <= 20; ++i) {
    policy.report(700000 + 63316 + i * 1000, 120, 0);
  }
  ASSERT_EQ(policy.target_kbps(), 14919);
  policy.report(700000 + 84316, 120, 120);
  policy.report(700000 + 85316, 120, 120);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::bisect);
  policy.report(700000 + 86316, 120, 0);
  policy.report(700000 + 87316, 120, 0);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  EXPECT_EQ(policy.target_kbps(), 14919);
}

TEST(LiveTuningPolicy, RebaseClearsFailedHoldDwellOnlyWhenItChangesTheTarget) {
  for (const bool changes_target : {false, true}) {
    SCOPED_TRACE(changes_target);
    auto policy = rp6_s2_failed_hold_window(63316);
    policy.rebase(changes_target ? 9945 : 27000, FLOOR);
    policy.report(700000 + 64344, 126, 81);
    if (changes_target) {
      ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);  // Mixed after the clamp.
      policy.report(700000 + 65344, 126, 81);
    }
    ASSERT_EQ(policy.state(), live_tuning_policy::state_e::bisect);
    ASSERT_EQ(policy.target_kbps(), 7703);
    policy.report(700000 + 66344, 120, 0);  // Mixed after the bisect cut.
    policy.report(700000 + 67344, 120, 0);
    if (changes_target) {
      EXPECT_GT(policy.target_kbps(), 7703);
    } else {
      EXPECT_EQ(policy.target_kbps(), 7703);
    }
  }
}

TEST(LiveTuningPolicy, Rp6S2DoesNotApproveARetryFromItsFirstMixedReport) {
  auto policy = rp6_s2_prefix(21411);
  ASSERT_EQ(policy.target_kbps(), 11264);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);

  // Actual next report, 1.012 s later: a half-weight 118/0 was enough to approve 11264 and jump
  // to 16896, above the still-active 9615.184 kbps cap. It cannot yet establish the retried rate.
  policy.report(700000 + 22423, 118, 0);
  EXPECT_EQ(policy.target_kbps(), 11264);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);

  // The following actual report exposes the failed retry. Its lower bound must still be 4000,
  // rather than the 11264 that was prematurely called clean.
  policy.rtt(700000 + 22743, 38.0);
  policy.report(700000 + 23438, 129, 91);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::bisect);
  EXPECT_EQ(policy.ceiling_kbps(), 11264);
  EXPECT_EQ(policy.target_kbps(), 6712);  // sqrt(4000 * 11264), below the actual cap
}

TEST(LiveTuningPolicy, Rp6S2CongestionDoesNotBecomeBurstinessForCleanRecovery) {
  // Isolate the baseline seam after the actual first 132/163-loss report, before a second lossy
  // report can authorize a probe. Adopt seeds the observed settled minimum; it does not assert
  // that an outside writer existed in S2. This is a component test, not a complete S2 replay.
  auto policy = rp6_s2_prefix(1136);
  ASSERT_EQ(policy.target_kbps(), 25987);
  policy.adopt(700000 + 5000, 4000);

  // Actual first 24 distinct clean report receipts/counts after cap removal. This component test's
  // omitted middle creates the first-report gap; in the trace 181127 follows 180139 by about 1 s.
  // The hold then uses its existing 20-report retry. No reports are synthesized to clear it.
  const auto &clean_reports = rp6_s2_clean_reports();
  for (std::size_t i = 0; i <= 20; ++i) {
    policy.report(700000 + clean_reports[i].first, clean_reports[i].second, 0);
  }
  ASSERT_EQ(policy.target_kbps(), 25987);
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
  for (std::size_t i = 21; i < clean_reports.size(); ++i) {
    policy.report(700000 + clean_reports[i].first, clean_reports[i].second, 0);
  }
  // After discarding the mixed report, 242 clean frames score -3.63, crossing the unchanged log(19) boundary
  // with the clean baseline's dispersion floor of 2. The old contaminated dispersion of 10 does not.
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::normal);
  EXPECT_EQ(policy.target_kbps(), 25987);
}

TEST(LiveTuningPolicy, ANewLossCandidateDefersAnOtherwiseDueHoldRetry) {
  live_tuning_policy::policy_t policy {25987, FLOOR};
  policy.adopt(0, 4000);
  policy.report(0, 120, 0);  // First report after the outside cut is mixed.
  for (int i = 1; i < 20; ++i) {
    policy.report(i * 1000, 120, 0);
  }
  ASSERT_EQ(policy.target_kbps(), 4000);
  // The actual S2 onset counts, deliberately placed on the twentieth judged hold report.
  policy.report(20000, 163, 132);
  EXPECT_EQ(policy.target_kbps(), 4000);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  // A second lossy report still authorizes the existing probe; the retry is not a new loss gate.
  policy.report(21000, 163, 132);
  EXPECT_EQ(policy.target_kbps(), 2400);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::descend);
}

TEST(LiveTuningPolicy, AnIsolatedBurstDefersADueRetryByOnlyTheExistingDutyWindow) {
  live_tuning_policy::policy_t policy {25987, FLOOR};
  policy.adopt(0, 4000);
  policy.report(0, 120, 0);  // First report after the outside cut is mixed.
  for (int i = 1; i < 20; ++i) {
    policy.report(i * 1000, 120, 0);
  }
  policy.report(20000, 163, 132);
  ASSERT_EQ(policy.target_kbps(), 4000);
  policy.report(21000, 120, 0);
  EXPECT_EQ(policy.target_kbps(), 4000);
  // The same three-report loss window has now judged one burst and two clean reports.
  policy.report(22000, 120, 0);
  EXPECT_EQ(policy.target_kbps(), 25987);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::verify_hi);
}

TEST(LiveTuningPolicy, Rp6RttCutRestoresTheBaselineAcrossSkippedGapReports) {
  auto policy = rp6_s2_prefix(8666);
  ASSERT_EQ(policy.target_kbps(), 22088);  // Actual first RTT cut, after both skipped gap reports.
  policy.adopt(700000 + 10000, 24000);  // An upward adoption cannot repair a missed cut rollback.
  for (const auto &[at, expected] : rp6_s2_clean_reports()) {
    policy.report(700000 + at, expected, 0);
  }
  // At d=2 the final clean reports finish the climb to base. At d=10 it remains a probe.
  // The upward adoption commits rather than restores: only the actual RTT cut can repair it.
  // This is restore-path coverage, not a full S2 replay.
  EXPECT_EQ(policy.target_kbps(), 25987);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::normal);
}

TEST(LiveTuningPolicyReplays, MatchTheReferenceModelSecondForSecond) {
  const auto pauses = [] {
    std::set<int> quiet;
    for (int i = 150; i < 170; ++i) {
      quiet.insert(i);
    }
    for (int i = 300; i < 335; ++i) {
      quiet.insert(i);
    }
    return quiet;
  }();
  const std::vector<std::pair<int, int>> hevc {{120, 0}, {122, 9}, {120, 0}, {120, 0}, {120, 0}, {120, 0}, {121, 9}, {120, 0}};
  const auto phases = [](std::vector<std::tuple<int, double, double>> spans) -> feed_t {
    std::vector<std::pair<double, double>> seq;
    for (const auto &[n, expected, lost] : spans) {
      seq.insert(seq.end(), n, {expected, lost});
    }
    return [seq](int i, int) {
      return i < static_cast<int>(seq.size()) ? seq[i] : std::pair<double, double> {120, 0};
    };
  };
  queue_t q100 {100000};
  queue_t q30 {30000};
  struct scenario_t {
    int seconds;
    feed_t feed;
    rtt_t rtt;
    std::function<bool(int)> quiet;
  };
  const std::map<std::string, scenario_t> scenarios {
    {"link_100", {600, link(100000), {}, {}}},
    {"link_50", {600, link(50000), {}, {}}},
    {"link_100_with_background", {600, link(100000, {120, 7}, {120, 1}), {}, {}}},
    {"link_200", {600, link(200000), {}, {}}},
    {"rp6_hevc", {1200, [&](int i, int) {
      return std::pair<double, double> {hevc[i % 8].first, hevc[i % 8].second};
    }, {}, {}}},
    {"burst_10s_at_5_percent", {90, phases({{20, 120, 0}, {10, 120, 6}, {60, 120, 0}}), {}, {}}},
    {"burst_3s_at_20_percent", {90, phases({{20, 120, 0}, {3, 120, 24}, {60, 120, 0}}), {}, {}}},
    {"link_100_recovers_at_200", {400, link(100000, {120, 6}, {120, 0}, 25, 200), {}, {}}},
    {"link_100_with_report_pauses", {600, link(100000), {}, [&](int i) {
      return pauses.count(i) > 0;
    }}},
    {"deck_scan", {1230, clean(), deck_scan_rtt(), {}}},
    {"queue_link_100", {600, q100.feed(), q100.rtt(), {}}},
    {"queue_link_30", {600, q30.feed(), q30.rtt(), {}}},
  };
  const auto &golden = fixture().at("targets_kbps");
  ASSERT_EQ(golden.size(), scenarios.size());
  for (const auto &[name, scenario] : scenarios) {
    SCOPED_TRACE(name);
    const auto expected = golden.at(name).get<std::vector<int>>();
    const auto targets = run(scenario.seconds, scenario.feed, scenario.rtt, scenario.quiet);
    ASSERT_EQ(targets.size(), expected.size());
    const auto mismatch = std::mismatch(targets.begin(), targets.end(), expected.begin());
    EXPECT_EQ(mismatch.first, targets.end()) << "first differs at second " << (mismatch.first - targets.begin())
                                             << ": " << *mismatch.first << " kbps, the reference has " << *mismatch.second;
  }
}

TEST(LiveTuningPolicy, AShrunkLinkSettlesJustUnderItAndTriesItRarely) {
  for (const int kbps : {50000, 100000, 200000}) {
    SCOPED_TRACE(kbps);
    const auto targets = run(600, link(kbps));
    const std::vector<int> settled(targets.begin() + 120, targets.end());
    EXPECT_GE(median(settled), kbps * 0.9);
    EXPECT_LE(median(settled), kbps);
    // Record the mixed-report discard's retry cost without changing the existing bound.
    RecordProperty("seconds_over_last480_link_" + std::to_string(kbps), seconds_over(targets, 120, kbps));
    RecordProperty("seconds_over_per240_link_" + std::to_string(kbps), std::to_string(seconds_over(targets, 120, kbps) * 240.0 / 480));
    EXPECT_LE(seconds_over(targets, 120, kbps) * 240 / 480, 25);
  }
}

TEST(LiveTuningPolicy, ARecoveredLinkIsBackAtTheRequestWithin30SecondsAtTheMedian) {
  for (const int kbps : {50000, 100000}) {
    SCOPED_TRACE(kbps);
    std::vector<int> waits;
    for (int recover_at = 40; recover_at <= 600; recover_at += 20) {
      const auto targets = run(recover_at + 300, link(kbps, {120, 6}, {120, 0}, 25, recover_at));
      const auto back = std::find_if(targets.begin() + recover_at, targets.end(), [](int t) {
        return t >= BASE;
      });
      waits.push_back(back == targets.end() ? 1000000 : static_cast<int>(back - targets.begin()) - recover_at);
    }
    EXPECT_LE(median(waits), 30);
  }
}

TEST(LiveTuningPolicy, LossThatDoesNotDependOnTheRateRarelyHalvesTheStreamAndNeverReachesTheFloor) {
  // The bench's bar: under 1% of seconds below half on rate-independent loss, never at the floor under 3%.
  for (const double p : {0.01, 0.02, 0.029}) {
    SCOPED_TRACE(p);
    double below_half = 0.0;
    int at_floor = 0;
    constexpr int seeds = 20;
    constexpr int seconds = 1200;
    for (int seed = 0; seed < seeds; ++seed) {
      const auto targets = run(seconds, background(p, 1000 + seed));
      below_half += std::count_if(targets.begin(), targets.end(), [](int t) {
        return t < BASE / 2;
      }) / static_cast<double>(seconds);
      at_floor += std::count(targets.begin(), targets.end(), FLOOR) > 0;
    }
    EXPECT_LT(below_half / seeds, 0.01);
    EXPECT_EQ(at_floor, 0);
  }
}

TEST(LiveTuningPolicy, TheRp6HevcBurstsNeverReachTheFloor) {
  const std::vector<std::pair<double, double>> hevc {{120, 0}, {122, 9}, {120, 0}, {120, 0}, {120, 0}, {120, 0}, {121, 9}, {120, 0}};
  const auto targets = run(1200, [&](int i, int) {
    return hevc[i % 8];
  });
  EXPECT_EQ(std::count(targets.begin(), targets.end(), FLOOR), 0);
  EXPECT_LT(std::count_if(targets.begin(), targets.end(), [](int t) {
    return t < BASE / 2;
  }), 12);
  EXPECT_EQ(targets.back(), BASE);
}

TEST(LiveTuningPolicy, WarmedRp6BurstsCanMoveTogetherWithoutStartingAProbe) {
  const std::vector<std::pair<double, double>> hevc {{120, 0}, {122, 9}, {120, 0}, {120, 0}, {120, 0}, {120, 0}, {121, 9}, {120, 0}};
  // The same 18 lost frames and 963 expected in every eight reports; move the second burst from
  // slot 6 to slot 2 so two bursts are within three. The loss still does not depend on the rate.
  const std::vector<std::pair<double, double>> jittered {{120, 0}, {122, 9}, {121, 9}, {120, 0}, {120, 0}, {120, 0}, {120, 0}, {120, 0}};
  live_tuning_policy::policy_t policy {BASE, FLOOR};
  const auto targets = run(480, [&](int i, int) {
    // Thirty complete cycles (240 s) teach the ordinary level and dispersion before the spacing changes.
    return i < 240 ? hevc[i % 8] : jittered[i % 8];
  }, {}, {}, &policy);
  const auto cut = std::find_if(targets.begin(), targets.end(), [](int t) {
    return t != BASE;
  });
  EXPECT_EQ(cut, targets.end()) << "first rate change at second " << (cut - targets.begin());
  EXPECT_EQ(policy.changes(), 0u);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::normal);
  EXPECT_EQ(policy.background_loss(), 0.0);  // No false probe followed by rate-independent back-off.
}

TEST(LiveTuningPolicy, ASteamDeckWifiScanNeverCutsTheRate) {
  // Captured 2026-10-25: a background scan every 306.9 s lifts ENet's smoothed RTT to 83 ms for 5 s.
  const auto targets = run(1230, clean(), deck_scan_rtt());
  EXPECT_TRUE(std::all_of(targets.begin(), targets.end(), [](int t) {
    return t == BASE;
  }));
}

TEST(LiveTuningPolicy, AStandingQueueIsCutUntilItDrainsAndStaysDrained) {
  for (const int kbps : {30000, 100000}) {
    SCOPED_TRACE(kbps);
    queue_t queue {kbps};
    const auto feed = queue.feed();
    const auto rtt = queue.rtt();
    std::vector<double> delays;
    const auto targets = run(600, feed, [&](std::int64_t at, int target) {
      const double smoothed = rtt(at, target);
      if (at % 1000 == 50) {
        delays.push_back(queue.raw);
      }
      return smoothed;
    });
    const std::vector<int> settled(targets.begin() + 120, targets.end());
    EXPECT_GE(median(settled), kbps * 0.85);
    EXPECT_LE(median(settled), kbps);
    EXPECT_LE(std::count_if(delays.begin() + 120, delays.end(), [](double d) {
      return d >= 60.0;
    }), 10);
  }
}

TEST(LiveTuningPolicy, AReportPauseFreezesTheSearchAndAddsNoTimeOverTheLink) {
  const auto steady = run(600, link(100000));
  std::set<int> quiet;
  for (const auto &[from, to] : std::vector<std::pair<int, int>> {{150, 170}, {300, 335}, {420, 423}}) {
    for (int i = from; i < to; ++i) {
      quiet.insert(i);
    }
  }
  const auto paused = run(600, link(100000), {}, [&](int i) {
    return quiet.count(i) > 0;
  });
  EXPECT_LE(seconds_over(paused, 120, 100000), seconds_over(steady, 120, 100000) + 2);
  for (const int i : quiet) {
    if (i > 0 && paused[i] > paused[i - 1]) {
      EXPECT_LE(paused[i], 100000) << "climbed over the link during the pause at second " << i;
    }
  }
}

TEST(LiveTuningPolicy, OneLossyReportNeverStartsAProbe) {
  const auto targets = run(120, [](int i, int) {
    return std::pair<double, double> {120, i == 40 ? 60.0 : 0.0};
  });
  EXPECT_TRUE(std::all_of(targets.begin(), targets.end(), [](int t) {
    return t == BASE;
  }));
}

TEST(LiveTuningPolicy, AShortBurstAtEveryRateIsBackAtTheRequestSoonAfterItEnds) {
  for (const auto &[seconds, lost] : std::vector<std::pair<int, double>> {{10, 6}, {3, 24}}) {
    SCOPED_TRACE(lost);
    const auto targets = run(90, [=](int i, int) {
      return std::pair<double, double> {120, i >= 20 && i < 20 + seconds ? lost : 0.0};
    });
    const int clean_at = 20 + seconds;
    const auto back = std::find_if(targets.begin() + clean_at, targets.end(), [](int t) {
      return t >= BASE;
    });
    ASSERT_NE(back, targets.end());
    EXPECT_LE(back - targets.begin() - clean_at, 30);
  }
}

TEST(LiveTuningPolicy, AnOutsideCutHoldsThereAndLaterRetriesTheRateItCameFrom) {
  live_tuning_policy::policy_t policy {BASE, FLOOR};
  policy.adopt(0, 100000);
  EXPECT_EQ(policy.target_kbps(), 100000);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::hold);
  EXPECT_EQ(policy.ceiling_kbps(), BASE);
  // Clean reports: the hold retries the rate it came from, then it is clean too.
  for (int i = 0; i < 60 && policy.target_kbps() < BASE; ++i) {
    policy.report(1000 * (i + 1), 120, 0);
  }
  EXPECT_EQ(policy.target_kbps(), BASE);
}

TEST(LiveTuningPolicy, ANewRequestClampsTheTargetAndIsClimbedTo) {
  live_tuning_policy::policy_t policy {BASE, FLOOR};
  policy.rebase(50000, FLOOR);
  EXPECT_EQ(policy.target_kbps(), 50000);
  policy.rebase(80000, FLOOR);
  for (int i = 0; i < 30 && policy.target_kbps() < 80000; ++i) {
    policy.report(1000 * (i + 1), 120, 0);
  }
  EXPECT_EQ(policy.target_kbps(), 80000);
}

namespace {

  // 8 of 120 frames lost at every rate: loss that does not depend on the rate.
  constexpr double RATE_INDEPENDENT_LOST = 8.0;

  /// A report a second, from at_ms, until the policy learns rate-independent background loss. Nothing may ask
  /// for parity before that result. Returns the time of the next report.
  std::int64_t until_rate_independent(live_tuning_policy::policy_t &policy, std::int64_t at_ms = 1000) {
    for (int i = 0; i < 60 && policy.background_loss() == 0.0; ++i, at_ms += 1000) {
      EXPECT_FALSE(policy.fec_request()) << "parity asked for before a rate-independent result, report " << i;
      policy.report(at_ms, 120, RATE_INDEPENDENT_LOST);
    }
    EXPECT_GT(policy.background_loss(), 0.0);
    return at_ms;
  }

  /// A policy at 10% FEC whose actuator has just published the raise to 15%. Returns the time of the next report.
  std::int64_t raised(live_tuning_policy::policy_t &policy) {
    policy.configure_fec(10);
    auto at = until_rate_independent(policy);
    EXPECT_EQ(policy.fec_request(), 15);
    // The actuator takes two reports to act; the first still covers the rate before the result.
    for (int i = 0; i < 2; ++i, at += 1000) {
      policy.report(at, 120, RATE_INDEPENDENT_LOST);
    }
    policy.fec_applied(at, 15);
    EXPECT_EQ(policy.fec_percentage(), 15);
    EXPECT_FALSE(policy.fec_request());
    return at;
  }

}  // namespace

TEST(LiveTuningPolicyFec, OnlyARateIndependentResultAsksForParity) {
  live_tuning_policy::policy_t steady {BASE, FLOOR};
  steady.configure_fec(10);
  // Clean reports, RTT readings, a 30 s report pause and loss that depends on the rate ask for nothing.
  const auto limited = link(100000, {120, 6}, {120, 0}, 120);
  for (int i = 0; i < 300; ++i) {
    const std::int64_t t0 = std::int64_t {i} * 1000;
    const auto [expected, lost] = limited(i, steady.target_kbps());
    if (i < 60 || i >= 90) {
      steady.report(t0 + 500, expected, lost);
    }
    for (int k = 0; k < 10; ++k) {
      steady.rtt(t0 + 100 * k + 50, 3.0);
    }
    ASSERT_FALSE(steady.fec_request()) << "second " << i;
  }
  EXPECT_GT(steady.changes(), 0u);  // it did search for the link
  EXPECT_EQ(steady.background_loss(), 0.0);
  EXPECT_EQ(steady.fec_percentage(), 10);

  live_tuning_policy::policy_t independent {BASE, FLOOR};
  independent.configure_fec(10);
  until_rate_independent(independent);
  EXPECT_EQ(independent.fec_request(), 15);  // 10 + max(5, 10 / 2)
  EXPECT_EQ(independent.fec_percentage(), 10);
}

TEST(LiveTuningPolicyFec, ARequestIsNotAConfirmationAndUnaskedPercentagesAreIgnored) {
  live_tuning_policy::policy_t policy {BASE, FLOOR};
  policy.configure_fec(10);
  policy.fec_applied(0, 15);  // not asked for yet
  EXPECT_EQ(policy.fec_percentage(), 10);
  const auto at = until_rate_independent(policy);
  ASSERT_EQ(policy.fec_request(), 15);
  EXPECT_EQ(policy.fec_percentage(), 10);
  for (const int unasked : {-1, 9, 14, 16, 22, 51, 300}) {
    policy.fec_applied(at, unasked);
    EXPECT_EQ(policy.fec_percentage(), 10) << unasked;
    EXPECT_EQ(policy.fec_request(), 15) << unasked;
  }
  policy.fec_applied(at, 15);
  EXPECT_EQ(policy.fec_percentage(), 15);
  EXPECT_FALSE(policy.fec_request());
  // The configured percentage is always taken back, asked for or not.
  policy.fec_applied(at, 10);
  EXPECT_EQ(policy.fec_percentage(), 10);
  EXPECT_FALSE(policy.fec_request());
}

TEST(LiveTuningPolicyFec, ARefusalHoldsThePercentageAndSuppressesTheNextEpisode) {
  // hard refusal, no refusal, or a soft evidence/ACK wait expiry without backoff
  for (const int refuse : {1, 0, 2}) {
    SCOPED_TRACE(refuse);
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    policy.configure_fec(10);
    auto at = until_rate_independent(policy);
    ASSERT_EQ(policy.fec_request(), 15);
    if (refuse) {
      policy.fec_refused(at, refuse == 1);
      EXPECT_FALSE(policy.fec_request());
      EXPECT_EQ(policy.fec_percentage(), 10);
    }
    // Heavier loss at every rate: a new probe, then a second rate-independent result.
    const double first = policy.background_loss();
    for (int i = 0; i < 60 && policy.background_loss() == first; ++i, at += 1000) {
      policy.report(at, 120, 18);
    }
    ASSERT_GT(policy.background_loss(), first);
    if (refuse == 1) {
      EXPECT_FALSE(policy.fec_request());  // still inside the back-off the refusal started
    } else {
      EXPECT_EQ(policy.fec_request(), 15);  // the probe dropped the unpublished raise; the new result asks again
    }
    EXPECT_EQ(policy.fec_percentage(), 10);
  }
}

TEST(LiveTuningPolicyFec, HelpedParityHoldsAndUnhelpedParityReverts) {
  {
    SCOPED_TRACE("helped");
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    auto at = raised(policy);
    policy.report(at, 120, 0);  // still covers 10%: not judged
    policy.report(at += 1000, 120, 0);
    policy.report(at += 1000, 120, 0);
    for (int i = 0; i < 30; ++i) {
      policy.report(at += 1000, 120, 0);
      ASSERT_FALSE(policy.fec_request()) << i;
    }
    EXPECT_EQ(policy.fec_percentage(), 15);
  }
  {
    SCOPED_TRACE("not helped");
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    auto at = raised(policy);
    policy.report(at, 120, RATE_INDEPENDENT_LOST);  // still covers 10%: not judged
    policy.report(at += 1000, 120, RATE_INDEPENDENT_LOST);  // one judged report cannot decide
    EXPECT_FALSE(policy.fec_request());
    policy.report(at += 1000, 120, RATE_INDEPENDENT_LOST);  // no help
    EXPECT_EQ(policy.fec_request(), 10);
    EXPECT_EQ(policy.fec_percentage(), 15);  // asked for, not yet published
    policy.fec_applied(at, 10);
    EXPECT_EQ(policy.fec_percentage(), 10);
    EXPECT_FALSE(policy.fec_request());
  }
}

TEST(LiveTuningPolicyFec, MissingReportsAndRttHoldTheTrial) {
  live_tuning_policy::policy_t policy {BASE, FLOOR};
  auto at = raised(policy);
  // Thirty seconds of RTT readings and no media report: nothing is judged and parity does not grow.
  for (std::int64_t t = at; t < at + 30000; t += 100) {
    policy.rtt(t, 3.0);
    ASSERT_FALSE(policy.fec_request()) << t;
  }
  EXPECT_EQ(policy.fec_percentage(), 15);
  at += 30000;
  policy.report(at, 120, RATE_INDEPENDENT_LOST);  // after a gap: not judged
  policy.report(at += 1000, 120, RATE_INDEPENDENT_LOST);
  EXPECT_FALSE(policy.fec_request());
  policy.report(at += 1000, 120, RATE_INDEPENDENT_LOST);
  EXPECT_EQ(policy.fec_request(), 10);
}

TEST(LiveTuningPolicyFec, OutsideWritersAndRateDependentLossAskForTheConfiguredPercentage) {
  {
    SCOPED_TRACE("unpublished raise");
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    policy.configure_fec(10);
    const auto at = until_rate_independent(policy);
    ASSERT_EQ(policy.fec_request(), 15);
    policy.adopt(at, 100000);
    EXPECT_FALSE(policy.fec_request());
    EXPECT_EQ(policy.fec_percentage(), 10);
  }
  for (const bool writer : {true, false}) {
    SCOPED_TRACE(writer ? "outside writer" : "new request");
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    const auto at = raised(policy);
    if (writer) {
      policy.adopt(at, 100000);
    } else {
      policy.rebase(BASE / 2, FLOOR);
    }
    EXPECT_EQ(policy.fec_request(), 10);
    EXPECT_EQ(policy.fec_percentage(), 15);  // only the actuator moves it
    policy.fec_applied(at, 10);
    EXPECT_EQ(policy.fec_percentage(), 10);
    EXPECT_FALSE(policy.fec_request());
  }
  {
    SCOPED_TRACE("rate-dependent loss after parity helped");
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    auto at = raised(policy);
    for (int i = 0; i < 3; ++i, at += 1000) {
      policy.report(at, 120, 0);
    }
    ASSERT_FALSE(policy.fec_request());
    for (int i = 0; i < 6 && !policy.fec_request(); ++i, at += 1000) {
      policy.report(at, 120, 60);
    }
    EXPECT_NE(policy.state(), live_tuning_policy::state_e::normal);
    EXPECT_EQ(policy.fec_request(), 10);
    EXPECT_EQ(policy.fec_percentage(), 15);
  }
}

TEST(LiveTuningPolicyFec, HighInvalidAndNewScopeConfigurationsStayFixed) {
  for (const int configured : {50, 255}) {
    SCOPED_TRACE(configured);
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    policy.configure_fec(configured);
    const auto at = until_rate_independent(policy);
    EXPECT_FALSE(policy.fec_request());
    for (const int other : {10, 49, 51, 60}) {
      policy.fec_applied(at, other);
    }
    EXPECT_EQ(policy.fec_percentage(), configured);
  }
  for (const int invalid : {-1, 256}) {
    SCOPED_TRACE(invalid);
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    policy.configure_fec(invalid);
    const auto at = until_rate_independent(policy);
    EXPECT_FALSE(policy.fec_request());
    policy.fec_applied(at, 15);
    policy.fec_refused(at);
    EXPECT_EQ(policy.fec_percentage(), 0);
  }
  {
    SCOPED_TRACE("configured zero");
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    policy.configure_fec(0);
    until_rate_independent(policy);
    EXPECT_EQ(policy.fec_request(), 5);
  }
  {
    SCOPED_TRACE("new scope");
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    auto at = raised(policy);
    policy.configure_fec(20);
    EXPECT_EQ(policy.fec_percentage(), 20);
    for (int i = 0; i < 4; ++i, at += 1000) {
      policy.report(at, 120, RATE_INDEPENDENT_LOST);
      EXPECT_FALSE(policy.fec_request()) << i;  // the old trial is gone with its scope
    }
  }
}

TEST(LiveTuningPolicyFec, ParityStateNeverChangesTheRateDecisions) {
  struct trace_t {
    std::vector<int> targets;
    std::uint64_t changes;
    live_tuning_policy::state_e state;
    double background;
    int highest_fec;
  };
  // 0: unconfigured. 1: configured, no actuator. 2: configured, the actuator publishes every request.
  const auto replay = [](int mode) {
    live_tuning_policy::policy_t policy {BASE, FLOOR};
    if (mode > 0) {
      policy.configure_fec(10);
    }
    trace_t trace {{}, 0, live_tuning_policy::state_e::normal, 0.0, policy.fec_percentage()};
    for (int i = 0; i < 240; ++i) {
      const std::int64_t t0 = std::int64_t {i} * 1000;
      const double lost = i < 20 ? 0.0 : i < 80 ? RATE_INDEPENDENT_LOST : i < 120 ? 0.0 : i < 135 ? 60.0 : 0.0;
      policy.report(t0 + 500, 120, lost);
      for (int k = 0; k < 10; ++k) {
        policy.rtt(t0 + 100 * k + 50, 3.0);
      }
      if (mode == 0) {
        EXPECT_FALSE(policy.fec_request());
      }
      if (const auto wanted = policy.fec_request(); mode == 2 && wanted) {
        policy.fec_applied(t0 + 999, *wanted);
      }
      trace.highest_fec = std::max(trace.highest_fec, policy.fec_percentage());
      trace.targets.push_back(policy.target_kbps());
    }
    trace.changes = policy.changes();
    trace.state = policy.state();
    trace.background = policy.background_loss();
    return trace;
  };
  const auto plain = replay(0);
  EXPECT_EQ(plain.highest_fec, 0);
  for (const int mode : {1, 2}) {
    SCOPED_TRACE(mode);
    const auto fec = replay(mode);
    EXPECT_EQ(fec.targets, plain.targets);
    EXPECT_EQ(fec.changes, plain.changes);
    EXPECT_EQ(fec.state, plain.state);
    EXPECT_EQ(fec.background, plain.background);
    EXPECT_EQ(fec.highest_fec, mode == 2 ? 15 : 10);
  }
}

TEST(LiveTuningPolicyNative, DedicatedLowerTargetFreezesEnetLossTimersAndFecWithoutLearning) {
  live_tuning_policy::policy_t policy {20000, 2000};
  policy.configure_fec(10);
  for (int i = 0; i < 40; ++i) { policy.rtt(i * 1000, 3); policy.report(i * 1000, 120, 0); }
  const double background = policy.background_loss();
  const auto ceiling = policy.ceiling_kbps();
  ASSERT_TRUE(policy.native_downward(40000, 20000, 17000));
  ASSERT_EQ(policy.state(), live_tuning_policy::state_e::native_hold);
  EXPECT_EQ(policy.target_kbps(), 17000);
  const auto changes = policy.changes();
  for (int i = 41; i < 2000; ++i) {
    policy.rtt(i * 1000, i % 2 ? 3 : 300);
    policy.report(i * 1000, 120, i % 2 ? 0 : 60);
    policy.tick(i * 1000 + 900);
  }
  EXPECT_EQ(policy.target_kbps(), 17000); EXPECT_EQ(policy.changes(), changes);
  EXPECT_EQ(policy.background_loss(), background); EXPECT_EQ(policy.ceiling_kbps(), ceiling);
  EXPECT_FALSE(policy.fec_request()); EXPECT_EQ(policy.fec_percentage(), 10);
  EXPECT_FALSE(policy.native_downward(2000000, 17000, 14450));
  policy.adopt(2000000, 17000);
  EXPECT_EQ(policy.state(), live_tuning_policy::state_e::native_hold);
  policy.adopt(2000001, 16000); // A different outside writer retains its existing authority.
  EXPECT_EQ(policy.target_kbps(), 16000); EXPECT_EQ(policy.state(), live_tuning_policy::state_e::hold);
}

TEST(LiveTuningPolicyNative, RefusesWrongCurrentFloorUpwardAndExistingLossFamily) {
  live_tuning_policy::policy_t policy {20000, 2000};
  EXPECT_FALSE(policy.native_downward(0, 19000, 17000));
  EXPECT_FALSE(policy.native_downward(0, 20000, 20000));
  EXPECT_FALSE(policy.native_downward(0, 20000, 25000));
  EXPECT_FALSE(policy.native_downward(0, 20000, 1999));
  policy.adopt(1, 18000);
  const auto ceiling = policy.ceiling_kbps();
  EXPECT_FALSE(policy.native_downward(2, 18000, 15300));
  EXPECT_EQ(policy.target_kbps(), 18000); EXPECT_EQ(policy.ceiling_kbps(), ceiling);
}
