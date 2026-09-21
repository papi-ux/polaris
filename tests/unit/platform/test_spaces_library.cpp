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
  /**
   * An identity carries the Space and its target, and a target belongs to a
   * launcher family. Encoding one against Steam's grammar alone made every
   * Heroic and Lutris entry come out empty, which a client reads as a library
   * it cannot verify at all.
   */
  TEST(SpacesLibrary, IdentityCarriesEveryLauncherFamilysTarget) {
    for (const auto *target : {"library-v1", "epic.AlanWake2", "gog.1207658924", "amazon.Larch", "id.42"}) {
      const auto id = game_identity("Alex_1", target);
      EXPECT_EQ(id, std::string("space.Alex_1.") + target) << target;
      EXPECT_EQ(parse_game_identity(id), (game_identity_t{"Alex_1", target})) << target;
    }
    // A target no launcher would accept still has no identity.
    for (const auto *target : {"store.Thing", "epic.", "id.", "library-v2", "epic.bad name"})
      EXPECT_TRUE(game_identity("Alex_1", target).empty()) << target;
  }
  /**
   * A scanner lists titles in its own family's grammar. Decoding every library
   * against Steam's would have emptied a Heroic one the moment a game was
   * installed in it.
   */
  TEST(SpacesLibrary, DecodesALibraryInItsOwnFamilysGrammar) {
    using multiseat::runtime_profile_e;
    const auto heroic = decode_library(
      R"({"schema":1,"games":[{"target":"epic.AlanWake2","name":"Alan Wake 2"},{"target":"gog.1207658924","name":"Witcher"}]})",
      runtime_profile_e::heroic);
    ASSERT_TRUE(heroic);
    ASSERT_EQ(heroic->games.size(), 2U);
    EXPECT_EQ(heroic->games[0], (library_game_t{"epic.AlanWake2", "Alan Wake 2"}));

    // The same payload is not a Steam library, and a Steam one is not Heroic's.
    EXPECT_FALSE(decode_library(R"({"schema":1,"games":[{"target":"epic.AlanWake2","name":"Alan Wake 2"}]})"));
    EXPECT_FALSE(decode_library(R"({"schema":1,"games":[{"target":"3527290","name":"PEAK"}]})", runtime_profile_e::heroic));
    // No scanner lists the tile that opens the launcher itself.
    EXPECT_FALSE(decode_library(R"({"schema":1,"games":[{"target":"library-v1","name":"Heroic"}]})", runtime_profile_e::heroic));
    EXPECT_FALSE(decode_library(R"({"schema":1,"games":[{"target":"big-picture-v1","name":"Steam"}]})"));
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

  // Runs one Python file the way the library helper does and returns what it printed.
  std::optional<std::string> run_python(const std::filesystem::path &script, const std::filesystem::path &output) {
    const auto pid = ::fork();
    if (pid < 0) return std::nullopt;
    if (pid == 0) {
      const int fd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0) _exit(126);
      ::close(fd); ::execl("/usr/bin/python3", "python3", "-I", script.c_str(), nullptr); _exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
    std::ifstream input(output);
    return std::string((std::istreambuf_iterator<char>(input)), {});
  }

  // The tables the reader asks, as Lutris 0.5.22 declares them, made by SQLite itself.
  constexpr std::string_view lutris_fixture = R"PY(import sqlite3
c = sqlite3.connect('DATABASE')
c.execute('PRAGMA journal_mode=JOURNAL')
c.execute('CREATE TABLE games (id INTEGER PRIMARY KEY, name TEXT, slug TEXT, runner TEXT, installed INTEGER)')
c.execute('CREATE TABLE categories (id INTEGER PRIMARY KEY, name TEXT UNIQUE)')
c.execute('CREATE TABLE games_categories (game_id INTEGER, category_id INTEGER)')
c.executemany('INSERT INTO games VALUES (?,?,?,?,?)', [
    (3, 'Quake', 'quake', 'linux', 1), (12, 'arx Fatalis', 'arx', 'wine', 1),
    (4, 'Not Installed', 'ni', 'wine', 0), (5, 'Hidden One', 'h', 'wine', 1),
    (6, 'Bad' + chr(7) + 'Bell', 'b', 'wine', 1), (7, '', 'e', 'wine', 1), (8, None, 'n', 'wine', 1),
    (4294967296, 'Too Large', 't', 'wine', 1), (9, 'Favourite', 'f', 'wine', 1)])
c.executemany('INSERT INTO categories VALUES (?,?)', [(1, 'favorite'), (2, '.hidden')])
c.executemany('INSERT INTO games_categories VALUES (?,?)', [(5, 2), (9, 1)])
c.commit()
c.close()
)PY";

  struct lutris_home_t {
    std::filesystem::path root, database;
    lutris_home_t() {
      char pattern[] = "/tmp/polaris-lutris-scanner-XXXXXX";
      if (const auto created = ::mkdtemp(pattern)) {
        root = created; database = root / ".local/share/lutris/pga.db";
        std::filesystem::create_directories(database.parent_path());
      }
    }
    ~lutris_home_t() { if (!root.empty()) std::filesystem::remove_all(root); }
    bool make(const std::string &journal_mode = "DELETE") {
      auto source = std::string(lutris_fixture);
      source.replace(source.find("DATABASE"), 8, database.string());
      source.replace(source.find("JOURNAL"), 7, journal_mode);
      std::ofstream(root / "make.py") << source;
      return run_python(root / "make.py", root / "make.out").has_value();
    }
    std::optional<library_t> scan() {
      auto source = std::string(lutris_library_scanner());
      source.replace(source.find("'/profile'"), 10, "'" + root.string() + "'");
      std::ofstream(root / "scanner.py") << source;
      const auto payload = run_python(root / "scanner.py", root / "result.json");
      if (!payload) return std::nullopt;
      return decode_library(*payload, multiseat::runtime_profile_e::lutris);
    }
  };

  TEST(SpacesLibrary, LutrisScannerListsInstalledVisibleGamesFromItsDatabase) {
    lutris_home_t home;
    ASSERT_TRUE(home.make());
    const auto result = home.scan();
    ASSERT_TRUE(result);
    // Sorted the way a player reads them, whatever case Lutris stored. A
    // favourite is listed, and only the hidden category hides a game. A title
    // that is empty, missing or carries a control character is skipped rather
    // than repaired, and so is a number the target grammar cannot carry.
    const std::vector<library_game_t> expected {{"id.12", "arx Fatalis"}, {"id.9", "Favourite"}, {"id.3", "Quake"}};
    EXPECT_EQ(result->games, expected);
  }

  TEST(SpacesLibrary, LutrisScannerReadsADatabaseLeftInWriteAheadMode) {
    lutris_home_t home;
    ASSERT_TRUE(home.make("WAL"));
    std::ifstream header(home.database, std::ios::binary);
    char bytes[20] {};
    header.read(bytes, sizeof(bytes));
    ASSERT_EQ(bytes[18], 2) << "the fixture is not in write-ahead mode, so this proves nothing";
    const auto result = home.scan();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->games.size(), 3U);
  }

  TEST(SpacesLibrary, LutrisScannerOpensNothingButAPlainDatabaseFile) {
    // The home is the player's, and the helper that reads it must not be led
    // out of it or left waiting on it.
    {
      lutris_home_t home;
      ASSERT_TRUE(home.make());
      const auto elsewhere = home.root / "elsewhere.db";
      std::filesystem::rename(home.database, elsewhere);
      std::filesystem::create_symlink(elsewhere, home.database);
      const auto result = home.scan();
      ASSERT_TRUE(result) << "a linked database is an empty library, not a failure";
      EXPECT_TRUE(result->games.empty());
    }
    {
      lutris_home_t home;
      ASSERT_EQ(::mkfifo(home.database.c_str(), 0600), 0);
      const auto result = home.scan();
      ASSERT_TRUE(result) << "a FIFO must not hold the helper open";
      EXPECT_TRUE(result->games.empty());
    }
    {
      lutris_home_t home;
      ASSERT_TRUE(home.make());
      const auto real = home.root / "real-share";
      std::filesystem::rename(home.root / ".local/share", real);
      std::filesystem::create_directory_symlink(real, home.root / ".local/share");
      const auto result = home.scan();
      ASSERT_TRUE(result);
      EXPECT_TRUE(result->games.empty()) << "a linked directory on the way was followed";
    }
    {
      lutris_home_t home;  // Lutris has never run here.
      const auto result = home.scan();
      ASSERT_TRUE(result);
      EXPECT_TRUE(result->games.empty());
    }
    {
      lutris_home_t home;
      std::ofstream(home.database) << "not a database";
      const auto result = home.scan();
      ASSERT_TRUE(result);
      EXPECT_TRUE(result->games.empty());
    }
  }

  TEST(SpacesLibrary, EveryLauncherFamilyHasAReaderAndNothingElseDoes) {
    using multiseat::runtime_profile_e;
    for (const auto family : {runtime_profile_e::steam, runtime_profile_e::heroic, runtime_profile_e::lutris}) {
      EXPECT_TRUE(has_library(family));
      EXPECT_FALSE(library_scanner(family).empty());
    }
    EXPECT_FALSE(has_library(runtime_profile_e::gamescope));
    EXPECT_NE(library_scanner(runtime_profile_e::lutris), library_scanner(runtime_profile_e::heroic));
    // A Lutris library is read in Lutris's grammar and no other.
    const auto lutris = decode_library(R"({"schema":1,"games":[{"target":"id.42","name":"Quake"}]})", runtime_profile_e::lutris);
    ASSERT_TRUE(lutris);
    EXPECT_EQ(lutris->games[0], (library_game_t{"id.42", "Quake"}));
    EXPECT_FALSE(decode_library(R"({"schema":1,"games":[{"target":"42","name":"Quake"}]})", runtime_profile_e::lutris));
    EXPECT_FALSE(decode_library(R"({"schema":1,"games":[{"target":"id.42","name":"Quake"}]})", runtime_profile_e::heroic));
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
