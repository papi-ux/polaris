/** @file Read-only, bounded Wayland registry evidence for output-pinned host capture. */
#include "host_virtual_capture.h"
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>
using namespace std::literals;
namespace virtual_display {
  namespace {
    struct registry_probe_t {
      virtual_display::capture_provider_snapshot_t snapshot { .state = virtual_display::registry_state_e::unknown };
      std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> globals;
      bool complete = false;
      wl_display *display = nullptr;
      wl_registry *registry = nullptr;
      wl_callback *sync = nullptr;
      ~registry_probe_t() {
        if (sync) wl_callback_destroy(sync);
        if (registry) wl_registry_destroy(registry);
        if (display) wl_display_disconnect(display);
      }
      static void global(void *data, wl_registry *, std::uint32_t id, const char *name, std::uint32_t version) {
        if (name) static_cast<registry_probe_t *>(data)->globals.insert_or_assign(id, std::pair {std::string {name}, version});
      }
      static void removed(void *data, wl_registry *, std::uint32_t id) {
        static_cast<registry_probe_t *>(data)->globals.erase(id);
      }
      static void done(void *data, wl_callback *, std::uint32_t) {
        static_cast<registry_probe_t *>(data)->complete = true;
      }
      static constexpr wl_registry_listener registry_listener {.global = global, .global_remove = removed};
      static constexpr wl_callback_listener sync_listener {.done = done};
    };

    virtual_display::capture_provider_snapshot_t read_capture_registry() {
      registry_probe_t p;
      const char *name = std::getenv("WAYLAND_DISPLAY");
      // Do not borrow/consume an inherited connection: it may already carry
      // another client's objects. Unsupported transport is unknown, not GNOME.
      if (const char *inherited = std::getenv("WAYLAND_SOCKET"); inherited && *inherited) return p.snapshot;
      if (!name || !*name) return {};
      const auto deadline = std::chrono::steady_clock::now() + 500ms;
      const auto remaining_ms = [&]() {
        const auto left = deadline - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) return 0;
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(left).count()) + 1;
      };
      std::string path {name};
      if (path.front() != '/') {
        const char *runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || *runtime != '/') return p.snapshot;
        path = std::string {runtime} + '/' + path;
      }
      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      if (path.size() >= sizeof(addr.sun_path)) return p.snapshot;
      std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
      if (fd < 0) return p.snapshot;
      if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) { close(fd); return p.snapshot; }
        pollfd connecting {fd, POLLOUT, 0};
        int result;
        do { result = poll(&connecting, 1, remaining_ms()); } while (result < 0 && errno == EINTR && remaining_ms() > 0);
        int error = 0;
        socklen_t size = sizeof(error);
        if (result <= 0 || !(connecting.revents & POLLOUT) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error != 0) {
          close(fd); return p.snapshot;
        }
      }
      // libwayland takes ownership even when this call fails.
      p.display = wl_display_connect_to_fd(fd);
      if (!p.display) return p.snapshot;
      p.registry = wl_display_get_registry(p.display);
      if (!p.registry || wl_registry_add_listener(p.registry, &registry_probe_t::registry_listener, &p) < 0) return p.snapshot;
      p.sync = wl_display_sync(p.display);
      if (!p.sync || wl_callback_add_listener(p.sync, &registry_probe_t::sync_listener, &p) < 0) return p.snapshot;
      while (!p.complete && remaining_ms() > 0) {
        if (wl_display_dispatch_pending(p.display) < 0) return p.snapshot;
        if (p.complete) break;
        while (wl_display_prepare_read(p.display) != 0) {
          if (wl_display_dispatch_pending(p.display) < 0 || remaining_ms() <= 0) return p.snapshot;
        }
        const int flushed = wl_display_flush(p.display);
        if (flushed < 0 && errno != EAGAIN) {
          wl_display_cancel_read(p.display); return p.snapshot;
        }
        pollfd reading {wl_display_get_fd(p.display), static_cast<short>(POLLIN | (flushed < 0 ? POLLOUT : 0)), 0};
        const int result = poll(&reading, 1, remaining_ms());
        if (result < 0 && errno == EINTR) { wl_display_cancel_read(p.display); continue; }
        if (result <= 0 || (reading.revents & (POLLERR | POLLHUP | POLLNVAL))) {
          wl_display_cancel_read(p.display); return p.snapshot;
        }
        if (reading.revents & POLLIN) {
          if (wl_display_read_events(p.display) < 0) return p.snapshot;
        } else {
          wl_display_cancel_read(p.display);
        }
      }
      if (p.complete) {
        p.snapshot.state = virtual_display::registry_state_e::complete;
        for (const auto &[id, global] : p.globals) p.snapshot.note_global(global.first, global.second);
      }
      return p.snapshot;
    }

  }
  virtual_display::capture_provider_snapshot_t probe_capture_provider(bool fresh) {
    static std::mutex mutex;
    static std::optional<virtual_display::capture_provider_snapshot_t> cached;
    static std::string cached_transport;
    static std::chrono::steady_clock::time_point observed;
    const auto env = [](const char *key) { const char *v = std::getenv(key); return std::string {v ? v : ""}; };
    const auto transport = env("XDG_RUNTIME_DIR") + '\n' + env("WAYLAND_DISPLAY") + '\n' + env("WAYLAND_SOCKET");
    const auto now = std::chrono::steady_clock::now();
    {
      const std::lock_guard lock {mutex};
      if (!fresh && cached && cached_transport == transport && now - observed < 2s) return *cached;
    }
    const auto result = read_capture_registry();
    const std::lock_guard lock {mutex};
    cached = result;
    cached_transport = transport;
    observed = std::chrono::steady_clock::now();
    return *cached;
  }

}
