#include "src/platform/linux/spaces_runtime_move.h"
#include "src/private_state_file.h"

#include <gtest/gtest.h>
#include <array>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <future>
#include <mutex>
#include <unistd.h>

#ifdef __linux__
namespace {
  using namespace multiseat;
  using json = nlohmann::json;
  using namespace std::chrono_literals;

  spaces::runtime_t catalog_runtime(const char *id, const char *variant, const char *driver, char digest) {
    return {id, variant, std::string(40, 'a'), "sha256:" + std::string(64, digest),
      "sha256:" + std::string(64, static_cast<char>(digest + 1)), driver};
  }
  const auto nvidia610 = catalog_runtime("steam-nvidia-610", "nvidia", "610.57.04", '1');
  const auto nvidia615 = catalog_runtime("steam-nvidia-615", "nvidia", "615.71.09", '3');
  const auto amd_intel = catalog_runtime("steam-default", "default", "", '5');
  const std::vector<spaces::runtime_t> catalog {amd_intel, nvidia610, nvidia615};
  // A lab-built image the catalog does not name, as the maintainer's first Space was made from.
  const std::string lab_image = "sha256:" + std::string(64, 'e');

  json labeled(const std::string &image, json labels) {
    return json::array({{{"Id", image}, {"Os", "linux"}, {"Config", {{"Labels", std::move(labels)}}}}});
  }
  json nvidia_labels(const std::string &driver) {
    return {{"io.polaris.multiseat.profile", "steam"}, {"io.polaris.multiseat.media-contract", "1"},
      {"io.polaris.multiseat.architecture", "linux/amd64"}, {"io.polaris.multiseat.nvidia.driver", driver}};
  }

  // Answers `docker image inspect`: labelled images by id, the catalog runtimes by reference.
  class inspect_host_t : public container::host_t {
  public:
    std::vector<std::vector<std::string>> calls;
    std::map<std::string, json> images;
    std::map<std::string, json> runtimes;  // reference -> inspection
    bool silent = false;
    std::uint64_t effective_uid() const override { return 1000; }
    bool executable_file(const std::filesystem::path &) const override { return true; }
    bool trusted_runtime_file(const std::filesystem::path &) const override { return true; }
    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override { return std::vector<std::uint64_t> {}; }
    bool readable_directory(const std::filesystem::path &) const override { return false; }
    bool private_read_write_directory(const std::filesystem::path &) const override { return false; }
    bool private_readable_file(const std::filesystem::path &) const override { return false; }
    std::optional<container::character_device_identity_t> read_write_character_device(const std::filesystem::path &) const override { return std::nullopt; }
    std::optional<std::string> read_owned_regular_file(const std::filesystem::path &, std::size_t) const override { return std::nullopt; }
    container::command_result_t run(const std::vector<std::string> &argv, std::chrono::milliseconds timeout, std::size_t bound) override {
      const auto prefix = container::command_prefix({});
      EXPECT_TRUE(std::equal(prefix.begin(), prefix.end(), argv.begin()));
      const std::vector<std::string> args(argv.begin() + prefix.size(), argv.end());
      calls.push_back(args);
      EXPECT_EQ(timeout, 5s);
      EXPECT_EQ(bound, 65536U);
      if (args.size() != 3 || args[0] != "image" || args[1] != "inspect") {
        ADD_FAILURE() << "Only image inspection is expected";
        return {.exit_status = 1};
      }
      if (silent) return {.exit_status = 1, .timed_out = true};
      if (const auto found = images.find(args[2]); found != images.end()) return {.exit_status = 0, .output = found->second.dump()};
      if (const auto found = runtimes.find(args[2]); found != runtimes.end()) return {.exit_status = 0, .output = found->second.dump()};
      return {.exit_status = 1, .output = "[]\n"};
    }
  };

  // What `docker image inspect` prints for a verified catalog runtime.
  json verified(const spaces::runtime_t &r) {
    return json::array({{{"Id", r.config_digest}, {"Os", "linux"}, {"Architecture", "amd64"},
      {"RepoDigests", json::array({r.reference()})}, {"Config", {
        {"Entrypoint", json::array({"/usr/bin/polaris-seat-worker"})}, {"Cmd", json::array({"run"})},
        {"Labels", {{"org.opencontainers.image.source", "https://github.com/papi-ux/polaris"},
          {"org.opencontainers.image.revision", r.source_revision}, {"io.polaris.multiseat.profile", "steam"},
          {"io.polaris.multiseat.architecture", "linux/amd64"}, {"io.polaris.multiseat.media-contract", "1"},
          {"io.polaris.multiseat.nvidia.driver", r.nvidia_driver}}}}}}});
  }

  TEST(SpacesRuntimeMove, TheCatalogNamesWhatEachRuntimeNeedsOfAHome) {
    const auto decoded = spaces::trusted_runtimes();
    ASSERT_TRUE(decoded);
    ASSERT_FALSE(decoded->empty());
    for (const auto &runtime : *decoded) {
      EXPECT_EQ(runtime.profile, "steam") << runtime.id;
      EXPECT_EQ(runtime.media_contract, "1") << runtime.id;
      EXPECT_EQ(runtime.uid, 1000U) << runtime.id;
      EXPECT_EQ(runtime.gid, 1000U) << runtime.id;
    }
    // The maintainer's host: a Space made on 610.57.04 after the driver moved to 615.71.09.
    const auto choice = spaces::choose_runtime(*decoded, std::string("615.71.09"));
    ASSERT_TRUE(choice.runtime);
    EXPECT_EQ(choice.runtime->nvidia_driver, "615.71.09");
    const auto made_for_610 = std::find_if(decoded->begin(), decoded->end(), [](const auto &r) { return r.nvidia_driver == "610.57.04"; });
    ASSERT_NE(made_for_610, decoded->end());
    const auto identity = spaces::catalog_image_runtime(made_for_610->config_digest, *decoded);
    ASSERT_TRUE(identity);
    EXPECT_TRUE(spaces::driver_mismatch(*identity, std::string("615.71.09")));
  }

  TEST(SpacesRuntimeMove, ReadsTheLoadedDriverOnlyAsAPlainVersion) {
    std::array<char, 64> pattern {};
    const std::string value = "/tmp/polaris-driver-XXXXXX";
    std::copy(value.begin(), value.end(), pattern.begin());
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    const std::filesystem::path root = pattern.data();
    const auto version = root / "version";
    EXPECT_EQ(spaces::loaded_nvidia_driver(version), std::nullopt) << "no module loaded";
    std::ofstream(version) << "615.71.09\n";
    EXPECT_EQ(spaces::loaded_nvidia_driver(version), std::optional<std::string>("615.71.09"));
    std::ofstream(version) << "615.71.09; rm -rf /\n";
    EXPECT_EQ(spaces::loaded_nvidia_driver(version), std::optional<std::string>(""));
    std::filesystem::remove_all(root);
  }

  TEST(SpacesRuntimeMove, ACatalogImageIsKnownWithoutAskingDocker) {
    for (const auto &image : {nvidia610.config_digest, nvidia610.registry_digest}) {
      const auto found = spaces::catalog_image_runtime(image, catalog);
      ASSERT_TRUE(found) << image;
      EXPECT_EQ(*found, (spaces::image_runtime_t {true, "steam-nvidia-610", "steam", "1", "610.57.04"}));
    }
    EXPECT_FALSE(spaces::catalog_image_runtime(lab_image, catalog));
    inspect_host_t host;
    EXPECT_EQ(spaces::identify_image(host, nvidia615.config_digest, catalog).nvidia_driver, "615.71.09");
    EXPECT_TRUE(host.calls.empty());
  }

  TEST(SpacesRuntimeMove, AnImageOutsideTheCatalogIsReadFromItsOwnLabels) {
    const auto found = spaces::labeled_image_runtime(lab_image, labeled(lab_image, nvidia_labels("610.57.04")).dump());
    ASSERT_TRUE(found);
    EXPECT_EQ(*found, (spaces::image_runtime_t {true, "", "steam", "1", "610.57.04"}));
    // The AMD and Intel runtime carries no driver label at all.
    auto plain = nvidia_labels("");
    plain.erase("io.polaris.multiseat.nvidia.driver");
    const auto amd = spaces::labeled_image_runtime(lab_image, labeled(lab_image, plain).dump());
    ASSERT_TRUE(amd);
    EXPECT_EQ(amd->nvidia_driver, "");
    EXPECT_FALSE(spaces::driver_mismatch(*amd, std::string("615.71.09")));

    const std::vector<json> refused {
      labeled("sha256:" + std::string(64, 'f'), nvidia_labels("610.57.04")),  // another image answered
      json::array({labeled(lab_image, nvidia_labels("610.57.04"))[0], labeled(lab_image, nvidia_labels("610.57.04"))[0]}),
      labeled(lab_image, nvidia_labels("610.57.04 or so")),
      labeled(lab_image, nvidia_labels("610..57")),
      labeled(lab_image, json::object()),
      labeled(lab_image, {{"io.polaris.multiseat.profile", "Steam!"}, {"io.polaris.multiseat.media-contract", "1"}}),
      labeled(lab_image, {{"io.polaris.multiseat.profile", "steam"}, {"io.polaris.multiseat.media-contract", "one"}}),
      labeled(lab_image, nullptr),
      json::object(),
    };
    for (std::size_t i = 0; i < refused.size(); ++i)
      EXPECT_FALSE(spaces::labeled_image_runtime(lab_image, refused[i].dump())) << i;
    EXPECT_FALSE(spaces::labeled_image_runtime(lab_image, "not json"));
    EXPECT_FALSE(spaces::labeled_image_runtime("latest", labeled("latest", nvidia_labels("610.57.04")).dump()));
  }

  TEST(SpacesRuntimeMove, DockerIsAskedOncePerImageAndOnlyAnswersAreKept) {
    inspect_host_t host;
    spaces::image_runtime_cache_t cache;
    host.silent = true;
    EXPECT_FALSE(spaces::identify_image(host, lab_image, catalog, &cache).known);
    EXPECT_EQ(host.calls.size(), 1U);
    host.silent = false;
    EXPECT_FALSE(spaces::identify_image(host, lab_image, catalog, &cache).known) << "not in Docker";
    EXPECT_EQ(host.calls.size(), 2U) << "a silent Docker and a missing image are asked again";
    host.images[lab_image] = labeled(lab_image, nvidia_labels("610.57.04"));
    EXPECT_EQ(spaces::identify_image(host, lab_image, catalog, &cache).nvidia_driver, "610.57.04");
    EXPECT_EQ(spaces::identify_image(host, lab_image, catalog, &cache).nvidia_driver, "610.57.04");
    EXPECT_EQ(host.calls.size(), 3U) << "an image ID's labels never change";
    EXPECT_EQ(host.calls.back(), (std::vector<std::string> {"image", "inspect", lab_image}));
    // Never a reference Docker would resolve through a tag.
    EXPECT_FALSE(spaces::identify_image(host, "ubuntu:latest", catalog, &cache).known);
    EXPECT_EQ(host.calls.size(), 3U);
  }

  TEST(SpacesRuntimeMove, OnlyAReadableDifferentNvidiaDriverIsAMismatch) {
    const spaces::image_runtime_t made_for_610 {true, "", "steam", "1", "610.57.04"};
    EXPECT_TRUE(spaces::driver_mismatch(made_for_610, std::string("615.71.09")));
    EXPECT_FALSE(spaces::driver_mismatch(made_for_610, std::string("610.57.04")));
    EXPECT_FALSE(spaces::driver_mismatch(made_for_610, std::nullopt)) << "no NVIDIA driver loaded";
    EXPECT_FALSE(spaces::driver_mismatch(made_for_610, std::string())) << "a driver whose version could not be read";
    EXPECT_FALSE(spaces::driver_mismatch({}, std::string("615.71.09"))) << "an image Polaris could not read";
    EXPECT_FALSE(spaces::driver_mismatch({true, "", "steam", "1", ""}, std::string("615.71.09")));
  }

  /**
   * The console refuses a whole snapshot it cannot verify, and says only
   * "Could not verify Spaces". So the shape this host sends and the shape that
   * console accepts are pinned to one file both sides read.
   */
  TEST(SpacesRuntimeMove, DescribesAnUpgradeExactlyAsTheConsoleExpectsIt) {
    const std::filesystem::path source {POLARIS_SOURCE_DIR};
    std::ifstream file(source / "tests/fixtures/spaces-runtime-upgrade.json");
    ASSERT_TRUE(file) << "the shared shape must be readable from both languages";
    const auto expected = json::parse(file);

    std::vector<spaces::runtime_t> both {
      {"steam-nvidia-615", "nvidia", std::string(40, 'a'), "sha256:" + std::string(64, '1'),
       "sha256:" + std::string(64, '2'), "615.71.09"},
    };
    spaces::runtime_t borrowing {"steam-nvidia-host", "nvidia-host", std::string(40, 'a'),
      "sha256:" + std::string(64, '3'), "sha256:" + std::string(64, '4'), ""};
    borrowing.nvidia_minimum_driver = "570.00";
    both.insert(both.begin(), borrowing);

    // A Space on the runtime built for exactly this driver: nothing is broken,
    // and the borrowing runtime is still offered.
    const spaces::image_runtime_t current {true, "steam-nvidia-615", "steam", "1", "615.71.09"};
    const auto upgrade = spaces::describe_space_runtime(current, std::string("615.71.09"),
      spaces::choose_runtime(both, std::string("615.71.09")), spaces::runtime_image_e::verified);
    EXPECT_EQ(upgrade, expected.at("upgrade"));

    // And a Space built for another driver still reads as a repair.
    const spaces::image_runtime_t older {true, "steam-nvidia-610", "steam", "1", "610.57.04"};
    std::vector<spaces::runtime_t> baked {
      {"steam-nvidia-615", "nvidia", std::string(40, 'a'), "sha256:" + std::string(64, '1'),
       "sha256:" + std::string(64, '2'), "615.71.09"},
    };
    const auto repair = spaces::describe_space_runtime(older, std::string("615.71.09"),
      spaces::choose_runtime(baked, std::string("615.71.09")), spaces::runtime_image_e::verified);
    EXPECT_EQ(repair, expected.at("mismatch"));

    // A Space on a borrowing image this build no longer lists, read from its
    // own labels. Nothing is broken and no driver changed, which is exactly
    // why nothing else would ever carry a fixed runtime to it.
    const spaces::image_runtime_t older_borrowing {true, "", "steam", "1", "", "host"};
    const std::vector<spaces::runtime_t> borrowing_only {borrowing};
    const auto updated = spaces::describe_space_runtime(older_borrowing, std::string("615.71.09"),
      spaces::choose_runtime(borrowing_only, std::string("615.71.09")), spaces::runtime_image_e::verified);
    EXPECT_EQ(updated, expected.at("updated"));

    // And a repair whose target borrows this PC's driver names no version:
    // the reason says why the Space moves, the target says what it moves to.
    const auto repair_to_host = spaces::describe_space_runtime(older, std::string("615.71.09"),
      spaces::choose_runtime(both, std::string("615.71.09")), spaces::runtime_image_e::verified);
    EXPECT_EQ(repair_to_host, expected.at("mismatch_to_host"));

    // A Space already on the runtime this build would choose is offered nothing.
    const spaces::image_runtime_t current_borrowing {true, "steam-nvidia-host", "steam", "1", "", "host"};
    EXPECT_TRUE(spaces::describe_space_runtime(current_borrowing, std::string("615.71.09"),
      spaces::choose_runtime(borrowing_only, std::string("615.71.09")), spaces::runtime_image_e::verified)
        .at("runtime_move").is_null());
  }

  TEST(SpacesRuntimeMove, TheSpacesPageLearnsWhyAndWhereTheSpaceCanMove) {
    const spaces::image_runtime_t made_for_610 {true, "steam-nvidia-610", "steam", "1", "610.57.04"};
    const auto choice = spaces::choose_runtime(catalog, std::string("615.71.09"));
    const auto ready = spaces::describe_space_runtime(made_for_610, std::string("615.71.09"), choice, spaces::runtime_image_e::verified);
    EXPECT_EQ(ready["runtime_driver"], "610.57.04");
    EXPECT_EQ(ready["runtime_id"], "steam-nvidia-610");
    EXPECT_EQ(ready["host_driver"], "615.71.09");
    EXPECT_EQ(ready["runtime_mismatch"], true);
    EXPECT_EQ(ready["runtime_move"], (json {{"available", true}, {"runtime_id", "steam-nvidia-615"},
      {"reason", "driver_mismatch"}, {"nvidia_driver", "615.71.09"}, {"installed", true}, {"code", "runtime_ready"}}));
    const auto absent = spaces::describe_space_runtime(made_for_610, std::string("615.71.09"), choice, spaces::runtime_image_e::absent);
    EXPECT_EQ(absent["runtime_move"]["installed"], false);
    EXPECT_EQ(absent["runtime_move"]["code"], "not_downloaded");
    EXPECT_EQ(spaces::describe_space_runtime(made_for_610, std::string("615.71.09"), choice,
      spaces::runtime_image_e::unverifiable)["runtime_move"]["code"], "inspection_failed");
    EXPECT_EQ(spaces::describe_space_runtime(made_for_610, std::string("615.71.09"), choice,
      spaces::runtime_image_e::mismatch)["runtime_move"]["code"], "runtime_identity_mismatch");
    // A driver this build has no runtime for: the mismatch shows, and nothing to move to.
    const auto newer = spaces::describe_space_runtime(made_for_610, std::string("620.10.01"),
      spaces::choose_runtime(catalog, std::string("620.10.01")), spaces::runtime_image_e::unverifiable);
    EXPECT_EQ(newer["runtime_mismatch"], true);
    EXPECT_EQ(newer["runtime_move"], (json {{"available", false}, {"code", "runtime_not_published"}}));
    const auto current = spaces::describe_space_runtime(made_for_610, std::string("610.57.04"),
      spaces::choose_runtime(catalog, std::string("610.57.04")), spaces::runtime_image_e::verified);
    EXPECT_EQ(current["runtime_mismatch"], false);
    EXPECT_TRUE(current["runtime_move"].is_null());
    const auto unknown = spaces::describe_space_runtime({}, std::string("615.71.09"), choice, spaces::runtime_image_e::verified);
    EXPECT_TRUE(unknown["runtime_driver"].is_null());
    EXPECT_EQ(unknown["runtime_mismatch"], false);
    EXPECT_TRUE(spaces::describe_space_runtime(made_for_610, std::nullopt,
      spaces::choose_runtime(catalog, std::nullopt), spaces::runtime_image_e::verified)["host_driver"].is_null());
  }

  TEST(SpacesRuntimeMove, TheSpacesListAsksDockerOnlyWhatItNeeds) {
    inspect_host_t host;
    spaces::image_runtime_cache_t images;
    spaces::runtime_inspection_cache_t targets;
    host.images[lab_image] = labeled(lab_image, nvidia_labels("610.57.04"));
    const std::vector<profile_summary_t> profiles {
      {"space-a", "Alex", {}, "steam", false, {}, true, lab_image},
      {"space-b", "Sam", {}, "steam", false, {}, true, nvidia610.config_digest},
      {"space-c", "Kai", {}, "steam", false, {}, true, nvidia615.config_digest},
    };
    // A PC without an NVIDIA driver cannot mismatch: Docker is not asked.
    const auto amd = spaces::describe_space_runtimes(host, profiles, catalog, std::nullopt, &images, &targets);
    EXPECT_TRUE(host.calls.empty());
    EXPECT_TRUE(amd["space-a"]["runtime_driver"].is_null());
    EXPECT_EQ(amd["space-b"]["runtime_driver"], "610.57.04");
    EXPECT_EQ(amd["space-b"]["runtime_mismatch"], false);

    host.runtimes[nvidia615.reference()] = verified(nvidia615);
    const auto nvidia = spaces::describe_space_runtimes(host, profiles, catalog, std::string("615.71.09"), &images, &targets);
    EXPECT_EQ(nvidia["space-a"]["runtime_mismatch"], true);
    EXPECT_EQ(nvidia["space-b"]["runtime_mismatch"], true);
    EXPECT_EQ(nvidia["space-c"]["runtime_mismatch"], false);
    EXPECT_EQ(nvidia["space-a"]["runtime_move"]["installed"], true);
    EXPECT_EQ(nvidia["space-b"]["runtime_move"]["runtime_id"], "steam-nvidia-615");
    // One label read for the lab image and one look at the target, shared by both Spaces.
    ASSERT_EQ(host.calls.size(), 2U);
    EXPECT_EQ(host.calls[0], (std::vector<std::string> {"image", "inspect", lab_image}));
    EXPECT_EQ(host.calls[1], (std::vector<std::string> {"image", "inspect", nvidia615.reference()}));
    (void) spaces::describe_space_runtimes(host, profiles, catalog, std::string("615.71.09"), &images, &targets);
    EXPECT_EQ(host.calls.size(), 2U) << "the next refresh reads both answers from their caches";
  }

  // Opt-in and read-only, against the local Docker Engine: POLARIS_TEST_SPACES_RUNTIME_IMAGE names
  // a local runtime image ID and POLARIS_TEST_SPACES_RUNTIME_DRIVER the NVIDIA driver it was built
  // for. Only `docker image inspect` runs.
  TEST(SpacesRuntimeMove, ThePickerListsTheLaunchersASpaceCanBeMadeForHere) {
    inspect_host_t host;
    spaces::runtime_inspection_cache_t targets;
    auto heroic = catalog_runtime("heroic-default", "default", "", '7');
    heroic.profile = "heroic";
    auto families = catalog;
    families.push_back(heroic);
    const std::vector<profile_summary_t> profiles {
      {"space-a", "Alex", {}, "steam", false, {}, true, amd_intel.config_digest},
      {"space-z", "Gone", {}, "lutris", true, {}, true, lab_image},
    };
    // Steam already runs a Space here and lends the next one its image, so
    // Docker is asked about Heroic alone. Lutris has only an archived Space
    // and this build has no runtime for it, so it is not offered at all.
    auto listed = spaces::describe_launchers(host, profiles, families, std::nullopt, &targets);
    ASSERT_EQ(listed.size(), 2U);
    EXPECT_EQ(listed[0], (json {{"family", "steam"}, {"has_space", true}, {"installed", true}, {"runtime_id", ""}}));
    EXPECT_EQ(listed[1], (json {{"family", "heroic"}, {"has_space", false}, {"installed", false}, {"runtime_id", "heroic-default"}}));
    EXPECT_EQ(host.calls.size(), 1U);
    // Reading the page again costs Docker nothing, and a pull is seen after it.
    listed = spaces::describe_launchers(host, profiles, families, std::nullopt, &targets);
    EXPECT_EQ(host.calls.size(), 1U);
    auto here = verified(heroic);
    here[0]["Config"]["Labels"]["io.polaris.multiseat.profile"] = "heroic";
    here[0]["Config"]["Labels"].erase("io.polaris.multiseat.nvidia.driver");
    host.runtimes[heroic.reference()] = here;
    targets.forget();
    listed = spaces::describe_launchers(host, profiles, families, std::nullopt, &targets);
    EXPECT_EQ(listed[1]["installed"], true);
    // A catalog that publishes Steam alone offers Steam alone, and asks nothing.
    const auto calls = host.calls.size();
    EXPECT_EQ(spaces::describe_launchers(host, profiles, catalog, std::nullopt, &targets).size(), 1U);
    EXPECT_EQ(host.calls.size(), calls);
  }

  TEST(SpacesRuntimeMove, PhysicalDockerReadsWhatALocalImageWasBuiltFor) {
    const char *image = std::getenv("POLARIS_TEST_SPACES_RUNTIME_IMAGE");
    const char *driver = std::getenv("POLARIS_TEST_SPACES_RUNTIME_DRIVER");
    if (!image || !driver) GTEST_SKIP() << "Opt-in image inspection against the local Docker Engine";
    container::local_host_t docker;
    spaces::image_runtime_cache_t cache;
    // An empty catalog makes Docker answer, as it does for an image the build does not name.
    const auto identity = spaces::identify_image(docker, image, {}, &cache);
    ASSERT_TRUE(identity.known);
    EXPECT_EQ(identity.profile, "steam");
    EXPECT_EQ(identity.media_contract, "1");
    EXPECT_EQ(identity.nvidia_driver, driver);
    EXPECT_EQ(cache.find(image), identity);
  }

  spaces::move_facts_t movable() {
    spaces::move_facts_t facts;
    facts.admin_available = true;
    facts.space = profile_summary_t {"space-a", "Alex", {"client-a"}, "steam", false, {}, true, lab_image};
    facts.image = {true, "", "steam", "1", "610.57.04"};
    facts.host_driver = "615.71.09";
    facts.choice = spaces::choose_runtime(catalog, facts.host_driver);
    facts.target = spaces::runtime_image_e::absent;
    facts.home_uid = facts.home_gid = 1000;
    return facts;
  }

  TEST(SpacesRuntimeMove, AMismatchedSpaceMovesToTheRuntimeForTheLoadedDriver) {
    const auto decision = spaces::decide_move(movable(), "steam-nvidia-615");
    EXPECT_EQ(decision.result.status, 202);
    ASSERT_TRUE(decision.target);
    EXPECT_EQ(decision.target->id, "steam-nvidia-615");
    // Asked again after it finished: already there, whatever else is going on.
    auto done = movable();
    done.space->image = nvidia615.registry_digest;
    done.image = *spaces::catalog_image_runtime(nvidia615.registry_digest, catalog);
    done.streaming = done.space_active = true;
    const auto again = spaces::decide_move(done, "steam-nvidia-615");
    EXPECT_EQ(again.result.status, 200);
    EXPECT_TRUE(again.result.code.empty());
  }

  TEST(SpacesRuntimeMove, ANewerBuildOfTheSameKindOfRuntimeIsOfferedAndNothingElseIs) {
    // The image is read from its labels and is no catalog entry: an older build
    // of the 615 runtime, which this build's catalog has since replaced.
    auto older = movable();
    older.image.nvidia_driver = "615.71.09";
    const auto decision = spaces::decide_move(older, "steam-nvidia-615");
    EXPECT_EQ(decision.result.status, 202);
    ASSERT_TRUE(decision.target);
    EXPECT_EQ(decision.target->id, "steam-nvidia-615");
    // Another kind of runtime is never called an update. A Space without NVIDIA
    // userspace on a PC that now has an NVIDIA card is a change of graphics.
    auto other_graphics = movable();
    other_graphics.image.nvidia_driver.clear();
    EXPECT_EQ(spaces::decide_move(other_graphics, "steam-nvidia-615").result.code, "space_runtime_current");
    // And the same in the other direction, which is what a PC with no NVIDIA
    // driver loaded looks like to a Space that carries one.
    auto no_driver = movable();
    no_driver.host_driver.reset();
    no_driver.choice = spaces::choose_runtime(catalog, std::nullopt);
    EXPECT_EQ(spaces::decide_move(no_driver, "steam-default").result.code, "space_runtime_current");
  }

  TEST(SpacesRuntimeMove, EveryRefusalSaysWhyAndChangesNothing) {
    struct case_t {
      const char *name;
      std::function<void(spaces::move_facts_t &)> change;
      int status;
      std::string_view code;
      std::string runtime_id = "steam-nvidia-615";
    };
    const std::vector<case_t> cases {
      {"no Spaces owner", [](auto &f) { f.admin_available = false; }, 503, "spaces_admin_unavailable"},
      {"unknown Space", [](auto &f) { f.space.reset(); }, 404, "space_unknown"},
      {"unreadable runtime", [](auto &f) { f.image = {}; }, 503, "space_runtime_unknown"},
      {"already on this runtime", [](auto &f) { f.image.runtime_id = "steam-nvidia-615"; f.image.nvidia_driver = "615.71.09"; },
        409, "space_runtime_current"},
      {"no NVIDIA driver", [](auto &f) { f.host_driver.reset(); f.choice = spaces::choose_runtime(catalog, std::nullopt); },
        409, "space_runtime_current"},
      {"AMD and Intel runtime", [](auto &f) { f.image.nvidia_driver.clear(); }, 409, "space_runtime_current"},
      {"no runtime for this driver", [](auto &f) {
        f.host_driver = "620.10.01"; f.choice = spaces::choose_runtime(catalog, f.host_driver); }, 409, "space_runtime_not_published"},
      {"stale page", [](auto &) {}, 409, "space_runtime_changed", "steam-nvidia-610"},
      {"another kind of Space", [](auto &f) { f.image.profile = "gamescope"; }, 409, "space_runtime_profile_mismatch"},
      {"a Space of another launcher family", [](auto &f) { f.space->family = "heroic"; }, 409, "space_runtime_profile_mismatch"},
      {"a Space whose workload this build cannot stream", [](auto &f) { f.space->family = ""; }, 409, "space_runtime_profile_mismatch"},
      {"another media contract", [](auto &f) { f.image.media_contract = "2"; }, 409, "space_runtime_media_mismatch"},
      {"another account", [](auto &f) { f.home_uid = 1001; }, 409, "space_runtime_identity_mismatch"},
      {"another group", [](auto &f) { f.home_gid = 1001; }, 409, "space_runtime_identity_mismatch"},
      {"host setup", [](auto &f) { f.host_setup_running = true; }, 409, "spaces_host_setup_running"},
      {"first Space setup", [](auto &f) { f.setup_running = true; }, 409, "spaces_setup_running"},
      {"another change", [](auto &f) { f.changing = true; }, 409, "spaces_change_running"},
      {"this Space is open", [](auto &f) { f.space_active = f.streaming = true; }, 409, "space_active"},
      {"another Space is open", [](auto &f) { f.streaming = true; }, 409, "spaces_streaming"},
    };
    for (const auto &item : cases) {
      auto facts = movable();
      item.change(facts);
      const auto decision = spaces::decide_move(facts, item.runtime_id);
      EXPECT_EQ(decision.result.status, item.status) << item.name;
      EXPECT_EQ(decision.result.code, item.code) << item.name;
      EXPECT_FALSE(decision.result.message.empty()) << item.name;
      EXPECT_FALSE(decision.result.action.empty()) << item.name;
      EXPECT_FALSE(decision.target) << item.name;
    }
    // What the Space is outranks what is happening: a Space that cannot move says so while it is open.
    auto open = movable();
    open.image.media_contract = "2";
    open.space_active = open.streaming = true;
    EXPECT_EQ(spaces::decide_move(open, "steam-nvidia-615").result.code, "space_runtime_media_mismatch");
  }

  TEST(SpacesRuntimeMove, AMoveRequestNamesOneSpaceOneRuntimeAndItsOwnIdentity) {
    const auto valid = R"({"request_id":"12345678-1234-4234-8234-123456789abc","profile_id":"space-a","runtime_id":"steam-nvidia-615"})";
    const auto request = spaces::decode_move_request(valid);
    ASSERT_TRUE(request);
    EXPECT_EQ(*request, (spaces::move_request_t {"12345678-1234-4234-8234-123456789abc", "space-a", "steam-nvidia-615"}));
    for (const auto *payload : {
           R"({"request_id":"12345678-1234-4234-8234-123456789abc","profile_id":"space-a"})",
           R"({"request_id":"12345678-1234-4234-8234-123456789abc","profile_id":"space-a","runtime_id":"steam-nvidia-615","image":"sha256:00"})",
           R"({"request_id":"12345678-1234-4234-8234-123456789ABC","profile_id":"space-a","runtime_id":"steam-nvidia-615"})",
           R"({"request_id":"move","profile_id":"space-a","runtime_id":"steam-nvidia-615"})",
           R"({"request_id":"12345678-1234-4234-8234-123456789abc","profile_id":"../space-a","runtime_id":"steam-nvidia-615"})",
           R"({"request_id":"12345678-1234-4234-8234-123456789abc","profile_id":"space-a","runtime_id":"ghcr.io/other@sha256:00"})",
           R"({"request_id":"12345678-1234-4234-8234-123456789abc","profile_id":"space-a","profile_id":"space-b","runtime_id":"steam-nvidia-615"})",
           R"({"request_id":"12345678-1234-4234-8234-123456789abc","profile_id":{"id":"space-a"},"runtime_id":"steam-nvidia-615"})",
           "", "[]"}) {
      EXPECT_FALSE(spaces::decode_move_request(payload)) << payload;
    }
  }

  // Drives the move service with fakes: facts, a download and the Spaces owner.
  class SpacesMoveService : public ::testing::Test {
  protected:
    spaces::move_facts_t facts = movable();
    std::atomic<unsigned> fact_reads {0}, installs {0}, moves {0};
    spaces::runtime_install_result_t install_answer {true, "runtime_ready", "The approved gaming runtime is available.", nvidia615.config_digest};
    std::deque<profile_launch_result_t> move_answers;
    std::vector<profiles::runtime_move_t> moved;
    std::mutex mutex;
    std::condition_variable_any changed;
    bool hold_install = false;
    std::unique_ptr<spaces::move_service_t> service;
    const spaces::move_request_t request {"12345678-1234-4234-8234-123456789abc", "space-a", "steam-nvidia-615"};
    void SetUp() override {
      service = std::make_unique<spaces::move_service_t>(spaces::move_operations_t {
        .facts = [this](const spaces::move_request_t &) { ++fact_reads; return facts; },
        .install = [this](const spaces::runtime_t &target, std::stop_token stop) {
          ++installs;
          EXPECT_EQ(target.id, "steam-nvidia-615");
          std::unique_lock lock(mutex);
          changed.wait(lock, stop, [&] { return !hold_install; });
          if (stop.stop_requested()) return spaces::runtime_install_result_t {false, "download_cancelled", "Setup stopped.", {}};
          return install_answer;
        },
        .move = [this](const profiles::runtime_move_t &move) -> profile_launch_result_t {
          std::lock_guard lock(mutex);
          ++moves;
          moved.push_back(move);
          if (move_answers.empty()) return {200, "Space moved to the new gaming runtime"};
          const auto answer = move_answers.front();
          move_answers.pop_front();
          return answer;
        },
      }, 1ms);
    }
    void TearDown() override {
      release();
      service.reset();
    }
    void release() {
      { std::lock_guard lock(mutex); hold_install = false; }
      changed.notify_all();
    }
    json settled() {
      for (int i = 0; i < 400 && service->active(); ++i) std::this_thread::sleep_for(5ms);
      EXPECT_FALSE(service->active());
      return service->snapshot();
    }
  };

  TEST_F(SpacesMoveService, DownloadsTheRuntimeThenMovesTheSpaceKeepingItsHome) {
    hold_install = true;
    const auto started = service->submit(request);
    EXPECT_EQ(started.status, 202);
    auto job = service->snapshot();
    EXPECT_EQ(job["state"], "downloading");
    EXPECT_EQ(job["profile_id"], "space-a");
    EXPECT_EQ(job["runtime_id"], "steam-nvidia-615");
    EXPECT_EQ(job["nvidia_driver"], "615.71.09");
    EXPECT_TRUE(service->active());
    // The same request while it runs joins it; another one waits its turn.
    EXPECT_EQ(service->submit(request).status, 202);
    const auto second = service->submit({"22345678-1234-4234-8234-123456789abc", "space-b", "steam-nvidia-615"});
    EXPECT_EQ(second.status, 409);
    EXPECT_EQ(second.code, "space_move_running");
    release();
    job = settled();
    EXPECT_EQ(job["state"], "done");
    EXPECT_EQ(job["code"], "space_runtime_moved");
    ASSERT_EQ(moved.size(), 1U);
    EXPECT_EQ(moved[0], (profiles::runtime_move_t {"space-a", lab_image, "1", nvidia615.config_digest, "1",
      runtime_profile_e::steam, 1000, 1000}));
    EXPECT_EQ(installs, 1U);
    EXPECT_EQ(fact_reads, 1U) << "joining and refusing never read the facts again";
    const auto answered = service->submit(request);
    EXPECT_EQ(answered.status, 200);
    EXPECT_EQ(answered.code, "space_runtime_moved");
    EXPECT_EQ(moves, 1U);
    const auto reused = service->submit({request.request_id, "space-b", "steam-nvidia-615"});
    EXPECT_EQ(reused.status, 409);
    EXPECT_EQ(reused.code, "move_request_in_use");
  }

  TEST_F(SpacesMoveService, ADownloadedRuntimeGoesStraightToTheMoveAndItsCheckStillRuns) {
    facts.target = spaces::runtime_image_e::verified;
    hold_install = true;
    ASSERT_EQ(service->submit(request).status, 202);
    EXPECT_EQ(service->snapshot()["state"], "moving");
    release();
    EXPECT_EQ(settled()["state"], "done");
    EXPECT_EQ(installs, 1U) << "the verified image ID comes from the same check that would download it";
  }

  TEST_F(SpacesMoveService, ARefusedMoveStartsNoJobAndNoDownload) {
    facts.space_active = facts.streaming = true;
    const auto refused = service->submit(request);
    EXPECT_EQ(refused.status, 409);
    EXPECT_EQ(refused.code, "space_active");
    EXPECT_EQ(refused.message, "This Space is open on a device.");
    EXPECT_TRUE(service->snapshot().is_null());
    EXPECT_FALSE(service->active());
    // Once the stream ends the same request can start.
    facts.space_active = facts.streaming = false;
    EXPECT_EQ(service->submit(request).status, 202);
    EXPECT_EQ(settled()["state"], "done");
    EXPECT_EQ(installs, 1U);
  }

  // The first Space of a launcher: the same job, with making the Space where a move would be.
  class SpacesCreateService : public ::testing::Test {
  protected:
    spaces::runtime_t heroic = [] {
      auto runtime = catalog_runtime("heroic-nvidia-host", "nvidia-host", "", '7');
      runtime.profile = "heroic";
      runtime.nvidia_minimum_driver = "570.00";  // a borrowing runtime is not valid without its floor
      return runtime;
    }();
    spaces::create_facts_t facts {true, {heroic, {}}, spaces::runtime_image_e::absent};
    std::atomic<unsigned> fact_reads {0}, installs {0};
    spaces::runtime_install_result_t install_answer {true, "runtime_ready", "The approved gaming runtime is available.", {}};
    std::deque<profile_launch_result_t> create_answers;
    std::vector<profiles::space_create_request_t> created;
    std::mutex mutex;
    std::condition_variable_any changed;
    bool hold_install = false;
    std::unique_ptr<spaces::move_service_t> service;
    const profiles::space_create_request_t request {"12345678-1234-4234-8234-123456789abc", "", "Player 2", "heroic"};
    void SetUp() override {
      install_answer.image = heroic.config_digest;
      service = std::make_unique<spaces::move_service_t>(spaces::move_operations_t {
        .facts = [](const spaces::move_request_t &) { return movable(); },
        .install = [this](const spaces::runtime_t &target, std::stop_token stop) {
          ++installs;
          EXPECT_EQ(target.id, "heroic-nvidia-host");
          std::unique_lock lock(mutex);
          changed.wait(lock, stop, [&] { return !hold_install; });
          if (stop.stop_requested()) return spaces::runtime_install_result_t {false, "download_cancelled", "Setup stopped.", {}};
          return install_answer;
        },
        .move = [](const profiles::runtime_move_t &) -> profile_launch_result_t {
          ADD_FAILURE() << "a create job never moves a Space";
          return {500, "unexpected"};
        },
        .create_facts = [this](const profiles::space_create_request_t &) { ++fact_reads; return facts; },
        .create = [this](const profiles::space_create_request_t &creation) -> profile_launch_result_t {
          std::lock_guard lock(mutex);
          created.push_back(creation);
          if (create_answers.empty()) return {200, "Space created"};
          const auto answer = create_answers.front();
          create_answers.pop_front();
          return answer;
        },
      }, 1ms);
    }
    void TearDown() override {
      release();
      service.reset();
    }
    void release() {
      { std::lock_guard lock(mutex); hold_install = false; }
      changed.notify_all();
    }
    json settled() {
      for (int i = 0; i < 400 && service->active(); ++i) std::this_thread::sleep_for(5ms);
      EXPECT_FALSE(service->active());
      return service->snapshot();
    }
  };

  TEST_F(SpacesCreateService, DownloadsTheRuntimeThenMakesTheFirstSpaceOfALauncher) {
    hold_install = true;
    EXPECT_EQ(service->submit_create(request).status, 202);
    auto job = service->snapshot();
    EXPECT_EQ(job["kind"], "create");
    EXPECT_EQ(job["state"], "downloading");
    EXPECT_EQ(job["profile_id"], "") << "the Space does not exist yet";
    EXPECT_EQ(job["family"], "heroic");
    EXPECT_EQ(job["name"], "Player 2");
    EXPECT_EQ(job["runtime_id"], "heroic-nvidia-host");
    EXPECT_EQ(job["nvidia_driver"], "");
    // The same request joins it, and nothing else starts beside it: one
    // download at a time, whatever it is for.
    EXPECT_EQ(service->submit_create(request).status, 202);
    EXPECT_EQ(service->submit({"22345678-1234-4234-8234-123456789abc", "space-a", "steam-nvidia-615"}).code, "space_move_running");
    EXPECT_EQ(service->submit_create({"32345678-1234-4234-8234-123456789abc", "", "Player 3", "heroic"}).code, "space_move_running");
    EXPECT_TRUE(created.empty()) << "no Space is made before its runtime is verified";
    release();
    job = settled();
    EXPECT_EQ(job["state"], "done");
    EXPECT_EQ(job["code"], "space_created");
    ASSERT_EQ(created.size(), 1U);
    EXPECT_EQ(created[0], request);
    EXPECT_EQ(installs, 1U);
    EXPECT_EQ(fact_reads, 1U);
    // Asked again once done, it answers done and makes nothing twice.
    EXPECT_EQ(service->submit_create(request).status, 200);
    EXPECT_EQ(created.size(), 1U);
    EXPECT_EQ(service->submit_create({request.request_id, "", "Someone Else", "heroic"}).code, "move_request_in_use");
    // A move's snapshot says what it is too.
    EXPECT_EQ(service->submit({request.request_id, "space-a", "steam-nvidia-615"}).code, "move_request_in_use");
  }

  TEST_F(SpacesCreateService, ARuntimeAlreadyHereGoesStraightToMakingTheSpace) {
    facts.target = spaces::runtime_image_e::verified;
    hold_install = true;
    ASSERT_EQ(service->submit_create(request).status, 202);
    EXPECT_EQ(service->snapshot()["state"], "creating");
    release();
    EXPECT_EQ(settled()["state"], "done");
    EXPECT_EQ(installs, 1U) << "the verified image comes from the same check that would download it";
  }

  TEST_F(SpacesCreateService, AFailedDownloadMakesNoSpaceAndTheSameRequestTriesAgain) {
    install_answer = {false, "download_incomplete", "The runtime download did not finish. Retry the same runtime to reuse verified layers.", {}};
    ASSERT_EQ(service->submit_create(request).status, 202);
    auto job = settled();
    EXPECT_EQ(job["state"], "failed");
    EXPECT_EQ(job["code"], "download_incomplete");
    EXPECT_EQ(job["message"], "The runtime download did not finish. Retry the same runtime to reuse verified layers. The Space was not created.");
    EXPECT_EQ(job["action"], "Create the Space again.");
    EXPECT_TRUE(created.empty());
    // An image the catalog does not approve for this runtime is never made into a Space.
    install_answer = {true, "runtime_ready", "ok", nvidia615.config_digest};
    ASSERT_EQ(service->submit_create(request).status, 202) << "the identity names the Space, so the same request is the retry";
    job = settled();
    EXPECT_EQ(job["code"], "runtime_verification_failed");
    EXPECT_TRUE(created.empty());
    install_answer.image = heroic.config_digest;
    ASSERT_EQ(service->submit_create(request).status, 202);
    EXPECT_EQ(settled()["state"], "done");
    EXPECT_EQ(created.size(), 1U);
    EXPECT_EQ(installs, 3U);
  }

  TEST_F(SpacesCreateService, TheSpacesOwnerHasTheLastWord) {
    // Still saving is asked again; a refusal is the job's failure, in the owner's words.
    create_answers = {{202, "The Space is still being created.", "spaces_change_pending"},
      {409, "Stop every Space stream and wait for cleanup before changing Spaces.", "spaces_streaming", "End the running Space streams, then try again."}};
    ASSERT_EQ(service->submit_create(request).status, 202);
    const auto job = settled();
    EXPECT_EQ(job["state"], "failed");
    EXPECT_EQ(job["code"], "spaces_streaming");
    EXPECT_EQ(job["action"], "End the running Space streams, then try again.");
    EXPECT_EQ(created.size(), 2U);
  }

  TEST_F(SpacesCreateService, ARefusedFirstSpaceStartsNoJobAndNoDownload) {
    struct case_t {
      const char *name;
      std::function<void(spaces::create_facts_t &)> change;
      int status;
      std::string_view code;
    };
    for (const auto &item : std::vector<case_t> {
           {"no Spaces owner", [](auto &f) { f.admin_available = false; }, 503, "spaces_admin_unavailable"},
           {"no runtime for that launcher", [](auto &f) { f.choice = {std::nullopt, "runtime_not_published"}; }, 404, "space_family_unpublished"},
           {"host setup", [](auto &f) { f.host_setup_running = true; }, 409, "spaces_host_setup_running"},
           {"first Space setup", [](auto &f) { f.setup_running = true; }, 409, "spaces_setup_running"},
           {"another change", [](auto &f) { f.changing = true; }, 409, "spaces_change_running"},
           {"a stream", [](auto &f) { f.streaming = true; }, 409, "spaces_streaming"}}) {
      auto changed_facts = facts;
      item.change(changed_facts);
      const auto decision = spaces::decide_create(changed_facts);
      EXPECT_EQ(decision.result.status, item.status) << item.name;
      EXPECT_EQ(decision.result.code, item.code) << item.name;
      EXPECT_FALSE(decision.result.action.empty()) << item.name;
      EXPECT_FALSE(decision.target) << item.name;
    }
    facts.streaming = true;
    const auto refused = service->submit_create(request);
    EXPECT_EQ(refused.status, 409);
    EXPECT_EQ(refused.code, "spaces_streaming");
    EXPECT_TRUE(service->snapshot().is_null());
    EXPECT_EQ(installs, 0U);
    // A Space to copy is not a launcher's first Space, and neither is no launcher at all.
    EXPECT_EQ(service->submit_create({request.request_id, "space-a", "Player 2", ""}).status, 400);
    EXPECT_EQ(service->submit_create({"not-a-uuid", "", "Player 2", "heroic"}).status, 400);
    facts.streaming = false;
    EXPECT_EQ(service->submit_create(request).status, 202);
    EXPECT_EQ(settled()["state"], "done");
  }

  TEST_F(SpacesMoveService, AFailedOrUnapprovedDownloadNeverTouchesTheSpace) {
    install_answer = {false, "download_incomplete", "The runtime download did not finish. Retry the same runtime to reuse verified layers.", {}};
    ASSERT_EQ(service->submit(request).status, 202);
    auto job = settled();
    EXPECT_EQ(job["state"], "failed");
    EXPECT_EQ(job["code"], "download_incomplete");
    EXPECT_NE(job["message"].get<std::string>().find("The Space was not changed."), std::string::npos);
    EXPECT_EQ(moves, 0U);
    // An image the catalog does not approve for this runtime is never pinned.
    install_answer = {true, "runtime_ready", "", nvidia610.config_digest};
    ASSERT_EQ(service->submit({"32345678-1234-4234-8234-123456789abc", "space-a", "steam-nvidia-615"}).status, 202);
    job = settled();
    EXPECT_EQ(job["state"], "failed");
    EXPECT_EQ(job["code"], "runtime_verification_failed");
    EXPECT_EQ(moves, 0U);
  }

  TEST_F(SpacesMoveService, APendingChangeIsAskedAgainAndARefusalIsReportedInItsWords) {
    move_answers = {{202, "The Space is still being moved. Refresh before retrying.", "spaces_change_pending"},
      {202, "The Space is still being moved. Refresh before retrying.", "spaces_change_pending"}};
    ASSERT_EQ(service->submit(request).status, 202);
    EXPECT_EQ(settled()["state"], "done");
    EXPECT_EQ(moves, 3U);
    for (const auto &move : moved) EXPECT_EQ(move, moved.front()) << "the retry is the same move, so the owner joins it";

    move_answers = {space_runtime_changed_result};
    ASSERT_EQ(service->submit({"42345678-1234-4234-8234-123456789abc", "space-a", "steam-nvidia-615"}).status, 202);
    const auto job = settled();
    EXPECT_EQ(job["state"], "failed");
    EXPECT_EQ(job["code"], "space_runtime_changed");
    EXPECT_EQ(job["message"], std::string(space_runtime_changed_result.message));
    EXPECT_EQ(job["action"], "Refresh Spaces and try again.");
  }

  TEST_F(SpacesMoveService, StoppingPolarisMidDownloadLeavesTheSpaceAsItWas) {
    hold_install = true;
    ASSERT_EQ(service->submit(request).status, 202);
    for (int i = 0; i < 200 && installs == 0; ++i) std::this_thread::sleep_for(5ms);
    ASSERT_EQ(installs, 1U);
    service->shutdown();
    const auto job = service->snapshot();
    EXPECT_EQ(job["state"], "failed");
    EXPECT_EQ(job["code"], "download_cancelled");
    EXPECT_EQ(moves, 0U);
    EXPECT_FALSE(service->active());
    EXPECT_EQ(service->submit({"52345678-1234-4234-8234-123456789abc", "space-a", "steam-nvidia-615"}).code, "spaces_stopping");
  }
}  // namespace

TEST(SpacesRuntimeMove, ABorrowingImageNeverMismatchesTheLoadedDriver) {
  spaces::runtime_t borrowing {"steam-nvidia-host", "nvidia-host", std::string(40, 'a'),
    "sha256:" + std::string(64, '7'), "sha256:" + std::string(64, '8'), ""};
  borrowing.nvidia_minimum_driver = "570.00";
  spaces::runtime_t baked {"steam-nvidia-610", "nvidia", std::string(40, 'a'),
    "sha256:" + std::string(64, '1'), "sha256:" + std::string(64, '2'), "610.57.04"};

  const auto borrowed_identity = spaces::catalog_image_runtime(borrowing.config_digest, {borrowing});
  ASSERT_TRUE(borrowed_identity);
  EXPECT_EQ(borrowed_identity->nvidia_source, "host");
  EXPECT_FALSE(spaces::driver_mismatch(*borrowed_identity, std::string {"615.71.09"}))
    << "an image with no driver of its own cannot be built for another one";

  const auto baked_identity = spaces::catalog_image_runtime(baked.config_digest, {baked});
  ASSERT_TRUE(baked_identity);
  EXPECT_TRUE(spaces::driver_mismatch(*baked_identity, std::string {"615.71.09"}));
}

#endif
