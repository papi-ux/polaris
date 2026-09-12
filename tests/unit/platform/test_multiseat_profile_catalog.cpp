#include "src/platform/linux/multiseat_profile_catalog.h"
#include "src/platform/linux/multiseat_profile_network.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  using namespace multiseat;
  using json = nlohmann::json;
  namespace psf = private_state_file;

  class MultiseatProfileCatalog : public ::testing::Test {
  protected:
    std::filesystem::path root, path;
    void SetUp() override {
      std::array<char, 64> pattern {};
      const std::string value = "/tmp/polaris-profile-catalog-XXXXXX";
      std::copy(value.begin(), value.end(), pattern.begin());
      const auto *created = ::mkdtemp(pattern.data());
      ASSERT_NE(created, nullptr);
      root = created;
      path = root / "profiles.json";
    }
    void TearDown() override {
      psf::set_write_fault_for_tests(psf::write_fault_e::none);
      std::filesystem::remove_all(root);
    }
    profiles::catalog_t sample() {
      return {1000, 1000, {{
        .storage = {"profile-a", "pv-profile-a", runtime_profile_e::gamescope, "sha256:" + std::string(64, 'a')},
        .name = "Living room", .workload = {workload_kind_e::gamescope, "input-pong-v1"},
        .client_keys = {"client-a"},
      }}};
    }
    void save(const profiles::catalog_t &catalog) { ASSERT_TRUE(psf::write_atomic(path, profiles::encode(catalog))); }
  };

  class provisioning_host_t : public container::host_t {
  public:
    std::vector<std::vector<std::string>> calls;
    std::size_t fail_call = 0;
    bool timeout = false, truncated = false, wrong_label = false, rootless = false, implicit_volume = false;
    std::uint64_t uid = 1000;
    std::string volume, profile, image;
    std::string image_family = "gamescope";
    bool wrong_network = false;
    std::uint64_t effective_uid() const override { return uid; }
    bool executable_file(const std::filesystem::path &) const override { return true; }
    bool trusted_runtime_file(const std::filesystem::path &) const override { return true; }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return std::vector<std::uint64_t> {}; }
    bool readable_directory(const std::filesystem::path &) const override { return true; }
    bool private_read_write_directory(const std::filesystem::path &) const override { return true; }
    bool private_readable_file(const std::filesystem::path &) const override { return true; }
    std::optional<container::character_device_identity_t> read_write_character_device(const std::filesystem::path &) const override { return std::nullopt; }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &, std::size_t) const override { return std::nullopt; }
    container::command_result_t run(const std::vector<std::string> &argv,
                                    std::chrono::milliseconds duration, std::size_t bound) override {
      calls.push_back(argv);
      EXPECT_EQ(duration, std::chrono::seconds(30));
      EXPECT_EQ(bound, profiles::maximum_catalog_bytes);
      const auto prefix = container::command_prefix({});
      EXPECT_TRUE(std::equal(prefix.begin(), prefix.end(), argv.begin()));
      if (calls.size() == fail_call) return {.exit_status = timeout || truncated ? 0 : 1, .timed_out = timeout, .output_truncated = truncated};
      const std::vector<std::string> args(argv.begin() + prefix.size(), argv.end());
      json result;
      if (args[0] == "info") {
        result = {{"OSType", "linux"}, {"Runtimes", {{"runc", {{"path", "runc"}}}}},
                  {"SecurityOptions", rootless ? json::array({"name=rootless"}) : json::array({"name=selinux"})}};
      } else if (args[0] == "image") {
        image = args.back();
        result = json::array({{{"Id", image}, {"Os", "linux"},
          {"Config", {{"Labels", {{"io.polaris.multiseat.profile", image_family}}},
            {"Volumes", implicit_volume ? json {{"/extra", json::object()}} : json(nullptr)}}}}});
      } else if (args[0] == "volume" && args[1] == "create") {
        volume = args.back();
        profile = volume.substr(3);
        return {.exit_status = 0, .output = volume};
      } else if (args[0] == "volume" && args[1] == "inspect") {
        result = json::array({{{"Name", volume}, {"Driver", "local"}, {"Scope", "local"},
          {"Options", nullptr}, {"Labels", {{"io.polaris.multiseat.profile", wrong_label ? "someone-else" : profile}}}}});
      } else if (args[0] == "volume" && args[1] == "ls") {
        return {.exit_status = 0, .output = "\"unrelated-volume\"\n"};
      } else if (args[0] == "network" && args[1] == "ls") {
        return {.exit_status = 0, .output = "\"bridge\"\n\"none\"\n"};
      } else if (args[0] == "network" && args[1] == "create") {
        EXPECT_EQ(args.back(), container::profile_network_name(profile));
        return {.exit_status = 0, .output = std::string(64, 'e') + "\n"};
      } else if (args[0] == "network" && args[1] == "inspect") {
        result = json::array({{{"Id", std::string(64, wrong_network ? 'f' : 'e')}, {"Name", container::profile_network_name(profile)},
          {"Driver", "bridge"}, {"Scope", "local"}, {"Internal", false}, {"Ingress", false}, {"Attachable", false},
          {"EnableIPv6", false}, {"Labels", {{"io.polaris.multiseat.profile", profile}}},
          {"Options", {{"com.docker.network.bridge.enable_icc", "false"}, {"com.docker.network.bridge.enable_ip_masquerade", "true"}}},
          {"IPAM", {{"Driver", "default"}, {"Options", nullptr}}}, {"Containers", json::object()}}});
      } else if (args[0] == "run") {
        for (const auto *required : {"--network=none", "--userns=host", "--read-only", "--cap-drop=ALL",
              "--cap-add=CHOWN", "--cap-add=FOWNER", "--security-opt=no-new-privileges", "--pull=never",
              "--runtime=runc", "--user=0:0", "--entrypoint=/usr/bin/python3", "-I"}) {
          EXPECT_NE(std::find(args.begin(), args.end(), required), args.end());
        }
        EXPECT_NE(std::find(args.begin(), args.end(), "--mount=type=volume,src=" + volume + ",dst=/profile,volume-nocopy"), args.end());
        EXPECT_EQ(std::count_if(args.begin(), args.end(), [](const auto &arg) { return arg.starts_with("--mount="); }), 1);
        for (const auto &arg : args) {
          EXPECT_FALSE(arg.starts_with("--device"));
          EXPECT_FALSE(arg.starts_with("--privileged"));
          EXPECT_FALSE(arg.starts_with("--gpus"));
        }
        return {.exit_status = 0};
      } else { ADD_FAILURE() << "Unexpected Docker operation"; return {}; }
      return {.exit_status = 0, .output = result.dump()};
    }
  };

  TEST_F(MultiseatProfileCatalog, AtomicAssignmentMovesPreserveOtherDevicesAndRejectUnknownTargets) {
    auto catalog = sample();
    auto other = catalog.profiles.front();
    other.storage.profile_key = "profile-b";
    other.storage.opaque_volume_name = "pv-profile-b";
    other.client_keys = {"client-b"};
    catalog.profiles.push_back(other);
    save(catalog);
    EXPECT_FALSE(profiles::set_assignment(path, "missing", "client-a"));
    ASSERT_TRUE(profiles::set_assignment(path, "profile-b", "client-a"));
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.profiles[0].client_keys.empty());
    EXPECT_EQ(loaded->catalog.profiles[1].client_keys, (std::vector<std::string>{"client-b", "client-a"}));
    EXPECT_FALSE(profiles::set_assignment(path, "", "client-a"));
    loaded.reset();
    ASSERT_TRUE(profiles::set_assignment(path, "", "client-a"));
    loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->catalog.profiles[1].client_keys, std::vector<std::string>{"client-b"});
  }

  TEST_F(MultiseatProfileCatalog, AssignmentWriteFailuresReportTheActualCommitBoundary) {
    save(sample());
    psf::set_write_fault_for_tests(psf::write_fault_e::rename);
    EXPECT_EQ(profiles::set_assignment(path, "", "client-a").status, psf::write_status_e::not_committed);
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->catalog.profiles[0].client_keys, std::vector<std::string>{"client-a"});
    loaded.reset();
    psf::set_write_fault_for_tests(psf::write_fault_e::post_rename_durability);
    EXPECT_EQ(profiles::set_assignment(path, "", "client-a").status, psf::write_status_e::durability_uncertain);
    loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.profiles[0].client_keys.empty());
  }

  TEST_F(MultiseatProfileCatalog, RoundTripPreservesPrivateAssignmentsAndAllTypedFamilies) {
    auto catalog = sample();
    const std::array families {runtime_profile_e::steam, runtime_profile_e::heroic, runtime_profile_e::lutris};
    const std::array kinds {workload_kind_e::steam, workload_kind_e::heroic, workload_kind_e::lutris};
    for (std::size_t index = 0; index < families.size(); ++index) {
      auto entry = catalog.profiles.front();
      entry.storage.profile_key += std::to_string(index);
      entry.storage.opaque_volume_name += std::to_string(index);
      entry.storage.runtime_profile = families[index];
      entry.workload.kind = kinds[index];
      entry.client_keys.clear();
      catalog.profiles.push_back(entry);
    }
    auto decoded = profiles::decode(profiles::encode(catalog));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(profiles::encode(*decoded), profiles::encode(catalog));
  }

  TEST_F(MultiseatProfileCatalog, RejectsAmbiguousOrUnboundedAuthority) {
    const auto base = json::parse(profiles::encode(sample()));
    const std::vector<std::function<void(json &)>> changes {
      [](auto &v) { v["schema"] = 2; }, [](auto &v) { v["schema"] = 1.0; },
      [](auto &v) { v["owner_uid"] = -1; }, [](auto &v) { v["owner_gid"] = 0; },
      [](auto &v) { v["owner_uid"] = 0x1000003e8ULL; },
      [](auto &v) { v["command"] = "/bin/sh"; },
      [](auto &v) { v["profiles"][0]["image"] = "polaris:latest"; },
      [](auto &v) { v["profiles"][0]["volume"] = "/profile/data"; },
      [](auto &v) { v["profiles"][0]["family"] = "unknown"; },
      [](auto &v) { v["profiles"][0]["target"] = "$(command)"; },
      [](auto &v) { v["profiles"][0]["name"] = "name\nforged log"; },
      [](auto &v) { v["profiles"][0]["clients"] = json::array({"--device", "client-a"}); },
      [](auto &v) { v["profiles"].push_back(v["profiles"][0]); },
      [](auto &v) { auto entry = v["profiles"][0]; entry["id"] = "other"; entry["volume"] = "pv-other"; v["profiles"].push_back(entry); },
      [](auto &v) { v["profiles"][0]["clients"].push_back("client-a"); },
    };
    for (std::size_t index = 0; index < changes.size(); ++index) {
      SCOPED_TRACE(index);
      auto value = base;
      changes[index](value);
      EXPECT_FALSE(profiles::decode(value.dump()));
    }
    auto duplicate = base.dump();
    duplicate.insert(1, "\"schema\":1,");
    EXPECT_FALSE(profiles::decode(duplicate));
    EXPECT_FALSE(profiles::decode(std::string(100, '[') + std::string(100, ']')));
    EXPECT_FALSE(profiles::decode(std::string(profiles::maximum_catalog_bytes + 1, ' ')));
  }

  TEST_F(MultiseatProfileCatalog, InitializesOnceAndRejectsUnsafePathsWithoutReplacingData) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    EXPECT_FALSE(profiles::initialize(path, 1000, 1000));
    struct stat metadata {};
    ASSERT_EQ(::stat(path.c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_mode & 0777, 0600);
    const auto link = root / "link.json";
    std::filesystem::create_symlink(path, link);
    EXPECT_FALSE(profiles::load(link));
    EXPECT_FALSE(profiles::initialize(link, 1000, 1000));
    std::filesystem::create_hard_link(path, root / "hard.json");
    EXPECT_FALSE(profiles::load(path));
    std::filesystem::remove(root / "hard.json");
    ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
    EXPECT_FALSE(profiles::load(path));
    EXPECT_FALSE(profiles::assign(path, "profile-a", "client-a"));
  }

  TEST_F(MultiseatProfileCatalog, LeaseBlocksCrossProcessEditsUntilLastOwnerReleases) {
    save(sample());
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    auto retained = loaded->lease;
    loaded.reset();
    const auto child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) ::_exit(profiles::assign(path, "profile-a", "client-b") ? 1 : 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_FALSE(profiles::load(path));
    retained.reset();
    ASSERT_TRUE(profiles::assign(path, "profile-a", "client-b"));
    auto updated = profiles::load(path);
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->catalog.profiles[0].client_keys.size(), 2);
  }

  TEST_F(MultiseatProfileCatalog, FailedLoadsReleaseLeaseAndAssignmentsRequireExplicitRemoval) {
    ASSERT_TRUE(psf::write_atomic(path, "{}"));
    EXPECT_FALSE(profiles::load(path));
    auto catalog = sample();
    auto second = catalog.profiles.front();
    second.storage.profile_key = "profile-b";
    second.storage.opaque_volume_name = "pv-profile-b";
    second.client_keys.clear();
    catalog.profiles.push_back(second);
    save(catalog);
    EXPECT_FALSE(profiles::assign(path, "profile-b", "client-a"));
    EXPECT_FALSE(profiles::assign(path, "missing", "new-device"));
    ASSERT_TRUE(profiles::unassign(path, "client-a"));
    ASSERT_TRUE(profiles::assign(path, "profile-b", "client-a"));
    ASSERT_TRUE(profiles::assign(path, "profile-b", "client-a"));
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.profiles[0].client_keys.empty());
    EXPECT_EQ(loaded->catalog.profiles[1].client_keys, std::vector<std::string> {"client-a"});
  }

  TEST_F(MultiseatProfileCatalog, CreatesFreshOwnedStorageBeforePublishingUnassignedProfile) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    provisioning_host_t host;
    auto result = profiles::create(path, "Private games", sample().profiles[0].storage.image_reference, host);
    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(host.calls.size(), 7);
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->catalog.profiles.size(), 1);
    EXPECT_EQ(loaded->catalog.profiles[0].storage.opaque_volume_name, result.volume_name);
    EXPECT_TRUE(loaded->catalog.profiles[0].client_keys.empty());
  }

  TEST_F(MultiseatProfileCatalog, SteamProfilePublishesOnlyAfterStorageAndNetworkAreVerified) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    provisioning_host_t host; host.image_family = "steam";
    const auto result = profiles::create(path, "Private Steam", sample().profiles[0].storage.image_reference,
      host, {workload_kind_e::steam, "big-picture-v1"});
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(host.calls.size(), 10U);
    EXPECT_EQ(result.network_name, container::profile_network_name(result.profile_key));
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->catalog.profiles.size(), 1U);
    EXPECT_EQ(loaded->catalog.profiles[0].storage.runtime_profile, runtime_profile_e::steam);
    EXPECT_EQ(loaded->catalog.profiles[0].workload.target_id, "big-picture-v1");
    EXPECT_TRUE(loaded->catalog.profiles[0].client_keys.empty());
  }

  TEST_F(MultiseatProfileCatalog, SteamNetworkFailureRetainsResourcesWithoutPublishingProfile) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    for (unsigned failure = 8; failure <= 11; ++failure) {
      provisioning_host_t host; host.image_family = "steam";
      host.fail_call = failure; host.wrong_network = failure == 11;
      const auto result = profiles::create(path, "Steam", sample().profiles[0].storage.image_reference,
        host, {workload_kind_e::steam, "570"});
      EXPECT_FALSE(result);
      EXPECT_FALSE(result.volume_name.empty());
      EXPECT_FALSE(result.network_name.empty());
      auto loaded = profiles::load(path);
      ASSERT_TRUE(loaded);
      EXPECT_TRUE(loaded->catalog.profiles.empty());
      for (const auto &call : host.calls) EXPECT_EQ(std::find(call.begin(), call.end(), "rm"), call.end());
    }
  }

  TEST_F(MultiseatProfileCatalog, InvalidSteamTargetFailsBeforeAnyDockerOperation) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    provisioning_host_t host; host.image_family = "steam";
    EXPECT_FALSE(profiles::create(path, "Steam", sample().profiles[0].storage.image_reference,
      host, {workload_kind_e::steam, "570 --other"}));
    EXPECT_TRUE(host.calls.empty());
  }

  TEST_F(MultiseatProfileCatalog, EveryDockerFailureLeavesCatalogUnchangedAndNeverDeletesResources) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    for (std::size_t fail = 1; fail <= 7; ++fail) {
      for (int mode = 0; mode < 3; ++mode) {
        SCOPED_TRACE(std::to_string(fail) + ":" + std::to_string(mode));
        provisioning_host_t host;
        host.fail_call = fail;
        host.timeout = mode == 1;
        host.truncated = mode == 2;
        auto result = profiles::create(path, "Private games", sample().profiles[0].storage.image_reference, host);
        EXPECT_FALSE(result);
        EXPECT_EQ(host.calls.size(), fail);
        if (fail >= 4) { EXPECT_FALSE(result.volume_name.empty()); }
        auto loaded = profiles::load(path);
        ASSERT_TRUE(loaded);
        EXPECT_TRUE(loaded->catalog.profiles.empty());
        for (const auto &call : host.calls) EXPECT_EQ(std::find(call.begin(), call.end(), "rm"), call.end());
      }
    }
  }

  TEST_F(MultiseatProfileCatalog, RejectsWrongEngineOwnershipImageAndCatalogBeforeInitialization) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    for (int mode = 0; mode < 5; ++mode) {
      provisioning_host_t host;
      host.uid = mode == 0 ? 1001 : 1000;
      host.rootless = mode == 1;
      host.implicit_volume = mode == 2;
      host.wrong_label = mode == 3;
      auto result = profiles::create(path, "Games", mode == 4 ? "mutable:latest" : sample().profiles[0].storage.image_reference, host);
      EXPECT_FALSE(result);
      for (const auto &call : host.calls) EXPECT_EQ(std::find(call.begin(), call.end(), "run"), call.end());
    }
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    provisioning_host_t host;
    EXPECT_FALSE(profiles::create(path, "Games", sample().profiles[0].storage.image_reference, host));
    EXPECT_TRUE(host.calls.empty());
  }

  TEST_F(MultiseatProfileCatalog, UncertainCommitPreservesVolumeAndReportsVisibleReplacement) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    psf::set_write_fault_for_tests(psf::write_fault_e::post_rename_durability);
    provisioning_host_t host;
    auto result = profiles::create(path, "Games", sample().profiles[0].storage.image_reference, host);
    EXPECT_EQ(result.status, psf::write_status_e::durability_uncertain);
    EXPECT_FALSE(result.volume_name.empty());
    EXPECT_NE(result.error.find("Read it back"), std::string::npos);
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->catalog.profiles.size(), 1);
    EXPECT_EQ(loaded->catalog.profiles[0].storage.opaque_volume_name, result.volume_name);
  }

  TEST_F(MultiseatProfileCatalog, FailedCatalogRenameRetainsFreshVolumeWithoutPublishingProfile) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    psf::set_write_fault_for_tests(psf::write_fault_e::rename);
    provisioning_host_t host;
    auto result = profiles::create(path, "Games", sample().profiles[0].storage.image_reference, host);
    EXPECT_EQ(result.status, psf::write_status_e::not_committed);
    EXPECT_FALSE(result.volume_name.empty());
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.profiles.empty());
    EXPECT_EQ(host.calls.size(), 7);
  }
}  // namespace
