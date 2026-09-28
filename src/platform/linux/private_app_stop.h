/**
 * @file src/platform/linux/private_app_stop.h
 * @brief Stop a private session's apps the way a player would, in order, while its compositor is
 *        still up.
 *
 * Ending a private stream used to stop the private compositor with the app still on it, or
 * together with it: the exact cleanup sent SIGTERM to every process carrying the session token at
 * once, and labwc, its supervisor and Xwayland carry it too. A game lost its display in the same
 * instant it was asked to quit, and a Flatpak launcher, whose processes the token cannot all see,
 * was not asked at all. This is the order that replaces it: the game is asked to close, then
 * signalled, then the launcher that started it is quit, and only then does the compositor stop.
 * The steps are here, with every action injected, so the order and the bounds are testable without
 * a session; process.cpp finds the processes and supplies the actions.
 */
#pragma once

#ifdef __linux__

  #include "flatpak_session_instances.h"
  #include "private_session_attach.h"

  #include <chrono>
  #include <cstdint>
  #include <functional>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <set>
  #include <string>
  #include <string_view>
  #include <sys/types.h>
  #include <vector>

namespace private_app_stop {
  using namespace std::chrono_literals;

  /// The waits of the phase. Tests shrink them; a session uses the defaults.
  struct timings_t {
    /// Everything the phase may wait for in all. No longer than the generic path could already
    /// hold the lifecycle lock: an exit timeout of at most 30 s, a second pass and the kills.
    std::chrono::milliseconds budget = 30s;
    std::chrono::milliseconds close_request = 1s;  ///< the X exchange that asks windows to close
    std::chrono::milliseconds close_wait_floor = 10s;  ///< the least a game is given after being asked
    std::chrono::milliseconds exit_timeout_ceiling = 30s;  ///< the most an app's exit timeout adds
    /// SIGTERM's grace when nothing could be asked first, or the app's exit timeout when that is
    /// longer, up to the ceiling.
    std::chrono::milliseconds sigterm = 10s;
    std::chrono::milliseconds sigterm_after_close = 5s;  ///< SIGTERM's grace after an unanswered close
    std::chrono::milliseconds immediate_sigterm = 2s;  ///< SIGTERM's grace on an immediate stop
    std::chrono::milliseconds sigkill_wait = 3s;  ///< a 6 GB Proton process takes a while to reap
    /// For what an app that closed when asked leaves of the session's own processes to exit on
    /// their own, as a crash handler does just after its game, before they get SIGTERM.
    std::chrono::milliseconds left_settle = 1s;
    std::chrono::milliseconds launcher_settle = 3s;  ///< for the launcher to see its game return
    std::chrono::milliseconds launcher_quit = 5s;  ///< for a launcher to quit after SIGTERM
    std::chrono::milliseconds backstop_wait = 3s;  ///< for the scopes to empty after the backstop
  };

  /// How long a game asked to close is given: its app's exit timeout, at least the floor and at
  /// most the ceiling, so raising an app's exit timeout gives a slow saver longer.
  std::chrono::milliseconds close_wait(std::chrono::seconds exit_timeout, const timings_t &timings = {});

  /// What the phase has to stop, as the steps need to know it.
  struct plan_t {
    bool immediate = false;  ///< skip the close request and the launcher's settle, shorten SIGTERM's grace
    std::chrono::seconds exit_timeout {};  ///< the app's own exit timeout
    bool has_app = false;  ///< a game instance or processes of the session's own to stop first
    std::string app_label = "game";  ///< what the log calls them
    std::vector<std::string> launchers;  ///< app ids of the launchers to quit once the app is gone
  };

  /// The actions the steps take. process.cpp gives the real ones; the tests record them.
  struct actions_t {
    std::function<std::chrono::steady_clock::time_point()> now = std::chrono::steady_clock::now;
    /// Ask the app's windows to close, within the timeout. Returns how many were asked.
    std::function<int(std::chrono::milliseconds timeout)> ask_app_to_close;
    /// Wait up to the timeout for every process of the app to exit. True when none is left.
    std::function<bool(std::chrono::milliseconds timeout)> wait_app;
    /// Wait up to the timeout for what the close request asked to exit: the game's processes, and
    /// those of the session's own whose windows it asked. Unset, wait_app stands in.
    std::function<bool(std::chrono::milliseconds timeout)> wait_asked;
    /// SIGTERM to the app's processes. Never a bwrap. Returns how many were signalled.
    std::function<int()> sigterm_app;
    /// SIGKILL to the app: a game sandbox's init when the game has a pid namespace of its own, else
    /// its processes, and the session's own processes, all by pidfd.
    std::function<void()> sigkill_app;
    /// Whether the launcher's sandbox is still running.
    std::function<bool(std::size_t launcher)> launcher_alive;
    /// Wait up to the timeout for what the launcher ran for the game to return, such as legendary.
    std::function<bool(std::chrono::milliseconds timeout)> settle_launchers;
    /// SIGTERM to the launcher's processes, never a bwrap. Returns how many were signalled.
    std::function<int(std::size_t launcher)> sigterm_launcher;
    /// Wait up to the timeout for the launcher's sandbox init to exit, which it does once its last
    /// child has.
    std::function<bool(std::size_t launcher, std::chrono::milliseconds timeout)> wait_launcher;
    /// The backstop: SIGKILL to the launcher's sandbox init. From the host's pid namespace it is
    /// always delivered, and the kernel then ends every process in that namespace and those nested
    /// in it: the launcher, a game sharing its namespace and its helpers, and nothing else.
    std::function<void(std::size_t launcher)> sigkill_launcher_init;
    /// Wait up to the timeout for every process of every instance the session owns to be gone.
    std::function<bool(std::chrono::milliseconds timeout)> wait_scopes;
  };

  struct launcher_outcome_t {
    std::string app_id;
    std::string path;  ///< exited, sigterm or sigkill
    std::chrono::milliseconds waited {};
  };

  struct outcome_t {
    /// exited_before_stop, close_request, sigterm or sigkill; empty when there was no app to stop.
    std::string path;
    int windows_asked = 0;
    std::chrono::milliseconds waited {};  ///< from the first step to the app gone
    std::vector<launcher_outcome_t> launchers;
    bool drained = true;  ///< everything the phase set out to stop is gone
    std::chrono::milliseconds elapsed {};  ///< the whole phase
  };

  /**
   * @brief Run the steps: ask, signal the game, quit the launcher, backstop.
   *
   * Every wait is bounded by one deadline, the budget from the phase's start, so the phase never
   * runs past it, whatever each step's own timeout says. When there are launchers to quit, the
   * app's steps leave them their share of it. Nothing here stops the compositor: it stops after
   * this returns, which is the point of the order.
   */
  outcome_t run(const plan_t &plan, const actions_t &actions, const timings_t &timings = {});

  /// What the host read of one process for the classification below.
  struct process_facts_t {
    flatpak_session_instances::process_t process;
    pid_t parent = 0;
    std::string comm;
    std::string cmdline;  ///< /proc/<pid>/cmdline, read only where the classification needs it
    std::vector<pid_t> nspid;  ///< NSpid, the host's pid first
    std::string cgroup;
    std::optional<std::string> environ;  ///< nullopt when it could not be read
    std::optional<uid_t> uid;  ///< real uid
  };

  /// The session's processes that are in no Flatpak instance, sorted by what may be done to them.
  struct session_processes_t {
    /// The compositor: the labwc supervisor, labwc, Xwayland, and what runs in it for its own sake,
    /// labwc's `sleep infinity` startup client when the app is Polaris's own child and the swaybg
    /// the generated autostart starts. Left alone until the phase ends.
    std::vector<flatpak_session_instances::process_t> compositor;
    /// T: carrying the session token, or descended from the supervisor, and in no Flatpak sandbox.
    /// These are signalled with the game.
    std::vector<flatpak_session_instances::process_t> session;
    /// The shells and wrappers between the compositor and the `flatpak run` of a Flatpak sandbox
    /// the session started. Signalling one of them would bring the sandbox down unordered, so they
    /// are left to exit when their child does.
    std::vector<flatpak_session_instances::process_t> launch_chain;
  };

  /**
   * @brief Sort the session's processes outside any Flatpak instance.
   *
   * A process is the session's when it carries the token in a readable environ or descends from
   * the supervisor through parents no younger than their children. Of those, the compositor and
   * what it runs for its own sake, the launch chains of the session's Flatpak sandboxes, anything
   * in a Flatpak scope or in a pid namespace below the host's, any bwrap a Flatpak record names, any
   * zero-byte-environ bwrap, and any process of another user are never in T.
   */
  session_processes_t classify(
    const std::vector<process_facts_t> &facts,
    const flatpak_session_instances::process_t &supervisor,
    std::string_view token,
    const flatpak_session_instances::instances_t &instances,
    uid_t uid
  );

  /// The launcher's processes it ran for the game, which the settle waits for: its app processes
  /// that carry the token and are not above one that does not, as heroic-run's shell is above
  /// Electron's main process, whose environ Electron overwrote and which lives as long as the
  /// launcher does.
  std::vector<flatpak_session_instances::member_t> settle_processes(
    const flatpak_session_instances::instance_t &launcher,
    const std::vector<flatpak_session_instances::member_t> &members
  );

  /// Which processes' windows the phase asks to close.
  struct window_owners_t {
    std::set<pid_t> host;  ///< host pids of the app's processes
    std::set<pid_t> sandbox_app;  ///< the app's pids inside a sandbox's namespace
    std::set<pid_t> sandbox_other;  ///< pids a launcher or helper has inside a sandbox's namespace
  };

  /// The host pids whose windows the close request asked, as the window owner test chose them. The
  /// exchange runs on a thread of its own, so the set is guarded.
  struct asked_owners_t {
    std::mutex mutex;
    std::set<pid_t> host;
  };

  /**
   * @brief The window owner test for close_targets.
   *
   * A client pid X-Resource reports is a host pid and is decided by the host set alone. Without
   * X-Resource a window's _NET_WM_PID may be a host pid or one inside a sandbox, and a value a
   * launcher or helper also has is never taken: asking Heroic's own window to close ends Heroic
   * before its game, the opposite of the order. Each host pid it takes goes into @p asked, when
   * given, so the close wait can wait for what was asked rather than for everything.
   */
  private_session_attach::window_owner_test_t window_owner_test(window_owners_t owners, std::shared_ptr<asked_owners_t> asked = {});

}  // namespace private_app_stop

#endif
