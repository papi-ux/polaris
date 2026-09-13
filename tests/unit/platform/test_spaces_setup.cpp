#include "src/platform/linux/spaces_setup.h"
#include <gtest/gtest.h>
#include <stdexcept>

#ifdef __linux__
namespace {
  using namespace multiseat;
  using json = nlohmann::json;
  class setup_host_t : public container::host_t {
  public:
    bool runtime = true, input = true, gpu = true;
    unsigned calls = 0;
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
      ++calls;
      auto expected = container::command_prefix({});
      expected.insert(expected.end(), {"info", "--format={{json .}}"});
      EXPECT_EQ(argv, expected);
      EXPECT_EQ(timeout, std::chrono::seconds(3));
      EXPECT_EQ(bound, 65536);
      return result;
    }
  };
  const json &check(const json &value, const char *id) {
    for (const auto &item : value.at("checks")) if (item.at("id") == id) return item;
    throw std::runtime_error("missing setup check");
  }
  spaces::setup_facts_t prepared() {
    spaces::setup_facts_t f;
    f.uid = f.gid = 1000;
    f.docker_cli = f.runc = f.daemon_replied = f.daemon_linux = f.daemon_runc = f.input_access = f.gpu_access = true;
    return f;
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
  f = prepared(); f.uid = 1001;
  EXPECT_EQ(check(spaces::describe_setup(f), "identity")["state"], "required");
  f = prepared(); f.gid = 1001;
  EXPECT_EQ(check(spaces::describe_setup(f), "identity")["state"], "required");
}

TEST(SpacesSetup, ProbeOnlyReadsBoundedLocalDockerInfo) {
  setup_host_t host;
  const auto value = spaces::inspect_setup(host, true, true);
  EXPECT_EQ(host.calls, 1);
  EXPECT_EQ(value["host_prerequisites_ready"], true);
  EXPECT_EQ(value["service_uid"], 1000);
  EXPECT_EQ(check(value, "gpu")["state"], "ready");
  EXPECT_FALSE(value.contains("daemon"));
}

TEST(SpacesSetup, DoesNotExecuteAnUntrustedRuntime) {
  setup_host_t host; host.runtime = false;
  const auto value = spaces::inspect_setup(host, false, false);
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
    const auto value = spaces::inspect_setup(host, false, false);
    EXPECT_EQ(check(value, "docker_access")["state"], "required");
    EXPECT_EQ(value["host_prerequisites_ready"], false);
    EXPECT_EQ(value.dump().find("private daemon error"), std::string::npos);
    EXPECT_EQ(value.dump().find("/untrusted"), std::string::npos);
  }
}

TEST(SpacesSetup, RefusesFailedTimedOutAndTruncatedProbes) {
  for (int failure = 0; failure < 3; ++failure) {
    setup_host_t host;
    host.result.exit_status = failure == 0 ? 1 : 0;
    host.result.timed_out = failure == 1;
    host.result.output_truncated = failure == 2;
    EXPECT_EQ(spaces::inspect_setup(host, false, false)["host_prerequisites_ready"], false);
  }
}
#endif
