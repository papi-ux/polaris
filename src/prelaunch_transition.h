/**
 * @file src/prelaunch_transition.h
 * @brief Leaving Steam Game Mode before a private launch (#204): coordinator, receipt and probes.
 *
 * An app may opt in to leaving Game Mode before its own private launch. Every admitted launch entry
 * (GameStream /launch, the console's launch and Browser Stream) asks this coordinator after it has
 * authorised the request and reserved the launch, and before the launch decides its display mode.
 *
 * The coordinator never runs a host command itself. It drives a session-switch adapter over a host
 * that reports typed session records. On Linux builds with GIO the production adapter is the
 * SteamOS one (src/platform/linux/steamos_adapter.h): it acts only when, on every launch, the fixed
 * trusted command, the audited steamos-manager package and this account's own manager prove it
 * supported, and the host's one SDDM-autologin graphical session is bound. Elsewhere the adapter is
 * unavailable and an enabled app on a host in Game Mode is refused before any effect. Tests inject
 * an adapter, a host and a clock that change nothing on the machine.
 *
 * steamos-manager's switch takes no session argument, so there is no compare-and-swap between the
 * last check here and its StopUnit: a session that changes in that window is detected afterwards
 * (the bound target generation is not the one that retired) and is never adopted or rolled back.
 */
#pragma once

#include "steamos_session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace prelaunch {
  /// The per-app key. Absent, or anything but JSON true, is off.
  inline constexpr std::string_view k_app_key = "leave-game-mode-before-launch";

  /// Every wait is bounded, and the bounds add up: a launch spends at most forward + rollback (85 s)
  /// here, inside the /launch reply's own 120 s. One run of the command is bounded by `child` within
  /// whichever of those it belongs to. A session-end restore runs on its own worker, outside every
  /// lifecycle lock, within `restore`.
  struct budgets_t {
    std::chrono::milliseconds forward {std::chrono::seconds {60}};
    std::chrono::milliseconds rollback {std::chrono::seconds {25}};
    std::chrono::milliseconds restore {std::chrono::seconds {45}};
    std::chrono::milliseconds child {std::chrono::seconds {15}};
    /// A readiness answer for an app list: no bus activation, never more than this.
    std::chrono::milliseconds readiness {std::chrono::seconds {1}};
  };
  inline constexpr std::chrono::milliseconds k_poll_period {50};

  using steamos::mode_e;
  using steamos::session_identity_t;
  using steamos::verdict_t;

  enum class entry_e { rtsp, console, browser };
  enum class state_e {
    idle,
    not_needed,  ///< off, not in Game Mode, or a path that binds an existing generation
    refused,  ///< refused before any effect
    switching,
    awaiting_old_end,
    awaiting_desktop,
    awaiting_capture,
    ready,
    failed,
    cancelled,
  };
  enum class restore_e {
    none,  ///< nothing of this generation's to restore
    owned,  ///< this generation changed the session and still owns that change
    restored,
    rolled_back,
    refused,  ///< ownership could not be proven, or the end was not a purposeful one; nothing done
    failed,
  };
  enum class direction_e { to_desktop, to_game_mode };
  enum class step_e { running, succeeded, failed };

  std::string_view name(entry_e value);
  std::string_view name(state_e value);
  std::string_view name(restore_e value);

  /// Whether `uid` has a Steam title open: a `reaper SteamLaunch AppId=` process of its own.
  bool steam_title_running(const std::filesystem::path &proc_root, std::uint32_t uid);
  /// Whether a /proc/self/cgroup text puts the process in polaris.service under the user manager,
  /// outside any login session scope, so ending a graphical session does not end it.
  bool cgroup_in_independent_service(std::string_view cgroup);

  /// One wait's end: a deadline and the cancellations that end it sooner.
  struct wait_t {
    std::chrono::steady_clock::time_point deadline;
    std::function<bool()> cancelled;
  };

  /// One fresh read of the host's sessions. Nothing here is a kept answer.
  struct observation_t {
    /// The read itself failed (unreachable, malformed, timed out, cancelled): nothing is known.
    verdict_t read;
    /// Read, but no one session of this account could be bound; why.
    verdict_t bind;
    /// The account's one graphical session, bound with its generation. Set only when both pass.
    std::optional<session_identity_t> session;
    /// With a `since` identity: that session and its target generation have both ended.
    bool retired = false;
    /// The account's Game Mode unit, from the same records. `unknown` unless a read said otherwise.
    steamos::game_mode_e game_mode = steamos::game_mode_e::unknown;
    /// Polaris's own unit survives the graphical session (asked for with `prove_service`).
    verdict_t service;
  };

  /// Host facts the coordinator reads. Production reads the host; tests inject.
  class host_t {
  public:
    virtual ~host_t() = default;
    /// Reads the host's sessions now, within `wait`: every decision, positive or not, comes from
    /// here. With `since`, also whether it retired; with `prove_service`, whether Polaris survives.
    virtual observation_t observe(const session_identity_t *since, const wait_t &wait, bool prove_service = false) = 0;
    virtual bool local_title_running() = 0;
    /// Polaris runs as an independent user service AND starts at boot; boot files alone are not enough.
    virtual bool service_independent(std::string &reason) = 0;
  };

  /// What leaves and re-enters Game Mode.
  class adapter_t {
  public:
    virtual ~adapter_t() = default;
    /// False for a build without a session switch: it is never started.
    virtual bool implemented() const = 0;
    /// The trusted command is installed at the adapter's one fixed path. Says nothing about support.
    virtual bool command_present() const = 0;
    /// Everything the command needs, proved now: called before every start, never cached.
    virtual verdict_t support(const wait_t &wait) = 0;
    /// The same answer for display only; it may be kept briefly while nothing it rests on changed.
    virtual verdict_t readiness_support(const wait_t &wait) {
      return support(wait);
    }
    virtual bool start(direction_e direction) = 0;
    /// running; succeeded when the command exited 0 (its request was sent, nothing more); failed.
    virtual step_e poll() = 0;
    /// Stops and reaps the command. Whatever it had sent before stands: the outcome is unknown.
    virtual void abort() = 0;
  };

  class clock_t {
  public:
    virtual ~clock_t() = default;
    virtual std::chrono::steady_clock::time_point now() = 0;
    virtual void sleep_for(std::chrono::milliseconds duration) = 0;
  };

  /// What else is on the host. Any of it refuses the transition.
  struct occupancy_t {
    bool app_running = false;
    bool streams_active = false;
    bool launch_pending = false;
    bool profile_service = false;
    bool browser_capture = false;
  };

  struct request_t {
    entry_e entry = entry_e::rtsp;
    std::string client_uuid;
    std::uint64_t lifecycle_generation = 0;
    bool policy_enabled = false;
    /// Paths that bind an existing generation or are not the host's own session: never switch.
    bool resume = false;
    bool watch_only = false;
    bool input_only = false;
    bool profile_or_space = false;
    /// The route this launch would use after leaving Game Mode is a private one. Leaving Game Mode
    /// for a mirror would change the host for nothing, so that is refused instead.
    bool private_route = true;
    occupancy_t occupancy;
    /// The entry's own cancellation, checked with the coordinator's on every wait.
    std::function<bool()> cancelled;
    /// Whether the route this launch will use is ready after the switch; reason on false.
    std::function<bool(std::string &)> capture_ready;
    /// A fresh look at the host's other sessions, for a rollback this run itself performs; reason on true.
    std::function<bool(std::string &)> others_now;
  };

  struct outcome_t {
    int status = 0;  ///< 0 lets the launch go on
    std::string code;
    std::string message;
    std::string action;
    state_e state = state_e::idle;
    std::uint64_t transition_generation = 0;
    bool changed_session = false;

    bool proceed() const {
      return status == 0;
    }
  };

  struct receipt_t {
    std::uint64_t transition_generation = 0;
    std::uint64_t lifecycle_generation = 0;
    entry_e entry = entry_e::rtsp;
    std::string client_uuid;
    state_e state = state_e::idle;
    restore_e restore = restore_e::none;
    std::string code;
    std::string restore_reason;
    std::optional<session_identity_t> pre;
    std::optional<session_identity_t> post;
    bool changed_session = false;
    bool committed = false;
    /// The command was stopped after it started, or its result could not be bound: what it sent, if
    /// anything, stands, and nothing of it is adopted or undone.
    bool outcome_unknown = false;
    /// How this launch's own cancellation reaches the wait: "launch" when the entry passed one,
    /// "unsupported" when it has none (console and Browser Stream have no cancel of their own).
    std::string cancellation_transport;
  };

  /// What a client or the console may show: the setting, and whether this build can act on it.
  struct readiness_t {
    bool enabled = false;
    bool adapter_implemented = false;
    bool command_present = false;
    bool ready = false;
    std::string code;  ///< the support refusal's code when not ready
    std::string reason;
  };

  /// Closes admission of every launch while a restore is owned. `acquire` must fail when a launch
  /// is already admitted, and runs in the same step that claims the restore; `release` reopens it.
  struct admission_fence_t {
    std::function<bool()> acquire;
    std::function<void()> release;
  };

  class coordinator_t {
  public:
    /// `follow_process_stop`: also stopped by prelaunch::request_stop() (every coordinator the
    /// process-wide registry makes, whenever it is made).
    coordinator_t(host_t &host, adapter_t &adapter, clock_t &clock, budgets_t budgets = {}, bool follow_process_stop = false);
    /// Shuts down: cancels and joins any restore still running.
    ~coordinator_t();
    coordinator_t(const coordinator_t &) = delete;
    coordinator_t &operator=(const coordinator_t &) = delete;

    /**
     * @brief Decide, and when admitted run, the transition for one reserved launch.
     *
     * Holds no lock while it waits. A refusal names its code before any effect; a failure or a
     * cancellation after the switch began rolls back only what this generation changed.
     */
    outcome_t run(const request_t &request);
    /// The launch reached its client: the change now belongs to that session until it ends.
    void commit(std::uint64_t transition_generation);
    /// A fresh check for other sessions on the host, taken right before a restore; reason on true.
    using others_t = std::function<bool(std::string &)>;
    /// The launch failed or was cancelled after a ready transition and before commit.
    restore_e rollback(std::uint64_t transition_generation, const others_t &others);
    /**
     * @brief The committed session of `transition_generation` ended.
     *
     * Bookkeeping only, so a caller may hold its lifecycle lock: it never waits for or joins a
     * thread. The restore is queued to this coordinator's one owned serial worker, which proves
     * everything again and acts outside every lock, within the restore budget. `fence` is acquired
     * in the same step: a launch already admitted refuses the restore, and no launch is admitted
     * until the worker has recorded the outcome and released it. Returns `owned` when a restore was
     * queued, `refused` for an end that may not restore, `none` for anything else. Restores only on
     * a purposeful end of that same generation.
     */
    restore_e on_session_end(std::uint64_t transition_generation, bool ended_on_request, bool daemon_shutdown, const others_t &others,
                             const admission_fence_t &fence = {});
    /// Waits for a handed-over restore to settle. Never call it holding a lock the restore may need.
    void wait_for_restore();
    /// Polaris is stopping: refuses new transitions and cancels the running one and any restore.
    /// Atomics only, no lock and no wait, so a signal handler may call it.
    void request_stop();
    /// request_stop(), then ends and joins the serial restore worker. Call it outside every lock.
    void shutdown();
    /// A session-end restore is still in flight: no launch of any app may be admitted until it settles.
    bool restore_pending() const;
    /// /cancel from the requesting client while its transition runs. Another client changes nothing.
    bool cancel_for_client(std::string_view client_uuid);
    receipt_t receipt() const;
    /// Support answered within the readiness budget; the session itself is checked at launch.
    readiness_t readiness(bool enabled) const;

  private:
    struct active_t {
      std::uint64_t generation = 0;
      std::string client_uuid;
      std::shared_ptr<std::atomic<bool>> cancelled;
    };

    outcome_t finish(const request_t &request, std::uint64_t generation, outcome_t outcome);
    /// Proves `post` is still this generation's desktop, then returns to Game Mode and proves a new
    /// Game Mode session. `outcome_unknown` is set when the command was stopped after it started.
    restore_e restore_owned(const session_identity_t &post, const others_t &others, const wait_t &wait, std::string &reason, bool &outcome_unknown);
    bool stopping() const;

    host_t &host_;
    adapter_t &adapter_;
    clock_t &clock_;
    budgets_t budgets_;

    mutable std::mutex mutex_;
    std::uint64_t next_generation_ = 0;
    std::optional<active_t> active_;
    struct job_t {
      std::uint64_t generation = 0;
      session_identity_t post;
      others_t others;
      std::function<void()> release_fence;
    };

    void worker_loop();

    bool follow_process_stop_ = false;
    bool restoring_ = false;
    std::atomic<bool> stopping_ {false};
    /// The one owned serial restore worker, started by the first restore and joined by shutdown().
    std::thread worker_;
    std::optional<job_t> job_;
    bool worker_exit_ = false;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    receipt_t receipt_;
  };

  /// The adapter of a build without a session switch (not Linux, or built without GIO): never started.
  class unavailable_adapter_t final: public adapter_t {
  public:
    explicit unavailable_adapter_t(std::filesystem::path command = std::string {steamos::k_steamosctl});
    bool implemented() const override;
    bool command_present() const override;
    verdict_t support(const wait_t &wait) override;
    bool start(direction_e direction) override;
    step_e poll() override;
    void abort() override;

  private:
    std::filesystem::path command_;
  };

  /// The process-wide coordinator over the production host, adapter and clock, or the test ones.
  coordinator_t &coordinator();
  /// Replaces the dependencies until called again with nulls. Tests only.
  void set_dependencies_for_tests(host_t *host, adapter_t *adapter, clock_t *clock, budgets_t budgets = {});
  /// Polaris is stopping (signal-safe): latches a process-wide flag every registry coordinator reads,
  /// including one made after this call.
  void request_stop();
  /// Clears that latch. Tests only.
  void reset_process_stop_for_tests();
  /// Polaris is stopping: shuts the production coordinator down (see coordinator_t::shutdown()).
  void shutdown();

  /// Records launch stages in order while a test watches; production never sets a sink.
  void note_stage(std::string_view stage);
  void set_stage_sink_for_tests(std::function<void(std::string_view)> sink);

  /**
   * @brief One entry's hold on its transition: rolls back unless the launch committed.
   *
   * Every entry keeps one for the length of its launch so an early return cannot strand a change.
   */
  class launch_scope_t {
  public:
    launch_scope_t() = default;
    explicit launch_scope_t(const outcome_t &outcome);
    launch_scope_t(const launch_scope_t &) = delete;
    launch_scope_t &operator=(const launch_scope_t &) = delete;
    ~launch_scope_t();
    /// Commits the change to the launched session; returns the generation the session must name at
    /// its end, or 0 when this launch changed nothing.
    std::uint64_t commit();
    /// The generation this launch owns, or 0, for binding to its session before commit.
    std::uint64_t owned_generation() const;
    void set_others(coordinator_t::others_t others);

  private:
    std::uint64_t generation_ = 0;
    bool owned_ = false;
    bool committed_ = false;
    coordinator_t::others_t others_;
  };
}  // namespace prelaunch
