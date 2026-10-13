/**
 * @file src/platform/linux/steamos_bus.h
 * @brief #204: bounded, cancellable reads of logind, the account's user manager and SteamOS Manager.
 *
 * Every connection is to an explicit address (never DBUS_*_BUS_ADDRESS from the environment), owned
 * by one reader and driven on its own GMainContext, so a stalled peer costs at most the caller's
 * deadline and a cancellation ends the wait. Calls never auto-start a service. Replies are checked
 * against the exact GVariant types documented by systemd v257 (org.freedesktop.login1(5),
 * org.freedesktop.systemd1(5)); any other shape is refused, never coerced.
 */
#pragma once

#include "src/steamos_session.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <gio/gio.h>

namespace prelaunch::steamos::bus {
  using clock_t = std::chrono::steady_clock;

  struct budget_t {
    clock_t::time_point deadline;
    std::function<bool()> cancelled;

    bool expired() const {
      return clock_t::now() >= deadline;
    }

    bool stop() const {
      return expired() || (cancelled && cancelled());
    }
  };

  struct endpoint_t {
    std::string address;
    /// True for a bus daemon (Hello, name ownership); false for a peer-to-peer test server.
    bool message_bus = true;
  };

  /// Where a read looks. Production uses the fixed paths; tests point everything at owned fixtures.
  struct sources_t {
    endpoint_t system {std::string {k_system_bus_address}, true};
    endpoint_t user;  ///< empty address: user_bus_address(uid)
    std::uint32_t uid = 0;
    std::filesystem::path proc_root = "/proc";
    std::filesystem::path boot_id = "/proc/sys/kernel/random/boot_id";
    std::filesystem::path self_cgroup = "/proc/self/cgroup";
    std::filesystem::path runtime_root = "/run/user";
    // SteamOS relocates pacman's DBPath to /usr/lib/holo/pacmandb.
    // The support probe and readiness fingerprint must read the same installed records.
    std::filesystem::path pacman_local = "/usr/lib/holo/pacmandb/local";
    std::filesystem::path steamosctl = std::string {k_steamosctl};
    std::uint32_t trusted_owner_uid = 0;  ///< owner of the executable and its directories
    std::filesystem::path trusted_root = "/";  ///< directories are checked up to and including this
  };

  struct gvariant_deleter {
    void operator()(GVariant *value) const {
      if (value) g_variant_unref(value);
    }
  };

  using variant_ptr = std::unique_ptr<GVariant, gvariant_deleter>;

  /// The outcome of one call.
  struct reply_t {
    variant_ptr value;
    std::string error_name;  ///< the remote D-Bus error name, when the peer answered with one
    std::string error;       ///< human text
    bool timed_out = false;
    bool cancelled = false;

    bool ok() const {
      return value != nullptr;
    }
  };

  /// One private connection on its own main context. Not shared, not thread-safe: one owner.
  class client_t {
  public:
    ~client_t();
    client_t(const client_t &) = delete;
    client_t &operator=(const client_t &) = delete;

    /// Connects within the budget. Null and `error` set on failure, timeout or cancellation.
    static std::unique_ptr<client_t> connect(const endpoint_t &endpoint, const budget_t &budget, std::string &error);

    /// One method call with an exact reply type. Never auto-starts the destination.
    reply_t call(
      const char *destination,
      const char *path,
      const char *interface,
      const char *method,
      GVariant *parameters,  ///< floating reference is consumed
      const GVariantType *reply_type,
      const budget_t &budget
    );

  private:
    client_t() = default;
    GMainContext *context_ = nullptr;
    GDBusConnection *connection_ = nullptr;
  };

  /**
   * @brief A fresh read of seat0, this account's sessions and its user manager's units.
   *
   * logind and the user manager are first bound by their bus names' current owners: the expected
   * account, a process start time and the cgroup systemd placed them in. Every call then goes to
   * those unique names. Two complete passes are compared field by field (the seat's active session,
   * the listing, every record, the leader's start, each unit's state, generation and, when asked,
   * the stop closure of graphical-session.target), and the owners and the boot are checked again.
   * Any difference, malformed record, timeout or cancellation is a refusal with a fixed code.
   */
  verdict_t read_snapshot(const sources_t &sources, const budget_t &budget, snapshot_t &out, bool with_closure = false);

  /// Parsed as D-Bus introspection XML: exactly one SessionManagement1 interface declaring
  /// SwitchToDesktopMode and SwitchToGameMode once each, without arguments. Malformed is refused.
  verdict_t introspection_supports_switch(std::string_view xml);

  /// SteamOS Manager on this account's own user bus: owned by this account, by its steamos-manager
  /// unit running the packaged executable, and offering SessionManagement1's two switch methods.
  /// Never activates the service.
  verdict_t probe_manager(const sources_t &sources, const budget_t &budget);

  /// The unique bus name currently owning SteamOS Manager's name on this account's bus, or empty when
  /// nobody owns it. Never activates it. False when the bus could not be asked.
  bool manager_owner(const sources_t &sources, const budget_t &budget, std::string &unique);

  /// Everything that must hold before the command may run: the trusted executable, the audited and
  /// owning package, the user bus and the manager.
  verdict_t probe_support(const sources_t &sources, const budget_t &budget);
}  // namespace prelaunch::steamos::bus
