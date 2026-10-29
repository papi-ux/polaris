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
 *
 * After the frozen bench, a wire layer under the same policy: packets, FEC blocks and sends as the RP6 measured
 * them, so loss on the network can be judged in packet terms. It leaves the 26 cells as they are. Last, links close
 * to the floor, each compared exactly with the policy at staging 457eeef9.
 */

#include "src/live_tuning_policy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
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
      // Keep every original51d input and also exercise this controller's own step boundaries.
      const auto unpaused = run(base_fx, o);
      const std::vector<int> reference_steps {32, 35, 38, 41};
      auto phase_steps = reference_steps;
      int candidate_steps = 0;
      for (int second : unpaused.step_seconds) {
        if (second > 30 && candidate_steps < 4) {
          phase_steps.push_back(second);
          ++candidate_steps;
        }
      }
      std::vector<int> starts {20, 26, 27, 28, 29, 31, 45, 70, 101, 150, 233, 300};
      for (int second : phase_steps) {
        starts.push_back(second);
        starts.push_back(second + 1);
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
      std::set<int> recovery_points;
      for (const auto *steps : {&reference_steps, &unpaused.step_seconds}) {
        int used = 0;
        for (int second : *steps) {
          if (second <= 30 || used >= 3) {
            continue;
          }
          recovery_points.insert(second);
          recovery_points.insert(second + 1);
          ++used;
        }
      }
      for (int recovery : recovery_points) {
        runs.push_back(run(recovers_at("adversarial-loss-ends-mid-probe-at-" + std::to_string(recovery), "A-loss-ends-mid-probe", recovery), o));
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
  std::string fixture_roster;
  for (const auto &r : runs) {
    fixture_roster += r.scenario + ";" + r.family + ";" + r.order + "\n";
  }
  RecordProperty("frozen_fixture_roster", fixture_roster);
  const std::set<std::string> shrunk_families {"F1-link-100000", "F2-link-50000", "F8-link-200000", "F3-link-100000-1in120"};
  std::size_t metric = 0;
  for (const auto &r : runs) {
    if (shrunk_families.count(r.family)) {
      RecordProperty("over_capacity_last120_" + std::to_string(metric++),
                     "scenario=" + r.scenario + ";order=" + r.order + ";tenths=" + std::to_string(r.over_capacity_last120));
    }
  }
  std::ifstream baseline(std::string(POLARIS_SOURCE_DIR) + "/tests/fixtures/live-tuning-acceptance-51d32f3a.txt");
  ASSERT_TRUE(baseline.is_open());
  std::set<std::string> original_roster;
  std::string descriptor;
  while (std::getline(baseline, descriptor)) {
    ASSERT_TRUE(original_roster.insert(descriptor).second) << descriptor;
  }
  ASSERT_EQ(original_roster.size(), 519u);
  std::set<std::string> actual_roster;
  std::size_t additions = 0;
  for (const auto &r : runs) {
    const auto name = r.scenario + ";" + r.family + ";" + r.order;
    ASSERT_TRUE(actual_roster.insert(name).second) << name;
    additions += original_roster.count(name) == 0;
  }
  for (const auto &name : original_roster) {
    EXPECT_TRUE(actual_roster.count(name)) << name;
  }
  ASSERT_EQ(runs.size(), original_roster.size() + additions);
  RecordProperty("original_fixture_count", static_cast<int>(original_roster.size()));
  RecordProperty("candidate_only_fixture_count", static_cast<int>(additions));
  RecordProperty("total_fixture_count", static_cast<int>(runs.size()));
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

namespace {

  /**
   * The bench's wire layer: what loss on the network does to the frames Live Tuning hears about.
   *
   * Calibrated to the RP6 on 2026-10-29 (#228): each frame is one FEC block of ceil(bytes / 1384) data shards,
   * at least two, plus max(2, ceil(10% of them)) parity, 19.56 data shards at 25987 kbps as the sender counted.
   * It leaves in sends of up to 46 packets with the pacer off, one GSO skb a frame, and of 5 packets with it on.
   * A frame is lost when it loses more shards than its parity, and the client then counts 3 frames lost and
   * one more expected: every lost count in the pacer-off run was a multiple of 3. With 1 in 50 skbs dropped this
   * gives 5.9% at every rate with the pacer off (measured 5.4-5.7%), and with it on 5.8% at 4000 kbps, 15.8% at
   * 15592 and 23.2% at 25987 (measured 6.3%, 15.5% and 24.2%). Synthetic: no device, encoder or RTT evidence.
   */
  struct wire_t {
    int batch = 46;  ///< packets a send: 46 with the pacer off, 5 with it on
    double skb_drop = 0.0;  ///< each send dropped whole
    double packet_drop = 0.0;  ///< each packet dropped on its own
    std::optional<int> link_kbps;  ///< above it, each packet dropped with the overload's share
  };

  constexpr int k_wire_base = 25987;
  constexpr int k_wire_seconds = 400;
  constexpr int k_wire_loss_from = 26;
  constexpr double k_wire_fps = 120;

  /// Whether one frame sent at kbps survives FEC. Arithmetic only, so every platform draws the same frames.
  bool wire_frame_lost(const wire_t &wire, int kbps, rng_t &rng) {
    double spread = -6.0;
    for (int i = 0; i < 12; ++i) {
      spread += rng.next();
    }
    const double bytes = kbps * 1000.0 / 8.0 / k_wire_fps * std::max(0.2, 1.0 + 0.25 * spread);
    const int data = std::max(2, static_cast<int>(std::ceil(bytes / 1384.0)));
    const int parity = std::max(2, (data * 10 + 99) / 100);
    const int packets = data + parity;
    const double overload = wire.link_kbps && kbps > *wire.link_kbps ? 1.0 - static_cast<double>(*wire.link_kbps) / kbps : 0.0;
    int lost = 0;
    for (int first = 0; first < packets; first += wire.batch) {
      const int sent = std::min(wire.batch, packets - first);
      if (rng.next() < wire.skb_drop) {
        lost += sent;
        continue;
      }
      for (int i = 0; i < sent; ++i) {
        lost += rng.next() < wire.packet_drop || rng.next() < overload;
      }
    }
    return lost > parity;
  }

  struct wire_run_t {
    int floor = 0;  ///< tenths of a second at the floor
    int below_half = 0;  ///< tenths below half the request
    int returns_cut = 0;  ///< returns to the request cut again within 10 s
    std::optional<int> recovery;  ///< tenths from the loss ending to a stable request
    double expected = 0;
    double lost = 0;
  };

  /// One run: 120 frames a second at the encoder's rate, a report about every second, RTT polls at 10 Hz, and the
  /// encoder taking each new target 100-300 ms later. Loss is on from second 26 until loss_until.
  wire_run_t wire_run(const wire_t &wire, std::uint64_t seed, int loss_until = k_wire_seconds) {
    // As background_series() does: an odd multiplier other than the generator's own step, so no seed's draws are
    // another's shifted.
    rng_t rng {seed * 0x100000001b3ULL + 7};
    live_tuning_policy::policy_t policy {k_wire_base, k_floor};
    int applied = k_wire_base;
    std::optional<std::int64_t> pending_since;
    std::int64_t ack_delay = 0;
    double expected = 0;
    double lost = 0;
    std::int64_t next_report = 500;
    std::int64_t next_frame_us = 0;
    std::optional<std::int64_t> returned_at;
    int last_target = k_wire_base;
    std::vector<int> applied_10hz;
    wire_run_t result;
    for (std::int64_t now = 0; now < std::int64_t {k_wire_seconds} * 1000; ++now) {
      while (next_frame_us <= now * 1000) {
        const auto second = next_frame_us / 1000000;
        const bool lossy = second >= k_wire_loss_from && second < loss_until;
        expected += 1;
        if (lossy && wire_frame_lost(wire, applied, rng)) {
          expected += 1;
          lost += 3;
        }
        next_frame_us += static_cast<std::int64_t>(1e6 / k_wire_fps);
      }
      if (now % 100 == 50) {
        policy.rtt(now, 3.0);
      }
      if (now == next_report) {
        lost = std::min(lost, expected);
        policy.report(now, expected, lost);
        result.expected += expected;
        result.lost += lost;
        expected = lost = 0;
        next_report += 990 + static_cast<std::int64_t>(rng.next() * 21);
      }
      if (policy.target_kbps() != last_target) {
        if (policy.target_kbps() >= k_wire_base) {
          returned_at = now;
        } else if (last_target >= k_wire_base && returned_at && now - *returned_at < 10000) {
          ++result.returns_cut;
        }
        last_target = policy.target_kbps();
      }
      if (policy.target_kbps() == applied) {
        pending_since.reset();
      } else if (!pending_since) {
        pending_since = now;
        ack_delay = 100 + static_cast<std::int64_t>(rng.next() * 201);
      } else if (now - *pending_since >= ack_delay) {
        applied = policy.target_kbps();
        pending_since.reset();
      }
      if (now % 100 == 0) {
        applied_10hz.push_back(applied);
        result.floor += applied <= k_floor;
        result.below_half += applied < k_wire_base / 2.0;
      }
    }
    if (loss_until < k_wire_seconds) {
      const auto from = static_cast<std::size_t>(loss_until) * 10;
      for (std::size_t i = applied_10hz.size(); i-- > from && applied_10hz[i] == k_wire_base;) {
        result.recovery = static_cast<int>(i - from);
      }
    }
    return result;
  }

  struct wire_family_t {
    std::string name;
    wire_t wire;
  };

  std::vector<wire_run_t> wire_runs(const wire_t &wire, int loss_until = k_wire_seconds) {
    std::vector<wire_run_t> runs;
    for (std::uint64_t seed = 0; seed < 40; ++seed) {
      runs.push_back(wire_run(wire, seed, loss_until));
    }
    return runs;
  }

  /// Median, 90th percentile and maximum over the seeds, for the test's XML properties.
  std::string wire_spread(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto at = [&](double q) {
      return values[static_cast<std::size_t>(std::lround(q * static_cast<double>(values.size() - 1)))];
    };
    return "median=" + std::to_string(at(0.5)) + ";p90=" + std::to_string(at(0.9)) + ";max=" + std::to_string(at(1.0));
  }

}  // namespace

TEST(LiveTuningAcceptance, WireLossUnderThreePercentNeverReachesTheFloor) {
  // #228's "never at the floor on background loss under 3%", read on the wire: skbs dropped whole with the pacer off
  // and on, and single packets dropped at random, each at 1, 2 and 2.9%, over 40 seeds. The seconds below half the
  // request are recorded, not judged: with the pacer on, or with single packets lost, frame loss grows with the rate.
  std::vector<wire_family_t> families;
  for (const double share : {0.01, 0.02, 0.029}) {
    const auto pct = std::to_string(static_cast<int>(std::lround(share * 1000)));
    families.push_back({"pacer_off_skb_" + pct + "permille", {46, share, 0.0, std::nullopt}});
    families.push_back({"pacer_on_skb_" + pct + "permille", {5, share, 0.0, std::nullopt}});
    families.push_back({"random_packets_" + pct + "permille", {46, 0.0, share, std::nullopt}});
  }
  for (const auto &family : families) {
    SCOPED_TRACE(family.name);
    const auto runs = wire_runs(family.wire);
    int at_floor = 0;
    std::vector<double> below_half;
    std::vector<double> loss;
    for (const auto &run : runs) {
      at_floor += run.floor > 0;
      below_half.push_back(run.below_half / 10.0);
      loss.push_back(100.0 * run.lost / run.expected);
    }
    RecordProperty("wire_" + family.name + "_below_half_s", wire_spread(below_half));
    RecordProperty("wire_" + family.name + "_frame_loss_pct", wire_spread(loss));
    RecordProperty("wire_" + family.name + "_runs_at_floor", at_floor);
    EXPECT_EQ(at_floor, 0);
  }
}

TEST(LiveTuningAcceptance, PacerOnLossIsRarelyCutAgainRightAfterAReturnToTheRequest) {
  // Arm D on 2026-10-29 went back to its request twice in 180 s under the same loss, as rate-independent, and lost
  // 20-35% of its frames for 3 s each time before the next cut. At its 1 in 50 skbs and at 2.9%, over 40 seeds, such
  // returns average at most 1.5 a 400 s run. At 1% the request loses little more than the rates under it: recorded.
  for (const double share : {0.01, 0.02, 0.029}) {
    SCOPED_TRACE(share);
    const auto runs = wire_runs({5, share, 0.0, std::nullopt});
    int returns_cut = 0;
    for (const auto &run : runs) {
      returns_cut += run.returns_cut;
    }
    RecordProperty("wire_pacer_on_skb_" + std::to_string(static_cast<int>(std::lround(share * 1000))) + "permille_returns_cut", returns_cut);
    if (share > 0.01) {
      EXPECT_LE(returns_cut * 2, static_cast<int>(runs.size()) * 3);
    }
  }
}

TEST(LiveTuningAcceptance, AHoldUnderClearlyHeavyLossProbesUnderIt) {
  // A 3000 kbps link under a 25987 kbps request: the probe's deep sample at 4000 loses more than a fifth of its
  // frames and must not become the hold.
  const auto runs = wire_runs({46, 0.0, 0.0, 3000});
  std::vector<double> loss;
  for (const auto &run : runs) {
    loss.push_back(100.0 * run.lost / run.expected);
  }
  RecordProperty("wire_link_3000_frame_loss_pct", wire_spread(loss));
  std::sort(loss.begin(), loss.end());
  EXPECT_LE(loss[loss.size() / 2], 20.0);
}

TEST(LiveTuningAcceptance, ALinkBelowTheFloorStillReachesIt) {
  // The floor stays for heavy loss: a 1500 kbps link is under it, so every run goes there.
  for (const auto &run : wire_runs({46, 0.0, 0.0, 1500})) {
    EXPECT_GT(run.floor, 0);
  }
}

TEST(LiveTuningAcceptance, WireRecoveryIsRecordedAsATarget) {
  // Recovery to the request after 1 in 50 skbs stops being dropped at second 206: recorded, not judged (30 s is a
  // target). With the pacer on the stream climbs from where the loss settled it.
  for (const int batch : {46, 5}) {
    std::vector<double> recovery;
    for (const auto &run : wire_runs({batch, 0.02, 0.0, std::nullopt}, 206)) {
      recovery.push_back((run.recovery ? *run.recovery : 10.0 * (k_wire_seconds - 206)) / 10.0);
    }
    RecordProperty(std::string("wire_recovery_") + (batch == 5 ? "pacer_on" : "pacer_off") + "_s", wire_spread(recovery));
  }
}

namespace {

  /**
   * Links close to the floor (#575 review): requests from 1.3 to 2.2 times their floor, where a probe's first step
   * reaches the floor or nearly, over a link that loses frames above it. 120 frames a report at 500 ms, a 3 ms RTT
   * every 100 ms and loss from second 30, carried so each report loses exactly its share. No draws, so each row is
   * compared exactly with the policy at staging 457eeef9, from tests/fixtures/live-tuning-near-floor-457eeef9.txt.
   */
  struct near_floor_t {
    std::string shape;  ///< "tail": 1 - link/rate of the frames over it; "step": 96 of 120 above it
    int request = 0;
    int link = 0;
    int floor = 0;
    double lost = 0;  ///< frames lost in seconds 100-299, of k_near_floor_frames
    int final = 0;  ///< the target after 300 s
  };

  constexpr double k_near_floor_frames = 24000;
  constexpr double k_near_floor_margin = 24;  // 0.1 points of k_near_floor_frames

  std::vector<near_floor_t> near_floor_reference() {
    std::ifstream input(std::string(POLARIS_SOURCE_DIR) + "/tests/fixtures/live-tuning-near-floor-457eeef9.txt");
    std::vector<near_floor_t> rows;
    for (std::string line; std::getline(input, line);) {
      if (line.empty() || line.front() == '#') {
        continue;
      }
      std::istringstream fields(line);
      near_floor_t row;
      fields >> row.shape >> row.request >> row.link >> row.floor >> row.lost >> row.final;
      rows.push_back(row);
    }
    return rows;
  }

  std::string near_floor_name(const near_floor_t &row) {
    return "near_floor_" + row.shape + "_" + std::to_string(row.request) + "_over_" + std::to_string(row.link) + "_floor_" +
           std::to_string(row.floor);
  }

  struct near_floor_run_t {
    double lost = 0;
    int final = 0;
    int lowest = 0;
    double background = 0;
    live_tuning_policy::state_e state = live_tuning_policy::state_e::normal;
  };

  near_floor_run_t near_floor_run(const near_floor_t &row) {
    live_tuning_policy::policy_t policy {row.request, row.floor};
    near_floor_run_t run;
    run.lowest = row.request;
    double carry = 0;
    for (int i = 0; i < 300; ++i) {
      const int kbps = policy.target_kbps();
      double share = 0;
      if (i >= 30) {
        share = row.shape == "tail" ? std::max(0.0, 1.0 - static_cast<double>(row.link) / kbps) : kbps > row.link ? 0.8 : 0.0;
      }
      const double owed = 120.0 * share + carry;
      const double lost = std::floor(owed);
      carry = owed - lost;
      for (int k = 0; k < 10; ++k) {
        if (k == 5) {
          policy.report(std::int64_t {i} * 1000 + 500, 120.0, lost);
        }
        policy.rtt(std::int64_t {i} * 1000 + 100 * k + 50, 3.0);
      }
      if (i >= 100) {
        run.lost += lost;
      }
      run.lowest = std::min(run.lowest, policy.target_kbps());
    }
    run.final = policy.target_kbps();
    run.background = policy.background_loss();
    run.state = policy.state();
    return run;
  }

}  // namespace

TEST(LiveTuningAcceptance, CongestionCloseToTheFloorSettlesUnderTheLinkAsStaging457eeef9Did) {
  // #575 review: a request this close to its floor had its first step stop a climb step above the floor, 95981 kbps
  // of a 100000 request over a 76785 PyroWave floor, with no room for a deep sample under it. The stream took its
  // link's loss for background and stayed at its request, losing 36.7% over a 1900 kbps link, 19.2% over 2100, and
  // 20% over an 80000 kbps link. A fresh first step lands on the floor again, and the stream settles under the link.
  const auto reference = near_floor_reference();
  for (const auto &[shape, request, link, floor] : std::vector<std::tuple<std::string, int, int, int>> {
         {"tail", 3000, 1900, 2000},
         {"tail", 2600, 2100, 2000},
         {"tail", 100000, 80000, 76785},
       }) {
    const auto row = std::find_if(reference.begin(), reference.end(), [&](const near_floor_t &r) {
      return r.shape == shape && r.request == request && r.link == link && r.floor == floor;
    });
    ASSERT_NE(row, reference.end());
    SCOPED_TRACE(near_floor_name(*row));
    const auto run = near_floor_run(*row);
    EXPECT_EQ(run.lowest, floor);
    EXPECT_EQ(run.state, live_tuning_policy::state_e::hold);
    EXPECT_LE(run.final, std::max(link, floor));
    EXPECT_EQ(run.background, 0.0);
    EXPECT_LE(run.lost, row->lost + k_near_floor_margin);
  }
}

TEST(LiveTuningAcceptance, LinksCloseToTheFloorDoNoWorseThanStaging457eeef9) {
  // Every row: at most 0.1 points more loss than 457eeef9, and no lower final rate unless the loss is lower by more.
  // One named exception, ROOT's decision on 2026-10-29: 3200 kbps over a 2450 kbps tail drop. Later in the episode a
  // descent from the hold at 2529 stops at 2500, a 1% step that cannot tell the link's loss from background, where
  // 457eeef9 stepped to the clean floor. Sending such a step to the floor instead puts wire runs under 3% loss on it
  // (WireLossUnderThreePercentNeverReachesTheFloor), so this row is bounded: no learned background, at most 5% lost,
  // and settled within one bisect step (the policy's 1.06) of its link.
  const auto reference = near_floor_reference();
  ASSERT_EQ(reference.size(), 141u);
  int same = 0;
  int less = 0;
  int more = 0;
  for (const auto &row : reference) {
    const auto name = near_floor_name(row);
    SCOPED_TRACE(name);
    const auto run = near_floor_run(row);
    same += run.lost == row.lost && run.final == row.final;
    less += run.lost < row.lost - k_near_floor_margin;
    more += run.lost > row.lost + k_near_floor_margin;
    if (row.shape == "tail" && row.request == 3200 && row.link == 2450) {
      RecordProperty(name, "lost=" + std::to_string(static_cast<int>(run.lost)) + ";final=" + std::to_string(run.final) +
                             ";staging_lost=" + std::to_string(static_cast<int>(row.lost)) + ";staging_final=" + std::to_string(row.final));
      EXPECT_EQ(run.background, 0.0);
      EXPECT_LE(run.lost, 0.05 * k_near_floor_frames);
      EXPECT_LE(run.final, row.link * 1.06);
      continue;
    }
    EXPECT_LE(run.lost, row.lost + k_near_floor_margin) << run.final << " kbps at the end; staging " << row.final;
    if (run.final < row.final) {
      EXPECT_LE(run.lost, row.lost - k_near_floor_margin) << run.final << " kbps at the end; staging " << row.final;
    }
  }
  RecordProperty("near_floor_rows", static_cast<int>(reference.size()));
  RecordProperty("near_floor_same_as_457eeef9", same);
  RecordProperty("near_floor_less_loss_than_457eeef9", less);
  RecordProperty("near_floor_more_loss_than_457eeef9", more);
}
