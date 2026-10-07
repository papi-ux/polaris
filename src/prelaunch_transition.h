/**
 * @file src/prelaunch_transition.h
 * @brief Leaving Steam Game Mode before a private launch (#204): coordinator, receipt and probes.
 *
 * An app may opt in to leaving Game Mode before its own private launch. Every admitted launch entry
 * (GameStream /launch, the console's launch and Browser Stream) asks this coordinator after it has
 * authorised the request and reserved the launch, and before the launch decides its display mode.
 *
 * The coordinator never runs a host command itself. It drives a session-switch adapter. The
 * production adapter is not implemented in this build: it reports itself unavailable and is never
 * started, so an enabled app on a host in Game Mode is refused before any mode-changing effect.
 * Tests inject an adapter, a host and a clock that change nothing on the machine.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prelaunch {
  /// The per-app key. Absent, or anything but JSON true, is off.
  inline constexpr std::string_view k_app_key = "leave-game-mode-before-launch";
  /// The longest a transition or a restore may take. The production adapter is unavailable, so no
  /// client timeout has been reconciled with it; this bounds the injected test engine only.
  inline constexpr std::chrono::milliseconds k_max_deadline {std::chrono::seconds {45}};
  inline constexpr std::chrono::milliseconds k_poll_period {50};

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

  /// A graphical session, by its leader process: pid, its start time and its logind scope. A process
  /// lookup alone is not proof of the current graphical session; it is only usable when unambiguous.
  struct session_identity_t {
    std::int64_t leader_pid = 0;
    std::uint64_t leader_start_ticks = 0;
    std::string logind_scope;  ///< e.g. "session-3.scope"; empty when the leader is in none

    bool valid() const {
      return leader_pid > 0 && leader_start_ticks > 0 && !logind_scope.empty();
    }

    friend bool operator==(const session_identity_t &, const session_identity_t &) = default;
  };

  /// The identity of `pid` under `proc_root`, from its stat start time and cgroup scope. Read twice
  /// around the cgroup, so a pid reused meanwhile is refused rather than bound to another process.
  std::optional<session_identity_t> process_identity(const std::filesystem::path &proc_root, std::int64_t pid);
  /// The one process of `uid` under `proc_root` whose argv[0] basename is one of `names`, in a logind
  /// session scope. None, more than one, or a replaced pid is nullopt: ambiguity is never resolved by
  /// picking one.
  std::optional<session_identity_t> find_session_leader(
    const std::filesystem::path &proc_root,
    std::uint32_t uid,
    const std::vector<std::string_view> &names
  );
  /// Whether `uid` has a Steam title open: a `reaper SteamLaunch AppId=` process of its own.
  bool steam_title_running(const std::filesystem::path &proc_root, std::uint32_t uid);
  /// Whether a /proc/self/cgroup text puts the process in polaris.service under the user manager,
  /// outside any login session scope, so ending a graphical session does not end it.
  bool cgroup_in_independent_service(std::string_view cgroup);

  /// Host facts the coordinator reads. Production reads the host; tests inject.
  class host_t {
  public:
    virtual ~host_t() = default;
    /// A Game Mode scan taken now, never a kept answer.
    virtual bool game_mode_live_fresh() = 0;
    virtual std::optional<session_identity_t> game_mode_session() = 0;
    virtual std::optional<session_identity_t> desktop_session() = 0;
    virtual bool local_title_running() = 0;
    /// Polaris runs as an independent user service AND starts at boot; boot files alone are not enough.
    virtual bool service_independent(std::string &reason) = 0;
  };

  /// What leaves and re-enters Game Mode.
  class adapter_t {
  public:
    virtual ~adapter_t() = default;
    /// False for the production adapter in this build: it must never be started.
    virtual bool implemented() const = 0;
    virtual std::string unavailable_reason() const = 0;
    /// An executable file is present at the adapter's one fixed path. Presence only: it says nothing
    /// about the command's supported semantics, and nothing runs. Required before any start().
    virtual bool command_present() const = 0;
    virtual bool start(direction_e direction) = 0;
    virtual step_e poll() = 0;
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
  };

  /// What a client or the console may show: the setting, and whether this build can act on it.
  struct readiness_t {
    bool enabled = false;
    bool adapter_implemented = false;
    bool command_present = false;
    bool ready = false;
    std::string reason;
  };

  class coordinator_t {
  public:
    coordinator_t(host_t &host, adapter_t &adapter, clock_t &clock, std::chrono::milliseconds deadline = k_max_deadline);

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
    /// The committed session of `transition_generation` ended. Restores only on a purposeful end of
    /// that same generation; a teardown naming another generation changes nothing.
    restore_e on_session_end(std::uint64_t transition_generation, bool ended_on_request, bool daemon_shutdown, const others_t &others);
    /// /cancel from the requesting client while its transition runs. Another client changes nothing.
    bool cancel_for_client(std::string_view client_uuid);
    receipt_t receipt() const;
    readiness_t readiness(bool enabled) const;

  private:
    struct active_t {
      std::uint64_t generation = 0;
      std::string client_uuid;
      std::shared_ptr<std::atomic<bool>> cancelled;
    };

    outcome_t finish(const request_t &request, std::uint64_t generation, outcome_t outcome);
    restore_e restore_owned(const session_identity_t &post, const others_t &others, std::string &reason);

    host_t &host_;
    adapter_t &adapter_;
    clock_t &clock_;
    std::chrono::milliseconds deadline_;

    mutable std::mutex mutex_;
    std::uint64_t next_generation_ = 0;
    std::optional<active_t> active_;
    bool restoring_ = false;
    receipt_t receipt_;
  };

  /// The production adapter of this build: unavailable, never started.
  class unavailable_adapter_t final: public adapter_t {
  public:
    explicit unavailable_adapter_t(std::filesystem::path command = "/usr/bin/steamosctl");
    bool implemented() const override;
    std::string unavailable_reason() const override;
    bool command_present() const override;
    bool start(direction_e direction) override;
    step_e poll() override;
    void abort() override;

  private:
    std::filesystem::path command_;
  };

  /// The process-wide coordinator over the production host, adapter and clock, or the test ones.
  coordinator_t &coordinator();
  /// Replaces the dependencies until called again with nulls. Tests only.
  void set_dependencies_for_tests(host_t *host, adapter_t *adapter, clock_t *clock, std::chrono::milliseconds deadline = k_max_deadline);

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
