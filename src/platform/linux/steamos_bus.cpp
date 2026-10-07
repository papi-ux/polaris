/**
 * @file src/platform/linux/steamos_bus.cpp
 * @brief #204: bounded, cancellable reads of logind, the user manager and SteamOS Manager.
 */
#include "steamos_bus.h"

#include <algorithm>
#include <map>
#include <set>
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
    constexpr const char *k_systemd_service = "org.freedesktop.systemd1.Service";
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

    verdict_t changed(std::string what) {
      return verdict_t::fail("prelaunch_session_identity_unavailable", "The host's " + std::move(what) + " changed while it was read.");
    }

    /// One a{sv}, each key exactly once. A repeated key, whatever its types, is not a record.
    class props_t {
    public:
      bool parse(GVariant *dict) {
        GVariantIter iter;
        const gchar *key = nullptr;
        GVariant *value = nullptr;
        g_variant_iter_init(&iter, dict);
        while (g_variant_iter_next(&iter, "{&sv}", &key, &value)) {
          variant_ptr owned {value};
          if (!values_.emplace(key, std::move(owned)).second) {
            duplicate_ = key;
            return false;
          }
        }
        return true;
      }

      const std::string &duplicate() const {
        return duplicate_;
      }

      /// The value of `key` when it has exactly `type`; nullptr when absent or of another type.
      GVariant *typed(const char *key, const char *type) const {
        const auto found = values_.find(key);
        if (found == values_.end() || !g_variant_is_of_type(found->second.get(), G_VARIANT_TYPE(type))) return nullptr;
        return found->second.get();
      }

      bool get(const char *key, std::string &out) const {
        auto *value = typed(key, "s");
        if (value) out = g_variant_get_string(value, nullptr);
        return value != nullptr;
      }

      bool get(const char *key, bool &out) const {
        auto *value = typed(key, "b");
        if (value) out = g_variant_get_boolean(value);
        return value != nullptr;
      }

      bool get(const char *key, std::uint32_t &out) const {
        auto *value = typed(key, "u");
        if (value) out = g_variant_get_uint32(value);
        return value != nullptr;
      }

      bool get(const char *key, std::uint64_t &out) const {
        auto *value = typed(key, "t");
        if (value) out = g_variant_get_uint64(value);
        return value != nullptr;
      }

      bool get(const char *key, std::vector<std::string> &out) const {
        auto *value = typed(key, "as");
        if (!value) return false;
        out.clear();
        GVariantIter iter;
        const gchar *item = nullptr;
        g_variant_iter_init(&iter, value);
        while (g_variant_iter_next(&iter, "&s", &item)) out.emplace_back(item);
        return true;
      }

    private:
      std::map<std::string, variant_ptr> values_;
      std::string duplicate_;
    };

    /// GetAll of `interface` on `path` at `destination`, parsed strictly.
    verdict_t get_all(client_t &client, const std::string &destination, const std::string &path, const char *interface,
                      const budget_t &budget, std::string_view what, props_t &out) {
      auto reply = client.call(destination.c_str(), path.c_str(), k_properties, "GetAll", g_variant_new("(s)", interface),
        G_VARIANT_TYPE("(a{sv})"), budget);
      if (!reply.ok()) return reply_failure(reply, what);
      variant_ptr dict {g_variant_get_child_value(reply.value.get(), 0)};
      if (!out.parse(dict.get())) return malformed(std::string {what} + " repeats its property " + out.duplicate());
      return verdict_t::pass();
    }

    /// A bus name's current owner, bound to an account, a process start time and a cgroup systemd
    /// assigned. Calls then go to the unique name, which cannot pass to another connection.
    struct owner_t {
      std::string unique;
      std::uint32_t pid = 0;
      std::uint64_t start_ticks = 0;
    };

    verdict_t name_owner(client_t &client, const char *name, const budget_t &budget, std::string &unique, std::string_view what) {
      auto owner = client.call(k_dbus, k_dbus_path, k_dbus, "GetNameOwner", g_variant_new("(s)", name), G_VARIANT_TYPE("(s)"), budget);
      if (!owner.ok()) {
        if (owner.error_name == "org.freedesktop.DBus.Error.NameHasNoOwner") {
          return verdict_t::fail("prelaunch_session_identity_unavailable", std::string {what} + " is not running.");
        }
        return reply_failure(owner, what);
      }
      const gchar *text = nullptr;
      g_variant_get(owner.value.get(), "(&s)", &text);
      unique = text;
      if (!unique.starts_with(":")) return malformed(std::string {what} + " owner is not a unique name");
      return verdict_t::pass();
    }

    verdict_t bind_owner(client_t &client, const char *name, std::uint32_t uid, const std::filesystem::path &proc_root,
                         const std::function<bool(std::string_view)> &placed, const budget_t &budget, std::string_view what,
                         std::string_view failure_code, owner_t &out) {
      if (auto found = name_owner(client, name, budget, out.unique, what); !found.ok()) {
        if (found.code == "prelaunch_session_identity_unavailable" && failure_code != found.code) {
          return verdict_t::fail(std::string {failure_code}, found.reason);
        }
        return found;
      }
      auto owner_uid = client.call(k_dbus, k_dbus_path, k_dbus, "GetConnectionUnixUser", g_variant_new("(s)", out.unique.c_str()),
        G_VARIANT_TYPE("(u)"), budget);
      if (!owner_uid.ok()) return reply_failure(owner_uid, what);
      auto owner_pid = client.call(k_dbus, k_dbus_path, k_dbus, "GetConnectionUnixProcessID", g_variant_new("(s)", out.unique.c_str()),
        G_VARIANT_TYPE("(u)"), budget);
      if (!owner_pid.ok()) return reply_failure(owner_pid, what);
      guint32 found_uid = 0;
      g_variant_get(owner_uid.value.get(), "(u)", &found_uid);
      g_variant_get(owner_pid.value.get(), "(u)", &out.pid);
      const auto refuse = [&](std::string why) {
        return verdict_t::fail(std::string {failure_code}, std::string {what} + " " + why);
      };
      if (found_uid != uid) return refuse("belongs to another account.");
      // The cgroup is assigned by systemd, never by the process; the start time brackets the read.
      const auto before = process_start_ticks(proc_root, out.pid);
      const auto cgroup = read_text_limited(proc_root / std::to_string(out.pid) / "cgroup", 4096);
      const auto after = process_start_ticks(proc_root, out.pid);
      if (!before || before != after) return refuse("process could not be identified.");
      if (!cgroup || !placed(*cgroup)) return refuse("is not the expected service.");
      out.start_ticks = *after;
      return verdict_t::pass();
    }

    /// The same connection still owns `name`, and its process is the same process.
    verdict_t owner_unchanged(client_t &client, const char *name, const owner_t &owner, const std::filesystem::path &proc_root,
                              const budget_t &budget, std::string_view what) {
      std::string unique;
      if (auto found = name_owner(client, name, budget, unique, what); !found.ok()) return found;
      if (unique != owner.unique || process_start_ticks(proc_root, owner.pid) != owner.start_ticks) {
        return changed(std::string {what} + " owner");
      }
      return verdict_t::pass();
    }

    verdict_t read_session(client_t &system, const std::string &logind, const std::string &path, const budget_t &budget, session_record_t &out) {
      props_t props;
      if (auto read = get_all(system, logind, path, k_login1_session, budget, "a login session", props); !read.ok()) return read;
      auto *user = props.typed("User", "(uo)");
      auto *seat = props.typed("Seat", "(so)");
      if (!user || !seat) return malformed("Session User/Seat");
      const gchar *seat_id = nullptr;
      g_variant_get(user, "(u&o)", &out.uid, nullptr);
      g_variant_get(seat, "(&s&o)", &seat_id, nullptr);
      out.seat = seat_id;
      if (!props.get("Id", out.id) || !props.get("Type", out.type) || !props.get("Class", out.klass) || !props.get("State", out.state) ||
          !props.get("Service", out.service) || !props.get("Active", out.active) || !props.get("Remote", out.remote) ||
          !props.get("Leader", out.leader) || !props.get("TimestampMonotonic", out.timestamp_monotonic)) {
        return malformed("Session properties");
      }
      return verdict_t::pass();
    }

    /// GetUnit: the object path, or empty for NoSuchUnit (not loaded).
    verdict_t unit_path(client_t &user, const std::string &systemd, const std::string &name, const budget_t &budget, std::string &path) {
      path.clear();
      auto unit = user.call(systemd.c_str(), k_systemd_path, k_systemd_manager, "GetUnit", g_variant_new("(s)", name.c_str()),
        G_VARIANT_TYPE("(o)"), budget);
      if (!unit.ok()) {
        if (unit.error_name == "org.freedesktop.systemd1.NoSuchUnit") return verdict_t::pass();
        return reply_failure(unit, "the user unit " + name);
      }
      const gchar *text = nullptr;
      g_variant_get(unit.value.get(), "(&o)", &text);
      path = text;
      return verdict_t::pass();
    }

    verdict_t read_unit(client_t &user, const std::string &systemd, const std::string &name, bool service_cgroup, const budget_t &budget, unit_record_t &out) {
      out = {};
      std::string path;
      if (auto found = unit_path(user, systemd, name, budget, path); !found.ok() || path.empty()) return found;
      props_t props;
      if (auto read = get_all(user, systemd, path, k_systemd_unit, budget, "the user unit " + name, props); !read.ok()) return read;
      auto *invocation = props.typed("InvocationID", "ay");
      if (!invocation || !props.get("ActiveState", out.active_state) || !props.get("PartOf", out.part_of) ||
          !props.get("BindsTo", out.binds_to) || !props.get("Requisite", out.requisite) ||
          !props.get("StopPropagatedFrom", out.stop_propagated_from)) {
        return malformed("Unit properties of " + name);
      }
      gsize length = 0;
      const auto *bytes = static_cast<const std::uint8_t *>(g_variant_get_fixed_array(invocation, &length, 1));
      out.invocation = invocation_hex(std::vector<std::uint8_t>(bytes, bytes + length));
      if (service_cgroup) {
        props_t service;
        if (auto read = get_all(user, systemd, path, k_systemd_service, budget, "the service " + name, service); !read.ok()) return read;
        if (!service.get("ControlGroup", out.control_group)) return malformed("ControlGroup of " + name);
      }
      out.loaded = true;
      return verdict_t::pass();
    }

    /// Every unit a stop of graphical-session.target reaches, within k_max_closure_units.
    verdict_t read_stop_closure(client_t &user, const std::string &systemd, const budget_t &budget, stop_closure_t &out) {
      out = {};
      std::set<std::string> seen {std::string {k_graphical_target}};
      std::vector<std::string> queue {std::string {k_graphical_target}};
      while (!queue.empty()) {
        const auto name = queue.back();
        queue.pop_back();
        std::string path;
        if (auto found = unit_path(user, systemd, name, budget, path); !found.ok()) return found;
        if (path.empty()) continue;  // not loaded: nothing of it to stop
        props_t props;
        if (auto read = get_all(user, systemd, path, k_systemd_unit, budget, "the user unit " + name, props); !read.ok()) return read;
        for (const auto *edge : {"RequiredBy", "ConsistsOf", "BoundBy", "RequisiteOf", "PropagatesStopTo"}) {
          std::vector<std::string> next;
          if (!props.get(edge, next)) return malformed(std::string {edge} + " of " + name);
          for (auto &unit : next) {
            if (!seen.insert(unit).second) continue;
            if (seen.size() > k_max_closure_units) return verdict_t::pass();  // unproven: complete stays false
            queue.push_back(std::move(unit));
          }
        }
      }
      out.units.assign(seen.begin(), seen.end());
      out.complete = true;
      return verdict_t::pass();
    }

    verdict_t read_active_seat(client_t &system, const std::string &logind, const budget_t &budget, std::string &id) {
      auto seat = system.call(logind.c_str(), k_login1_path, k_login1_manager, "GetSeat", g_variant_new("(s)", "seat0"), G_VARIANT_TYPE("(o)"), budget);
      if (!seat.ok()) return reply_failure(seat, "seat0");
      const gchar *seat_path = nullptr;
      g_variant_get(seat.value.get(), "(&o)", &seat_path);
      const std::string path = seat_path;
      auto active = system.call(logind.c_str(), path.c_str(), k_properties, "Get", g_variant_new("(ss)", k_login1_seat, "ActiveSession"),
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

    /// One complete pass over every record a snapshot holds, through the bound owners.
    verdict_t read_pass(client_t &system, const std::string &logind, client_t &user, const std::string &systemd, const sources_t &sources,
                        bool with_closure, const budget_t &budget, snapshot_t &snapshot) {
      if (auto seat = read_active_seat(system, logind, budget, snapshot.seat0_active_id); !seat.ok()) return seat;
      auto list = system.call(logind.c_str(), k_login1_path, k_login1_manager, "ListSessions", nullptr, G_VARIANT_TYPE("(a(susso))"), budget);
      if (!list.ok()) return reply_failure(list, "the login sessions");
      {
        variant_ptr rows {g_variant_get_child_value(list.value.get(), 0)};
        if (g_variant_n_children(rows.get()) > k_max_sessions) return malformed("too many login sessions");
        GVariantIter iter;
        const gchar *id = nullptr;
        const gchar *name = nullptr;
        const gchar *seat = nullptr;
        const gchar *path = nullptr;
        guint32 uid = 0;
        g_variant_iter_init(&iter, rows.get());
        std::set<std::string> ids;
        while (g_variant_iter_next(&iter, "(&su&s&s&o)", &id, &uid, &name, &seat, &path)) {
          if (!ids.insert(id).second) return malformed("a session listed twice");
          snapshot.rows.push_back({id, uid, name, seat, path});
        }
      }
      // Records for this account's sessions and for the seat's active one, whoever owns it.
      for (const auto &row : snapshot.rows) {
        if (row.uid != sources.uid && row.id != snapshot.seat0_active_id) continue;
        session_record_t record;
        if (auto read = read_session(system, logind, row.path, budget, record); !read.ok()) return read;
        if (record.id != row.id) return malformed("Session Id differs from its listing");
        snapshot.records[row.id] = record;
      }
      if (const auto active = snapshot.records.find(snapshot.seat0_active_id); active != snapshot.records.end()) {
        snapshot.leader_start_ticks = process_start_ticks(sources.proc_root, active->second.leader).value_or(0);
      }
      if (auto read = read_unit(user, systemd, std::string {k_graphical_target}, false, budget, snapshot.target); !read.ok()) return read;
      if (auto read = read_unit(user, systemd, std::string {k_game_mode_unit}, false, budget, snapshot.game_mode); !read.ok()) return read;
      if (auto read = read_unit(user, systemd, std::string {k_polaris_unit}, true, budget, snapshot.polaris); !read.ok()) return read;
      if (with_closure) {
        if (auto read = read_stop_closure(user, systemd, budget, snapshot.stop_closure); !read.ok()) return read;
      }
      return verdict_t::pass();
    }

    std::string trimmed_boot_id(const sources_t &sources) {
      auto boot = read_text_limited(sources.boot_id, 64).value_or("");
      while (!boot.empty() && (boot.back() == '\n' || boot.back() == ' ')) boot.pop_back();
      return boot;
    }

    endpoint_t user_endpoint(const sources_t &sources) {
      return sources.user.address.empty() ? endpoint_t {user_bus_address(sources.uid), true} : sources.user;
    }

    verdict_t connect_failure(const budget_t &budget, std::string_view which, const std::string &error, std::string_view code) {
      if (budget.stop()) {
        return budget.expired() ? verdict_t::fail("prelaunch_session_read_timeout", "The " + std::string {which} + " did not answer in time.") :
                                  verdict_t::fail("prelaunch_cancelled", "The launch was cancelled.");
      }
      return verdict_t::fail(std::string {code}, "The " + std::string {which} + " could not be reached: " + error);
    }
  }  // namespace

  client_t::~client_t() {
    if (connection_) {
      // Closed through GDBus's own worker before it is released, so the socket is never finalized
      // here while that worker still polls it. Closing needs nothing from the peer.
      g_dbus_connection_close_sync(connection_, nullptr, nullptr);
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

  verdict_t introspection_supports_switch(std::string_view xml) {
    const auto unsupported = [](std::string why) {
      return verdict_t::fail("prelaunch_session_management_unavailable", "SteamOS Manager does not offer session switching here: " + std::move(why));
    };
    // The real D-Bus introspection grammar (GDBus's parser): comments, malformed markup and text
    // that merely mentions a name prove nothing.
    GError *error = nullptr;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(std::string {xml}.c_str(), &error);
    if (!node) {
      const std::string why = error && error->message ? error->message : "malformed introspection";
      if (error) g_error_free(error);
      return unsupported(why);
    }
    const GDBusInterfaceInfo *found = nullptr;
    int interfaces = 0;
    for (auto **interface = node->interfaces; interface && *interface; ++interface) {
      if (std::string_view {(*interface)->name} == k_session_management) {
        ++interfaces;
        found = *interface;
      }
    }
    auto verdict = verdict_t::pass();
    if (interfaces != 1) {
      verdict = unsupported(interfaces == 0 ? "no SessionManagement1 interface" : "SessionManagement1 is declared more than once");
    } else {
      for (const std::string_view wanted : {"SwitchToDesktopMode", "SwitchToGameMode"}) {
        int methods = 0;
        bool argumentless = true;
        for (auto **method = found->methods; method && *method; ++method) {
          if (wanted != (*method)->name) continue;
          ++methods;
          argumentless = argumentless && (!(*method)->in_args || !(*method)->in_args[0]) && (!(*method)->out_args || !(*method)->out_args[0]);
        }
        if (methods != 1 || !argumentless) {
          verdict = unsupported(std::string {wanted} + (methods == 0 ? " is missing" : methods > 1 ? " is declared more than once" : " takes arguments"));
          break;
        }
      }
    }
    g_dbus_node_info_unref(node);
    return verdict;
  }

  verdict_t read_snapshot(const sources_t &sources, const budget_t &budget, snapshot_t &out, bool with_closure) {
    const auto boot_before = trimmed_boot_id(sources);
    std::string error;
    auto system = client_t::connect(sources.system, budget, error);
    if (!system) return connect_failure(budget, "system bus", error, "prelaunch_session_identity_unavailable");
    owner_t logind;
    if (auto bound = bind_owner(*system, k_login1, 0, sources.proc_root, cgroup_is_logind, budget, "logind", "prelaunch_session_identity_unavailable", logind);
        !bound.ok()) {
      return bound;
    }
    if (sources.user.address.empty()) {
      if (auto bus = user_bus_trusted(sources.runtime_root / std::to_string(sources.uid), sources.uid); !bus.ok()) return bus;
    }
    auto user = client_t::connect(user_endpoint(sources), budget, error);
    if (!user) return connect_failure(budget, "user bus", error, "prelaunch_session_identity_unavailable");
    owner_t manager;
    const auto uid = sources.uid;
    if (auto bound = bind_owner(*user, k_systemd, uid, sources.proc_root, [uid](std::string_view cgroup) { return cgroup_is_user_manager(cgroup, uid); },
          budget, "this account's systemd user manager", "prelaunch_session_identity_unavailable", manager);
        !bound.ok()) {
      return bound;
    }

    // Two complete passes. Anything a pass holds that differs in the other (the active session, the
    // listing, any record, a leader's start, any unit's state or generation) is a refusal.
    snapshot_t first;
    snapshot_t second;
    first.uid = second.uid = sources.uid;
    first.boot_id = second.boot_id = boot_before;
    if (auto read = read_pass(*system, logind.unique, *user, manager.unique, sources, with_closure, budget, first); !read.ok()) return read;
    if (auto read = read_pass(*system, logind.unique, *user, manager.unique, sources, with_closure, budget, second); !read.ok()) return read;
    if (!(first == second)) return changed("sessions or units");
    if (auto same = owner_unchanged(*system, k_login1, logind, sources.proc_root, budget, "logind"); !same.ok()) return same;
    if (auto same = owner_unchanged(*user, k_systemd, manager, sources.proc_root, budget, "user manager"); !same.ok()) return same;
    if (trimmed_boot_id(sources) != boot_before) return changed("boot");
    second.self_cgroup = read_text_limited(sources.self_cgroup, 4096).value_or("");
    out = std::move(second);
    return verdict_t::pass();
  }

  verdict_t probe_manager(const sources_t &sources, const budget_t &budget) {
    if (sources.user.address.empty()) {
      if (auto bus = user_bus_trusted(sources.runtime_root / std::to_string(sources.uid), sources.uid); !bus.ok()) return bus;
    }
    std::string error;
    auto user = client_t::connect(user_endpoint(sources), budget, error);
    if (!user) return connect_failure(budget, "user bus", error, "prelaunch_session_management_unavailable");
    const std::string name {k_manager_name};
    const auto uid = sources.uid;
    owner_t manager;
    if (auto bound = bind_owner(*user, name.c_str(), uid, sources.proc_root, [uid](std::string_view cgroup) { return cgroup_is_manager_unit(cgroup, uid); },
          budget, "SteamOS Manager", "prelaunch_session_management_unavailable", manager);
        !bound.ok()) {
      return bound;
    }
    // The process's executable link is set by the kernel at exec, never by the process.
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink(sources.proc_root / std::to_string(manager.pid) / "exe", ec);
    if (ec || exe != std::filesystem::path {std::string {k_manager_executable}} || process_start_ticks(sources.proc_root, manager.pid) != manager.start_ticks) {
      return verdict_t::fail("prelaunch_session_management_unavailable", "The process owning SteamOS Manager's name is not the packaged steamos-manager.");
    }
    // Introspect the unique name, not the well-known one: it cannot activate anything.
    auto xml = user->call(manager.unique.c_str(), std::string {k_manager_path}.c_str(), "org.freedesktop.DBus.Introspectable", "Introspect", nullptr,
      G_VARIANT_TYPE("(s)"), budget);
    if (!xml.ok()) return reply_failure(xml, "SteamOS Manager's interfaces");
    const gchar *text = nullptr;
    g_variant_get(xml.value.get(), "(&s)", &text);
    if (auto supported = introspection_supports_switch(text); !supported.ok()) return supported;
    // The well-known name the command will call is still that same connection and process.
    if (auto same = owner_unchanged(*user, name.c_str(), manager, sources.proc_root, budget, "SteamOS Manager"); !same.ok()) {
      return verdict_t::fail("prelaunch_session_management_unavailable", same.reason);
    }
    return verdict_t::pass();
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
