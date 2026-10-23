#include <gtest/gtest.h>
#include "../tests_paths.h"
#include <src/epic_library.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#ifndef _WIN32
  #include <sys/stat.h>
#endif

namespace {
  using nlohmann::json;
  json row() {
    return {{"installationId", "install1"}, {"namespace", "sandbox"}, {"itemId", "catalog"},
      {"artifactId", "artifact"}, {"dir", "/games/Alan Wake 2"}, {"platform", "Windows"},
      {"launchExecutable", "bin/Game.exe"}, {"complete", true}, {"version", "1"}};
  }
  std::string installed_log(const std::vector<json> &rows) {
    std::string raw = "{\"version\":1,\"entries\":0}\n";
    for (const auto &r : rows) raw += r.dump() + '\n';
    return raw;
  }
  epic_library::snapshot_t parse(const std::vector<json> &rows) {
    return epic_library::parse_installations(installed_log(rows), "/home/player/.local/share/com.epicgames.eglv2");
  }
  const epic_library::identity_t title {"sandbox", "catalog", "artifact"};
  std::filesystem::path root(const char *suffix) {
    auto p = test_paths::root() / "epic_library" / suffix;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p;
  }
  void write(const std::filesystem::path &p, const std::string &raw) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p); f << raw;
  }
}

TEST(EpicLibraryTests, CompletedInstallUsesExactTripleAndDirectoryNameWithoutAccountData) {
  const auto s = parse({row()});
  ASSERT_TRUE(s.available); ASSERT_EQ(s.games.size(), 1);
  EXPECT_EQ(s.games[0].identity, title);
  EXPECT_EQ(s.games[0].name, "Alan Wake 2");
  EXPECT_EQ(s.games[0].platform, "Windows");
}
TEST(EpicLibraryTests, OwnerControlRequiresExactConfirmationAndSessionRevision) {
  const auto request = epic_library::parse_control_request(R"({"action":"adopt","session_id":"session","revision":"revision","confirm":true})");
  ASSERT_TRUE(request); EXPECT_TRUE(request->confirmed); EXPECT_EQ(request->session_id, "session");
  EXPECT_TRUE(epic_library::parse_control_request(R"({"action":"release","session_id":"session","revision":"revision"})"));
  for (const auto &raw : {R"({"action":"adopt","session_id":"session","revision":"revision","confirm":"true"})",
      R"({"action":"adopt","session_id":"session","revision":"revision","confirm":false})",
      R"({"action":"adopt","session_id":"session","revision":"revision"})",
      R"({"action":"adopt","session_id":"session","revision":"revision","confirm":false,"confirm":true})",
      R"({"action":"release","session_id":"session","revision":""})"})
    EXPECT_FALSE(epic_library::parse_control_request(raw));
}
TEST(EpicLibraryTests, OwnerControlNeverAcceptsBrowserProcessOrPathTargets) {
  const json baseline {{"action", "adopt"}, {"session_id", "session"}, {"revision", "revision"}, {"confirm", true}};
  for (auto key : {"pid", "process", "path", "prefix", "runtime", "signal", "command"}) {
    auto j = baseline; j[key] = "arbitrary target";
    EXPECT_FALSE(epic_library::parse_control_request(j.dump())) << key;
  }
}
TEST(EpicLibraryTests, NewIncompleteRowNeverRevivesEarlierCompletedInstall) {
  auto newer = row(); newer["complete"] = false;
  auto s = parse({row(), newer});
  EXPECT_TRUE(s.available); EXPECT_TRUE(s.games.empty()); EXPECT_FALSE(s.warnings.empty());
}
TEST(EpicLibraryTests, NewInvalidRowStillSupersedesOldInstall) {
  auto newer = row(); newer.erase("artifactId");
  EXPECT_TRUE(parse({row(), newer}).games.empty());
}
TEST(EpicLibraryTests, TombstoneRemovesOnlyItsInstallation) {
  EXPECT_TRUE(parse({row(), {{"installationId", "install1"}, {"deleted", true}, {"writtenAt", "now"}}}).games.empty());
  EXPECT_FALSE(parse({row(), {{"installationId", "install1"}, {"deleted", true}}}).available);
}
TEST(EpicLibraryTests, ConflictingUidOrDiscriminantRefusesSnapshot) {
  auto r = row(); r["uid"] = "other"; EXPECT_FALSE(parse({r}).available);
  r = row(); r["deleted"] = "false"; EXPECT_FALSE(parse({r}).available);
}
TEST(EpicLibraryTests, DuplicateJsonKeyRefusesWholeSnapshot) {
  auto raw = installed_log({row()}); raw += "{\"installationId\":\"install1\",\"complete\":false,\"complete\":true}\n";
  EXPECT_FALSE(epic_library::parse_installations(raw, "/valid/root").available);
}
TEST(EpicLibraryTests, DuplicateTripleOrDirectoryNeverPicksFirstCandidate) {
  auto r = row(); r["installationId"] = "second";
  EXPECT_TRUE(parse({row(), r}).games.empty());
  r["artifactId"] = "different";
  EXPECT_TRUE(parse({row(), r}).games.empty());
}
TEST(EpicLibraryTests, EmptyMalformedOversizedAndUnknownVersionRefuse) {
  for (const auto &raw : {std::string {}, std::string("{\"version\":2,\"entries\":0}\n"),
      std::string("{\"version\":1,\"entries\":false}\n"), installed_log({row()}) + '\n',
      std::string(64 * 1024 + 1, 'x')})
    EXPECT_FALSE(epic_library::parse_installations(raw, "/valid/root").available);
}
TEST(EpicLibraryTests, UnsafePathsIdentityAndCustomLaunchAreUnavailable) {
  for (const auto &[field, val] : std::vector<std::pair<std::string, std::string>> {
      {"dir", "/"}, {"dir", "//games/share"}, {"dir", "/games/../other"}, {"dir", "/games/\nother"},
      {"launchExecutable", "../other.exe"}, {"launchExecutable", "/other.exe"},
      {"artifactId", "a;command"}, {"namespace", "a/b"}, {"launchCommand", "another-game"}}) {
    auto r = row(); r[field] = val; EXPECT_TRUE(parse({r}).games.empty()) << field << ':' << val;
  }
}
TEST(EpicLibraryTests, PlatformOrPendingUpdateDoesNotGuessRuntime) {
  auto r = row(); r["platform"] = "Linux"; EXPECT_TRUE(parse({r}).games.empty());
  r = row(); r["pendingVersion"] = "2"; EXPECT_TRUE(parse({r}).games.empty());
}
TEST(EpicLibraryTests, UriEncodesSeparatorsAndRejectsInjectedIdentifiers) {
  EXPECT_EQ(epic_library::launch_uri(title), "com.epicgames.launcher://apps/sandbox%3Acatalog%3Aartifact?action=launch&silent=true");
  EXPECT_THROW(epic_library::launch_uri({"sandbox", "catalog", "artifact&action=uninstall"}), std::invalid_argument);
  EXPECT_EQ(epic_library::launch_command("/home/player/Epic's App.AppImage", title),
    "'/home/player/Epic'\\''s App.AppImage' 'com.epicgames.launcher://apps/sandbox%3Acatalog%3Aartifact?action=launch&silent=true'");
}
TEST(EpicLibraryTests, CatalogMatchesAllThreeIdentifiersNeverNameAlone) {
  // Legendary's actual file wraps the catalog record in metadata; app_name is its artifact.
  json catalog {{"app_name", "artifact"}, {"metadata", {{"namespace", "sandbox"}, {"id", "catalog"}, {"title", "Actual title"}}}};
  std::string name;
  EXPECT_TRUE(epic_library::matches_heroic_catalog(title, catalog.dump(), &name)); EXPECT_EQ(name, "Actual title");
  for (auto key : {"namespace", "id", "app_name"}) {
    auto j = catalog;
    if (std::string(key) == "app_name") j[key] = "different";
    else j["metadata"][key] = "different";
    EXPECT_FALSE(epic_library::matches_heroic_catalog(title, j.dump()));
  }
}
#ifndef _WIN32
TEST(EpicLibraryTests, ReaderRefusesSymlinkAndDoesNotRecoverPrevFile) {
  const auto p = root("nofollow");
  write(p / "installations.jsonl.prev", installed_log({row()}));
  EXPECT_FALSE(epic_library::read_installations(p).available);
  std::filesystem::create_symlink(p / "installations.jsonl.prev", p / "installations.jsonl");
  EXPECT_FALSE(epic_library::read_installations(p).available);
  std::filesystem::remove(p / "installations.jsonl");
  write(p / "installations.jsonl", installed_log({row()}));
  EXPECT_TRUE(epic_library::read_installations(p).available);
}
TEST(EpicLibraryTests, ReaderRefusesFifoWithoutBlocking) {
  const auto p = root("fifo");
  ASSERT_EQ(::mkfifo((p / "installations.jsonl").c_str(), 0600), 0);
  EXPECT_FALSE(epic_library::read_installations(p).available);
}
TEST(EpicLibraryTests, ScanDeduplicatesNoTitleByNameAndKeepsUnreadableRootUnavailable) {
  const auto home = root("home"), other = root("other");
  const auto suffix = ".local/share/com.epicgames.eglv2/installations.jsonl";
  write(home / suffix, installed_log({row()}));
  auto s = epic_library::scan({home}); ASSERT_TRUE(s.available); ASSERT_EQ(s.games.size(), 1);
  write(other / suffix, installed_log({row()}));
  s = epic_library::scan({home, other}); EXPECT_FALSE(s.available); EXPECT_TRUE(s.games.empty());
  write(other / suffix, "bad");
  EXPECT_FALSE(epic_library::scan({home, other}).available);
}
#endif
