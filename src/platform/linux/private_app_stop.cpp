/**
 * @file src/platform/linux/private_app_stop.cpp
 * @brief Stop a private session's apps in order, while its compositor is still up.
 */

#include "private_app_stop.h"

#ifdef __linux__

  #include "../../logging.h"

  #include <algorithm>
  #include <map>
  #include <memory>

using namespace std::literals;

namespace private_app_stop {
  namespace fsi = flatpak_session_instances;

  namespace {

    std::chrono::milliseconds remaining_until(
      std::chrono::steady_clock::time_point deadline,
      const actions_t &actions
    ) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - actions.now());
      return std::max(0ms, left);
    }

    std::chrono::milliseconds since(std::chrono::steady_clock::time_point start, const actions_t &actions) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(actions.now() - start);
    }

    std::string cgroup_basename(std::string_view cgroup) {
      const auto slash = cgroup.find_last_of('/');
      return std::string {slash == std::string_view::npos ? cgroup : cgroup.substr(slash + 1)};
    }

    /**
     * After the app closed when asked: what it leaves of the session's own processes, a helper it
     * started or a launcher that outlived its game, is ended before the compositor too. It gets a
     * moment to exit on its own, then SIGTERM with the grace after a close, then SIGKILL.
     */
    bool stop_what_the_app_left(
      const plan_t &plan,
      const actions_t &actions,
      const timings_t &timings,
      std::chrono::steady_clock::time_point app_deadline,
      std::chrono::steady_clock::time_point kill_deadline
    ) {
      if (actions.wait_app(std::min(timings.left_settle, remaining_until(app_deadline, actions)))) {
        return true;
      }
      const auto signalled = actions.sigterm_app();
      if (signalled == 0 && actions.wait_app(0ms)) {
        return true;
      }
      const auto grace = std::min(timings.sigterm_after_close, remaining_until(app_deadline, actions));
      BOOST_LOG(info) << "process: private app stop: SIGTERM to "sv << signalled << " other process(es) of the session the "sv
                      << plan.app_label << " left, waiting up to "sv << grace.count() << " ms"sv;
      if (actions.wait_app(grace)) {
        return true;
      }
      BOOST_LOG(warning) << "process: private app stop: what the "sv << plan.app_label << " left did not exit after SIGTERM; SIGKILL"sv;
      actions.sigkill_app();
      return actions.wait_app(std::min(timings.sigkill_wait, remaining_until(kill_deadline, actions)));
    }

  }  // namespace

  std::chrono::milliseconds close_wait(std::chrono::seconds exit_timeout, const timings_t &timings) {
    const auto own = std::min<std::chrono::milliseconds>(std::max(0s, exit_timeout), timings.exit_timeout_ceiling);
    return std::max(timings.close_wait_floor, own);
  }

  outcome_t run(const plan_t &plan, const actions_t &actions, const timings_t &timings) {
    outcome_t outcome;
    const auto started = actions.now();
    const auto deadline = started + timings.budget;
    // The launcher is quit once its game is gone, and it needs time for that too: settle, quit and
    // the backstop's wait come off the app's share, as does the wait after the app's SIGKILL, so a
    // game that takes long to close cannot leave its launcher only the backstop.
    std::chrono::milliseconds launcher_reserve {};
    if (!plan.launchers.empty()) {
      launcher_reserve = plan.immediate ? timings.immediate_sigterm + timings.backstop_wait :
                                          timings.launcher_settle + timings.launcher_quit + timings.backstop_wait;
    }
    const auto kill_deadline = std::max(started, deadline - launcher_reserve);
    const auto app_deadline = std::max(started, kill_deadline - timings.sigkill_wait);

    if (plan.has_app) {
      const auto app_started = actions.now();
      const auto &label = plan.app_label;
      if (actions.wait_app(0ms)) {
        outcome.path = "exited_before_stop";
        BOOST_LOG(info) << "process: private app stop: the "sv << label << " had already exited"sv;
      } else {
        int asked = 0;
        if (!plan.immediate) {
          asked = actions.ask_app_to_close(std::min(timings.close_request, remaining_until(app_deadline, actions)));
        }
        outcome.windows_asked = asked;
        bool gone = false;
        if (asked > 0) {
          const auto wait = std::min(close_wait(plan.exit_timeout, timings), remaining_until(app_deadline, actions));
          BOOST_LOG(info) << "process: private app stop: asked "sv << asked << ' ' << label
                          << " window(s) to close; waiting up to "sv << wait.count() << " ms"sv;
          // What was asked, not everything of the session's: a helper that never exits on its own
          // would hold the wait to its end and make a game that closed read as one that did not.
          gone = actions.wait_asked ? actions.wait_asked(wait) : actions.wait_app(wait);
          if (gone) {
            outcome.path = "close_request";
            BOOST_LOG(info) << "process: private app stop: "sv << label << " closed after the close request waited_ms="sv
                            << since(app_started, actions).count();
            gone = stop_what_the_app_left(plan, actions, timings, app_deadline, kill_deadline);
          }
        }
        if (!gone) {
          const auto signalled = actions.sigterm_app();
          if (signalled == 0 && actions.wait_app(0ms)) {
            // It went on its own between the first look and the signal.
            outcome.path = asked > 0 ? "close_request" : "exited_before_stop";
            gone = true;
            BOOST_LOG(info) << "process: private app stop: the "sv << label << " had exited before SIGTERM"sv;
          } else {
            // With nothing asked first, SIGTERM is the app's only notice, so it gets its exit timeout
            // when that is longer, as the sweep always gave an emulator that saves on SIGTERM.
            const auto grace = plan.immediate ? timings.immediate_sigterm :
                               asked > 0      ? timings.sigterm_after_close :
                                                std::max(timings.sigterm, close_wait(plan.exit_timeout, timings));
            const auto wait = std::min(grace, remaining_until(app_deadline, actions));
            if (asked > 0) {
              BOOST_LOG(info) << "process: private app stop: "sv << label << " did not close; SIGTERM to "sv
                              << signalled << " process(es), waiting up to "sv << wait.count() << " ms"sv;
            } else {
              BOOST_LOG(info) << "process: private app stop: "sv << (plan.immediate ? "immediate stop"sv : "no window to ask"sv)
                              << "; SIGTERM to "sv << signalled << ' ' << label << " process(es), waiting up to "sv
                              << wait.count() << " ms"sv;
            }
            gone = actions.wait_app(wait);
            if (gone) {
              outcome.path = "sigterm";
              BOOST_LOG(info) << "process: private app stop: "sv << label << " exited after SIGTERM waited_ms="sv
                              << since(app_started, actions).count();
            } else {
              outcome.path = "sigkill";
              BOOST_LOG(warning) << "process: private app stop: "sv << label << " did not exit after SIGTERM; SIGKILL"sv;
              actions.sigkill_app();
              gone = actions.wait_app(std::min(timings.sigkill_wait, remaining_until(kill_deadline, actions)));
            }
          }
        }
        outcome.drained = gone;
      }
      outcome.waited = since(app_started, actions);
    }

    if (!plan.launchers.empty()) {
      outcome.launchers.resize(plan.launchers.size());
      std::vector<bool> alive(plan.launchers.size());
      bool any_alive = false;
      for (std::size_t i = 0; i < plan.launchers.size(); ++i) {
        outcome.launchers[i].app_id = plan.launchers[i];
        alive[i] = actions.launcher_alive(i);
        any_alive = any_alive || alive[i];
      }
      const auto launchers_started = actions.now();
      if (any_alive && !plan.immediate) {
        // The game is gone; let the launcher see it return and record it, as Heroic writes the
        // session's playtime once legendary returns.
        actions.settle_launchers(std::min(timings.launcher_settle, remaining_until(deadline, actions)));
      }
      bool killed = false;
      if (any_alive) {
        std::vector<bool> asked(plan.launchers.size());
        for (std::size_t i = 0; i < plan.launchers.size(); ++i) {
          if (alive[i] && actions.launcher_alive(i)) {
            asked[i] = true;
            BOOST_LOG(info) << "process: private app stop: SIGTERM to "sv << actions.sigterm_launcher(i)
                            << " process(es) of launcher "sv << plan.launchers[i];
          }
        }
        const auto quit_deadline = actions.now() +
                                   std::min(plan.immediate ? timings.immediate_sigterm : timings.launcher_quit, remaining_until(deadline, actions));
        for (std::size_t i = 0; i < plan.launchers.size(); ++i) {
          auto &launcher = outcome.launchers[i];
          if (!alive[i] || !asked[i]) {
            launcher.path = "exited";
            BOOST_LOG(info) << "process: private app stop: launcher "sv << plan.launchers[i] << " exited on its own"sv;
            continue;
          }
          if (actions.wait_launcher(i, remaining_until(quit_deadline, actions))) {
            launcher.path = "sigterm";
            launcher.waited = since(launchers_started, actions);
            BOOST_LOG(info) << "process: private app stop: launcher "sv << plan.launchers[i]
                            << " quit after SIGTERM waited_ms="sv << launcher.waited.count();
            continue;
          }
          launcher.path = "sigkill";
          BOOST_LOG(warning) << "process: private app stop: launcher "sv << plan.launchers[i]
                             << " did not quit after SIGTERM; SIGKILL to its sandbox init"sv;
          actions.sigkill_launcher_init(i);
          killed = true;
        }
        for (std::size_t i = 0; i < plan.launchers.size(); ++i) {
          if (outcome.launchers[i].path == "sigkill") {
            outcome.launchers[i].waited = since(launchers_started, actions);
          }
        }
      } else {
        for (std::size_t i = 0; i < plan.launchers.size(); ++i) {
          outcome.launchers[i].path = "exited";
          BOOST_LOG(info) << "process: private app stop: launcher "sv << plan.launchers[i] << " exited on its own"sv;
        }
      }
      // Every instance the session owns, helpers included, is gone before the compositor goes.
      const auto scopes_empty = actions.wait_scopes(std::min(timings.backstop_wait, remaining_until(deadline, actions)));
      if (!scopes_empty) {
        BOOST_LOG(warning) << "process: private app stop: processes of the session's Flatpak instances remain"sv
                           << (killed ? " after the backstop"sv : ""sv);
      }
      outcome.drained = outcome.drained && scopes_empty;
    }
    outcome.elapsed = since(started, actions);
    return outcome;
  }

  session_processes_t classify(
    const std::vector<process_facts_t> &facts,
    const fsi::process_t &supervisor,
    std::string_view token,
    const fsi::instances_t &instances,
    uid_t uid
  ) {
    session_processes_t result;
    std::map<pid_t, const process_facts_t *> by_pid;
    for (const auto &fact : facts) {
      by_pid[fact.process.pid] = &fact;
    }

    // Descent from the supervisor, through parents no younger than their children, memoized.
    std::map<pid_t, bool> descends;
    const auto descends_from_supervisor = [&](const process_facts_t &start) {
      std::vector<pid_t> walked;
      const process_facts_t *current = &start;
      bool answer = false;
      for (std::size_t depth = 0; depth < 4096; ++depth) {
        if (const auto known = descends.find(current->process.pid); known != descends.end()) {
          answer = known->second;
          break;
        }
        walked.push_back(current->process.pid);
        if (current->parent == supervisor.pid) {
          const auto parent = by_pid.find(current->parent);
          answer = parent != by_pid.end() && parent->second->process.start_time == supervisor.start_time &&
                   supervisor.start_time <= current->process.start_time;
          break;
        }
        const auto parent = by_pid.find(current->parent);
        if (current->parent <= 1 || parent == by_pid.end() ||
            parent->second->process.start_time > current->process.start_time) {
          break;
        }
        current = parent->second;
      }
      for (const auto pid : walked) {
        descends[pid] = answer;
      }
      return answer;
    };

    std::set<pid_t> compositor;
    if (const auto found = by_pid.find(supervisor.pid);
        found != by_pid.end() && found->second->process.start_time == supervisor.start_time) {
      compositor.insert(supervisor.pid);
      result.compositor.push_back(found->second->process);
    }
    // What the compositor runs for its own sake, which carries the token too and never exits on its
    // own: labwc's startup client when the app is Polaris's own child, which cage_display_router makes
    // `exec sleep infinity`, and the swaybg the generated autostart starts. Taken for the app, they
    // held every close request to its full wait. They go with the compositor.
    const auto runs_for_the_compositor = [](const process_facts_t &fact) {
      return (fact.comm == "sleep" && fact.cmdline == "sleep\0infinity\0"sv) || fact.comm == "swaybg";
    };
    for (const auto &fact : facts) {
      if ((fact.comm == "labwc" || fact.comm == "Xwayland" || runs_for_the_compositor(fact)) && descends_from_supervisor(fact)) {
        compositor.insert(fact.process.pid);
        result.compositor.push_back(fact.process);
      }
    }

    std::vector<fsi::instance_t> records = instances.live;
    records.insert(records.end(), instances.unresolved.begin(), instances.unresolved.end());

    // The chain from the compositor down to each Flatpak sandbox's `flatpak run`: an owned
    // launcher's, and one still starting that no record confirms yet.
    std::set<pid_t> chain;
    for (const auto &record : records) {
      auto found = by_pid.find(record.bwrap.pid);
      std::vector<const process_facts_t *> ancestors;
      bool reaches_compositor = false;
      while (found != by_pid.end()) {
        const auto parent = found->second->parent;
        if (parent == supervisor.pid || compositor.contains(parent)) {
          reaches_compositor = true;
          break;
        }
        const auto next = by_pid.find(parent);
        if (parent <= 1 || next == by_pid.end() || next->second->process.start_time > found->second->process.start_time) {
          break;
        }
        ancestors.push_back(next->second);
        found = next;
      }
      // A sandbox the desktop started has no chain here: its parents are the desktop's.
      if (!reaches_compositor) {
        continue;
      }
      for (const auto *ancestor : ancestors) {
        if (chain.insert(ancestor->process.pid).second) {
          result.launch_chain.push_back(ancestor->process);
        }
      }
    }

    std::set<std::string> instance_cgroups;
    for (const auto &instance : instances.live) {
      instance_cgroups.insert(instance.cgroup);
    }

    for (const auto &fact : facts) {
      const auto pid = fact.process.pid;
      // The compositor's names keep a process out even when its lineage could not be proven, since
      // it carries the token too: signalling it is stopping the compositor.
      if (compositor.contains(pid) || chain.contains(pid) || pid == supervisor.pid || fact.comm == "labwc" ||
          fact.comm == "Xwayland") {
        continue;
      }
      const bool carries_token = fact.environ && fsi::environ_carries_token(*fact.environ, token);
      if (!carries_token && !descends_from_supervisor(fact)) {
        continue;
      }
      const bool other_user = !fact.uid || *fact.uid != uid;
      const bool in_sandbox_namespace = fact.nspid.size() > 1;
      const bool in_flatpak_scope = instance_cgroups.contains(fact.cgroup) ||
                                    cgroup_basename(fact.cgroup).starts_with("app-flatpak-");
      const bool flatpak_bwrap = fsi::is_instance_bwrap(fact.process, records);
      const bool anonymous_bwrap = fact.comm == "bwrap" && fact.environ && fact.environ->empty();
      if (other_user || in_sandbox_namespace || in_flatpak_scope || flatpak_bwrap || anonymous_bwrap) {
        continue;
      }
      result.session.push_back(fact.process);
    }
    return result;
  }

  std::vector<fsi::member_t> settle_processes(const fsi::instance_t &launcher, const std::vector<fsi::member_t> &members) {
    const auto apps = fsi::app_processes(launcher, members);
    std::map<pid_t, const fsi::member_t *> by_pid;
    for (const auto &member : apps) {
      by_pid[member.process.pid] = &member;
    }
    std::set<pid_t> ancestors_of_foreign;
    for (const auto &member : apps) {
      if (member.carries_token) {
        continue;
      }
      // Walk up from a process whose environ is not the session's, such as Electron's main
      // process, and mark every app process above it: those live as long as it does.
      auto parent = by_pid.find(member.parent);
      std::set<pid_t> seen;
      while (parent != by_pid.end() && seen.insert(parent->first).second) {
        ancestors_of_foreign.insert(parent->first);
        parent = by_pid.find(parent->second->parent);
      }
    }
    std::vector<fsi::member_t> settle;
    for (const auto &member : apps) {
      if (member.carries_token && !ancestors_of_foreign.contains(member.process.pid)) {
        settle.push_back(member);
      }
    }
    return settle;
  }

  private_session_attach::window_owner_test_t window_owner_test(window_owners_t owners, std::shared_ptr<asked_owners_t> asked) {
    auto shared = std::make_shared<window_owners_t>(std::move(owners));
    // A window taken by a host pid is asked, and the close wait waits for that process. One taken by
    // a pid inside a game's sandbox is the game's, which it waits for anyway.
    const auto note = [asked](pid_t pid) {
      if (asked) {
        std::lock_guard lock(asked->mutex);
        asked->host.insert(pid);
      }
    };
    return {
      [shared, note](std::uint32_t pid) {
        const auto value = static_cast<pid_t>(pid);
        const bool owns = shared->host.contains(value);
        if (owns) {
          note(value);
        }
        return owns;
      },
      [shared, note](std::uint32_t pid) {
        const auto value = static_cast<pid_t>(pid);
        if (shared->sandbox_other.contains(value)) {
          return false;
        }
        if (shared->host.contains(value)) {
          note(value);
          return true;
        }
        return shared->sandbox_app.contains(value);
      },
    };
  }

}  // namespace private_app_stop

#endif
