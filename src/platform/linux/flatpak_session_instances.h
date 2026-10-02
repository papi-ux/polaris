/**
 * @file src/platform/linux/flatpak_session_instances.h
 * @brief Which live Flatpak instances a private session started, proven from what the kernel
 *        and Flatpak record rather than guessed from a process name.
 *
 * A Flatpak launcher run in a private session leaves processes the session token cannot find:
 * the outer bwrap and the sandbox's init read as a zero-byte environ for as long as they live,
 * and an Electron launcher overwrites its own. A game the launcher starts through the Flatpak
 * portal does not descend from the session at all. Flatpak records every instance under
 * $XDG_RUNTIME_DIR/.flatpak/<instance>/, and this reads those records, confirms each against the
 * kernel, and decides which instances the session owns. Everything else, a PrusaSlicer or a
 * browser already open on the desktop, is left alone.
 */
#pragma once

#ifdef __linux__

  #include <cstdint>
  #include <filesystem>
  #include <functional>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <sys/types.h>
  #include <vector>

namespace flatpak_session_instances {

  /// What the host reads about processes. Injected, so tests give fixtures in place of /proc.
  struct proc_reader_t {
    /// /proc/<pid>/<name>. nullopt when it cannot be read, because the process is gone or the
    /// kernel refused; an empty string is a read that returned zero bytes.
    std::function<std::optional<std::string>(pid_t pid, std::string_view name)> read;
    /// The inode of the pid namespace /proc/<pid>/ns/pid names.
    std::function<std::optional<std::uint64_t>(pid_t pid)> pid_namespace;
    /// The inode of that namespace's parent, from NS_GET_PARENT. nullopt at the host's namespace.
    std::function<std::optional<std::uint64_t>(pid_t pid)> parent_pid_namespace;
    /// The pids in a cgroup, given the path /proc/<pid>/cgroup names for it.
    std::function<std::optional<std::vector<pid_t>>(const std::string &cgroup)> cgroup_procs;
    /// Every pid in /proc.
    std::function<std::vector<pid_t>()> all_pids;
  };

  /// The reader for this host's /proc and cgroup file system.
  proc_reader_t live_proc_reader();

  /// One process, named by pid and start time together so a reused pid is never taken for it.
  struct process_t {
    pid_t pid = 0;
    std::uint64_t start_time = 0;  ///< /proc/<pid>/stat field 22, clock ticks since boot
  };

  /// A Flatpak instance its record describes and the kernel confirms is running.
  struct instance_t {
    std::string id;  ///< the instance directory's name
    std::string app_id;  ///< [Application] name= in its info file
    process_t bwrap;  ///< the outer bwrap, which keeps the pid `flatpak run` had
    process_t init;  ///< pid 1 of the sandbox, bwrapinfo.json child-pid
    std::uint64_t pid_namespace = 0;  ///< bwrapinfo.json pid-namespace
    std::string cgroup;  ///< its app-flatpak-<app>-<instance>.scope, as /proc/<pid>/cgroup names it
  };

  struct instances_t {
    std::vector<instance_t> live;
    /// Records whose bwrap is running but which cannot be confirmed yet or at all, such as a
    /// sandbox still starting that has not written bwrapinfo.json, or one outside any scope. Never
    /// owned and never signalled; their bwrap and init are known so nothing else signals them.
    std::vector<instance_t> unresolved;
  };

  /**
   * @brief Read the instance records in @p runtime_dir/.flatpak and keep those that are live.
   *
   * A record is live when its init's parent is its bwrap, the init started no earlier than the
   * bwrap, the init is in the pid namespace bwrapinfo.json names, and the init's cgroup is the
   * record's own app-flatpak-<escaped app id>-<instance>.scope. A directory Flatpak has not yet
   * collected names processes that are gone, or pids that were reused, and fails these checks.
   */
  instances_t read_live_instances(const std::filesystem::path &runtime_dir, const proc_reader_t &reader);

  /// Why an instance is the session's.
  enum class rule_e {
    lineage,  ///< R1: its outer bwrap descends from the session's compositor supervisor
    shared_namespace,  ///< R2: it shares, or nests in, the pid namespace of an owned instance
    token,  ///< R3: a process in its scope carries the session token in a readable environ
  };

  /// What an owned instance is to the session.
  enum class role_e {
    launcher,  ///< started by the session's own command (R1)
    game,  ///< not started by it, but carrying its token, as a portal sub-sandbox does (R3)
    helper,  ///< nothing but a shared namespace, as a Chromium helper sub-sandbox has (R2)
  };

  std::string_view role_name(role_e role);

  /// What the session holds that proves an instance is its own.
  struct evidence_t {
    process_t supervisor;  ///< the session's labwc supervisor, a subreaper
    std::string token;  ///< the session's POLARIS_SESSION_INSTANCE_ID
    /// App ids the session's commands ran with `flatpak run`, to name a desktop instance of the
    /// same app that the session left alone.
    std::vector<std::string> launched_app_ids;
  };

  struct owned_instance_t {
    instance_t instance;
    role_e role = role_e::helper;
    rule_e rule = rule_e::shared_namespace;
  };

  struct attribution_t {
    std::vector<owned_instance_t> owned;
    /// Live instances the session did not start.
    int left_alone = 0;
    /// Of those, the ones with the app id of an owned instance or of an app the session launched:
    /// a launcher already open on the desktop, which the session's `flatpak run` handed its launch
    /// to (issue #234).
    std::vector<instance_t> left_alone_same_app;
    std::vector<instance_t> unresolved;
  };

  /**
   * @brief Decide which live instances the session owns, and in what role.
   *
   * An instance whose outer bwrap started before the supervisor is never owned, whatever the rules
   * say: nothing the session started can be older than the session.
   */
  attribution_t attribute(const instances_t &instances, const evidence_t &evidence, const proc_reader_t &reader);

  /// One process of an instance, with what the host read of it.
  struct member_t {
    process_t process;
    pid_t parent = 0;
    std::vector<pid_t> nspid;  ///< NSpid from /proc/<pid>/status, the host's pid first
    bool carries_token = false;
    std::string comm;
  };

  /**
   * @brief Every process of @p instance.
   *
   * Read from its scope's cgroup.procs. When that cannot be read, every process in its pid
   * namespace, unless another live instance shares that namespace, when which processes are whose
   * cannot be told apart and nothing is returned.
   */
  std::optional<std::vector<member_t>> scope_members(
    const instance_t &instance,
    std::string_view token,
    const std::vector<instance_t> &live,
    const proc_reader_t &reader
  );

  /**
   * @brief The members that are the app: in a pid namespace below the host's, and neither pid 1
   *        of it nor the instance's own init.
   *
   * The outer bwrap and xdg-dbus-proxy run in the host's namespace and the init is pid 1 of the
   * sandbox's, so none of them is ever an app process, and none is ever sent SIGTERM. SIGTERM
   * kills the outer bwrap, which takes the sandbox down unordered, and the init ignores it, since
   * a signal from an ancestor namespace reaches it only when it has a handler. A portal
   * sub-sandbox that shares its launcher's namespace has an init that is not pid 1 there; it is
   * named by the instance's record instead.
   */
  std::vector<member_t> app_processes(const instance_t &instance, const std::vector<member_t> &members);

  /// The live instances of @p app_id.
  std::vector<instance_t> instances_of(const instances_t &instances, std::string_view app_id);

  /// Whether @p process is the outer bwrap or the sandbox init of one of @p instances.
  bool is_instance_bwrap(const process_t &process, const std::vector<instance_t> &instances);

  /// Whether @p pid descends from @p ancestor through parents none of which started later than its
  /// child, which a reused pid somewhere along the chain would.
  bool descends_through_rising_start_times(
    const process_t &process,
    const process_t &ancestor,
    const proc_reader_t &reader
  );

  // Parsers, exposed for tests.
  std::optional<std::uint64_t> start_time_from_stat(std::string_view stat);
  std::optional<pid_t> parent_from_stat(std::string_view stat);
  std::vector<pid_t> nspid_from_status(std::string_view status);
  /// The cgroup path in /proc/<pid>/cgroup: the unified hierarchy's, else systemd's own.
  std::string cgroup_from_proc(std::string_view cgroup);
  /// An app id as systemd unit names carry it: io.gitlab.librewolf-community becomes
  /// io.gitlab.librewolf\x2dcommunity.
  std::string escape_unit_name(std::string_view app_id);
  bool environ_carries_token(std::string_view environ, std::string_view token);

}  // namespace flatpak_session_instances

#endif
