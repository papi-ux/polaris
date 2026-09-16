#include "src/platform/linux/spaces_host_admin.h"
#include "spaces_host_admin_data.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/private_state_file.h"
#include "src/utility.h"

#include <Simple-Web-Server/client_https.hpp>
#include <Simple-Web-Server/server_https.hpp>
#include <gtest/gtest.h>

#include <array>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>

#ifdef __linux__
namespace confighttp {
  void registerSpacesSetupRoutes(SimpleWeb::ServerBase<SimpleWeb::HTTPS> &);
  void with_web_session_for_tests(const std::filesystem::path &, const std::string &,
    const std::function<void(const std::string &)> &);
}

namespace {
  using namespace multiseat;
  using json = nlohmann::json;
  using namespace std::chrono_literals;
  constexpr std::string_view first_id = "12345678-1234-4234-8234-123456789abc";
  constexpr std::string_view second_id = "87654321-4321-4321-8321-cba987654321";

  spaces::host_admin_facts_t ready_facts() {
    spaces::host_admin_facts_t facts;
    facts.helper = facts.pkexec = facts.policy = true;
    return facts;
  }

  class fake_host_t : public container::host_t {
  public:
    std::vector<std::vector<std::string>> calls;
    std::vector<container::command_result_t> replies;
    bool policy = true;
    std::uint64_t effective_uid() const override { return 1000; }
    bool executable_file(const std::filesystem::path &) const override { return false; }
    bool trusted_runtime_file(const std::filesystem::path &) const override { return false; }
    bool trusted_data_file(const std::filesystem::path &path, std::string_view expected) const override {
      EXPECT_EQ(path, std::filesystem::path(spaces::host_admin_data::policy_path));
      EXPECT_EQ(expected, spaces::host_admin_data::policy);
      return policy;
    }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return std::vector<std::uint64_t> {}; }
    bool readable_directory(const std::filesystem::path &) const override { return false; }
    bool private_read_write_directory(const std::filesystem::path &) const override { return false; }
    bool private_readable_file(const std::filesystem::path &) const override { return false; }
    std::optional<container::character_device_identity_t> read_write_character_device(const std::filesystem::path &) const override { return std::nullopt; }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &, std::size_t) const override { return std::nullopt; }
    container::command_result_t run(const std::vector<std::string> &argv, std::chrono::milliseconds timeout, std::size_t bound) override {
      calls.push_back(argv);
      EXPECT_EQ(timeout, 2s);
      EXPECT_EQ(bound, 4096U);
      if (replies.empty()) return {};
      auto reply = replies.front();
      replies.erase(replies.begin());
      return reply;
    }
  };

  // A runner that holds each run until the test releases it, the way a password prompt waits.
  class SpacesHostAdminService : public ::testing::Test {
  protected:
    std::mutex mutex;
    std::condition_variable_any changed;
    spaces::host_admin_facts_t facts = ready_facts();
    bool release = false, approve = false, facts_throw = false;
    spaces::host_action_run_t next {.exit_status = 0, .approved = true};
    std::vector<std::vector<std::string>> argvs;
    std::vector<std::chrono::milliseconds> timeouts;
    unsigned rechecks = 0;

    std::shared_ptr<spaces::host_admin_service_t> make() {
      return std::make_shared<spaces::host_admin_service_t>(spaces::host_admin_options_t {
        .facts = [this] {
          std::lock_guard lock(mutex);
          if (facts_throw) throw std::runtime_error("probe failed");
          return facts;
        },
        .run = [this](const std::vector<std::string> &argv, std::chrono::milliseconds timeout,
                   const std::function<void()> &approved, std::stop_token stop) {
          std::unique_lock lock(mutex);
          argvs.push_back(argv);
          timeouts.push_back(timeout);
          if (approve) {
            lock.unlock();
            approved();
            lock.lock();
          }
          changed.notify_all();
          if (!changed.wait(lock, stop, [&] { return release; })) return spaces::host_action_run_t {};
          return next;
        },
        .recheck = [this](spaces::host_action_e action) -> std::optional<json> {
          std::lock_guard lock(mutex);
          ++rechecks;
          return json {{"id", action == spaces::host_action_e::security_install ? "security" : "docker_access"}, {"state", "ready"}};
        },
        .approval_timeout = 90s,
      });
    }
    bool wait_for_run(std::size_t count) {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, 3s, [&] { return argvs.size() >= count; });
    }
    void finish(spaces::host_action_run_t run) {
      {
        std::lock_guard lock(mutex);
        next = std::move(run);
        release = true;
      }
      changed.notify_all();
    }
    static bool wait_idle(const spaces::host_admin_service_t &service) {
      for (int attempt = 0; attempt < 300 && service.running(); ++attempt) std::this_thread::sleep_for(10ms);
      return !service.running();
    }
  };
}  // namespace

TEST(SpacesHostAdmin, EachActionRunsPkexecWithoutItsTextAgentOnTheInstalledHelper) {
  const std::string helper(spaces::host_admin_data::helper_path);
  EXPECT_EQ(spaces::host_action_argv(spaces::host_action_e::security_install),
    (std::vector<std::string> {"/usr/bin/pkexec", "--disable-internal-agent", helper, "install"}));
  EXPECT_EQ(spaces::host_action_argv(spaces::host_action_e::docker_access),
    (std::vector<std::string> {"/usr/bin/pkexec", "--disable-internal-agent", helper, "docker-access"}));
  EXPECT_TRUE(helper.ends_with("/bin/polaris-spaces-setup"));
  for (const auto action : spaces::host_actions)
    EXPECT_EQ(spaces::parse_host_action(spaces::host_action_name(action)), action);
  for (const auto name : {"install", "docker-access", "docker_install", "remove", "SECURITY_INSTALL", ""})
    EXPECT_FALSE(spaces::parse_host_action(name)) << name;
}

TEST(SpacesHostAdmin, ThePolicyPolarisChecksIsTheOneThePackageShips) {
  const std::string policy(spaces::host_admin_data::policy);
  const std::string prefix(spaces::host_admin_data::action_prefix);
  EXPECT_EQ(prefix, "dev.polaris-stream.app.polaris");
  for (const auto &[action, argument] : std::vector<std::pair<std::string, std::string>> {
         {prefix + ".spaces-security-install", "install"}, {prefix + ".spaces-docker-access", "docker-access"}}) {
    const auto start = policy.find("<action id=\"" + action + "\">");
    ASSERT_NE(start, std::string::npos) << action;
    const auto body = policy.substr(start, policy.find("</action>", start) - start);
    EXPECT_NE(body.find("<allow_any>no</allow_any>"), std::string::npos) << action;
    EXPECT_NE(body.find("<allow_inactive>no</allow_inactive>"), std::string::npos) << action;
    EXPECT_NE(body.find("<allow_active>auth_admin</allow_active>"), std::string::npos) << action;
    EXPECT_NE(body.find(">" + std::string(spaces::host_admin_data::helper_path) + "</annotate>"), std::string::npos) << action;
    EXPECT_NE(body.find("exec.argv1\">" + argument + "</annotate>"), std::string::npos) << action;
  }
  EXPECT_EQ(policy.find("auth_admin_keep"), std::string::npos);
  EXPECT_TRUE(std::string(spaces::host_admin_data::policy_path).ends_with("/polkit-1/actions/dev.polaris-stream.app.Polaris.policy"));
}

TEST(SpacesHostAdmin, RequestsNameOneKnownActionAndAUuid) {
  const json valid {{"action", "docker_access"}, {"request_id", first_id}};
  const auto decoded = spaces::decode_host_action_request(valid.dump());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->action, spaces::host_action_e::docker_access);
  EXPECT_EQ(decoded->request_id, first_id);
  for (const auto &[key, value] : std::vector<std::pair<std::string, json>> {
         {"action", "docker_install"}, {"action", 1}, {"request_id", "12345678-1234-4234-8234-123456789ABC"},
         {"request_id", "../../etc"}, {"request_id", json::object()}, {"helper", "/tmp/evil"}}) {
    auto bad = valid;
    bad[key] = value;
    EXPECT_FALSE(spaces::decode_host_action_request(bad.dump())) << key;
  }
  EXPECT_FALSE(spaces::decode_host_action_request(json {{"action", "docker_access"}}.dump()));
  auto duplicate = valid.dump();
  duplicate.insert(1, "\"action\":\"security_install\",");
  EXPECT_FALSE(spaces::decode_host_action_request(duplicate));
  EXPECT_FALSE(spaces::decode_host_action_request("[]"));
  EXPECT_FALSE(spaces::decode_host_action_request(""));
  EXPECT_FALSE(spaces::decode_host_action_request(std::string(4097, ' ')));
}

TEST(SpacesHostAdmin, RefusalsComeMostBasicFirstAndSayWhatToDo) {
  EXPECT_FALSE(spaces::host_action_refusal(ready_facts()));
  auto everything = ready_facts();
  everything.image_based = true;
  everything.helper = everything.pkexec = everything.policy = false;
  everything.session_problem = "no_session";
  everything.setup_active = everything.spaces_active = everything.stream_active = true;
  const std::vector<std::pair<std::string, std::function<void(spaces::host_admin_facts_t &)>>> order {
    {"image_based_host", [](auto &f) { f.image_based = false; }},
    {"helper_missing", [](auto &f) { f.helper = true; }},
    {"pkexec_missing", [](auto &f) { f.pkexec = true; }},
    {"policy_missing", [](auto &f) { f.policy = true; }},
    {"no_local_desktop", [](auto &f) { f.session_problem.clear(); }},
    {"setup_active", [](auto &f) { f.setup_active = false; }},
    {"spaces_active", [](auto &f) { f.spaces_active = false; }},
    {"stream_active", [](auto &f) { f.stream_active = false; }},
  };
  for (const auto &[code, clear] : order) {
    const auto refusal = spaces::host_action_refusal(everything);
    ASSERT_TRUE(refusal) << code;
    EXPECT_EQ(refusal->code, code);
    EXPECT_FALSE(refusal->message.empty());
    EXPECT_EQ(refusal->message.find(" - "), std::string::npos) << code;
    clear(everything);
  }
  EXPECT_FALSE(spaces::host_action_refusal(everything));
  auto remote = ready_facts();
  remote.session_problem = "remote_session";
  EXPECT_EQ(spaces::host_action_refusal(remote)->message,
    "Someone must be signed in at this PC's desktop to approve the password prompt. Otherwise, run the terminal steps on this PC.");
}

TEST(SpacesHostAdmin, OnlyAnActiveLocalDesktopSessionCanApprove) {
  EXPECT_EQ(spaces::display_session_id("Display=2\n"), "2");
  EXPECT_EQ(spaces::display_session_id("Display=c135"), "c135");
  EXPECT_EQ(spaces::display_session_id("Display=\n"), "");
  EXPECT_FALSE(spaces::display_session_id(""));
  EXPECT_FALSE(spaces::display_session_id("Sessions=2 3\n"));
  EXPECT_FALSE(spaces::display_session_id("Display=2; reboot\n"));
  const auto session = [](std::string active, std::string remote, std::string type, std::string seat) {
    return "Type=" + type + "\nRemote=" + remote + "\nActive=" + active + "\nSeat=" + seat + "\n";
  };
  EXPECT_EQ(spaces::desktop_session_problem(session("yes", "no", "wayland", "seat0")), "");
  EXPECT_EQ(spaces::desktop_session_problem(session("yes", "no", "x11", "seat0")), "");
  EXPECT_EQ(spaces::desktop_session_problem(session("no", "no", "wayland", "seat0")), "inactive_session");
  EXPECT_EQ(spaces::desktop_session_problem(session("yes", "yes", "wayland", "seat0")), "remote_session");
  EXPECT_EQ(spaces::desktop_session_problem(session("yes", "no", "wayland", "")), "remote_session");
  EXPECT_EQ(spaces::desktop_session_problem(session("yes", "no", "tty", "seat0")), "no_desktop");
  EXPECT_EQ(spaces::desktop_session_problem("Active=yes\nRemote=no\n"), "session_unknown");
}

TEST(SpacesHostAdmin, InspectionAsksLoginctlAboutThisAccountsDesktopSession) {
  const auto uid = std::to_string(1000);
  fake_host_t host;
  host.replies = {{.exit_status = 0, .output = "Display=2\n"}, {.exit_status = 0, .output = "Type=wayland\nRemote=no\nActive=yes\nSeat=seat0\n"}};
  auto facts = spaces::inspect_host_admin(host);
  EXPECT_TRUE(facts.policy);
  EXPECT_EQ(facts.session_problem, "");
  ASSERT_EQ(host.calls.size(), 2U);
  EXPECT_EQ(host.calls[0], (std::vector<std::string> {"/usr/bin/loginctl", "show-user", uid, "--property=Display"}));
  EXPECT_EQ(host.calls[1], (std::vector<std::string> {"/usr/bin/loginctl", "show-session", "2", "--property=Active",
    "--property=Remote", "--property=Type", "--property=Seat"}));
  fake_host_t missing;
  missing.policy = false;
  missing.replies = {{.exit_status = 1, .output = ""}};
  facts = spaces::inspect_host_admin(missing);
  EXPECT_FALSE(facts.policy);
  EXPECT_EQ(facts.session_problem, "no_session");
  EXPECT_EQ(missing.calls.size(), 1U);
  fake_host_t lingering;
  lingering.replies = {{.exit_status = 0, .output = "Display=\n"}};
  EXPECT_EQ(spaces::inspect_host_admin(lingering).session_problem, "no_session");
  fake_host_t slow;
  slow.replies = {{.exit_status = 0, .output = "Display=2\n"}, {.exit_status = 124, .timed_out = true}};
  EXPECT_EQ(spaces::inspect_host_admin(slow).session_problem, "session_unknown");
}

TEST(SpacesHostAdmin, OutcomesFollowPkexecBeforeApprovalAndTheHelperAfter) {
  using spaces::host_action_e;
  const auto outcome = [](host_action_e action, spaces::host_action_run_t run) { return spaces::classify_host_action(action, run); };
  EXPECT_EQ(outcome(host_action_e::security_install, {.exit_status = 143, .approval_timed_out = true}).state, "timed_out");
  EXPECT_EQ(outcome(host_action_e::security_install, {.exit_status = 126, .errors = "Error executing command as another user: Request dismissed\n"}).state, "cancelled");
  EXPECT_EQ(outcome(host_action_e::security_install, {.exit_status = 127, .errors = "Error executing command as another user: No authentication agent found.\n"}).state, "no_agent");
  EXPECT_EQ(outcome(host_action_e::docker_access, {.exit_status = 127, .errors = "Error executing command as another user: Not authorized\n\nThis incident has been reported.\n"}).state, "not_authorized");
  const auto broken = outcome(host_action_e::security_install, {.exit_status = 127, .errors = "Cannot run program /usr/bin/polaris-spaces-setup: No such file or directory\n"});
  EXPECT_EQ(broken.state, "failed");
  EXPECT_EQ(broken.detail, "Cannot run program /usr/bin/polaris-spaces-setup: No such file or directory");
  const auto installed = outcome(host_action_e::security_install, {.exit_status = 0, .approved = true,
    .output = "Spaces setup started.\nSpaces security files are installed. Reopen Polaris and select Recheck setup.\n"});
  EXPECT_EQ(installed.state, "done");
  EXPECT_EQ(installed.message, "Spaces security support is installed.");
  EXPECT_EQ(installed.detail, "");
  const auto access = outcome(host_action_e::docker_access, {.exit_status = 0, .approved = true,
    .output = "Spaces setup started.\nDocker is running, and papi is in the docker group. A Polaris that is already running keeps the access it started with, so restart this PC, then recheck.\n"});
  EXPECT_EQ(access.state, "done");
  EXPECT_EQ(access.detail, "Docker is running, and papi is in the docker group. A Polaris that is already running keeps the access it started with, so restart this PC, then recheck.");
  const auto refused = outcome(host_action_e::security_install, {.exit_status = 1, .approved = true, .output = "Spaces setup started.\n",
    .errors = "Spaces setup: Stop Spaces streams and quit any other Polaris before changing security setup. Still active: polaris (pid 42).\n"});
  EXPECT_EQ(refused.state, "refused");
  EXPECT_EQ(refused.detail, "Stop Spaces streams and quit any other Polaris before changing security setup. Still active: polaris (pid 42).");
  EXPECT_EQ(outcome(host_action_e::security_install, {.exit_status = 2, .approved = true, .errors = "Traceback\x1b[2J\n"}).detail, "Traceback?[2J");
  EXPECT_EQ(outcome(host_action_e::security_install, {.exit_status = -1, .approved = true}).message,
    "Polaris could not tell whether the change finished. Recheck setup.");
}

TEST(SpacesHostAdminRunner, ApprovalComesOnlyFromTheFirstLine) {
  unsigned approvals = 0;
  auto run = spaces::run_host_action_process({"/bin/sh", "-c", "printf 'Spaces setup started.\\nDocker is running.\\n'; printf 'careful\\n' >&2"},
    5s, [&] { ++approvals; }, {});
  EXPECT_TRUE(run.approved);
  EXPECT_EQ(approvals, 1U);
  EXPECT_EQ(run.exit_status, 0);
  EXPECT_FALSE(run.approval_timed_out);
  EXPECT_EQ(run.output, "Spaces setup started.\nDocker is running.\n");
  EXPECT_EQ(run.errors, "careful\n");
  run = spaces::run_host_action_process({"/bin/sh", "-c", "printf 'Error\\nSpaces setup started.\\n'; exit 127"}, 5s, [&] { ++approvals; }, {});
  EXPECT_FALSE(run.approved);
  EXPECT_EQ(approvals, 1U);
  EXPECT_EQ(run.exit_status, 127);
  run = spaces::run_host_action_process({"relative/pkexec"}, 5s, {}, {});
  EXPECT_EQ(run.exit_status, -1);
  EXPECT_FALSE(run.approved);
}

TEST(SpacesHostAdminRunner, AnUnapprovedPromptClosesAtItsDeadlineAndOnStop) {
  auto start = std::chrono::steady_clock::now();
  auto run = spaces::run_host_action_process({"/bin/sh", "-c", "exec sleep 30"}, 200ms, {}, {});
  EXPECT_TRUE(run.approval_timed_out);
  EXPECT_FALSE(run.approved);
  EXPECT_EQ(run.exit_status, 128 + SIGTERM);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
  std::stop_source stop;
  std::jthread stopper([&] {
    std::this_thread::sleep_for(150ms);
    stop.request_stop();
  });
  start = std::chrono::steady_clock::now();
  run = spaces::run_host_action_process({"/bin/sh", "-c", "exec sleep 30"}, 60s, {}, stop.get_token());
  EXPECT_FALSE(run.approval_timed_out);
  EXPECT_FALSE(run.approved);
  EXPECT_EQ(run.exit_status, 128 + SIGTERM);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
}

TEST(SpacesHostAdminRunner, AnApprovedChangeIsNeverCutShort) {
  const auto run = spaces::run_host_action_process({"/bin/sh", "-c", "printf 'Spaces setup started.\\n'; sleep 1; exit 3"}, 300ms, {}, {});
  EXPECT_TRUE(run.approved);
  EXPECT_FALSE(run.approval_timed_out);
  EXPECT_EQ(run.exit_status, 3);
}

TEST(SpacesHostAdminRunner, NothingFromPolarisReachesTheProcessAndOutputIsBounded) {
  ASSERT_EQ(setenv("POLARIS_HOST_ADMIN_AMBIENT", "leaked", 1), 0);
  auto restore = util::fail_guard([] { unsetenv("POLARIS_HOST_ADMIN_AMBIENT"); });
  auto run = spaces::run_host_action_process({"/bin/sh", "-c", "printf 'Spaces setup started.\\n'; env; cat; ls /proc/self/fd"}, 5s, {}, {});
  EXPECT_EQ(run.exit_status, 0);
  EXPECT_EQ(run.output.find("POLARIS_HOST_ADMIN_AMBIENT"), std::string::npos);
  EXPECT_NE(run.output.find("PATH=/usr/sbin:/usr/bin:/sbin:/bin"), std::string::npos);
  EXPECT_NE(run.output.find("LC_ALL=C"), std::string::npos);
  run = spaces::run_host_action_process({"/bin/sh", "-c", "head -c 100000 /dev/zero | tr '\\0' x; head -c 100000 /dev/zero | tr '\\0' y >&2"}, 5s, {}, {});
  EXPECT_EQ(run.exit_status, 0);
  EXPECT_EQ(run.output.size(), 16384U);
  EXPECT_EQ(run.errors.size(), 16384U);
}

TEST_F(SpacesHostAdminService, OneActionAtATimeAndARepeatedRequestIsNotASecondPrompt) {
  auto service = make();
  const spaces::host_action_request_t install {spaces::host_action_e::security_install, std::string(first_id)};
  ASSERT_EQ(service->submit(install).status, 202);
  EXPECT_TRUE(service->running());
  ASSERT_TRUE(wait_for_run(1));
  auto snapshot = service->snapshot();
  EXPECT_EQ(snapshot["version"], 1);
  EXPECT_EQ(snapshot["available"], false);
  EXPECT_EQ(snapshot["reason"], "host_setup_running");
  EXPECT_EQ(snapshot["job"]["action"], "security_install");
  EXPECT_EQ(snapshot["job"]["state"], "waiting_for_approval");
  EXPECT_EQ(snapshot["job"]["message"], "Waiting for approval. A password prompt is open on this PC's screen.");
  const auto second = service->submit({spaces::host_action_e::docker_access, std::string(second_id)});
  EXPECT_EQ(second.status, 409);
  ASSERT_TRUE(second.refusal);
  EXPECT_EQ(second.refusal->code, "host_setup_running");
  EXPECT_EQ(service->submit(install).status, 200);
  EXPECT_EQ(service->submit({spaces::host_action_e::docker_access, std::string(first_id)}).status, 409);
  finish({.exit_status = 0, .approved = true, .output = "Spaces setup started.\nSpaces security files are installed.\n"});
  ASSERT_TRUE(wait_idle(*service));
  snapshot = service->snapshot();
  EXPECT_EQ(snapshot["available"], true);
  EXPECT_FALSE(snapshot.contains("reason"));
  EXPECT_EQ(snapshot["job"]["state"], "done");
  EXPECT_EQ(snapshot["job"]["message"], "Spaces security support is installed.");
  EXPECT_EQ(snapshot["job"]["check"]["id"], "security");
  EXPECT_EQ(service->submit(install).status, 200);
  {
    std::lock_guard lock(mutex);
    EXPECT_EQ(argvs.size(), 1U);
    EXPECT_EQ(argvs[0], spaces::host_action_argv(spaces::host_action_e::security_install));
    EXPECT_EQ(timeouts[0], 90s);
    EXPECT_EQ(rechecks, 1U);
  }
  ASSERT_EQ(service->submit({spaces::host_action_e::docker_access, std::string(second_id)}).status, 202);
  ASSERT_TRUE(wait_idle(*service));
  EXPECT_EQ(service->snapshot()["job"]["action"], "docker_access");
}

TEST_F(SpacesHostAdminService, ApprovalShowsAsRunningAndAHelperRefusalKeepsItsWords) {
  approve = true;
  auto service = make();
  ASSERT_EQ(service->submit({spaces::host_action_e::security_install, std::string(first_id)}).status, 202);
  ASSERT_TRUE(wait_for_run(1));
  auto snapshot = service->snapshot();
  EXPECT_EQ(snapshot["job"]["state"], "running");
  EXPECT_EQ(snapshot["job"]["message"], "Approved. Polaris is changing this PC's setup.");
  finish({.exit_status = 1, .approved = true, .output = "Spaces setup started.\n",
    .errors = "Spaces setup: Stop Spaces streams and quit any other Polaris before changing security setup. Still active: 1 Polaris multiseat input device.\n"});
  ASSERT_TRUE(wait_idle(*service));
  snapshot = service->snapshot();
  EXPECT_EQ(snapshot["job"]["state"], "refused");
  EXPECT_EQ(snapshot["job"]["detail"],
    "Stop Spaces streams and quit any other Polaris before changing security setup. Still active: 1 Polaris multiseat input device.");
  // An approved run may have changed this PC even when the helper stopped, so the check is read again.
  EXPECT_EQ(snapshot["job"]["check"]["state"], "ready");
}

TEST_F(SpacesHostAdminService, ARefusalBeforeThePromptLeavesNoJobAndOpensTheGateAgain) {
  auto service = make();
  facts.stream_active = true;
  auto result = service->submit({spaces::host_action_e::docker_access, std::string(first_id)});
  EXPECT_EQ(result.status, 409);
  ASSERT_TRUE(result.refusal);
  EXPECT_EQ(result.refusal->code, "stream_active");
  EXPECT_FALSE(service->running());
  auto snapshot = service->snapshot();
  EXPECT_EQ(snapshot["available"], false);
  EXPECT_EQ(snapshot["reason"], "stream_active");
  EXPECT_EQ(snapshot["message"], "End every stream on this PC, then try again.");
  EXPECT_TRUE(snapshot["job"].is_null());
  facts.session_problem = "inactive_session";
  EXPECT_EQ(service->submit({spaces::host_action_e::docker_access, std::string(first_id)}).refusal->code, "no_local_desktop");
  facts_throw = true;
  EXPECT_EQ(service->submit({spaces::host_action_e::docker_access, std::string(first_id)}).refusal->code, "host_unknown");
  EXPECT_EQ(service->snapshot()["reason"], "host_unknown");
  EXPECT_TRUE(argvs.empty());
}

TEST_F(SpacesHostAdminService, ShutdownClosesAWaitingPromptAndRefusesNewOnes) {
  auto service = make();
  ASSERT_EQ(service->submit({spaces::host_action_e::security_install, std::string(first_id)}).status, 202);
  ASSERT_TRUE(wait_for_run(1));
  service->shutdown();
  EXPECT_FALSE(service->running());
  EXPECT_EQ(service->submit({spaces::host_action_e::security_install, std::string(second_id)}).status, 503);
  EXPECT_EQ(service->snapshot()["reason"], "closing");
}

TEST_F(SpacesHostAdminService, TheGateFollowsTheInstalledService) {
  EXPECT_FALSE(spaces::host_admin_running());
  auto service = make();
  ASSERT_TRUE(spaces::install_host_admin_service(service));
  auto uninstall = util::fail_guard([&] {
    finish({});
    service->shutdown();
    spaces::uninstall_host_admin_service(service);
  });
  EXPECT_FALSE(spaces::install_host_admin_service(make()));
  EXPECT_FALSE(spaces::host_admin_running());
  ASSERT_EQ(service->submit({spaces::host_action_e::docker_access, std::string(first_id)}).status, 202);
  EXPECT_TRUE(spaces::host_admin_running());
  finish({.exit_status = 126});
  ASSERT_TRUE(wait_idle(*service));
  EXPECT_FALSE(spaces::host_admin_running());
  EXPECT_EQ(service->snapshot()["job"]["state"], "cancelled");
  EXPECT_EQ(rechecks, 0U);
}

TEST_F(SpacesHostAdminService, RoutesNeedTheConsoleLoginAndCsrfAndSayWhyARequestWasRefused) {
  const auto old_config = config::sunshine;
  auto restore = util::fail_guard([&] { config::sunshine = old_config; });
  config::sunshine.username = "test-admin";
  config::sunshine.api_key = "isolated-host-admin-test-key";
  std::array<char, 64> pattern {};
  const std::string value = "/tmp/polaris-host-admin-XXXXXX";
  std::copy(value.begin(), value.end(), pattern.begin());
  ASSERT_NE(mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path root = pattern.data();
  auto cleanup = util::fail_guard([&] { std::filesystem::remove_all(root); });
  const auto credentials = crypto::gen_creds("localhost", 2048);
  ASSERT_TRUE(private_state_file::write_atomic(root / "cert.pem", credentials.x509));
  ASSERT_TRUE(private_state_file::write_atomic(root / "key.pem", credentials.pkey));
  auto service = make();
  confighttp::with_web_session_for_tests(root / "sessions.json", "host-csrf", [&](const std::string &cookie) {
    SimpleWeb::Server<SimpleWeb::HTTPS> server((root / "cert.pem").string(), (root / "key.pem").string());
    server.config.address = "127.0.0.1";
    server.config.port = 0;
    server.config.timeout_request = 5;
    server.config.timeout_content = 5;
    confighttp::registerSpacesSetupRoutes(server);
    std::atomic<unsigned short> port {0};
    std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
    auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
    for (int attempt = 0; attempt < 100 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
    ASSERT_NE(port, 0);
    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port.load()), false);
    client.config.timeout = 5;
    const auto call = [&](const std::string &method, const std::string &body, SimpleWeb::CaseInsensitiveMultimap headers) {
      headers.emplace("Content-Type", "application/json");
      const auto response = client.request(method, "/api/spaces/setup/host-action", body, headers);
      return std::pair {std::stoi(response->status_code), response->content.string()};
    };
    const SimpleWeb::CaseInsensitiveMultimap signed_in {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", "host-csrf"}};
    const auto install = json {{"action", "security_install"}, {"request_id", first_id}}.dump();
    EXPECT_EQ(call("GET", "", {{"Cookie", "auth=" + cookie}}).first, 404);
    ASSERT_TRUE(spaces::install_host_admin_service(service));
    auto uninstall = util::fail_guard([&] {
      finish({});
      service->shutdown();
      spaces::uninstall_host_admin_service(service);
    });
    EXPECT_EQ(call("GET", "", {}).first, 401);
    EXPECT_EQ(call("POST", install, {}).first, 403);
    EXPECT_EQ(call("POST", install, {{"Cookie", "auth=" + cookie}}).first, 403);
    EXPECT_EQ(call("POST", install, {{"X-CSRF-Token", "host-csrf"}}).first, 401);
    EXPECT_EQ(call("POST", "{\"action\":\"docker_install\",\"request_id\":\"" + std::string(first_id) + "\"}", signed_in).first, 400);
    {
      std::lock_guard lock(mutex);
      EXPECT_TRUE(argvs.empty());
      facts.spaces_active = true;
    }
    auto [status, body] = call("POST", install, signed_in);
    EXPECT_EQ(status, 409);
    auto reply = json::parse(body);
    EXPECT_EQ(reply["accepted"], false);
    EXPECT_EQ(reply["refusal"]["code"], "spaces_active");
    EXPECT_EQ(reply["reason"], "spaces_active");
    {
      std::lock_guard lock(mutex);
      facts.spaces_active = false;
    }
    std::tie(status, body) = call("POST", install, signed_in);
    EXPECT_EQ(status, 202);
    EXPECT_EQ(json::parse(body)["accepted"], true);
    ASSERT_TRUE(wait_for_run(1));
    EXPECT_EQ(call("POST", install, signed_in).first, 200);
    const auto other = json {{"action", "docker_access"}, {"request_id", second_id}}.dump();
    std::tie(status, body) = call("POST", other, signed_in);
    EXPECT_EQ(status, 409);
    EXPECT_EQ(json::parse(body)["refusal"]["code"], "host_setup_running");
    finish({.exit_status = 0, .approved = true});
    ASSERT_TRUE(wait_idle(*service));
    std::tie(status, body) = call("GET", "", {{"Cookie", "auth=" + cookie}});
    EXPECT_EQ(status, 200);
    EXPECT_EQ(json::parse(body)["job"]["state"], "done");
  });
}
#endif
