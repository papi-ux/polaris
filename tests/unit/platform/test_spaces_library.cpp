#include "src/platform/linux/spaces_library.h"
#include <gtest/gtest.h>
#include "src/platform/linux/multiseat_container_host.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
  using namespace multiseat::spaces;
  TEST(SpacesLibrary, IdentityBindsTheSpaceAndBoundedSteamTarget) {
    const auto id = game_identity("Alex_1", "3527290");
    EXPECT_EQ(id, "space.Alex_1.3527290");
    EXPECT_EQ(parse_game_identity(id), (game_identity_t{"Alex_1", "3527290"}));
    EXPECT_TRUE(parse_game_identity("space.Alex_1.big-picture-v1"));
    for (const auto value : {"space.other.0", "space.a.01", "space.a.4294967296", "space.a.-1", "space.a.1;cmd",
                            "space.a.1.extra", "space...1", "space.a./tmp/game", "space.a.1\n"})
      EXPECT_FALSE(parse_game_identity(value)) << value;
    EXPECT_TRUE(game_identity("other/home", "10").empty());
  }
  TEST(SpacesLibrary, CatalogRequiresUniqueInstalledTitleIdentities) {
    const auto catalog = decode_library(R"({"schema":1,"games":[{"target":"3527290","name":"PEAK"}]})");
    ASSERT_TRUE(catalog);
    EXPECT_TRUE(catalog->available);
    ASSERT_EQ(catalog->games.size(), 1U);
    EXPECT_EQ(catalog->games[0], (library_game_t{"3527290", "PEAK"}));
    for (const auto value : {
        R"({"schema":1,"games":[{"target":"1","name":"A"},{"target":"1","name":"B"}]})",
        R"({"schema":1,"games":[{"target":"1","target":"2","name":"A"}]})",
        R"({"schema":1,"games":[{"target":"1","name":"A\nB"}]})",
        R"({"schema":1,"games":[{"target":"big-picture-v1","name":"Fake Launcher"}]})",
        R"({"schema":1,"games":[{"target":"1","name":"A","path":"/host/private"}]})",
        R"({"schema":1,"games":[],"games":[]})"}) EXPECT_FALSE(decode_library(value)) << value;
  }
  TEST(SpacesLibrary, ScannerReadsInstalledGamesWithoutFollowingHomeOrManifestLinks) {
    char pattern[] = "/tmp/polaris-library-scanner-XXXXXX";
    const auto created = ::mkdtemp(pattern); ASSERT_NE(created, nullptr);
    const std::filesystem::path root = created, steam = root / ".steam/debian-installation/steamapps";
    struct cleanup_t { std::filesystem::path root; ~cleanup_t() { std::filesystem::remove_all(root); } } cleanup{root};
    std::filesystem::create_directories(steam / "common/Control");
    auto manifest = [&](std::string id, std::string title, std::string install, std::string state = "4") {
      std::ofstream(steam / ("appmanifest_" + id + ".acf")) <<
        "\"AppState\" { \"appid\" \"" << id << "\" \"name\" \"" << title <<
        "\" \"StateFlags\" \"" << state << "\" \"installdir\" \"" << install << "\" }";
    };
    manifest("870780", "Control", "Control");
    manifest("10", "Incomplete", "Control", "2");
    manifest("11", "Proton 10", "Control");
    manifest("12", "Missing Files", "Missing");
    manifest("13", "Escape", "../../..");
    std::filesystem::create_directory_symlink(root, steam / "common/Linked");
    manifest("14", "Linked Install", "Linked");
    std::ofstream(root / "outside.acf") << "private sentinel";
    std::filesystem::create_symlink(root / "outside.acf", steam / "appmanifest_15.acf");
    ASSERT_EQ(::mkfifo((steam / "appmanifest_16.acf").c_str(), 0600), 0);
    auto source = std::string(steam_library_scanner());
    // Substitute only the fixed mount point with the fixture directory. The
    // scanner, parser, and all descriptor-relative NOFOLLOW operations are real.
    source.replace(source.find("'/profile'"), 10, "'" + root.string() + "'");
    const auto script = root / "scanner.py", output = root / "result.json";
    std::ofstream(script) << source;
    const auto pid = ::fork(); ASSERT_GE(pid, 0);
    if (pid == 0) {
      const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) _exit(126);
      ::close(fd); ::execl("/usr/bin/python3", "python3", "-I", script.c_str(), nullptr); _exit(127);
    }
    int status = 0; ASSERT_EQ(::waitpid(pid, &status, 0), pid); ASSERT_TRUE(WIFEXITED(status)); ASSERT_EQ(WEXITSTATUS(status), 0);
    std::ifstream input(output); const std::string payload((std::istreambuf_iterator<char>(input)), {});
    const auto result = decode_library(payload); ASSERT_TRUE(result); ASSERT_EQ(result->games.size(), 1U);
    EXPECT_EQ(result->games[0], (library_game_t{"870780", "Control"}));
  }

  TEST(SpacesLibraryPhysical, ReadsAnExplicitlySelectedRetainedSteamHomeWithoutStartingAWorker) {
    const auto profile = std::getenv("POLARIS_TEST_LIBRARY_PROFILE");
    const auto volume = std::getenv("POLARIS_TEST_LIBRARY_VOLUME");
    const auto image = std::getenv("POLARIS_TEST_LIBRARY_IMAGE");
    if (!profile || !volume || !image) GTEST_SKIP() << "Opt-in read-only Steam home check";
    multiseat::container::local_host_t host;
    const auto result = read_steam_library(host, {profile, volume, multiseat::runtime_profile_e::steam, image});
    ASSERT_TRUE(result.available);
    RecordProperty("installed_titles", std::to_string(result.games.size()));
    if (const auto expected = std::getenv("POLARIS_TEST_LIBRARY_APPID")) {
      EXPECT_TRUE(std::any_of(result.games.begin(), result.games.end(), [&](const auto &game) { return game.target == expected; }));
    }
  }

}
