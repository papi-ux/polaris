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

}  // namespace

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
