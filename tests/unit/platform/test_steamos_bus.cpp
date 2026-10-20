/**
 * @file tests/unit/platform/test_steamos_bus.cpp
 * @brief #204: the GIO reader, the SteamOS adapter and the coordinator over a fake bus and child.
 *
 * One in-process GDBus peer server stands in for logind, the user manager, the bus daemon and
 * SteamOS Manager, answering from a scripted host. The "steamosctl" is a /bin/sh fixture that only
 * moves that script. Nothing touches the machine's buses, sessions or Steam. Passing here is not
 * hardware acceptance: it shows what the code does with the records upstream documents.
 */
#include <src/owned_child.h>
#include <src/platform/linux/steamos_adapter.h>
#include <src/platform/linux/steamos_bus.h>
#include <src/prelaunch_transition.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace std::literals;
namespace steamos = prelaunch::steamos;
namespace bus = prelaunch::steamos::bus;
using prelaunch::restore_e;
using prelaunch::state_e;

namespace {
  constexpr const char *k_boot = "6c1b5f3e-2a4d-4c8e-9f10-0123456789ab";
  constexpr std::uint32_t k_manager_pid = 4242;
  constexpr const char *k_polaris_cgroup = "/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service";

  struct scratch_t {
    std::filesystem::path root;

    scratch_t() {
      std::random_device device;
      root = std::filesystem::temp_directory_path() / ("pl-sb-" + std::to_string(device() % 100000000));
      std::filesystem::create_directories(root);
    }

    ~scratch_t() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path write(const std::filesystem::path &relative, const std::string &text) const {
      const auto path = root / relative;
      std::filesystem::create_directories(path.parent_path());
      std::ofstream {path, std::ios::binary} << text;
      return path;
    }

    std::string read(const std::filesystem::path &relative) const {
      std::ifstream input {root / relative};
      std::stringstream text;
      text << input.rdbuf();
      return text.str();
    }
  };

  std::vector<std::uint8_t> invocation(std::uint8_t seed) {
    std::vector<std::uint8_t> bytes(16);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(seed + i);
    return bytes;
  }

  std::string stat_line(std::uint32_t pid, std::uint64_t ticks) {
    std::string stat = std::to_string(pid) + " (leader) S 1";
    for (int field = 5; field <= 21; ++field) stat += " 0";
    return stat + " " + std::to_string(ticks) + " 0 0\n";
  }

  /// The host the fake bus answers from. Every field is read under the fake's mutex.
  struct host_script_t {
    struct session_t {
      std::string id;
      std::uint32_t uid = 1000;
      std::uint32_t leader = 0;
      std::uint64_t started = 0;
      std::string service = "sddm-autologin";
      std::string state = "active";
    };

    struct unit_t {
      bool loaded = true;
      std::string active_state = "active";
      std::vector<std::uint8_t> invocation;
      std::vector<std::string> part_of;
      std::string control_group;
      /// systemd's reverse stop edges, as the Unit interface reports them.
      std::vector<std::string> required_by;
      std::vector<std::string> consists_of;
      std::vector<std::string> bound_by;
      std::vector<std::string> requisite_of;
      std::vector<std::string> propagates_stop_to;
    };

    /// A bus name's owner: its unique name, account and process.
    struct owner_t {
      std::string unique;
      std::uint32_t uid = 0;
      std::uint32_t pid = 0;
    };

    std::uint32_t uid = 1000;
    std::vector<session_t> sessions;
    std::string active;
    std::map<std::string, unit_t> units;
    owner_t logind {":1.1", 0, 11};
    owner_t systemd {":1.2", 1000, 22};
    std::string manager_owner = ":1.42";
    std::uint32_t manager_uid = 1000;

    unit_t &target() {
      return units["graphical-session.target"];
    }

    unit_t &game_mode() {
      return units["gamescope-session.service"];
    }

    unit_t &polaris() {
      return units["polaris.service"];
    }
    std::string manager_xml = R"(<node><interface name="com.steampowered.SteamOSManager1.SessionManagement1">)"
                              R"(<method name="SwitchToDesktopMode"/><method name="SwitchToGameMode"/></interface></node>)";
    /// Methods that are received and never answered.
    std::set<std::string> stalled;
    /// Replaces a reply's body when it returns non-null (a floating GVariant): malformed shapes.
    std::function<GVariant *(const std::string &path, const std::string &method)> override_body;
    /// Called for every method received, outside the fake's lock.
    std::function<void(const std::string &method)> on_call;
  };

  /// A GDBus peer server on its own thread and main context.
  class fake_bus_t {
  public:
    explicit fake_bus_t(const std::filesystem::path &socket) {
      address_ = "unix:path=" + socket.string();
      std::promise<void> started;
      auto ready = started.get_future();
      context_ = g_main_context_new();
      loop_ = g_main_loop_new(context_, FALSE);
      thread_ = std::thread([this, &started]() {
        g_main_context_push_thread_default(context_);
        gchar *guid = g_dbus_generate_guid();
        GError *error = nullptr;
        server_ = g_dbus_server_new_sync(address_.c_str(), G_DBUS_SERVER_FLAGS_NONE, guid, nullptr, nullptr, &error);
        g_free(guid);
        if (!server_) {
          ADD_FAILURE() << "fake bus: " << (error ? error->message : "?");
          if (error) g_error_free(error);
          started.set_value();
          g_main_context_pop_thread_default(context_);
          return;
        }
        g_signal_connect(server_, "new-connection", G_CALLBACK(on_new_connection), this);
        g_dbus_server_start(server_);
        started.set_value();
        g_main_loop_run(loop_);
        g_dbus_server_stop(server_);
        g_object_unref(server_);
        g_main_context_pop_thread_default(context_);
      });
      ready.wait();
    }

    ~fake_bus_t() {
      g_main_loop_quit(loop_);
      thread_.join();
      std::vector<GDBusConnection *> connections;
      {
        std::lock_guard lock {mutex_};
        connections.swap(connections_);
      }
      for (auto *connection : connections) {
        g_dbus_connection_close_sync(connection, nullptr, nullptr);
        g_object_unref(connection);
      }
      g_main_loop_unref(loop_);
      g_main_context_unref(context_);
    }

    std::string address() const {
      return address_;
    }

    template<class F>
    void edit(F &&change) {
      std::lock_guard lock {mutex_};
      change(script_);
    }

    std::vector<std::string> methods() {
      std::lock_guard lock {mutex_};
      return methods_;
    }

    /// Every call carried NO_AUTO_START: reading never starts a service.
    bool all_no_auto_start() {
      std::lock_guard lock {mutex_};
      return !methods_.empty() && auto_start_calls_ == 0;
    }

  private:
    static gboolean on_new_connection(GDBusServer *, GDBusConnection *connection, gpointer data) {
      auto *self = static_cast<fake_bus_t *>(data);
      g_object_ref(connection);
      {
        std::lock_guard lock {self->mutex_};
        self->connections_.push_back(connection);
      }
      g_dbus_connection_add_filter(connection, on_message, self, nullptr);
      return TRUE;
    }

    static GDBusMessage *on_message(GDBusConnection *connection, GDBusMessage *message, gboolean incoming, gpointer data) {
      if (!incoming || g_dbus_message_get_message_type(message) != G_DBUS_MESSAGE_TYPE_METHOD_CALL) return message;
      static_cast<fake_bus_t *>(data)->answer(connection, message);
      g_object_unref(message);
      return nullptr;
    }

    void answer(GDBusConnection *connection, GDBusMessage *message) {
      const std::string path = g_dbus_message_get_path(message) ? g_dbus_message_get_path(message) : "";
      const std::string method = g_dbus_message_get_member(message) ? g_dbus_message_get_member(message) : "";
      std::function<void(const std::string &)> hook;
      {
        std::lock_guard lock {mutex_};
        methods_.push_back(method);
        if (!(g_dbus_message_get_flags(message) & G_DBUS_MESSAGE_FLAGS_NO_AUTO_START)) ++auto_start_calls_;
        hook = script_.on_call;
      }
      if (hook) hook(method);
      GVariant *body = nullptr;
      std::string error_name;
      {
        std::lock_guard lock {mutex_};
        if (script_.stalled.contains(method)) return;
        if (script_.override_body) body = script_.override_body(path, method);
        if (!body) body = reply_body(path, method, g_dbus_message_get_body(message), error_name);
      }
      GDBusMessage *reply = error_name.empty() ? g_dbus_message_new_method_reply(message) :
                                                 g_dbus_message_new_method_error(message, error_name.c_str(), "%s", "fake");
      if (body) g_dbus_message_set_body(reply, body);
      g_dbus_connection_send_message(connection, reply, G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr, nullptr);
      g_object_unref(reply);
    }

    static GVariant *strv(const std::vector<std::string> &items) {
      std::vector<const gchar *> pointers;
      for (const auto &item : items) pointers.push_back(item.c_str());
      return g_variant_new_strv(pointers.data(), static_cast<gssize>(pointers.size()));
    }

    static GVariant *unit_props(const host_script_t::unit_t &unit) {
      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&builder, "{sv}", "ActiveState", g_variant_new_string(unit.active_state.c_str()));
      g_variant_builder_add(&builder, "{sv}", "InvocationID",
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, unit.invocation.data(), unit.invocation.size(), 1));
      g_variant_builder_add(&builder, "{sv}", "PartOf", strv(unit.part_of));
      g_variant_builder_add(&builder, "{sv}", "BindsTo", strv({}));
      g_variant_builder_add(&builder, "{sv}", "Requisite", strv({}));
      g_variant_builder_add(&builder, "{sv}", "StopPropagatedFrom", strv({}));
      g_variant_builder_add(&builder, "{sv}", "RequiredBy", strv(unit.required_by));
      g_variant_builder_add(&builder, "{sv}", "ConsistsOf", strv(unit.consists_of));
      g_variant_builder_add(&builder, "{sv}", "BoundBy", strv(unit.bound_by));
      g_variant_builder_add(&builder, "{sv}", "RequisiteOf", strv(unit.requisite_of));
      g_variant_builder_add(&builder, "{sv}", "PropagatesStopTo", strv(unit.propagates_stop_to));
      return g_variant_new("(a{sv})", &builder);
    }

    /// Unit object paths by position in the (ordered) unit map.
    std::string unit_object(const std::string &name) const {
      std::size_t index = 0;
      for (const auto &[unit, _] : script_.units) {
        if (unit == name) break;
        ++index;
      }
      return "/org/freedesktop/systemd1/unit/u" + std::to_string(index);
    }

    /// The scripted host's answer. Under the lock.
    GVariant *reply_body(const std::string &path, const std::string &method, GVariant *args, std::string &error_name) {
      const auto &s = script_;
      const auto arg_string = [&](int index) {
        if (!args || g_variant_n_children(args) <= static_cast<gsize>(index)) return std::string {};
        GVariant *child = g_variant_get_child_value(args, index);
        std::string out = g_variant_is_of_type(child, G_VARIANT_TYPE_STRING) ? g_variant_get_string(child, nullptr) : "";
        g_variant_unref(child);
        return out;
      };
      if (method == "GetNameOwner") {
        const auto name = arg_string(0);
        const auto &owner = name == "org.freedesktop.login1" ? s.logind.unique : name == "org.freedesktop.systemd1" ? s.systemd.unique : s.manager_owner;
        if (owner.empty()) {
          error_name = "org.freedesktop.DBus.Error.NameHasNoOwner";
          return nullptr;
        }
        return g_variant_new("(s)", owner.c_str());
      }
      if (method == "GetConnectionUnixUser" || method == "GetConnectionUnixProcessID") {
        const auto unique = arg_string(0);
        const bool user = method == "GetConnectionUnixUser";
        if (unique == s.logind.unique) return g_variant_new("(u)", user ? s.logind.uid : s.logind.pid);
        if (unique == s.systemd.unique) return g_variant_new("(u)", user ? s.systemd.uid : s.systemd.pid);
        if (unique == s.manager_owner) return g_variant_new("(u)", user ? s.manager_uid : k_manager_pid);
        error_name = "org.freedesktop.DBus.Error.NameHasNoOwner";
        return nullptr;
      }
      if (method == "Introspect") return g_variant_new("(s)", s.manager_xml.c_str());
      if (method == "GetSeat") return g_variant_new("(o)", "/org/freedesktop/login1/seat/seat0");
      if (method == "ListSessions") {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a(susso)"));
        for (const auto &session : s.sessions) {
          const auto object = "/org/freedesktop/login1/session/_" + session.id;
          g_variant_builder_add(&builder, "(susso)", session.id.c_str(), session.uid, "deck", "seat0", object.c_str());
        }
        return g_variant_new("(a(susso))", &builder);
      }
      if (method == "Get" && path == "/org/freedesktop/login1/seat/seat0") {
        const auto object = "/org/freedesktop/login1/session/_" + s.active;
        return g_variant_new("(v)", g_variant_new("(so)", s.active.c_str(), object.c_str()));
      }
      if (method == "GetUnit") {
        const auto name = arg_string(0);
        const auto found = s.units.find(name);
        if (found == s.units.end() || !found->second.loaded) {
          error_name = "org.freedesktop.systemd1.NoSuchUnit";
          return nullptr;
        }
        return g_variant_new("(o)", unit_object(name).c_str());
      }
      if (method == "GetAll" && path.starts_with("/org/freedesktop/systemd1/unit/")) {
        for (const auto &[name, unit] : s.units) {
          if (unit_object(name) != path) continue;
          if (arg_string(0) == "org.freedesktop.systemd1.Service") {
            GVariantBuilder builder;
            g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&builder, "{sv}", "ControlGroup", g_variant_new_string(unit.control_group.c_str()));
            return g_variant_new("(a{sv})", &builder);
          }
          return unit_props(unit);
        }
        error_name = "org.freedesktop.DBus.Error.UnknownObject";
        return nullptr;
      }
      if (method == "GetAll" && path.starts_with("/org/freedesktop/login1/session/_")) {
        const auto id = path.substr(std::string {"/org/freedesktop/login1/session/_"}.size());
        for (const auto &session : s.sessions) {
          if (session.id != id) continue;
          GVariantBuilder builder;
          g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
          g_variant_builder_add(&builder, "{sv}", "Id", g_variant_new_string(session.id.c_str()));
          g_variant_builder_add(&builder, "{sv}", "User", g_variant_new("(uo)", session.uid, "/org/freedesktop/login1/user/_1000"));
          g_variant_builder_add(&builder, "{sv}", "Seat", g_variant_new("(so)", "seat0", "/org/freedesktop/login1/seat/seat0"));
          g_variant_builder_add(&builder, "{sv}", "Type", g_variant_new_string("wayland"));
          g_variant_builder_add(&builder, "{sv}", "Class", g_variant_new_string("user"));
          g_variant_builder_add(&builder, "{sv}", "State", g_variant_new_string(session.state.c_str()));
          g_variant_builder_add(&builder, "{sv}", "Service", g_variant_new_string(session.service.c_str()));
          g_variant_builder_add(&builder, "{sv}", "Active", g_variant_new_boolean(session.id == s.active));
          g_variant_builder_add(&builder, "{sv}", "Remote", g_variant_new_boolean(FALSE));
          g_variant_builder_add(&builder, "{sv}", "Leader", g_variant_new_uint32(session.leader));
          g_variant_builder_add(&builder, "{sv}", "TimestampMonotonic", g_variant_new_uint64(session.started));
          return g_variant_new("(a{sv})", &builder);
        }
        error_name = "org.freedesktop.DBus.Error.UnknownObject";
        return nullptr;
      }
      error_name = "org.freedesktop.DBus.Error.UnknownMethod";
      return nullptr;
    }

    std::string address_;
    GMainContext *context_ = nullptr;
    GMainLoop *loop_ = nullptr;
    GDBusServer *server_ = nullptr;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<GDBusConnection *> connections_;
    host_script_t script_;
    std::vector<std::string> methods_;
    int auto_start_calls_ = 0;
  };

  /// A host in Game Mode, its fixture files and a fake bus answering for it.
  struct world_t {
    scratch_t scratch;
    std::unique_ptr<fake_bus_t> fake;
    bus::sources_t sources;
    std::uint32_t next_session = 3;
    std::uint8_t next_invocation = 0x40;

    world_t() {
      fake = std::make_unique<fake_bus_t>(scratch.root / "bus");
      sources.system = {fake->address(), false};
      sources.user = {fake->address(), false};
      sources.uid = 1000;
      sources.proc_root = scratch.root / "proc";
      sources.boot_id = scratch.write("boot_id", std::string {k_boot} + "\n");
      sources.runtime_root = scratch.root / "run";
      sources.pacman_local = scratch.root / "pacman";
      sources.trusted_owner_uid = static_cast<std::uint32_t>(::getuid());
      sources.trusted_root = scratch.root / "root";
      sources.self_cgroup = scratch.write("self_cgroup", "0::" + std::string {k_polaris_cgroup} + "\n");
      // logind and this account's user manager, where systemd places them.
      scratch.write("proc/11/stat", stat_line(11, 5));
      scratch.write("proc/11/cgroup", "0::/system.slice/systemd-logind.service\n");
      scratch.write("proc/22/stat", stat_line(22, 6));
      scratch.write("proc/22/cgroup", "0::/user.slice/user-1000.slice/user@1000.service/init.scope\n");
      scratch.write("pacman/steamos-manager-26.4.1-1/desc", "%NAME%\nsteamos-manager\n\n%VERSION%\n26.4.1-1\n\n");
      scratch.write("pacman/steamos-manager-26.4.1-1/files", "%FILES%\nusr/bin/steamosctl\nusr/lib/steamos-manager\n\n");
      // The manager process: this account's unit, the packaged executable.
      scratch.write("proc/4242/stat", stat_line(k_manager_pid, 99));
      scratch.write("proc/4242/cgroup", "0::/user.slice/user-1000.slice/user@1000.service/app.slice/steamos-manager.service\n");
      std::filesystem::create_symlink("/usr/lib/steamos-manager", scratch.root / "proc/4242/exe");
      install_command(R"(
case "$1" in
  switch-to-desktop-mode) echo desktop > "$STATE_FILE" ;;
  switch-to-game-mode) echo game > "$STATE_FILE" ;;
esac
exit 0
)");
      fake->edit([&](host_script_t &s) {
        s.sessions = {{"2", 1000, 1234, 5000000}};
        s.active = "2";
        s.target().invocation = invocation(0x10);
        s.target().consists_of = {"gamescope-session.service", "steam.service"};
        s.game_mode().invocation = invocation(0x20);
        s.game_mode().bound_by = {"steam.service"};
        s.polaris().invocation = invocation(0x30);
        s.polaris().control_group = k_polaris_cgroup;
        s.units["steam.service"].invocation = invocation(0x50);
      });
      scratch.write("proc/1234/stat", stat_line(1234, 777));
      // The fake steamosctl only writes the mode it was asked for; the fake bus moves the session.
      fake->edit([this](host_script_t &s) {
        s.on_call = [this](const std::string &method) {
          if (method == "GetSeat") apply_requested_mode();
        };
      });
    }

    /// The fixture "steamosctl": records its argv and environment, then runs `body`.
    void install_command(const std::string &body) {
      const auto state = (scratch.root / "requested").string();
      const auto argv = (scratch.root / "argv").string();
      const auto env = (scratch.root / "env").string();
      const auto path = scratch.write("root/usr/bin/steamosctl",
        "#!/bin/sh\nprintf '%s\\n' \"$0\" \"$@\" >> " + argv + "\n/usr/bin/env > " + env + "\nSTATE_FILE=" + state + "\n" + body);
      for (const auto &dir : {scratch.root / "root", scratch.root / "root/usr", scratch.root / "root/usr/bin"}) ::chmod(dir.c_str(), 0755);
      ::chmod(path.c_str(), 0755);
      sources.steamosctl = path;
    }

    /// What the real manager would do after the command: end the session, SDDM autologin starts the
    /// other one with a new target generation.
    void apply_requested_mode() {
      auto requested = scratch.read("requested");
      while (!requested.empty() && requested.back() == '\n') requested.pop_back();
      if (requested.empty()) return;
      std::filesystem::remove(scratch.root / "requested");
      const auto id = std::to_string(next_session++);
      const auto leader = 2000 + next_session;
      scratch.write("proc/" + std::to_string(leader) + "/stat", stat_line(leader, 1000 + next_session));
      const auto target = invocation(next_invocation += 0x10);
      fake->edit([&](host_script_t &s) {
        s.sessions = {{id, 1000, leader, 6000000ull + next_session}};
        s.active = id;
        s.target().invocation = target;
        s.game_mode().active_state = requested == "game" ? "active" : "inactive";
      });
    }

    bus::budget_t budget(std::chrono::milliseconds within = 3s, std::function<bool()> cancelled = {}) const {
      return {std::chrono::steady_clock::now() + within, std::move(cancelled)};
    }

    std::vector<std::string> command_lines() const {
      std::vector<std::string> lines;
      std::istringstream text {scratch.read("argv")};
      for (std::string line; std::getline(text, line);) lines.push_back(line);
      return lines;
    }
  };

  steamos::verdict_t read(world_t &world, steamos::snapshot_t &snapshot, std::chrono::milliseconds within = 3s) {
    return bus::read_snapshot(world.sources, world.budget(within), snapshot);
  }

  /// The production observation over the fake bus: every decision, Game Mode included, comes from
  /// the typed records. Only the open-title refusal and headless-boot readiness stand apart.
  class bus_host_t final: public prelaunch::host_t {
  public:
    explicit bus_host_t(world_t &world):
        world_ {world} {}

    prelaunch::observation_t observe(const steamos::session_identity_t *since, const prelaunch::wait_t &wait, bool prove_service) override {
      return steamos::observe(world_.sources, since, {wait.deadline, wait.cancelled}, prove_service);
    }

    bool local_title_running() override {
      return false;
    }

    bool service_independent(std::string &) override {
      return true;
    }

  private:
    world_t &world_;
  };

  class real_clock_t final: public prelaunch::clock_t {
  public:
    std::chrono::steady_clock::time_point now() override {
      return std::chrono::steady_clock::now();
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      std::this_thread::sleep_for(duration);
    }
  };

  prelaunch::budgets_t short_budgets() {
    prelaunch::budgets_t budgets;
    budgets.forward = 4s;
    budgets.rollback = 3s;
    budgets.restore = 3s;
    budgets.child = 1500ms;
    return budgets;
  }

  prelaunch::request_t launch_request() {
    prelaunch::request_t request;
    request.client_uuid = "client-a";
    request.lifecycle_generation = 7;
    request.policy_enabled = true;
    request.capture_ready = [](std::string &) {
      return true;
    };
    request.others_now = [](std::string &) {
      return false;
    };
    return request;
  }

  const prelaunch::coordinator_t::others_t nobody_else = [](std::string &) {
    return false;
  };

  /// The coordinator over the production observer and adapter, against the fake world.
  struct rig_t {
    world_t world;
    bus_host_t host {world};
    std::unique_ptr<steamos::steamos_adapter_t> adapter;
    real_clock_t clock;
    std::unique_ptr<prelaunch::coordinator_t> coordinator;

    rig_t() {
      adapter = std::make_unique<steamos::steamos_adapter_t>(world.sources);
      coordinator = std::make_unique<prelaunch::coordinator_t>(host, *adapter, clock, short_budgets());
    }

    /// Re-makes the adapter after the fixture command changed.
    void reload() {
      coordinator.reset();
      adapter = std::make_unique<steamos::steamos_adapter_t>(world.sources);
      coordinator = std::make_unique<prelaunch::coordinator_t>(host, *adapter, clock, short_budgets());
    }
  };
}  // namespace

TEST(SteamosBusTests, ATypedSnapshotBindsTheGameModeSessionWithoutStartingAnyService) {
  world_t world;
  steamos::snapshot_t snapshot;
  ASSERT_TRUE(read(world, snapshot).ok());
  EXPECT_EQ(snapshot.boot_id, k_boot);
  EXPECT_EQ(snapshot.seat0_active_id, "2");
  EXPECT_EQ(snapshot.leader_start_ticks, 777u);
  EXPECT_EQ(snapshot.target.invocation, "101112131415161718191a1b1c1d1e1f");
  EXPECT_EQ(snapshot.polaris.control_group, k_polaris_cgroup);
  steamos::session_identity_t identity;
  ASSERT_TRUE(steamos::bind_session(snapshot, identity).ok());
  EXPECT_EQ(identity.mode, steamos::mode_e::game);
  EXPECT_EQ(identity.logind_started, 5000000u);
  EXPECT_TRUE(world.fake->all_no_auto_start());

  world.fake->edit([](host_script_t &s) { s.game_mode().loaded = false; });
  ASSERT_TRUE(read(world, snapshot).ok());
  ASSERT_TRUE(steamos::bind_session(snapshot, identity).ok());
  EXPECT_EQ(identity.mode, steamos::mode_e::desktop) << "NoSuchUnit is a unit that is not loaded, not an error";
}

TEST(SteamosBusTests, AnyMalformedRecordIsRefusedNeverCoerced) {
  struct case_t {
    std::string name;
    std::string method;
    std::string path_prefix;
    std::function<GVariant *()> body;
    std::string code;
  };
  const auto session_without = [](const char *skip, const char *replace_key = nullptr, GVariant *replacement = nullptr) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    const std::vector<std::pair<const char *, GVariant *>> fields {
      {"Id", g_variant_new_string("2")},
      {"User", g_variant_new("(uo)", 1000u, "/org/freedesktop/login1/user/_1000")},
      {"Seat", g_variant_new("(so)", "seat0", "/org/freedesktop/login1/seat/seat0")},
      {"Type", g_variant_new_string("wayland")},
      {"Class", g_variant_new_string("user")},
      {"State", g_variant_new_string("active")},
      {"Service", g_variant_new_string("sddm-autologin")},
      {"Active", g_variant_new_boolean(TRUE)},
      {"Remote", g_variant_new_boolean(FALSE)},
      {"Leader", g_variant_new_uint32(1234)},
      {"TimestampMonotonic", g_variant_new_uint64(5000000)},
    };
    for (const auto &[key, value] : fields) {
      if (skip && std::string {key} == skip) {
        g_variant_unref(g_variant_ref_sink(value));
        continue;
      }
      if (replace_key && std::string {key} == replace_key) {
        g_variant_unref(g_variant_ref_sink(value));
        g_variant_builder_add(&builder, "{sv}", key, replacement);
        continue;
      }
      g_variant_builder_add(&builder, "{sv}", key, value);
    }
    return g_variant_new("(a{sv})", &builder);
  };
  const std::vector<case_t> cases {
    {"Leader as a string", "GetAll", "/org/freedesktop/login1/session/",
     [&]() { return session_without(nullptr, "Leader", g_variant_new_string("1234")); }, "prelaunch_session_record_malformed"},
    {"no Service", "GetAll", "/org/freedesktop/login1/session/", [&]() { return session_without("Service"); }, "prelaunch_session_record_malformed"},
    {"User without its object", "GetAll", "/org/freedesktop/login1/session/",
     [&]() { return session_without(nullptr, "User", g_variant_new_uint32(1000)); }, "prelaunch_session_record_malformed"},
    {"a different session's Id", "GetAll", "/org/freedesktop/login1/session/",
     [&]() { return session_without(nullptr, "Id", g_variant_new_string("9")); }, "prelaunch_session_record_malformed"},
    {"ListSessions of another signature", "ListSessions", "",
     []() { return g_variant_new("(as)", nullptr); }, "prelaunch_session_identity_unavailable"},
    {"ActiveSession without its object", "Get", "/org/freedesktop/login1/seat/",
     []() { return g_variant_new("(v)", g_variant_new_string("2")); }, "prelaunch_session_record_malformed"},
    {"a unit without an InvocationID", "GetAll", "/org/freedesktop/systemd1/unit/", []() {
       GVariantBuilder builder;
       g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
       g_variant_builder_add(&builder, "{sv}", "ActiveState", g_variant_new_string("active"));
       return g_variant_new("(a{sv})", &builder);
     }, "prelaunch_session_record_malformed"},
  };
  for (const auto &c : cases) {
    world_t world;
    world.fake->edit([&](host_script_t &s) {
      s.override_body = [&](const std::string &path, const std::string &method) -> GVariant * {
        return method == c.method && path.starts_with(c.path_prefix) ? c.body() : nullptr;
      };
    });
    steamos::snapshot_t snapshot;
    EXPECT_EQ(read(world, snapshot).code, c.code) << c.name;
  }
}

TEST(SteamosBusTests, AStalledPeerCostsAtMostTheDeadlineAndACancellationEndsTheWait) {
  world_t world;
  world.fake->edit([](host_script_t &s) { s.stalled = {"ListSessions"}; });
  steamos::snapshot_t snapshot;
  auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(read(world, snapshot, 300ms).code, "prelaunch_session_read_timeout");
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms);

  std::atomic<bool> cancelled {false};
  std::thread canceller([&]() {
    std::this_thread::sleep_for(150ms);
    cancelled = true;
  });
  start = std::chrono::steady_clock::now();
  const auto verdict = bus::read_snapshot(world.sources, world.budget(30s, [&]() { return cancelled.load(); }), snapshot);
  canceller.join();
  EXPECT_EQ(verdict.code, "prelaunch_cancelled");
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s) << "not the 30 s deadline";
}

TEST(SteamosBusTests, APeerThatNeverAuthenticatesCostsAtMostTheDeadline) {
  scratch_t scratch;
  const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(listener, 0);
  sockaddr_un address {};
  address.sun_family = AF_UNIX;
  const auto path = (scratch.root / "mute").string();
  std::copy(path.begin(), path.end(), address.sun_path);
  ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(listener, 4), 0);
  std::string error;
  const auto start = std::chrono::steady_clock::now();
  const auto client = bus::client_t::connect({"unix:path=" + path, false}, {std::chrono::steady_clock::now() + 300ms, {}}, error);
  EXPECT_FALSE(client);
  EXPECT_EQ(error, "timed out");
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms);
  ::close(listener);
}

TEST(SteamosBusTests, ASessionOrLeaderThatChangesWhileItIsReadIsRefused) {
  {
    world_t world;
    int seats = 0;
    world.fake->edit([&](host_script_t &s) {
      s.on_call = [&](const std::string &method) {
        // The second seat read, at the end of the bracket, sees another active session.
        if (method == "GetSeat" && ++seats == 2) world.fake->edit([](host_script_t &h) { h.active = "5"; });
      };
    });
    steamos::snapshot_t snapshot;
    const auto verdict = read(world, snapshot);
    EXPECT_EQ(verdict.code, "prelaunch_session_identity_unavailable");
    EXPECT_NE(verdict.reason.find("changed"), std::string::npos) << verdict.reason;
  }
  {
    world_t world;
    world.fake->edit([&](host_script_t &s) {
      s.on_call = [&](const std::string &method) {
        // A new process with the leader's pid: same pid, another start time.
        if (method == "GetUnit") world.scratch.write("proc/1234/stat", stat_line(1234, 888));
      };
    });
    steamos::snapshot_t snapshot;
    EXPECT_EQ(read(world, snapshot).code, "prelaunch_session_identity_unavailable");
  }
}

TEST(SteamosBusTests, TheManagerMustBeThisAccountsUnitRunningThePackagedExecutable) {
  struct case_t {
    std::string name;
    std::function<void(world_t &)> change;
    bool supported;
  };
  const std::vector<case_t> cases {
    {"as installed", [](world_t &) {}, true},
    {"not running", [](world_t &w) { w.fake->edit([](host_script_t &s) { s.manager_owner.clear(); }); }, false},
    {"owned by another account", [](world_t &w) { w.fake->edit([](host_script_t &s) { s.manager_uid = 0; }); }, false},
    {"in a login session's cgroup", [](world_t &w) {
       w.scratch.write("proc/4242/cgroup", "0::/user.slice/user-1000.slice/session-2.scope\n");
     }, false},
    {"another executable", [](world_t &w) {
       std::filesystem::remove(w.scratch.root / "proc/4242/exe");
       std::filesystem::create_symlink("/tmp/steamos-manager", w.scratch.root / "proc/4242/exe");
     }, false},
    {"its process unreadable", [](world_t &w) { std::filesystem::remove(w.scratch.root / "proc/4242/stat"); }, false},
    {"no session management", [](world_t &w) {
       w.fake->edit([](host_script_t &s) { s.manager_xml = "<node/>"; });
     }, false},
    {"an unaudited package", [](world_t &w) {
       std::filesystem::rename(w.scratch.root / "pacman/steamos-manager-26.4.1-1", w.scratch.root / "pacman/steamos-manager-26.5.0-1");
       w.scratch.write("pacman/steamos-manager-26.5.0-1/desc", "%NAME%\nsteamos-manager\n\n%VERSION%\n26.5.0-1\n\n");
     }, false},
    {"an untrusted command", [](world_t &w) { ::chmod(w.sources.steamosctl.c_str(), 0777); }, false},
  };
  for (const auto &c : cases) {
    world_t world;
    c.change(world);
    const auto verdict = bus::probe_support(world.sources, world.budget());
    EXPECT_EQ(verdict.ok(), c.supported) << c.name << ": " << verdict.code << " " << verdict.reason;
    if (!c.supported) EXPECT_FALSE(verdict.code.empty()) << c.name;
    EXPECT_TRUE(world.fake->methods().empty() || world.fake->all_no_auto_start()) << c.name;
  }
}

TEST(SteamosBusTests, TheAdapterRunsExactlyTheFixedVerbWithOnlyTheAccountsBus) {
  world_t world;
  steamos::steamos_adapter_t adapter {world.sources};
  ASSERT_TRUE(adapter.start(prelaunch::direction_e::to_desktop));
  const auto until = std::chrono::steady_clock::now() + 5s;
  auto step = adapter.poll();
  while (step == prelaunch::step_e::running && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(10ms);
    step = adapter.poll();
  }
  EXPECT_EQ(step, prelaunch::step_e::succeeded) << "exit 0: the request was sent, nothing more";
  EXPECT_EQ(world.command_lines(), (std::vector<std::string> {world.sources.steamosctl.string(), "switch-to-desktop-mode"}));
  std::istringstream env {world.scratch.read("env")};
  std::set<std::string> keys;
  for (std::string line; std::getline(env, line);) {
    keys.insert(line.substr(0, line.find('=')));
    if (line.starts_with("DBUS_SESSION_BUS_ADDRESS=")) EXPECT_EQ(line, "DBUS_SESSION_BUS_ADDRESS=" + world.fake->address());
  }
  for (const auto *ambient : {"DBUS_SYSTEM_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY", "HOME", "XDG_RUNTIME_DIR", "LD_PRELOAD"}) {
    EXPECT_FALSE(keys.contains(ambient)) << ambient;
  }
  EXPECT_TRUE(keys.contains("DBUS_SESSION_BUS_ADDRESS"));
  EXPECT_TRUE(keys.contains("PATH"));
}

TEST(SteamosBusTests, ALeaveIsProvedFromTheRecordsAndAPurposefulEndRestoresANewGameModeOutsideLocks) {
  rig_t rig;
  const auto outcome = rig.coordinator->run(launch_request());
  ASSERT_TRUE(outcome.proceed()) << outcome.code << ": " << outcome.message;
  EXPECT_EQ(outcome.state, state_e::ready);
  auto receipt = rig.coordinator->receipt();
  ASSERT_TRUE(receipt.pre && receipt.post);
  EXPECT_EQ(receipt.pre->logind_id, "2");
  EXPECT_EQ(receipt.pre->mode, steamos::mode_e::game);
  EXPECT_EQ(receipt.post->logind_id, "3");
  EXPECT_EQ(receipt.post->mode, steamos::mode_e::desktop);
  EXPECT_NE(receipt.post->target_invocation, receipt.pre->target_invocation);
  EXPECT_EQ(receipt.restore, restore_e::owned);
  EXPECT_FALSE(receipt.outcome_unknown);
  EXPECT_EQ(receipt.cancellation_transport, "unsupported") << "this request had no cancellation of its own";
  rig.coordinator->commit(outcome.transition_generation);

  // The caller ends the session holding its own lock; the restore needs that lock and must not run
  // under it, and no launch may be admitted while it settles.
  std::timed_mutex lifecycle;
  const auto caller = std::this_thread::get_id();
  std::atomic<bool> others_on_caller_thread {false};
  std::atomic<bool> others_locked {false};
  const prelaunch::coordinator_t::others_t others = [&](std::string &) {
    if (std::this_thread::get_id() == caller) {
      others_on_caller_thread = true;
      return true;
    }
    others_locked = lifecycle.try_lock_for(5s);
    if (others_locked) lifecycle.unlock();
    return false;
  };
  {
    std::lock_guard lock {lifecycle};
    EXPECT_EQ(rig.coordinator->on_session_end(outcome.transition_generation, true, false, others), restore_e::owned);
    EXPECT_TRUE(rig.coordinator->restore_pending());
    EXPECT_EQ(rig.coordinator->run(launch_request()).code, "prelaunch_transition_busy") << "the admission fence holds";
    std::this_thread::sleep_for(100ms);
  }
  rig.coordinator->wait_for_restore();
  EXPECT_FALSE(others_on_caller_thread);
  EXPECT_TRUE(others_locked) << "the restore took the caller's lock only after it was released";
  receipt = rig.coordinator->receipt();
  EXPECT_EQ(receipt.restore, restore_e::restored) << receipt.restore_reason;
  EXPECT_FALSE(rig.coordinator->restore_pending());
  EXPECT_EQ(rig.world.command_lines(), (std::vector<std::string> {
    rig.world.sources.steamosctl.string(), "switch-to-desktop-mode", rig.world.sources.steamosctl.string(), "switch-to-game-mode"}));
  steamos::snapshot_t snapshot;
  ASSERT_TRUE(read(rig.world, snapshot).ok());
  EXPECT_EQ(snapshot.seat0_active_id, "4") << "a new Game Mode session, not the old one";
  EXPECT_EQ(snapshot.game_mode.active_state, "active");
}

TEST(SteamosBusTests, APurposefulEndRechecksTheCurrentStopClosureBeforeAnyReturn) {
  rig_t rig;
  const auto outcome = rig.coordinator->run(launch_request());
  ASSERT_TRUE(outcome.proceed()) << outcome.code << ": " << outcome.message;
  rig.coordinator->commit(outcome.transition_generation);
  const auto commands = rig.world.command_lines();
  ASSERT_EQ(commands.size(), 2u);
  // The session still belongs to this launch, but a dependency added during
  // the stream makes the next graphical-session stop also end Polaris.
  rig.world.fake->edit([](host_script_t &s) {
    s.target().consists_of.push_back("session-helper.service");
    s.units["session-helper.service"].propagates_stop_to = {"polaris.service"};
  });
  ASSERT_EQ(rig.coordinator->on_session_end(outcome.transition_generation, true, false,
    [](std::string &) { return false; }), restore_e::owned);
  rig.coordinator->wait_for_restore();
  const auto receipt = rig.coordinator->receipt();
  EXPECT_EQ(receipt.restore, restore_e::refused) << receipt.restore_reason;
  EXPECT_NE(receipt.restore_reason.find("survive"), std::string::npos);
  EXPECT_EQ(rig.world.command_lines(), commands) << "a stale launch-time survival proof must not submit a return";
  EXPECT_FALSE(rig.coordinator->restore_pending());
}

TEST(SteamosBusTests, AnEarlyExitZeroWhileTheOldSessionPersistsIsNeverReadyAndNeverRolledBack) {
  rig_t rig;
  rig.world.install_command("exit 0\n");
  rig.reload();
  const auto outcome = rig.coordinator->run(launch_request());
  EXPECT_EQ(outcome.code, "prelaunch_old_session_timeout");
  EXPECT_EQ(outcome.state, state_e::failed);
  const auto receipt = rig.coordinator->receipt();
  EXPECT_EQ(receipt.restore, restore_e::refused);
  EXPECT_TRUE(receipt.outcome_unknown) << "a sent request whose effect was never bound";
  EXPECT_EQ(rig.world.command_lines().size(), 2u) << "only the switch; no return to Game Mode was attempted";
}

TEST(SteamosBusTests, AnAbortAfterTheRequestWasDeliveredIsOutcomeUnknownAndNotRolledBack) {
  rig_t rig;
  // Delivers its effect, then hangs past the child budget.
  rig.world.install_command(R"(
case "$1" in switch-to-desktop-mode) echo desktop > "$STATE_FILE" ;; esac
exec /bin/sleep 30
)");
  rig.reload();
  const auto start = std::chrono::steady_clock::now();
  const auto outcome = rig.coordinator->run(launch_request());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 3500ms) << "the child budget, not the sleep";
  EXPECT_EQ(outcome.code, "prelaunch_switch_outcome_unknown");
  const auto receipt = rig.coordinator->receipt();
  EXPECT_TRUE(receipt.outcome_unknown);
  EXPECT_EQ(receipt.restore, restore_e::refused);
  EXPECT_FALSE(receipt.post) << "the desktop it may have opened is never adopted";
  EXPECT_EQ(rig.world.command_lines().size(), 2u) << "no rollback of an unbound post-state";
}

TEST(SteamosBusTests, ACancelledSwitchStopsOnlyItsOwnChildAndIsOutcomeUnknown) {
  rig_t rig;
  rig.world.install_command("exec /bin/sleep 30\n");
  rig.reload();
  std::atomic<bool> cancelled {false};
  auto request = launch_request();
  request.cancelled = [&]() {
    return cancelled.load();
  };
  std::thread canceller([&]() {
    std::this_thread::sleep_for(300ms);
    cancelled = true;
  });
  const auto start = std::chrono::steady_clock::now();
  const auto outcome = rig.coordinator->run(request);
  canceller.join();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms);
  EXPECT_EQ(outcome.state, state_e::cancelled);
  EXPECT_EQ(outcome.code, "prelaunch_switch_outcome_unknown");
  EXPECT_EQ(rig.coordinator->receipt().cancellation_transport, "launch");
  EXPECT_TRUE(rig.coordinator->receipt().outcome_unknown);
}

TEST(SteamosBusTests, ATargetGenerationThatChangesBeforeTheHandOffIsNeverOwned) {
  rig_t rig;
  auto request = launch_request();
  request.capture_ready = [&](std::string &) {
    // Something restarted graphical-session.target after the desktop was bound.
    rig.world.fake->edit([](host_script_t &s) { s.target().invocation = invocation(0x77); });
    return true;
  };
  const auto outcome = rig.coordinator->run(request);
  EXPECT_EQ(outcome.code, "prelaunch_desktop_changed");
  const auto receipt = rig.coordinator->receipt();
  EXPECT_EQ(receipt.restore, restore_e::refused) << "that desktop is not this launch's to undo";
  EXPECT_EQ(rig.world.command_lines().size(), 2u);
}

TEST(SteamosBusTests, AnUnsupportedHostIsRefusedBeforeAnyCommandRuns) {
  rig_t rig;
  rig.world.fake->edit([](host_script_t &s) { s.manager_owner.clear(); });
  const auto outcome = rig.coordinator->run(launch_request());
  EXPECT_EQ(outcome.code, "prelaunch_session_management_unavailable");
  EXPECT_EQ(outcome.state, state_e::refused);
  EXPECT_TRUE(rig.world.command_lines().empty());
  const auto readiness = rig.coordinator->readiness(true);
  EXPECT_TRUE(readiness.adapter_implemented);
  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.code, "prelaunch_session_management_unavailable");
}

TEST(SteamosBusTests, AKeptReadinessAnswerNeverHidesAChangedManager) {
  rig_t rig;
  ASSERT_TRUE(rig.coordinator->readiness(true).ready);
  const auto introspections = [&]() {
    const auto methods = rig.world.fake->methods();
    return std::count(methods.begin(), methods.end(), "Introspect");
  };
  const auto probed = introspections();
  ASSERT_TRUE(rig.coordinator->readiness(true).ready);
  EXPECT_EQ(introspections(), probed) << "kept while nothing it rests on changed";
  // A replaced manager: another unique name, now another account's.
  rig.world.fake->edit([](host_script_t &s) {
    s.manager_owner = ":1.99";
    s.manager_uid = 0;
  });
  const auto changed = rig.coordinator->readiness(true);
  EXPECT_FALSE(changed.ready);
  EXPECT_EQ(changed.code, "prelaunch_session_management_unavailable");
  // Restored, the refusal still answers for a moment: it is never turned into a positive answer.
  rig.world.fake->edit([](host_script_t &s) {
    s.manager_owner = ":1.42";
    s.manager_uid = 1000;
  });
  EXPECT_FALSE(rig.coordinator->readiness(true).ready);
  std::this_thread::sleep_for(steamos::steamos_adapter_t::k_refusal_cache + 100ms);
  ASSERT_TRUE(rig.coordinator->readiness(true).ready) << "proved again once the refusal lapsed";
  // A removed command is seen at once, cache or not.
  std::filesystem::remove(rig.world.sources.steamosctl);
  EXPECT_EQ(rig.coordinator->readiness(true).code, "prelaunch_command_unavailable");
}

TEST(SteamosBusTests, ShutdownCancelsAStalledRestoreAndJoinsItWithoutRunningTheCommand) {
  rig_t rig;
  const auto outcome = rig.coordinator->run(launch_request());
  ASSERT_TRUE(outcome.proceed()) << outcome.code;
  rig.coordinator->commit(outcome.transition_generation);
  rig.world.fake->edit([](host_script_t &s) { s.stalled = {"ListSessions"}; });
  EXPECT_EQ(rig.coordinator->on_session_end(outcome.transition_generation, true, false, nobody_else), restore_e::owned);
  std::this_thread::sleep_for(100ms);
  const auto start = std::chrono::steady_clock::now();
  rig.coordinator->shutdown();
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms) << "cancelled, not the restore budget";
  const auto receipt = rig.coordinator->receipt();
  EXPECT_NE(receipt.restore, restore_e::restored);
  EXPECT_NE(receipt.restore, restore_e::owned);
  EXPECT_FALSE(rig.coordinator->restore_pending());
  EXPECT_EQ(rig.world.command_lines().size(), 2u) << "only the leave; the return never ran";
  EXPECT_EQ(rig.coordinator->run(launch_request()).code, "prelaunch_shutting_down");
}

TEST(SteamosBusTests, ASecondPassThatDiffersInAnyRecordIsRefused) {
  struct case_t {
    std::string name;
    std::function<void(world_t &)> change;  // applied as the second pass begins
  };
  const std::vector<case_t> cases {
    {"graphical-session.target restarted (seat and leader unchanged)", [](world_t &w) {
       w.fake->edit([](host_script_t &s) { s.target().invocation = invocation(0x66); });
     }},
    {"the logind record reused by a new session", [](world_t &w) {
       w.fake->edit([](host_script_t &s) { s.sessions[0].started += 1; });
     }},
    {"another graphical session of this account appeared", [](world_t &w) {
       w.scratch.write("proc/3000/stat", stat_line(3000, 4));
       w.fake->edit([](host_script_t &s) { s.sessions.push_back({"8", 1000, 3000, 7000000}); });
     }},
    {"Game Mode stopped", [](world_t &w) { w.fake->edit([](host_script_t &s) { s.game_mode().active_state = "deactivating"; }); }},
    {"polaris.service moved", [](world_t &w) { w.fake->edit([](host_script_t &s) { s.polaris().control_group = "/elsewhere"; }); }},
  };
  for (const auto &c : cases) {
    world_t world;
    int seats = 0;
    world.fake->edit([&](host_script_t &s) {
      s.on_call = [&](const std::string &method) {
        if (method == "GetSeat" && ++seats == 2) c.change(world);
      };
    });
    steamos::snapshot_t snapshot;
    const auto verdict = read(world, snapshot);
    EXPECT_EQ(verdict.code, "prelaunch_session_identity_unavailable") << c.name;
    EXPECT_NE(verdict.reason.find("changed while it was read"), std::string::npos) << c.name << ": " << verdict.reason;
  }
}

TEST(SteamosBusTests, LogindAndTheUserManagerAreBoundByOwnerAccountProcessAndPlacement) {
  struct case_t {
    std::string name;
    std::function<void(world_t &)> change;
  };
  const std::vector<case_t> cases {
    {"the user manager name owned by another account", [](world_t &w) { w.fake->edit([](host_script_t &s) { s.systemd.uid = 1001; }); }},
    {"the user manager name owned by an ordinary service", [](world_t &w) {
       w.scratch.write("proc/22/cgroup", "0::/user.slice/user-1000.slice/user@1000.service/app.slice/impostor.service\n");
     }},
    {"no user manager on the bus", [](world_t &w) { w.fake->edit([](host_script_t &s) { s.systemd.unique.clear(); }); }},
    {"the user manager's process unreadable", [](world_t &w) { std::filesystem::remove(w.scratch.root / "proc/22/stat"); }},
    {"logind owned by a user", [](world_t &w) { w.fake->edit([](host_script_t &s) { s.logind.uid = 1000; }); }},
    {"logind's name owned outside systemd-logind.service", [](world_t &w) {
       w.scratch.write("proc/11/cgroup", "0::/system.slice/other.service\n");
     }},
  };
  for (const auto &c : cases) {
    world_t world;
    c.change(world);
    steamos::snapshot_t snapshot;
    EXPECT_EQ(read(world, snapshot).code, "prelaunch_session_identity_unavailable") << c.name;
  }
  // A name that passes to another connection, or a process that restarts, during the read.
  for (const bool restart : {false, true}) {
    world_t world;
    int lists = 0;
    world.fake->edit([&](host_script_t &s) {
      s.on_call = [&](const std::string &method) {
        if (method != "ListSessions" || ++lists != 2) return;
        if (restart) {
          world.scratch.write("proc/22/stat", stat_line(22, 60));
        } else {
          world.fake->edit([](host_script_t &h) { h.systemd.unique = ":1.9"; });
        }
      };
    });
    steamos::snapshot_t snapshot;
    const auto verdict = read(world, snapshot);
    EXPECT_EQ(verdict.code, "prelaunch_session_identity_unavailable") << restart;
    EXPECT_NE(verdict.reason.find("owner changed"), std::string::npos) << verdict.reason;
  }
}

TEST(SteamosBusTests, ARepeatedPropertyIsRefusedWhateverItsTypes) {
  for (const bool same_type : {false, true}) {
    world_t world;
    world.fake->edit([&](host_script_t &s) {
      s.override_body = [same_type](const std::string &path, const std::string &method) -> GVariant * {
        if (method != "GetAll" || !path.starts_with("/org/freedesktop/login1/session/")) return nullptr;
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&builder, "{sv}", "Id", g_variant_new_string("2"));
        g_variant_builder_add(&builder, "{sv}", "Leader", g_variant_new_uint32(1234));
        g_variant_builder_add(&builder, "{sv}", "Leader", same_type ? g_variant_new_uint32(1) : g_variant_new_string("1234"));
        return g_variant_new("(a{sv})", &builder);
      };
    });
    steamos::snapshot_t snapshot;
    const auto verdict = read(world, snapshot);
    EXPECT_EQ(verdict.code, "prelaunch_session_record_malformed") << same_type;
    EXPECT_NE(verdict.reason.find("Leader"), std::string::npos) << verdict.reason;
  }
}

TEST(SteamosBusTests, IntrospectionIsParsedAsXmlNotSearchedAsText) {
  const std::string upstream = R"(<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd">
<node>
  <interface name="org.freedesktop.DBus.Introspectable"><method name="Introspect"><arg type="s" direction="out"/></method></interface>
  <interface name="com.steampowered.SteamOSManager1.SessionManagement1">
    <method name="SwitchToDesktopMode"><annotation name="org.freedesktop.DBus.Method.NoReply" value="false"/></method>
    <method name="SwitchToGameMode">
    </method>
    <property name="Version" type="u" access="read"/>
  </interface>
</node>)";
  EXPECT_TRUE(bus::introspection_supports_switch(upstream).ok());
  const std::vector<std::pair<std::string, std::string>> refused {
    {"only inside a comment", R"(<node><!-- <interface name="com.steampowered.SteamOSManager1.SessionManagement1">)"
                              R"(<method name="SwitchToDesktopMode"/><method name="SwitchToGameMode"/></interface> --></node>)"},
    {"malformed markup", R"(<node><interface name="com.steampowered.SteamOSManager1.SessionManagement1">)"
                         R"(<method name="SwitchToDesktopMode"/><method name="SwitchToGameMode"/></node>)"},
    {"in an attribute value", R"(<node><interface name="x.Other" doc='<interface name="com.steampowered.SteamOSManager1.SessionManagement1">)"
                              R"(<method name="SwitchToDesktopMode"/><method name="SwitchToGameMode"/>'/></node>)"},
    {"declared twice", R"(<node><interface name="com.steampowered.SteamOSManager1.SessionManagement1"><method name="SwitchToDesktopMode"/>)"
                       R"(<method name="SwitchToGameMode"/></interface><interface name="com.steampowered.SteamOSManager1.SessionManagement1">)"
                       R"(<method name="SwitchToDesktopMode"/><method name="SwitchToGameMode"/></interface></node>)"},
    {"a method declared twice", R"(<node><interface name="com.steampowered.SteamOSManager1.SessionManagement1"><method name="SwitchToDesktopMode"/>)"
                                R"(<method name="SwitchToDesktopMode"/><method name="SwitchToGameMode"/></interface></node>)"},
    {"an input argument", R"(<node><interface name="com.steampowered.SteamOSManager1.SessionManagement1"><method name="SwitchToDesktopMode">)"
                          R"(<arg type="s" direction="in"/></method><method name="SwitchToGameMode"/></interface></node>)"},
    {"an output argument", R"(<node><interface name="com.steampowered.SteamOSManager1.SessionManagement1"><method name="SwitchToDesktopMode"/>)"
                           R"(<method name="SwitchToGameMode"><arg type="b" direction="out"/></method></interface></node>)"},
    {"a missing method", R"(<node><interface name="com.steampowered.SteamOSManager1.SessionManagement1"><method name="SwitchToGameMode"/></interface></node>)"},
    {"on another interface", R"(<node><interface name="com.steampowered.SteamOSManager1.Manager"><method name="SwitchToDesktopMode"/>)"
                             R"(<method name="SwitchToGameMode"/></interface></node>)"},
    {"empty", ""},
  };
  for (const auto &[name, xml] : refused) {
    EXPECT_EQ(bus::introspection_supports_switch(xml).code, "prelaunch_session_management_unavailable") << name;
  }
  // The same through the manager probe: a comment never stands in for the interface.
  world_t world;
  world.fake->edit([&](host_script_t &s) { s.manager_xml = refused.front().second; });
  EXPECT_EQ(bus::probe_support(world.sources, world.budget()).code, "prelaunch_session_management_unavailable");
}

TEST(SteamosBusTests, TheManagerNameMustStillBeTheIntrospectedConnection) {
  world_t world;
  world.fake->edit([&](host_script_t &s) {
    s.on_call = [&](const std::string &method) {
      if (method == "Introspect") world.fake->edit([](host_script_t &h) { h.manager_owner = ":1.43"; });
    };
  });
  const auto verdict = bus::probe_support(world.sources, world.budget());
  EXPECT_EQ(verdict.code, "prelaunch_session_management_unavailable");
  EXPECT_NE(verdict.reason.find("owner changed"), std::string::npos) << verdict.reason;
}

TEST(SteamosBusTests, ProductionDatabaseLookupUsesTheSteamOSLayoutAndKeepsPackageGuards) {
  world_t world;
  // Populate the actual SteamOS layout independently of the production default. Mapping that
  // default under our owned fixture catches the missing-Arch-DB refusal seen on the real Deck.
  const auto db = world.scratch.root / "usr/lib/holo/pacmandb/local";
  std::filesystem::create_directories(db.parent_path());
  std::filesystem::rename(world.sources.pacman_local, db);
  world.sources.pacman_local = world.scratch.root / bus::sources_t {}.pacman_local.relative_path();
  ASSERT_FALSE(std::filesystem::exists(world.scratch.root / "var/lib/pacman/local"));
  const auto supported = bus::probe_support(world.sources, world.budget());
  ASSERT_TRUE(supported.ok()) << supported.code << ": " << supported.reason;
  EXPECT_TRUE(world.fake->all_no_auto_start());
  EXPECT_TRUE(world.command_lines().empty());

  const auto record = "usr/lib/holo/pacmandb/local/steamos-manager-26.4.1-1/"s;
  world.scratch.write(record + "files", "%FILES%\nusr/bin/steamosctl\n\n");
  EXPECT_EQ(bus::probe_support(world.sources, world.budget()).code, "prelaunch_adapter_package_malformed");
  world.scratch.write(record + "files", "%FILES%\nusr/bin/steamosctl\nusr/lib/steamos-manager\n\n");
  world.scratch.write(record + "desc", "%NAME%\nsteamos-manager\n\n%VERSION%\n26.4.2-1\n\n");
  std::filesystem::rename(db / "steamos-manager-26.4.1-1", db / "steamos-manager-26.4.2-1");
  EXPECT_EQ(bus::probe_support(world.sources, world.budget()).code, "prelaunch_adapter_version_unaudited");
  std::filesystem::remove(db / "steamos-manager-26.4.2-1/files");
  EXPECT_EQ(bus::probe_support(world.sources, world.budget()).code, "prelaunch_adapter_package_unreadable");
  EXPECT_TRUE(world.command_lines().empty());
}

TEST(SteamosBusTests, PolarisMustProvablySurviveTheStopBeforeAnyCommandRuns) {
  struct case_t {
    std::string name;
    std::function<void(world_t &)> change;
  };
  const std::vector<case_t> cases {
    {"reached through a unit bound to Game Mode", [](world_t &w) {
       w.fake->edit([](host_script_t &s) {
         s.game_mode().bound_by.push_back("session-helper.service");
         s.units["session-helper.service"].required_by = {"polaris.service"};
       });
     }},
    {"PartOf the target, reported as ConsistsOf", [](world_t &w) {
       w.fake->edit([](host_script_t &s) { s.target().consists_of.push_back("polaris.service"); });
     }},
    {"reached by PropagatesStopTo", [](world_t &w) {
       w.fake->edit([](host_script_t &s) { s.units["steam.service"].propagates_stop_to = {"polaris.service"}; });
     }},
    {"a closure too large to prove", [](world_t &w) {
       w.fake->edit([](host_script_t &s) {
         for (int i = 0; i < 200; ++i) s.target().consists_of.push_back("extra-" + std::to_string(i) + ".service");
       });
     }},
    {"an inactive, stale polaris.service", [](world_t &w) { w.fake->edit([](host_script_t &s) { s.polaris().active_state = "inactive"; }); }},
    {"polaris.service is another process", [](world_t &w) { w.scratch.write("self_cgroup", "0::/user.slice/user-1000.slice/session-2.scope\n"); }},
  };
  for (const auto &c : cases) {
    rig_t rig;
    c.change(rig.world);
    const auto outcome = rig.coordinator->run(launch_request());
    EXPECT_EQ(outcome.code, "prelaunch_service_not_independent") << c.name << ": " << outcome.message;
    EXPECT_TRUE(rig.world.command_lines().empty()) << c.name;
  }
}

TEST(SteamosBusTests, TheModeDecisionComesOnlyFromTheTypedRecords) {
  {
    rig_t rig;
    rig.world.fake->edit([](host_script_t &s) { s.game_mode().active_state = "activating"; });
    EXPECT_EQ(rig.coordinator->run(launch_request()).code, "prelaunch_session_mode_unknown");
    EXPECT_TRUE(rig.world.command_lines().empty());
  }
  {
    rig_t rig;
    rig.world.fake->edit([](host_script_t &s) { s.game_mode().active_state = "inactive"; });
    const auto outcome = rig.coordinator->run(launch_request());
    EXPECT_EQ(outcome.state, state_e::not_needed);
    EXPECT_TRUE(rig.world.command_lines().empty());
  }
  {
    // Unread is not "not in Game Mode".
    rig_t rig;
    rig.world.fake->edit([](host_script_t &s) { s.systemd.unique.clear(); });
    const auto outcome = rig.coordinator->run(launch_request());
    EXPECT_EQ(outcome.state, state_e::refused);
    EXPECT_EQ(outcome.code, "prelaunch_session_identity_unavailable");
    EXPECT_TRUE(rig.world.command_lines().empty());
  }
}

TEST(SteamosBusTests, AKeptReadinessAnswerFollowsAnInPlaceEditOfThePackageRecord) {
  rig_t rig;
  ASSERT_TRUE(rig.coordinator->readiness(true).ready);
  // Same directory, same name: the desc now records a release that was not audited.
  std::this_thread::sleep_for(20ms);
  rig.world.scratch.write("pacman/steamos-manager-26.4.1-1/desc", "%NAME%\nsteamos-manager\n\n%VERSION%\n26.4.1-2\n\n");
  const auto edited = rig.coordinator->readiness(true);
  EXPECT_FALSE(edited.ready);
  EXPECT_EQ(edited.code, "prelaunch_adapter_package_malformed");
}

TEST(SteamosBusTests, AStalledBusCostsOneReadinessBudgetPerAppListAndIsNeverHealth) {
  rig_t rig;
  rig.world.fake->edit([](host_script_t &s) { s.stalled = {"GetNameOwner"}; });
  const auto start = std::chrono::steady_clock::now();
  for (int app = 0; app < 6; ++app) {
    const auto readiness = rig.coordinator->readiness(true);
    EXPECT_FALSE(readiness.ready) << app;
    EXPECT_EQ(readiness.code, "prelaunch_session_read_timeout") << app;
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, prelaunch::budgets_t {}.readiness + 600ms) << "one budget for the whole list";
  rig.world.fake->edit([](host_script_t &s) { s.stalled.clear(); });
  EXPECT_FALSE(rig.coordinator->readiness(true).ready) << "the unknown is not replaced by a remembered positive";
}

TEST(SteamosBusTests, RegisteredCasesAreFrozen) {
  const std::set<std::string> expected {
    "ATypedSnapshotBindsTheGameModeSessionWithoutStartingAnyService",
    "AnyMalformedRecordIsRefusedNeverCoerced",
    "AStalledPeerCostsAtMostTheDeadlineAndACancellationEndsTheWait",
    "APeerThatNeverAuthenticatesCostsAtMostTheDeadline",
    "ASessionOrLeaderThatChangesWhileItIsReadIsRefused",
    "TheManagerMustBeThisAccountsUnitRunningThePackagedExecutable",
    "TheAdapterRunsExactlyTheFixedVerbWithOnlyTheAccountsBus",
    "ALeaveIsProvedFromTheRecordsAndAPurposefulEndRestoresANewGameModeOutsideLocks",
    "APurposefulEndRechecksTheCurrentStopClosureBeforeAnyReturn",
    "AnEarlyExitZeroWhileTheOldSessionPersistsIsNeverReadyAndNeverRolledBack",
    "AnAbortAfterTheRequestWasDeliveredIsOutcomeUnknownAndNotRolledBack",
    "ACancelledSwitchStopsOnlyItsOwnChildAndIsOutcomeUnknown",
    "ATargetGenerationThatChangesBeforeTheHandOffIsNeverOwned",
    "AnUnsupportedHostIsRefusedBeforeAnyCommandRuns",
    "AKeptReadinessAnswerNeverHidesAChangedManager",
    "ShutdownCancelsAStalledRestoreAndJoinsItWithoutRunningTheCommand",
    "ASecondPassThatDiffersInAnyRecordIsRefused",
    "LogindAndTheUserManagerAreBoundByOwnerAccountProcessAndPlacement",
    "ARepeatedPropertyIsRefusedWhateverItsTypes",
    "IntrospectionIsParsedAsXmlNotSearchedAsText",
    "TheManagerNameMustStillBeTheIntrospectedConnection",
    "ProductionDatabaseLookupUsesTheSteamOSLayoutAndKeepsPackageGuards",
    "PolarisMustProvablySurviveTheStopBeforeAnyCommandRuns",
    "TheModeDecisionComesOnlyFromTheTypedRecords",
    "AKeptReadinessAnswerFollowsAnInPlaceEditOfThePackageRecord",
    "AStalledBusCostsOneReadinessBudgetPerAppListAndIsNeverHealth",
    "RegisteredCasesAreFrozen",
  };
  std::set<std::string> registered;
  const auto *unit = ::testing::UnitTest::GetInstance();
  for (int suite = 0; suite < unit->total_test_suite_count(); ++suite) {
    const auto *info = unit->GetTestSuite(suite);
    if (std::string {info->name()} != "SteamosBusTests") continue;
    for (int test = 0; test < info->total_test_count(); ++test) registered.insert(info->GetTestInfo(test)->name());
  }
  EXPECT_EQ(registered, expected);
}
