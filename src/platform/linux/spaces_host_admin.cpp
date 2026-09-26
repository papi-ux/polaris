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
    return action == host_action_e::security_install ? "security_install" : "docker_access";
  }

  std::vector<std::string> host_action_argv(host_action_e action) {
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
    if (f.stream_active)
      return host_action_refusal_t {"stream_active", "End every stream on this PC, then try again."};
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

  host_action_run_t run_host_action_process(const std::vector<std::string> &argv,
    std::chrono::milliseconds approval_timeout, const std::function<void()> &approved, std::stop_token stop) {
    host_action_run_t result;
    if (argv.empty() || argv.front().empty() || argv.front().front() != '/') return result;
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
    if (posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0 ||
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
          if (end != std::string::npos && std::string_view(result.output).substr(0, end) == host_action_started_line) {
            result.approved = true;
            result.approval_timed_out = false;
            if (approved) approved();
          }
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

  host_admin_service_t::submit_result_t host_admin_service_t::submit(const host_action_request_t &request) {
    std::jthread previous;
    {
      std::lock_guard lock(mutex_);
      if (closing_) return {503, host_action_refusal_t {"closing", std::string(closing_message)}};
      if (job_ && job_->request.request_id == request.request_id)
        return {job_->request.action == request.action ? 200 : 409, {}};
      if (pending_ && pending_->request_id == request.request_id)
        return {pending_->action == request.action ? 202 : 409, {}};
      if (running_) return {409, host_action_refusal_t {"host_setup_running", std::string(running_message)}};
      if (activities_ != 0) return {409, host_action_refusal_t {"spaces_active", std::string(activity_message)}};
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
      refusal = host_action_refusal(facts);
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
    job_ = job_t {request, "waiting_for_approval", std::string(waiting_message), {}, std::nullopt};
    BOOST_LOG(info) << "Spaces host setup: asking for approval to run "sv << host_action_name(request.action);
    worker_ = std::jthread([this, request](std::stop_token stop) { work(request, stop); });
    return {202, {}};
  }

  void host_admin_service_t::work(host_action_request_t request, std::stop_token stop) {
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
    if (job) {
      result["job"] = {{"request_id", job->request.request_id}, {"action", host_action_name(job->request.action)},
        {"state", job->state}, {"message", job->message}, {"detail", job->detail}};
      if (job->check) result["job"]["check"] = *job->check;
    }
    return result;
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
    if (closing_ || running_) return false;
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
