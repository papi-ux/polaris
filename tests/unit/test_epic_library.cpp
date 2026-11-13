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
#ifndef _WIN32
  std::filesystem::path flatpak_fixture(const std::filesystem::path &home, const std::filesystem::path &location,
                                       const std::string &commit = std::string(64, 'a')) {
    const auto app_home = home / ".var/app/com.epicgames.launcher";
    const auto dir = app_home / "data/Games/A Game";
    auto r = row(); r["dir"] = dir.string();
    write(app_home / "data/com.epicgames.eglv2/installations.jsonl", installed_log({r}));
    const auto ref = location / "app/com.epicgames.launcher/x86_64/stable";
    write(ref / commit / "metadata", "[Application]\nname=com.epicgames.launcher\nruntime=org.gnome.Platform/x86_64/51\ncommand=EpicGamesLauncher\n");
    write(ref / commit / "files/bin/EpicGamesLauncher", std::string("\177ELF", 4));
    std::filesystem::permissions(ref / commit / "files/bin/EpicGamesLauncher", std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    std::filesystem::create_directories(dir);
    std::filesystem::remove(ref / "active");
    std::filesystem::create_symlink(commit, ref / "active");
    return ref;
  }
#endif
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

TEST(EpicLibraryTests, OwnerBlockerJournalCarriesTheNamePassReasonButNeverTheName) {
  // #588: the journal says why the name was left out, as a fixed code. The name itself stays on the host console.
  std::ifstream stream(std::filesystem::path(POLARIS_SOURCE_DIR) / "src/process.cpp");
  ASSERT_TRUE(stream.good());
  const std::string source {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  const auto census = source.find("launcher_instances(reason, proof, &owner_blocker_label, &owner_blocker_reason)");
  ASSERT_NE(census, std::string::npos);
  EXPECT_NE(source.find("<< owner_blocker_reason", census), std::string::npos);
  EXPECT_EQ(source.find("<< owner_blocker_label"), std::string::npos);
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
TEST(EpicLibraryTests, FlatpakDispatchAndStatusHaveNoNativeAdoptionOrGenericEndFallback) {
  std::ifstream file(std::filesystem::path(POLARIS_SOURCE_DIR) / "src/process.cpp");
  ASSERT_TRUE(file.good());
  const std::string source {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const auto admission = source.find("if (const auto refusal = admit_private_epic(*epic_title");
  const auto effects = source.find("terminate_impl(false, false);", admission);
  ASSERT_NE(admission, std::string::npos); ASSERT_NE(effects, std::string::npos);
  EXPECT_LT(admission, effects);
  const auto control = source.find("proc_t::epic_session_control(");
  const auto flatpak = source.find("if (_epic_title->flatpak)", control);
  const auto native = source.find("platf::epic::observe(target)", control);
  ASSERT_NE(flatpak, std::string::npos); ASSERT_NE(native, std::string::npos);
  EXPECT_LT(flatpak, native);
  const auto refused = source.substr(flatpak, source.find("if (_epic_private_requested)", flatpak) - flatpak);
  EXPECT_NE(refused.find("return {200"), std::string::npos);
  EXPECT_NE(refused.find("{\"can_adopt\", false}"), std::string::npos);
  EXPECT_EQ(refused.find("platf::epic::observe"), std::string::npos);
  EXPECT_EQ(refused.find("platf::epic::adopt"), std::string::npos);
  EXPECT_NE(refused.find("End ends the stream and leaves the game and launcher running"), std::string::npos);
  const auto check = source.find("!platf::epic::verify_flatpak_desktop(*epic_flatpak_desktop, reason)");
  const auto spawn = source.find("auto child = platf::run_command(_app.elevated, true, cmd", check);
  ASSERT_NE(check, std::string::npos); ASSERT_NE(spawn, std::string::npos); EXPECT_LT(check, spawn);
  const auto end = source.find("void proc_t::terminate_isolated_session_generation()");
  const auto no_generic = source.find("if (_app.source == \"epic_launcher\" && !_epic_private_requested)", end);
  const auto returns = source.find("return;", no_generic);
  const auto generic = source.find("const bool apps_stopped_first", end);
  ASSERT_NE(no_generic, std::string::npos); ASSERT_NE(returns, std::string::npos); ASSERT_NE(generic, std::string::npos);
  EXPECT_LT(returns, generic);
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
TEST(EpicLibraryTests, ScanFindsCompletedEpicFlatpakInItsAppPrivateDataDirectory) {
  const auto home = std::filesystem::canonical(root("flatpak-original-regression"));
  const auto app_home = home / ".var/app/com.epicgames.launcher";
  auto r = row();
  r["dir"] = (app_home / "data/Games/Alan Wake 2").string();
  write(app_home / "data/com.epicgames.eglv2/installations.jsonl", installed_log({r}));
  const auto ref = home / ".local/share/flatpak/app/com.epicgames.launcher/x86_64/stable";
  const std::string commit(64, 'a');
  write(ref / commit / "metadata", "[Application]\nname=com.epicgames.launcher\nruntime=org.gnome.Platform/x86_64/51\ncommand=EpicGamesLauncher\n");
  write(ref / commit / "files/bin/EpicGamesLauncher", std::string("\177ELF", 4));
  std::filesystem::permissions(ref / commit / "files/bin/EpicGamesLauncher", std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
  std::filesystem::create_directories(app_home / "data/Games/Alan Wake 2");
  std::filesystem::create_symlink(commit, ref / "active");
  const auto found = epic_library::scan({home});
  ASSERT_TRUE(found.available);
  ASSERT_EQ(found.games.size(), 1);
  EXPECT_EQ(found.games[0].identity, title);
  EXPECT_EQ(found.games[0].data_root, app_home / "data/com.epicgames.eglv2");
}
TEST(EpicLibraryTests, FlatpakScopeAndDeploymentBindTheQuotedCommand) {
  for (bool system : {false, true}) {
    const auto home = std::filesystem::canonical(root(system ? "flatpak-system" : "flatpak-user"));
    const auto location = home / (system ? "system-flatpak" : ".local/share/flatpak");
    flatpak_fixture(home, location);
    const auto s = epic_library::scan({home}, home / "system-flatpak");
    ASSERT_TRUE(s.available); ASSERT_EQ(s.games.size(), 1); ASSERT_TRUE(s.games[0].flatpak);
    const auto &d = *s.games[0].flatpak;
    EXPECT_EQ(d.scope, system ? "system" : "user");
    EXPECT_EQ(d.installation, system ? "default" : "");
    EXPECT_EQ(d.arch, "x86_64"); EXPECT_EQ(d.branch, "stable"); EXPECT_EQ(d.commit, std::string(64, 'a'));
    EXPECT_TRUE(epic_library::verify_flatpak_deployment(d));
    const auto command = epic_library::flatpak_launch_command(d, title);
    EXPECT_TRUE(command.starts_with("env HOME='" + home.string() + "' "));
    EXPECT_NE(command.find(system ? "--installation=default" : "--user"), std::string::npos);
    EXPECT_NE(command.find("'--commit=" + d.commit + "'"), std::string::npos);
    EXPECT_TRUE(command.ends_with("'" + epic_library::launch_uri(title) + "'"));
    EXPECT_EQ(command.find("/app/bin"), std::string::npos);
    EXPECT_EQ(command.find("xdg-open"), std::string::npos);
  }
}
TEST(EpicLibraryTests, EmptyFlatpakScanCannotBeMistakenForNativeSetupMetadata) {
  const auto home = std::filesystem::canonical(root("flatpak-empty"));
  flatpak_fixture(home, home / ".local/share/flatpak");
  write(home / ".var/app/com.epicgames.launcher/data/com.epicgames.eglv2/installations.jsonl", installed_log({}));
  const auto s = epic_library::scan({home});
  EXPECT_TRUE(s.available); EXPECT_TRUE(s.games.empty()); EXPECT_TRUE(s.flatpak_only);
  auto native = s; native.flatpak_only = false;
  EXPECT_FALSE(epic_library::same_snapshot(s, native));
}
TEST(EpicLibraryTests, FlatpakMissingInstalledStateNeverRecoversPrevOrAnOldNativeInstallation) {
  const auto home = std::filesystem::canonical(root("flatpak-no-state"));
  flatpak_fixture(home, home / ".local/share/flatpak");
  const auto state = home / ".var/app/com.epicgames.launcher/data/com.epicgames.eglv2/installations.jsonl";
  std::filesystem::rename(state, state.string() + ".prev");
  write(home / ".local/share/com.epicgames.eglv2/installations.jsonl", installed_log({row(),
    {{"installationId", "install1"}, {"deleted", true}, {"writtenAt", "now"}}}));
  const auto absent = epic_library::scan({home});
  EXPECT_FALSE(absent.available); EXPECT_TRUE(absent.games.empty());
  auto incomplete = row(); incomplete["complete"] = false;
  incomplete["dir"] = (home / ".var/app/com.epicgames.launcher/data/Games/A Game").string();
  write(state, installed_log({incomplete}));
  const auto pending = epic_library::scan({home});
  EXPECT_TRUE(pending.available); EXPECT_TRUE(pending.games.empty()); EXPECT_FALSE(pending.warnings.empty());
}
TEST(EpicLibraryTests, FlatpakDuplicateScopesAndRefsNeverPickAnInstallation) {
  const auto home = std::filesystem::canonical(root("flatpak-ambiguous"));
  const auto user = home / ".local/share/flatpak", system = home / "system-flatpak";
  const auto ref = flatpak_fixture(home, user);
  flatpak_fixture(home, system);
  EXPECT_FALSE(epic_library::scan({home}, system).available);
  std::filesystem::remove_all(system);
  const auto beta = ref.parent_path() / "beta";
  std::filesystem::copy(ref, beta, std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks);
  EXPECT_FALSE(epic_library::scan({home}, system).available);
}
TEST(EpicLibraryTests, FlatpakUpdateInvalidatesTheWholeSnapshotAndRetainedDeployment) {
  const auto home = std::filesystem::canonical(root("flatpak-update"));
  const auto user = home / ".local/share/flatpak";
  flatpak_fixture(home, user);
  const auto before = epic_library::scan({home}); ASSERT_EQ(before.games.size(), 1);
  const auto retained = *before.games[0].flatpak;
  flatpak_fixture(home, user, std::string(64, 'b'));
  const auto after = epic_library::scan({home}); ASSERT_EQ(after.games.size(), 1);
  EXPECT_FALSE(epic_library::same_snapshot(before, after));
  EXPECT_FALSE(epic_library::verify_flatpak_deployment(retained));
  EXPECT_TRUE(epic_library::verify_flatpak_deployment(*after.games[0].flatpak));
  auto changed = after;
  changed.games[0].flatpak->scope = "system";
  EXPECT_FALSE(epic_library::same_snapshot(after, changed));
}
TEST(EpicLibraryTests, FlatpakMissingUnreadableRedirectedAndWrongDeploymentStayUnavailable) {
  for (int failure = 0; failure < 6; ++failure) {
    const auto home = std::filesystem::canonical(root(("flatpak-deploy-failure-" + std::to_string(failure)).c_str()));
    const auto ref = flatpak_fixture(home, home / ".local/share/flatpak");
    const auto dir = ref / std::string(64, 'a');
    switch (failure) {
      case 0: std::filesystem::remove(ref / "active"); break;
      case 1: std::filesystem::remove(dir / "metadata"); break;
      case 2: write(dir / "metadata", "[Application]\nname=another.app\ncommand=EpicGamesLauncher\n"); break;
      case 3: std::filesystem::permissions(dir / "files/bin/EpicGamesLauncher", std::filesystem::perms::group_write, std::filesystem::perm_options::add); break;
      case 4: std::filesystem::permissions(dir / "metadata", std::filesystem::perms::none); break;
      case 5: std::filesystem::remove(ref / "active"); std::filesystem::create_symlink("../elsewhere", ref / "active"); break;
    }
    EXPECT_FALSE(epic_library::scan({home}).available) << failure;
    if (failure == 4) std::filesystem::permissions(dir / "metadata", std::filesystem::perms::owner_read);
  }
}
TEST(EpicLibraryTests, FlatpakNeverGuessesPortalOrSandboxOrRedirectedGameMappings) {
  for (const auto *path : {"/app/Games/A", "/run/user/1000/doc/abcdef/A", "/games/A", "/var/data/Games/A"}) {
    const auto home = std::filesystem::canonical(root("flatpak-unmapped"));
    flatpak_fixture(home, home / ".local/share/flatpak");
    auto r = row(); r["dir"] = path;
    write(home / ".var/app/com.epicgames.launcher/data/com.epicgames.eglv2/installations.jsonl", installed_log({r}));
    const auto s = epic_library::scan({home});
    EXPECT_TRUE(s.available); EXPECT_TRUE(s.games.empty()); EXPECT_FALSE(s.warnings.empty());
  }
  const auto home = std::filesystem::canonical(root("flatpak-game-symlink"));
  flatpak_fixture(home, home / ".local/share/flatpak");
  const auto dir = home / ".var/app/com.epicgames.launcher/data/Games/A Game";
  std::filesystem::remove(dir); std::filesystem::create_directories(home / "elsewhere");
  std::filesystem::create_directory_symlink(home / "elsewhere", dir);
  EXPECT_TRUE(epic_library::scan({home}).games.empty());
}
TEST(EpicLibraryTests, NativeAndFlatpakKeepExactTitleDedupAndNativeCommands) {
  const auto home = std::filesystem::canonical(root("flatpak-native-dedup"));
  flatpak_fixture(home, home / ".local/share/flatpak");
  const auto native = home / ".local/share/com.epicgames.eglv2/installations.jsonl";
  write(native, installed_log({row()}));
  EXPECT_FALSE(epic_library::scan({home}).available);
  auto other = row(); other["artifactId"] = "another"; other["dir"] = "/games/A Game";
  write(native, installed_log({other}));
  const auto s = epic_library::scan({home}); ASSERT_TRUE(s.available); ASSERT_EQ(s.games.size(), 2);
  EXPECT_EQ(s.games[0].name, s.games[1].name); // Names alone never merge installations.
  EXPECT_FALSE(epic_library::find(s, {"sandbox", "catalog", "another"})->flatpak);
  EXPECT_TRUE(epic_library::find(s, title)->flatpak);
  EXPECT_EQ(epic_library::launch_command("/usr/bin/EpicGamesLauncher", title), "'/usr/bin/EpicGamesLauncher' '" + epic_library::launch_uri(title) + "'");
}
TEST(EpicLibraryTests, FlatpakTitleStillUsesOnlyExactHeroicCatalogIdentityForProviderDedupHints) {
  const auto home = std::filesystem::canonical(root("flatpak-heroic-identity"));
  flatpak_fixture(home, home / ".local/share/flatpak");
  const auto catalog = home / ".config/heroic/legendaryConfig/legendary/metadata/artifact.json";
  json metadata {{"app_name", "artifact"}, {"metadata", {{"namespace", "sandbox"}, {"id", "catalog"}, {"title", "Actual title"}}}};
  write(catalog, metadata.dump());
  auto s = epic_library::scan({home}); ASSERT_EQ(s.games.size(), 1);
  EXPECT_EQ(s.games[0].name, "Actual title"); EXPECT_TRUE(s.games[0].flatpak);
  metadata["metadata"]["id"] = "different";
  write(catalog, metadata.dump());
  s = epic_library::scan({home}); ASSERT_EQ(s.games.size(), 1);
  EXPECT_EQ(s.games[0].name, "A Game");
}
TEST(EpicLibraryTests, FlatpakInstanceInfoMustMatchEveryDeploymentAndAppPrivatePath) {
  const auto home = std::filesystem::canonical(root("flatpak-info"));
  flatpak_fixture(home, home / ".local/share/flatpak");
  const auto s = epic_library::scan({home}); ASSERT_EQ(s.games.size(), 1);
  const auto d = *s.games[0].flatpak;
  const std::string info = "[Application]\nname=com.epicgames.launcher\n[Instance]\ninstance-id=123456\narch=x86_64\nbranch=stable\napp-commit=" + d.commit +
    "\napp-path=" + (d.directory / "files").string() + "\ninstance-path=" + d.app_home.string() + '\n';
  ASSERT_EQ(epic_library::flatpak_instance_id(info, d), "123456");
  for (const auto &[old, replacement] : std::vector<std::pair<std::string, std::string>> {
      {"name=com.epicgames.launcher", "name=other.app"}, {"arch=x86_64", "arch=aarch64"}, {"branch=stable", "branch=beta"},
      {d.commit, std::string(64, 'b')}, {d.directory.string(), "/another/scope"}, {d.app_home.string(), "/another/player"}}) {
    auto raw = info; raw.replace(raw.find(old), old.size(), replacement);
    EXPECT_FALSE(epic_library::flatpak_instance_id(raw, d)) << old;
  }
  for (const auto *extra : {"app-commit=duplicate\n", "sandbox=true\n", "build=true\n", "original-app-path=/other\n"})
    EXPECT_FALSE(epic_library::flatpak_instance_id(info + extra, d));
  auto bad = d; bad.branch = "stable;touch";
  EXPECT_THROW(epic_library::flatpak_launch_command(bad, title), std::invalid_argument);
  bad = d; bad.installation = "other";
  EXPECT_THROW(epic_library::flatpak_launch_command(bad, title), std::invalid_argument);
  EXPECT_THROW(epic_library::flatpak_launch_command(d, {"sandbox", "catalog", "a&action=uninstall"}), std::invalid_argument);
}
#endif
