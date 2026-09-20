#include "src/platform/linux/spaces_runtime.h"
#include <gtest/gtest.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "spaces_runtime_catalog.h"

#ifdef __linux__
namespace {
  using namespace multiseat;
  using json = nlohmann::json;
  json entry() {
    return {{"id", "steam-test"}, {"profile", "steam"}, {"variant", "default"},
      {"platform", "linux/amd64"}, {"media_contract", 1}, {"uid", 1000}, {"gid", 1000},
      {"source_revision", std::string(40, 'a')}, {"registry_digest", "sha256:" + std::string(64, 'b')},
      {"config_digest", "sha256:" + std::string(64, 'c')}, {"nvidia_driver", ""},
      {"nvidia_minimum_driver", ""}};
  }
  std::string catalog(json entries) { return json({{"schema", 2}, {"runtimes", entries}}).dump(); }
  spaces::runtime_t runtime() { return spaces::decode_runtime_catalog(catalog(json::array({entry()})))->front(); }
  json inspected(const spaces::runtime_t &r) {
    return json::array({{{"Id", r.config_digest}, {"Os", "linux"}, {"Architecture", "amd64"},
      {"RepoDigests", json::array({r.reference()})}, {"Config", {
        {"Entrypoint", json::array({"/usr/bin/polaris-seat-worker"})}, {"Cmd", json::array({"run"})},
        {"Volumes", nullptr}, {"ExposedPorts", nullptr}, {"OnBuild", nullptr},
        {"Labels", {{"org.opencontainers.image.source", "https://github.com/papi-ux/polaris"},
          {"org.opencontainers.image.revision", r.source_revision}, {"io.polaris.multiseat.profile", "steam"},
          {"io.polaris.multiseat.architecture", "linux/amd64"}, {"io.polaris.multiseat.media-contract", "1"},
          {"io.polaris.multiseat.nvidia.driver", r.nvidia_driver}}}}}}});
  }
  class download_host_t : public container::host_t {
  public:
    bool trusted = true;
    std::vector<std::vector<std::string>> calls;
    std::deque<container::command_result_t> replies;
    std::uint64_t effective_uid() const override { return 1000; }
    bool executable_file(const std::filesystem::path &) const override { return true; }
    bool trusted_runtime_file(const std::filesystem::path &) const override { return trusted; }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return {}; }
    bool readable_directory(const std::filesystem::path &) const override { return false; }
    bool private_read_write_directory(const std::filesystem::path &) const override { return false; }
    bool private_readable_file(const std::filesystem::path &) const override { return false; }
    std::optional<container::character_device_identity_t> read_write_character_device(const std::filesystem::path &) const override {
      ADD_FAILURE() << "Runtime download cannot access input or graphics devices";
      return {};
    }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &, std::size_t) const override { return {}; }
    container::command_result_t run(const std::vector<std::string> &args, std::chrono::milliseconds timeout, std::size_t bytes) override {
      calls.push_back(args);
      const auto prefix = container::command_prefix({});
      if (args.size() <= prefix.size()) { ADD_FAILURE() << "Invalid Docker command"; return {.exit_status = 1}; }
      EXPECT_EQ(std::vector<std::string>(args.begin(), args.begin() + prefix.size()), prefix);
      EXPECT_EQ(bytes, 65536U);
      const auto verb = args.at(prefix.size());
      if (verb == "info") EXPECT_EQ(timeout, std::chrono::seconds(3));
      else {
        EXPECT_EQ(verb, "image");
        const auto operation = args.at(prefix.size() + 1);
        EXPECT_TRUE(operation == "inspect" || operation == "pull");
        EXPECT_EQ(timeout, operation == "pull" ? std::chrono::seconds(1800) : std::chrono::seconds(5));
      }
      if (replies.empty()) { ADD_FAILURE() << "Unexpected Docker command"; return {.exit_status = 1}; }
      auto result = replies.front(); replies.pop_front(); return result;
    }
  };
  container::command_result_t ok(std::string output) { return {.exit_status = 0, .output = std::move(output)}; }
  auto engine() { return ok(R"({"OSType":"linux","SecurityOptions":["name=selinux"],"Runtimes":{"runc":{"path":"runc"}}})"); }
}

TEST(SpacesRuntime, CatalogIsBoundedAndRejectsIncompatibleOrAmbiguousEntries) {
  ASSERT_TRUE(spaces::trusted_runtimes());
  EXPECT_TRUE(spaces::decode_runtime_catalog(catalog(json::array())));
  for (const auto &[key, value] : std::vector<std::pair<std::string, json>> {
      {"profile", "heroic"}, {"platform", "linux/arm64"}, {"media_contract", 2}, {"media_contract", true},
      {"uid", 1001}, {"gid", 0}, {"id", "--all"}, {"id", "../steam"}, {"variant", "other"},
      {"source_revision", "main"}, {"registry_digest", "latest"}, {"config_digest", "sha256:no"},
      {"nvidia_driver", "610.57.04"}, {"url", "https://untrusted.invalid/image"},
      {"nvidia_minimum_driver", "570.00"}}) {
    auto e = entry(); e[key] = value;
    EXPECT_FALSE(spaces::decode_runtime_catalog(catalog(json::array({e})))) << key;
  }
  auto duplicate = catalog(json::array({entry(), entry()}));
  EXPECT_FALSE(spaces::decode_runtime_catalog(duplicate));
  auto text = catalog(json::array({entry()}));
  text.insert(1, "\"schema\":2,");
  EXPECT_FALSE(spaces::decode_runtime_catalog(text));
  EXPECT_FALSE(spaces::decode_runtime_catalog(std::string(65537, ' ')));
  EXPECT_FALSE(spaces::decode_runtime_catalog(R"({"schema":2.0,"runtimes":[]})"));
  EXPECT_FALSE(spaces::decode_runtime_catalog(R"({"schema":1,"runtimes":[]})")) << "the old shape is not accepted";
  auto nvidia = entry(); nvidia["variant"] = "nvidia"; nvidia["nvidia_driver"] = "610.57.04";
  ASSERT_TRUE(spaces::decode_runtime_catalog(catalog(json::array({nvidia}))));
  // A host-driver runtime carries no driver of its own and names its floor.
  auto host_driver = entry();
  host_driver["variant"] = "nvidia-host";
  host_driver["nvidia_minimum_driver"] = "570.00";
  ASSERT_TRUE(spaces::decode_runtime_catalog(catalog(json::array({host_driver}))));
  for (const auto &[key, value] : std::vector<std::pair<std::string, json>> {
      {"nvidia_driver", "615.71.09"}, {"nvidia_minimum_driver", ""}, {"nvidia_minimum_driver", "570"}}) {
    auto broken = host_driver; broken[key] = value;
    EXPECT_FALSE(spaces::decode_runtime_catalog(catalog(json::array({broken})))) << key;
  }
}

TEST(SpacesRuntime, UnknownOrUntrustedDownloadsCannotReachDocker) {
  download_host_t host;
  EXPECT_EQ(spaces::install_runtime(host, "steam-test", {}).code, "runtime_not_published");
  EXPECT_EQ(spaces::install_runtime(host, "ghcr.io/other/image:latest", {runtime()}).code, "runtime_not_published");
  host.trusted = false;
  EXPECT_EQ(spaces::install_runtime(host, "steam-test", {runtime()}).code, "docker_unavailable");
  EXPECT_TRUE(host.calls.empty());
}

TEST(SpacesRuntime, EngineFailuresAndRootlessRepliesCannotStartADownload) {
  const auto r = runtime();
  for (auto reply : std::vector<container::command_result_t> {
      {.exit_status = 1}, {.exit_status = 0, .output = "not JSON"},
      ok(R"({"OSType":"windows","SecurityOptions":[],"Runtimes":{"runc":{"path":"runc"}}})"),
      ok(R"({"OSType":"linux","SecurityOptions":["name=rootless"],"Runtimes":{"runc":{"path":"runc"}}})"),
      ok(R"({"OSType":"linux","SecurityOptions":[],"Runtimes":{"runc":{"path":"/tmp/runc"}}})")}) {
    download_host_t host; host.replies = {reply};
    EXPECT_EQ(spaces::install_runtime(host, r.id, {r}).code, "docker_unavailable");
    EXPECT_EQ(host.calls.size(), 1U);
  }
  for (int failure = 0; failure < 2; ++failure) {
    auto reply = engine(); reply.timed_out = failure == 0; reply.output_truncated = failure == 1;
    download_host_t host; host.replies = {reply};
    EXPECT_EQ(spaces::install_runtime(host, r.id, {r}).code, "docker_unavailable");
    EXPECT_EQ(host.calls.size(), 1U);
  }
}

TEST(SpacesRuntime, RechecksInstalledIdentityAndDoesNotPullOrStartAnExistingImage) {
  const auto r = runtime();
  download_host_t host; host.replies = {engine(), ok(inspected(r).dump())};
  const auto result = spaces::install_runtime(host, r.id, {r});
  EXPECT_TRUE(result.ready);
  EXPECT_EQ(result.image, r.config_digest);
  EXPECT_EQ(host.calls.size(), 2U);
}

TEST(SpacesRuntime, ContainerdUsesTheApprovedManifestAsItsLocalImageIdentity) {
  const auto r = runtime();
  auto image = inspected(r);
  image[0]["Id"] = r.registry_digest;
  image[0]["Descriptor"] = {{"digest", r.registry_digest}, {"size", 7834},
    {"mediaType", "application/vnd.oci.image.manifest.v1+json"}};
  download_host_t host; host.replies = {engine(), ok(image.dump())};
  const auto result = spaces::install_runtime(host, r.id, {r});
  ASSERT_TRUE(result.ready);
  EXPECT_EQ(result.image, r.registry_digest);
  EXPECT_EQ(host.calls.size(), 2U);
  host.replies = {engine(), {.exit_status = 1}, ok("downloaded"), ok(image.dump())};
  const auto downloaded = spaces::install_runtime(host, r.id, {r});
  ASSERT_TRUE(downloaded.ready);
  EXPECT_EQ(downloaded.image, r.registry_digest);

  for (const auto &[path, value] : std::vector<std::pair<std::string, json>> {
      {"/0/Descriptor", nullptr}, {"/0/Descriptor/digest", r.config_digest},
      {"/0/Descriptor/size", 0}, {"/0/Descriptor/size", 65537},
      {"/0/Descriptor/mediaType", "application/vnd.oci.image.index.v1+json"},
      {"/0/Id", "sha256:" + std::string(64, 'd')}, {"/0/RepoDigests", json::array()},
      {"/0/Config/Labels/org.opencontainers.image.revision", std::string(40, 'e')}}) {
    auto bad = image; bad[json::json_pointer(path)] = value;
    EXPECT_FALSE(spaces::matches_runtime_image(r, bad.dump())) << path;
  }
}

TEST(SpacesRuntime, PullUsesOnlyTheApprovedRegistryDigestThenChecksTheInstalledImage) {
  const auto r = runtime();
  download_host_t host;
  host.replies = {engine(), {.exit_status = 1}, ok("downloaded"), ok(inspected(r).dump())};
  ASSERT_TRUE(spaces::install_runtime(host, r.id, {r}).ready);
  ASSERT_EQ(host.calls.size(), 4U);
  auto expected = container::command_prefix({});
  expected.insert(expected.end(), {"image", "pull", "--quiet", "--platform=linux/amd64", r.reference()});
  EXPECT_EQ(host.calls[2], expected);
  EXPECT_EQ(host.calls[1], host.calls[3]);
}

TEST(SpacesRuntime, FailedOrInterruptedDownloadsCanBeRetriedWithoutCreatingResources) {
  const auto r = runtime();
  for (int failure = 0; failure < 3; ++failure) {
    download_host_t host;
    container::command_result_t failed {.exit_status = failure == 0 ? 1 : 0, .output = "private error"};
    failed.timed_out = failure == 1; failed.output_truncated = failure == 2;
    host.replies = {engine(), {.exit_status = 1}, failed};
    auto result = spaces::install_runtime(host, r.id, {r});
    EXPECT_FALSE(result.ready);
    EXPECT_EQ(result.code, "download_incomplete");
    EXPECT_TRUE(result.image.empty());
    EXPECT_EQ(result.message.find("private error"), std::string::npos);
    host.replies = {engine(), ok(inspected(r).dump())};
    EXPECT_TRUE(spaces::install_runtime(host, r.id, {r}).ready);
    EXPECT_EQ(host.calls.size(), 5U);
  }
}

TEST(SpacesRuntime, SelfConsistentButUnapprovedImagesCannotBeUsed) {
  const auto r = runtime();
  for (const auto &[path, value] : std::vector<std::pair<std::string, json>> {
      {"/0/Id", "sha256:" + std::string(64, 'd')}, {"/0/Architecture", "arm64"},
      {"/0/RepoDigests", json::array()}, {"/0/Config/Entrypoint", json::array({"/bin/sh"})},
      {"/0/Config/Cmd", json::array({"other"})}, {"/0/Config/Volumes", {{"/shared", json::object()}}},
      {"/0/Config/ExposedPorts", {{"80/tcp", json::object()}}}, {"/0/Config/OnBuild", json::array({"RUN touch /oops"})},
      {"/0/Config/Labels/org.opencontainers.image.revision", std::string(40, 'e')},
      {"/0/Config/Labels/io.polaris.multiseat.media-contract", "2"},
      {"/0/Config/Labels/io.polaris.multiseat.nvidia.driver", "610.57.04"}}) {
    auto image = inspected(r); image[json::json_pointer(path)] = value;
    EXPECT_FALSE(spaces::matches_runtime_image(r, image.dump())) << path;
    download_host_t host; host.replies = {engine(), ok(image.dump())};
    EXPECT_EQ(spaces::install_runtime(host, r.id, {r}).code, "runtime_identity_mismatch");
    EXPECT_EQ(host.calls.size(), 2U);
    host.replies = {engine(), {.exit_status = 1}, ok("downloaded"), ok(image.dump())};
    EXPECT_EQ(spaces::install_runtime(host, r.id, {r}).code, "runtime_verification_failed");
  }
}

TEST(SpacesRuntime, CancelledDownloadDoesNotReachDocker) {
  download_host_t host;
  std::stop_source stop;
  stop.request_stop();
  const auto result = spaces::install_runtime(host, runtime().id, {runtime()}, stop.get_token());
  EXPECT_EQ(result.code, "download_cancelled");
  EXPECT_FALSE(result.ready);
  EXPECT_TRUE(host.calls.empty());
}

TEST(SpacesRuntime, ReferencePullsFromTheCompiledRepository) {
  const auto r = runtime();
  EXPECT_EQ(r.reference(), std::string {multiseat::spaces::runtime_repository} + "@" + r.registry_digest);
}

TEST(SpacesRuntime, ReleaseBuildsNeverOverrideTheRuntimeRepositoryOrCatalog) {
  // The override exists for lab builds only. A workflow or packaging script that sets
  // it would ship a host that trusts runtimes from somewhere else.
  const std::filesystem::path source {POLARIS_SOURCE_DIR};
  const auto read = [](const std::filesystem::path &path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  EXPECT_NE(read(source / "cmake/compile_definitions/linux.cmake")
              .find("set(POLARIS_SPACES_RUNTIME_REPOSITORY \"ghcr.io/papi-ux/polaris-worker-steam\" CACHE STRING"),
            std::string::npos);
  EXPECT_EQ(multiseat::spaces::release_runtime_repository, "ghcr.io/papi-ux/polaris-worker-steam");
  for (const auto *directory : {".github/workflows", "scripts/ci", "packaging"}) {
    if (!std::filesystem::exists(source / directory)) continue;
    for (const auto &file : std::filesystem::recursive_directory_iterator(source / directory)) {
      if (!file.is_regular_file()) continue;
      const auto text = read(file.path());
      EXPECT_EQ(text.find("POLARIS_SPACES_RUNTIME_REPOSITORY"), std::string::npos) << file.path();
      EXPECT_EQ(text.find("POLARIS_SPACES_RUNTIME_CATALOG_FILE"), std::string::npos) << file.path();
    }
  }
}
#endif
