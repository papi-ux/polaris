/**
 * A private PipeWire server in the test process with a listening socket in a directory the test
 * owns, plus a producer in a SEPARATE process (tests/fixtures/pipewire_remote_producer.cpp) that
 * connects to that socket. The capture under test connects to the same server over its own socket
 * through capture_t's production remote_fd path, so its registry, sync and credentials are real.
 *
 * Nothing here reads XDG_RUNTIME_DIR or connects to a user daemon. Environment variables that
 * PipeWire reads at context creation are set only around that creation and restored at once.
 * This is graph acceptance across processes, not compositor, portal, Deck or physical acceptance.
 */
#pragma once

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <spawn.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <pipewire/impl.h>
#include <pipewire/pipewire.h>

#include "src/pipewire_producer_binding.h"

extern char **environ;

namespace test_utils {
  /** Set or clear environment variables for one scope, restoring each previous value. */
  class scoped_env_t {
  public:
    scoped_env_t(std::initializer_list<std::pair<const char *, std::optional<std::string>>> values) {
      for (const auto &[name, value] : values) {
        const char *previous = std::getenv(name);
        saved_.emplace_back(name, previous ? std::optional<std::string> {previous} : std::nullopt);
        if (value) setenv(name, value->c_str(), 1);
        else unsetenv(name);
      }
    }
    ~scoped_env_t() {
      for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
        if (it->second) setenv(it->first, it->second->c_str(), 1);
        else unsetenv(it->first);
      }
    }
    scoped_env_t(const scoped_env_t &) = delete;
    scoped_env_t &operator=(const scoped_env_t &) = delete;

  private:
    std::vector<std::pair<const char *, std::optional<std::string>>> saved_;
  };

  /** One registry global as the server publishes it to a fully permitted observer. */
  struct remote_global_t {
    std::string type;
    std::map<std::string, std::string> props;
  };

  struct pipewire_remote_server_t {
    std::filesystem::path runtime_dir;
    std::string socket_name;
    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *control = nullptr;  ///< in-process observer and link creator
    pw_registry *registry = nullptr;
    spa_hook context_listener {};
    spa_hook registry_listener {};
    bool context_listener_added = false;
    std::map<std::uint32_t, remote_global_t> globals;  ///< server loop lock

    pipewire_remote_server_t() {
      static std::once_flag initialized;
      std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
    }

    bool start() {
      char tmpl[] = "/tmp/pw-rp-XXXXXX";  // short: sun_path is 108 bytes
      if (!mkdtemp(tmpl)) return false;
      runtime_dir = tmpl;
      socket_name = "polaris-remote-" + std::to_string(getpid());
      loop = pw_thread_loop_new("polaris-remote-server", nullptr);
      if (!loop) return false;
      {
        // Only for this creation: protocol-native reads these while loading.
        scoped_env_t env {
          {"PIPEWIRE_RUNTIME_DIR", runtime_dir.string()},
          {"PIPEWIRE_CORE", socket_name},
          {"PIPEWIRE_CONFIG_NAME", std::nullopt},
          {"PIPEWIRE_DAEMON", std::nullopt},
          {"PIPEWIRE_REMOTE", std::nullopt},
        };
        context = pw_context_new(pw_thread_loop_get_loop(loop),
          pw_properties_new(PW_KEY_CONFIG_NAME, POLARIS_SOURCE_DIR "/tests/fixtures/pipewire-remote-producer.conf", nullptr), 0);
      }
      if (!context || !std::filesystem::exists(runtime_dir / socket_name)) return false;
      // What module-access would do for an unrestricted client: socket clients start with no
      // permissions, and every client's first property update asks this listener.
      static const pw_context_events context_events = [] {
        pw_context_events events {};
        events.version = PW_VERSION_CONTEXT_EVENTS;
        events.check_access = [](void *, pw_impl_client *client) {
          const pw_permission all = PW_PERMISSION_INIT(PW_ID_ANY, PW_PERM_ALL);
          pw_impl_client_update_permissions(client, 1, &all);
        };
        return events;
      }();
      pw_context_add_listener(context, &context_listener, &context_events, this);
      context_listener_added = true;
      control = pw_context_connect_self(context, nullptr, 0);
      if (!control) return false;
      static const pw_registry_events registry_events = [] {
        pw_registry_events events {};
        events.version = PW_VERSION_REGISTRY_EVENTS;
        events.global = [](void *data, std::uint32_t id, std::uint32_t, const char *type, std::uint32_t, const spa_dict *props) {
          auto &server = *static_cast<pipewire_remote_server_t *>(data);
          remote_global_t global {type ? type : "", {}};
          const spa_dict_item *item;
          if (props) spa_dict_for_each(item, props) global.props[item->key] = item->value ? item->value : "";
          server.globals[id] = std::move(global);
        };
        events.global_remove = [](void *data, std::uint32_t id) {
          static_cast<pipewire_remote_server_t *>(data)->globals.erase(id);
        };
        return events;
      }();
      registry = pw_core_get_registry(control, PW_VERSION_REGISTRY, 0);
      if (!registry) return false;
      pw_registry_add_listener(registry, &registry_listener, &registry_events, this);
      return pw_thread_loop_start(loop) >= 0;
    }

    /** A new client connection to the private socket, made by THIS process. */
    int connect_socket() const {
      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) return -1;
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      const auto path = (runtime_dir / socket_name).string();
      if (path.size() >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
      }
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        close(fd);
        return -1;
      }
      return fd;
    }

    template<class Predicate> bool wait(Predicate predicate, std::chrono::milliseconds budget = std::chrono::seconds(5)) {
      const auto end = std::chrono::steady_clock::now() + budget;
      do {
        pw_thread_loop_lock(loop);
        const bool done = predicate();
        pw_thread_loop_unlock(loop);
        if (done) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      } while (std::chrono::steady_clock::now() < end);
      return false;
    }

    // The lookups below run under the server loop lock and read only server-set keys.
    std::vector<std::uint32_t> clients_with_pid(pid_t pid) const {
      std::vector<std::uint32_t> ids;
      for (const auto &[id, global] : globals) {
        const auto found = global.props.find("pipewire.sec.pid");
        if (global.type == PW_TYPE_INTERFACE_Client && found != global.props.end() && found->second == std::to_string(pid)) ids.push_back(id);
      }
      return ids;
    }

    std::vector<std::uint32_t> nodes_of_client(std::uint32_t client) const {
      std::vector<std::uint32_t> ids;
      for (const auto &[id, global] : globals) {
        const auto found = global.props.find("client.id");
        if (global.type == PW_TYPE_INTERFACE_Node && found != global.props.end() && found->second == std::to_string(client)) ids.push_back(id);
      }
      return ids;
    }

    std::uint64_t serial(std::uint32_t id) const {
      const auto global = globals.find(id);
      if (global == globals.end()) return 0;
      const auto found = global->second.props.find("object.serial");
      return found == global->second.props.end() ? 0 : pipewire_producer_binding::parse_decimal(found->second, UINT64_MAX).value_or(0);
    }

    std::uint32_t control_client() const {
      return pw_proxy_get_bound_id(reinterpret_cast<pw_proxy *>(pw_core_get_client(control)));
    }

    /** A link made by the control client, as a session manager would make it. Caller holds the lock. */
    pw_proxy *link(std::uint32_t output_node, std::uint32_t input_node) {
      auto *props = pw_properties_new(nullptr, nullptr);
      pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", output_node);
      pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", input_node);
      auto *proxy = static_cast<pw_proxy *>(pw_core_create_object(control, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
      pw_properties_free(props);
      return proxy;
    }

    void stop() {
      if (loop) pw_thread_loop_stop(loop);
      if (registry) {
        spa_hook_remove(&registry_listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
        registry = nullptr;
      }
      if (control) {
        pw_core_disconnect(control);
        control = nullptr;
      }
      if (context_listener_added) {
        spa_hook_remove(&context_listener);
        context_listener_added = false;
      }
      if (context) {
        pw_context_destroy(context);
        context = nullptr;
      }
      if (loop) {
        pw_thread_loop_destroy(loop);
        loop = nullptr;
      }
      std::error_code ignored;
      if (!runtime_dir.empty()) std::filesystem::remove_all(runtime_dir, ignored);
      runtime_dir.clear();
    }

    ~pipewire_remote_server_t() { stop(); }
  };

  /** The producer helper process, spawned with an explicit environment and two pipes. */
  struct pipewire_remote_producer_t {
    pid_t pid = -1;
    int to_child = -1;    ///< closing it asks the helper to exit
    int from_child = -1;  ///< "ready <node>\n"
    std::uint32_t node = SPA_ID_INVALID;

    bool spawn(const pipewire_remote_server_t &server) {
      int in[2] = {-1, -1}, out[2] = {-1, -1};
      if (pipe2(in, O_CLOEXEC) < 0) return false;
      if (pipe2(out, O_CLOEXEC) < 0) {
        close(in[0]);
        close(in[1]);
        return false;
      }
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
      posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
      // Only what the helper needs: the private runtime dir, and library search paths if the
      // build set them. No XDG_RUNTIME_DIR, DBUS or PIPEWIRE_REMOTE from the test's environment.
      std::vector<std::string> env {"PIPEWIRE_RUNTIME_DIR=" + server.runtime_dir.string()};
      for (const char *name : {"LD_LIBRARY_PATH", "PIPEWIRE_MODULE_DIR", "SPA_PLUGIN_DIR", "PATH"}) {
        if (const char *value = std::getenv(name)) env.push_back(std::string {name} + '=' + value);
      }
      std::vector<char *> envp;
      for (auto &entry : env) envp.push_back(entry.data());
      envp.push_back(nullptr);
      std::string program = POLARIS_PIPEWIRE_REMOTE_PRODUCER_PATH;
      std::string config = POLARIS_SOURCE_DIR "/tests/fixtures/pipewire-pool.conf";
      std::string remote = server.socket_name;
      char *argv[] = {program.data(), config.data(), remote.data(), nullptr};
      const int spawned = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv, envp.data());
      posix_spawn_file_actions_destroy(&actions);
      close(in[0]);
      close(out[1]);
      to_child = in[1];
      from_child = out[0];
      if (spawned != 0) {
        pid = -1;
        return false;
      }
      return read_ready(std::chrono::seconds(5));
    }

    bool read_ready(std::chrono::milliseconds budget) {
      std::string line;
      const auto end = std::chrono::steady_clock::now() + budget;
      const int flags = fcntl(from_child, F_GETFL);
      fcntl(from_child, F_SETFL, flags | O_NONBLOCK);
      while (std::chrono::steady_clock::now() < end) {
        char ch = 0;
        const auto got = read(from_child, &ch, 1);
        if (got == 1) {
          if (ch == '\n') break;
          line.push_back(ch);
          continue;
        }
        if (got == 0) return false;  // the helper exited before it was ready
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (line.rfind("ready ", 0) != 0) return false;
      const auto id = pipewire_producer_binding::parse_id(std::string_view {line}.substr(6));
      if (!id) return false;
      node = *id;
      return true;
    }

    /** Ask the helper to exit and reap it; a helper that ignores that for 5 s is killed. */
    int finish() {
      if (to_child >= 0) {
        close(to_child);
        to_child = -1;
      }
      int status = -1;
      if (pid > 0) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (waitpid(pid, &status, WNOHANG) == 0) {
          if (std::chrono::steady_clock::now() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        pid = -1;
      }
      if (from_child >= 0) {
        close(from_child);
        from_child = -1;
      }
      return status;
    }

    ~pipewire_remote_producer_t() { finish(); }
  };
}  // namespace test_utils
