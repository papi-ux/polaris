/**
 * @file tests/unit/test_live_tuning_acceptance.cpp
 * @brief Live Tuning's policy against the frozen acceptance bench: its fixtures, harness and 26 cells.
 *
 * A port of the bench's replay.cpp and summarize.py. The fixtures, the time base, the 1200 media units per
 * report, the constant 2 ms RTT of the ping orders, the per-run measures and every threshold are the bench's,
 * unchanged. Measures count tenths of a second, as the bench prints them. Only the controller is Polaris's own:
 * live_tuning_policy::policy_t, with RTT polls as rtt(), ticks as tick(), and its probe, recheck, bisect and climb
 * states read as the bench's descending and evaluating modes. Synthetic controller decisions on injected time;
 * not encoder, counter or device evidence.
 */

#include "src/live_tuning_policy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

  constexpr int k_base = 268988;
  constexpr int k_floor = 2000;
  constexpr int k_seconds = 400;
  constexpr double k_units = 1200;  // media units per one-second report, as the bench sends them

  struct rng_t {
    std::uint64_t s;

    double next() {  // splitmix64 -> [0,1), identical on every platform
      std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
      z ^= z >> 31;
      return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }
  };

  struct fixture_t {
    std::string name;
    std::string family;
    std::function<std::optional<int>(int)> capacity;  ///< model capacity at second s, if rate-sensitive
    double above_loss_pct = 5.0;  ///< loss on the share of an interval above capacity
    std::function<double(int)> independent;  ///< rate-independent loss at second s, every rate
    std::set<int> paused;  ///< seconds whose report never arrives
    std::optional<int> recovery_second;
    bool rate_independent_family = false;
    bool background_under_3 = false;
  };

  struct order_t {
    std::string name;
    bool ping = false;
    int phase = 0;  ///< 100 ms slot of the report within each second (ping orders)
  };

  /// The bench's modes, from the policy's states.
  std::string mode_of(const live_tuning_policy::policy_t &policy) {
    using s = live_tuning_policy::state_e;
    switch (policy.state()) {
      case s::descend:
      case s::rtt_cut:
        return "descending";
      case s::verify_hi:
      case s::bisect:
      case s::climb:
        return "evaluating";
      default:
        return "steady";
    }
  }

  struct second_row_t {
    double reported = -1;  ///< loss percent, four decimals as the trace keeps it; -1 when paused
    std::string mode;
  };

  struct run_t {
    std::string scenario;
    std::string family;
    std::string order;
    std::vector<int> applied_10hz;
    std::vector<second_row_t> rows;  ///< per second, from second 1
    std::vector<int> step_seconds;
    bool climbed_during_gap = false;
    bool rate_independent_family = false;
    bool background_under_3 = false;
    // The bench's measures, in tenths of a second.
    int floor = 0;
    int below_half_usable = 0;
    int over_capacity_last120 = 0;
    int over_capacity_during_gap = 0;
    std::optional<int> recovery;
  };

  run_t run(const fixture_t &fx, const order_t &order) {
    live_tuning_policy::policy_t policy {k_base, k_floor};
    int applied = k_base;
    std::vector<std::pair<std::int64_t, int>> timeline {{0, k_base}};  // in ms
    std::optional<std::int64_t> pending_since;
    std::int64_t last_report_end = 0;
    std::int64_t last_report_arrival = 0;
    int requested_before_gap = k_base;
    bool in_gap = false;
    std::uint64_t steps_seen = 0;
    std::uint64_t changes_seen = 0;
    run_t result;
    result.scenario = fx.name;
    result.family = fx.family;
    result.order = order.name;
    result.rate_independent_family = fx.rate_independent_family;
    result.background_under_3 = fx.background_under_3;

    // The bench counts every change that is not a cut as a step.
    const auto note_steps = [&] {
      if (policy.changes() == changes_seen) {
        return;
      }
      changes_seen = policy.changes();
      const auto &change = policy.last_change();
      if (change.to_kbps >= change.from_kbps) {
        ++steps_seen;
      }
    };
    const auto fraction_above = [&](std::int64_t from, std::int64_t to, std::optional<int> cap) {
      if (!cap) {
        return 0.0;
      }
      double above = 0;
      for (std::size_t i = 0; i < timeline.size(); ++i) {
        const auto begin = std::max(from, timeline[i].first);
        const auto end = std::min(to, i + 1 < timeline.size() ? timeline[i + 1].first : to);
        if (end > begin && timeline[i].second > *cap) {
          above += static_cast<double>(end - begin);
        }
      }
      return above / static_cast<double>(to - from);
    };
    const auto apply_if_due = [&](std::int64_t now) {
      if (policy.target_kbps() == applied) {
        pending_since.reset();
        return;
      }
      if (!pending_since) {
        pending_since = now;
      }
      // The encoder picks the request up on its next pass: the same instant in report order, the next
      // 100 ms poll in ping order.
      const std::int64_t delay = order.ping ? 100 : 0;
      if (now - *pending_since >= delay) {
        applied = policy.target_kbps();
        timeline.emplace_back(now, applied);
        pending_since.reset();
      }
    };
    const auto deliver_report = [&](int second, std::int64_t now) {
      const std::int64_t start = std::max<std::int64_t>(last_report_end, now - 1000);
      const auto cap = fx.capacity ? fx.capacity(second) : std::nullopt;
      double loss = fx.above_loss_pct * fraction_above(start, now, cap) + (fx.independent ? fx.independent(second) : 0.0);
      loss = std::clamp(loss, 0.0, 100.0);
      policy.report(now, k_units, static_cast<double>(std::lround(loss * k_units / 100.0)));
      note_steps();
      last_report_end = now;
      last_report_arrival = now;
      return std::round(loss * 1e4) / 1e4;
    };
    const auto gap_check = [&](std::int64_t now) {
      const bool missing = now - last_report_arrival > 1500;
      if (missing && !in_gap) {
        in_gap = true;
        requested_before_gap = policy.target_kbps();
      }
      if (in_gap && policy.target_kbps() > requested_before_gap) {
        result.climbed_during_gap = true;
      }
      if (in_gap) {
        requested_before_gap = std::min(requested_before_gap, policy.target_kbps());
      }
    };

    std::uint64_t steps_recorded = 0;
    for (int second = 1; second <= k_seconds; ++second) {
      double reported = -1;
      if (!order.ping) {
        const std::int64_t now = std::int64_t {second} * 1000;
        policy.tick(now);
        note_steps();
        gap_check(now);
        apply_if_due(now);
        if (!fx.paused.count(second)) {
          reported = deliver_report(second, now);
          in_gap = false;
        }
        apply_if_due(now);
        result.applied_10hz.insert(result.applied_10hz.end(), 10, applied);
      } else {
        for (int k = 0; k < 10; ++k) {
          const std::int64_t now = std::int64_t {second - 1} * 1000 + (k + 1) * 100;
          policy.rtt(now, 2.0);
          note_steps();
          gap_check(now);
          apply_if_due(now);
          if (k == order.phase && !fx.paused.count(second)) {
            reported = deliver_report(second, now);
            in_gap = false;
            apply_if_due(now);
          }
          result.applied_10hz.push_back(applied);
        }
      }
      if (steps_seen != steps_recorded) {
        steps_recorded = steps_seen;
        result.step_seconds.push_back(second);
      }
      result.rows.push_back({reported, mode_of(policy)});
    }

    // Usable ceiling: min(base, model capacity); the base where loss is rate-independent.
    for (std::size_t i = 0; i < result.applied_10hz.size(); ++i) {
      const int second = static_cast<int>(i / 10) + 1;
      const int a = result.applied_10hz[i];
      const auto cap = fx.capacity ? fx.capacity(second) : std::nullopt;
      const int usable = cap ? std::min(k_base, *cap) : k_base;
      result.below_half_usable += a < usable / 2.0;
      result.floor += a <= k_floor;
      if (cap && a > *cap) {
        result.over_capacity_during_gap += fx.paused.count(second) > 0;
        result.over_capacity_last120 += second > k_seconds - 120;
      }
    }
    if (fx.recovery_second) {
      const std::size_t from = static_cast<std::size_t>(*fx.recovery_second) * 10;
      std::optional<std::size_t> stable;
      for (std::size_t i = result.applied_10hz.size(); i-- > from;) {
        if (result.applied_10hz[i] != k_base) {
          break;
        }
        stable = i;
      }
      if (stable) {
        result.recovery = static_cast<int>(*stable - from);
      }
    }
    return result;
  }

  std::vector<double> background_series(std::uint64_t seed, double max_pct) {
    rng_t rng {seed * 0x100000001b3ULL + 17};
    std::vector<double> values(k_seconds + 2);
    for (auto &v : values) {
      v = rng.next() * max_pct;
    }
    return values;
  }

  double rp6_periodic(int s) {
    return (s % 8 == 1 || s % 8 == 6) ? 7.4 : 0.0;
  }

  fixture_t link(const std::string &name, int capacity, int from_second = 26) {
    fixture_t fx;
    fx.name = name;
    fx.family = "link";
    fx.capacity = [capacity, from_second](int s) -> std::optional<int> {
      if (s < from_second) {
        return std::nullopt;
      }
      return capacity;
    };
    return fx;
  }

  fixture_t recovers_at(const std::string &name, const std::string &family, int recovery) {
    fixture_t fx;
    fx.name = name;
    fx.family = family;
    fx.capacity = [recovery](int s) -> std::optional<int> {
      if (s < 26) {
        return std::nullopt;
      }
      return s < recovery ? 100000 : 1000000;
    };
    fx.recovery_second = recovery;
    return fx;
  }

  /// Every run the bench makes.
  std::vector<run_t> all_runs() {
    std::vector<run_t> runs;
    const std::vector<order_t> deterministic_orders {
      {"reports", false, 0},
      {"ping_10hz_p0", true, 0},
      {"ping_10hz_p3", true, 3},
      {"ping_10hz_p5", true, 5},
      {"ping_10hz_p9", true, 9},
    };
    const auto seeded_orders = [](std::uint64_t seed) {
      const int phases[] = {0, 3, 5, 9};
      const int phase = phases[static_cast<std::size_t>(seed % 4)];
      return std::vector<order_t> {{"reports", false, 0}, {"ping_10hz_p" + std::to_string(phase), true, phase}};
    };
    const auto run_all = [&](const fixture_t &fx, const std::vector<order_t> &orders) {
      for (const auto &o : orders) {
        runs.push_back(run(fx, o));
      }
    };

    // The link drops.
    for (int capacity : {100000, 50000, 200000}) {
      auto fx = link("link-" + std::to_string(capacity), capacity);
      fx.family = capacity == 100000 ? "F1-link-100000" : capacity == 50000 ? "F2-link-50000" : "F8-link-200000";
      run_all(fx, deterministic_orders);
    }
    // The 100 Mbps link with 1 in 120 lost at every rate.
    {
      auto fx = link("link-100000-one-in-120", 100000);
      fx.family = "F3-link-100000-1in120";
      fx.independent = [](int) {
        return 100.0 / 120.0;
      };
      fx.rate_independent_family = true;
      fx.background_under_3 = true;
      run_all(fx, deterministic_orders);
    }
    // Recovery to 1 Gbps at 40 seeded moments.
    for (std::uint64_t seed = 0; seed < 40; ++seed) {
      rng_t rng {seed + 1000};
      const int recovery = 40 + static_cast<int>(rng.next() * 161);
      run_all(recovers_at("recovery-seed-" + std::to_string(seed), "F4-recovery", recovery), seeded_orders(seed));
    }
    // RP6-like periodic loss: 2 of 8 reports at about 7.4 % whatever the rate.
    {
      fixture_t fx;
      fx.name = "rp6-periodic";
      fx.family = "F5-rp6-periodic";
      fx.independent = rp6_periodic;
      fx.rate_independent_family = true;
      run_all(fx, deterministic_orders);
    }
    // Bursts at every rate.
    {
      fixture_t fx;
      fx.name = "burst-10s-5pct";
      fx.family = "F7-burst";
      fx.independent = [](int s) {
        return (s >= 26 && s < 36) ? 5.0 : 0.0;
      };
      fx.rate_independent_family = true;
      fx.recovery_second = 36;
      run_all(fx, deterministic_orders);
      fx.name = "burst-3s-20pct";
      fx.independent = [](int s) {
        return (s >= 26 && s < 29) ? 20.0 : 0.0;
      };
      fx.recovery_second = 29;
      run_all(fx, deterministic_orders);
    }
    // Random background loss of 0 to 2.5 % at every rate, 40 seeds.
    for (std::uint64_t seed = 0; seed < 40; ++seed) {
      const auto series = background_series(seed, 2.5);
      fixture_t fx;
      fx.name = "background-seed-" + std::to_string(seed);
      fx.family = "F9-background";
      fx.independent = [series](int s) {
        return series[static_cast<std::size_t>(s)];
      };
      fx.rate_independent_family = true;
      fx.background_under_3 = true;
      run_all(fx, seeded_orders(seed));
    }
    // Background that falls by chance for 30 s, then sits at 2.5 % for 30 s; alone and on the shrunk link.
    {
      auto series = background_series(99, 2.5);
      std::fill(series.begin() + 60, series.begin() + 90, 0.0);
      std::fill(series.begin() + 90, series.begin() + 120, 2.5);
      const auto independent = [series](int s) {
        return series[static_cast<std::size_t>(s)];
      };
      fixture_t fx;
      fx.name = "adversarial-background-falls-by-chance";
      fx.family = "A-background-chance";
      fx.independent = independent;
      fx.rate_independent_family = true;
      fx.background_under_3 = true;
      run_all(fx, deterministic_orders);
      auto mixed = link("adversarial-link-100000-with-background", 100000);
      mixed.family = "A-link-background";
      mixed.independent = independent;
      run_all(mixed, deterministic_orders);
    }
    // Report pauses of 3 to 40 s at varied moments, including the seconds a step was taken in the unpaused run,
    // and loss that ends mid-probe.
    const auto base_fx = link("link-100000", 100000);
    for (const order_t &o : {order_t {"reports", false, 0}, order_t {"ping_10hz_p5", true, 5}}) {
      const auto unpaused = run(base_fx, o);
      std::vector<int> starts {20, 26, 27, 28, 29, 31, 45, 70, 101, 150, 233, 300};
      int added = 0;
      for (int s : unpaused.step_seconds) {
        if (s > 30 && added < 4) {
          starts.push_back(s);
          starts.push_back(s + 1);
          ++added;
        }
      }
      std::sort(starts.begin(), starts.end());
      starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
      for (int start : starts) {
        for (int length : {3, 5, 8, 13, 20, 30, 40}) {
          auto fx = base_fx;
          fx.name = "pause-link-100000-at-" + std::to_string(start) + "-for-" + std::to_string(length);
          fx.family = "F6-pause";
          for (int s = start; s < start + length; ++s) {
            fx.paused.insert(s);
          }
          runs.push_back(run(fx, o));
        }
      }
      for (int start : {98, 154, 263}) {
        for (int length : {8, 20, 40}) {
          fixture_t fx;
          fx.name = "pause-rp6-at-" + std::to_string(start) + "-for-" + std::to_string(length);
          fx.family = "F6-pause-rp6";
          fx.independent = rp6_periodic;
          fx.rate_independent_family = true;
          for (int s = start; s < start + length; ++s) {
            fx.paused.insert(s);
          }
          runs.push_back(run(fx, o));
        }
      }
      for (int start : {50, 151}) {
        const auto series = background_series(static_cast<std::uint64_t>(start), 2.5);
        fixture_t fx;
        fx.name = "pause-background-at-" + std::to_string(start) + "-for-40";
        fx.family = "F6-pause-background";
        fx.independent = [series](int s) {
          return series[static_cast<std::size_t>(s)];
        };
        fx.rate_independent_family = true;
        fx.background_under_3 = true;
        for (int s = start; s < start + 40; ++s) {
          fx.paused.insert(s);
        }
        runs.push_back(run(fx, o));
      }
      int used = 0;
      for (int s : unpaused.step_seconds) {
        if (s <= 30 || used >= 3) {
          continue;
        }
        for (int offset : {0, 1}) {
          const int recovery = s + offset;
          runs.push_back(run(recovers_at("adversarial-loss-ends-mid-probe-at-" + std::to_string(recovery), "A-loss-ends-mid-probe", recovery), o));
        }
        ++used;
      }
    }
    return runs;
  }

  struct verdict_t {
    bool pass = true;
    std::string detail;  ///< the first run that fails it
  };

  /// summarize.py's cells, keyed by criterion and order class.
  std::map<std::pair<std::string, std::string>, verdict_t> cells(const std::vector<run_t> &runs) {
    using pred_t = std::function<bool(const run_t &)>;
    std::map<std::pair<std::string, std::string>, verdict_t> out;
    const auto all = [](const std::vector<const run_t *> &selected, const pred_t &ok) {
      for (const auto *r : selected) {
        if (!ok(*r)) {
          return verdict_t {false, r->scenario + "/" + r->order};
        }
      }
      return verdict_t {};
    };
    for (const std::string order : {"reports", "ping_10hz"}) {
      const auto select = [&](const pred_t &pred) {
        std::vector<const run_t *> selected;
        for (const auto &r : runs) {
          if ((r.order == "reports") == (order == "reports") && pred(r)) {
            selected.push_back(&r);
          }
        }
        return selected;
      };
      const auto family_is = [](std::set<std::string> families) {
        return [families = std::move(families)](const run_t &r) {
          return families.count(r.family) > 0;
        };
      };
      const auto set = [&](const std::string &criterion, verdict_t verdict) {
        out[{criterion, order}] = std::move(verdict);
      };

      set("background under 3 % never reaches the floor", all(select([](const run_t &r) {
        return r.background_under_3;
      }), [](const run_t &r) {
        return r.floor == 0;
      }));
      set("rate-independent loss: at most 1 % of fixture time below half the declared usable ceiling", all(select([](const run_t &r) {
        return r.rate_independent_family;
      }), [](const run_t &r) {
        return r.below_half_usable <= k_seconds / 10;
      }));
      {
        std::vector<double> values;
        for (const auto *r : select(family_is({"F4-recovery"}))) {
          values.push_back(r->recovery ? *r->recovery : std::numeric_limits<double>::infinity());
        }
        std::sort(values.begin(), values.end());
        const auto n = values.size();
        const double median = n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
        set("recovery to base: median within 30 s", {median <= 300, "median " + std::to_string(median / 10.0) + " s"});
      }
      set("bursts (10 s at 5 %, 3 s at 20 %): stable base within 30 s of the burst end, no floor", all(select(family_is({"F7-burst"})), [](const run_t &r) {
        return r.recovery && *r.recovery <= 300 && r.floor == 0;
      }));
      set("shrunk links: no floor; at most 10 % of the last 120 s above the link (declared bound)", all(select(family_is({"F1-link-100000", "F2-link-50000", "F8-link-200000", "F3-link-100000-1in120"})), [](const run_t &r) {
        return r.floor == 0 && r.over_capacity_last120 <= 120;
      }));
      const auto gaps = select([](const run_t &r) {
        return r.family.rfind("F6-pause", 0) == 0;
      });
      set("report pauses: never a rate climb while reports are missing", all(gaps, [](const run_t &r) {
        return !r.climbed_during_gap;
      }));
      // Each pause on the shrunk link, by the media evidence there was when it began.
      std::map<std::string, std::vector<const run_t *>> by_class;
      for (const auto *r : gaps) {
        if (r->family != "F6-pause") {
          continue;
        }
        const int start = std::stoi(r->scenario.substr(r->scenario.find("-at-") + 4));
        std::string evidence = "none";  // the link shrinks inside the pause: no report ever showed it
        if (start > 26) {
          const auto &last = r->rows[start - 2];
          bool recent_loss = false;
          for (int s = std::max(1, start - 3); s < start; ++s) {
            recent_loss = recent_loss || r->rows[s - 1].reported >= 3.0;
          }
          evidence = last.mode != "steady" ? "confirmed_or_probe" : recent_loss ? "single_unconfirmed_loss" : "clean";
        }
        by_class[evidence].push_back(r);
      }
      for (const auto &[evidence, members] : by_class) {
        set("report pauses on a shrunk link [" + evidence + "]: at most 6 s above the link inside any pause (declared bound)", all(members, [](const run_t &r) {
          return r.over_capacity_during_gap <= 60;
        }));
      }
      set("pauses during RP6-like rate-independent loss: at most 1 % below half the usable ceiling", all(select(family_is({"F6-pause-rp6"})), [](const run_t &r) {
        return r.below_half_usable <= k_seconds / 10;
      }));
      set("report pauses: no floor", all(gaps, [](const run_t &r) {
        return r.floor == 0;
      }));
      set("loss ending mid-probe: stable base within 30 s, never stuck low", all(select(family_is({"A-loss-ends-mid-probe"})), [](const run_t &r) {
        return r.recovery && *r.recovery <= 300;
      }));
    }
    return out;
  }

}  // namespace

TEST(LiveTuningAcceptance, TheFrozenBenchPasses22CellsAndNamesTheFourThatNeedACongestionSignalWhileReportsStop) {
  const auto runs = all_runs();
  const std::set<std::string> shrunk_families {"F1-link-100000", "F2-link-50000", "F8-link-200000", "F3-link-100000-1in120"};
  std::size_t metric = 0;
  for (const auto &r : runs) {
    if (shrunk_families.count(r.family)) {
      RecordProperty("over_capacity_last120_" + std::to_string(metric++),
                     "scenario=" + r.scenario + ";order=" + r.order + ";tenths=" + std::to_string(r.over_capacity_last120));
    }
  }
  ASSERT_EQ(runs.size(), 519u);
  const auto verdicts = cells(runs);
  ASSERT_EQ(verdicts.size(), 26u);
  // Unmet, in both orders: the link shrinks inside a pause, or just after one lossy report, and nothing tells
  // the policy until reports come back. They need a congestion signal that keeps arriving while media reports
  // stop; the bench's RTT is a constant 2 ms. Nothing here relaxes them.
  const std::set<std::string> unmet {
    "report pauses on a shrunk link [none]: at most 6 s above the link inside any pause (declared bound)",
    "report pauses on a shrunk link [single_unconfirmed_loss]: at most 6 s above the link inside any pause (declared bound)",
  };
  int passed = 0;
  for (const auto &[cell, verdict] : verdicts) {
    EXPECT_EQ(verdict.pass, !unmet.count(cell.first)) << cell.second << ": " << cell.first << " " << verdict.detail;
    passed += verdict.pass;
  }
  EXPECT_EQ(passed, 22);
}
