/**
 * @file src/platform/linux/steamos_bus.cpp
 * @brief #204: bounded, cancellable reads of logind, the user manager and SteamOS Manager.
 */
#include "steamos_bus.h"

#include <algorithm>
#include <thread>

#ifndef _WIN32
  #include <unistd.h>
#endif

namespace prelaunch::steamos::bus {
  namespace {
    constexpr const char *k_login1 = "org.freedesktop.login1";
    constexpr const char *k_login1_path = "/org/freedesktop/login1";
    constexpr const char *k_login1_manager = "org.freedesktop.login1.Manager";
    constexpr const char *k_login1_seat = "org.freedesktop.login1.Seat";
    constexpr const char *k_login1_session = "org.freedesktop.login1.Session";
    constexpr const char *k_systemd = "org.freedesktop.systemd1";
    constexpr const char *k_systemd_path = "/org/freedesktop/systemd1";
    constexpr const char *k_systemd_manager = "org.freedesktop.systemd1.Manager";
    constexpr const char *k_systemd_unit = "org.freedesktop.systemd1.Unit";
    constexpr const char *k_properties = "org.freedesktop.DBus.Properties";
    constexpr const char *k_dbus = "org.freedesktop.DBus";
    constexpr const char *k_dbus_path = "/org/freedesktop/DBus";
    /// No real host has more; a longer list is refused rather than walked without bound.
    constexpr std::size_t k_max_sessions = 64;

    /// Async completion captured for the pump.
    struct pending_t {
      bool done = false;
      GAsyncResult *result = nullptr;
    };

    void on_ready(GObject *, GAsyncResult *result, gpointer data) {
      auto *pending = static_cast<pending_t *>(data);
      pending->result = G_ASYNC_RESULT(g_object_ref(result));
      pending->done = true;
    }

    /// Iterates `context` until `pending` completes. On deadline or cancellation it cancels the
    /// operation and still waits for its completion callback, which GIO delivers promptly, so no
    /// callback can outlive the stack frame that owns `pending`.
    void pump(GMainContext *context, pending_t &pending, GCancellable *cancellable, const budget_t &budget, bool &timed_out, bool &cancelled) {
      while (!pending.done) {
        if (!g_cancellable_is_cancelled(cancellable) && budget.stop()) {
          timed_out = budget.expired();
          cancelled = !timed_out;
          g_cancellable_cancel(cancellable);
        }
        if (!g_main_context_iteration(context, FALSE)) {
          std::this_thread::sleep_for(std::chrono::milliseconds {1});
        }
      }
    }

    std::string text_of(GError *error) {
      return error && error->message ? error->message : "unknown error";
    }

    /// The remaining budget as a D-Bus call timeout (at least 1 ms, at most INT_MAX).
    int timeout_ms(const budget_t &budget) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(budget.deadline - clock_t::now()).count();
      return static_cast<int>(std::clamp<long long>(left, 1, 0x7fffffff));
    }

    verdict_t malformed(std::string what) {
      return verdict_t::fail("prelaunch_session_record_malformed", "A session record had an unexpected shape: " + std::move(what) + ".");
    }

    verdict_t reply_failure(const reply_t &reply, std::string_view what) {
      if (reply.cancelled) return verdict_t::fail("prelaunch_cancelled", "The launch was cancelled while the host's sessions were read.");
      if (reply.timed_out) return verdict_t::fail("prelaunch_session_read_timeout", "Reading " + std::string {what} + " did not finish in time.");
      return verdict_t::fail("prelaunch_session_identity_unavailable", "Reading " + std::string {what} + " failed: " + reply.error);
    }

    /// Looks up `key` in an a{sv} with an exact type. Absent or another type is nullptr.
    GVariant *lookup(GVariant *dict, const char *key, const char *type) {
      return g_variant_lookup_value(dict, key, G_VARIANT_TYPE(type));
    }

    bool get_string(GVariant *dict, const char *key, std::string &out) {
      variant_ptr value {lookup(dict, key, "s")};
      if (!value) return false;
      out = g_variant_get_string(value.get(), nullptr);
      return true;
    }

    bool get_bool(GVariant *dict, const char *key, bool &out) {
      variant_ptr value {lookup(dict, key, "b")};
      if (!value) return false;
      out = g_variant_get_boolean(value.get());
      return true;
    }

    bool get_u32(GVariant *dict, const char *key, std::uint32_t &out) {
      variant_ptr value {lookup(dict, key, "u")};
      if (!value) return false;
      out = g_variant_get_uint32(value.get());
      return true;
    }

    bool get_u64(GVariant *dict, const char *key, std::uint64_t &out) {
      variant_ptr value {lookup(dict, key, "t")};
      if (!value) return false;
      out = g_variant_get_uint64(value.get());
      return true;
    }

    bool get_strv(GVariant *dict, const char *key, std::vector<std::string> &out) {
      variant_ptr value {lookup(dict, key, "as")};
      if (!value) return false;
      out.clear();
      GVariantIter iter;
      const gchar *item = nullptr;
      g_variant_iter_init(&iter, value.get());
      while (g_variant_iter_next(&iter, "&s", &item)) out.emplace_back(item);
      return true;
    }

    reply_t get_all(client_t &client, const char *destination, const char *path, const char *interface, const budget_t &budget) {
      return client.call(destination, path, k_properties, "GetAll", g_variant_new("(s)", interface), G_VARIANT_TYPE("(a{sv})"), budget);
    }

    verdict_t read_session(client_t &system, const std::string &path, const budget_t &budget, session_record_t &out) {
      auto reply = get_all(system, k_login1, path.c_str(), k_login1_session, budget);
      if (!reply.ok()) return reply_failure(reply, "a login session");
      variant_ptr dict {g_variant_get_child_value(reply.value.get(), 0)};
      variant_ptr user {lookup(dict.get(), "User", "(uo)")};
      variant_ptr seat {lookup(dict.get(), "Seat", "(so)")};
      if (!user || !seat) return malformed("Session User/Seat");
      const gchar *seat_id = nullptr;
      g_variant_get(user.get(), "(u&o)", &out.uid, nullptr);
      g_variant_get(seat.get(), "(&s&o)", &seat_id, nullptr);
      out.seat = seat_id;
      if (!get_string(dict.get(), "Id", out.id) || !get_string(dict.get(), "Type", out.type) ||
          !get_string(dict.get(), "Class", out.klass) || !get_string(dict.get(), "State", out.state) ||
          !get_string(dict.get(), "Service", out.service) || !get_bool(dict.get(), "Active", out.active) ||
          !get_bool(dict.get(), "Remote", out.remote) || !get_u32(dict.get(), "Leader", out.leader) ||
          !get_u64(dict.get(), "TimestampMonotonic", out.timestamp_monotonic)) {
        return malformed("Session properties");
      }
      return verdict_t::pass();
    }

    verdict_t read_unit(client_t &user, const char *name, const budget_t &budget, unit_record_t &out) {
      out = {};
      auto unit = user.call(k_systemd, k_systemd_path, k_systemd_manager, "GetUnit", g_variant_new("(s)", name), G_VARIANT_TYPE("(o)"), budget);
      if (!unit.ok()) {
        if (unit.error_name == "org.freedesktop.systemd1.NoSuchUnit") return verdict_t::pass();  // not loaded
        return reply_failure(unit, std::string {"the user unit "} + name);
      }
      const gchar *path = nullptr;
      g_variant_get(unit.value.get(), "(&o)", &path);
      const std::string unit_path = path;
      auto reply = get_all(user, k_systemd, unit_path.c_str(), k_systemd_unit, budget);
      if (!reply.ok()) return reply_failure(reply, std::string {"the user unit "} + name);
      variant_ptr dict {g_variant_get_child_value(reply.value.get(), 0)};
      variant_ptr invocation {lookup(dict.get(), "InvocationID", "ay")};
      if (!invocation || !get_string(dict.get(), "ActiveState", out.active_state) || !get_strv(dict.get(), "PartOf", out.part_of) ||
          !get_strv(dict.get(), "BindsTo", out.binds_to) || !get_strv(dict.get(), "Requisite", out.requisite) ||
          !get_strv(dict.get(), "StopPropagatedFrom", out.stop_propagated_from)) {
        return malformed(std::string {"Unit properties of "} + name);
      }
      gsize length = 0;
      const auto *bytes = static_cast<const std::uint8_t *>(g_variant_get_fixed_array(invocation.get(), &length, 1));
      out.invocation = invocation_hex(std::vector<std::uint8_t>(bytes, bytes + length));
      out.loaded = true;
      return verdict_t::pass();
    }

    verdict_t read_active_seat(client_t &system, const budget_t &budget, std::string &id) {
      auto seat = system.call(k_login1, k_login1_path, k_login1_manager, "GetSeat", g_variant_new("(s)", "seat0"), G_VARIANT_TYPE("(o)"), budget);
      if (!seat.ok()) return reply_failure(seat, "seat0");
      const gchar *seat_path = nullptr;
      g_variant_get(seat.value.get(), "(&o)", &seat_path);
      const std::string path = seat_path;
      auto active = system.call(k_login1, path.c_str(), k_properties, "Get", g_variant_new("(ss)", k_login1_seat, "ActiveSession"),
        G_VARIANT_TYPE("(v)"), budget);
      if (!active.ok()) return reply_failure(active, "seat0's active session");
      variant_ptr boxed {g_variant_get_child_value(active.value.get(), 0)};
      variant_ptr inner {g_variant_get_variant(boxed.get())};
      if (!g_variant_is_of_type(inner.get(), G_VARIANT_TYPE("(so)"))) return malformed("Seat ActiveSession");
      const gchar *session_id = nullptr;
      g_variant_get(inner.get(), "(&s&o)", &session_id, nullptr);
      id = session_id;
      return verdict_t::pass();
    }

    endpoint_t user_endpoint(const sources_t &sources) {
      return sources.user.address.empty() ? endpoint_t {user_bus_address(sources.uid), true} : sources.user;
    }
  }  // namespace

  client_t::~client_t() {
    if (connection_) {
      // A peer that still has replies in flight cannot reach this context once it is gone.
      g_object_unref(connection_);
    }
    if (context_) {
      // Drain anything already dispatched to this context before it is released.
      while (g_main_context_iteration(context_, FALSE)) {}
      g_main_context_unref(context_);
    }
  }

  std::unique_ptr<client_t> client_t::connect(const endpoint_t &endpoint, const budget_t &budget, std::string &error) {
    std::unique_ptr<client_t> client {new client_t};
    client->context_ = g_main_context_new();
    g_main_context_push_thread_default(client->context_);
    GCancellable *cancellable = g_cancellable_new();
    pending_t pending;
    auto flags = static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                                   (endpoint.message_bus ? G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION : 0));
    g_dbus_connection_new_for_address(endpoint.address.c_str(), flags, nullptr, cancellable, on_ready, &pending);
    bool timed_out = false;
    bool cancelled = false;
    pump(client->context_, pending, cancellable, budget, timed_out, cancelled);
    GError *gerror = nullptr;
    client->connection_ = g_dbus_connection_new_for_address_finish(pending.result, &gerror);
    g_object_unref(pending.result);
    g_object_unref(cancellable);
    g_main_context_pop_thread_default(client->context_);
    if (!client->connection_) {
      error = cancelled ? "cancelled" : timed_out ? "timed out" : text_of(gerror);
      if (gerror) g_error_free(gerror);
      return nullptr;
    }
    g_dbus_connection_set_exit_on_close(client->connection_, FALSE);
    return client;
  }

  reply_t client_t::call(
    const char *destination,
    const char *path,
    const char *interface,
    const char *method,
    GVariant *parameters,
    const GVariantType *reply_type,
    const budget_t &budget
  ) {
    reply_t reply;
    if (budget.stop()) {
      if (parameters) g_variant_unref(g_variant_ref_sink(parameters));
      reply.timed_out = budget.expired();
      reply.cancelled = !reply.timed_out;
      reply.error = reply.timed_out ? "timed out" : "cancelled";
      return reply;
    }
    g_main_context_push_thread_default(context_);
    GCancellable *cancellable = g_cancellable_new();
    pending_t pending;
    g_dbus_connection_call(connection_, destination, path, interface, method, parameters, reply_type,
      G_DBUS_CALL_FLAGS_NO_AUTO_START, timeout_ms(budget), cancellable, on_ready, &pending);
    pump(context_, pending, cancellable, budget, reply.timed_out, reply.cancelled);
    GError *gerror = nullptr;
    reply.value.reset(g_dbus_connection_call_finish(connection_, pending.result, &gerror));
    g_object_unref(pending.result);
    g_object_unref(cancellable);
    g_main_context_pop_thread_default(context_);
    if (!reply.value) {
      if (gerror) {
        if (g_error_matches(gerror, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) reply.timed_out = true;
        if (gchar *remote = g_dbus_error_get_remote_error(gerror)) {
          reply.error_name = remote;
          g_free(remote);
        }
        reply.error = text_of(gerror);
        g_error_free(gerror);
      }
      if (reply.timed_out) reply.cancelled = false;
    }
    return reply;
  }

  verdict_t read_snapshot(const sources_t &sources, const budget_t &budget, snapshot_t &out) {
    snapshot_t snapshot;
    snapshot.uid = sources.uid;
    if (const auto boot = read_text_limited(sources.boot_id, 64)) {
      snapshot.boot_id = *boot;
      while (!snapshot.boot_id.empty() && (snapshot.boot_id.back() == '\n' || snapshot.boot_id.back() == ' ')) snapshot.boot_id.pop_back();
    }

    std::string error;
    auto system = client_t::connect(sources.system, budget, error);
    if (!system) {
      if (budget.stop()) return budget.expired() ? verdict_t::fail("prelaunch_session_read_timeout", "The system bus did not answer in time.") :
                                                   verdict_t::fail("prelaunch_cancelled", "The launch was cancelled.");
      return verdict_t::fail("prelaunch_session_identity_unavailable", "The system bus could not be reached: " + error);
    }
    if (auto seat = read_active_seat(*system, budget, snapshot.seat0_active_id); !seat.ok()) return seat;

    auto list = system->call(k_login1, k_login1_path, k_login1_manager, "ListSessions", nullptr, G_VARIANT_TYPE("(a(susso))"), budget);
    if (!list.ok()) return reply_failure(list, "the login sessions");
    {
      variant_ptr rows {g_variant_get_child_value(list.value.get(), 0)};
      if (g_variant_n_children(rows.get()) > k_max_sessions) return malformed("too many login sessions");
      GVariantIter iter;
      const gchar *id = nullptr;
      const gchar *user = nullptr;
      const gchar *seat = nullptr;
      const gchar *path = nullptr;
      guint32 uid = 0;
      g_variant_iter_init(&iter, rows.get());
      while (g_variant_iter_next(&iter, "(&su&s&s&o)", &id, &uid, &user, &seat, &path)) {
        snapshot.rows.push_back({id, uid, user, seat, path});
      }
    }
    // Records for this account's sessions and for the seat's active one, whoever owns it.
    for (const auto &row : snapshot.rows) {
      if (row.uid != sources.uid && row.id != snapshot.seat0_active_id) continue;
      session_record_t record;
      if (auto read = read_session(*system, row.path, budget, record); !read.ok()) return read;
      if (record.id != row.id) return malformed("Session Id differs from its listing");
      snapshot.records[row.id] = record;
    }
    // The leader's start time, read before the units and checked again after them.
    std::uint64_t leader_before = 0;
    if (const auto active = snapshot.records.find(snapshot.seat0_active_id); active != snapshot.records.end()) {
      leader_before = process_start_ticks(sources.proc_root, active->second.leader).value_or(0);
    }

    if (auto bus = user_bus_trusted(sources.runtime_root / std::to_string(sources.uid), sources.uid); !bus.ok() && sources.user.address.empty()) {
      return bus;
    }
    auto user = client_t::connect(user_endpoint(sources), budget, error);
    if (!user) {
      if (budget.stop()) return budget.expired() ? verdict_t::fail("prelaunch_session_read_timeout", "The user bus did not answer in time.") :
                                                   verdict_t::fail("prelaunch_cancelled", "The launch was cancelled.");
      return verdict_t::fail("prelaunch_session_identity_unavailable", "This account's user bus could not be reached: " + error);
    }
    if (auto read = read_unit(*user, std::string {k_graphical_target}.c_str(), budget, snapshot.target); !read.ok()) return read;
    if (auto read = read_unit(*user, std::string {k_game_mode_unit}.c_str(), budget, snapshot.game_mode); !read.ok()) return read;
    if (auto read = read_unit(*user, std::string {k_polaris_unit}.c_str(), budget, snapshot.polaris); !read.ok()) return read;

    // Close the bracket: the same active session and the same leader process throughout.
    std::string active_after;
    if (auto seat = read_active_seat(*system, budget, active_after); !seat.ok()) return seat;
    if (active_after != snapshot.seat0_active_id) {
      return verdict_t::fail("prelaunch_session_identity_unavailable", "The host's active session changed while it was read.");
    }
    if (const auto active = snapshot.records.find(snapshot.seat0_active_id); active != snapshot.records.end()) {
      const auto leader_after = process_start_ticks(sources.proc_root, active->second.leader).value_or(0);
      if (leader_before != leader_after) {
        return verdict_t::fail("prelaunch_session_identity_unavailable", "The session leader changed while it was read.");
      }
      snapshot.leader_start_ticks = leader_after;
    }
    out = std::move(snapshot);
    return verdict_t::pass();
  }

  verdict_t probe_manager(const sources_t &sources, const budget_t &budget) {
    const auto unavailable = [](std::string reason) {
      return verdict_t::fail("prelaunch_session_management_unavailable", std::move(reason));
    };
    if (auto bus = user_bus_trusted(sources.runtime_root / std::to_string(sources.uid), sources.uid); !bus.ok() && sources.user.address.empty()) {
      return bus;
    }
    std::string error;
    auto user = client_t::connect(user_endpoint(sources), budget, error);
    if (!user) {
      if (budget.stop()) return budget.expired() ? verdict_t::fail("prelaunch_session_read_timeout", "The user bus did not answer in time.") :
                                                   verdict_t::fail("prelaunch_cancelled", "The launch was cancelled.");
      return unavailable("This account's user bus could not be reached: " + error);
    }
    const std::string name {k_manager_name};
    auto owner = user->call(k_dbus, k_dbus_path, k_dbus, "GetNameOwner", g_variant_new("(s)", name.c_str()), G_VARIANT_TYPE("(s)"), budget);
    if (!owner.ok()) {
      // Not running: reading never starts it.
      if (owner.error_name == "org.freedesktop.DBus.Error.NameHasNoOwner") return unavailable("SteamOS Manager is not running for this account.");
      if (owner.timed_out || owner.cancelled) return reply_failure(owner, "SteamOS Manager");
      return unavailable("SteamOS Manager's bus name could not be read: " + owner.error);
    }
    const gchar *unique_name = nullptr;
    g_variant_get(owner.value.get(), "(&s)", &unique_name);
    const std::string unique = unique_name;
    auto owner_uid = user->call(k_dbus, k_dbus_path, k_dbus, "GetConnectionUnixUser", g_variant_new("(s)", unique.c_str()), G_VARIANT_TYPE("(u)"), budget);
    auto owner_pid = user->call(k_dbus, k_dbus_path, k_dbus, "GetConnectionUnixProcessID", g_variant_new("(s)", unique.c_str()), G_VARIANT_TYPE("(u)"), budget);
    if (!owner_uid.ok()) return reply_failure(owner_uid, "SteamOS Manager's owner");
    if (!owner_pid.ok()) return reply_failure(owner_pid, "SteamOS Manager's process");
    guint32 uid = 0;
    guint32 pid = 0;
    g_variant_get(owner_uid.value.get(), "(u)", &uid);
    g_variant_get(owner_pid.value.get(), "(u)", &pid);
    if (uid != sources.uid) return unavailable("SteamOS Manager on this bus belongs to another account.");
    // The bus owner is this account's steamos-manager unit running the packaged executable: the
    // cgroup is assigned by systemd and the executable link by the kernel, never by the process.
    const auto before = process_start_ticks(sources.proc_root, pid);
    const auto cgroup = read_text_limited(sources.proc_root / std::to_string(pid) / "cgroup", 4096);
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink(sources.proc_root / std::to_string(pid) / "exe", ec);
    const auto after = process_start_ticks(sources.proc_root, pid);
    if (!before || before != after || !cgroup || !cgroup_is_manager_unit(*cgroup, sources.uid)) {
      return unavailable("The process owning SteamOS Manager's name is not this account's steamos-manager unit.");
    }
    if (ec || exe != std::filesystem::path {std::string {k_manager_executable}}) {
      return unavailable("The process owning SteamOS Manager's name is not the packaged steamos-manager.");
    }
    // Introspect the unique name, not the well-known one: it cannot activate anything.
    auto xml = user->call(unique.c_str(), std::string {k_manager_path}.c_str(), "org.freedesktop.DBus.Introspectable", "Introspect", nullptr,
      G_VARIANT_TYPE("(s)"), budget);
    if (!xml.ok()) return reply_failure(xml, "SteamOS Manager's interfaces");
    const gchar *text = nullptr;
    g_variant_get(xml.value.get(), "(&s)", &text);
    return introspection_supports_switch(text);
  }

  bool manager_owner(const sources_t &sources, const budget_t &budget, std::string &unique) {
    unique.clear();
    std::string error;
    auto user = client_t::connect(user_endpoint(sources), budget, error);
    if (!user) return false;
    const std::string name {k_manager_name};
    auto owner = user->call(k_dbus, k_dbus_path, k_dbus, "GetNameOwner", g_variant_new("(s)", name.c_str()), G_VARIANT_TYPE("(s)"), budget);
    if (owner.ok()) {
      const gchar *owner_name = nullptr;
      g_variant_get(owner.value.get(), "(&s)", &owner_name);
      unique = owner_name;
      return true;
    }
    return !owner.timed_out && !owner.cancelled && owner.error_name == "org.freedesktop.DBus.Error.NameHasNoOwner";
  }

  verdict_t probe_support(const sources_t &sources, const budget_t &budget) {
    if (auto trusted = executable_trusted(sources.steamosctl, sources.trusted_owner_uid, sources.trusted_root); !trusted.ok()) return trusted;
    package_record_t package;
    if (auto read = read_installed_package(sources.pacman_local, k_package, package); !read.ok()) return read;
    if (auto supported = package_supported(package); !supported.ok()) return supported;
    return probe_manager(sources, budget);
  }
}  // namespace prelaunch::steamos::bus
