#include <gtest/gtest.h>
#include "../tests_paths.h"
#include <src/epic_library.h>
#include <src/launch_failure.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>
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
    return epic_library::parse_installations(installed_log(rows), "/fixture/player/.local/share/com.epicgames.eglv2");
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

TEST(EpicLibraryTests, SetupSnapshotBindingUsesAvailableExactMetadataIndependentOfOrder) {
  auto second = row();
  second["artifactId"] = "other";
  second["installationId"] = "install2";
  second["dir"] = "/games/Other"; // Shared directories make both installations ambiguous.
  const auto first = parse({row(), second});
  ASSERT_TRUE(first.available);
  ASSERT_EQ(first.games.size(), 2);
  ASSERT_TRUE(first.warnings.empty());
  EXPECT_TRUE(epic_library::same_snapshot(first, parse({second, row()})));
  // Parsing orders by installation ID, so also reverse the snapshot entries themselves.
  auto reversed = first;
  std::reverse(reversed.games.begin(), reversed.games.end());
  EXPECT_TRUE(epic_library::same_snapshot(first, reversed));
  EXPECT_FALSE(epic_library::same_snapshot(first, parse({row()})));
  EXPECT_TRUE(epic_library::same_snapshot({true, {}, {}}, {true, {}, {}}));
  EXPECT_FALSE(epic_library::same_snapshot({false, {}, {}}, {true, {}, {}}));
  EXPECT_FALSE(epic_library::same_snapshot({false, {}, {}}, {false, {}, {}}));
  // Every field of the selected installation participates; neither a title label nor a
  // client-supplied completion flag may substitute for the server metadata snapshot.
  for (int field = 0; field < 10; ++field) {
    auto changed = first;
    auto &game = changed.games.front();
    switch (field) {
      case 0: game.identity.namespace_id += "x"; break;
      case 1: game.identity.item_id += "x"; break;
      case 2: game.identity.artifact_id += "x"; break;
      case 3: game.installation_id += "x"; break;
      case 4: game.name += "x"; break;
      case 5: game.launch_executable += "x"; break;
      case 6: game.version += "x"; break;
      case 7: game.install_dir /= "changed"; break;
      case 8: game.data_root /= "changed"; break;
      case 9: game.platform = "Linux"; break;
    }
    EXPECT_FALSE(epic_library::same_snapshot(first, changed)) << field;
  }
  auto warned = first;
  warned.warnings.push_back("Metadata needs attention");
  EXPECT_FALSE(epic_library::same_snapshot(first, warned));
}
TEST(EpicLibraryTests, OwnerBlockerMessageNeverChangesTheGenericRefusalOrAction) {
  const std::string generic = "Another AppImage app is running on the host, and Polaris can't tell it apart from Epic.";
  const std::string action = "Quit other AppImage apps on the host, then try Private Stream again, or use Mirror Desktop.";
  const std::string owner = "Orca (an AppImage) is running; quit it to use Private Stream.";
  launch_failure::clear();
  EXPECT_EQ(launch_failure::refuse(503, "epic_other_appimage_running", generic, action, owner), 503);
  const auto refusal = launch_failure::take();
  ASSERT_TRUE(refusal);
  EXPECT_EQ(refusal->status, 503);
  EXPECT_EQ(refusal->code, "epic_other_appimage_running");
  EXPECT_EQ(refusal->message, generic);
  EXPECT_EQ(refusal->action, action);
  EXPECT_EQ(launch_failure::status_message(*refusal), generic + " " + action);
  EXPECT_EQ(launch_failure::authenticated_owner_status_message(*refusal), owner + " " + action);
  EXPECT_EQ(launch_failure::status_message(*refusal).find("Orca"), std::string::npos);
  EXPECT_FALSE(launch_failure::take());
  launch_failure::refuse(503, "unknown", generic, action);
  EXPECT_EQ(launch_failure::authenticated_owner_status_message(*launch_failure::take()), generic + " " + action);
  launch_failure::refuse(503, "unknown", generic, action, "");
  EXPECT_EQ(launch_failure::authenticated_owner_status_message(*launch_failure::take()), generic + " " + action);
}

TEST(EpicLibraryTests, OwnerBlockerMessageIsAttemptLocalAndCannotSurviveReplacementOrAThreadBoundary) {
  launch_failure::refuse(503, "epic_other_appimage_running", "Generic", "", "Orca");
  std::thread other([] {
    EXPECT_FALSE(launch_failure::take());
    launch_failure::refuse(503, "unknown", "Other attempt");
    EXPECT_EQ(launch_failure::authenticated_owner_status_message(*launch_failure::take()), "Other attempt");
  });
  other.join();
  ASSERT_TRUE(launch_failure::pending);
  EXPECT_EQ(launch_failure::authenticated_owner_status_message(*launch_failure::pending), "Orca");
  launch_failure::refuse_if_unexplained(503, "wrapper", "Generic wrapper");
  EXPECT_EQ(launch_failure::authenticated_owner_status_message(*launch_failure::take()), "Orca");
  launch_failure::refuse(503, "epic_other_appimage_running", "Generic", "", "Orca");
  launch_failure::refuse(409, "new_attempt", "Generic new attempt");
  EXPECT_EQ(launch_failure::authenticated_owner_status_message(*launch_failure::take()), "Generic new attempt");
  launch_failure::clear();
}

TEST(EpicLibraryTests, OwnerBlockerAccessorRejectsUnboundedControlOrMarkupText) {
  for (const auto &owner : {std::string(161, 'a'), std::string("Orca\nInjected"), std::string("Orca\033[31m"),
         std::string("<b>Orca</b>"), std::string("Orca & other"), std::string("\"Orca\""), std::string("Orca\302\240")}) {
    launch_failure::refuse(503, "epic_other_appimage_running", "Generic", "Same action", owner);
    const auto refusal = launch_failure::take();
    ASSERT_TRUE(refusal);
    EXPECT_EQ(launch_failure::authenticated_owner_status_message(*refusal), "Generic Same action");
    EXPECT_EQ(launch_failure::status_message(*refusal), "Generic Same action");
  }
}

TEST(EpicLibraryTests, OwnerBlockerAccessorStaysBehindTheConsoleGuardAndOutOfGenericConsumers) {
  const auto source = [](const char *file) {
    std::ifstream stream(std::filesystem::path(POLARIS_SOURCE_DIR) / file);
    EXPECT_TRUE(stream.good()) << file;
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  };
  const auto console = source("src/confighttp.cpp");
  const auto launch = console.find("void launchApp(");
  ASSERT_NE(launch, std::string::npos);
  const auto next_handler = console.find("void disconnect", launch);
  ASSERT_NE(next_handler, std::string::npos);
  const auto guard = console.find("!authenticate(response, request)) {\n      return;", launch);
  const auto owner_read = console.find("launch_failure::authenticated_owner_status_message(*refusal)");
  ASSERT_NE(guard, std::string::npos);
  ASSERT_NE(owner_read, std::string::npos);
  EXPECT_LT(guard, owner_read);
  EXPECT_LT(owner_read, next_handler);
  EXPECT_EQ(console.find("launch_failure::authenticated_owner_status_message", owner_read + 1), std::string::npos);
  for (const auto *file : {"src/nvhttp.cpp", "src/rtsp.cpp", "src/browser_stream.cpp", "src/process.cpp", "src/codec_support_readout.h"}) {
    const auto consumer = source(file);
    EXPECT_EQ(consumer.find("authenticated_owner_status_message"), std::string::npos) << file;
    EXPECT_EQ(consumer.find("->owner_message"), std::string::npos) << file;
    EXPECT_EQ(consumer.find(".owner_message"), std::string::npos) << file;
  }
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
  EXPECT_EQ(epic_library::launch_command("/fixture/player/Epic's App.AppImage", title),
    "'/fixture/player/Epic'\\''s App.AppImage' 'com.epicgames.launcher://apps/sandbox%3Acatalog%3Aartifact?action=launch&silent=true'");
}
TEST(EpicLibraryTests, TheAppImageStageNeverGivesTheRequestToTheMountStep) {
  const auto uri = epic_library::launch_uri(title);
  std::string isolated;
  const auto stage = epic_library::appimage_stage_command("/fixture/My Desk's/Epic.AppImage", "/games/Alan Wake's 2", title, [&](const std::string &inner) {
    isolated = inner;
    return "ISOLATE(" + inner + ")";
  });
  // The request appears once, inside the isolated command and nowhere else.
  EXPECT_EQ(stage.find(uri), stage.rfind(uri));
  EXPECT_EQ(isolated, "cd -- '/games/Alan Wake'\\''s 2' && exec \"$APPDIR/AppRun\" '" + uri + "'");
  EXPECT_LT(stage.find("ISOLATE("), stage.find(uri));
  // The mount step takes no request, and quotes the path whatever it holds.
  const auto mount = stage.substr(0, stage.find('\n', stage.find("--appimage-mount")));
  EXPECT_EQ(mount.find(uri), std::string::npos);
  EXPECT_NE(mount.find("exec '/fixture/My Desk'\\''s/Epic.AppImage' --appimage-mount)"), std::string::npos);
  EXPECT_NE(stage.find("cd -- '/games/Alan Wake'\\''s 2' || exit 127\n"), std::string::npos);
  EXPECT_NE(stage.find("export APPDIR APPIMAGE='/fixture/My Desk'\\''s/Epic.AppImage' ARGV0='/fixture/My Desk'\\''s/Epic.AppImage' OWD=\"$PWD\"\n"), std::string::npos);
  // Fd 9 stays clear of the diagnostics pipes and the identity channel, and the wait stays inside the 30 s birth window.
  EXPECT_NE(stage.find("exec 9< <("), std::string::npos);
  EXPECT_NE(stage.find("read -r -t 10 APPDIR <&9"), std::string::npos);
  const auto keep = [](const std::string &) { return std::string {"bwrap"}; };
  EXPECT_THROW(epic_library::appimage_stage_command("relative.AppImage", "/games/a", title, keep), std::invalid_argument);
  EXPECT_THROW(epic_library::appimage_stage_command("/fixture/a\nb.AppImage", "/games/a", title, keep), std::invalid_argument);
  EXPECT_THROW(epic_library::appimage_stage_command("/fixture/a.AppImage", "relative", title, keep), std::invalid_argument);
  EXPECT_THROW(epic_library::appimage_stage_command("/fixture/a.AppImage", "/games/a", {"bad id", "b", "c"}, keep), std::invalid_argument);
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
