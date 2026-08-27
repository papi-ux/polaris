/**
 * @file src/platform/linux/stream_runtime_gamescope.cpp
 * @brief Owned / attach Gamescope private stream runtime.
 *
 * Prefer attaching to an existing gamescope-0 (idle unit). If absent, spawn a
 * headless gamescope that owns gamescope-0 so portal capture stays stable.
 */

#include "stream_runtime.h"

#ifdef __linux__

  #include "executable_path.h"
  #include "gamescope_process.h"
  #include "src/logging.h"

  #include <chrono>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <dirent.h>
  #include <filesystem>
  #include <fstream>
  #include <functional>
  #include <fcntl.h>
  #include <mutex>
  #include <optional>
  #include <poll.h>
  #include <signal.h>
  #include <sstream>
  #include <string>
  #include <string_view>
  #include <sys/file.h>
  #include <sys/stat.h>
  #include <sys/syscall.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>

using namespace std::literals;

namespace stream_runtime {
  namespace {

    namespace fs = std::filesystem;
    namespace gp = gamescope_process;

    std::string xdg_runtime_dir();

    void close_inherited_descriptors(long fallback_limit) {
#ifdef SYS_close_range
      // No descriptor is intentionally passed to an owned gamescope. Use the
      // kernel primitive when available so a high RLIMIT_NOFILE does not turn
      // every launch into a million close(2) calls.
      if (syscall(SYS_close_range, static_cast<unsigned int>(STDERR_FILENO + 1), ~0U, 0) == 0) {
        return;
      }
#endif
      const auto limit = fallback_limit > STDERR_FILENO ? fallback_limit : 1024;
      for (long fd = STDERR_FILENO + 1; fd < limit; ++fd) {
        (void) close(static_cast<int>(fd));
      }
    }

    class owner_transition_lock_t {
    public:
      explicit owner_transition_lock_t(bool acquire = true) {
        if (!acquire) {
          return;
        }
        const auto path = fs::path(xdg_runtime_dir()) / "polaris-gamescope.lock";
        fd_ = open(path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
        if (fd_ >= 0 && flock(fd_, LOCK_EX) != 0) {
          close(fd_);
          fd_ = -1;
        }
      }

      ~owner_transition_lock_t() {
        if (fd_ >= 0) {
          flock(fd_, LOCK_UN);
          close(fd_);
        }
      }

      owner_transition_lock_t(const owner_transition_lock_t &) = delete;
      owner_transition_lock_t &operator=(const owner_transition_lock_t &) = delete;

      explicit operator bool() const { return fd_ >= 0; }

      void unlock() {
        if (fd_ >= 0) {
          flock(fd_, LOCK_UN);
          close(fd_);
          fd_ = -1;
        }
      }

    private:
      int fd_ = -1;
    };

    std::string xdg_runtime_dir() {
      if (const char *xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
        return xdg;
      }
      return "/run/user/" + std::to_string(getuid());
    }

    bool runtime_acquisition_allowed_locked() {
      const auto rt = fs::path(xdg_runtime_dir());
      const auto path_is_absent = [](const fs::path &path) {
        std::error_code ec;
        const auto status = fs::symlink_status(path, ec);
        if (ec == std::errc::no_such_file_or_directory) {
          return true;
        }
        return !ec && status.type() == fs::file_type::not_found;
      };
      if (!path_is_absent(rt / "polaris-gamescope-wsi-nested")) {
        return false;
      }

      const auto state_path = rt / "polaris-gamescope-session-state";
      if (!path_is_absent(state_path)) {
        std::ifstream state(state_path);
        std::string session_id;
        std::string mode;
        std::string service_mode;
        std::string extra;
        if (!(state >> session_id >> mode) || session_id.empty()) {
          return false;
        }
        if (state >> service_mode) {
          if (service_mode != "managed" && service_mode != "standalone") {
            return false;
          }
          if (state >> extra) {
            return false;
          }
        }
        return mode == "attach";
      }

      const auto legacy_id = rt / "polaris-gamescope-session-id";
      const auto legacy_mode = rt / "polaris-gamescope-session-mode";
      const bool id_absent = path_is_absent(legacy_id);
      const bool mode_absent = path_is_absent(legacy_mode);
      if (id_absent && mode_absent) {
        return true;
      }
      if (id_absent || mode_absent) {
        return false;
      }
      std::ifstream mode_file(legacy_mode);
      std::string mode;
      std::string extra;
      if (!(mode_file >> mode) || (mode_file >> extra)) {
        return false;
      }
      return mode == "attach";
    }

    bool socket_exists(const std::string &name) {
      return fs::exists(xdg_runtime_dir() + "/" + name);
    }

    enum class private_group_state_e {
      alive,
      drained,
      unknown,
    };

    private_group_state_e private_group_state(pid_t pgid) {
      DIR *proc = opendir("/proc");
      if (!proc) {
        return private_group_state_e::unknown;
      }
      bool alive = false;
      bool complete = true;
      while (true) {
        errno = 0;
        auto *entry = readdir(proc);
        if (!entry) {
          if (errno != 0) {
            complete = false;
          }
          break;
        }
        char *end = nullptr;
        const long parsed = std::strtol(entry->d_name, &end, 10);
        if (parsed <= 1 || !end || *end != '\0') {
          continue;
        }
        const pid_t pid = static_cast<pid_t>(parsed);
        std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
        std::string stat_line;
        if (!std::getline(stat_file, stat_line)) {
          if (kill(pid, 0) == 0 || errno == EPERM) {
            complete = false;
          }
          continue;
        }
        const auto close = stat_line.rfind(')');
        if (close == std::string::npos || close + 2 >= stat_line.size()) {
          complete = false;
          continue;
        }
        std::istringstream fields(stat_line.substr(close + 2));
        char state = '\0';
        pid_t ppid = 0;
        pid_t process_group = 0;
        pid_t session = 0;
        if (!(fields >> state >> ppid >> process_group >> session)) {
          complete = false;
          continue;
        }
        if (session == pgid) {
          // The SID is the private ownership domain. Descendants may move to a
          // separate process group while remaining in this exact session; they
          // must keep teardown live so marker/environment authority is retained.
          if (state != 'Z' && state != 'X') {
            alive = true;
          }
        }
        else if (process_group == pgid) {
          // The numeric PGID is now associated with another session. Authority
          // is ambiguous and negative-PGID signaling must fail closed.
          complete = false;
        }
      }
      closedir(proc);
      if (!complete) {
        return private_group_state_e::unknown;
      }
      return alive ? private_group_state_e::alive : private_group_state_e::drained;
    }

    bool is_private_group_leader(pid_t pid) {
      errno = 0;
      const auto pgid = getpgid(pid);
      if (pgid < 0) {
        return false;
      }
      const auto sid = getsid(pid);
      return sid >= 0 && pgid == pid && sid == pid;
    }

    bool pidfd_has_exited(int pidfd) {
      pollfd descriptor {.fd = pidfd, .events = POLLIN, .revents = 0};
      const int result = poll(&descriptor, 1, 0);
      return result == 1 && (descriptor.revents & (POLLIN | POLLHUP)) != 0;
    }

    bool pidfd_targets_pid(int pidfd, pid_t expected_pid) {
      if (pidfd < 0 || expected_pid <= 1) {
        return false;
      }
      std::ifstream info("/proc/self/fdinfo/" + std::to_string(pidfd));
      std::string key;
      pid_t pid = -1;
      while (info >> key) {
        if (key == "Pid:") {
          return (info >> pid) && pid == expected_pid;
        }
        std::string ignored;
        std::getline(info, ignored);
      }
      return false;
    }

    bool drain_private_process_group(
      pid_t pgid,
      int leader_pidfd,
      const std::function<bool()> &authority_still_current,
      int term_steps = 30,
      int kill_steps = 20
    ) {
      if (!pidfd_targets_pid(leader_pidfd, pgid) || !authority_still_current()) {
        return false;
      }
      auto state = private_group_state(pgid);
      if (state == private_group_state_e::unknown) {
        return false;
      }
      if (state == private_group_state_e::drained) {
        return true;
      }
      if (kill(-pgid, SIGTERM) != 0 && errno != ESRCH) {
        return false;
      }
      int status = 0;
      for (int i = 0; i < term_steps && state == private_group_state_e::alive; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        state = private_group_state(pgid);
      }
      if (state == private_group_state_e::alive) {
        if (!pidfd_targets_pid(leader_pidfd, pgid) || !authority_still_current() ||
            (kill(-pgid, SIGKILL) != 0 && errno != ESRCH)) {
          return false;
        }
        for (int i = 0; i < kill_steps && state == private_group_state_e::alive; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          state = private_group_state(pgid);
        }
      }
      (void) waitpid(pgid, &status, WNOHANG);
      return state == private_group_state_e::drained;
    }

    bool rollback_spawned_private_group(pid_t child, int leader_pidfd) {
      const auto group_state = private_group_state(child);
      if (group_state == private_group_state_e::unknown) {
        return false;
      }
      if (group_state == private_group_state_e::alive) {
        return drain_private_process_group(child, leader_pidfd, []() { return true; });
      }
      int status = 0;
      const auto child_state = waitpid(child, &status, WNOHANG);
      if (child_state == child) {
        return true;
      }
      if (child_state != 0 || kill(child, SIGTERM) != 0) {
        return false;
      }
      return waitpid(child, nullptr, 0) == child;
    }

    /// Wait until socket exists for two consecutive polls (avoids attach races).
    bool wait_for_stable_socket(const std::string &name, int timeout_ms = 8000) {
      const auto step = 50;
      int seen = 0;
      for (int elapsed = 0; elapsed < timeout_ms; elapsed += step) {
        if (socket_exists(name)) {
          if (++seen >= 2) {
            return true;
          }
        }
        else {
          seen = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
      }
      return socket_exists(name);
    }

    /// Read $XDG_RUNTIME_DIR/polaris-gamescope-force (1/true → HDR wanted).
    bool read_hdr_force() {
      const char *rt = std::getenv("XDG_RUNTIME_DIR");
      if (!rt || !*rt) {
        return false;
      }
      std::ifstream f(std::string(rt) + "/polaris-gamescope-force");
      std::string line;
      if (!f || !std::getline(f, line)) {
        return false;
      }
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
      }
      return line == "1" || line == "true";
    }

    class gamescope_runtime_t: public stream_runtime_t {
    public:
      std::string_view backend_id() const override {
        return stream_display_policy::k_runtime_gamescope;
      }

      bool start(const start_params_t &params) override {
        std::lock_guard lock(mu_);
        const bool retained_state =
          owned_ || owned_group_drained_ || pid_ > 0 || leader_pidfd_ >= 0 || marker_ ||
          !socket_name_.empty() || !x11_display_.empty();
        if (retained_state) {
          owner_transition_lock_t retained_lock;
          if (!retained_lock || !runtime_acquisition_allowed_locked()) {
            BOOST_LOG(warning) << "gamescope_runtime: nested ownership claim blocks retained runtime reuse"sv;
            return false;
          }
          if (is_running_unlocked() && revalidate_running_unlocked(params, true)) {
            BOOST_LOG(info) << "gamescope_runtime: already "sv
                            << (owned_ ? "owned"sv : "attached"sv)
                            << " "sv << (socket_name_.empty() ? "gamescope-0" : socket_name_)
                            << "; reusing"sv;
            return true;
          }
          retained_lock.unlock();
          if (owned_) {
            if (!marker_) {
              BOOST_LOG(error) << "gamescope_runtime: retained owned state lacks immutable marker authority; refusing restart"sv;
              return false;
            }
            BOOST_LOG(warning) << "gamescope_runtime: draining retained owned generation before restart"sv;
            stop_unlocked();
            if (owned_ || pid_ > 0 || leader_pidfd_ >= 0 || marker_) {
              BOOST_LOG(error) << "gamescope_runtime: retained owned generation did not drain; refusing replacement launch"sv;
              return false;
            }
          }
          else {
            BOOST_LOG(warning) << "gamescope_runtime: dropping stale attached gamescope state"sv;
            clear_runtime_state_unlocked();
          }
        }

        ensure_portal_stack();

        // 1) Prefer attach to idle gamescope-0, but participate in the same
        // locked durable-claim protocol as nested shell recovery.
        {
          owner_transition_lock_t acquisition_lock;
          if (!acquisition_lock || !runtime_acquisition_allowed_locked()) {
            BOOST_LOG(warning) << "gamescope_runtime: nested ownership claim blocks runtime acquisition"sv;
            return false;
          }
          if (try_attach_gamescope0(params, true)) {
            return true;
          }
        }

        // 2) Ask the host idle unit to start. This helper releases the ownership
        // lock around systemd activation, then reacquires and rechecks the claim
        // before attaching.
        if (try_start_idle_unit_and_attach(params)) {
          return true;
        }

        const auto gamescope_on_path = platf::linux_util::find_executable_in_path("gamescope");
        if (gamescope_on_path.empty()) {
          BOOST_LOG(error) << "gamescope_runtime: gamescope not found on PATH"sv;
          return false;
        }
        std::error_code executable_ec;
        const auto absolute_gamescope = fs::absolute(gamescope_on_path, executable_ec);
        if (executable_ec) {
          BOOST_LOG(error) << "gamescope_runtime: could not resolve gamescope executable: "sv
                           << executable_ec.message();
          return false;
        }
        const auto gamescope_executable = absolute_gamescope.lexically_normal().string();

        // 3) Spawn owned headless gamescope only inside the same ownership lock
        // and after proving no durable nested transition/recovery claim exists.
        owner_transition_lock_t acquisition_lock;
        if (!acquisition_lock || !runtime_acquisition_allowed_locked()) {
          BOOST_LOG(warning) << "gamescope_runtime: nested claim appeared before owned spawn"sv;
          return false;
        }
        // Never attach/spawn onto gamescope-1 (portal is hard-wired to gamescope-0).
        // Crash residue with no live holder is reclaimed under this transaction.
        reclaim_orphan_gamescope_sockets();
        if (socket_exists("gamescope-0") && wait_for_stable_socket("gamescope-0", 1500)) {
          if (try_attach_gamescope0(params, true)) {
            return true;
          }
          BOOST_LOG(error) << "gamescope_runtime: gamescope-0 exists but is not a validated owner; refusing destructive cleanup"sv;
          return false;
        }
        if (socket_exists("gamescope-1") && !socket_exists("gamescope-0")) {
          BOOST_LOG(error) << "gamescope_runtime: gamescope-1 exists without a validated gamescope-0 owner; refusing destructive cleanup"sv;
          return false;
        }

        const auto width = std::max(params.width, 640);
        const auto height = std::max(params.height, 480);
        const auto refresh = std::max(params.refresh_hz, 30);

        std::vector<std::string> args {
          gamescope_executable,
          "--backend",
          "headless",
          "--expose-wayland",
          "--steam",
          "--xwayland-count",
          "2",
          "-W",
          std::to_string(width),
          "-H",
          std::to_string(height),
          "-r",
          std::to_string(refresh),
          "-w",
          std::to_string(width),
          "-h",
          std::to_string(height),
          "--",
        };
        if (!params.game_cmd.empty()) {
          args.push_back("bash");
          args.push_back("-lc");
          args.push_back(params.game_cmd);
        }
        else {
          args.push_back("sleep");
          args.push_back("infinity");
        }

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (auto &a : args) {
          argv.push_back(a.data());
        }
        argv.push_back(nullptr);

        const long inherited_fd_limit = sysconf(_SC_OPEN_MAX);
        const pid_t child = fork();
        if (child < 0) {
          BOOST_LOG(error) << "gamescope_runtime: fork failed: "sv << std::strerror(errno);
          return false;
        }
        if (child == 0) {
          if (setsid() < 0) {
            _exit(126);
          }
          unsetenv("WAYLAND_DISPLAY");
          unsetenv("ENABLE_GAMESCOPE_WSI");
          unsetenv("ENABLE_HDR_WSI");
          // Prefer gamescope-0 naming when the compositor honors it.
          setenv("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0", 1);
          // The owned runtime is forked after Polaris starts its RTSP/HTTPS
          // listeners. Without this boundary, gamescope and its Xwayland
          // children keep those sockets bound after Polaris is killed.
          close_inherited_descriptors(inherited_fd_limit);
          execv(gamescope_executable.c_str(), argv.data());
          _exit(127);
        }

        leader_pidfd_ = static_cast<int>(syscall(SYS_pidfd_open, child, 0));
        if (leader_pidfd_ < 0) {
          (void) kill(child, SIGKILL);
          (void) waitpid(child, nullptr, 0);
          BOOST_LOG(error) << "gamescope_runtime: pidfd_open failed for owned leader"sv;
          return false;
        }
        pid_ = child;
        owned_ = true;
        owned_group_drained_ = false;
        socket_name_ = "gamescope-0";

        // Capture the exact PID generation only after exec has exposed the
        // expected gamescope --backend headless argv.
        // Keep the leader unreaped until every negative-PGID operation finishes;
        // its live/zombie PID allocation prevents the numeric PGID from being reused.
        for (int i = 0; i < 100 && !marker_; ++i) {
          marker_ = gp::marker_for_pid(child, "runtime");
          if (!marker_) {
            if (pidfd_has_exited(leader_pidfd_)) {
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
          }
        }
        if (marker_ && !is_private_group_leader(child)) {
          marker_.reset();
        }
        bool marker_written = false;
        if (marker_ && runtime_acquisition_allowed_locked()) {
          const auto current_owner = gp::validated_marker(marker_path());
          const bool authority_free = !current_owner || *current_owner == *marker_;
          marker_written = authority_free && gp::write_marker(marker_path(), *marker_);
        }
        if (marker_written) {
          // The exact runtime generation is now durable. Release the acquisition
          // transaction so nested recovery can observe and stop this marker.
          acquisition_lock.unlock();
        }
        if (!marker_written) {
          BOOST_LOG(error) << "gamescope_runtime: could not record exact owned gamescope generation"sv;

          const bool rollback_drained = rollback_spawned_private_group(child, leader_pidfd_);
          if (!rollback_drained) {
            BOOST_LOG(error) << "gamescope_runtime: launch rollback could not prove private group drained; preserving state"sv;
            return false;
          }
          close_leader_pidfd();
          pid_ = 0;
          owned_ = false;
          owned_group_drained_ = false;
          marker_.reset();
          socket_name_.clear();
          return false;
        }

        const bool socket_ok = wait_for_stable_socket("gamescope-0", 10000);
        if (!socket_ok) {
          // If we got gamescope-1, fail — portal cannot use it reliably.
          if (socket_exists("gamescope-1") && !socket_exists("gamescope-0")) {
            BOOST_LOG(error) << "gamescope_runtime: owned spawn bound gamescope-1; aborting (portal requires gamescope-0)"sv;
            stop_unlocked();
            return false;
          }
          if (pidfd_has_exited(leader_pidfd_)) {
            BOOST_LOG(error) << "gamescope_runtime: gamescope exited before socket appeared"sv;
            const auto marker_matches = [this]() {
              const auto marker_on_disk = gp::read_marker(marker_path());
              return marker_ && marker_on_disk && *marker_on_disk == *marker_;
            };
            if (!drain_private_process_group(pid_, leader_pidfd_, marker_matches)) {
              BOOST_LOG(error) << "gamescope_runtime: exited leader left an unverified private group; preserving ownership"sv;
              return false;
            }
            if (!remove_owned_files_if_current()) {
              BOOST_LOG(error) << "gamescope_runtime: failed to remove drained generation marker; preserving ownership"sv;
              return false;
            }
            close_leader_pidfd();
            pid_ = 0;
            owned_ = false;
            owned_group_drained_ = false;
            marker_.reset();
            socket_name_.clear();
            return false;
          }
          BOOST_LOG(error) << "gamescope_runtime: gamescope-0 never appeared (owned spawn)"sv;
          stop_unlocked();
          return false;
        }

        if (!gp::process_tree_owns_socket(*marker_, socket_path("gamescope-0"))) {
          BOOST_LOG(error) << "gamescope_runtime: gamescope-0 is not owned by the spawned generation"sv;
          stop_unlocked();
          return false;
        }
        x11_display_ = wait_for_owned_x11(*marker_, 8000);
        if (x11_display_.empty()) {
          BOOST_LOG(error) << "gamescope_runtime: spawned generation exposed no owned Xwayland socket"sv;
          stop_unlocked();
          return false;
        }
        refresh_runtime_state(params);
        if (!write_env_file() || !is_running_unlocked()) {
          BOOST_LOG(error) << "gamescope_runtime: owned gamescope-0 up but ownership/env validation failed"sv;
          stop_unlocked();
          return false;
        }
        BOOST_LOG(info) << "gamescope_runtime: started owned gamescope pid="sv << pid_
                        << " socket=gamescope-0 "sv << width << "x"sv << height
                        << "@"sv << refresh << " healthy=1"sv;
        return true;
      }

      void stop() override {
        std::lock_guard lock(mu_);
        stop_unlocked();
      }

      void reset_after_external_stop() override {
        std::lock_guard lock(mu_);
        if (pid_ > 0) {
          // Non-blocking reap only.
          int status = 0;
          waitpid(pid_, &status, WNOHANG);
        }
        close_leader_pidfd();
        pid_ = 0;
        owned_ = false;
        owned_group_drained_ = false;
        marker_.reset();
        socket_name_.clear();
        x11_display_.clear();
        state_ = {};
        state_.backend_name = "gamescope";
      }

      bool is_running() const override {
        std::lock_guard lock(mu_);
        return is_running_unlocked();
      }

      // Socket (+ owned pid) liveness is enough; former attach pgrep was equivalent
      // when the socket already exists (is_running_unlocked).
      bool is_healthy() const override {
        std::lock_guard lock(mu_);
        return is_running_unlocked();
      }

      pid_t pid() const override {
        std::lock_guard lock(mu_);
        return pid_;
      }

      // Env-only attach: process applies DISPLAY/GAMESCOPE_*/HDR via
      // apply_gamescope_attach_env. Do not dual-embed shell env prefixes here.
      std::string wrap_cmd(const std::string &cmd) const override {
        return cmd;
      }

      std::string wayland_socket() const override {
        std::lock_guard lock(mu_);
        return socket_name_;
      }

      std::string x11_display() const override {
        std::lock_guard lock(mu_);
        return x11_display_;
      }

      platf::runtime_state_t runtime_state() const override {
        std::lock_guard lock(mu_);
        return state_;
      }

    private:
      mutable std::mutex mu_;
      pid_t pid_ = 0;
      int leader_pidfd_ = -1;
      bool owned_ = false;
      // A successful drain consumes live process identity, but socket removal
      // can still fail closed on a lock/holder race. Keep that progress so a
      // later stop retries exact socket/file cleanup without trying to signal a
      // process group that has already been proven empty.
      bool owned_group_drained_ = false;
      std::optional<gp::marker_t> marker_;
      std::string socket_name_;
      std::string x11_display_;
      platf::runtime_state_t state_ {
        .requested_headless = true,
        .effective_headless = true,
        .gpu_native_override_active = false,
        .backend_name = "gamescope",
        .path_id = "gamescope_stream",
      };

      void close_leader_pidfd() {
        if (leader_pidfd_ >= 0) {
          close(leader_pidfd_);
          leader_pidfd_ = -1;
        }
      }

      fs::path marker_path() const {
        return fs::path(xdg_runtime_dir()) / "polaris-gamescope.pid";
      }

      fs::path socket_path(std::string_view name) const {
        return fs::path(xdg_runtime_dir()) / name;
      }

      std::optional<gp::marker_t> validated_marker_for_socket(
        std::string_view socket,
        std::string_view expected_role = {}
      ) const {
        auto marker = gamescope_process::validated_marker(marker_path(), expected_role);
        if (!marker || !gp::process_tree_owns_socket(*marker, socket_path(socket))) {
          return std::nullopt;
        }
        return marker;
      }

      std::string wait_for_owned_x11(const gp::marker_t &marker, int timeout_ms) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        do {
          if (const auto display = gp::discover_owned_x11_display(marker)) {
            return *display;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < deadline);
        return {};
      }

      bool is_running_unlocked() const {
        if (!marker_ || socket_name_.empty()) {
          return false;
        }
        const auto current = validated_marker_for_socket(socket_name_, marker_->role);
        return current && *current == *marker_;
      }

      bool remove_owned_files_if_current() const {
        owner_transition_lock_t owner_lock;
        if (!owner_lock) {
          return false;
        }
        return remove_owned_files_if_current_unlocked();
      }

      bool remove_owned_files_if_current_unlocked() const {
        if (!marker_) {
          return false;
        }
        const auto current = gp::read_marker(marker_path());
        if (!current || *current != *marker_) {
          return false;
        }
        std::error_code ec;
        const auto env_path = fs::path(xdg_runtime_dir()) / "polaris-gamescope.env";
        const bool env_exists = fs::exists(env_path, ec);
        if (ec || (env_exists && (!fs::remove(env_path, ec) || ec))) {
          return false;
        }
        ec.clear();
        return fs::remove(marker_path(), ec) && !ec;
      }

      bool reclaim_owned_gamescope_sockets_after_drain_unlocked() const {
        if (!owned_group_drained_ || socket_name_ != "gamescope-0") {
          BOOST_LOG(error) << "gamescope_runtime: refusing post-drain cleanup without exact gamescope-0 ownership"sv;
          return false;
        }

        if (!gp::remove_orphan_socket(socket_path("gamescope-0"))) {
          BOOST_LOG(error) << "gamescope_runtime: gamescope-0 socket remains live or ambiguous after owned drain"sv;
          return false;
        }
        if (!gp::remove_orphan_socket(socket_path("gamescope-0-ei"))) {
          BOOST_LOG(error) << "gamescope_runtime: gamescope-0-ei socket remains live or ambiguous after owned drain"sv;
          return false;
        }
        return true;
      }

      void stop_unlocked() {
        if (owned_ && !marker_) {
          BOOST_LOG(error) << "gamescope_runtime: owned runtime lacks immutable marker authority; preserving state"sv;
          return;
        }
        if (owned_ && marker_) {
          owner_transition_lock_t owner_lock;
          if (!owner_lock) {
            BOOST_LOG(error) << "gamescope_runtime: could not acquire ownership transition lock; refusing teardown"sv;
            return;
          }
          // The durable marker remains authoritative across a retry after the
          // live group has drained. Re-read it under the transition lock before
          // either signaling or consuming socket/file ownership.
          const auto marker_on_disk = gp::read_marker(marker_path());
          if (!marker_on_disk || *marker_on_disk != *marker_) {
            BOOST_LOG(warning) << "gamescope_runtime: refusing teardown with stale or replaced marker group="sv
                               << marker_->pid;
            return;
          }

          if (!owned_group_drained_) {
            const auto current = gp::validated_marker(marker_path(), "runtime");
            if (!current || *current != *marker_ || !is_private_group_leader(marker_->pid)) {
              BOOST_LOG(warning) << "gamescope_runtime: refusing to signal stale or unverified group="sv
                                 << marker_->pid;
              return;
            }
            // The pidfd keeps the unreaped leader allocation as the PGID
            // barrier. After TERM the leader may be a zombie, so escalation
            // fences the immutable marker instead of requiring live socket
            // ownership that teardown is intentionally consuming.
            const auto authority_still_current = [this]() {
              const auto marker_on_disk = gp::read_marker(marker_path());
              return marker_on_disk && *marker_on_disk == *marker_;
            };
            if (!drain_private_process_group(marker_->pid, leader_pidfd_, authority_still_current)) {
              BOOST_LOG(error) << "gamescope_runtime: private group did not drain; preserving ownership state"sv;
              return;
            }
            owned_group_drained_ = true;
            BOOST_LOG(info) << "gamescope_runtime: drained owned gamescope group="sv << marker_->pid;
          }

          if (!reclaim_owned_gamescope_sockets_after_drain_unlocked()) {
            BOOST_LOG(error) << "gamescope_runtime: failed to reclaim drained generation sockets; preserving state"sv;
            return;
          }
          if (!remove_owned_files_if_current_unlocked()) {
            BOOST_LOG(error) << "gamescope_runtime: failed to clear drained ownership files; preserving state"sv;
            return;
          }
        }
        else if (!owned_ && marker_) {
          BOOST_LOG(info) << "gamescope_runtime: detached from validated "sv << socket_name_
                          << " owner pid="sv << marker_->pid << " (left running)"sv;
        }
        close_leader_pidfd();
        pid_ = 0;
        owned_ = false;
        owned_group_drained_ = false;
        marker_.reset();
        socket_name_.clear();
        x11_display_.clear();
        state_.backend_name = "gamescope";
      }

      void refresh_runtime_state(const start_params_t &params) {
        state_.requested_headless = true;
        state_.effective_headless = true;
        state_.gpu_native_override_active = false;
        state_.backend_name = "gamescope";
        state_.path_id = "gamescope_stream";
        (void) params;
      }

      void clear_runtime_state_unlocked() {
        close_leader_pidfd();
        marker_.reset();
        owned_ = false;
        owned_group_drained_ = false;
        pid_ = 0;
        socket_name_.clear();
        x11_display_.clear();
        state_ = {};
      }

      bool revalidate_running_unlocked(const start_params_t &params, bool ownership_lock_held = false) {
        if (!marker_ || socket_name_.empty() || x11_display_.empty()) {
          return false;
        }
        const auto current = validated_marker_for_socket(socket_name_, marker_->role);
        if (!current || *current != *marker_) {
          return false;
        }
        const auto display = gp::discover_owned_x11_display(*marker_);
        if (!display || *display != x11_display_) {
          return false;
        }
        refresh_runtime_state(params);
        return write_env_file(ownership_lock_held);
      }

      static void ensure_portal_stack() {
        // Best-effort: private portal frontend can be TERM'd while polaris stays up.
        // Idle recovery alone is not enough — CreateSession needs org.freedesktop.portal.Desktop.
        (void) std::system(
          "systemctl --user start polaris-portal-dbus.service "
          "polaris-portal-gamescope.service polaris-portal.service 2>/dev/null"
        );
      }

      // Best-effort: drop crash residue only. Live holders are left for attach / fail-closed.
      static void reclaim_orphan_gamescope_sockets() {
        const auto rt = fs::path(xdg_runtime_dir());
        for (const char *name : {"gamescope-0", "gamescope-1", "gamescope-0-ei", "gamescope-1-ei"}) {
          const auto path = rt / name;
          std::error_code ec;
          if (!fs::exists(path, ec) || ec) {
            continue;
          }
          if (gp::remove_orphan_socket(path)) {
            BOOST_LOG(info) << "gamescope_runtime: reclaimed orphan socket "sv << path.string();
          }
        }
      }

      bool try_attach_gamescope0(const start_params_t &params, bool ownership_lock_held = false) {
        if (!wait_for_stable_socket("gamescope-0", 2000)) {
          return false;
        }
        const auto marker = validated_marker_for_socket("gamescope-0");
        if (!marker) {
          BOOST_LOG(warning) << "gamescope_runtime: refusing unmarked/unowned gamescope-0 socket"sv;
          return false;
        }
        const auto display = wait_for_owned_x11(*marker, 5000);
        if (display.empty()) {
          BOOST_LOG(warning) << "gamescope_runtime: marked gamescope-0 has no owned Xwayland socket"sv;
          return false;
        }
        close_leader_pidfd();
        marker_ = marker;
        owned_ = false;
        owned_group_drained_ = false;
        pid_ = marker->pid;
        socket_name_ = "gamescope-0";
        x11_display_ = display;
        refresh_runtime_state(params);
        if (!write_env_file(ownership_lock_held)) {
          marker_.reset();
          pid_ = 0;
          socket_name_.clear();
          x11_display_.clear();
          return false;
        }
        BOOST_LOG(info) << "gamescope_runtime: attached to validated gamescope-0 owner pid="sv
                        << marker->pid << " generation="sv << marker->start_time
                        << " display="sv << display;
        return true;
      }

      // polaris-gamescope-idle reads $XDG_RUNTIME_DIR/polaris-gamescope-force at start for --hdr-enabled.
      // process.cpp writes the file but does not restart the unit — so an idle started in
      // SDR stays SDR while portal claims HDR (BGRx + PQ washout). Restart when needed.
      static bool idle_hdr_flags_match_force() {
        const bool want_hdr = read_hdr_force();
        const auto marker_path = fs::path(xdg_runtime_dir()) / "polaris-gamescope.pid";
        const auto marker = gp::validated_marker(marker_path, "idle");
        if (!marker || !gp::process_tree_owns_socket(*marker, fs::path(xdg_runtime_dir()) / "gamescope-0")) {
          return false;
        }
        return gp::process_has_argument(*marker, "--hdr-enabled") == want_hdr;
      }

      static bool restart_or_start_idle_unit() {
        ensure_portal_stack();
        const int rc = std::system(
          "systemctl --user restart polaris-gamescope-idle.service 2>/dev/null || "
          "systemctl --user start polaris-gamescope-idle.service 2>/dev/null");
        return rc == 0;
      }

      bool try_start_idle_unit_and_attach(const start_params_t &params) {
        // Best-effort: host may ship polaris-gamescope-idle.service. Its own
        // startup transaction reclaims residue under the shared ownership lock.
        const bool active =
          std::system("systemctl --user is-active --quiet polaris-gamescope-idle.service 2>/dev/null") == 0;
        const bool activating =
          std::system(
            "test \"$(systemctl --user show -p ActiveState --value polaris-gamescope-idle.service 2>/dev/null)\" = activating") == 0;
        if ((active || activating) && idle_hdr_flags_match_force()) {
          if (wait_for_stable_socket("gamescope-0", 8000)) {
            owner_transition_lock_t acquisition_lock;
            if (!acquisition_lock || !runtime_acquisition_allowed_locked()) {
              return false;
            }
            return try_attach_gamescope0(params, true);
          }
        }
        if (active && !idle_hdr_flags_match_force()) {
          BOOST_LOG(info) << "gamescope_runtime: restarting polaris-gamescope-idle to match polaris-gamescope-force"sv;
        }
        if (restart_or_start_idle_unit()) {
          if (wait_for_stable_socket("gamescope-0", 12000)) {
            owner_transition_lock_t acquisition_lock;
            if (!acquisition_lock || !runtime_acquisition_allowed_locked()) {
              return false;
            }
            if (try_attach_gamescope0(params, true)) {
              BOOST_LOG(info) << "gamescope_runtime: polaris-gamescope-idle ready (HDR flags synced); attached to gamescope-0"sv;
              return true;
            }
          }
          BOOST_LOG(warning) << "gamescope_runtime: polaris-gamescope-idle started but gamescope-0 not ready; will try owned spawn"sv;
        }
        return false;
      }

      bool write_env_file(bool ownership_lock_held = false) const {
        if (!marker_ || x11_display_.empty() || socket_name_.empty()) {
          return false;
        }
        owner_transition_lock_t owner_lock(!ownership_lock_held);
        if ((!ownership_lock_held && !owner_lock) || !runtime_acquisition_allowed_locked()) {
          return false;
        }
        const auto revalidate_publication_pair = [this]() {
          const auto current = validated_marker_for_socket(socket_name_, marker_->role);
          const auto current_display = gp::discover_owned_x11_display(*marker_);
          return current && *current == *marker_ &&
                 current_display && *current_display == x11_display_;
        };
        if (!revalidate_publication_pair()) {
          return false;
        }

        const auto path = fs::path(xdg_runtime_dir()) / "polaris-gamescope.env";
        const auto temporary = path.string() + ".tmp." + std::to_string(getpid());
        {
          std::ofstream out(temporary, std::ios::trunc);
          if (!out) {
            return false;
          }
          out << "WAYLAND_DISPLAY=" << socket_name_ << "\n";
          out << "GAMESCOPE_WAYLAND_DISPLAY=" << socket_name_ << "\n";
          out << "DISPLAY=" << x11_display_ << "\n";
          out << "POLARIS_GAMESCOPE_PID=" << marker_->pid << "\n";
          out << "POLARIS_GAMESCOPE_START_TIME=" << marker_->start_time << "\n";
          out << "POLARIS_GAMESCOPE_ROLE=" << marker_->role << "\n";
          out << "POLARIS_GAMESCOPE_EXECUTABLE=" << marker_->executable.string() << "\n";
        }
        std::error_code ec;
        if (!revalidate_publication_pair()) {
          fs::remove(temporary, ec);
          return false;
        }
        fs::rename(temporary, path, ec);
        if (ec) {
          fs::remove(temporary, ec);
          return false;
        }
        return true;
      }
    };

    // Process-lifetime singleton (not shared ownership).
    gamescope_runtime_t g_gamescope_runtime;

  }  // namespace

  bool drain_gamescope_private_group_for_tests(pid_t pgid) {
    const int pidfd = static_cast<int>(syscall(SYS_pidfd_open, pgid, 0));
    if (pidfd < 0) {
      return false;
    }
    const bool drained = drain_private_process_group(pgid, pidfd, []() { return true; }, 3, 20);
    close(pidfd);
    return drained;
  }

  bool rollback_gamescope_spawn_for_tests(pid_t pgid, int leader_pidfd) {
    return rollback_spawned_private_group(pgid, leader_pidfd);
  }

  bool gamescope_runtime_acquisition_allowed_for_tests() {
    owner_transition_lock_t owner_lock;
    return owner_lock && runtime_acquisition_allowed_locked();
  }

  bool gamescope_runtime_closes_inherited_descriptors_for_tests() {
    int descriptors[2] = {-1, -1};
    if (pipe2(descriptors, O_CLOEXEC) != 0) {
      return false;
    }
    const long fallback_limit = sysconf(_SC_OPEN_MAX);
    const pid_t child = fork();
    if (child < 0) {
      close(descriptors[0]);
      close(descriptors[1]);
      return false;
    }
    if (child == 0) {
      const int probe_fd = descriptors[0];
      close_inherited_descriptors(fallback_limit);
      const bool closed = fcntl(probe_fd, F_GETFD) == -1 && errno == EBADF;
      _exit(closed ? 0 : 1);
    }

    close(descriptors[0]);
    close(descriptors[1]);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
      if (errno != EINTR) {
        return false;
      }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }

  stream_runtime_t *gamescope_runtime_instance() {
    return &g_gamescope_runtime;
  }

}  // namespace stream_runtime

#endif
