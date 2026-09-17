/**
 * @file src/platform/linux/spaces_host_admin.h
 * @brief Host setup an administrator approves at this PC. Polaris runs the packaged Spaces setup
 * helper through pkexec, one fixed operation at a time, and reports in words what happened.
 */
#pragma once
#ifdef __linux__
#include "multiseat_container_backend.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace multiseat::spaces {
  /// The only privileged changes Polaris asks for. Each is one helper operation and one polkit action.
  enum class host_action_e { security_install, docker_access };
  inline constexpr std::array<host_action_e, 2> host_actions {host_action_e::security_install, host_action_e::docker_access};

  [[nodiscard]] std::optional<host_action_e> parse_host_action(std::string_view name);
  [[nodiscard]] std::string_view host_action_name(host_action_e action);
  /// pkexec without its text agent, the installed helper and one operation from a closed list.
  [[nodiscard]] std::vector<std::string> host_action_argv(host_action_e action);

  /// The Display= value of loginctl show-user: a session id, empty when the account has no desktop
  /// session, nullopt when the output is not that property.
  [[nodiscard]] std::optional<std::string> display_session_id(std::string_view user_properties);
  /// Why that session cannot show a polkit prompt to someone at this PC: remote_session,
  /// no_desktop, inactive_session or session_unknown. Empty when it can.
  [[nodiscard]] std::string desktop_session_problem(std::string_view session_properties);

  /// What decides whether Polaris may ask for approval now.
  struct host_admin_facts_t {
    bool helper = false;  ///< the packaged helper is installed, owned by root
    bool pkexec = false;  ///< pkexec is installed setuid root
    bool policy = false;  ///< the exact polkit policy this build ships is installed
    bool image_based = false;
    std::string session_problem;  ///< empty when this account has an active local desktop session
    bool setup_active = false;  ///< the first Space setup is downloading, preparing or configuring
    bool spaces_active = false;  ///< a Space is starting, running, stopping or in use, or Spaces are changing
    bool stream_active = false;
  };
  struct host_action_refusal_t {
    std::string code, message;
  };
  [[nodiscard]] std::optional<host_action_refusal_t> host_action_refusal(const host_admin_facts_t &facts);

  /// The first line the helper prints when pkexec ran it, which only happens after approval.
  inline constexpr std::string_view host_action_started_line = "Spaces setup started.";
  struct host_action_run_t {
    int exit_status = -1;  ///< the process exit status, 128 plus the signal, or -1 when it did not start or was left running
    bool approved = false;  ///< the started line arrived
    bool approval_timed_out = false;  ///< nobody approved in time and Polaris closed the prompt
    std::string output, errors;  ///< bounded standard output and error
  };
  struct host_action_outcome_t {
    std::string state, message, detail;
  };
  /// done, refused, cancelled, not_authorized, no_agent, timed_out or failed, with a sentence.
  [[nodiscard]] host_action_outcome_t classify_host_action(host_action_e action, const host_action_run_t &run);

  using host_action_runner_t = std::function<host_action_run_t(const std::vector<std::string> &argv,
    std::chrono::milliseconds approval_timeout, const std::function<void()> &approved, std::stop_token stop)>;
  /// Runs argv without a shell, with a fixed minimal environment and standard input from /dev/null.
  /// Before approval it closes the process at the timeout or on stop; after approval it is never killed.
  [[nodiscard]] host_action_run_t run_host_action_process(const std::vector<std::string> &argv,
    std::chrono::milliseconds approval_timeout, const std::function<void()> &approved, std::stop_token stop);

  struct host_action_request_t {
    host_action_e action = host_action_e::security_install;
    std::string request_id;
  };
  [[nodiscard]] std::optional<host_action_request_t> decode_host_action_request(std::string_view payload);

  struct host_admin_options_t {
    std::function<host_admin_facts_t()> facts;
    host_action_runner_t run;
    /// The setup check an action fixes, read again after it finished. nullopt when it cannot be read.
    std::function<std::optional<nlohmann::json>(host_action_e)> recheck;
    std::chrono::milliseconds approval_timeout = std::chrono::minutes(5);
  };

  class host_admin_service_t {
  public:
    explicit host_admin_service_t(host_admin_options_t options);
    ~host_admin_service_t();
    host_admin_service_t(const host_admin_service_t &) = delete;
    host_admin_service_t &operator=(const host_admin_service_t &) = delete;

    struct submit_result_t {
      int status = 503;
      std::optional<host_action_refusal_t> refusal;
    };
    /// 202 asked for approval, 200 this request is already known, 409 refused or another action runs,
    /// 503 Polaris is shutting down.
    [[nodiscard]] submit_result_t submit(const host_action_request_t &request);
    [[nodiscard]] nlohmann::json snapshot() const;
    /// Waiting for approval or changing this PC. Space launches and Spaces changes wait meanwhile.
    [[nodiscard]] bool running() const { return running_.load(); }
    void shutdown();

  private:
    struct job_t {
      host_action_request_t request;
      std::string state, message, detail;
      std::optional<nlohmann::json> check;
    };
    void work(host_action_request_t request, std::stop_token stop);

    host_admin_options_t options_;
    mutable std::mutex mutex_;
    std::optional<job_t> job_;
    std::optional<host_action_request_t> pending_;  ///< claimed, facts still being read
    std::atomic<bool> running_ {false};
    bool closing_ = false;
    std::jthread worker_;
  };

  /// This PC's side of the refusals: helper, pkexec, policy, image based host and the desktop session.
  [[nodiscard]] host_admin_facts_t inspect_host_admin(container::host_t &host);

  struct host_admin_probes_t {
    std::function<bool()> setup_active, spaces_active, stream_active;
    std::function<std::optional<nlohmann::json>()> setup;  ///< the current /api/spaces/setup body
  };
  [[nodiscard]] std::shared_ptr<host_admin_service_t> make_host_admin_service(host_admin_probes_t probes);
  bool install_host_admin_service(const std::shared_ptr<host_admin_service_t> &service);
  void uninstall_host_admin_service(const std::shared_ptr<host_admin_service_t> &service);
  [[nodiscard]] std::shared_ptr<host_admin_service_t> installed_host_admin_service();
  /// True while the installed service waits for approval or changes this PC.
  [[nodiscard]] bool host_admin_running();
}
#endif
