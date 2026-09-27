/**
 * @file tests/unit/test_launcher_identity.cpp
 * @brief Test the Lutris runner → platform/runtime mapping the library serves.
 */
#include "../tests_common.h"

#include <src/process.h>

TEST(LauncherIdentityTests, RunnersThatDetermineTheAnswerAreMapped) {
  using proc::launcher_identity_from_lutris_runner;

  const auto wine = launcher_identity_from_lutris_runner("wine");
  EXPECT_EQ("windows", wine.platform);
  EXPECT_EQ("wine", wine.runtime);

  const auto proton = launcher_identity_from_lutris_runner("proton");
  EXPECT_EQ("windows", proton.platform);
  EXPECT_EQ("proton", proton.runtime);

  const auto umu = launcher_identity_from_lutris_runner("umu");
  EXPECT_EQ("windows", umu.platform);
  EXPECT_EQ("umu", umu.runtime);

  const auto native = launcher_identity_from_lutris_runner("linux");
  EXPECT_EQ("linux", native.platform);
  EXPECT_EQ("native", native.runtime);
}

TEST(LauncherIdentityTests, TheSteamRunnerNamesARuntimeButNoPlatform) {
  // Lutris' steam runner launches native and Proton titles alike, so claiming
  // a platform would be a guess.
  const auto steam = proc::launcher_identity_from_lutris_runner("steam");
  EXPECT_TRUE(steam.platform.empty());
  EXPECT_EQ("steam", steam.runtime);
}

TEST(LauncherIdentityTests, UnknownStaysEmptyRatherThanGuessed) {
  // Nova renders nothing for an empty value; a wrong badge in the library is
  // worse than a missing one.
  for (const auto runner : {"", "flatpak", "mame", "dosbox", "something-new"}) {
    SCOPED_TRACE(runner);
    const auto identity = proc::launcher_identity_from_lutris_runner(runner);
    EXPECT_TRUE(identity.platform.empty());
    EXPECT_TRUE(identity.runtime.empty());
  }
}

TEST(LauncherIdentityTests, InputIsNormalizedBeforeMapping) {
  const auto identity = proc::launcher_identity_from_lutris_runner("  Wine \n");
  EXPECT_EQ("windows", identity.platform);
  EXPECT_EQ("wine", identity.runtime);
}

TEST(LauncherIdentityTests, EmulatorEntriesNameTheirConsoleAndEmulator) {
  const auto eden = proc::launcher_identity_from_emulator("eden");
  EXPECT_EQ(eden.platform, "switch");
  EXPECT_EQ(eden.platform_label, "Nintendo Switch");
  EXPECT_EQ(eden.runtime, "eden");
  EXPECT_EQ(eden.runtime_label, "Eden");

  const auto duckstation = proc::launcher_identity_from_emulator(" DuckStation ");
  EXPECT_EQ(duckstation.platform, "psx");
  EXPECT_EQ(duckstation.platform_label, "PlayStation");
  EXPECT_EQ(duckstation.runtime, "duckstation");
  EXPECT_EQ(duckstation.runtime_label, "DuckStation");
}

TEST(LauncherIdentityTests, CustomAndUnknownEmulatorsStayHonest) {
  const auto custom = proc::launcher_identity_from_emulator("custom");
  EXPECT_TRUE(custom.platform.empty());
  EXPECT_TRUE(custom.platform_label.empty());
  EXPECT_EQ(custom.runtime, "custom");
  EXPECT_EQ(custom.runtime_label, "Custom emulator");

  for (const auto *unknown : {"", "ryujinx", "  "}) {
    const auto identity = proc::launcher_identity_from_emulator(unknown);
    EXPECT_TRUE(identity.platform.empty()) << unknown;
    EXPECT_TRUE(identity.platform_label.empty()) << unknown;
    EXPECT_TRUE(identity.runtime.empty()) << unknown;
    EXPECT_TRUE(identity.runtime_label.empty()) << unknown;
  }
}

TEST(LauncherIdentityTests, HeroicApiMetadataUsesExactStoredIdentityAndCurrentSettings) {
  game_library::heroic_runtime_snapshot_t snapshot {
    {{game_library::launcher_install_t::native, "epic", "SameId"}, {"linux", "native", ""}},
    {{game_library::launcher_install_t::flatpak, "epic", "SameId"}, {"windows", "proton", "GE-Proton"}},
    {{game_library::launcher_install_t::flatpak, "gog", "SameId"}, {"windows", "wine", "Wine-GE"}},
  };
  proc::ctx_t app {};
  app.source = "heroic";
  app.heroic_app_name = "SameId";
  app.heroic_store = "epic";
  app.heroic_runner = "legendary";
  app.heroic_install = "flatpak";
  EXPECT_EQ(proc::launcher_metadata_for_app(app, snapshot), (nlohmann::json {
    {"platform", "windows"}, {"runtime", "proton"}, {"runtime_label", "GE-Proton"}}));
  app.heroic_install = "native";
  EXPECT_EQ(proc::launcher_metadata_for_app(app, snapshot), (nlohmann::json {
    {"platform", "linux"}, {"runtime", "native"}}));
  app.heroic_install = "flatpak";
  app.heroic_store = "gog";
  EXPECT_TRUE(proc::launcher_metadata_for_app(app, snapshot).empty());
  app.heroic_runner = "gog";
  EXPECT_EQ(proc::launcher_metadata_for_app(app, snapshot).at("runtime"), "wine");
  app.heroic_app_name.clear();
  EXPECT_TRUE(proc::launcher_metadata_for_app(app, snapshot).empty()) << "the Heroic launcher has no game's runtime";
  app.heroic_app_name = "OtherId";
  EXPECT_TRUE(proc::launcher_metadata_for_app(app, snapshot).empty());
  app.heroic_app_name = "../SameId";
  EXPECT_TRUE(proc::launcher_metadata_for_app(app, snapshot).empty());
}

TEST(LauncherIdentityTests, HeroicApiLeavesUnknownFieldsAbsentAndUsesNovaPlatformIds) {
  game_library::heroic_runtime_snapshot_t snapshot {
    {{game_library::launcher_install_t::native, "gog", "123"}, {"mac", "", ""}},
  };
  proc::ctx_t app {};
  app.source = "heroic";
  app.heroic_app_name = "123";
  app.heroic_store = "gog";
  app.heroic_runner = "gog";
  app.heroic_install = "native";
  app.lutris_runner = "wine"; // Unrelated imported fields cannot supply a missing Heroic runtime.
  EXPECT_EQ(proc::launcher_metadata_for_app(app, snapshot), (nlohmann::json {{"platform", "macos"}}));
  EXPECT_TRUE(proc::launcher_metadata_for_app(app, {}).empty());
}

TEST(LauncherIdentityTests, SharedApiMetadataPreservesLutrisEmulatorAndManualFields) {
  proc::ctx_t app {};
  app.source = "lutris";
  app.lutris_runner = "wine";
  EXPECT_EQ(proc::launcher_metadata_for_app(app, {}), (nlohmann::json {
    {"platform", "windows"}, {"runtime", "wine"}}));
  app.source = "emulator";
  app.lutris_runner.clear();
  app.emulator = "eden";
  EXPECT_EQ(proc::launcher_metadata_for_app(app, {}), (nlohmann::json {
    {"platform", "switch"}, {"platform_label", "Nintendo Switch"}, {"runtime", "eden"},
    {"runtime_label", "Eden"}, {"emulator", "eden"}}));
  app.source = "manual";
  EXPECT_TRUE(proc::launcher_metadata_for_app(app, {}).empty());
}
