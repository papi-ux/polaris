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

}  // namespace

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
    // A retry every 20 reports costs about one second over the link.
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
