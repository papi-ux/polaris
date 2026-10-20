/**
 * @file src/steamos_session.h
 * @brief #204 SteamOS session switch: the portable rules (records, decisions, fixed command).
 *
 * The production adapter reads logind (system bus), the account's own systemd user manager and
 * SteamOS Manager (the account's user bus), the pacman package record and /proc, and hands the raw
 * records to the functions here. Nothing in this header performs I/O on a bus or runs a command, so
 * every rule is testable without a host.
 *
 * Upstream facts these rules rely on (steamos-manager 26.4.1 = a0d399de, main 08c45b54; systemd v257
 * 70bae764; retained under the #204 evidence folder):
 * - `steamosctl switch-to-desktop-mode` / `switch-to-game-mode` write a temporary SDDM autologin
 *   file and then queue StopUnit("graphical-session.target") on the caller's user manager; the call
 *   returns when the stop job is queued, not when the session ended (session.rs:283-305).
 * - graphical-session.target is per account, not per login session, so the command can only be bound
 *   to one login session when the account has exactly one graphical session.
 * - a target acquires a new InvocationID on every start (systemd target.c:108-118), which fences
 *   the target generation the command will stop.
 * - the call takes no session or invocation argument: there is no compare-and-swap. The window
 *   between our last check and the manager's StopUnit cannot be closed; it is detected afterwards.
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prelaunch::steamos {
  /// The one trusted command, at its one fixed path.
  inline constexpr std::string_view k_steamosctl = "/usr/bin/steamosctl";
  /// The user daemon the command talks to, as its package installs it.
  inline constexpr std::string_view k_manager_executable = "/usr/lib/steamos-manager";
  inline constexpr std::string_view k_package = "steamos-manager";
  /// Exact upstream releases whose session semantics were audited. Anything else is unknown.
  inline const std::vector<std::string> &audited_versions() {
    static const std::vector<std::string> versions {"26.4.1"};
    return versions;
  }
  inline constexpr std::string_view k_manager_name = "com.steampowered.SteamOSManager1";
  inline constexpr std::string_view k_manager_path = "/com/steampowered/SteamOSManager1";
  inline constexpr std::string_view k_session_management = "com.steampowered.SteamOSManager1.SessionManagement1";
  inline constexpr std::string_view k_graphical_target = "graphical-session.target";
  inline constexpr std::string_view k_game_mode_unit = "gamescope-session.service";
  inline constexpr std::string_view k_polaris_unit = "polaris.service";

  /// A fixed reason: a stable code a client can key on, and words for a person.
  struct verdict_t {
    std::string code;  ///< empty when the check passed
    std::string reason;

    bool ok() const {
      return code.empty();
    }

    static verdict_t pass() {
      return {};
    }

    static verdict_t fail(std::string code, std::string reason) {
      return {std::move(code), std::move(reason)};
    }
  };

  // ---------------------------------------------------------------- package and executable

  struct package_version_t {
    std::uint64_t epoch = 0;
    std::string pkgver;
    std::string pkgrel;
    bool operator==(const package_version_t &) const = default;
  };

  /// "[epoch:]pkgver-pkgrel" as pacman writes it. Strict: epoch digits only, pkgver of
  /// [A-Za-z0-9._+] without '-' or ':', pkgrel digits with at most one ".digits" suffix.
  std::optional<package_version_t> parse_package_version(std::string_view text);

  struct package_record_t {
    std::string name;
    package_version_t version;
    std::vector<std::string> files;  ///< from the %FILES% section, as written (no leading '/')
  };

  /// A pacman local-database `desc` text: the %NAME% and %VERSION% sections, each exactly once with
  /// exactly one value line. Anything else is nullopt.
  std::optional<package_record_t> parse_package_desc(std::string_view desc);
  /// The %FILES% section of a `files` text. Missing section or empty is nullopt.
  std::optional<std::vector<std::string>> parse_package_files(std::string_view files);

  /// Exactly one installed record of `name` under `local_db` (the selected pacman local database), whose
  /// directory name is `name-version` and whose desc names it. Zero or several is a refusal.
  verdict_t read_installed_package(const std::filesystem::path &local_db, std::string_view name, package_record_t &out);
  /// The installed release is one of the audited ones (epoch 0) and owns both fixed paths.
  verdict_t package_supported(const package_record_t &record, const std::vector<std::string> &audited = audited_versions());

  /// A file that may be executed for this transition: a regular file (not a symlink) owned by
  /// `owner_uid`, not writable by group or others, executable, with every parent directory up to and
  /// including `trusted_root` owned by `owner_uid` and not writable by group or others. Production
  /// passes uid 0 and "/"; a path outside `trusted_root` is untrusted.
  verdict_t executable_trusted(const std::filesystem::path &path, std::uint32_t owner_uid, const std::filesystem::path &trusted_root = "/");

  // ---------------------------------------------------------------- bus records (typed)

  /// One login1 ListSessions() row, a(susso).
  struct session_row_t {
    std::string id;
    std::uint32_t uid = 0;
    std::string user;
    std::string seat;
    std::string path;
    bool operator==(const session_row_t &) const = default;
  };

  /// The login1 Session properties this transition reads (types as in org.freedesktop.login1(5)).
  struct session_record_t {
    std::string id;                        ///< s Id
    std::uint32_t uid = 0;                 ///< (uo) User
    std::string seat;                      ///< (so) Seat, the id
    std::string type;                      ///< s Type
    std::string klass;                     ///< s Class
    std::string state;                     ///< s State
    std::string service;                   ///< s Service (PAM service, recorded by logind)
    bool active = false;                   ///< b Active
    bool remote = false;                   ///< b Remote
    std::uint32_t leader = 0;              ///< u Leader
    std::uint64_t timestamp_monotonic = 0; ///< t TimestampMonotonic
    bool operator==(const session_record_t &) const = default;
  };

  /// A user-manager unit: s ActiveState, ay InvocationID, as PartOf/BindsTo/Requisite/StopPropagatedFrom.
  struct unit_record_t {
    bool loaded = false;  ///< false when GetUnit answered NoSuchUnit
    std::string active_state;
    std::string invocation;  ///< 32 lowercase hex digits, or empty
    std::vector<std::string> part_of;
    std::vector<std::string> binds_to;
    std::vector<std::string> requisite;
    std::vector<std::string> stop_propagated_from;
    std::string control_group;  ///< s ControlGroup of a service (read for polaris.service only)
    bool operator==(const unit_record_t &) const = default;
  };

  /// The units a stop of graphical-session.target reaches, following systemd's stop propagation in
  /// reverse: RequiredBy, ConsistsOf (PartOf), BoundBy, RequisiteOf and PropagatesStopTo, transitively.
  struct stop_closure_t {
    bool complete = false;  ///< false when it was not walked, or exceeded its bound
    std::vector<std::string> units;  ///< sorted
    bool operator==(const stop_closure_t &) const = default;
  };
  /// The most units a closure walk visits before it is called unproven.
  inline constexpr std::size_t k_max_closure_units = 128;

  /// Hex of a 16-byte InvocationID. Any other length, or all zeros, is empty (no invocation).
  std::string invocation_hex(const std::vector<std::uint8_t> &bytes);

  /// Everything one fresh read gathered, with the leader's identity bracketed from /proc.
  struct snapshot_t {
    std::string boot_id;
    std::uint32_t uid = 0;
    std::string seat0_active_id;  ///< empty when seat0 has no active session
    std::vector<session_row_t> rows;
    std::map<std::string, session_record_t> records;  ///< by session id; graphical rows of this uid
    std::uint64_t leader_start_ticks = 0;  ///< of the seat0 active session's leader, bracketed
    unit_record_t target;
    unit_record_t game_mode;
    unit_record_t polaris;
    stop_closure_t stop_closure;  ///< walked only when the caller asked for it
    /// This process's own cgroup v2 path (from /proc/self/cgroup); empty when unread. Not bracketed.
    std::string self_cgroup;
    bool operator==(const snapshot_t &) const = default;
  };

  /// What a Game Mode unit says about the host, from typed records only.
  enum class game_mode_e {
    unknown,  ///< not read
    absent,  ///< this platform has no Game Mode
    inactive,  ///< not loaded, inactive or failed
    active,
    transitional,  ///< activating, deactivating, reloading or anything else
  };
  game_mode_e game_mode_of(const unit_record_t &unit);

  enum class mode_e { game, desktop };

  /// The generation a transition binds and later proves again. Equality is field by field.
  struct session_identity_t {
    std::string boot_id;
    std::string logind_id;
    std::uint64_t logind_started = 0;  ///< TimestampMonotonic
    std::uint32_t uid = 0;
    std::uint32_t leader_pid = 0;
    std::uint64_t leader_start_ticks = 0;
    std::string target_invocation;
    mode_e mode = mode_e::game;

    bool valid() const {
      return !boot_id.empty() && !logind_id.empty() && logind_started > 0 && leader_pid > 1 &&
             leader_start_ticks > 0 && target_invocation.size() == 32;
    }

    bool operator==(const session_identity_t &) const = default;
  };

  inline bool graphical_type(std::string_view type) {
    return type == "wayland" || type == "x11" || type == "mir";
  }

  /**
   * @brief The account's one graphical session, when it is provably the one the command will end.
   *
   * Requires: seat0's active session belongs to this account; it is the account's only graphical
   * user-class session; it is local, active, started by SDDM autologin; its leader's start time was
   * read; graphical-session.target is active with an invocation; and the Game Mode unit says which
   * mode it is (active = game, inactive/failed/not loaded = desktop, anything in between = unknown).
   */
  verdict_t bind_session(const snapshot_t &snapshot, session_identity_t &out);

  /// Whether `bound` has ended: its logind session is gone or closing, and the target generation it
  /// carried is no longer the active one. Absent records are retired; nothing is guessed.
  bool retired(const snapshot_t &snapshot, const session_identity_t &bound);

  /// The start time (field 22 of /proc/<pid>/stat) of `pid`, or nullopt when it cannot be read.
  std::optional<std::uint64_t> process_start_ticks(const std::filesystem::path &proc_root, std::uint32_t pid);
  /// A small file's whole text, refused (not truncated) when larger than `limit`.
  std::optional<std::string> read_text_limited(const std::filesystem::path &path, std::size_t limit);

  /**
   * @brief Polaris survives the switch: the active polaris.service is this very process's unit and
   * no stop that graphical-session.target propagates reaches it.
   *
   * Requires: polaris.service active, its ControlGroup equal to this process's own cgroup, none of
   * its direct stop dependencies naming the session's units, and a complete stop closure of
   * graphical-session.target that does not contain it. An unwalked or oversized closure is unproven.
   */
  verdict_t polaris_independent_of_session(const snapshot_t &snapshot);
  /// The path of a cgroup v2 /proc/<pid>/cgroup text ("0::<path>", one line), or empty.
  std::string cgroup_v2_path(std::string_view cgroup);
  /// The system bus's logind: systemd-logind.service in the system slice.
  bool cgroup_is_logind(std::string_view cgroup);
  /// This account's own systemd user manager: user@<uid>.service/init.scope.
  bool cgroup_is_user_manager(std::string_view cgroup, std::uint32_t uid);

  // ---------------------------------------------------------------- the command

  enum class switch_e { to_desktop, to_game_mode };

  /// The exact argv: the fixed path and one temporary-switch verb, nothing else. Never a session
  /// name, never a persistent default, never a shell.
  std::vector<std::string> command_argv(switch_e direction);
  /// The child's whole environment: this account's user bus, a fixed PATH and the C locale.
  std::vector<std::string> command_env(std::uint32_t uid);
  /// unix:path=/run/user/<uid>/bus. Never taken from DBUS_SESSION_BUS_ADDRESS.
  std::string user_bus_address(std::uint32_t uid);
  inline constexpr std::string_view k_system_bus_address = "unix:path=/run/dbus/system_bus_socket";
  /// The account's runtime directory and bus socket belong to it (owner uid, 0700 directory, socket).
  verdict_t user_bus_trusted(const std::filesystem::path &runtime_dir, std::uint32_t uid);

  /// The bus owner's cgroup is this account's steamos-manager.service under its own user manager.
  bool cgroup_is_manager_unit(std::string_view cgroup, std::uint32_t uid);
}  // namespace prelaunch::steamos
