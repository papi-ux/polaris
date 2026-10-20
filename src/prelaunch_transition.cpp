/**
 * @file src/prelaunch_transition.cpp
 * @brief Leaving Steam Game Mode before a private launch (#204): coordinator, receipt and probes.
 */
#include "prelaunch_transition.h"
#include "logging.h"

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
  #ifdef POLARIS_BUILD_GIO
    #include "platform/linux/steamos_adapter.h"
  #endif

  #include <pwd.h>
  #include <unistd.h>
#endif

namespace prelaunch {
  namespace {
    /// Polaris is stopping. Latched once, lock-free, before any coordinator may exist: every
    /// coordinator the registry makes, whenever it is made, reads it.
    std::atomic<bool> process_stop {false};
  }  // namespace

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

  namespace {
    /// The HTTP status a refusal code is reported with.
    int status_of(std::string_view code) {
      if (code == "prelaunch_cancelled") return 409;
      if (code == "prelaunch_session_read_timeout") return 504;
      return 503;
    }

    bool is_new_session(const session_identity_t &now, const session_identity_t &old) {
      return now.valid() && now.boot_id == old.boot_id && now.uid == old.uid && now.logind_id != old.logind_id &&
             now.target_invocation != old.target_invocation;
    }
  }  // namespace

  coordinator_t::coordinator_t(host_t &host, adapter_t &adapter, clock_t &clock, budgets_t budgets, bool follow_process_stop):
      host_ {host},
      adapter_ {adapter},
      clock_ {clock},
      budgets_ {budgets},
      follow_process_stop_ {follow_process_stop} {}

  coordinator_t::~coordinator_t() {
    shutdown();
  }

  bool coordinator_t::stopping() const {
    return stopping_.load() || (follow_process_stop_ && process_stop.load());
  }

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
      if (stopping()) {
        return refusal(503, "prelaunch_shutting_down", "Polaris is stopping, so this launch cannot leave Game Mode.", "Try again once Polaris is running.");
      }
      if (active_ || restoring_) {
        return refusal(409, "prelaunch_transition_busy", "Another launch is leaving or returning to Game Mode on this host.",
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
      receipt_.cancellation_transport = request.cancelled ? "launch" : "unsupported";
    }

    const std::function<bool()> is_cancelled = [this, cancelled, entry = request.cancelled]() {
      return cancelled->load() || stopping() || (entry && entry());
    };
    // Everything up to a ready transition, reads included, shares one deadline.
    const wait_t forward {clock_.now() + budgets_.forward, is_cancelled};

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

    // Whether anything is needed is decided from the typed records alone: the account's Game Mode
    // unit as its own user manager reports it. Not read, or in between, is a refusal for an app
    // that opted in, never "not in Game Mode".
    const auto seen = host_.observe(nullptr, forward, true);
    if (!seen.read.ok()) {
      return finish(request, generation, refusal(status_of(seen.read.code), seen.read.code,
        "This app leaves Game Mode before it launches, and the host's sessions could not be read: " + seen.read.reason,
        "Try again, or turn off \"Leave Game Mode before launch\" for this app."));
    }
    switch (seen.game_mode) {
      case steamos::game_mode_e::absent:
      case steamos::game_mode_e::inactive: {
        outcome_t outcome;
        outcome.state = state_e::not_needed;
        return finish(request, generation, outcome);
      }
      case steamos::game_mode_e::active: break;
      default:
        return finish(request, generation, refusal(409, "prelaunch_session_mode_unknown",
          "Whether the host is in Game Mode is not known right now (Game Mode is starting or stopping, or was not read).",
          "Try again in a moment."));
    }
    if (!request.private_route) {
      return finish(request, generation, refusal(409, "prelaunch_private_route_required",
        "This app leaves Game Mode only for a private stream, and this launch would mirror the desktop.",
        "Set Launch as to a private stream for this app, or turn off \"Leave Game Mode before launch\"."));
    }
    // Support is proved again for every launch: the command, its package and this account's manager.
    if (const auto support = adapter_.support(forward); !support.ok()) {
      return finish(request, generation, refusal(status_of(support.code), support.code,
        "This app is set to leave Game Mode before it launches, but this host cannot leave Game Mode for it: " + support.reason,
        "Switch the host to Desktop Mode yourself, or turn off \"Leave Game Mode before launch\" for this app."));
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
    if (!seen.service.ok()) {
      return finish(request, generation, refusal(503, "prelaunch_service_not_independent",
        "Polaris would not survive leaving Game Mode: " + seen.service.reason,
        "Run Polaris as its user service with headless boot enabled (polaris --setup-host --enable-headless-boot)."));
    }
    if (!seen.bind.ok() || !seen.session || !seen.session->valid()) {
      const auto code = seen.bind.ok() ? std::string {"prelaunch_session_identity_unavailable"} : seen.bind.code;
      return finish(request, generation, refusal(503, code,
        "The Game Mode session could not be bound, so leaving it could not be tracked: " +
          (seen.bind.ok() ? std::string {"no session was identified."} : seen.bind.reason),
        "Switch the host to Desktop Mode yourself."));
    }
    if (seen.session->mode != mode_e::game) {
      return finish(request, generation, refusal(409, "prelaunch_session_mode_unknown",
        "Game Mode is running, but not in the session it would leave: the session's mode is not known.",
        "Switch the host to Desktop Mode yourself."));
    }
    const auto pre = *seen.session;
    if (is_cancelled()) {
      auto outcome = refusal(409, "prelaunch_cancelled", "The launch was cancelled.", "");
      outcome.state = state_e::cancelled;
      return finish(request, generation, outcome);
    }
    {
      std::lock_guard lock {mutex_};
      receipt_.pre = pre;
      receipt_.state = state_e::switching;
    }

    if (is_cancelled()) {
      // Checked again right before the effect: a cancellation or a stop during the proofs wins.
      auto outcome = refusal(409, stopping() ? "prelaunch_shutting_down" : "prelaunch_cancelled", "The launch was cancelled.", "");
      outcome.state = state_e::cancelled;
      return finish(request, generation, outcome);
    }
    note_stage("transition_start");
    if (!adapter_.start(direction_e::to_desktop)) {
      // Nothing ran.
      auto outcome = refusal(503, "prelaunch_switch_failed", "Leaving Game Mode could not start.", "Switch the host to Desktop Mode yourself.");
      outcome.state = state_e::failed;
      return finish(request, generation, outcome);
    }

    // From here this generation may have changed the session. No lock is held while waiting.
    const auto child_deadline = std::min(clock_.now() + budgets_.child, forward.deadline);
    state_e phase = state_e::switching;
    std::optional<session_identity_t> post;
    std::string wait_reason;
    outcome_t failure;
    failure.changed_session = true;
    bool outcome_unknown = false;
    bool done = false;
    const auto fail = [&](int status, std::string code, std::string message, std::string action, state_e state) {
      failure.status = status;
      failure.code = std::move(code);
      failure.message = std::move(message);
      failure.action = std::move(action);
      failure.state = state;
      done = true;
    };
    while (!done) {
      if (is_cancelled()) {
        if (phase == state_e::switching) {
          // Stopped after it started: it may already have asked the host to leave Game Mode.
          adapter_.abort();
          outcome_unknown = true;
          fail(409, "prelaunch_switch_outcome_unknown",
            "The launch was cancelled while the host was leaving Game Mode; the switch may already have been sent.",
            "Check the host before launching again.", state_e::cancelled);
        } else {
          fail(409, "prelaunch_cancelled", "The launch was cancelled while the host was leaving Game Mode.", "", state_e::cancelled);
        }
        break;
      }
      const auto now = clock_.now();
      if (phase == state_e::switching && now >= child_deadline) {
        adapter_.abort();
        outcome_unknown = true;
        fail(504, "prelaunch_switch_outcome_unknown",
          "The command that leaves Game Mode did not finish in time and was stopped; the switch may already have been sent.",
          "Check the host, then try again or switch it to Desktop Mode yourself.", state_e::failed);
        break;
      }
      if (now >= forward.deadline) {
        fail(504,
          phase == state_e::awaiting_old_end ? "prelaunch_old_session_timeout" :
          phase == state_e::awaiting_desktop ? "prelaunch_desktop_timeout" :
                                               "prelaunch_capture_timeout",
          "The host did not finish leaving Game Mode in time (" + std::string {name(phase)} + ")." +
            (wait_reason.empty() ? "" : " " + wait_reason),
          "Try again, or switch the host to Desktop Mode yourself.", state_e::failed);
        break;
      }
      switch (phase) {
        case state_e::switching:
          switch (adapter_.poll()) {
            case step_e::running: break;
            case step_e::failed:
              // It ran and reported failure: what it did before failing is not known.
              outcome_unknown = true;
              fail(503, "prelaunch_switch_failed", "Leaving Game Mode failed on the host.", "Switch the host to Desktop Mode yourself.", state_e::failed);
              continue;
            case step_e::succeeded:
              // Exit 0 says only that the request was sent. The old session must be seen to end.
              phase = state_e::awaiting_old_end;
              continue;
          }
          break;
        case state_e::awaiting_old_end:
        case state_e::awaiting_desktop: {
          const auto now_seen = host_.observe(&pre, forward);
          if (!now_seen.read.ok()) {
            wait_reason = now_seen.read.reason;
            break;
          }
          if (now_seen.session && now_seen.session->boot_id != pre.boot_id) {
            fail(409, "prelaunch_session_changed_during_switch", "The host restarted while it was leaving Game Mode.", "Try again.", state_e::failed);
            continue;
          }
          if (!now_seen.retired) {
            wait_reason = "the Game Mode session has not ended yet";
            break;
          }
          phase = state_e::awaiting_desktop;
          if (!now_seen.session || now_seen.session->mode != mode_e::desktop || !is_new_session(*now_seen.session, pre)) {
            wait_reason = now_seen.bind.ok() ? "no new desktop session yet" : now_seen.bind.reason;
            break;
          }
          post = *now_seen.session;
          {
            std::lock_guard lock {mutex_};
            receipt_.post = post;
          }
          phase = state_e::awaiting_capture;
          continue;
        }
        case state_e::awaiting_capture: {
          // Up to the hand-off: still this run, still the desktop this launch opened, read fresh
          // before and after the capture check. Anything else and the change is not published as owned.
          bool read_failed = false;
          const auto same_desktop = [&](std::string &reason) {
            {
              std::lock_guard lock {mutex_};
              if (!active_ || active_->generation != generation) return false;
            }
            const auto now_seen = host_.observe(&pre, forward);
            read_failed = !now_seen.read.ok();
            if (read_failed) {
              reason = now_seen.read.reason;
              return false;
            }
            return now_seen.session && *now_seen.session == *post && now_seen.game_mode == steamos::game_mode_e::inactive;
          };
          std::string read_reason;
          if (!same_desktop(read_reason)) {
            if (!read_reason.empty()) {
              // Not read is not changed: wait for a read, within the deadline.
              wait_reason = read_reason;
              break;
            }
            fail(409, "prelaunch_desktop_changed", "The host's session changed while this launch waited for capture.", "Try again.", state_e::failed);
            continue;
          }
          wait_reason.clear();
          if (request.capture_ready && request.capture_ready(wait_reason)) {
            // Capture and the last typed read share the original forward deadline. Either can
            // consume the remaining time or observe a caller cancellation; never publish ready
            // from that late answer. The existing loop handles failure and owned rollback.
            if (is_cancelled() || clock_.now() >= forward.deadline) continue;
            const bool desktop_unchanged = same_desktop(read_reason);
            if (is_cancelled() || clock_.now() >= forward.deadline) continue;
            if (!desktop_unchanged) {
              if (read_failed) {
                wait_reason = read_reason;
                break;
              }
              fail(409, "prelaunch_desktop_changed", "The host's session changed while this launch checked capture.", "Try again.", state_e::failed);
              continue;
            }
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
          if (!request.capture_ready) wait_reason = "No capture readiness check was given.";
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

    // Failure or cancellation after the switch began: undo only what this generation provably
    // changed, with its own budget. A cancelled launch is still rolled back; only shutdown stops it.
    std::string reason;
    restore_e restore = restore_e::refused;
    if (post && post->valid()) {
      const wait_t back {clock_.now() + budgets_.rollback, [this]() {
                           return stopping();
                         }};
      bool unknown = false;
      restore = restore_owned(*post, request.others_now, back, reason, unknown);
      outcome_unknown = outcome_unknown || unknown;
      if (restore == restore_e::restored) restore = restore_e::rolled_back;
    } else {
      // The command ran, and what followed was never bound: whatever it did stands, unadopted.
      outcome_unknown = true;
      reason = "the desktop session this launch would own was never bound; nothing it may have changed is undone";
    }
    {
      std::lock_guard lock {mutex_};
      if (receipt_.transition_generation == generation) {
        receipt_.restore = restore;
        receipt_.restore_reason = reason;
        receipt_.outcome_unknown = outcome_unknown;
      }
    }
    return finish(request, generation, failure);
  }

  restore_e coordinator_t::restore_owned(const session_identity_t &post, const others_t &others, const wait_t &wait, std::string &reason, bool &outcome_unknown) {
    // Fresh, right before acting: nothing taken from the launch's start stands in for these.
    if (stopping()) {
      reason = "Polaris is stopping";
      return restore_e::refused;
    }
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
    const auto seen = host_.observe(nullptr, wait, true);
    if (!seen.read.ok()) {
      reason = "the host's session could not be read: " + seen.read.reason;
      return restore_e::refused;
    }
    if (seen.game_mode != steamos::game_mode_e::inactive) {
      reason = seen.game_mode == steamos::game_mode_e::active ? "the host is already in Game Mode" : "whether the host is in Game Mode is not known";
      return restore_e::refused;
    }
    if (!seen.session || *seen.session != post) {
      reason = "the desktop session is not the one this launch opened";
      return restore_e::refused;
    }
    if (!seen.service.ok()) {
      reason = "Polaris would not survive returning to Game Mode: " + seen.service.reason;
      return restore_e::refused;
    }
    if (std::string why; !host_.service_independent(why)) {
      reason = "Polaris would not survive returning to Game Mode: " + why;
      return restore_e::refused;
    }
    if (!adapter_.implemented()) {
      reason = "this build cannot return the host to Game Mode";
      return restore_e::refused;
    }
    if (const auto support = adapter_.support(wait); !support.ok()) {
      reason = "returning to Game Mode is not supported now: " + support.reason;
      return restore_e::refused;
    }
    if (stopping() || (wait.cancelled && wait.cancelled())) {
      // Checked again right before the effect: a stop that began during the proofs above wins.
      reason = "Polaris is stopping";
      return restore_e::refused;
    }
    if (!adapter_.start(direction_e::to_game_mode)) {
      reason = "returning to Game Mode could not start";
      return restore_e::failed;
    }
    const auto child_deadline = std::min(clock_.now() + budgets_.child, wait.deadline);
    bool sent = false;
    for (;;) {
      const bool stop = wait.cancelled && wait.cancelled();
      if (!sent) {
        const auto step = adapter_.poll();
        if (step == step_e::failed) {
          outcome_unknown = true;
          reason = "returning to Game Mode failed on the host";
          return restore_e::failed;
        }
        sent = step == step_e::succeeded;
        if (!sent && (stop || clock_.now() >= child_deadline)) {
          adapter_.abort();
          outcome_unknown = true;
          reason = stop ? "Polaris stopped while returning to Game Mode; the switch may already have been sent" :
                          "the command that returns to Game Mode did not finish in time; the switch may already have been sent";
          return restore_e::failed;
        }
      }
      if (sent) {
        // Exit 0 only sent the request: the desktop must end and a new Game Mode session appear.
        const auto now_seen = host_.observe(&post, wait);
        if (now_seen.read.ok() && now_seen.retired && now_seen.session && now_seen.session->mode == mode_e::game &&
            is_new_session(*now_seen.session, post)) {
          return restore_e::restored;
        }
        if (stop || clock_.now() >= wait.deadline) {
          outcome_unknown = true;
          reason = "the host did not return to Game Mode in time";
          return restore_e::failed;
        }
      }
      clock_.sleep_for(k_poll_period);
    }
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
    bool unknown = false;
    const wait_t wait {clock_.now() + budgets_.rollback, [this]() {
                         return stopping();
                       }};
    auto result = restore_owned(post, others, wait, reason, unknown);
    if (result == restore_e::restored) result = restore_e::rolled_back;
    std::lock_guard lock {mutex_};
    restoring_ = false;
    if (receipt_.transition_generation == transition_generation) {
      receipt_.restore = result;
      receipt_.restore_reason = reason;
      receipt_.outcome_unknown = receipt_.outcome_unknown || unknown;
    }
    return result;
  }

  restore_e coordinator_t::on_session_end(std::uint64_t transition_generation, bool ended_on_request, bool daemon_shutdown, const others_t &others,
                                          const admission_fence_t &fence) {
    const auto generation = transition_generation;
    std::lock_guard lock {mutex_};
    if (generation == 0 || receipt_.transition_generation != generation || !receipt_.committed ||
        receipt_.restore != restore_e::owned || !receipt_.post || restoring_) {
      return restore_e::none;
    }
    const auto settle = [&](std::string reason) {
      // The host stays where the session left it.
      receipt_.restore = restore_e::refused;
      receipt_.restore_reason = std::move(reason);
      receipt_.committed = false;
      return restore_e::refused;
    };
    if (daemon_shutdown || stopping()) return settle("Polaris is stopping");
    if (!ended_on_request) return settle("the session did not end on request");
    // The fence is taken in the same step that claims the restore: from here no launch of any app
    // is admitted until the worker releases it, and a launch already admitted refuses the claim.
    if (fence.acquire && !fence.acquire()) return settle("a launch was being admitted when the session ended");
    // Bookkeeping only under the caller's locks. The owned serial worker proves and acts without
    // them; nothing here waits for or joins a thread.
    restoring_ = true;
    job_ = job_t {generation, *receipt_.post, others, fence.release};
    if (!worker_.joinable()) worker_ = std::thread([this]() { worker_loop(); });
    work_cv_.notify_one();
    return restore_e::owned;
  }

  void coordinator_t::worker_loop() {
    for (;;) {
      job_t job;
      {
        std::unique_lock lock {mutex_};
        work_cv_.wait(lock, [this]() { return job_.has_value() || worker_exit_; });
        if (!job_) return;
        job = std::move(*job_);
        job_.reset();
      }
      std::string reason;
      bool unknown = false;
      auto result = restore_e::failed;
      try {
        const wait_t wait {clock_.now() + budgets_.restore, [this]() {
                             return stopping();
                           }};
        result = restore_owned(job.post, job.others, wait, reason, unknown);
      } catch (...) {
        reason = "the restore stopped on an internal error";
      }
      {
        std::lock_guard lock {mutex_};
        if (receipt_.transition_generation == job.generation) {
          receipt_.restore = result;
          receipt_.restore_reason = reason;
          receipt_.committed = false;
          receipt_.outcome_unknown = receipt_.outcome_unknown || unknown;
        }
      }
      BOOST_LOG(info) << "prelaunch: return generation=" << job.generation << " result=" << name(result)
                      << " outcome_unknown=" << unknown << " reason=" << reason;
      // Launches are admitted again only once the outcome is recorded, outside the coordinator lock.
      if (job.release_fence) job.release_fence();
      {
        std::lock_guard lock {mutex_};
        restoring_ = false;
      }
      done_cv_.notify_all();
    }
  }

  void coordinator_t::wait_for_restore() {
    std::unique_lock lock {mutex_};
    done_cv_.wait(lock, [this]() { return !restoring_; });
  }

  void coordinator_t::request_stop() {
    // Every wait polls stopping(): the running transition and any restore end at their next check.
    stopping_.store(true);
  }

  void coordinator_t::shutdown() {
    request_stop();
    std::thread worker;
    {
      std::lock_guard lock {mutex_};
      worker_exit_ = true;
      worker = std::move(worker_);
    }
    work_cv_.notify_all();
    // A queued restore still runs once, refuses at once (stopping) and releases its fence.
    if (worker.joinable()) worker.join();
  }

  bool coordinator_t::restore_pending() const {
    std::lock_guard lock {mutex_};
    return restoring_;
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
    if (!enabled) {
      readiness.reason = "off";
      return readiness;
    }
    // Bounded and never activating a service; the session itself is bound by each launch.
    const auto support = adapter_.readiness_support(wait_t {clock_.now() + budgets_.readiness, {}});
    readiness.ready = readiness.adapter_implemented && support.ok();
    readiness.code = support.code;
    readiness.reason = support.ok() ? "supported; the host's session is checked at launch" : support.reason;
    return readiness;
  }

  unavailable_adapter_t::unavailable_adapter_t(std::filesystem::path command):
      command_ {std::move(command)} {}

  bool unavailable_adapter_t::implemented() const {
    return false;
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

  verdict_t unavailable_adapter_t::support(const wait_t &) {
    return verdict_t::fail("prelaunch_transition_unavailable", "this Polaris build has no SteamOS session switch.");
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
    /// The account's own process facts that are not session records: an open Steam title (a
    /// conservative refusal only, never proof of anything) and Polaris's own service placement.
    class linux_host_base_t: public host_t {
    public:
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
#endif

#if defined(__linux__) && defined(POLARIS_BUILD_GIO)
    /// Sessions from logind and this account's user manager, read on explicit buses within the wait.
    class production_host_t final: public linux_host_base_t {
    public:
      observation_t observe(const session_identity_t *since, const wait_t &wait, bool prove_service) override {
        return steamos::observe(steamos::production_sources(), since, {wait.deadline, wait.cancelled}, prove_service);
      }
    };

    using production_adapter_t = steamos::steamos_adapter_t;
#elif defined(__linux__)
    class production_host_t final: public linux_host_base_t {
    public:
      observation_t observe(const session_identity_t *, const wait_t &, bool) override {
        // Unknown, so an app that opted in is refused rather than told Game Mode is not running.
        observation_t out;
        out.read = verdict_t::fail("prelaunch_session_identity_unavailable", "this Polaris build cannot read the host's sessions.");
        return out;
      }
    };

    using production_adapter_t = unavailable_adapter_t;
#else
    class production_host_t final: public host_t {
    public:
      observation_t observe(const session_identity_t *, const wait_t &, bool) override {
        // A platform without Steam Game Mode: known absent, so nothing is ever needed here.
        observation_t out;
        out.game_mode = steamos::game_mode_e::absent;
        out.bind = verdict_t::fail("prelaunch_session_identity_unavailable", "this platform has no Steam Game Mode.");
        return out;
      }

      bool local_title_running() override {
        return true;
      }

      bool service_independent(std::string &reason) override {
        reason = "this platform has no Steam Game Mode.";
        return false;
      }
    };

    using production_adapter_t = unavailable_adapter_t;
#endif

    /// The production coordinator once made, for request_stop(), which may not take a lock.
    std::atomic<coordinator_t *> production_coordinator {nullptr};

    struct registry_t {
      ~registry_t() {
        production_coordinator.store(nullptr);
      }

      std::mutex mutex;
      production_host_t host;
      production_adapter_t adapter;
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
    if (!value.production) {
      value.production = std::make_unique<coordinator_t>(value.host, value.adapter, value.clock, budgets_t {}, true);
      production_coordinator.store(value.production.get());
    }
    return *value.production;
  }

  void set_dependencies_for_tests(host_t *host, adapter_t *adapter, clock_t *clock, budgets_t budgets) {
    std::unique_ptr<coordinator_t> previous;
    {
      auto &value = registry();
      std::lock_guard lock {value.mutex};
      previous = std::move(value.test);
      if (host && adapter && clock) value.test = std::make_unique<coordinator_t>(*host, *adapter, *clock, budgets, true);
    }
    // Its restore worker, if any, is cancelled and joined outside the registry's lock.
    previous.reset();
  }

  void request_stop() {
    // No lock and no allocation: a signal handler calls this. A coordinator made later is stopped
    // from birth, because stopping() reads the latch.
    process_stop.store(true);
  }

  void shutdown() {
    request_stop();
    // The coordinator itself stays alive for request handlers until the servers are joined; only its
    // restore worker is collected here.
    if (auto *production = production_coordinator.load()) production->shutdown();
  }

  void reset_process_stop_for_tests() {
    process_stop.store(false);
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
