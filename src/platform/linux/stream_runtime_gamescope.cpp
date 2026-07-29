/**
 * @file src/platform/linux/stream_runtime_gamescope.cpp
 * @brief Owned / attach Gamescope private stream runtime.
 *
 * Prefer attaching to an existing gamescope-0 (idle unit). If absent, spawn a
 * headless gamescope that owns gamescope-0 so portal capture stays stable.
 */

#include "stream_runtime.h"

#ifdef __linux__

  #include "stream_path.h"
  #include "gamescope_process.h"
  #include "src/logging.h"

  #include <chrono>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <fcntl.h>
  #include <mutex>
  #include <optional>
  #include <signal.h>
  #include <string>
  #include <string_view>
  #include <sys/file.h>
  #include <sys/stat.h>
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

    class owner_transition_lock_t {
    public:
      owner_transition_lock_t() {
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

    private:
      int fd_ = -1;
    };

    std::string xdg_runtime_dir() {
      if (const char *xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
        return xdg;
      }
      return "/run/user/" + std::to_string(getuid());
    }

    bool socket_exists(const std::string &name) {
      return fs::exists(xdg_runtime_dir() + "/" + name);
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
        if (is_running_unlocked()) {
          // Crash/teardown can leave in-memory attach state while gamescope is dead.
          // Re-validate exact generation + socket ownership before reuse.
          if (revalidate_running_unlocked(params)) {
            BOOST_LOG(info) << "gamescope_runtime: already "sv
                            << (owned_ ? "owned"sv : "attached"sv)
                            << " "sv << (socket_name_.empty() ? "gamescope-0" : socket_name_)
                            << "; reusing"sv;
            return true;
          }
          BOOST_LOG(warning) << "gamescope_runtime: dropping stale in-memory gamescope state"sv;
          clear_runtime_state_unlocked();
        }

        ensure_portal_stack();

        // 1) Prefer attach to idle gamescope-0 (portal + polaris-gamescope.env contract).
        if (try_attach_gamescope0(params)) {
          return true;
        }

        // 2) Ask the host idle unit to start (if present) before we spawn our own.
        if (try_start_idle_unit_and_attach(params)) {
          return true;
        }

        if (!stream_path::binary_on_path("gamescope")) {
          BOOST_LOG(error) << "gamescope_runtime: gamescope not found on PATH"sv;
          return false;
        }

        // 3) Spawn owned headless gamescope — only if gamescope-0 is still free.
        // Never attach/spawn onto gamescope-1 (portal is hard-wired to gamescope-0).
        // Crash residue with no live holder is reclaimed; live unowned holders fail closed.
        reclaim_orphan_gamescope_sockets();
        if (socket_exists("gamescope-0") && wait_for_stable_socket("gamescope-0", 1500)) {
          if (try_attach_gamescope0(params)) {
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
          "gamescope",
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

        const pid_t child = fork();
        if (child < 0) {
          BOOST_LOG(error) << "gamescope_runtime: fork failed: "sv << std::strerror(errno);
          return false;
        }
        if (child == 0) {
          setsid();
          unsetenv("WAYLAND_DISPLAY");
          unsetenv("ENABLE_GAMESCOPE_WSI");
          unsetenv("ENABLE_HDR_WSI");
          // Prefer gamescope-0 naming when the compositor honors it.
          setenv("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0", 1);
          execvp("gamescope", argv.data());
          _exit(127);
        }

        pid_ = child;
        owned_ = true;
        socket_name_ = "gamescope-0";

        // Capture the exact PID generation only after exec has exposed the
        // expected gamescope --backend headless argv.
        bool child_reaped = false;
        for (int i = 0; i < 100 && !marker_; ++i) {
          marker_ = gp::marker_for_pid(child, "runtime");
          if (!marker_) {
            int status = 0;
            if (waitpid(child, &status, WNOHANG) == child) {
              child_reaped = true;
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
          }
        }
        bool marker_written = false;
        if (marker_) {
          owner_transition_lock_t owner_lock;
          if (owner_lock) {
            const auto current_owner = gp::validated_marker(marker_path());
            const bool authority_free = !current_owner ||
              *current_owner == *marker_;
            marker_written = authority_free && gp::write_marker(marker_path(), *marker_);
          }
        }
        if (!marker_written) {
          BOOST_LOG(error) << "gamescope_runtime: could not record exact owned gamescope generation"sv;
          // An unreaped child PID cannot be reused. Re-check that relationship
          // immediately before signaling; if the loop already reaped it (or it
          // is no longer our child), the numeric PID is no longer authority.
          if (!child_reaped) {
            int status = 0;
            const auto child_state = waitpid(child, &status, WNOHANG);
            if (child_state == 0) {
              kill(child, SIGTERM);
              waitpid(child, nullptr, 0);
            }
          }
          pid_ = 0;
          owned_ = false;
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
          int status = 0;
          if (waitpid(pid_, &status, WNOHANG) == pid_) {
            BOOST_LOG(error) << "gamescope_runtime: gamescope exited before socket appeared"sv;
            remove_owned_files_if_current();
            pid_ = 0;
            owned_ = false;
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
        pid_ = 0;
        owned_ = false;
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
      bool owned_ = false;
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

      void remove_owned_files_if_current() const {
        owner_transition_lock_t owner_lock;
        if (!owner_lock) {
          return;
        }
        remove_owned_files_if_current_unlocked();
      }

      void remove_owned_files_if_current_unlocked() const {
        if (!marker_) {
          return;
        }
        const auto current = gp::read_marker(marker_path());
        if (!current || *current != *marker_) {
          return;
        }
        std::error_code ec;
        fs::remove(marker_path(), ec);
        fs::remove(fs::path(xdg_runtime_dir()) / "polaris-gamescope.env", ec);
      }

      void stop_unlocked() {
        if (owned_ && marker_) {
          owner_transition_lock_t owner_lock;
          if (!owner_lock) {
            BOOST_LOG(error) << "gamescope_runtime: could not acquire ownership transition lock; refusing teardown"sv;
            return;
          }
          const auto current = gp::validated_marker(marker_path(), "runtime");
          if (current && *current == *marker_) {
            // The marker validates this exact PID generation immediately before
            // signalling its setsid-owned process group.
            kill(-marker_->pid, SIGTERM);
            for (int i = 0; i < 30; ++i) {
              int status = 0;
              const auto result = waitpid(marker_->pid, &status, WNOHANG);
              if (result == marker_->pid || (result < 0 && errno == ECHILD)) {
                break;
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (gp::is_valid_headless_gamescope(*marker_)) {
              kill(-marker_->pid, SIGKILL);
              waitpid(marker_->pid, nullptr, 0);
            }
            BOOST_LOG(info) << "gamescope_runtime: stopped owned gamescope pid="sv << marker_->pid;
          }
          else {
            BOOST_LOG(warning) << "gamescope_runtime: refusing to signal stale/reused owned pid="sv
                               << marker_->pid;
          }
          remove_owned_files_if_current_unlocked();
        }
        else if (!owned_ && marker_) {
          BOOST_LOG(info) << "gamescope_runtime: detached from validated "sv << socket_name_
                          << " owner pid="sv << marker_->pid << " (left running)"sv;
        }
        pid_ = 0;
        owned_ = false;
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
        marker_.reset();
        owned_ = false;
        pid_ = 0;
        socket_name_.clear();
        x11_display_.clear();
        state_ = {};
      }

      bool revalidate_running_unlocked(const start_params_t &params) {
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
        return write_env_file();
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

      bool try_attach_gamescope0(const start_params_t &params) {
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
        marker_ = marker;
        owned_ = false;
        pid_ = marker->pid;
        socket_name_ = "gamescope-0";
        x11_display_ = display;
        refresh_runtime_state(params);
        if (!write_env_file()) {
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
        // Best-effort: host may ship polaris-gamescope-idle.service that owns gamescope-0.
        // Clear crash residue so idle can bind gamescope-0 again.
        reclaim_orphan_gamescope_sockets();
        const bool active =
          std::system("systemctl --user is-active --quiet polaris-gamescope-idle.service 2>/dev/null") == 0;
        const bool activating =
          std::system(
            "test \"$(systemctl --user show -p ActiveState --value polaris-gamescope-idle.service 2>/dev/null)\" = activating") == 0;
        if ((active || activating) && idle_hdr_flags_match_force()) {
          if (wait_for_stable_socket("gamescope-0", 8000)) {
            return try_attach_gamescope0(params);
          }
        }
        if (active && !idle_hdr_flags_match_force()) {
          BOOST_LOG(info) << "gamescope_runtime: restarting polaris-gamescope-idle to match polaris-gamescope-force"sv;
        }
        if (restart_or_start_idle_unit()) {
          if (wait_for_stable_socket("gamescope-0", 12000) && try_attach_gamescope0(params)) {
            BOOST_LOG(info) << "gamescope_runtime: polaris-gamescope-idle ready (HDR flags synced); attached to gamescope-0"sv;
            return true;
          }
          BOOST_LOG(warning) << "gamescope_runtime: polaris-gamescope-idle started but gamescope-0 not ready; will try owned spawn"sv;
        }
        return false;
      }

      bool write_env_file() const {
        if (!marker_ || x11_display_.empty() || socket_name_.empty()) {
          return false;
        }
        owner_transition_lock_t owner_lock;
        if (!owner_lock) {
          return false;
        }
        const auto current = validated_marker_for_socket(socket_name_, marker_->role);
        const auto current_display = gp::discover_owned_x11_display(*marker_);
        if (!current || *current != *marker_ ||
            !current_display || *current_display != x11_display_) {
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

  stream_runtime_t *gamescope_runtime_instance() {
    return &g_gamescope_runtime;
  }

}  // namespace stream_runtime

#endif
