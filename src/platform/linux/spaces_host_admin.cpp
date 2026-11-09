/**
 * @file src/platform/linux/spaces_host_admin.cpp
 * @brief Host setup an administrator approves at this PC, through pkexec and the Spaces setup helper.
 */
#include "spaces_host_admin.h"
#ifdef __linux__
#include "multiseat_container_host.h"
#include "spaces_host_admin_data.h"
#include "src/logging.h"
#include "src/utility.h"
#include "src/update_status.h"
#include "src/platform/common.h"
#include "src/uuid.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace multiseat::spaces {
  using namespace std::literals;

  namespace {
    using json = nlohmann::json;

    constexpr std::string_view waiting_message = "Waiting for approval. A password prompt is open on this PC's screen.";
    constexpr std::string_view approved_message = "Approved. Polaris is changing this PC's setup.";
    constexpr std::string_view running_message = "Polaris is already waiting on a change to this PC's setup. Wait for it to finish.";
    constexpr std::string_view closing_message = "Polaris is shutting down.";
    constexpr std::size_t output_limit = 16384;

    bool uuid(std::string_view value) {
      if (value.size() != 36) return false;
      for (std::size_t i = 0; i < value.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? value[i] != '-' : std::string_view("0123456789abcdef").find(value[i]) == std::string_view::npos) return false;
      }
      return true;
    }

    bool same_request(const host_action_request_t &a, const host_action_request_t &b) {
      return a.action == b.action && a.request_id == b.request_id && a.channel == b.channel &&
        a.version == b.version && a.review_id == b.review_id;
    }

    /// The last line a process wrote that says something, as printable ASCII for a person to read.
    std::string last_line(std::string_view text) {
      while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
      const auto start = text.find_last_of('\n');
      auto line = std::string(start == std::string_view::npos ? text : text.substr(start + 1));
      for (auto &character : line) {
        if (static_cast<unsigned char>(character) < 0x20 || static_cast<unsigned char>(character) >= 0x7f) character = '?';
      }
      return line.size() > 2048 ? line.substr(0, 2048) : line;
    }

    std::map<std::string, std::string, std::less<>> properties(std::string_view text) {
      std::map<std::string, std::string, std::less<>> result;
      while (!text.empty()) {
        const auto end = text.find('\n');
        const auto line = text.substr(0, end);
        const auto equals = line.find('=');
        if (equals != std::string_view::npos) result.emplace(std::string(line.substr(0, equals)), std::string(line.substr(equals + 1)));
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
      }
      return result;
    }

    /// Root owns it and its directories, nobody else can write them, and setuid is exactly as expected.
    bool trusted_program(const std::filesystem::path &path, bool setuid_root) {
      struct stat metadata {};
      if (!path.is_absolute() || path.lexically_normal() != path) return false;
      if (lstat(path.c_str(), &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != 0 ||
          (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0 || (metadata.st_mode & S_IXUSR) == 0 ||
          setuid_root != ((metadata.st_mode & S_ISUID) != 0)) return false;
      for (auto directory = path.parent_path();; directory = directory.parent_path()) {
        if (lstat(directory.c_str(), &metadata) != 0 || !S_ISDIR(metadata.st_mode) || metadata.st_uid != 0 ||
            (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0) return false;
        if (directory == directory.root_path()) break;
      }
      return true;
    }

    struct descriptor_t {
      int fd = -1;
      descriptor_t() = default;
      explicit descriptor_t(int value): fd(value) {}
      descriptor_t(const descriptor_t &) = delete;
      descriptor_t &operator=(const descriptor_t &) = delete;
      ~descriptor_t() { reset(); }
      void reset() {
        if (fd >= 0) close(fd);
        fd = -1;
      }
    };

    std::mutex installed_mutex;
    std::shared_ptr<host_admin_service_t> installed;
    constexpr std::string_view activity_message = "Wait for the current Space launch or change to finish, then try again.";
  }  // namespace

  std::optional<host_action_e> parse_host_action(std::string_view name) {
    if (name == "security_install") return host_action_e::security_install;
    if (name == "docker_access") return host_action_e::docker_access;
    return std::nullopt;
  }

  std::string_view host_action_name(host_action_e action) {
    switch (action) {
      case host_action_e::security_install: return "security_install";
      case host_action_e::docker_access: return "docker_access";
      case host_action_e::update_install: return "install";
      case host_action_e::update_finish: return "finish";
      case host_action_e::update_cancel: return "cancel";
    }
    return {};
  }

  std::vector<std::string> host_action_argv(host_action_e action) {
    if (action != host_action_e::security_install && action != host_action_e::docker_access) return {};
    // pkexec picks the polkit action whose exec.path and exec.argv1 match, so the prompt names the
    // change. Its text agent stays off: without a desktop agent the request fails instead of
    // waiting for a terminal nobody is at.
    return {std::string(host_admin_data::pkexec_path), "--disable-internal-agent", std::string(host_admin_data::helper_path),
      action == host_action_e::security_install ? "install" : "docker-access"};
  }

  std::optional<std::string> display_session_id(std::string_view user_properties) {
    const auto values = properties(user_properties);
    const auto display = values.find("Display");
    if (display == values.end() || display->second.size() > 64 ||
        display->second.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
      return std::nullopt;
    return display->second;
  }

  std::string desktop_session_problem(std::string_view session_properties) {
    const auto values = properties(session_properties);
    const auto value = [&](std::string_view key) -> std::optional<std::string> {
      const auto found = values.find(key);
      return found == values.end() ? std::nullopt : std::optional {found->second};
    };
    const auto active = value("Active"), remote = value("Remote"), type = value("Type"), seat = value("Seat");
    if (!active || !remote || !type || !seat) return "session_unknown";
    // polkit counts a session as local only when it sits on a seat.
    if (*remote != "no" || seat->empty()) return "remote_session";
    if (*type != "x11" && *type != "wayland" && *type != "mir") return "no_desktop";
    if (*active != "yes") return "inactive_session";
    return {};
  }

  std::optional<host_action_refusal_t> host_action_refusal(const host_admin_facts_t &f) {
    if (f.image_based)
      return host_action_refusal_t {"image_based_host",
        "This PC runs an image based system, where Polaris does not change host setup. Follow the guide for your system instead."};
    if (!f.helper)
      return host_action_refusal_t {"helper_missing",
        "The Spaces setup helper is not installed. Install the native Polaris package for your system, then recheck."};
    if (!f.pkexec)
      return host_action_refusal_t {"pkexec_missing",
        "This PC has no pkexec, so Polaris cannot ask for administrator approval. Run the terminal steps on this PC instead."};
    if (!f.policy)
      return host_action_refusal_t {"policy_missing",
        "The Polaris polkit policy is missing or changed. Reinstall the native Polaris package, then recheck."};
    if (!f.session_problem.empty())
      return host_action_refusal_t {"no_local_desktop",
        "Someone must be signed in at this PC's desktop to approve the password prompt. Otherwise, run the terminal steps on this PC."};
    if (f.setup_active)
      return host_action_refusal_t {"setup_active", "Wait for the first Space setup to finish, then try again."};
    if (f.spaces_active)
      return host_action_refusal_t {"spaces_active", "Stop every Space stream and wait for Spaces to finish closing, then try again."};
    if (f.external_update_active)
      return host_action_refusal_t {"update_running", "A privileged Polaris update is still running on this PC."};
    if (f.resumable_active)
      return host_action_refusal_t {"resumable_active", "A game is running or paused for resume on this PC. End it before updating."};
    if (f.session_active)
      return host_action_refusal_t {"session_active", "A session is starting, running or closing on this PC. Wait for it to end."};
    if (f.stream_active)
      return host_action_refusal_t {"stream_active", "End every stream on this PC, then try again."};
    return std::nullopt;
  }

  bool update_family_qualified(std::string_view id, std::string_view version_id) {
    return id == "fedora" &&
      std::find(update_fedora_releases.begin(), update_fedora_releases.end(), version_id) != update_fedora_releases.end();
  }

  std::optional<host_action_refusal_t> update_action_refusal(host_admin_facts_t facts, bool finish) {
    if (!facts.update_family)
      return host_action_refusal_t {"family_unqualified", "Polaris can install updates on Fedora 44 or 45 x86_64 only. Use the guided steps for this system."};
    facts.helper = facts.update_helper;
    if (finish) facts.helper = facts.pkexec = facts.policy = true;
    if (const auto refusal = host_action_refusal(facts)) {
      if (refusal->code == "helper_missing")
        return host_action_refusal_t {"helper_missing", "The Polaris update helper is missing. Install the matching native package, then review again."};
      return refusal;
    }
    return std::nullopt;
  }

  host_action_outcome_t classify_host_action(host_action_e action, const host_action_run_t &run) {
    if (run.approval_timed_out)
      return {"timed_out", "Nobody approved the password prompt in time, so Polaris closed it. Nothing was changed.", {}};
    auto error = last_line(run.errors);
    if (!run.approved) {
      // pkexec's own exits: 126 when the prompt was dismissed, 127 when approval failed or was impossible.
      if (run.exit_status == 126) return {"cancelled", "The password prompt was closed without approval. Nothing was changed.", {}};
      if (run.exit_status == 127 && run.errors.find("No authentication agent found") != std::string::npos)
        return {"no_agent",
          "No password prompt could open on this PC's screen. Someone must be signed in at this PC's desktop, or run the terminal steps on this PC.", {}};
      if (run.exit_status == 127 && run.errors.find("Not authorized") != std::string::npos)
        return {"not_authorized", "The password was not accepted, so nothing was changed.", {}};
      return {"failed", "Polaris could not ask for approval on this PC. Run the terminal steps on this PC instead.", std::move(error)};
    }
    if (run.exit_status == 0) {
      if (action == host_action_e::security_install) return {"done", "Spaces security support is installed.", {}};
      // The helper's own words say whether a restart is still needed.
      std::string_view output = run.output;
      if (output.starts_with(host_action_started_line)) output.remove_prefix(host_action_started_line.size());
      return {"done", "Docker access is set up.", last_line(output)};
    }
    constexpr std::string_view prefix = "Spaces setup: ";
    if (run.exit_status == 1 && error.starts_with(prefix))
      return {"refused", "The Spaces setup helper did not make the change.", error.substr(prefix.size())};
    if (run.exit_status < 0)
      return {"failed", "Polaris could not tell whether the change finished. Recheck setup.", std::move(error)};
    return {"failed", "The change did not finish. Run the terminal steps on this PC to see what happened.", std::move(error)};
  }

  static host_action_run_t run_action_process(const std::vector<std::string> &argv,
    std::chrono::milliseconds approval_timeout, const std::function<void()> &approved, std::stop_token stop,
    std::string_view input, const std::function<void(const json &)> &progress) {
    host_action_run_t result;
    if (argv.empty() || argv.front().empty() || argv.front().front() != '/') return result;
    int in[2] {-1, -1};
    if (input.size() > 65 || pipe2(in, O_CLOEXEC) != 0) return result;
    descriptor_t in_read {in[0]}, in_write {in[1]};
    // A fixed 65-byte equality guard fits in an empty pipe; write and close before spawning.
    if (!input.empty() && write(in_write.fd, input.data(), input.size()) != static_cast<ssize_t>(input.size())) return result;
    in_write.reset();
    int out[2] {-1, -1}, err[2] {-1, -1};
    if (pipe2(out, O_CLOEXEC) != 0) return result;
    descriptor_t out_read {out[0]}, out_write {out[1]};
    if (pipe2(err, O_CLOEXEC) != 0) return result;
    descriptor_t err_read {err[0]}, err_write {err[1]};
    for (const int fd : {out_read.fd, err_read.fd}) {
      const int flags = fcntl(fd, F_GETFL);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) return result;
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return result;
    auto actions_guard = util::fail_guard([&] { posix_spawn_file_actions_destroy(&actions); });
    posix_spawnattr_t attributes;
    if (posix_spawnattr_init(&attributes) != 0) return result;
    auto attributes_guard = util::fail_guard([&] { posix_spawnattr_destroy(&attributes); });
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    for (const int signal_number : {SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGCHLD}) sigaddset(&defaults, signal_number);
    if (posix_spawn_file_actions_adddup2(&actions, in_read.fd, STDIN_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, out_write.fd, STDOUT_FILENO) != 0 ||
        posix_spawn_file_actions_adddup2(&actions, err_write.fd, STDERR_FILENO) != 0 ||
        posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1) != 0 ||
        posix_spawnattr_setsigmask(&attributes, &empty) != 0 || posix_spawnattr_setsigdefault(&attributes, &defaults) != 0 ||
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF) != 0)
      return result;
    std::vector<char *> native;
    for (const auto &argument : argv) native.push_back(const_cast<char *>(argument.c_str()));
    native.push_back(nullptr);
    // Nothing from Polaris's own environment reaches pkexec or the helper.
    static char path_variable[] = "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
    static char lang_variable[] = "LANG=C";
    static char locale_variable[] = "LC_ALL=C";
    std::array<char *, 4> environment {path_variable, lang_variable, locale_variable, nullptr};
    pid_t child = -1;
    if (posix_spawn(&child, native.front(), &actions, &attributes, native.data(), environment.data()) != 0) return result;
    out_write.reset();
    err_write.reset();

    const auto deadline = std::chrono::steady_clock::now() + std::max(approval_timeout, 0ms);
    std::optional<std::chrono::steady_clock::time_point> force_after, stop_deadline, drain_deadline;
    std::size_t progress_offset = 0;
    bool out_open = true, err_open = true, reaped = false, lost = false, signalled = false, line_checked = false;
    int status = 0;
    const auto drain = [](descriptor_t &fd, bool &open, std::string &into) {
      std::array<char, 4096> buffer {};
      for (unsigned reads = 0; open && reads < 16; ++reads) {
        const auto count = read(fd.fd, buffer.data(), buffer.size());
        if (count > 0) {
          into.append(buffer.data(), std::min<std::size_t>(static_cast<std::size_t>(count), output_limit - std::min(output_limit, into.size())));
          continue;
        }
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) open = false;
        break;
      }
    };
    for (;;) {
      if (out_open) drain(out_read, out_open, result.output);
      if (err_open) drain(err_read, err_open, result.errors);
      if (!line_checked) {
        const auto end = result.output.find('\n');
        if (end != std::string::npos || result.output.size() >= output_limit || !out_open) {
          line_checked = true;
          if (end != std::string::npos && (std::string_view(result.output).substr(0, end) == host_action_started_line ||
                  std::string_view(result.output).substr(0, end) == "Polaris update started.")) {
            result.approved = true;
            result.approval_timed_out = false;
            if (approved) approved();
          }
        }
      }
      if (progress) {
        for (auto end = result.output.find('\n', progress_offset); end != std::string::npos;
             end = result.output.find('\n', progress_offset)) {
          const auto line = result.output.substr(progress_offset, end - progress_offset);
          progress_offset = end + 1;
          try {
            if (!line.empty() && line.front() == '{') progress(json::parse(line));
          } catch (...) {}
        }
      }
      if (!reaped) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) reaped = true;
        else if (waited < 0 && errno != EINTR) reaped = lost = true;  // another owner reaped it
      }
      const auto now = std::chrono::steady_clock::now();
      if (reaped) {
        if (!out_open && !err_open) break;
        if (!drain_deadline) drain_deadline = now + 2s;
        else if (now >= *drain_deadline) break;
      } else if (!result.approved && !signalled && (stop.stop_requested() || now >= deadline)) {
        // Still pkexec waiting on the prompt, running as this account, so it can be closed.
        if (kill(child, SIGTERM) == 0) {
          signalled = true;
          result.approval_timed_out = !stop.stop_requested();
          force_after = now + 2s;
        } else if (errno == EPERM) {
          // pkexec already became the approved helper; its started line is on the way.
          signalled = true;
        }
      } else if (signalled && force_after && !result.approved && now >= *force_after) {
        (void) kill(child, SIGKILL);
        force_after.reset();
      } else if (result.approved && stop.stop_requested()) {
        // Approved changes finish on their own; Polaris waits a little, then leaves the helper to its journal.
        if (!stop_deadline) stop_deadline = now + 30s;
        else if (now >= *stop_deadline) return result;
      }
      std::array<pollfd, 2> descriptors {};
      nfds_t count = 0;
      if (out_open) descriptors[count++] = {out_read.fd, POLLIN, 0};
      if (err_open) descriptors[count++] = {err_read.fd, POLLIN, 0};
      if (count) (void) poll(descriptors.data(), count, 50);
      else std::this_thread::sleep_for(50ms);
    }
    if (!lost) {
      if (WIFEXITED(status)) result.exit_status = WEXITSTATUS(status);
      else if (WIFSIGNALED(status)) result.exit_status = 128 + WTERMSIG(status);
    }
    return result;
  }

  host_action_run_t run_host_action_process(const std::vector<std::string> &argv,
    std::chrono::milliseconds timeout, const std::function<void()> &approved, std::stop_token stop) {
    return run_action_process(argv, timeout, approved, stop, {}, {});
  }

  host_action_run_t run_update_action_process(const std::vector<std::string> &argv, std::string_view guard,
    std::chrono::milliseconds timeout, const std::function<void()> &approved,
    const std::function<void(const json &)> &progress, std::stop_token stop) {
    if (!std::regex_match(std::string(guard), std::regex("[0-9a-f]{64}"))) return {};
    return run_action_process(argv, timeout, approved, stop, std::string(guard) + "\n", progress);
  }

  bool valid_update_version(std::string_view version) {
    static const std::regex pattern(R"((0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})(-(beta|rc)\.[1-9][0-9]{0,5})?)");
    return std::regex_match(std::string(version), pattern);
  }

  std::optional<host_action_request_t> decode_update_request(std::string_view payload, bool finish) {
    if (payload.empty() || payload.size() > 4096) return std::nullopt;
    try {
      std::set<std::string> keys;
      const auto body = json::parse(payload, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 1) throw std::invalid_argument("update nesting");
        if (event == json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
          throw std::invalid_argument("duplicate update field");
        return true;
      });
      const bool cancel = !finish && body.is_object() && body.value("action", "") == "cancel";
      const std::set<std::string> expected = cancel ? std::set<std::string> {"action", "request_id"} :
        finish ? std::set<std::string> {"action", "request_id", "version"} :
        std::set<std::string> {"action", "request_id", "channel", "version", "review_id"};
      if (!body.is_object() || keys != expected) return std::nullopt;
      for (const auto &key : keys) if (!body.at(key).is_string()) return std::nullopt;
      if (body.at("action") != (cancel ? "cancel" : finish ? "finish" : "install")) return std::nullopt;
      host_action_request_t request;
      request.action = cancel ? host_action_e::update_cancel : finish ? host_action_e::update_finish : host_action_e::update_install;
      request.request_id = body.at("request_id").get<std::string>();
      if (cancel) return uuid(request.request_id) ? std::optional {request} : std::nullopt;
      request.version = body.at("version").get<std::string>();
      if (!uuid(request.request_id) || !valid_update_version(request.version)) return std::nullopt;
      if (!finish) {
        request.channel = body.at("channel").get<std::string>();
        request.review_id = body.at("review_id").get<std::string>();
        if ((request.channel != "stable" && request.channel != "beta") || !uuid(request.review_id)) return std::nullopt;
      }
      return request;
    } catch (...) { return std::nullopt; }
  }

  std::optional<host_action_request_t> decode_host_action_request(std::string_view payload) {
    if (payload.empty() || payload.size() > 4096) return std::nullopt;
    try {
      std::set<std::string> keys;
      const auto body = json::parse(payload, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 1) throw std::invalid_argument("host action nesting");
        if (event == json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
          throw std::invalid_argument("duplicate host action field");
        return true;
      });
      if (!body.is_object() || body.size() != 2 || !body.contains("action") || !body.contains("request_id") ||
          !body.at("action").is_string() || !body.at("request_id").is_string()) return std::nullopt;
      const auto action = parse_host_action(body.at("action").get<std::string>());
      auto request_id = body.at("request_id").get<std::string>();
      if (!action || !uuid(request_id)) return std::nullopt;
      return host_action_request_t {*action, std::move(request_id)};
    } catch (...) {
      return std::nullopt;
    }
  }

  host_admin_service_t::host_admin_service_t(host_admin_options_t options):
      options_(std::move(options)) {}

  host_admin_service_t::~host_admin_service_t() {
    shutdown();
  }

  host_admin_service_t::submit_result_t host_admin_service_t::submit(const host_action_request_t &input_request) {
    auto request = input_request;
    if (request.action == host_action_e::update_cancel) return {400, {}};
    const bool update = request.action == host_action_e::update_install || request.action == host_action_e::update_finish;
    std::jthread previous;
    {
      std::lock_guard lock(mutex_);
      if (closing_) return {503, host_action_refusal_t {"closing", std::string(closing_message)}};
      if (job_ && job_->request.request_id == request.request_id)
        return {same_request(job_->request, request) ? 200 : 409, {}};
      if (pending_ && pending_->request_id == request.request_id)
        return {same_request(*pending_, request) ? 202 : 409, {}};
      if (running_) return {409, host_action_refusal_t {"host_setup_running", std::string(running_message)}};
      if (activities_ != 0) return {409, host_action_refusal_t {"spaces_active", std::string(activity_message)}};
      if (request.action == host_action_e::update_install) {
        const auto review = std::find_if(reviews_.begin(), reviews_.end(), [&](const auto &entry) {
          return entry.plan.value("review_id", "") == request.review_id && entry.plan.value("channel", "") == request.channel &&
            entry.plan.value("version", "") == request.version && entry.plan.value("update_needed", false) &&
            std::chrono::steady_clock::now() - entry.at <= 10min;
        });
        if (review == reviews_.end())
          return {409, host_action_refusal_t {"review_required", "Review this update again before asking for approval."}};
        request.review_guard = review->plan.value("review_guard", "");
        reviews_.erase(review); // Consume only this tab's review, including a later preflight refusal.
      }
      // Reservations and running_ share this mutex: an admitted launch/change cannot be
      // overtaken while it is still doing preflight and invisible to the facts below.
      running_ = true;
      pending_ = request;
      previous = std::move(worker_);
    }
    if (previous.joinable()) previous.join();
    std::optional<host_action_refusal_t> refusal;
    host_admin_facts_t facts;
    try {
      facts = options_.facts ? options_.facts() : host_admin_facts_t {};
      refusal = update ? update_action_refusal(facts, request.action == host_action_e::update_finish) : host_action_refusal(facts);
      if (!refusal && request.action == host_action_e::update_finish) {
        const auto status = options_.update_status ? options_.update_status() : json {};
        const auto installed = status.value("installed", json::object()).value("polaris", json::object());
        const auto kms = status.value("installed", json::object()).value("polaris-kms", json::object());
        const auto binary = options_.running_binary ? options_.running_binary() : json::object();
        if (installed.value("state", "") != "installed" || installed.value("version", "") != request.version ||
            installed.value("version", "") == PROJECT_VERSION || !binary.is_object() || binary.value("matches_package", json {}) != true ||
            (kms.value("state", "") != "absent" && (kms.value("state", "") != "installed" || kms.value("version", "") != request.version)))
          refusal = host_action_refusal_t {"finish_unavailable", "The installed package and running binary could not be confirmed for Finish. Recheck this PC."};
      }
    } catch (...) {
      refusal = host_action_refusal_t {"host_unknown", "Polaris could not check this PC's setup. Recheck, then try again."};
    }
    std::lock_guard lock(mutex_);
    pending_.reset();
    if (closing_) {
      running_ = false;
      return {503, host_action_refusal_t {"closing", std::string(closing_message)}};
    }
    if (refusal) {
      running_ = false;
      BOOST_LOG(info) << "Spaces host setup: "sv << host_action_name(request.action) << " refused: "sv << refusal->code
                      << (facts.session_problem.empty() ? ""s : " ("s + facts.session_problem + ")"s);
      return {409, std::move(refusal)};
    }
    job_ = job_t {request, request.action == host_action_e::update_finish ? "restarting" : "waiting_for_approval",
      request.action == host_action_e::update_finish ? "Restarting Polaris. This page will try to reconnect." :
      update ? "Approve the update on this PC's desktop. Streams cannot start until you approve, cancel or it times out." :
      std::string(waiting_message), {}, std::nullopt};
    BOOST_LOG(info) << "Spaces host setup: asking for approval to run "sv << host_action_name(request.action);
    worker_ = std::jthread([this, request](std::stop_token stop) { work(request, stop); });
    return {202, {}};
  }

  void host_admin_service_t::work(host_action_request_t request, std::stop_token stop) {
    if (request.action == host_action_e::update_finish) {
      // running_ remains held across restart, so no launch can enter after admission.
      std::this_thread::sleep_for(250ms);
      try {
        if (options_.finish) {
          options_.finish();
          // Linux restart() requests an orderly asynchronous process exit and returns.
          // Retain admission until shutdown rather than reopening a launch window.
          return;
        }
      } catch (...) {}
      std::lock_guard lock(mutex_);
      job_->state = "failed";
      job_->message = "Polaris could not restart. Recheck this PC before choosing Finish again.";
      running_ = false;
      return;
    }
    if (request.action == host_action_e::update_install) {
      host_action_run_t run;
      try {
        if (options_.run_update) run = options_.run_update(request, options_.approval_timeout, [this] {
          std::lock_guard lock(mutex_);
          job_->state = "running";
          job_->message = "Approved. Checking the official release again.";
        }, [this](const json &value) {
          if (!value.is_object() || value.value("state", "") != "running") return;
          std::lock_guard lock(mutex_);
          job_->check = value;
          job_->message = "Updating this PC. Streams cannot start until it finishes.";
        }, stop);
      } catch (...) {}
      json result;
      const auto end = run.output.find_last_not_of("\r\n");
      if (end != std::string::npos) {
        const auto start = run.output.rfind('\n', end);
        try { result = json::parse(run.output.substr(start == std::string::npos ? 0 : start + 1, end - (start == std::string::npos ? 0 : start + 1) + 1)); } catch (...) {}
      }
      auto outcome = classify_host_action(host_action_e::security_install, run);
      std::lock_guard lock(mutex_);
      if (run.approved && result.is_object() && result.contains("state") && result.contains("installed")) {
        job_->state = result.value("state", "failed") == "installed" && run.exit_status == 0 ? "installed" : "failed";
        job_->message = result.value("message", "Read installed state, then review again.");
        job_->check = result;
        job_->detail = result.value("detail", "");
      } else {
        job_->state = stop.stop_requested() && !run.approved ? "cancelled" : outcome.state;
        job_->message = run.approved ? "The helper's result is unavailable. Installed state is unknown; recheck before reviewing again." :
          job_->state == "cancelled" ? "Approval cancelled. No package transaction was started." : outcome.message;
        job_->detail = outcome.detail;
        job_->check = json {{"installed", {{"polaris", {{"state", "unknown"}}}, {"polaris-kms", {{"state", "unknown"}}}}}};
      }
      running_ = false;
      return;
    }
    host_action_run_t run;
    try {
      if (options_.run) {
        run = options_.run(host_action_argv(request.action), options_.approval_timeout, [this, &request] {
          std::lock_guard lock(mutex_);
          if (job_ && job_->request.request_id == request.request_id && job_->state == "waiting_for_approval") {
            job_->state = "running";
            job_->message = approved_message;
          }
        }, stop);
      }
    } catch (...) {
      run = {};
    }
    auto outcome = classify_host_action(request.action, run);
    std::optional<json> check;
    // Whatever the helper said, an approved run may have changed this PC, so its check is read again.
    if (run.approved && options_.recheck && !stop.stop_requested()) {
      try {
        check = options_.recheck(request.action);
      } catch (...) {}
    }
    BOOST_LOG(info) << "Spaces host setup: "sv << host_action_name(request.action) << ' ' << outcome.state
                    << " (exit "sv << run.exit_status << ')' << (outcome.detail.empty() ? ""s : ": "s + outcome.detail);
    std::lock_guard lock(mutex_);
    if (job_ && job_->request.request_id == request.request_id) {
      job_->state = std::move(outcome.state);
      job_->message = std::move(outcome.message);
      job_->detail = std::move(outcome.detail);
      job_->check = std::move(check);
    }
    running_ = false;
  }

  json host_admin_service_t::snapshot() const {
    std::optional<job_t> job;
    bool busy = false, closing = false, activity = false;
    {
      std::lock_guard lock(mutex_);
      job = job_;
      busy = running_;
      closing = closing_;
      activity = activities_ != 0;
    }
    std::optional<host_action_refusal_t> refusal;
    if (closing) refusal = host_action_refusal_t {"closing", std::string(closing_message)};
    else if (busy) refusal = host_action_refusal_t {"host_setup_running", std::string(running_message)};
    else if (activity) refusal = host_action_refusal_t {"spaces_active", std::string(activity_message)};
    else {
      try {
        refusal = host_action_refusal(options_.facts ? options_.facts() : host_admin_facts_t {});
      } catch (...) {
        refusal = host_action_refusal_t {"host_unknown", "Polaris could not check this PC's setup. Recheck, then try again."};
      }
    }
    json result {{"version", 1}, {"available", !refusal}, {"job", nullptr}};
    if (refusal) {
      result["reason"] = refusal->code;
      result["message"] = refusal->message;
    }
    // Spaces has a closed action schema. Update jobs share admission, not its response body.
    if (job && (job->request.action == host_action_e::security_install || job->request.action == host_action_e::docker_access)) {
      result["job"] = {{"request_id", job->request.request_id}, {"action", host_action_name(job->request.action)},
        {"state", job->state}, {"message", job->message}, {"detail", job->detail}};
      if (job->check) result["job"]["check"] = *job->check;
    }
    return result;
  }

  json host_admin_service_t::update_snapshot() const {
    std::optional<job_t> job;
    bool busy, closing, activity;
    {
      std::lock_guard lock(mutex_);
      job = job_;
      busy = running_;
      closing = closing_;
      activity = activities_ != 0;
    }
    std::optional<host_action_refusal_t> refusal, finish_refusal;
    json current = json::object();
    // Only a qualified native RPM host has package state or a journal worth reading. Guided hosts
    // (other families, image based, no helper) never run the helper's status read.
    bool qualified = false;
    try {
      const auto facts = options_.facts ? options_.facts() : host_admin_facts_t {};
      refusal = update_action_refusal(facts);
      finish_refusal = update_action_refusal(facts, true);
      qualified = facts.update_family && facts.update_helper && !facts.image_based;
      if (qualified && options_.update_status) current = options_.update_status();
    } catch (...) {
      refusal = finish_refusal = host_action_refusal_t {"host_unknown", "Polaris could not read this PC's update state."};
    }
    // A status read that could not run (no installed map, update_unavailable) is transport, not a
    // transaction result: it is reported as unreadable and never becomes a journal job.
    const bool read_failed = qualified && (!current.is_object() || !current.contains("installed") ||
      !current.at("installed").is_object() || current.value("code", "") == "update_unavailable");
    if (qualified && !read_failed) {
      const auto base = current.at("installed").value("polaris", json::object());
      if (base.is_object() && base.value("state", "") == "absent") {
        // No RPM-installed Polaris here: that is a guided first installation, not an RPM repair.
        qualified = false;
        refusal = host_action_refusal_t {"native_package_absent",
          "This Polaris was not installed from the Fedora RPM package, so it cannot update itself here. "
          "Use the guided steps to install the RPM; later updates can then be installed here."};
      }
    }
    // A guided host keeps its qualification reason; busy state only matters on the native path.
    if (qualified && (closing || busy || activity)) {
      refusal = finish_refusal = host_action_refusal_t {closing ? "closing" : busy ? "host_admin_running" : "activity_pending",
        closing ? std::string(closing_message) : busy ? "Wait for the current host change to finish." :
        "A stream or Space is starting or changing on this PC. Wait for it to finish."};
    }
    json result {{"schema", 1}, {"qualified", qualified}, {"available", !refusal}, {"finish_available", !finish_refusal},
      {"busy", {{"blocked", false}, {"reasons", json::array()}}}, {"job", nullptr}, {"running_version", PROJECT_VERSION}};
    if (qualified) result["installed"] = read_failed ? json::object() : current.at("installed");
    if (read_failed) result["readback"] = "unavailable";
    if (refusal) result["refusal"] = {{"code", refusal->code}, {"message", refusal->message}};
    if (finish_refusal) result["finish_refusal"] = {{"code", finish_refusal->code}, {"message", finish_refusal->message}};
    const auto packages = result.value("installed", json::object());
    const auto base = packages.value("polaris", json::object());
    const auto kms = packages.value("polaris-kms", json::object());
    const auto binary = options_.running_binary ? options_.running_binary() : json::object();
    if (base.value("state", "") != "installed" || base.value("version", "") == PROJECT_VERSION ||
        !binary.is_object() || binary.value("matches_package", json {}) != true ||
        (kms.value("state", "") != "absent" && (kms.value("state", "") != "installed" || kms.value("version", "") != base.value("version", "")))) {
      result["finish_available"] = false;
      if (!finish_refusal) result["finish_refusal"] = {{"code", "finish_unavailable"},
        {"message", "Finish needs a confirmed installed package pair and the packaged running binary. Recheck this PC."}};
    }
    const std::set<std::string> busy_codes {"closing", "host_admin_running", "activity_pending", "setup_active", "spaces_active",
      "stream_active", "resumable_active", "session_active", "update_running"};
    if (refusal && busy_codes.contains(refusal->code))
      result["busy"] = {{"blocked", true}, {"reasons", json::array({refusal->message})}};
    if (job && (job->request.action == host_action_e::update_install || job->request.action == host_action_e::update_finish)) {
      result["job"] = {{"request_id", job->request.request_id}, {"action", host_action_name(job->request.action)},
        {"state", job->state}, {"message", job->message}, {"detail", job->detail},
        {"version", job->request.version}, {"channel", job->request.channel}};
      if (job->check) result["job"]["check"] = *job->check;
    } else if (qualified && !read_failed && current.value("state", "idle") != "idle") {
      // A new Polaris process reads the root journal and real package database.
      // A stale running journal is uncertainty, never success or an old-version promise.
      if (current.value("state", "") == "running" && !update_status::privileged_update_active()) {
        current["state"] = "unknown";
        current["message"] = "The previous helper stopped without a final result. Read the installed state, then review again.";
      }
      result["job"] = {{"request_id", ""}, {"action", "install"}, {"state", current.value("state", "unknown")},
        {"message", current.value("message", "Reading the privileged update on this PC.")},
        {"version", current.value("version", "")}, {"check", current}};
    }
    return result;
  }

  json host_admin_service_t::review_update(std::string_view channel, bool refresh) {
    if (channel != "stable" && channel != "beta") return {{"state", "failed"}, {"message", "Unknown release channel."}};
    auto availability = update_snapshot();
    // Guided hosts never read a release plan through the helper. Busy qualified hosts can still
    // review; capability failures stay guided and never invoke a mutation.
    if (!availability.value("qualified", false) ||
        (!availability.value("available", false) && !availability.at("busy").value("blocked", false))) {
      availability["route"] = "guided";
      return availability;
    }
    json plan = json::object();
    if (!refresh) {
      std::lock_guard lock(mutex_);
      const auto cached = std::find_if(reviews_.rbegin(), reviews_.rend(), [&](const auto &entry) {
        return entry.plan.value("channel", "") == channel && std::chrono::steady_clock::now() - entry.at < 60s;
      });
      if (cached != reviews_.rend()) plan = cached->plan;
    }
    if (plan.empty()) {
      try { plan = options_.update_plan ? options_.update_plan(channel) : json::object(); } catch (...) {}
    }
    if (!plan.is_object() || !plan.contains("review_guard") || !plan.contains("version")) {
      availability["route"] = "guided";
      availability["refusal"] = {{"code", plan.is_object() ? plan.value("code", "release_unverifiable") : "release_unverifiable"},
        {"message", plan.is_object() ? plan.value("message", "The official release could not be verified. Check again.") :
          "The official release could not be verified. Check again."}};
      return availability;
    }
    if (!plan.contains("review_id")) {
      auto review_id = uuid_util::uuid_t::generate().string();
      for (auto &character : review_id) if (character >= 'A' && character <= 'F') character += 'a' - 'A';
      plan["review_id"] = review_id;
      std::lock_guard lock(mutex_);
      std::erase_if(reviews_, [](const auto &entry) { return std::chrono::steady_clock::now() - entry.at > 10min; });
      if (reviews_.size() >= 8) reviews_.erase(reviews_.begin());
      reviews_.push_back({plan, std::chrono::steady_clock::now()});
    }
    plan["route"] = "helper";
    plan["busy"] = availability.at("busy");
    plan["available"] = availability.at("available");
    if (availability.contains("refusal")) plan["refusal"] = availability.at("refusal");
    // The fingerprint stays on the server. The browser returns only this opaque review nonce.
    plan.erase("review_guard");
    return plan;
  }

  bool host_admin_service_t::cancel_update(std::string_view request_id) {
    std::lock_guard lock(mutex_);
    if (!job_ || job_->request.action != host_action_e::update_install ||
        job_->request.request_id != request_id || job_->state != "waiting_for_approval" || !worker_.joinable()) return false;
    worker_.request_stop();
    return true;
  }

  void host_admin_service_t::shutdown() {
    std::jthread worker;
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
      worker = std::move(worker_);
    }
    if (worker.joinable()) {
      worker.request_stop();
      worker.join();
    }
  }

  host_admin_facts_t inspect_host_admin(container::host_t &host) {
    host_admin_facts_t facts;
    std::error_code error;
    facts.image_based = std::filesystem::exists("/run/ostree-booted", error) || error;
    facts.update_helper = trusted_program(std::filesystem::path(host_admin_data::update_helper_path), false);
    const auto distro = update_status::detect_host_distro();
#if defined(__x86_64__)
    facts.update_family = update_family_qualified(distro.id, distro.version_id);
#endif
    facts.external_update_active = update_status::privileged_update_active();
    facts.helper = trusted_program(std::filesystem::path(host_admin_data::helper_path), false);
    facts.pkexec = trusted_program(std::filesystem::path(host_admin_data::pkexec_path), true);
    facts.policy = host.trusted_data_file(std::filesystem::path(host_admin_data::policy_path), host_admin_data::policy);
    const auto user = host.run({"/usr/bin/loginctl", "show-user", std::to_string(host.effective_uid()), "--property=Display"}, 2s, 4096);
    if (user.timed_out || user.output_truncated) facts.session_problem = "session_unknown";
    else if (user.exit_status != 0) facts.session_problem = "no_session";  // not signed in at all
    else if (const auto id = display_session_id(user.output); !id) facts.session_problem = "session_unknown";
    else if (id->empty()) facts.session_problem = "no_session";
    else {
      const auto session = host.run({"/usr/bin/loginctl", "show-session", *id, "--property=Active", "--property=Remote",
        "--property=Type", "--property=Seat"}, 2s, 4096);
      facts.session_problem = session.timed_out || session.output_truncated || session.exit_status != 0 ?
        "session_unknown" : desktop_session_problem(session.output);
    }
    return facts;
  }

  std::shared_ptr<host_admin_service_t> make_host_admin_service(host_admin_probes_t probes) {
    return std::make_shared<host_admin_service_t>(host_admin_options_t {
      .facts = [probes] {
        container::local_host_t host;
        auto facts = inspect_host_admin(host);
        facts.setup_active = probes.setup_active && probes.setup_active();
        facts.spaces_active = probes.spaces_active && probes.spaces_active();
        facts.stream_active = probes.stream_active && probes.stream_active();
        facts.resumable_active = probes.resumable_active && probes.resumable_active();
        facts.session_active = probes.session_active && probes.session_active();
        return facts;
      },
      .run = run_host_action_process,
      .recheck = [probes](host_action_e action) -> std::optional<json> {
        const auto setup = probes.setup ? probes.setup() : std::nullopt;
        if (!setup || !setup->is_object() || !setup->contains("checks") || !setup->at("checks").is_array()) return std::nullopt;
        const std::string_view id = action == host_action_e::security_install ? "security" : "docker_access";
        for (const auto &check : setup->at("checks"))
          if (check.is_object() && check.value("id", "") == id) return check;
        return std::nullopt;
      },
      .update_plan = [](std::string_view channel) { return update_status::helper_update_read("plan", channel); },
      .update_status = [] { return update_status::helper_update_read("status"); },
      .run_update = [](const host_action_request_t &request, std::chrono::milliseconds timeout,
          const std::function<void()> &approved, const std::function<void(const json &)> &progress, std::stop_token stop) {
        return run_update_action_process({std::string(host_admin_data::pkexec_path), "--disable-internal-agent",
          std::string(host_admin_data::update_helper_path), "install", request.channel, request.version},
          request.review_guard, timeout, approved, progress, stop);
      },
      .finish = [] { platf::restart(); },
      .running_binary = update_status::running_binary_json,
    });
  }

  bool install_host_admin_service(const std::shared_ptr<host_admin_service_t> &service) {
    std::lock_guard lock(installed_mutex);
    if (!service || installed) return false;
    installed = service;
    return true;
  }

  void uninstall_host_admin_service(const std::shared_ptr<host_admin_service_t> &service) {
    std::lock_guard lock(installed_mutex);
    if (installed == service) installed.reset();
  }

  std::shared_ptr<host_admin_service_t> installed_host_admin_service() {
    std::lock_guard lock(installed_mutex);
    return installed;
  }

  bool host_admin_running() {
    const auto service = installed_host_admin_service();
    return service && service->running();
  }

  bool host_admin_service_t::begin_activity() {
    std::lock_guard lock(mutex_);
    if (closing_ || running_ || update_status::privileged_update_active()) return false;
    ++activities_;
    return true;
  }

  void host_admin_service_t::finish_activity() {
    std::lock_guard lock(mutex_);
    --activities_;
  }

  host_activity_guard_t::host_activity_guard_t(std::shared_ptr<host_admin_service_t> service):
      service_(std::move(service)) {}

  host_activity_guard_t::~host_activity_guard_t() {
    if (service_) service_->finish_activity();
  }

  std::optional<host_activity_guard_t> try_begin_host_activity() {
    auto service = installed_host_admin_service();
    if (service && !service->begin_activity()) return std::nullopt;
    return host_activity_guard_t(std::move(service));
  }
}  // namespace multiseat::spaces
#endif
