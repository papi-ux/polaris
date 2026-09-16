#include "src/platform/linux/spaces_setup.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <stdexcept>

#ifdef __linux__
namespace {
  using namespace multiseat;
  using json = nlohmann::json;
  class setup_host_t : public container::host_t {
  public:
    bool runtime = true, input = true, gpu = true;
    unsigned calls = 0, inspections = 0;
    container::command_result_t result {.exit_status = 0, .output =
      R"({"OSType":"linux","SecurityOptions":["name=selinux"],"Runtimes":{"runc":{"path":"runc"}}})"};
    std::uint64_t effective_uid() const override { return 1000; }
    bool executable_file(const std::filesystem::path &) const override { return true; }
    bool trusted_runtime_file(const std::filesystem::path &) const override { return runtime; }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return std::vector<std::uint64_t> {}; }
    bool readable_directory(const std::filesystem::path &) const override { return true; }
    bool private_read_write_directory(const std::filesystem::path &) const override { return true; }
    bool private_readable_file(const std::filesystem::path &) const override { return true; }
    std::optional<container::character_device_identity_t> read_write_character_device(const std::filesystem::path &path) const override {
      if ((input && (path == "/dev/uinput" || path == "/dev/uhid")) || (gpu && path == "/dev/dri/renderD128"))
        return container::character_device_identity_t {};
      return std::nullopt;
    }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &, std::size_t) const override { return std::nullopt; }
    container::command_result_t run(const std::vector<std::string> &argv, std::chrono::milliseconds timeout, std::size_t bound) override {
      EXPECT_EQ(bound, 65536);
      auto inspect = container::command_prefix({});
      inspect.insert(inspect.end(), {"image", "inspect"});
      if (argv.size() == inspect.size() + 1 && std::equal(inspect.begin(), inspect.end(), argv.begin())) {
        // Only a lab build compiles a runtime catalog; its image is inspected, never pulled.
        ++inspections;
        EXPECT_EQ(timeout, std::chrono::seconds(5));
        return {.exit_status = 1, .output = "[]"};
      }
      ++calls;
      auto expected = container::command_prefix({});
      expected.insert(expected.end(), {"info", "--format={{json .}}"});
      EXPECT_EQ(argv, expected);
      EXPECT_EQ(timeout, std::chrono::seconds(3));
      return result;
    }
  };
  // Answers only `docker image inspect`, with one fixed reply.
  class runtime_host_t : public container::host_t {
  public:
    std::vector<std::vector<std::string>> calls;
    container::command_result_t reply {.exit_status = 1, .output = "[]\n"};
    std::uint64_t effective_uid() const override { return 1000; }
    bool executable_file(const std::filesystem::path &) const override { return true; }
    bool trusted_runtime_file(const std::filesystem::path &) const override { return true; }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return std::vector<std::uint64_t> {}; }
    bool readable_directory(const std::filesystem::path &) const override { return false; }
    bool private_read_write_directory(const std::filesystem::path &) const override { return false; }
    bool private_readable_file(const std::filesystem::path &) const override { return false; }
    std::optional<container::character_device_identity_t> read_write_character_device(const std::filesystem::path &) const override {
      ADD_FAILURE() << "The runtime check cannot open devices";
      return std::nullopt;
    }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &, std::size_t) const override { return std::nullopt; }
    container::command_result_t run(const std::vector<std::string> &argv, std::chrono::milliseconds timeout, std::size_t bound) override {
      calls.push_back(argv);
      EXPECT_EQ(timeout, std::chrono::seconds(5));
      EXPECT_EQ(bound, 65536U);
      return reply;
    }
  };
  spaces::runtime_t catalog_runtime(const char *id, const char *variant, const char *driver, char digest) {
    return {id, variant, std::string(40, 'a'), "sha256:" + std::string(64, digest),
      "sha256:" + std::string(64, static_cast<char>(digest + 1)), driver};
  }
  const auto nvidia610 = catalog_runtime("steam-nvidia-610", "nvidia", "610.57.04", '1');
  const auto nvidia615 = catalog_runtime("steam-nvidia-615", "nvidia", "615.20.01", '3');
  const auto amd_intel = catalog_runtime("steam-default", "default", "", '5');
  json inspected(const spaces::runtime_t &r, const std::string &revision = std::string(40, 'a')) {
    return json::array({{{"Id", r.config_digest}, {"Os", "linux"}, {"Architecture", "amd64"},
      {"RepoDigests", json::array({r.reference()})}, {"Config", {
        {"Entrypoint", json::array({"/usr/bin/polaris-seat-worker"})}, {"Cmd", json::array({"run"})},
        {"Labels", {{"org.opencontainers.image.source", "https://github.com/papi-ux/polaris"},
          {"org.opencontainers.image.revision", revision}, {"io.polaris.multiseat.profile", "steam"},
          {"io.polaris.multiseat.architecture", "linux/amd64"}, {"io.polaris.multiseat.media-contract", "1"},
          {"io.polaris.multiseat.nvidia.driver", r.nvidia_driver}}}}}}});
  }
  const json &check(const json &value, const char *id) {
    for (const auto &item : value.at("checks")) if (item.at("id") == id) return item;
    throw std::runtime_error("missing setup check");
  }
  spaces::setup_facts_t prepared() {
    spaces::setup_facts_t f;
    f.uid = f.gid = 1000;
    f.docker_cli = f.runc = f.daemon_replied = f.daemon_linux = f.daemon_runc = f.input_access = f.gpu_access = true;
    f.security = spaces::describe_security({.seccomp = true});
    return f;
  }
  json runtime_check(const spaces::runtime_facts_t &facts) {
    auto f = prepared();
    f.runtime = facts;
    return check(spaces::describe_setup(f), "runtime");
  }
}

TEST(SpacesSetup, DockerAloneDoesNotClaimReadyToPlay) {
  auto f = prepared();
  const auto value = spaces::describe_setup(f);
  EXPECT_EQ(value["host_prerequisites_ready"], true);
  EXPECT_EQ(value["configured"], false);
  EXPECT_EQ(value["available"], false);
  EXPECT_EQ(check(value, "spaces")["state"], "not_configured");
  f.controller_enabled = true;
  EXPECT_EQ(check(spaces::describe_setup(f), "spaces")["state"], "required");
  f.controller_available = true;
  EXPECT_EQ(check(spaces::describe_setup(f), "spaces")["state"], "ready");
}

TEST(SpacesSetup, EveryCheckLinksTheGuideSectionThatFixesIt) {
  spaces::setup_facts_t f;
  const auto value = spaces::describe_setup(f);
  for (const auto &[id, anchor] : std::vector<std::pair<std::string, std::string>> {
         {"docker", "#prepare-docker-from-spaces"}, {"docker_access", "#prepare-docker-from-spaces"},
         {"identity", "#gaming-runtime-account"}, {"input", "#controller-access"}, {"gpu", "#graphics-access"},
         {"security", "#prepare-spaces-security-support"}, {"runtime", "#download-the-gaming-runtime"},
         {"spaces", "#prepare-your-first-space"}}) {
    EXPECT_EQ(check(value, id.c_str())["doc_anchor"], anchor) << id;
  }
}

TEST(SpacesSetup, EveryHostPrerequisiteMustPass) {
  for (auto member : {&spaces::setup_facts_t::docker_cli, &spaces::setup_facts_t::runc,
      &spaces::setup_facts_t::daemon_replied, &spaces::setup_facts_t::daemon_linux,
      &spaces::setup_facts_t::daemon_runc, &spaces::setup_facts_t::input_access,
      &spaces::setup_facts_t::gpu_access}) {
    auto f = prepared(); f.*member = false;
    EXPECT_EQ(spaces::describe_setup(f)["host_prerequisites_ready"], false);
  }
  auto f = prepared(); f.daemon_rootless = true;
  EXPECT_EQ(spaces::describe_setup(f)["host_prerequisites_ready"], false);
  f = prepared(); f.security.ready = false;
  EXPECT_EQ(spaces::describe_setup(f)["host_prerequisites_ready"], false);
  f = prepared(); f.uid = 1001;
  EXPECT_EQ(check(spaces::describe_setup(f), "identity")["state"], "required");
  f = prepared(); f.gid = 1001;
  EXPECT_EQ(check(spaces::describe_setup(f), "identity")["state"], "required");
}

TEST(SpacesSetup, ProbeOnlyReadsBoundedLocalDockerInfo) {
  setup_host_t host;
  const auto value = spaces::inspect_setup(host, true, true, spaces::security_facts_t {.seccomp = true});
  EXPECT_EQ(host.calls, 1);
  EXPECT_EQ(value["host_prerequisites_ready"], true);
  EXPECT_EQ(value["service_uid"], 1000);
  EXPECT_EQ(check(value, "gpu")["state"], "ready");
  EXPECT_FALSE(value.contains("daemon"));
  // The runtime check reads the catalog compiled into this build.
  const auto &catalog = spaces::trusted_runtimes();
  ASSERT_TRUE(catalog);
  EXPECT_EQ(check(value, "runtime")["runtime"]["status"] == "not_published", catalog->empty());
  EXPECT_LE(host.inspections, 1U);
}

TEST(SpacesSetup, DoesNotExecuteAnUntrustedRuntime) {
  setup_host_t host; host.runtime = false;
  const auto value = spaces::inspect_setup(host, false, false, spaces::security_facts_t {.seccomp = true});
  EXPECT_EQ(host.calls, 0);
  EXPECT_EQ(value["host_prerequisites_ready"], false);
}

TEST(SpacesSetup, RefusesUnverifiedDaemonRepliesWithoutReturningTheirContents) {
  for (const auto &output : {
      "not json: private daemon error",
      R"({"OSType":"linux","SecurityOptions":[]})",
      R"({"OSType":"linux","SecurityOptions":[{}],"Runtimes":{"runc":{"path":"runc"}}})",
      R"({"OSType":"linux","SecurityOptions":["name=rootless"],"Runtimes":{"runc":{"path":"runc"}}})",
      R"({"OSType":"windows","SecurityOptions":[],"Runtimes":{"runc":{"path":"runc"}}})",
      R"({"OSType":"linux","SecurityOptions":[],"Runtimes":{"runc":{"path":"/untrusted/runc"}}})"}) {
    setup_host_t host; host.result.output = output;
    const auto value = spaces::inspect_setup(host, false, false, spaces::security_facts_t {.seccomp = true});
    EXPECT_EQ(check(value, "docker_access")["state"], "required");
    EXPECT_EQ(value["host_prerequisites_ready"], false);
    EXPECT_EQ(value.dump().find("private daemon error"), std::string::npos);
    EXPECT_EQ(value.dump().find("/untrusted"), std::string::npos);
  }
}

TEST(SpacesSetup, AdministratorFixesAreOfferedOnlyWhereAnApprovedPromptFixesTheCheck) {
  auto f = prepared();
  f.daemon_replied = f.daemon_linux = f.daemon_runc = false;
  auto value = spaces::describe_setup(f);
  EXPECT_EQ(check(value, "docker_access")["host_action"], "docker_access");
  EXPECT_EQ(check(value, "docker_access")["detail"], "Start Docker and allow the Polaris service account to use it.");
  for (const auto id : {"docker", "identity", "input", "gpu", "security", "runtime", "spaces"})
    EXPECT_FALSE(check(value, id).contains("host_action")) << id;
  auto image = f;
  image.immutable_host = true;
  EXPECT_FALSE(check(spaces::describe_setup(image), "docker_access").contains("host_action"));
  auto missing = f;
  missing.docker_cli = false;
  EXPECT_FALSE(check(spaces::describe_setup(missing), "docker_access").contains("host_action"));
  auto pending = f;
  pending.docker_access_pending = true;
  value = spaces::describe_setup(pending);
  EXPECT_FALSE(check(value, "docker_access").contains("host_action"));
  EXPECT_EQ(check(value, "docker_access")["detail"],
    "Polaris was given access to Docker after it started, and a running Polaris keeps the access it started with. Restart this PC, then recheck.");
  auto rootless = prepared();
  rootless.daemon_rootless = true;
  EXPECT_FALSE(check(spaces::describe_setup(rootless), "docker_access").contains("host_action"));

  auto security = prepared();
  security.security = spaces::describe_security({.seccomp = true, .selinux = true, .kernel_probe = true, .enforcing = true});
  ASSERT_EQ(security.security.code, "install_selinux");
  EXPECT_EQ(check(spaces::describe_setup(security), "security")["host_action"], "security_install");
  security.immutable_host = true;
  EXPECT_FALSE(check(spaces::describe_setup(security), "security").contains("host_action"));
  for (const auto &facts : {spaces::security_facts_t {.seccomp = true, .selinux = true, .kernel_probe = true},
         spaces::security_facts_t {.seccomp = false},
         spaces::security_facts_t {.seccomp = true, .selinux = true},
         spaces::security_facts_t {.seccomp = true, .selinux = true, .kernel_probe = true, .enforcing = true, .contexts = true, .rule = true, .receipt = true}}) {
    auto other = prepared();
    other.security = spaces::describe_security(facts);
    EXPECT_FALSE(check(spaces::describe_setup(other), "security").contains("host_action")) << other.security.code;
  }
}

TEST(SpacesSetup, DockerAccessGrantedAfterPolarisStartedWaitsForARestart) {
  using container::group_membership_t;
  const std::vector<std::uint64_t> without {10, 39}, with {10, 39, 966};
  EXPECT_TRUE(spaces::docker_access_pending(group_membership_t {966, true}, 1000, without));
  EXPECT_FALSE(spaces::docker_access_pending(group_membership_t {966, true}, 1000, with));
  EXPECT_FALSE(spaces::docker_access_pending(group_membership_t {966, true}, 966, without));
  EXPECT_FALSE(spaces::docker_access_pending(group_membership_t {966, false}, 1000, without));
  EXPECT_FALSE(spaces::docker_access_pending(std::nullopt, 1000, without));
  EXPECT_FALSE(spaces::docker_access_pending(group_membership_t {966, true}, 1000, std::nullopt));

  class grouped_host_t : public setup_host_t {
  public:
    std::optional<container::group_membership_t> docker {container::group_membership_t {966, true}};
    std::optional<container::group_membership_t> group_membership(std::string_view group) const override {
      EXPECT_EQ(group, "docker");
      return docker;
    }
  } host;
  host.result = {.exit_status = 1, .output = "permission denied while trying to connect to the Docker daemon socket"};
  auto value = spaces::inspect_setup(host, false, false, spaces::security_facts_t {.seccomp = true});
  EXPECT_NE(check(value, "docker_access")["detail"].get<std::string>().find("Restart this PC, then recheck."), std::string::npos);
  EXPECT_FALSE(check(value, "docker_access").contains("host_action"));
  host.docker->member = false;
  value = spaces::inspect_setup(host, false, false, spaces::security_facts_t {.seccomp = true});
  EXPECT_EQ(check(value, "docker_access")["detail"], "Start Docker and allow the Polaris service account to use it.");
  // A host with a working engine never reads the group: access is already there.
  host.docker->member = true;
  host.result = setup_host_t {}.result;
  value = spaces::inspect_setup(host, false, false, spaces::security_facts_t {.seccomp = true});
  EXPECT_EQ(check(value, "docker_access")["state"], "ready");
}

TEST(SpacesSetup, RefusesFailedTimedOutAndTruncatedProbes) {
  for (int failure = 0; failure < 3; ++failure) {
    setup_host_t host;
    host.result.exit_status = failure == 0 ? 1 : 0;
    host.result.timed_out = failure == 1;
    host.result.output_truncated = failure == 2;
    EXPECT_EQ(spaces::inspect_setup(host, false, false, spaces::security_facts_t {.seccomp = true})["host_prerequisites_ready"], false);
  }
}
TEST(SpacesSetup, RuntimeCheckFollowsSecurityAndIsNotAHostPrerequisite) {
  auto f = prepared();
  const auto value = spaces::describe_setup(f);
  std::vector<std::string> order;
  for (const auto &item : value.at("checks")) order.push_back(item.at("id"));
  EXPECT_EQ(order, (std::vector<std::string> {"docker", "docker_access", "identity", "input", "gpu", "security", "runtime", "spaces"}));
  // No catalog entry: a neutral row, and nothing for this PC to do.
  EXPECT_EQ(check(value, "runtime"), json({{"id", "runtime"}, {"title", "Gaming runtime"}, {"state", "not_configured"},
    {"detail", "This Polaris build has no approved gaming runtime yet."}, {"action", ""},
    {"doc_anchor", "#download-the-gaming-runtime"}, {"runtime", {{"status", "not_published"}, {"code", "runtime_not_published"}}}}));
  EXPECT_EQ(value["host_prerequisites_ready"], true);
  runtime_host_t host; host.reply = {.exit_status = 0, .output = inspected(nvidia610, std::string(40, 'f')).dump()};
  f.runtime = spaces::inspect_runtime(host, {nvidia610}, "610.57.04", true);
  EXPECT_EQ(check(spaces::describe_setup(f), "runtime")["runtime"]["status"], "failed");
  EXPECT_EQ(spaces::describe_setup(f)["host_prerequisites_ready"], true);
}

TEST(SpacesSetup, RuntimeVariantFollowsTheLoadedNvidiaDriver) {
  const std::vector<spaces::runtime_t> catalog {amd_intel, nvidia610, nvidia615};
  EXPECT_EQ(spaces::choose_runtime(catalog, std::nullopt).runtime->id, "steam-default");
  EXPECT_EQ(spaces::choose_runtime(catalog, "610.57.04").runtime->id, "steam-nvidia-610");
  EXPECT_EQ(spaces::choose_runtime(catalog, "615.20.01").runtime->id, "steam-nvidia-615");
  for (const auto &driver : {"580.95.05", "610.57", ""}) {
    const auto choice = spaces::choose_runtime(catalog, std::string {driver});
    EXPECT_FALSE(choice.runtime) << driver;
    EXPECT_EQ(choice.code, "driver_mismatch") << driver;
  }
  EXPECT_EQ(spaces::choose_runtime({amd_intel}, "610.57.04").code, "graphics_unsupported");
  EXPECT_EQ(spaces::choose_runtime({nvidia610}, std::nullopt).code, "graphics_unsupported");
  EXPECT_EQ(spaces::choose_runtime({}, std::nullopt).code, "runtime_not_published");
  EXPECT_EQ(spaces::choose_runtime({}, "610.57.04").code, "runtime_not_published");

  // When no entry fits, the check says which driver the build needs and never
  // asks Docker for a runtime this PC cannot use.
  runtime_host_t host;
  auto mismatch = runtime_check(spaces::inspect_runtime(host, {nvidia610, nvidia615}, "580.95.05", true));
  EXPECT_EQ(mismatch["state"], "required");
  EXPECT_EQ(mismatch["action"], "");
  EXPECT_EQ(mismatch["runtime"], json({{"status", "unsupported"}, {"code", "driver_mismatch"}}));
  EXPECT_EQ(mismatch["detail"], "The gaming runtime in this Polaris build needs NVIDIA driver 610.57.04 or 615.20.01. "
    "This PC runs 580.95.05. Install the matching driver, restart the PC, then recheck.");
  EXPECT_EQ(runtime_check(spaces::inspect_runtime(host, {nvidia610}, "", true))["detail"],
    "The gaming runtime in this Polaris build needs NVIDIA driver 610.57.04. Polaris could not read the NVIDIA driver version on this PC.");
  EXPECT_EQ(runtime_check(spaces::inspect_runtime(host, {nvidia610}, std::nullopt, true))["detail"],
    "The gaming runtime in this Polaris build needs NVIDIA graphics with driver 610.57.04. No NVIDIA driver is loaded on this PC.");
  const auto nvidia_host = runtime_check(spaces::inspect_runtime(host, {amd_intel}, "610.57.04", true));
  EXPECT_EQ(nvidia_host["runtime"]["code"], "graphics_unsupported");
  EXPECT_EQ(nvidia_host["detail"], "The gaming runtime in this Polaris build is for AMD and Intel graphics. "
    "This PC uses the NVIDIA driver, and no NVIDIA runtime is approved yet.");
  EXPECT_TRUE(host.calls.empty());
}

TEST(SpacesSetup, RuntimeCheckVerifiesThePinnedImageWithOneBoundedInspect) {
  const std::vector<spaces::runtime_t> catalog {amd_intel, nvidia610};
  runtime_host_t host;
  auto available = runtime_check(spaces::inspect_runtime(host, catalog, "610.57.04", true));
  ASSERT_EQ(host.calls.size(), 1U);
  auto expected = container::command_prefix({});
  expected.insert(expected.end(), {"image", "inspect", nvidia610.reference()});
  EXPECT_EQ(host.calls.front(), expected);
  EXPECT_EQ(available, json({{"id", "runtime"}, {"title", "Gaming runtime"}, {"state", "required"},
    {"detail", "This PC needs the gaming runtime for NVIDIA driver 610.57.04. It is not downloaded yet, and the download is several gigabytes."},
    {"action", "download_runtime"}, {"doc_anchor", "#download-the-gaming-runtime"},
    {"runtime", {{"status", "available"}, {"code", "not_downloaded"}, {"id", "steam-nvidia-610"},
      {"variant", "nvidia"}, {"nvidia_driver", "610.57.04"}}}}));

  host.reply = {.exit_status = 0, .output = inspected(amd_intel).dump()};
  const auto ready = runtime_check(spaces::inspect_runtime(host, catalog, std::nullopt, true));
  EXPECT_EQ(host.calls.back().back(), amd_intel.reference());
  EXPECT_EQ(ready["state"], "ready");
  EXPECT_EQ(ready["action"], "");
  EXPECT_EQ(ready["detail"], "The gaming runtime for AMD and Intel graphics is downloaded and verified.");
  EXPECT_EQ(ready["runtime"]["status"], "ready");
  EXPECT_EQ(ready["runtime"]["code"], "runtime_ready");

  // An image under the pinned reference that differs from the catalog entry.
  host.reply = {.exit_status = 0, .output = inspected(nvidia610, std::string(40, 'f')).dump()};
  const auto mismatch = runtime_check(spaces::inspect_runtime(host, catalog, "610.57.04", true));
  EXPECT_EQ(mismatch["state"], "required");
  EXPECT_EQ(mismatch["action"], "retry_runtime");
  EXPECT_EQ(mismatch["runtime"]["status"], "failed");
  EXPECT_EQ(mismatch["runtime"]["code"], "runtime_identity_mismatch");
  EXPECT_EQ(mismatch["detail"], "The gaming runtime on this PC does not match this Polaris build, so Spaces will not use it. "
    "Retry, and open Doctor & Support if it persists.");

  // Verification that could not finish is a failure to retry, never a download offer.
  for (const auto &reply : std::vector<container::command_result_t> {
      {.exit_status = 0, .timed_out = true, .output = "[]"},
      {.exit_status = 1, .output_truncated = true, .output = "[]"},
      {.exit_status = 1, .output = "not json: private daemon error"},
      {.exit_status = 125, .output = "[]"},
      {.exit_status = 0, .output = "private daemon error"}}) {
    host.reply = reply;
    const auto failed = runtime_check(spaces::inspect_runtime(host, catalog, "610.57.04", true));
    EXPECT_EQ(failed["runtime"]["status"], "failed") << reply.output;
    EXPECT_EQ(failed["runtime"]["code"], "inspection_failed") << reply.output;
    EXPECT_EQ(failed["action"], "retry_runtime");
    EXPECT_EQ(failed["detail"], "Polaris could not verify the gaming runtime on this PC. Retry, and check Docker if it keeps failing.");
    EXPECT_EQ(failed.dump().find("private daemon error"), std::string::npos);
  }

  // Without an engine that answered, Docker is not asked; the download waits for Docker.
  const auto calls = host.calls.size();
  const auto waiting = runtime_check(spaces::inspect_runtime(host, catalog, "610.57.04", false));
  EXPECT_EQ(host.calls.size(), calls);
  EXPECT_EQ(waiting["runtime"]["code"], "docker_unavailable");
  EXPECT_EQ(waiting["action"], "download_runtime");
  EXPECT_EQ(waiting["detail"], "This PC needs the gaming runtime for NVIDIA driver 610.57.04. Polaris checks whether it is downloaded once Docker Engine is ready.");
  EXPECT_EQ(runtime_check(spaces::inspect_runtime(host, {}, "610.57.04", true))["runtime"]["status"], "not_published");
  EXPECT_EQ(host.calls.size(), calls);
}

TEST(SpacesSetup, RuntimeInspectionIsKeptBrieflyAndForgottenAfterADownload) {
  auto now = std::chrono::steady_clock::time_point {} + std::chrono::hours(1);
  spaces::runtime_inspection_cache_t cache(std::chrono::seconds(15), [&] { return now; });
  runtime_host_t host;
  const auto status = [&](const std::optional<std::string> &driver = "610.57.04") {
    return spaces::inspect_runtime(host, {amd_intel, nvidia610}, driver, true, &cache).status;
  };
  EXPECT_EQ(status(), "available");
  host.reply = {.exit_status = 0, .output = inspected(nvidia610).dump()};
  EXPECT_EQ(status(), "available");
  EXPECT_EQ(host.calls.size(), 1U);
  cache.forget();
  EXPECT_EQ(status(), "ready");
  EXPECT_EQ(host.calls.size(), 2U);
  now += std::chrono::seconds(14);
  EXPECT_EQ(status(), "ready");
  EXPECT_EQ(host.calls.size(), 2U);
  now += std::chrono::seconds(1);
  host.reply = {.exit_status = 0, .timed_out = true};
  EXPECT_EQ(status(), "failed");
  EXPECT_EQ(host.calls.size(), 3U);
  // An answer Docker could not give is asked again at once.
  EXPECT_EQ(status(), "failed");
  EXPECT_EQ(host.calls.size(), 4U);
  // Another runtime is another question.
  host.reply = {.exit_status = 1, .output = "[]"};
  EXPECT_EQ(status(), "available");
  EXPECT_EQ(status(std::nullopt), "available");
  EXPECT_EQ(host.calls.size(), 6U);
  // A download that finishes while Docker is answering makes that answer stale.
  cache.forget();
  EXPECT_EQ(cache.remember("stale", [&] { cache.forget(); return spaces::runtime_image_e::absent; }), spaces::runtime_image_e::absent);
  EXPECT_EQ(cache.remember("stale", [] { return spaces::runtime_image_e::verified; }), spaces::runtime_image_e::verified);
  EXPECT_EQ(cache.remember("stale", [] { return spaces::runtime_image_e::absent; }), spaces::runtime_image_e::verified);
}

TEST(SpacesSecurity, EnforcingHostsRequireEveryPieceOfTheMatchingInstallation) {
  spaces::security_facts_t f {.seccomp = true, .selinux = true, .kernel_probe = true,
    .enforcing = true, .contexts = true, .rule = true, .receipt = true};
  EXPECT_EQ(spaces::installed_worker_label(f), "polaris_nvidia_worker_t");
  for (auto member : {&spaces::security_facts_t::seccomp, &spaces::security_facts_t::kernel_probe,
      &spaces::security_facts_t::enforcing, &spaces::security_facts_t::contexts,
      &spaces::security_facts_t::rule, &spaces::security_facts_t::receipt}) {
    auto missing = f; missing.*member = false;
    EXPECT_FALSE(spaces::describe_security(missing).ready);
    EXPECT_FALSE(spaces::installed_worker_label(missing));
  }
  f.enforcing = false;
  EXPECT_EQ(spaces::describe_security(f).code, "not_enforcing");
  f.kernel_probe = false;
  EXPECT_EQ(spaces::describe_security(f).code, "probe_failed");
  f.selinux = false;
  EXPECT_EQ(spaces::installed_worker_label(f), "");
  f.seccomp = false;
  EXPECT_EQ(spaces::describe_security(f).code, "package_missing");
}
#endif
