/**
 * @file src/prelaunch_transition.cpp
 * @brief Leaving Steam Game Mode before a private launch (#204): coordinator, receipt and probes.
 */
#include "prelaunch_transition.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <utility>

#ifdef __linux__
  #include "platform/linux/game_mode_host.h"
  #include "platform/linux/user_unit_override.h"

  #include <pwd.h>
  #include <unistd.h>
#endif

namespace prelaunch {
  std::string_view name(entry_e value) {
    switch (value) {
      case entry_e::rtsp: return "rtsp";
      case entry_e::console: return "console";
      case entry_e::browser: return "browser";
    }
    return "unknown";
  }

  std::string_view name(state_e value) {
    switch (value) {
      case state_e::idle: return "idle";
      case state_e::not_needed: return "not_needed";
      case state_e::refused: return "refused";
      case state_e::switching: return "switching";
      case state_e::awaiting_old_end: return "awaiting_old_end";
      case state_e::awaiting_desktop: return "awaiting_desktop";
      case state_e::awaiting_capture: return "awaiting_capture";
      case state_e::ready: return "ready";
      case state_e::failed: return "failed";
      case state_e::cancelled: return "cancelled";
    }
    return "unknown";
  }

  std::string_view name(restore_e value) {
    switch (value) {
      case restore_e::none: return "none";
      case restore_e::owned: return "owned";
      case restore_e::restored: return "restored";
      case restore_e::rolled_back: return "rolled_back";
      case restore_e::refused: return "refused";
      case restore_e::failed: return "failed";
    }
    return "unknown";
  }

  namespace {
    std::optional<std::string> read_text(const std::filesystem::path &path) {
      std::ifstream input {path, std::ios::binary};
      if (!input) return std::nullopt;
      return std::string {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
    }

    std::optional<std::int64_t> pid_of(const std::filesystem::path &entry) {
      const auto name = entry.filename().string();
      std::int64_t pid = 0;
      const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), pid);
      if (error != std::errc {} || end != name.data() + name.size() || pid <= 0) return std::nullopt;
      return pid;
    }

    /// The real uid on the status file's "Uid:" line.
    std::optional<std::uint32_t> real_uid(const std::filesystem::path &proc_dir) {
      const auto status = read_text(proc_dir / "status");
      if (!status) return std::nullopt;
      std::istringstream lines {*status};
      for (std::string line; std::getline(lines, line);) {
        if (!line.starts_with("Uid:")) continue;
        std::istringstream fields {line.substr(4)};
        std::string token;
        if (!(fields >> token)) return std::nullopt;
        std::uint64_t uid = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), uid);
        // A malformed or out-of-range uid matches nobody.
        if (error != std::errc {} || end != token.data() + token.size() || uid > UINT32_MAX) return std::nullopt;
        return static_cast<std::uint32_t>(uid);
      }
      return std::nullopt;
    }

    std::vector<std::string> argv_of(const std::filesystem::path &proc_dir) {
      std::vector<std::string> argv;
      const auto cmdline = read_text(proc_dir / "cmdline");
      if (!cmdline) return argv;
      std::string current;
      for (const char c : *cmdline) {
        if (c == '\0') {
          argv.push_back(std::move(current));
          current.clear();
        } else {
          current.push_back(c);
        }
      }
      if (!current.empty()) argv.push_back(std::move(current));
      return argv;
    }

    std::string_view basename_of(std::string_view path) {
      const auto slash = path.rfind('/');
      return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    template<class F>
    void for_each_process_of(const std::filesystem::path &proc_root, std::uint32_t uid, F &&visit) {
      std::error_code ec;
      for (std::filesystem::directory_iterator it {proc_root, ec}, end; !ec && it != end; it.increment(ec)) {
        const auto pid = pid_of(it->path());
        if (!pid) continue;
        const auto owner = real_uid(it->path());
        if (!owner || *owner != uid) continue;
        visit(*pid, it->path());
      }
    }

    outcome_t refusal(int status, std::string code, std::string message, std::string action) {
      outcome_t outcome;
      outcome.status = status;
      outcome.code = std::move(code);
      outcome.message = std::move(message);
      outcome.action = std::move(action);
      outcome.state = state_e::refused;
      return outcome;
    }

    bool iequals(std::string_view a, std::string_view b) {
      return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
      });
    }
  }  // namespace

  namespace {
    /// The start time in /proc/<pid>/stat, field 22.
    std::optional<std::uint64_t> start_ticks_of(const std::filesystem::path &dir) {
      const auto stat = read_text(dir / "stat");
      if (!stat) return std::nullopt;
      // Fields after the parenthesised comm, which may itself hold spaces and parentheses.
      const auto close = stat->rfind(')');
      if (close == std::string::npos) return std::nullopt;
      std::istringstream fields {stat->substr(close + 1)};
      std::string field;
      std::uint64_t start_ticks = 0;
      // Field 3 (state) is the first after the comm; field 22 (starttime) is the 20th.
      for (int index = 3; index <= 22 && fields >> field; ++index) {
        if (index == 22) {
          const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), start_ticks);
          if (error != std::errc {} || end != field.data() + field.size()) return std::nullopt;
        }
      }
      if (start_ticks == 0) return std::nullopt;
      return start_ticks;
    }
  }  // namespace

  std::optional<session_identity_t> process_identity(const std::filesystem::path &proc_root, std::int64_t pid) {
    if (pid <= 0) return std::nullopt;
    const auto dir = proc_root / std::to_string(pid);
    const auto start_ticks = start_ticks_of(dir);
    if (!start_ticks) return std::nullopt;

    session_identity_t identity;
    identity.leader_pid = pid;
    identity.leader_start_ticks = *start_ticks;
    if (const auto cgroup = read_text(dir / "cgroup")) {
      std::istringstream lines {*cgroup};
      for (std::string line; std::getline(lines, line);) {
        std::string_view path {line};
        while (!path.empty()) {
          const auto slash = path.find('/');
          const auto part = path.substr(0, slash);
          if (part.starts_with("session-") && part.ends_with(".scope")) identity.logind_scope = std::string {part};
          if (slash == std::string_view::npos) break;
          path.remove_prefix(slash + 1);
        }
      }
    }
    // The same process throughout: a pid reused while it was read is refused.
    if (start_ticks_of(dir) != start_ticks) return std::nullopt;
    return identity;
  }

  std::optional<session_identity_t> find_session_leader(
    const std::filesystem::path &proc_root,
    std::uint32_t uid,
    const std::vector<std::string_view> &names
  ) {
    std::optional<session_identity_t> found;
    int matches = 0;
    bool unreadable = false;
    for_each_process_of(proc_root, uid, [&](std::int64_t pid, const std::filesystem::path &dir) {
      // The uid (checked by the walk) and argv belong to the process whose start time brackets them.
      const auto before = start_ticks_of(dir);
      const auto argv = argv_of(dir);
      if (argv.empty()) return;
      const auto base = basename_of(argv.front());
      if (std::find(names.begin(), names.end(), base) == names.end()) return;
      ++matches;
      const auto identity = process_identity(proc_root, pid);
      if (!before || !identity || identity->leader_start_ticks != *before || real_uid(dir) != uid) {
        unreadable = true;
        return;
      }
      found = identity;
    });
    if (matches != 1 || unreadable || !found || !found->valid()) return std::nullopt;
    return found;
  }

  bool steam_title_running(const std::filesystem::path &proc_root, std::uint32_t uid) {
    bool running = false;
    for_each_process_of(proc_root, uid, [&](std::int64_t, const std::filesystem::path &dir) {
      if (running) return;
      const auto argv = argv_of(dir);
      bool reaper = false;
      bool launch = false;
      bool appid = false;
      for (const auto &arg : argv) {
        if (basename_of(arg) == "reaper") reaper = true;
        if (arg == "SteamLaunch") launch = true;
        if (arg.starts_with("AppId=") && arg.size() > 6) appid = true;
      }
      running = reaper && launch && appid;
    });
    return running;
  }

  bool cgroup_in_independent_service(std::string_view cgroup) {
    while (!cgroup.empty()) {
      const auto newline = cgroup.find('\n');
      auto line = cgroup.substr(0, newline);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
      const auto colon = line.rfind(':');
      const auto path = colon == std::string_view::npos ? line : line.substr(colon + 1);
      if (path.ends_with("/polaris.service") && path.find("/user@") != std::string_view::npos &&
          path.find("/session-") == std::string_view::npos) {
        return true;
      }
      if (newline == std::string_view::npos) break;
      cgroup.remove_prefix(newline + 1);
    }
    return false;
  }

  coordinator_t::coordinator_t(host_t &host, adapter_t &adapter, clock_t &clock, std::chrono::milliseconds deadline):
      host_ {host},
      adapter_ {adapter},
      clock_ {clock},
      deadline_ {std::min(deadline, k_max_deadline)} {}

  outcome_t coordinator_t::finish(const request_t &request, std::uint64_t generation, outcome_t outcome) {
    outcome.transition_generation = generation;
    std::lock_guard lock {mutex_};
    if (active_ && active_->generation == generation) active_.reset();
    if (receipt_.transition_generation == generation) {
      receipt_.state = outcome.state;
      receipt_.code = outcome.code;
      receipt_.changed_session = outcome.changed_session;
    }
    (void) request;
    return outcome;
  }

  outcome_t coordinator_t::run(const request_t &request) {
    outcome_t not_needed;
    not_needed.state = state_e::not_needed;
    // Off, and every path that binds an existing generation: no scan, no receipt, no effect.
    if (!request.policy_enabled || request.resume || request.watch_only || request.input_only) {
      return not_needed;
    }

    std::uint64_t generation = 0;
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard lock {mutex_};
      if (active_ || restoring_) {
        return refusal(409, "prelaunch_transition_busy", "Another launch is leaving Game Mode on this host.",
          "Wait for that launch to finish, then try again.");
      }
      if (receipt_.restore == restore_e::owned) {
        // A change this coordinator made is not settled yet: its launch has not committed or rolled
        // back, or its committed session has not ended. Nothing may overwrite that receipt.
        return refusal(409, "prelaunch_transition_owned", "A launch that left Game Mode still owns that change on this host.",
          "End that session first.");
      }
      generation = ++next_generation_;
      active_ = active_t {generation, request.client_uuid, cancelled};
      receipt_ = receipt_t {};
      receipt_.transition_generation = generation;
      receipt_.lifecycle_generation = request.lifecycle_generation;
      receipt_.entry = request.entry;
      receipt_.client_uuid = request.client_uuid;
    }

    const auto is_cancelled = [&]() {
      return cancelled->load() || (request.cancelled && request.cancelled());
    };

    if (request.profile_or_space) {
      return finish(request, generation, refusal(409, "prelaunch_transition_profile_unsupported",
        "This app leaves Game Mode before it launches, and a profile or Space launch cannot change the host's session.",
        "Launch it from the host's own session, or turn off \"Leave Game Mode before launch\" for this app."));
    }
    const auto &occupancy = request.occupancy;
    if (occupancy.app_running || occupancy.streams_active || occupancy.launch_pending || occupancy.browser_capture ||
        occupancy.profile_service) {
      const auto *what = occupancy.app_running ? "An app is already running" :
                         occupancy.streams_active ? "A stream is already active" :
                         occupancy.launch_pending ? "Another launch is pending" :
                         occupancy.browser_capture ? "A Browser Stream session is active" :
                                                     "Profile sessions are configured";
      return finish(request, generation, refusal(409, "prelaunch_host_occupied",
        std::string {what} + " on this host, so it cannot leave Game Mode for this launch.",
        "End the other session first."));
    }

    if (!host_.game_mode_live_fresh()) {
      outcome_t outcome;
      outcome.state = state_e::not_needed;
      return finish(request, generation, outcome);
    }
    if (!request.private_route) {
      return finish(request, generation, refusal(409, "prelaunch_private_route_required",
        "This app leaves Game Mode only for a private stream, and this launch would mirror the desktop.",
        "Set Launch as to a private stream for this app, or turn off \"Leave Game Mode before launch\"."));
    }
    if (!adapter_.implemented()) {
      // The production adapter of this build. Refused before anything changes.
      return finish(request, generation, refusal(503, "prelaunch_transition_unavailable",
        "This app is set to leave Game Mode before it launches, but this Polaris build cannot leave Game Mode: " +
          adapter_.unavailable_reason(),
        "Switch the host to Desktop Mode yourself, or turn off \"Leave Game Mode before launch\" for this app."));
    }
    if (!adapter_.command_present()) {
      // No installed command at the adapter's fixed path: nothing may be started.
      return finish(request, generation, refusal(503, "prelaunch_command_unavailable",
        "The command that leaves Game Mode is not installed on this host.",
        "Switch the host to Desktop Mode yourself."));
    }
    if (host_.local_title_running()) {
      return finish(request, generation, refusal(409, "prelaunch_local_title_running",
        "A game is open in Game Mode on the host. Leaving Game Mode would close it.",
        "Quit the game on the host first."));
    }
    if (std::string reason; !host_.service_independent(reason)) {
      return finish(request, generation, refusal(503, "prelaunch_service_not_independent",
        "Polaris would not survive leaving Game Mode: " + reason,
        "Run Polaris as its user service with headless boot enabled (polaris --setup-host --enable-headless-boot)."));
    }
    const auto pre = host_.game_mode_session();
    if (!pre || !pre->valid()) {
      return finish(request, generation, refusal(503, "prelaunch_session_identity_unavailable",
        "The Game Mode session could not be identified, so leaving it could not be tracked.",
        "Switch the host to Desktop Mode yourself."));
    }
    if (is_cancelled()) {
      auto outcome = refusal(409, "prelaunch_cancelled", "The launch was cancelled.", "");
      outcome.state = state_e::cancelled;
      return finish(request, generation, outcome);
    }
    {
      std::lock_guard lock {mutex_};
      receipt_.pre = *pre;
      receipt_.state = state_e::switching;
    }

    note_stage("transition_start");
    if (!adapter_.start(direction_e::to_desktop)) {
      auto outcome = refusal(503, "prelaunch_switch_failed", "Leaving Game Mode could not start.", "Switch the host to Desktop Mode yourself.");
      outcome.state = state_e::failed;
      return finish(request, generation, outcome);
    }

    // From here this generation may have changed the session. No lock is held while waiting.
    const auto deadline = clock_.now() + deadline_;
    state_e phase = state_e::switching;
    std::optional<session_identity_t> post;
    std::string capture_reason;
    outcome_t failure;
    failure.changed_session = true;
    bool done = false;
    while (!done) {
      if (is_cancelled()) {
        if (phase == state_e::switching) adapter_.abort();
        failure.status = 409;
        failure.code = "prelaunch_cancelled";
        failure.message = "The launch was cancelled while the host was leaving Game Mode.";
        failure.state = state_e::cancelled;
        break;
      }
      if (clock_.now() >= deadline) {
        if (phase == state_e::switching) adapter_.abort();
        failure.status = 504;
        failure.code = phase == state_e::switching ? "prelaunch_switch_timeout" :
                       phase == state_e::awaiting_old_end ? "prelaunch_old_session_timeout" :
                       phase == state_e::awaiting_desktop ? "prelaunch_desktop_timeout" :
                                                            "prelaunch_capture_timeout";
        failure.message = "The host did not finish leaving Game Mode in time (" + std::string {name(phase)} + ")." +
                          (capture_reason.empty() ? "" : " " + capture_reason);
        failure.action = "Try again, or switch the host to Desktop Mode yourself.";
        failure.state = state_e::failed;
        break;
      }
      switch (phase) {
        case state_e::switching:
          switch (adapter_.poll()) {
            case step_e::running: break;
            case step_e::failed:
              failure.status = 503;
              failure.code = "prelaunch_switch_failed";
              failure.message = "Leaving Game Mode failed on the host.";
              failure.action = "Switch the host to Desktop Mode yourself.";
              failure.state = state_e::failed;
              done = true;
              continue;
            case step_e::succeeded: phase = state_e::awaiting_old_end; continue;
          }
          break;
        case state_e::awaiting_old_end: {
          const auto current = host_.game_mode_session();
          if (!current || *current != *pre) {
            phase = state_e::awaiting_desktop;
            continue;
          }
          break;
        }
        case state_e::awaiting_desktop:
          if (!host_.game_mode_live_fresh()) {
            post = host_.desktop_session();
            if (post && post->valid()) {
              std::lock_guard lock {mutex_};
              receipt_.post = post;
              phase = state_e::awaiting_capture;
              continue;
            }
          }
          break;
        case state_e::awaiting_capture: {
          // Up to the hand-off: still out of Game Mode, still the desktop this launch opened, still
          // this generation's run. Anything else and the change is not published as owned.
          bool current_run = false;
          {
            std::lock_guard lock {mutex_};
            current_run = active_ && active_->generation == generation;
          }
          const auto now_desktop = host_.desktop_session();
          if (!current_run || host_.game_mode_live_fresh() || !now_desktop || *now_desktop != *post) {
            failure.status = 409;
            failure.code = "prelaunch_desktop_changed";
            failure.message = "The host's session changed while this launch waited for capture.";
            failure.action = "Try again.";
            failure.state = state_e::failed;
            done = true;
            continue;
          }
          capture_reason.clear();
          if (request.capture_ready && request.capture_ready(capture_reason)) {
            outcome_t ready;
            ready.state = state_e::ready;
            ready.changed_session = true;
            {
              std::lock_guard lock {mutex_};
              receipt_.restore = restore_e::owned;
            }
            note_stage("transition_ready");
            return finish(request, generation, ready);
          }
          if (!request.capture_ready) capture_reason = "No capture readiness check was given.";
          break;
        }
        default: break;
      }
      {
        std::lock_guard lock {mutex_};
        if (receipt_.transition_generation == generation) receipt_.state = phase;
      }
      clock_.sleep_for(k_poll_period);
    }

    // Failure or cancellation after the switch began: undo only what this generation provably changed.
    std::string reason;
    restore_e restore = restore_e::refused;
    if (post && post->valid()) {
      restore = restore_owned(*post, request.others_now, reason);
      if (restore == restore_e::restored) restore = restore_e::rolled_back;
    } else {
      reason = "the desktop session this launch would own was never identified";
    }
    {
      std::lock_guard lock {mutex_};
      if (receipt_.transition_generation == generation) {
        receipt_.restore = restore;
        receipt_.restore_reason = reason;
      }
    }
    return finish(request, generation, failure);
  }

  restore_e coordinator_t::restore_owned(const session_identity_t &post, const others_t &others, std::string &reason) {
    // Fresh, right before acting: nothing taken from the launch's start stands in for these.
    if (!others) {
      reason = "the host's other sessions could not be checked";
      return restore_e::refused;
    }
    if (std::string other; others(other)) {
      reason = other.empty() ? "another session is on the host" : other;
      return restore_e::refused;
    }
    if (host_.local_title_running()) {
      reason = "a game is open on the host";
      return restore_e::refused;
    }
    if (host_.game_mode_live_fresh()) {
      reason = "the host is already in Game Mode";
      return restore_e::refused;
    }
    const auto current = host_.desktop_session();
    if (!current || *current != post) {
      reason = "the desktop session is not the one this launch opened";
      return restore_e::refused;
    }
    if (!adapter_.implemented() || !adapter_.command_present()) {
      reason = "this build cannot return the host to Game Mode";
      return restore_e::refused;
    }
    if (!adapter_.start(direction_e::to_game_mode)) {
      reason = "returning to Game Mode could not start";
      return restore_e::failed;
    }
    const auto deadline = clock_.now() + deadline_;
    bool switched = false;
    while (clock_.now() < deadline) {
      if (!switched) {
        const auto step = adapter_.poll();
        if (step == step_e::failed) {
          reason = "returning to Game Mode failed";
          return restore_e::failed;
        }
        switched = step == step_e::succeeded;
      }
      if (switched && host_.game_mode_live_fresh()) return restore_e::restored;
      clock_.sleep_for(k_poll_period);
    }
    if (!switched) adapter_.abort();
    reason = "the host did not return to Game Mode in time";
    return restore_e::failed;
  }

  void coordinator_t::commit(std::uint64_t transition_generation) {
    std::lock_guard lock {mutex_};
    if (receipt_.transition_generation == transition_generation && receipt_.state == state_e::ready &&
        receipt_.restore == restore_e::owned) {
      receipt_.committed = true;
    }
  }

  restore_e coordinator_t::rollback(std::uint64_t transition_generation, const others_t &others) {
    session_identity_t post;
    {
      std::lock_guard lock {mutex_};
      if (receipt_.transition_generation != transition_generation || receipt_.committed ||
          receipt_.restore != restore_e::owned || !receipt_.post || restoring_) {
        return restore_e::none;
      }
      post = *receipt_.post;
      restoring_ = true;
    }
    std::string reason;
    auto result = restore_owned(post, others, reason);
    if (result == restore_e::restored) result = restore_e::rolled_back;
    std::lock_guard lock {mutex_};
    restoring_ = false;
    if (receipt_.transition_generation == transition_generation) {
      receipt_.restore = result;
      receipt_.restore_reason = reason;
    }
    return result;
  }

  restore_e coordinator_t::on_session_end(std::uint64_t transition_generation, bool ended_on_request, bool daemon_shutdown, const others_t &others) {
    session_identity_t post;
    const auto generation = transition_generation;
    {
      std::lock_guard lock {mutex_};
      if (generation == 0 || receipt_.transition_generation != generation || !receipt_.committed ||
          receipt_.restore != restore_e::owned || !receipt_.post || restoring_) {
        return restore_e::none;
      }
      if (daemon_shutdown || !ended_on_request) {
        // Only a purposeful End Session restores. The host stays where the session left it.
        receipt_.restore = restore_e::refused;
        receipt_.restore_reason = daemon_shutdown ? "Polaris is stopping" : "the session did not end on request";
        receipt_.committed = false;
        return restore_e::refused;
      }
      post = *receipt_.post;
      restoring_ = true;
    }
    std::string reason;
    const auto result = restore_owned(post, others, reason);
    std::lock_guard lock {mutex_};
    restoring_ = false;
    if (receipt_.transition_generation == generation) {
      receipt_.restore = result;
      receipt_.restore_reason = reason;
      receipt_.committed = false;
    }
    return result;
  }

  bool coordinator_t::cancel_for_client(std::string_view client_uuid) {
    std::lock_guard lock {mutex_};
    if (!active_ || client_uuid.empty() || !iequals(active_->client_uuid, client_uuid)) return false;
    active_->cancelled->store(true);
    return true;
  }

  receipt_t coordinator_t::receipt() const {
    std::lock_guard lock {mutex_};
    return receipt_;
  }

  readiness_t coordinator_t::readiness(bool enabled) const {
    readiness_t readiness;
    readiness.enabled = enabled;
    readiness.adapter_implemented = adapter_.implemented();
    readiness.command_present = adapter_.command_present();
    // Host probes are left to the launch itself; readiness here only says what this build can do.
    readiness.ready = enabled && readiness.adapter_implemented && readiness.command_present;
    if (!enabled) {
      readiness.reason = "off";
    } else if (!readiness.adapter_implemented) {
      readiness.reason = adapter_.unavailable_reason();
    } else if (!readiness.command_present) {
      readiness.reason = "the command that leaves Game Mode is not installed";
    } else {
      readiness.reason = "checked at launch";
    }
    return readiness;
  }

  unavailable_adapter_t::unavailable_adapter_t(std::filesystem::path command):
      command_ {std::move(command)} {}

  bool unavailable_adapter_t::implemented() const {
    return false;
  }

  std::string unavailable_adapter_t::unavailable_reason() const {
    return "the SteamOS session switch is not implemented in this build";
  }

  bool unavailable_adapter_t::command_present() const {
    // Presence only: a regular executable file at the one fixed path. Nothing is run, and presence
    // says nothing about the command's supported semantics.
    std::error_code ec;
    const auto status = std::filesystem::status(command_, ec);
    if (ec || !std::filesystem::is_regular_file(status)) return false;
    using std::filesystem::perms;
    return (status.permissions() & (perms::owner_exec | perms::group_exec | perms::others_exec)) != perms::none;
  }

  bool unavailable_adapter_t::start(direction_e) {
    return false;
  }

  step_e unavailable_adapter_t::poll() {
    return step_e::failed;
  }

  void unavailable_adapter_t::abort() {}

  namespace {
    class steady_clock_t final: public clock_t {
    public:
      std::chrono::steady_clock::time_point now() override {
        return std::chrono::steady_clock::now();
      }

      void sleep_for(std::chrono::milliseconds duration) override {
        std::this_thread::sleep_for(duration);
      }
    };

#ifdef __linux__
    class production_host_t final: public host_t {
    public:
      bool game_mode_live_fresh() override {
        return platf::game_mode_host::session_live_fresh();
      }

      std::optional<session_identity_t> game_mode_session() override {
        return find_session_leader("/proc", ::getuid(), {"gamescope-session", "gamescope-session-plus", "start-gamescope-session"});
      }

      std::optional<session_identity_t> desktop_session() override {
        return find_session_leader("/proc", ::getuid(), {"startplasma-wayland", "startplasma-x11"});
      }

      bool local_title_running() override {
        return steam_title_running("/proc", ::getuid());
      }

      bool service_independent(std::string &reason) override {
        const auto cgroup = read_text("/proc/self/cgroup");
        if (!cgroup || !platf::user_unit::in_polaris_service(*cgroup) || !cgroup_in_independent_service(*cgroup)) {
          reason = "it is not running as the polaris user service outside the login session.";
          return false;
        }
        const auto *pw = getpwuid(geteuid());
        if (!pw || !pw->pw_name || !pw->pw_dir) {
          reason = "the service account could not be read.";
          return false;
        }
        const auto boot = platf::game_mode_host::boot_readiness(platf::game_mode_host::default_boot_paths(pw->pw_name, pw->pw_dir));
        if (!boot.independent()) {
          reason = "headless boot is not enabled for this account.";
          return false;
        }
        return true;
      }
    };
#else
    class production_host_t final: public host_t {
    public:
      bool game_mode_live_fresh() override {
        return false;
      }

      std::optional<session_identity_t> game_mode_session() override {
        return std::nullopt;
      }

      std::optional<session_identity_t> desktop_session() override {
        return std::nullopt;
      }

      bool local_title_running() override {
        return true;
      }

      bool service_independent(std::string &reason) override {
        reason = "this platform has no Steam Game Mode.";
        return false;
      }
    };
#endif

    struct registry_t {
      std::mutex mutex;
      production_host_t host;
      unavailable_adapter_t adapter;
      steady_clock_t clock;
      std::unique_ptr<coordinator_t> production;
      std::unique_ptr<coordinator_t> test;
      std::function<void(std::string_view)> stage_sink;
    };

    registry_t &registry() {
      static registry_t value;
      return value;
    }
  }  // namespace

  coordinator_t &coordinator() {
    auto &value = registry();
    std::lock_guard lock {value.mutex};
    if (value.test) return *value.test;
    if (!value.production) value.production = std::make_unique<coordinator_t>(value.host, value.adapter, value.clock);
    return *value.production;
  }

  void set_dependencies_for_tests(host_t *host, adapter_t *adapter, clock_t *clock, std::chrono::milliseconds deadline) {
    auto &value = registry();
    std::lock_guard lock {value.mutex};
    if (host && adapter && clock) {
      value.test = std::make_unique<coordinator_t>(*host, *adapter, *clock, deadline);
    } else {
      value.test.reset();
    }
  }

  void note_stage(std::string_view stage) {
    std::function<void(std::string_view)> sink;
    {
      auto &value = registry();
      std::lock_guard lock {value.mutex};
      sink = value.stage_sink;
    }
    if (sink) sink(stage);
  }

  void set_stage_sink_for_tests(std::function<void(std::string_view)> sink) {
    auto &value = registry();
    std::lock_guard lock {value.mutex};
    value.stage_sink = std::move(sink);
  }

  launch_scope_t::launch_scope_t(const outcome_t &outcome):
      generation_ {outcome.transition_generation},
      owned_ {outcome.state == state_e::ready && outcome.changed_session} {}

  launch_scope_t::~launch_scope_t() {
    if (owned_ && !committed_) {
      (void) coordinator().rollback(generation_, others_);
    }
  }

  std::uint64_t launch_scope_t::commit() {
    const bool commits = owned_ && !committed_;
    if (commits) coordinator().commit(generation_);
    committed_ = true;
    return commits ? generation_ : 0;
  }

  std::uint64_t launch_scope_t::owned_generation() const {
    return owned_ && !committed_ ? generation_ : 0;
  }

  void launch_scope_t::set_others(coordinator_t::others_t others) {
    others_ = std::move(others);
  }
}  // namespace prelaunch
