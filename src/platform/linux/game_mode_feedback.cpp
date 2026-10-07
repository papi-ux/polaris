#include "game_mode_feedback.h"

#ifdef __linux__
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef POLARIS_BUILD_X11_XCB
#include <xcb/xcb.h>
#endif

namespace platf::game_mode_feedback {
  namespace {
    using namespace game_mode_adaptive_sync;
    using namespace std::chrono_literals;

    std::string small_file(const std::filesystem::path &path) {
      std::ifstream file(path, std::ios::binary);
      if (!file) return {};
      std::array<char, 16385> buffer {};
      file.read(buffer.data(), buffer.size());
      if (file.gcount() > 16384) return {};
      return {buffer.data(), static_cast<std::size_t>(file.gcount())};
    }

    template<class T> bool decimal(std::string_view text, T &value) {
      if (text.empty() || text.front() < '0' || text.front() > '9') return false;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      return parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size();
    }

    std::optional<generation_t> process_generation(const probe_t &probe, const capture_producer::binding_t &binding) {
      if (binding.producer_pid <= 1 || binding.producer_uid != getuid()) return std::nullopt;
      const auto path = probe.proc_root / std::to_string(binding.producer_pid);
      const auto status = small_file(path / "status");
      const auto pos = status.find("\nUid:");
      if (pos == std::string::npos) return std::nullopt;
      std::istringstream uid_line(status.substr(pos + 5, status.find('\n', pos + 1) - pos - 5));
      std::uint64_t real = 0, effective = 0, saved = 0, fs = 0;
      if (!(uid_line >> real >> effective >> saved >> fs) || real != binding.producer_uid ||
          effective != real || saved != real || fs != real) return std::nullopt;
      // comm may contain spaces and parentheses: field 3 starts after the LAST closing parenthesis.
      const auto stat_text = small_file(path / "stat");
      const auto end = stat_text.rfind(')');
      if (end == std::string::npos) return std::nullopt;
      std::istringstream fields(stat_text.substr(end + 1));
      std::string field;
      for (int index = 3; index <= 22; ++index) if (!(fields >> field)) return std::nullopt;
      generation_t generation;
      generation.pid = binding.producer_pid;
      if (!decimal(field, generation.start_time) || generation.start_time == 0) return std::nullopt;
      struct stat executable {};
      // Capability-holding gamescope can withhold ptrace-read access to /proc/P/exe. The linked
      // client, socket peer, UID and process start-time bracket still bind the observation; only
      // executable/version provenance is unknown. Never substitute comm, a node name or a path.
      if (::stat((path / "exe").c_str(), &executable) != 0 || !S_ISREG(executable.st_mode)) return generation;
      generation.exe_device = executable.st_dev;
      generation.exe_inode = executable.st_ino;
      generation.exe_size = executable.st_size;
      generation.exe_mtime_ns = std::int64_t(executable.st_mtim.tv_sec) * 1'000'000'000 + executable.st_mtim.tv_nsec;
      return generation;
    }

    version_t versions(const probe_t &probe, const capture_producer::binding_t &binding) {
      version_t result;
      std::error_code error;
      result.producer_exe = std::filesystem::read_symlink(probe.proc_root / std::to_string(binding.producer_pid) / "exe", error).string();
      if (error) result.producer_exe.clear();
      std::istringstream release(small_file(probe.os_release));
      std::string line;
      while (std::getline(release, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const auto key = line.substr(0, equals);
        if (key != "ID" && key != "VERSION_ID" && key != "BUILD_ID" && key != "VARIANT_ID") continue;
        auto value = line.substr(equals + 1);
        if (value.size() > 256) continue;
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
          value = value.substr(1, value.size() - 2);
        result.os_release[key] = std::move(value);
      }
      // There is no verified package-to-running-executable or affected/fixed version mapping.
      return result;
    }

#ifdef POLARIS_BUILD_X11_XCB
    /** Interrupt the actual socket at the deadline, including libxcb's setup handshake. A duplicate
     * keeps shutdown tied to this socket even if libxcb closes/reuses its own descriptor. Never
     * detach this thread, and stop it before the connection is destroyed. */
    class bounded_x_t {
    public:
      bounded_x_t(int fd, steady_clock_t::time_point deadline): cancel_fd_(::fcntl(fd, F_DUPFD_CLOEXEC, 0)) {
        if (cancel_fd_ < 0) { ::close(fd); return; }
        timer_ = std::thread([this, deadline] {
          std::unique_lock lock(mutex_);
          if (!changed_.wait_until(lock, deadline, [this] { return done_; })) ::shutdown(cancel_fd_, SHUT_RDWR);
        });
        // Gamescope's local Xwayland normally permits its session account. An authentication
        // requirement fails closed; do not obtain credentials from another process's environment.
        connection_ = xcb_connect_to_fd(fd, nullptr);
      }
      ~bounded_x_t() {
        { std::lock_guard lock(mutex_); done_ = true; }
        changed_.notify_all();
        if (timer_.joinable()) timer_.join();
        if (connection_) xcb_disconnect(connection_);
        if (cancel_fd_ >= 0) ::close(cancel_fd_);
      }
      xcb_connection_t *get() const { return connection_; }
      bool good() const { return connection_ && xcb_connection_has_error(connection_) == 0; }
    private:
      int cancel_fd_ = -1;
      xcb_connection_t *connection_ = nullptr;
      std::mutex mutex_;
      std::condition_variable changed_;
      bool done_ = false;
      std::thread timer_;
    };

    int connect_peer(const std::filesystem::path &path, const capture_producer::binding_t &binding, steady_clock_t::time_point deadline) {
      struct stat socket_file {};
      if (::stat(path.c_str(), &socket_file) != 0 || !S_ISSOCK(socket_file.st_mode) || socket_file.st_uid != binding.producer_uid) return -1;
      const auto name = path.string();
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      if (name.size() >= sizeof(address.sun_path)) return -1;
      std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
      const auto fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      if (fd < 0) return -1;
      const auto failed = [&] { ::close(fd); return -1; };
      if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS) return failed();
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - steady_clock_t::now()).count();
        if (remaining <= 0) return failed();
        pollfd waiting {fd, POLLOUT, 0};
        if (::poll(&waiting, 1, static_cast<int>(remaining)) <= 0) return failed();
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) return failed();
      }
      ucred peer {};
      socklen_t size = sizeof(peer);
      if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) < 0 || size != sizeof(peer) ||
          peer.pid != binding.producer_pid || peer.uid != binding.producer_uid) return failed();
      return fd;
    }

    xcb_atom_t atom(xcb_connection_t *connection, const char *name) {
      auto *reply = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 1, std::strlen(name), name), nullptr);
      if (!reply) return XCB_ATOM_NONE;
      const auto value = reply->atom;
      std::free(reply);
      return value;
    }

    property_t property(xcb_connection_t *connection, xcb_window_t root, xcb_atom_t key) {
      property_t value;
      if (key == XCB_ATOM_NONE) return value;
      auto *reply = xcb_get_property_reply(connection, xcb_get_property(connection, 0, root, key, XCB_GET_PROPERTY_TYPE_ANY, 0, 1), nullptr);
      if (!reply) return value;
      value.present = reply->type != XCB_ATOM_NONE;
      value.cardinal = reply->type == XCB_ATOM_CARDINAL;
      value.format = reply->format;
      value.count = reply->value_len;
      value.bytes_after = reply->bytes_after;
      if (reply->format == 32 && reply->value_len == 1 && xcb_get_property_value_length(reply) == 4)
        std::memcpy(&value.value, xcb_get_property_value(reply), 4);
      std::free(reply);
      return value;
    }

    bool root_zero(const property_t &value) {
      return value.present && value.cardinal && value.format == 32 && value.count == 1 && value.bytes_after == 0 && value.value == 0;
    }

    observation_t read_roots(observation_t value, const capture_producer::binding_t &binding, const probe_t &probe) {
      const auto deadline = steady_clock_t::now() + 250ms;
      std::error_code error;
      std::vector<std::filesystem::path> sockets;
      unsigned scanned = 0;
      for (std::filesystem::directory_iterator entry(probe.socket_dir, error), end; !error && entry != end; entry.increment(error)) {
        if (++scanned > 64) { value.reason = "root_display_ambiguous"; return value; }
        if (steady_clock_t::now() >= deadline) { value.reason = "x_read_timeout"; return value; }
        const auto name = entry->path().filename().string();
        std::uint32_t number = 0;
        if (name.size() > 1 && name.front() == 'X' && decimal(std::string_view(name).substr(1), number)) sockets.push_back(entry->path());
        if (sockets.size() > 8) { value.reason = "root_display_ambiguous"; return value; }
      }
      if (error) { value.reason = "root_display_not_found"; return value; }
      std::sort(sockets.begin(), sockets.end());
      std::optional<snapshot_t> selected;
      for (const auto &socket : sockets) {
        if (steady_clock_t::now() >= deadline) { value.reason = "x_read_timeout"; return value; }
        const auto fd = connect_peer(socket, binding, deadline);
        if (fd < 0) continue;
        bounded_x_t connection(fd, deadline);
        if (!connection.good()) {
          value.reason = steady_clock_t::now() >= deadline ? "x_read_timeout" : "x_connect_failed_or_denied";
          return value;  // Do not select a different display when this matching peer was unreadable.
        }
        auto *x = connection.get();
        const auto screens = xcb_setup_roots_iterator(xcb_get_setup(x));
        if (screens.rem != 1) { value.reason = "root_display_ambiguous"; return value; }
        const auto root = screens.data->root;
        const auto id = atom(x, "GAMESCOPE_XWAYLAND_SERVER_ID");
        const auto capable = atom(x, "GAMESCOPE_VRR_CAPABLE");
        const auto feedback = atom(x, "GAMESCOPE_VRR_FEEDBACK");
        // Select PropertyChange events for THIS client only; do not change any root property.
        const std::uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        const auto subscribed = xcb_change_window_attributes_checked(x, root, XCB_CW_EVENT_MASK, &mask);
        if (auto *failure = xcb_request_check(x, subscribed)) {
          std::free(failure); value.reason = "x_connect_failed_or_denied"; return value;
        }
        const auto root_id = property(x, root, id);
        snapshot_t snapshot {property(x, root, capable), property(x, root, feedback), false};
        const auto root_id_after = property(x, root, id);
        const auto barrier = xcb_get_input_focus(x);
        auto *reply = xcb_get_input_focus_reply(x, barrier, nullptr);
        if (!reply || !connection.good() || steady_clock_t::now() >= deadline) {
          std::free(reply); value.reason = "x_read_timeout"; return value;
        }
        std::free(reply);
        while (auto *event = xcb_poll_for_queued_event(x)) {
          // SendEvent is not a producer notification. It still invalidates this observation.
          if ((event->response_type & 0x7f) == XCB_PROPERTY_NOTIFY) {
            const auto *notify = reinterpret_cast<xcb_property_notify_event_t *>(event);
            if (notify->window == root && (notify->atom == id || notify->atom == capable || notify->atom == feedback) &&
                event->full_sequence >= subscribed.sequence && event->full_sequence <= barrier.sequence) snapshot.torn = true;
          } else if (event->response_type == 0) snapshot.torn = true;
          std::free(event);
        }
        if (!root_zero(root_id) && !root_zero(root_id_after)) continue;
        if (!root_zero(root_id) || !root_zero(root_id_after)) snapshot.torn = true;
        if (selected) { value.reason = "root_display_ambiguous"; return value; }
        selected = snapshot;
      }
      if (!selected) { value.reason = "root_display_not_found"; return value; }
      value.route_bound = true;
      value.properties = *selected;
      value.observed_at = steady_clock_t::now();
      value.reason = snapshot_reason(value);
      return value;
    }
#endif
  }

  game_mode_adaptive_sync::observation_t collect_once(const graph_reader_t &graph, const probe_t &probe) {
    using namespace game_mode_adaptive_sync;
    observation_t value;
    const auto before = graph(std::chrono::milliseconds(100));
    if (!before.verified) { value.reason = before.reason; return value; }
    const auto generation = process_generation(probe, before);
    if (!generation) { value.reason = "producer_generation_unreadable"; return value; }
    value.generation = *generation;
    value.generation_checked = true;
    value.version = versions(probe, before);
    if (generation->exe_inode == 0) value.version.producer_exe.clear();
#ifdef POLARIS_BUILD_X11_XCB
    value = read_roots(std::move(value), before, probe);
#else
    value.reason = "x_reader_unavailable";
#endif
    const auto after = graph(std::chrono::milliseconds(100));
    const auto final_generation = process_generation(probe, after);
    if (!after.verified || before != after) {
      value.route_bound = false;
      value.generation_checked = false;
      value.reason = "pipewire_graph_changed";
    } else if (!final_generation || *final_generation != *generation) {
      value.route_bound = false;
      value.generation_checked = false;
      value.reason = "producer_restarted";
    } else {
      value.generation_checked_at = game_mode_adaptive_sync::steady_clock_t::now();
    }
    // The socket and root-ID property are observed facts. Property writer and PID-reuse ancestry
    // remain unattested; neither a physical connector nor independently fresh VRR state is known.
    return value;
  }

  collector_t::collector_t(graph_reader_t graph, probe_t probe): store_(std::make_shared<game_mode_adaptive_sync::store_t>()) {
    worker_ = std::thread([this, graph = std::move(graph), probe = std::move(probe)] {
      for (;;) {
        { std::lock_guard lock(mutex_); if (stop_) break; }
        store_->publish(collect_once(graph, probe));
        std::unique_lock lock(mutex_);
        if (changed_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; })) break;
      }
    });
  }

  collector_t::~collector_t() {
    { std::lock_guard lock(mutex_); stop_ = true; }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
    store_->end();
  }
}
#endif
