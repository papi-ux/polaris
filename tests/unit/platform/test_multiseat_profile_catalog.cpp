#include "src/platform/linux/multiseat_profile_catalog.h"
#include "src/platform/linux/spaces_library.h"
#include "src/platform/linux/multiseat_container_host.h"
#include "src/platform/linux/multiseat_profile_network.h"
#include "src/utility.h"
#include "src/uuid.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
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
    bool library_mode = false;
    bool timeout = false, truncated = false, wrong_label = false, rootless = false, implicit_volume = false;
    std::uint64_t uid = 1000;
    std::string volume, profile, image;
    std::string image_family = "gamescope";
    bool wrong_network = false;
    bool retain_volume_in_inventory = false;
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
      EXPECT_EQ(duration, std::chrono::seconds(library_mode ? 15 : 30));
      EXPECT_EQ(bound, library_mode ? 2 * 1024 * 1024 : profiles::maximum_catalog_bytes);
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
        if (retain_volume_in_inventory && !volume.empty()) return {.exit_status = 0, .output = json(volume).dump() + "\n"};
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
      } else if (args[0] == "run" && library_mode) {
        for (const auto *required : {"--network=none", "--userns=host", "--read-only", "--cap-drop=ALL",
              "--security-opt=no-new-privileges", "--pull=never", "--runtime=runc", "--user=1000:1000"})
          EXPECT_NE(std::find(args.begin(), args.end(), required), args.end());
        EXPECT_NE(std::find(args.begin(), args.end(), "--mount=type=volume,src=" + volume + ",dst=/profile,readonly,volume-nocopy"), args.end());
        EXPECT_EQ(args.back(), spaces::steam_library_scanner());
        for (const auto &arg : args) {
          EXPECT_FALSE(arg.starts_with("--device") || arg.starts_with("--privileged") ||
            arg.starts_with("--gpus") || arg.starts_with("--cap-add") || arg == "--security-opt=label=disable");
        }
        return {.exit_status = 0, .output = R"({"schema":1,"games":[{"target":"870780","name":"Control"}]})"};
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

  const profiles::space_create_request_t steam_request {
    "12345678-1234-4234-8234-123456789abc", "profile-a", "Second player"
  };

  const profiles::first_space_request_t first_request {steam_request.request_id, "First player"};
  const std::string first_image = "sha256:" + std::string(64, 'b');

  TEST_F(MultiseatProfileCatalog, RemoveAndRestorePreserveTheHomeButNeverRestoreDeviceAccess) {
    save(sample());
    const profiles::edit_request_t remove {profiles::edit_operation_e::remove, "profile-a", ""};
    ASSERT_TRUE(profiles::edit(path, remove));
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->catalog.profiles.size(), 1U);
    const auto entry = loaded->catalog.profiles.front();
    EXPECT_TRUE(entry.archived);
    EXPECT_TRUE(entry.client_keys.empty());
    EXPECT_EQ(entry.storage.opaque_volume_name, sample().profiles[0].storage.opaque_volume_name);
    EXPECT_EQ(entry.storage.image_reference, sample().profiles[0].storage.image_reference);
    EXPECT_EQ(entry.workload, sample().profiles[0].workload);
    EXPECT_EQ(entry.name, "Living room");
    EXPECT_EQ(json::parse(profiles::encode(loaded->catalog))["schema"], 2);
    EXPECT_FALSE(profiles::edit(path, {profiles::edit_operation_e::restore, "profile-a", ""})); // live lease
    loaded.reset();
    EXPECT_FALSE(profiles::assign(path, "profile-a", "client-b"));
    EXPECT_FALSE(profiles::set_assignment(path, "profile-a", "client-b"));
    ASSERT_TRUE(profiles::edit(path, remove)); // safe retry
    ASSERT_TRUE(profiles::edit(path, {profiles::edit_operation_e::restore, "profile-a", ""}));
    loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded->catalog.profiles[0].archived);
    EXPECT_TRUE(loaded->catalog.profiles[0].client_keys.empty());
    EXPECT_EQ(json::parse(profiles::encode(loaded->catalog))["schema"], 1);
  }

  TEST_F(MultiseatProfileCatalog, RestoreRetriesPreserveLaterExplicitAssignments) {
    save(sample());
    ASSERT_TRUE(profiles::edit(path, {profiles::edit_operation_e::remove, "profile-a", ""}));
    const profiles::edit_request_t restore {profiles::edit_operation_e::restore, "profile-a", ""};
    ASSERT_TRUE(profiles::edit(path, restore));
    // A Default Space is only ever a Space the device may already open.
    ASSERT_TRUE(profiles::set_access(path, "profile-a", "client-b", true));
    ASSERT_TRUE(profiles::set_assignment(path, "profile-a", "client-b"));
    ASSERT_TRUE(profiles::edit(path, restore));
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded->catalog.profiles[0].archived);
    EXPECT_EQ(loaded->catalog.profiles[0].client_keys, std::vector<std::string>{"client-b"});
  }

  TEST_F(MultiseatProfileCatalog, RenamePreservesAssignmentsAndRemoveRespectsCommitFailures) {
    save(sample());
    ASSERT_TRUE(profiles::edit(path, {profiles::edit_operation_e::rename, "profile-a", "Living room Steam"}));
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->catalog.profiles[0].name, "Living room Steam");
    EXPECT_EQ(loaded->catalog.profiles[0].client_keys, sample().profiles[0].client_keys);
    loaded.reset();
    psf::set_write_fault_for_tests(psf::write_fault_e::rename);
    EXPECT_EQ(profiles::edit(path, {profiles::edit_operation_e::remove, "profile-a", ""}).status, psf::write_status_e::not_committed);
    loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_FALSE(loaded->catalog.profiles[0].archived);
    loaded.reset();
    psf::set_write_fault_for_tests(psf::write_fault_e::post_rename_durability);
    EXPECT_EQ(profiles::edit(path, {profiles::edit_operation_e::remove, "profile-a", ""}).status, psf::write_status_e::durability_uncertain);
    loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.profiles[0].archived);
  }

  TEST_F(MultiseatProfileCatalog, ArchivedCatalogCannotContainRoutesOrAmbiguousArchiveState) {
    auto encoded = json::parse(profiles::encode(sample()));
    encoded["schema"] = 2;
    encoded["profiles"][0]["archived"] = true;
    EXPECT_FALSE(profiles::decode(encoded.dump()));
    encoded["profiles"][0]["clients"] = json::array();
    EXPECT_TRUE(profiles::decode(encoded.dump()));
    encoded["profiles"][0]["archived"] = 1;
    EXPECT_FALSE(profiles::decode(encoded.dump()));
    encoded["profiles"][0].erase("archived");
    EXPECT_FALSE(profiles::decode(encoded.dump()));
  }

  TEST(MultiseatProfileEditRequest, RejectsAmbiguousRemovalAndUntrustedRuntimeFields) {
    EXPECT_TRUE(profiles::decode_edit_request(R"({"operation":"remove","profile_id":"space-a"})"));
    EXPECT_TRUE(profiles::decode_edit_request(R"({"operation":"restore","profile_id":"space-a"})"));
    EXPECT_TRUE(profiles::decode_edit_request(R"({"operation":"rename","profile_id":"space-a","name":"Player 2"})"));
    for (const auto *body : {
      R"({"operation":"rename","profile_id":"space-a","name":""})",
      R"({"operation":"rename","profile_id":"space-a","name":" x "})",
      R"({"operation":"remove","profile_id":"space-a","name":"x"})",
      R"({"operation":"remove","profile_id":"space-a","delete_data":true})",
      R"({"operation":"remove","profile_id":"space-a","image":"sha256:abc"})",
      R"({"operation":"remove","profile_id":"space-a","profile_id":"space-b"})",
      R"({"operation":"unknown","profile_id":"space-a"})",
      R"({"operation":"remove","profile_id":"../space-a"})",
      R"({"operation":"remove","profile_id":[]})"}) EXPECT_FALSE(profiles::decode_edit_request(body));
    EXPECT_FALSE(profiles::decode_edit_request(std::string(4097, ' ')));
  }

  TEST(MultiseatProfileEditRequest, RemovingForGoodIsItsOwnOperationWithTheTypedNameAndARequestIdentity) {
    const auto removal = profiles::decode_edit_request(
      R"({"operation":"delete","profile_id":"space-a","confirm_name":"Living room","request_id":"12345678-1234-4234-8234-123456789abc"})");
    ASSERT_TRUE(removal);
    EXPECT_EQ(removal->operation, profiles::edit_operation_e::remove_for_good);
    EXPECT_EQ(removal->confirm_name, "Living room");
    EXPECT_EQ(removal->request_id, "12345678-1234-4234-8234-123456789abc");
    EXPECT_TRUE(removal->name.empty());
    for (const auto *body : {
      R"({"operation":"delete","profile_id":"space-a","request_id":"12345678-1234-4234-8234-123456789abc"})",
      R"({"operation":"delete","profile_id":"space-a","confirm_name":"Living room"})",
      R"({"operation":"delete","profile_id":"space-a","confirm_name":"","request_id":"12345678-1234-4234-8234-123456789abc"})",
      R"({"operation":"delete","profile_id":"space-a","confirm_name":"Living room","request_id":"12345678-1234-4234-8234-123456789ABC"})",
      R"({"operation":"delete","profile_id":"space-a","confirm_name":"Living\u0007room","request_id":"12345678-1234-4234-8234-123456789abc"})",
      R"({"operation":"delete","profile_id":"space-a","confirm_name":"Living room","request_id":"12345678-1234-4234-8234-123456789abc","name":"x"})",
      R"({"operation":"delete","profile_id":"../space-a","confirm_name":"Living room","request_id":"12345678-1234-4234-8234-123456789abc"})",
      R"({"operation":"remove","profile_id":"space-a","confirm_name":"Living room"})",
      R"({"operation":"remove","profile_id":"space-a","request_id":"12345678-1234-4234-8234-123456789abc"})",
      R"({"operation":"restore","profile_id":"space-a","confirm_name":"Living room","request_id":"12345678-1234-4234-8234-123456789abc"})"})
      EXPECT_FALSE(profiles::decode_edit_request(body)) << body;
    EXPECT_FALSE(profiles::valid_edit_request({profiles::edit_operation_e::rename, "space-a", "Player 2", "Player 2"}));
  }

  TEST_F(MultiseatProfileCatalog, FirstSpaceCreatesAPrivateCatalogAndFreshHomeWithoutASource) {
    provisioning_host_t host; host.image_family = "steam";
    const auto result = profiles::create_first_space(path, first_request, first_image, "steam", host);
    ASSERT_TRUE(result) << result.error;
    ASSERT_EQ(host.calls.size(), 10U);
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->catalog.profiles.size(), 1U);
    const auto &created = loaded->catalog.profiles.front();
    EXPECT_EQ(created.storage.profile_key, first_request.request_id);
    EXPECT_EQ(created.storage.opaque_volume_name, "pv-" + first_request.request_id);
    EXPECT_EQ(created.storage.image_reference, first_image);
    EXPECT_EQ(created.name, first_request.name);
    EXPECT_EQ(created.workload.target_id, "big-picture-v1");
    EXPECT_TRUE(created.client_keys.empty());
  }

  TEST_F(MultiseatProfileCatalog, FirstSpaceSupportsAnEmptyInitializedCatalog) {
    ASSERT_TRUE(profiles::initialize(path, 1000, 1000));
    provisioning_host_t host; host.image_family = "steam";
    EXPECT_TRUE(profiles::create_first_space(path, first_request, first_image, "steam", host));
    EXPECT_EQ(host.calls.size(), 10U);
  }

  TEST_F(MultiseatProfileCatalog, FirstSpaceRetryPreservesItsHomeAndLaterPlayerAssignments) {
    provisioning_host_t host; host.image_family = "steam";
    ASSERT_TRUE(profiles::create_first_space(path, first_request, first_image, "steam", host));
    ASSERT_TRUE(profiles::assign(path, first_request.request_id, "paired-client"));
    ASSERT_TRUE(profiles::create_first_space(path, first_request, first_image, "steam", host));
    EXPECT_EQ(host.calls.size(), 10U);
    auto changed = first_request; changed.name = "Someone else";
    EXPECT_FALSE(profiles::create_first_space(path, changed, first_image, "steam", host));
    EXPECT_FALSE(profiles::create_first_space(path, first_request, "sha256:" + std::string(64, 'c'), "steam", host));
    // A family this build does not carry cannot make the first Space, because
    // the workload it would open is not one any image holds a launcher for.
    EXPECT_FALSE(profiles::create_first_space(path, first_request, first_image, "", host));
    EXPECT_FALSE(profiles::create_first_space(path, first_request, first_image, "gamescope", host));
    EXPECT_EQ(host.calls.size(), 10U);
    const auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->catalog.profiles.front().client_keys, std::vector<std::string>{"paired-client"});
  }

  TEST_F(MultiseatProfileCatalog, FirstSpaceCannotReplaceAnExistingUnsafeOrBusyCatalog) {
    // A catalog that already holds a Space of this family, which is the state
    // the first-Space path exists to refuse.
    auto configured = sample();
    configured.profiles.front().storage.runtime_profile = runtime_profile_e::steam;
    configured.profiles.front().workload = {workload_kind_e::steam, "big-picture-v1"};
    save(configured);
    const auto before = psf::read_secure(path, profiles::maximum_catalog_bytes).payload;
    provisioning_host_t host; host.image_family = "steam";
    EXPECT_FALSE(profiles::create_first_space(path, first_request, first_image, "steam", host));
    EXPECT_EQ(psf::read_secure(path, profiles::maximum_catalog_bytes).payload, before);
    {
      auto lease = profiles::load(path);
      ASSERT_TRUE(lease);
      EXPECT_FALSE(profiles::create_first_space(path, first_request, first_image, "steam", host));
    }
    ASSERT_TRUE(psf::write_atomic(path, "not a catalog"));
    EXPECT_FALSE(profiles::create_first_space(path, first_request, first_image, "steam", host));
    EXPECT_EQ(psf::read_secure(path, profiles::maximum_catalog_bytes).payload, "not a catalog");
    EXPECT_TRUE(host.calls.empty());
  }

  /**
   * A launcher family's runtime carries its own launcher and its own library,
   * so a host full of Steam Spaces still has no Heroic one to copy. The first
   * Space of each family takes the first-Space path, and only a Space of that
   * same family closes it.
   */
  TEST_F(MultiseatProfileCatalog, AHostWithSteamSpacesCanStillMakeItsFirstHeroicSpace) {
    auto configured = sample();
    configured.profiles.front().storage.runtime_profile = runtime_profile_e::steam;
    configured.profiles.front().workload = {workload_kind_e::steam, "big-picture-v1"};
    save(configured);

    provisioning_host_t host; host.image_family = "heroic";
    ASSERT_TRUE(profiles::create_first_space(path, first_request, first_image, "heroic", host));
    const auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->catalog.profiles.size(), 2U);
    const auto &made = loaded->catalog.profiles.back();
    EXPECT_EQ(made.storage.runtime_profile, runtime_profile_e::heroic);
    EXPECT_EQ(made.workload.kind, workload_kind_e::heroic);
    EXPECT_EQ(made.workload.target_id, "library-v1");
    EXPECT_EQ(loaded->catalog.profiles.front().storage.runtime_profile, runtime_profile_e::steam)
      << "the Steam Space it was made beside is untouched";

    // And a second Heroic Space is not made this way: it copies the first.
    provisioning_host_t again; again.image_family = "heroic";
    profiles::first_space_request_t second {"22345678-1234-4234-8234-123456789abc", "Another"};
    EXPECT_FALSE(profiles::create_first_space(path, second, first_image, "heroic", again));
  }

  TEST_F(MultiseatProfileCatalog, FirstSpaceFailuresRetainResourcesWithoutPublishingOrAdoptingAHome) {
    for (std::size_t failure = 1; failure <= 10; ++failure) {
      const auto target = root / ("failed-" + std::to_string(failure) + ".json");
      provisioning_host_t host; host.image_family = "steam"; host.fail_call = failure;
      const auto result = profiles::create_first_space(target, first_request, first_image, "steam", host);
      EXPECT_FALSE(result) << failure;
      EXPECT_EQ(host.calls.size(), failure);
      EXPECT_EQ(psf::read_secure(target, profiles::maximum_catalog_bytes).status, psf::read_status_e::missing);
      for (const auto &args : host.calls) EXPECT_EQ(std::find(args.begin(), args.end(), "rm"), args.end());
      if (!host.volume.empty()) {
        EXPECT_EQ(result.volume_name, "pv-" + first_request.request_id);
        host.retain_volume_in_inventory = true;
        EXPECT_FALSE(profiles::create_first_space(target, first_request, first_image, "steam", host));
        EXPECT_EQ(host.calls.size(), failure + 2);
      }
    }
  }

  TEST_F(MultiseatProfileCatalog, FirstSpaceUncertainCommitIsConfirmedWithoutProvisioningAgain) {
    provisioning_host_t host; host.image_family = "steam";
    psf::set_write_fault_for_tests(psf::write_fault_e::post_rename_durability);
    EXPECT_EQ(profiles::create_first_space(path, first_request, first_image, "steam", host).status,
      psf::write_status_e::durability_uncertain);
    psf::set_write_fault_for_tests(psf::write_fault_e::none);
    EXPECT_TRUE(profiles::create_first_space(path, first_request, first_image, "steam", host));
    EXPECT_EQ(host.calls.size(), 10U);
  }

  TEST_F(MultiseatProfileCatalog, FirstSpaceRejectsInvalidIdentityBeforeStorageOrDocker) {
    provisioning_host_t host; host.image_family = "steam";
    auto invalid = first_request; invalid.request_id = "../other";
    EXPECT_FALSE(profiles::create_first_space(path, invalid, first_image, "steam", host));
    invalid = first_request; invalid.name = " Hidden";
    EXPECT_FALSE(profiles::create_first_space(path, invalid, first_image, "steam", host));
    EXPECT_FALSE(profiles::create_first_space(path, first_request, "mutable:latest", "steam", host));
    host.uid = 1001;
    EXPECT_FALSE(profiles::create_first_space(path, first_request, first_image, "steam", host));
    EXPECT_TRUE(host.calls.empty());
    EXPECT_EQ(psf::read_secure(path, profiles::maximum_catalog_bytes).status, psf::read_status_e::missing);
  }

  TEST_F(MultiseatProfileCatalog, SteamCreationUsesConfiguredImageAndFreshBigPictureHome) {
    auto catalog = sample();
    catalog.profiles[0].storage.runtime_profile = runtime_profile_e::steam;
    catalog.profiles[0].workload = {workload_kind_e::steam, "870780"};
    save(catalog);
    provisioning_host_t host; host.image_family = "steam";
    const auto result = profiles::create_space(path, steam_request, host);
    ASSERT_TRUE(result) << result.error;
    EXPECT_EQ(result.profile_key, steam_request.request_id);
    EXPECT_EQ(host.calls.size(), 10U);
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->catalog.profiles.size(), 2U);
    const auto &created = loaded->catalog.profiles.back();
    EXPECT_EQ(created.name, steam_request.name);
    EXPECT_EQ(created.storage.image_reference, catalog.profiles[0].storage.image_reference);
    EXPECT_NE(created.storage.opaque_volume_name, catalog.profiles[0].storage.opaque_volume_name);
    EXPECT_TRUE(created.client_keys.empty());
    EXPECT_EQ(created.workload.target_id, "big-picture-v1");
    EXPECT_EQ(loaded->catalog.profiles[0].client_keys, catalog.profiles[0].client_keys);
  }

  TEST_F(MultiseatProfileCatalog, RepeatedCreationConfirmsTheSameHomeAndPreservesLaterAssignments) {
    auto catalog = sample();
    catalog.profiles[0].storage.runtime_profile = runtime_profile_e::steam;
    catalog.profiles[0].workload = {workload_kind_e::steam, "big-picture-v1"};
    save(catalog);
    provisioning_host_t host; host.image_family = "steam";
    ASSERT_TRUE(profiles::create_space(path, steam_request, host));
    ASSERT_TRUE(profiles::assign(path, steam_request.request_id, "client-b"));
    ASSERT_TRUE(profiles::create_space(path, steam_request, host));
    EXPECT_EQ(host.calls.size(), 10U);
    auto changed = steam_request; changed.name = "Different player";
    EXPECT_FALSE(profiles::create_space(path, changed, host));
    EXPECT_EQ(host.calls.size(), 10U);
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->catalog.profiles.size(), 2U);
    EXPECT_EQ(loaded->catalog.profiles.back().client_keys, std::vector<std::string>{"client-b"});
    EXPECT_EQ(loaded->catalog.profiles.back().name, steam_request.name);
  }

  TEST_F(MultiseatProfileCatalog, FailedCreationRetainsResourcesAndRetryCannotAdoptTheOrphan) {
    auto catalog = sample();
    catalog.profiles[0].storage.runtime_profile = runtime_profile_e::steam;
    catalog.profiles[0].workload = {workload_kind_e::steam, "big-picture-v1"};
    save(catalog);
    provisioning_host_t host; host.image_family = "steam";
    host.fail_call = 5; host.retain_volume_in_inventory = true;
    const auto failed = profiles::create_space(path, steam_request, host);
    EXPECT_FALSE(failed);
    EXPECT_EQ(failed.volume_name, "pv-" + steam_request.request_id);
    EXPECT_EQ(host.calls.size(), 5U);
    EXPECT_FALSE(profiles::create_space(path, steam_request, host));
    EXPECT_EQ(host.calls.size(), 7U); // Only engine admission and absence inventory on retry.
    auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(profiles::encode(loaded->catalog), profiles::encode(catalog));
  }

  TEST_F(MultiseatProfileCatalog, UncertainCreationCanBeConfirmedWithoutProvisioningAgain) {
    auto catalog = sample();
    catalog.profiles[0].storage.runtime_profile = runtime_profile_e::steam;
    catalog.profiles[0].workload = {workload_kind_e::steam, "big-picture-v1"};
    save(catalog);
    provisioning_host_t host; host.image_family = "steam";
    psf::set_write_fault_for_tests(psf::write_fault_e::post_rename_durability);
    EXPECT_EQ(profiles::create_space(path, steam_request, host).status, psf::write_status_e::durability_uncertain);
    psf::set_write_fault_for_tests(psf::write_fault_e::none);
    ASSERT_TRUE(profiles::create_space(path, steam_request, host));
    EXPECT_EQ(host.calls.size(), 10U);
  }

  TEST_F(MultiseatProfileCatalog, CreationRejectsUnconfiguredOrNonSteamSourcesBeforeDocker) {
    save(sample());
    provisioning_host_t host;
    EXPECT_FALSE(profiles::create_space(path, steam_request, host));
    auto request = steam_request; request.source_profile_id = "missing";
    EXPECT_FALSE(profiles::create_space(path, request, host));
    EXPECT_TRUE(host.calls.empty());
  }

  TEST(MultiseatSteamCreationRequest, RejectsUnboundedAmbiguousAndRuntimeAuthorityFields) {
    const json base {{"request_id", steam_request.request_id}, {"source_profile_id", steam_request.source_profile_id},
      {"name", steam_request.name}};
    ASSERT_TRUE(profiles::decode_space_create_request(base.dump()));
    const std::vector<std::function<void(json &)>> mutations {
      [](auto &v) { v["image"] = "sha256:" + std::string(64, 'a'); },
      [](auto &v) { v["volume"] = "existing-home"; },
      [](auto &v) { v["command"] = "/bin/sh"; },
      [](auto &v) { v["clients"] = json::array({"client-a"}); },
      [](auto &v) { v["name"] = ""; }, [](auto &v) { v["name"] = "   "; },
      [](auto &v) { v["name"] = "name\nsecond line"; },
      [](auto &v) { v["name"] = std::string(129, 'a'); },
      [](auto &v) { v["name"] = 123; },
      [](auto &v) { v["source_profile_id"] = "../profile"; },
      [](auto &v) { v["source_profile_id"] = v["request_id"]; },
      [](auto &v) { v["request_id"] = "../../../somewhere"; },
      [](auto &v) { v["request_id"] = "12345678-1234-4234-8234-123456789abz"; },
      [](auto &v) { v.erase("request_id"); },
    };
    for (const auto &mutate : mutations) {
      auto payload = base; mutate(payload);
      EXPECT_FALSE(profiles::decode_space_create_request(payload.dump())) << payload;
    }
    auto duplicate = base.dump(); duplicate.insert(1, "\"name\":\"Other\",");
    EXPECT_FALSE(profiles::decode_space_create_request(duplicate));
    EXPECT_FALSE(profiles::decode_space_create_request(std::string(4097, ' ')));
    EXPECT_FALSE(profiles::decode_space_create_request("null"));
    EXPECT_FALSE(profiles::decode_space_create_request("[]"));
  }

  TEST_F(MultiseatProfileCatalog, AtomicAssignmentMovesPreserveOtherDevicesAndRejectUnknownTargets) {
    auto catalog = sample();
    auto other = catalog.profiles.front();
    other.storage.profile_key = "profile-b";
    other.storage.opaque_volume_name = "pv-profile-b";
    other.client_keys = {"client-b"};
    catalog.profiles.push_back(other);
    save(catalog);
    EXPECT_FALSE(profiles::set_assignment(path, "missing", "client-a"));
    ASSERT_TRUE(profiles::set_access(path, "profile-b", "client-a", true));
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
  TEST_F(MultiseatProfileCatalog, AdditionalSpaceAccessDoesNotMoveTheDefaultAssignment) {
    auto catalog = sample();
    auto extra = catalog.profiles.front();
    extra.storage.profile_key = "profile-b"; extra.storage.opaque_volume_name = "pv-profile-b";
    extra.client_keys.clear(); catalog.profiles.push_back(extra); save(catalog);
    ASSERT_TRUE(profiles::set_access(path, "profile-b", "client-a", true));
    auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->catalog.profiles[0].client_keys, std::vector<std::string>{"client-a"});
    EXPECT_EQ(loaded->catalog.profiles[1].access_clients, std::vector<std::string>{"client-a"});
    EXPECT_TRUE(loaded->catalog.profiles[1].client_keys.empty());
    const auto payload = profiles::encode(loaded->catalog); loaded.reset();
    EXPECT_EQ(json::parse(payload)["schema"], 3);
    ASSERT_TRUE(profiles::set_access(path, "profile-b", "client-a", false));
    loaded = profiles::load(path); ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.profiles[1].access_clients.empty());
  }

  TEST_F(MultiseatProfileCatalog, DuplicateAccessAndAccessToRemovedSpacesAreRejected) {
    auto catalog = sample(); catalog.profiles[0].access_clients = {"client-b", "client-b"};
    EXPECT_THROW((void)profiles::encode(catalog), std::invalid_argument);
    catalog.profiles[0].access_clients = {"client-b"}; save(catalog);
    ASSERT_TRUE(profiles::edit(path, {profiles::edit_operation_e::remove, "profile-a", {}}));
    auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.profiles[0].access_clients.empty()); loaded.reset();
    EXPECT_FALSE(profiles::set_access(path, "profile-a", "client-b", true));
  }

  TEST_F(MultiseatProfileCatalog, ReturningToOrdinaryStreamingRemovesEverySpaceGrant) {
    auto catalog = sample(); catalog.profiles[0].access_clients = {"client-a", "client-b"}; save(catalog);
    ASSERT_TRUE(profiles::set_assignment(path, "", "client-a"));
    auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.profiles[0].client_keys.empty());
    EXPECT_EQ(loaded->catalog.profiles[0].access_clients, std::vector<std::string>{"client-b"});
  }

  // A Default Space says where a device opens first and nothing more: saving one never changes
  // which Spaces or Desktop the device may open. Removal from every Space stays its own request.
  TEST_F(MultiseatProfileCatalog, DesktopDefaultKeepsSpaceAccessAndNeedsDesktopAccess) {
    save(sample());  // client-a has Living room as its Default Space
    const auto refused = profiles::set_assignment(path, "desktop", "client-a");
    EXPECT_FALSE(refused);
    ASSERT_TRUE(refused.refusal);
    EXPECT_EQ(refused.refusal->code, "desktop_access_required");
    EXPECT_EQ(refused.error, "Give this device Desktop Access before making Desktop its Default Space.");
    {
      const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
      EXPECT_EQ(loaded->catalog.profiles[0].client_keys, std::vector<std::string>{"client-a"});
      EXPECT_TRUE(loaded->catalog.desktop_default_clients.empty());
    }
    ASSERT_TRUE(profiles::set_desktop_access(path, "client-a", true));
    ASSERT_TRUE(profiles::set_assignment(path, "desktop", "client-a"));
    {
      const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
      EXPECT_TRUE(loaded->catalog.profiles[0].client_keys.empty());
      EXPECT_EQ(loaded->catalog.profiles[0].access_clients, std::vector<std::string>{"client-a"});
      EXPECT_EQ(loaded->catalog.desktop_clients, std::vector<std::string>{"client-a"});
      EXPECT_EQ(loaded->catalog.desktop_default_clients, std::vector<std::string>{"client-a"});
      const auto encoded = json::parse(profiles::encode(loaded->catalog));
      EXPECT_EQ(encoded["schema"], 5);
      // As strict as every schema: a repeat, a missing key, an older schema number, or a device
      // with two defaults is refused.
      auto malformed = encoded; malformed["desktop_default_clients"].push_back("client-a");
      EXPECT_FALSE(profiles::decode(malformed.dump()));
      malformed = encoded; malformed.erase("desktop_default_clients");
      EXPECT_FALSE(profiles::decode(malformed.dump()));
      malformed = encoded; malformed["schema"] = 4;
      EXPECT_FALSE(profiles::decode(malformed.dump()));
      malformed = encoded; malformed["profiles"][0]["clients"].push_back("client-a");
      EXPECT_FALSE(profiles::decode(malformed.dump()));
      EXPECT_TRUE(profiles::decode(encoded.dump()));
    }
    ASSERT_TRUE(profiles::set_assignment(path, "desktop", "client-a"));  // saving it again changes nothing
    ASSERT_TRUE(profiles::set_assignment(path, "profile-a", "client-a"));
    const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->catalog.profiles[0].client_keys, std::vector<std::string>{"client-a"});
    EXPECT_EQ(loaded->catalog.profiles[0].access_clients, std::vector<std::string>{"client-a"});
    EXPECT_EQ(loaded->catalog.desktop_clients, std::vector<std::string>{"client-a"});
    EXPECT_TRUE(loaded->catalog.desktop_default_clients.empty());
    EXPECT_EQ(json::parse(profiles::encode(loaded->catalog))["schema"], 4);
  }

  TEST_F(MultiseatProfileCatalog, SpaceDefaultNeedsAccessAndLeavingOneKeepsIt) {
    auto catalog = sample();
    auto other = catalog.profiles.front();
    other.storage.profile_key = "profile-b";
    other.storage.opaque_volume_name = "pv-profile-b";
    other.name = "Sam";
    other.client_keys.clear();
    catalog.profiles.push_back(other);
    save(catalog);
    const auto refused = profiles::set_assignment(path, "profile-b", "client-a");
    EXPECT_FALSE(refused);
    ASSERT_TRUE(refused.refusal);
    EXPECT_EQ(refused.refusal->code, "space_access_required");
    ASSERT_TRUE(profiles::set_access(path, "profile-b", "client-a", true));
    ASSERT_TRUE(profiles::set_assignment(path, "profile-b", "client-a"));
    {
      const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
      EXPECT_TRUE(loaded->catalog.profiles[0].client_keys.empty());
      EXPECT_EQ(loaded->catalog.profiles[0].access_clients, std::vector<std::string>{"client-a"});
      EXPECT_EQ(loaded->catalog.profiles[1].client_keys, std::vector<std::string>{"client-a"});
    }
    // Unticking the Default Space takes the device out of it entirely.
    ASSERT_TRUE(profiles::set_access(path, "profile-b", "client-a", false));
    const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.profiles[1].client_keys.empty());
    EXPECT_TRUE(loaded->catalog.profiles[1].access_clients.empty());
    EXPECT_EQ(loaded->catalog.profiles[0].access_clients, std::vector<std::string>{"client-a"});
  }

  TEST_F(MultiseatProfileCatalog, ADesktopDefaultGoesWithDesktopAccessOrAnExplicitRemoval) {
    auto catalog = sample();
    catalog.profiles[0].access_clients = {"client-b"};
    catalog.desktop_clients = {"client-b"};
    save(catalog);
    // A device without any Space already opens Desktop, so nothing is recorded for it.
    ASSERT_TRUE(profiles::set_assignment(path, "desktop", "client-z"));
    ASSERT_TRUE(profiles::set_assignment(path, "desktop", "client-b"));
    ASSERT_TRUE(profiles::set_desktop_access(path, "client-b", false));
    {
      const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
      EXPECT_TRUE(loaded->catalog.desktop_default_clients.empty());
      EXPECT_EQ(loaded->catalog.profiles[0].access_clients, std::vector<std::string>{"client-b"});
    }
    ASSERT_TRUE(profiles::set_desktop_access(path, "client-b", true));
    ASSERT_TRUE(profiles::set_assignment(path, "desktop", "client-b"));
    ASSERT_TRUE(profiles::set_assignment(path, "", "client-b"));
    {
      const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
      EXPECT_TRUE(loaded->catalog.desktop_default_clients.empty());
      EXPECT_TRUE(loaded->catalog.profiles[0].access_clients.empty());
      EXPECT_EQ(loaded->catalog.desktop_clients, std::vector<std::string>{"client-b"});
    }
    ASSERT_TRUE(profiles::set_assignment(path, "desktop", "client-b"));
    ASSERT_TRUE(profiles::unassign(path, "client-b"));
    ASSERT_TRUE(profiles::assign(path, "profile-a", "client-b"));
    const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.desktop_default_clients.empty());
    EXPECT_EQ(loaded->catalog.profiles[0].client_keys, (std::vector<std::string>{"client-a", "client-b"}));
  }

  TEST_F(MultiseatProfileCatalog, DesktopAccessIsExplicitAtomicAndBackwardsCompatible) {
    save(sample());
    ASSERT_TRUE(profiles::set_desktop_access(path, "client-a", true));
    {
      const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
      EXPECT_EQ(loaded->catalog.desktop_clients, (std::vector<std::string>{"client-a"}));
      EXPECT_EQ(json::parse(profiles::encode(loaded->catalog))["schema"], 4);
      auto malformed = json::parse(profiles::encode(loaded->catalog));
      malformed["desktop_clients"].push_back("client-a");
      EXPECT_FALSE(profiles::decode(malformed.dump()));
      malformed["desktop_clients"] = "client-a"; EXPECT_FALSE(profiles::decode(malformed.dump()));
    }
    ASSERT_TRUE(profiles::set_desktop_access(path, "client-a", false));
    const auto loaded = profiles::load(path); ASSERT_TRUE(loaded);
    EXPECT_TRUE(loaded->catalog.desktop_clients.empty());
    EXPECT_EQ(json::parse(profiles::encode(loaded->catalog))["schema"], 1);
  }

  // A Docker that answers removal questions from a small in-memory state.
  class removal_host_t : public container::host_t {
  public:
    std::vector<std::vector<std::string>> calls;
    std::set<std::string> volumes {"pv-space-a", "pv-space-b", "unrelated"};
    std::set<std::string> networks {"bridge", "pn-space-a", "pn-space-b"};
    json home = json::array({{{"Name", "pv-space-b"}, {"Driver", "local"}, {"Scope", "local"}, {"Options", nullptr},
      {"Labels", {{"io.polaris.multiseat.profile", "space-b"}}}, {"Mountpoint", "/var/lib/docker/volumes/pv-space-b/_data"}}});
    bool docker_down = false, rootless = false, keep_after_rm = false, network_occupied = false;
    std::size_t busy_listings = 0;  // `ps` reports a container using the home this many times
    std::uint64_t effective_uid() const override { return 1000; }
    bool executable_file(const std::filesystem::path &) const override { return true; }
    bool trusted_runtime_file(const std::filesystem::path &) const override { return true; }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return std::vector<std::uint64_t> {}; }
    bool readable_directory(const std::filesystem::path &) const override { return true; }
    bool private_read_write_directory(const std::filesystem::path &) const override { return true; }
    bool private_readable_file(const std::filesystem::path &) const override { return true; }
    std::optional<container::character_device_identity_t> read_write_character_device(const std::filesystem::path &) const override { return std::nullopt; }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &, std::size_t) const override { return std::nullopt; }
    static std::string lines(const std::set<std::string> &names) {
      std::string output;
      for (const auto &name : names) output += json(name).dump() + "\n";
      return output;
    }
    std::size_t count(std::string_view verb, std::string_view object) const {
      return std::count_if(calls.begin(), calls.end(), [&](const auto &args) {
        return args.size() > 1 && args[0] == object && args[1] == verb;
      });
    }
    container::command_result_t run(const std::vector<std::string> &argv,
                                    std::chrono::milliseconds duration, std::size_t bound) override {
      const auto prefix = container::command_prefix({});
      EXPECT_TRUE(std::equal(prefix.begin(), prefix.end(), argv.begin()));
      EXPECT_EQ(bound, profiles::maximum_catalog_bytes);
      const std::vector<std::string> args(argv.begin() + prefix.size(), argv.end());
      calls.push_back(args);
      if (docker_down) return {.exit_status = 1};
      const auto answer = [](std::string output) { return container::command_result_t {.exit_status = 0, .output = std::move(output)}; };
      if (args[0] == "info") {
        return answer(json {{"OSType", "linux"}, {"SecurityOptions", rootless ? json::array({"name=rootless"}) : json::array({"name=selinux"})}}.dump());
      }
      if (args[0] == "volume" && args[1] == "ls") return answer(lines(volumes));
      if (args[0] == "volume" && args[1] == "inspect") { EXPECT_EQ(args[2], "pv-space-b"); return answer(home.dump()); }
      if (args[0] == "ps") {
        EXPECT_EQ(args, (std::vector<std::string> {"ps", "--all", "--filter=volume=pv-space-b", "--format={{json .ID}}"}));
        if (busy_listings == 0) return answer("");
        --busy_listings;
        return answer("\"4f1c\"\n");
      }
      if (args[0] == "volume" && args[1] == "rm") {
        EXPECT_EQ(duration, std::chrono::minutes(10));
        EXPECT_EQ(args.size(), 3U);
        if (!keep_after_rm) volumes.erase(args[2]);
        return answer("");
      }
      EXPECT_EQ(duration, std::chrono::seconds(30));
      if (args[0] == "network" && args[1] == "ls") return answer(lines(networks));
      if (args[0] == "network" && args[1] == "inspect") {
        return answer(json::array({{{"Id", std::string(64, 'e')}, {"Name", "pn-space-b"},
          {"Driver", "bridge"}, {"Scope", "local"}, {"Internal", false}, {"Ingress", false}, {"Attachable", false},
          {"EnableIPv6", false}, {"Labels", {{"io.polaris.multiseat.profile", "space-b"}}},
          {"Options", {{"com.docker.network.bridge.enable_icc", "false"}, {"com.docker.network.bridge.enable_ip_masquerade", "true"}}},
          {"IPAM", {{"Driver", "default"}, {"Options", nullptr}}},
          {"Containers", network_occupied ? json {{"4f1c", json::object()}} : json::object()}}}).dump());
      }
      if (args[0] == "network" && args[1] == "rm") { networks.erase(args[2]); return answer(""); }
      ADD_FAILURE() << "Unexpected Docker operation " << args[0];
      return {};
    }
  };

  class MultiseatSpaceRemoval : public MultiseatProfileCatalog {
  protected:
    static profiles::entry_t steam(std::string id, std::string name, std::vector<std::string> clients) {
      return {.storage = {id, "pv-" + id, runtime_profile_e::steam, "sha256:" + std::string(64, 'a')},
        .name = std::move(name), .workload = {workload_kind_e::steam, "big-picture-v1"}, .client_keys = std::move(clients)};
    }
    profiles::catalog_t two_spaces() {
      auto catalog = profiles::catalog_t {1000, 1000, {steam("space-a", "Alex", {"client-a"}), steam("space-b", "Sam", {"client-b"})}};
      catalog.profiles[1].access_clients = {"client-a"};
      return catalog;
    }
    static profiles::edit_request_t removal(std::string name = "Sam", std::string id = "space-b") {
      return {profiles::edit_operation_e::remove_for_good, std::move(id), "", std::move(name), "12345678-1234-4234-8234-123456789abc"};
    }
    removal_host_t host;
    const profiles::entry_t *find(const profiles::catalog_t &catalog, std::string_view id) {
      const auto entry = std::find_if(catalog.profiles.begin(), catalog.profiles.end(),
        [&](const auto &value) { return value.storage.profile_key == id; });
      return entry == catalog.profiles.end() ? nullptr : &*entry;
    }
    // The Space is still exactly as it was: listed, not archived, with its devices.
    void expect_untouched() {
      const auto loaded = profiles::load(path);
      ASSERT_TRUE(loaded);
      const auto *entry = find(loaded->catalog, "space-b");
      ASSERT_NE(entry, nullptr);
      EXPECT_FALSE(entry->archived);
      EXPECT_EQ(entry->client_keys, std::vector<std::string> {"client-b"});
      EXPECT_EQ(entry->access_clients, std::vector<std::string> {"client-a"});
      EXPECT_TRUE(host.volumes.contains("pv-space-b"));
      EXPECT_EQ(host.count("rm", "volume"), 0U);
      EXPECT_EQ(host.count("rm", "network"), 0U);
    }
  };

  TEST_F(MultiseatSpaceRemoval, DeletesTheHomeTheNetworkAndTheRecordThroughDockerOnly) {
    save(two_spaces());
    const auto result = profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0));
    ASSERT_TRUE(result);
    EXPECT_TRUE(result.archived);
    EXPECT_TRUE(result.kept_volume.empty());
    EXPECT_TRUE(result.kept_network.empty());
    EXPECT_EQ(host.volumes, (std::set<std::string> {"pv-space-a", "unrelated"}));
    EXPECT_EQ(host.networks, (std::set<std::string> {"bridge", "pn-space-a"}));
    EXPECT_EQ(host.count("rm", "volume"), 1U);
    EXPECT_EQ(host.count("rm", "network"), 1U);
    for (const auto &args : host.calls) {
      EXPECT_NE(args[0], "run") << "removal never starts a container";
      EXPECT_TRUE(std::none_of(args.begin(), args.end(), [](const auto &arg) { return arg == "--force" || arg == "-f"; }));
    }
    const auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->catalog.profiles.size(), 1U);
    EXPECT_EQ(loaded->catalog.profiles[0].storage.profile_key, "space-a");
    EXPECT_EQ(loaded->catalog.profiles[0].client_keys, std::vector<std::string> {"client-a"});
  }

  TEST_F(MultiseatSpaceRemoval, RefusesAWrongNameAnUnknownSpaceAndTheLastSteamSpaceWithoutAskingDocker) {
    save(two_spaces());
    for (const auto *typed : {"sam", "Sam ", " Sam", "Alex"}) {
      EXPECT_EQ(profiles::remove_for_good(path, removal(typed), host, std::chrono::milliseconds(0)).outcome,
        profiles::removal_outcome_e::name_mismatch) << typed;
    }
    EXPECT_EQ(profiles::remove_for_good(path, removal("Sam", "space-z"), host, std::chrono::milliseconds(0)).outcome,
      profiles::removal_outcome_e::not_found);
    auto invalid = removal();
    invalid.request_id.clear();
    EXPECT_EQ(profiles::remove_for_good(path, invalid, host, std::chrono::milliseconds(0)).outcome, profiles::removal_outcome_e::invalid);
    EXPECT_TRUE(host.calls.empty());
    expect_untouched();
    // An ordinary catalog edit can never carry a removal for good.
    EXPECT_FALSE(profiles::edit(path, removal()));
    expect_untouched();

    auto only = two_spaces();
    only.profiles.erase(only.profiles.begin());
    save(only);
    const auto last = profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0));
    EXPECT_EQ(last.outcome, profiles::removal_outcome_e::last_space);
    EXPECT_FALSE(last.archived);
    EXPECT_TRUE(host.calls.empty());
    expect_untouched();
  }

  TEST_F(MultiseatSpaceRemoval, NeverDeletesStorageItCannotProveIsTheHomePolarisMadeForThisSpace) {
    save(two_spaces());
    const auto original = host.home;
    std::vector<std::function<void(json &)>> impostors {
      [](json &home) { home[0]["Labels"]["io.polaris.multiseat.profile"] = "space-a"; },
      [](json &home) { home[0]["Labels"] = json::object(); },
      // A local volume bound to a host directory would reach outside Docker's store.
      [](json &home) { home[0]["Options"] = {{"type", "none"}, {"o", "bind"}, {"device", "/srv/player"}}; },
      [](json &home) { home[0]["Mountpoint"] = "/var/lib/docker/volumes/pv-space-b/_data/../../../../../srv/player"; },
      [](json &home) { home[0]["Mountpoint"] = "/var/lib/docker/volumes/pv-space-b/./_data"; },
      [](json &home) { home[0]["Mountpoint"] = "volumes/pv-space-b/_data"; },
      [](json &home) { home[0]["Mountpoint"] = "/var/lib/docker/volumes/pv-space-a/_data"; },
      [](json &home) { home[0]["Driver"] = "nfs"; },
      [](json &home) { home[0]["Name"] = "pv-space-a"; },
      [](json &home) { home.push_back(home[0]); },
      [](json &home) { home = json::object(); },
    };
    for (std::size_t i = 0; i < impostors.size(); ++i) {
      host.home = original;
      impostors[i](host.home);
      host.calls.clear();
      const auto result = profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0));
      EXPECT_EQ(result.outcome, profiles::removal_outcome_e::storage_unverified) << i;
      EXPECT_EQ(result.kept_volume, "pv-space-b") << i;
      EXPECT_FALSE(result.archived) << i;
      expect_untouched();
    }
    // Docker may keep its store anywhere; the volume's own directory layout is what counts.
    host.home = original;
    host.home[0]["Mountpoint"] = "/srv/docker-data/volumes/pv-space-b/_data";
    EXPECT_TRUE(profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0)));
  }

  TEST_F(MultiseatSpaceRemoval, ChangesNothingWhileDockerIsDownRootlessOrStillUsingTheHome) {
    save(two_spaces());
    host.docker_down = true;
    EXPECT_EQ(profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0)).outcome, profiles::removal_outcome_e::docker_unavailable);
    expect_untouched();
    host.docker_down = false; host.rootless = true;
    EXPECT_EQ(profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0)).outcome, profiles::removal_outcome_e::docker_unavailable);
    expect_untouched();
    host.rootless = false; host.busy_listings = 1000;
    const auto busy = profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0));
    EXPECT_EQ(busy.outcome, profiles::removal_outcome_e::storage_in_use);
    EXPECT_EQ(busy.kept_volume, "pv-space-b");
    expect_untouched();
    // A short library read that finishes is waited out instead of refused.
    host.busy_listings = 1;
    EXPECT_TRUE(profiles::remove_for_good(path, removal(), host, std::chrono::seconds(5)));
    EXPECT_FALSE(host.volumes.contains("pv-space-b"));
  }

  TEST_F(MultiseatSpaceRemoval, AnUnfinishedDockerRemovalLeavesAnArchivedSpaceThatARetryFinishes) {
    save(two_spaces());
    host.keep_after_rm = true;
    const auto first = profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0));
    EXPECT_EQ(first.outcome, profiles::removal_outcome_e::storage_not_removed);
    EXPECT_TRUE(first.archived);
    EXPECT_EQ(first.kept_volume, "pv-space-b");
    EXPECT_EQ(host.count("rm", "network"), 0U) << "the network waits for the home";
    {
      const auto loaded = profiles::load(path);
      ASSERT_TRUE(loaded);
      const auto *entry = find(loaded->catalog, "space-b");
      ASSERT_NE(entry, nullptr);
      EXPECT_TRUE(entry->archived);
      EXPECT_TRUE(entry->client_keys.empty());
      EXPECT_TRUE(entry->access_clients.empty());
    }
    host.keep_after_rm = false;
    const auto retry = profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0));
    ASSERT_TRUE(retry);
    const auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(find(loaded->catalog, "space-b"), nullptr);
    EXPECT_FALSE(host.volumes.contains("pv-space-b"));
    EXPECT_FALSE(host.networks.contains("pn-space-b"));
  }

  TEST_F(MultiseatSpaceRemoval, FinishesWhenTheHomeIsAlreadyGoneAndNamesANetworkDockerKept) {
    save(two_spaces());
    host.volumes.erase("pv-space-b");
    host.network_occupied = true;
    const auto result = profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0));
    ASSERT_TRUE(result);
    EXPECT_EQ(host.count("inspect", "volume"), 0U);
    EXPECT_EQ(host.count("rm", "volume"), 0U);
    EXPECT_EQ(host.count("rm", "network"), 0U) << "an occupied or unfamiliar network is left alone";
    EXPECT_EQ(result.kept_network, "pn-space-b");
    const auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(find(loaded->catalog, "space-b"), nullptr);
  }

  TEST_F(MultiseatSpaceRemoval, AnArchiveThatWasNotSavedDeletesNothing) {
    save(two_spaces());
    psf::set_write_fault_for_tests(psf::write_fault_e::rename);
    const auto unsaved = profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0));
    psf::set_write_fault_for_tests(psf::write_fault_e::none);
    EXPECT_EQ(unsaved.outcome, profiles::removal_outcome_e::not_saved);
    EXPECT_EQ(unsaved.status, psf::write_status_e::not_committed);
    EXPECT_FALSE(unsaved.archived);
    expect_untouched();
    psf::set_write_fault_for_tests(psf::write_fault_e::post_rename_durability);
    const auto uncertain = profiles::remove_for_good(path, removal(), host, std::chrono::milliseconds(0));
    psf::set_write_fault_for_tests(psf::write_fault_e::none);
    EXPECT_EQ(uncertain.status, psf::write_status_e::durability_uncertain);
    EXPECT_FALSE(uncertain);
    EXPECT_EQ(host.count("rm", "volume"), 0U);
    EXPECT_TRUE(host.volumes.contains("pv-space-b"));
  }

  // Opt-in, against the local Docker Engine. The test makes its own scratch
  // homes and network, removes only those, and proves every other volume and
  // network is still there afterwards.
  TEST_F(MultiseatSpaceRemoval, PhysicalDockerRemovesOnlyTheScratchSpaceItMade) {
    if (!std::getenv("POLARIS_TEST_SPACE_REMOVAL_DOCKER")) GTEST_SKIP() << "Opt-in removal against the local Docker Engine";
    container::local_host_t docker;
    const auto run = [&](std::vector<std::string> args) {
      auto argv = container::command_prefix({});
      argv.insert(argv.end(), args.begin(), args.end());
      return docker.run(argv, std::chrono::seconds(60), profiles::maximum_catalog_bytes);
    };
    const auto names = [&](std::vector<std::string> args) {
      const auto result = run(std::move(args));
      EXPECT_EQ(result.exit_status, 0);
      std::set<std::string> listed;
      std::istringstream lines(result.output);
      for (std::string line; std::getline(lines, line);) if (!line.empty()) listed.insert(json::parse(line).get<std::string>());
      return listed;
    };
    const auto suffix = uuid_util::uuid_t::generate().string();
    const auto key = "removal-test-" + suffix, keeper = "removal-keeper-" + suffix, impostor = "removal-impostor-" + suffix;
    const std::vector<std::string> scratch_volumes {"pv-" + key, "pv-" + impostor};
    const auto network = container::profile_network_name(key);
    // Only this test's own names are ever cleaned up, whatever happens above.
    struct cleanup_t {
      std::function<void()> run;
      ~cleanup_t() { run(); }
    } cleanup {[&] {
      for (const auto &volume : scratch_volumes) (void) run({"volume", "rm", volume});
      (void) run({"network", "rm", network});
    }};
    const auto volumes_before = names({"volume", "ls", "--format={{json .Name}}"});
    const auto networks_before = names({"network", "ls", "--format={{json .Name}}"});
    ASSERT_EQ(run({"volume", "create", "--driver=local", "--label=io.polaris.multiseat.profile=" + key, "pv-" + key}).exit_status, 0);
    ASSERT_TRUE(container::create_profile_network(docker, key));
    // A home labelled for another Space must be refused and kept.
    ASSERT_EQ(run({"volume", "create", "--driver=local", "--label=io.polaris.multiseat.profile=" + key, "pv-" + impostor}).exit_status, 0);

    profiles::catalog_t catalog {1000, 1000, {steam(key, "Scratch", {}), steam(keeper, "Keeper", {}), steam(impostor, "Impostor", {})}};
    save(catalog);
    const auto refused = profiles::remove_for_good(path,
      {profiles::edit_operation_e::remove_for_good, impostor, "", "Impostor", "12345678-1234-4234-8234-123456789abc"}, docker, std::chrono::seconds(0));
    EXPECT_EQ(refused.outcome, profiles::removal_outcome_e::storage_unverified);
    EXPECT_TRUE(names({"volume", "ls", "--format={{json .Name}}"}).contains("pv-" + impostor));

    const auto removed = profiles::remove_for_good(path,
      {profiles::edit_operation_e::remove_for_good, key, "", "Scratch", "22345678-1234-4234-8234-123456789abc"}, docker, std::chrono::seconds(5));
    ASSERT_TRUE(removed) << static_cast<int>(removed.outcome) << " kept " << removed.kept_volume << " " << removed.kept_network;
    EXPECT_TRUE(removed.kept_volume.empty());
    EXPECT_TRUE(removed.kept_network.empty());
    const auto volumes_after = names({"volume", "ls", "--format={{json .Name}}"});
    const auto networks_after = names({"network", "ls", "--format={{json .Name}}"});
    EXPECT_FALSE(volumes_after.contains("pv-" + key));
    EXPECT_FALSE(networks_after.contains(network));
    for (const auto &volume : volumes_before) EXPECT_TRUE(volumes_after.contains(volume)) << volume << " must survive";
    for (const auto &name : networks_before) EXPECT_TRUE(networks_after.contains(name)) << name << " must survive";
    const auto loaded = profiles::load(path);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(find(loaded->catalog, key), nullptr);
    EXPECT_NE(find(loaded->catalog, keeper), nullptr);
    EXPECT_NE(find(loaded->catalog, impostor), nullptr);
  }

  // Moving a Space to another runtime: the removal Docker answers the questions a move asks.
  class MultiseatRuntimeMove : public MultiseatSpaceRemoval {
  protected:
    const std::string old_image = "sha256:" + std::string(64, 'a');
    const std::string new_image = "sha256:" + std::string(64, '9');
    profiles::runtime_move_t move() const {
      return {"space-b", old_image, "1", new_image, "1", runtime_profile_e::steam, 1000, 1000};
    }
    std::string bytes() const {
      const auto read = psf::read_secure(path, profiles::maximum_catalog_bytes, false, false);
      EXPECT_TRUE(read);
      return read.payload;
    }
    profiles::catalog_t now() const { return *profiles::decode(bytes()); }
    // A move asks Docker and never tells it to change anything or start a container.
    void expect_only_questions() const {
      for (const auto &args : host.calls) {
        EXPECT_TRUE(args[0] == "info" || (args.size() > 1 && (args[1] == "ls" || args[1] == "inspect")))
          << args[0] << ' ' << (args.size() > 1 ? args[1] : "");
      }
    }
  };

  TEST_F(MultiseatRuntimeMove, ChangesOnlyTheImageAndKeepsTheHomeNetworkNameAndDevices) {
    save(two_spaces());
    auto expected = two_spaces();
    expected.profiles[1].storage.image_reference = new_image;
    const auto result = profiles::move_runtime(path, move(), host);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.outcome, profiles::runtime_move_outcome_e::moved);
    EXPECT_EQ(result.status, psf::write_status_e::committed);
    EXPECT_EQ(result.previous_image, old_image);
    // Byte for byte the catalog it was, but for this Space's image.
    EXPECT_EQ(bytes(), profiles::encode(expected));
    const auto after = now();
    const auto *entry = find(after, "space-b");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->storage.opaque_volume_name, "pv-space-b");
    EXPECT_EQ(entry->name, "Sam");
    EXPECT_EQ(entry->client_keys, std::vector<std::string> {"client-b"});
    EXPECT_EQ(entry->access_clients, std::vector<std::string> {"client-a"});
    EXPECT_EQ(find(now(), "space-a")->storage.image_reference, old_image) << "another Space keeps its runtime";
    expect_only_questions();
    EXPECT_EQ(host.count("inspect", "volume"), 1U);
    EXPECT_TRUE(host.volumes.contains("pv-space-b"));
    EXPECT_TRUE(host.networks.contains("pn-space-b"));
  }

  TEST_F(MultiseatRuntimeMove, AMoveThatAlreadyLandedIsConfirmedWithoutDocker) {
    auto moved = two_spaces();
    moved.profiles[1].storage.image_reference = new_image;
    save(moved);
    const auto before = bytes();
    const auto result = profiles::move_runtime(path, move(), host);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.outcome, profiles::runtime_move_outcome_e::already_moved);
    EXPECT_EQ(result.status, psf::write_status_e::committed);
    EXPECT_EQ(bytes(), before);
    EXPECT_TRUE(host.calls.empty());
  }

  TEST_F(MultiseatRuntimeMove, EveryRefusalLeavesTheCatalogAsItWas) {
    using outcome_e = profiles::runtime_move_outcome_e;
    struct case_t {
      const char *name;
      std::function<void(profiles::catalog_t &, profiles::runtime_move_t &, removal_host_t &)> change;
      outcome_e outcome;
      bool asks_docker;
    };
    const std::vector<case_t> cases {
      {"unknown Space", [](auto &, auto &m, auto &) { m.profile_id = "space-z"; }, outcome_e::not_found, false},
      {"runtime changed meanwhile", [](auto &c, auto &, auto &) { c.profiles[1].storage.image_reference = "sha256:" + std::string(64, 'c'); },
        outcome_e::space_changed, false},
      {"another kind of Space", [](auto &, auto &m, auto &) { m.to_profile = runtime_profile_e::gamescope; }, outcome_e::profile_mismatch, false},
      {"another media contract", [](auto &, auto &m, auto &) { m.to_media_contract = "2"; }, outcome_e::media_contract_mismatch, false},
      {"another runtime account", [](auto &, auto &m, auto &) { m.to_uid = 1001; }, outcome_e::identity_mismatch, false},
      {"another runtime group", [](auto &, auto &m, auto &) { m.to_gid = 1001; }, outcome_e::identity_mismatch, false},
      {"a home owned by another account", [](auto &c, auto &m, auto &) { c.owner_uid = 1001; m.to_uid = 1001; },
        outcome_e::identity_mismatch, false},
      {"Docker down", [](auto &, auto &, auto &h) { h.docker_down = true; }, outcome_e::docker_unavailable, true},
      {"rootless Docker", [](auto &, auto &, auto &h) { h.rootless = true; }, outcome_e::docker_unavailable, true},
      {"home gone", [](auto &, auto &, auto &h) { h.volumes.erase("pv-space-b"); }, outcome_e::storage_unverified, true},
      {"home someone else labelled", [](auto &, auto &, auto &h) { h.home[0]["Labels"]["io.polaris.multiseat.profile"] = "space-a"; },
        outcome_e::storage_unverified, true},
      {"home bound to a host directory", [](auto &, auto &, auto &h) {
        h.home[0]["Options"] = {{"type", "none"}, {"o", "bind"}, {"device", "/srv/player"}}; }, outcome_e::storage_unverified, true},
    };
    for (const auto &item : cases) {
      auto catalog = two_spaces();
      auto request = move();
      host = removal_host_t {};
      item.change(catalog, request, host);
      save(catalog);
      const auto before = bytes();
      const auto result = profiles::move_runtime(path, request, host);
      EXPECT_FALSE(result) << item.name;
      EXPECT_EQ(result.outcome, item.outcome) << item.name;
      EXPECT_EQ(result.status, psf::write_status_e::not_committed) << item.name;
      EXPECT_EQ(bytes(), before) << item.name;
      EXPECT_EQ(!host.calls.empty(), item.asks_docker) << item.name;
      expect_only_questions();
    }
  }

  TEST_F(MultiseatRuntimeMove, AnInvalidMoveTouchesNeitherTheCatalogNorDocker) {
    save(two_spaces());
    const auto before = bytes();
    std::vector<profiles::runtime_move_t> invalid(7, move());
    invalid[0].to_image = "ghcr.io/papi-ux/polaris-worker-steam:latest";
    invalid[1].from_image.clear();
    invalid[2].profile_id = "../space-b";
    invalid[3].to_profile = runtime_profile_e::unknown;
    invalid[4].to_uid = 0;
    invalid[5].to_media_contract = "one";
    invalid[6].from_media_contract.clear();
    for (std::size_t i = 0; i < invalid.size(); ++i) {
      EXPECT_FALSE(profiles::valid_runtime_move(invalid[i])) << i;
      EXPECT_EQ(profiles::move_runtime(path, invalid[i], host).outcome, profiles::runtime_move_outcome_e::invalid) << i;
    }
    EXPECT_TRUE(profiles::valid_runtime_move(move()));
    EXPECT_EQ(bytes(), before);
    EXPECT_TRUE(host.calls.empty());
  }

  TEST_F(MultiseatRuntimeMove, ARetryAfterAnInterruptedWriteConvergesOnTheMovedSpace) {
    save(two_spaces());
    const auto before = bytes();
    // The replacement never reached the catalog: nothing changed, and the retry moves it.
    psf::set_write_fault_for_tests(psf::write_fault_e::rename);
    const auto unsaved = profiles::move_runtime(path, move(), host);
    psf::set_write_fault_for_tests(psf::write_fault_e::none);
    EXPECT_FALSE(unsaved);
    EXPECT_EQ(unsaved.outcome, profiles::runtime_move_outcome_e::not_saved);
    EXPECT_EQ(unsaved.status, psf::write_status_e::not_committed);
    EXPECT_EQ(bytes(), before);
    // The replacement landed but its durability is uncertain: the caller fails closed, and the
    // retry finds the new image and saves it again to confirm it.
    psf::set_write_fault_for_tests(psf::write_fault_e::post_rename_durability);
    const auto uncertain = profiles::move_runtime(path, move(), host);
    psf::set_write_fault_for_tests(psf::write_fault_e::none);
    EXPECT_FALSE(uncertain);
    EXPECT_EQ(uncertain.status, psf::write_status_e::durability_uncertain);
    EXPECT_EQ(find(now(), "space-b")->storage.image_reference, new_image);
    host.calls.clear();
    const auto confirmed = profiles::move_runtime(path, move(), host);
    ASSERT_TRUE(confirmed);
    EXPECT_EQ(confirmed.outcome, profiles::runtime_move_outcome_e::already_moved);
    EXPECT_EQ(confirmed.status, psf::write_status_e::committed);
    EXPECT_TRUE(host.calls.empty());
    auto expected = two_spaces();
    expected.profiles[1].storage.image_reference = new_image;
    EXPECT_EQ(bytes(), profiles::encode(expected));
  }

  TEST_F(MultiseatRuntimeMove, AControllerHoldingTheCatalogKeepsTheMoveOut) {
    save(two_spaces());
    const auto before = bytes();
    {
      const auto leased = profiles::load(path);
      ASSERT_TRUE(leased);
      const auto busy = profiles::move_runtime(path, move(), host);
      EXPECT_EQ(busy.outcome, profiles::runtime_move_outcome_e::not_saved);
      EXPECT_EQ(busy.status, psf::write_status_e::not_committed);
    }
    EXPECT_EQ(bytes(), before);
    EXPECT_TRUE(profiles::move_runtime(path, move(), host));
  }

  TEST(SpacesLibraryHost, UsesOnlyItsOwnedVolumeWithAnIsolatedReadOnlyHelper) {
    provisioning_host_t host; host.library_mode = true; host.volume = "pv-alex";
    host.profile = "alex"; host.image_family = "steam";
    container::profile_t profile{"alex", "pv-alex", runtime_profile_e::steam, "sha256:" + std::string(64, 'a')};
    const auto result = spaces::read_steam_library(host, profile);
    ASSERT_TRUE(result.available); ASSERT_EQ(result.games.size(), 1U);
    EXPECT_EQ(result.games[0].target, "870780"); EXPECT_EQ(host.calls.size(), 4U);
    for (int mode = 0; mode < 5; ++mode) {
      host.calls.clear(); host.rootless = mode == 0; host.wrong_label = mode == 1;
      host.implicit_volume = mode == 2; host.fail_call = mode >= 3 ? 4 : 0;
      host.timeout = mode == 3; host.truncated = mode == 4;
      EXPECT_FALSE(spaces::read_steam_library(host, profile).available);
      if (mode < 3) { EXPECT_LT(host.calls.size(), 4U); }
    }
  }

}  // namespace
