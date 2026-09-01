#include "../tests_common.h"
#include "../tests_paths.h"

#include <src/game_library_scanner.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
  std::filesystem::path lutris_test_root(const std::string &name) {
    const auto root = test_paths::root() / "lutris_scanner" / name;
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
  }

  void write_text(const std::filesystem::path &path, const std::string &content) {
    std::ofstream file(path);
    ASSERT_TRUE(file.good());
    file << content;
  }
}  // namespace

TEST(LutrisLibraryScannerTests, ParsesTopLevelMetadataAndBuildsCanonicalCommand) {
  const auto root = lutris_test_root("metadata");
  const auto config = root / "ignored-filename.yml";
  write_text(config,
    "name: \"Slay the Spire\"\n"
    "slug: slay-the-spire\n"
    "game_slug: ignored-game-slug\n"
    "runner: wine\n"
    "game:\n"
    "  name: Nested name should not win\n");

  const auto game = game_library::parse_lutris_game_config(config);
  ASSERT_TRUE(game.has_value());
  EXPECT_EQ(game->name, "Slay the Spire");
  EXPECT_EQ(game->slug, "slay-the-spire");
  EXPECT_EQ(game->runner, "wine");
  EXPECT_EQ(game->command, "setsid lutris lutris:rungame/slay-the-spire");
}

TEST(LutrisLibraryScannerTests, FallsBackToGameSlugThenFilenameStem) {
  const auto root = lutris_test_root("fallbacks");
  const auto game_slug_config = root / "ignored.yml";
  const auto filename_config = root / "missing-name-game.yml";

  write_text(game_slug_config,
    "name: 'Game Slug Title'\n"
    "game_slug: game-slug-title\n");
  write_text(filename_config, "runner: native\n");

  const auto game_slug = game_library::parse_lutris_game_config(game_slug_config);
  ASSERT_TRUE(game_slug.has_value());
  EXPECT_EQ(game_slug->slug, "game-slug-title");
  EXPECT_EQ(game_slug->name, "Game Slug Title");

  const auto filename = game_library::parse_lutris_game_config(filename_config);
  ASSERT_TRUE(filename.has_value());
  EXPECT_EQ(filename->slug, "missing-name-game");
  EXPECT_EQ(filename->name, "missing name game");
}

TEST(LutrisLibraryScannerTests, ScansDeterministicallyAndSkipsDuplicateSlugs) {
  const auto root = lutris_test_root("scan");
  write_text(root / "b.yml",
    "name: Second Copy\n"
    "slug: duplicate-game\n");
  write_text(root / "a.yml",
    "name: First Copy\n"
    "slug: duplicate-game\n");
  write_text(root / "c.yaml",
    "name: Unique Game # inline comment\n"
    "slug: unique-game\n"
    "runner: linux\n");
  write_text(root / "bad slug.yml", "name: Unsafe Slug\n");

  const auto games = game_library::scan_lutris_games(root);
  ASSERT_EQ(games.size(), 2);
  EXPECT_EQ(games[0].name, "First Copy");
  EXPECT_EQ(games[0].slug, "duplicate-game");
  EXPECT_EQ(games[1].name, "Unique Game");
  EXPECT_EQ(games[1].slug, "unique-game");
  EXPECT_EQ(games[1].runner, "linux");
}

TEST(LutrisLibraryScannerTests, ScansMultipleXdgDirectories) {
  const auto root = lutris_test_root("xdg_dirs");
  const auto config_dir = root / "config";
  const auto data_dir = root / "data";
  std::filesystem::create_directories(config_dir);
  std::filesystem::create_directories(data_dir);

  write_text(config_dir / "config-game.yml",
    "name: Config Game\n"
    "slug: config-game\n");
  write_text(data_dir / "data-game.yml",
    "name: Data Game\n"
    "slug: data-game\n"
    "runner: wine\n");

  const auto games = game_library::scan_lutris_games(
    std::vector<std::filesystem::path> {config_dir, data_dir}
  );
  ASSERT_EQ(games.size(), 2);
  EXPECT_EQ(games[0].slug, "config-game");
  EXPECT_EQ(games[1].slug, "data-game");
  EXPECT_EQ(games[1].runner, "wine");
}

TEST(LutrisLibraryScannerTests, ParsesLutrisListGamesJson) {
  const auto games = game_library::parse_lutris_list_games_json(R"json([
    {"id": 3, "slug": "arc-raiders", "name": "ARC Raiders", "runner": "steam", "coverPath": "lutris/coverart/arc-raiders.jpg"},
    {"id": 14, "slug": "vam-vr", "name": "VaM (VR)", "runner": "wine"},
    {"id": 99, "slug": "bad slug", "name": "Unsafe", "runner": "linux"},
    {"id": 100, "slug": "missing-name", "runner": "linux"}
  ])json");

  ASSERT_EQ(games.size(), 2);
  EXPECT_EQ(games[0].name, "ARC Raiders");
  EXPECT_EQ(games[0].slug, "arc-raiders");
  EXPECT_EQ(games[0].runner, "steam");
  EXPECT_EQ(games[0].command, "setsid lutris lutris:rungame/arc-raiders");
  EXPECT_EQ(games[0].image_path, "lutris/coverart/arc-raiders.jpg");
  EXPECT_EQ(games[1].name, "VaM (VR)");
  EXPECT_EQ(games[1].slug, "vam-vr");
  EXPECT_EQ(games[1].runner, "wine");
  EXPECT_TRUE(games[1].image_path.empty());
}

TEST(LutrisLibraryScannerTests, ValidatesSlugsBeforeBuildingCommands) {
  EXPECT_TRUE(game_library::is_lutris_slug_safe("abc-123_game.name"));
  EXPECT_FALSE(game_library::is_lutris_slug_safe(""));
  EXPECT_FALSE(game_library::is_lutris_slug_safe("bad slug"));
  EXPECT_FALSE(game_library::is_lutris_slug_safe("bad;slug"));
}

TEST(LutrisLibraryScannerTests, FindsLocalLutrisArtworkBySlug) {
  const auto root = lutris_test_root("artwork");
  const auto lutris_root = root / "lutris";
  std::filesystem::create_directories(lutris_root / "coverart");
  std::filesystem::create_directories(lutris_root / "banners");
  write_text(lutris_root / "coverart" / "black-myth-wukong.jpg", "cover");
  write_text(lutris_root / "banners" / "black-myth-wukong.jpg", "banner");

  const auto image_path = game_library::find_lutris_image_path("black-myth-wukong", {lutris_root});
  EXPECT_EQ(image_path, (lutris_root / "coverart" / "black-myth-wukong.jpg").string());
  EXPECT_TRUE(game_library::find_lutris_image_path("bad slug", {lutris_root}).empty());
}

TEST(GameLibraryScannerTests, IncludesAccountHomeWhenRuntimeHomeIsProfile) {
  const auto roots = game_library::library_home_roots("/tmp/agent-profile-home", "/tmp/account-home");

  ASSERT_EQ(roots.size(), 2);
  EXPECT_EQ(roots[0], std::filesystem::path("/tmp/agent-profile-home"));
  EXPECT_EQ(roots[1], std::filesystem::path("/tmp/account-home"));
}

namespace {
  // Trimmed from a real localconfig.vdf: the apps block sits five levels down, and the
  // profile around it carries keys that look just like the ones we want.
  constexpr const char *kLocalConfig = R"vdf(
"UserLocalConfigStore"
{
  "friends"
  {
    "PersonaName"  "papi"
  }
  "Software"
  {
    "Valve"
    {
      "Steam"
      {
        "apps"
        {
          "10"
          {
            "LastPlayed"  "86400"
            "Playtime"    "16"
          }
          "440"
          {
            "LastPlayed"  "1311404400"
            "Playtime"    "1684"
          }
          "620"
          {
            "LastPlayed"  "1500000000"
            "Playtime"    "0"
          }
        }
      }
    }
  }
}
)vdf";
}  // namespace

TEST(SteamPlaytimeTests, ReadsMinutesAndLastPlayedForEachApp) {
  const auto playtime = game_library::parse_steam_playtime_vdf(kLocalConfig);

  ASSERT_EQ(playtime.count("440"), 1u);
  EXPECT_EQ(playtime.at("440").minutes, 1684);
  EXPECT_EQ(playtime.at("440").last_played, 1311404400);

  ASSERT_EQ(playtime.count("10"), 1u);
  EXPECT_EQ(playtime.at("10").minutes, 16);
}

TEST(SteamPlaytimeTests, DropsAppsThatWereLaunchedButNeverPlayed) {
  const auto playtime = game_library::parse_steam_playtime_vdf(kLocalConfig);

  // 620 has a LastPlayed but no minutes, which says nothing worth showing.
  EXPECT_EQ(playtime.count("620"), 0u);
}

TEST(SteamPlaytimeTests, IgnoresKeysOutsideTheAppsBlock) {
  // The same key name, at the same depth, under a different owner.
  constexpr const char *decoy = R"vdf(
"UserLocalConfigStore"
{
  "Software"
  {
    "Valve"
    {
      "Steam"
      {
        "somethingelse"
        {
          "999"
          {
            "Playtime"  "99999"
          }
        }
      }
    }
  }
}
)vdf";

  EXPECT_TRUE(game_library::parse_steam_playtime_vdf(decoy).empty());
}

TEST(SteamPlaytimeTests, SurvivesEmptyAndMalformedPayloads) {
  EXPECT_TRUE(game_library::parse_steam_playtime_vdf("").empty());
  EXPECT_TRUE(game_library::parse_steam_playtime_vdf("not a vdf at all").empty());

  // A value Steam wrote in a shape we do not understand must not cost the others.
  constexpr const char *mixed = R"vdf(
"UserLocalConfigStore"
{
  "Software"
  {
    "Valve"
    {
      "Steam"
      {
        "apps"
        {
          "1"
          {
            "Playtime"  "not-a-number"
          }
          "2"
          {
            "Playtime"  "42"
          }
        }
      }
    }
  }
}
)vdf";

  const auto playtime = game_library::parse_steam_playtime_vdf(mixed);
  EXPECT_EQ(playtime.count("1"), 0u);
  ASSERT_EQ(playtime.count("2"), 1u);
  EXPECT_EQ(playtime.at("2").minutes, 42);
}

TEST(SteamPlaytimeTests, FindsLocalconfigForEveryUserUnderARoot) {
  const auto root = lutris_test_root("steam_userdata");
  const auto user = root / "userdata" / "11324806" / "config";
  std::filesystem::create_directories(user);
  write_text(user / "localconfig.vdf", kLocalConfig);

  // A user directory without the file must not be reported.
  std::filesystem::create_directories(root / "userdata" / "999" / "config");

  const auto found = game_library::steam_localconfig_paths({root});
  ASSERT_EQ(found.size(), 1u);
  EXPECT_EQ(found.front(), user / "localconfig.vdf");
}

TEST(SteamPlaytimeTests, ReportsEachProfileOnceWhenTwoRootsResolveToTheSameDirectory) {
  const auto root = lutris_test_root("steam_userdata_aliased");
  const auto user = root / "userdata" / "11324806" / "config";
  std::filesystem::create_directories(user);
  write_text(user / "localconfig.vdf", kLocalConfig);

  // A stock install symlinks ~/.steam/steam at ~/.local/share/Steam, and both are
  // probed, so this profile is reachable twice. Counting callers must still see one.
  const auto alias = root.parent_path() / "steam_userdata_aliased_link";
  std::filesystem::remove_all(alias);
  std::error_code link_ec;
  std::filesystem::create_directory_symlink(root, alias, link_ec);
  if (link_ec) {
    GTEST_SKIP() << "symlinks are unavailable on this filesystem";
  }

  const auto found = game_library::steam_localconfig_paths({root, alias});
  ASSERT_EQ(found.size(), 1u);

  // Reached through the alias alone, the profile is still reported once, by the
  // path the caller actually walked rather than the resolved target.
  const auto through_alias = game_library::steam_localconfig_paths({alias});
  ASSERT_EQ(through_alias.size(), 1u);
  EXPECT_EQ(through_alias.front(), alias / "userdata" / "11324806" / "config" / "localconfig.vdf");
}

TEST(SteamInputTests, ReadsXboxOptInAndOnlyCountsForcedApps) {
  constexpr const char *config = R"vdf(
"UserLocalConfigStore"
{
  "system"
  {
    "SteamController_XBoxSupport"  "1"
  }
  "Software"
  {
    "Valve"
    {
      "Steam"
      {
        "apps"
        {
          "111"
          {
            "UseSteamControllerConfig"  "2"
          }
          "222"
          {
            "UseSteamControllerConfig"  "0"
          }
        }
        "somethingelse"
        {
          "333"
          {
            "UseSteamControllerConfig"  "2"
          }
        }
      }
    }
  }
}
)vdf";

  const auto parsed = game_library::parse_steam_input_vdf(config);
  EXPECT_TRUE(parsed.parsed);
  EXPECT_TRUE(parsed.xbox_support_enabled);
  EXPECT_EQ(parsed.forced_app_count, 1);
}

TEST(SteamInputTests, LastSettingWinsAndMalformedInputStaysUnknown) {
  constexpr const char *config = R"vdf(
"SteamController_XBoxSupport"  "1"
"SteamController_XBoxSupport"  "0"
"apps"
{
  "444"
  {
    "UseSteamControllerConfig"  "2"
    "UseSteamControllerConfig"  "1"
  }
}
)vdf";

  const auto parsed = game_library::parse_steam_input_vdf(config);
  EXPECT_TRUE(parsed.parsed);
  EXPECT_FALSE(parsed.xbox_support_enabled);
  EXPECT_EQ(parsed.forced_app_count, 0);

  const auto malformed = game_library::parse_steam_input_vdf("not a vdf payload");
  EXPECT_FALSE(malformed.parsed);
  EXPECT_FALSE(malformed.xbox_support_enabled);
  EXPECT_EQ(malformed.forced_app_count, 0);
}

TEST(SteamInputTests, AggregatesProfilesWithoutLeakingPathsOrAppIds) {
  const auto root = lutris_test_root("steam_input_profiles");
  const auto first = root / "profile-a.vdf";
  const auto second = root / "profile-b.vdf";
  write_text(first,
    "\"SteamController_XBoxSupport\"  \"1\"\n"
    "\"apps\"\n"
    "{\n"
    "  \"555\"\n"
    "  {\n"
    "    \"UseSteamControllerConfig\"  \"2\"\n"
    "  }\n"
    "}\n");
  write_text(second, "\"SteamController_XBoxSupport\"  \"0\"\n");

  const auto snapshot = game_library::inspect_steam_input_configs({first, second});
  EXPECT_EQ(snapshot.status, "xbox_opt_in_and_per_app_forced");
  EXPECT_EQ(snapshot.profiles_checked, 2);
  EXPECT_EQ(snapshot.profiles_with_xbox_support, 1);
  EXPECT_EQ(snapshot.forced_app_count, 1);
  EXPECT_TRUE(snapshot.conflict());
  EXPECT_EQ(snapshot.detail.find(root.string()), std::string::npos);
  EXPECT_EQ(snapshot.detail.find("555"), std::string::npos);
}

TEST(SteamInputTests, ClearsTheXboxOptInWithoutDisturbingTheRestOfTheProfile) {
  const std::string profile =
    "\"UserLocalConfigStore\"\n{\n"
    "\t\"SteamController_XBoxSupport\"\t\t\"1\"\n"
    "\t\"SteamController_GenericGamepadSupport\"\t\t\"1\"\n"
    "}\n";

  const auto updated = game_library::set_steam_input_xbox_support(profile, false);
  ASSERT_TRUE(updated.has_value());
  EXPECT_NE(updated->find("\"SteamController_XBoxSupport\"\t\t\"0\""), std::string::npos);
  // Only that one value may move: this rewrites a live Steam profile.
  EXPECT_NE(updated->find("\"SteamController_GenericGamepadSupport\"\t\t\"1\""), std::string::npos);
  EXPECT_EQ(updated->size(), profile.size());

  // Re-enabling is the undo path, and it round-trips exactly.
  const auto restored = game_library::set_steam_input_xbox_support(*updated, true);
  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(*restored, profile);
}

TEST(SteamInputTests, SkipsTheWriteWhenTheOptInAlreadyHoldsTheRequestedValue) {
  const std::string already_off =
    "\"UserLocalConfigStore\"\n{\n\t\"SteamController_XBoxSupport\"\t\t\"0\"\n}\n";
  EXPECT_FALSE(game_library::set_steam_input_xbox_support(already_off, false).has_value());

  // A profile that never mentions the key is not ours to invent.
  const std::string absent = "\"UserLocalConfigStore\"\n{\n\t\"Other\"\t\t\"1\"\n}\n";
  EXPECT_FALSE(game_library::set_steam_input_xbox_support(absent, false).has_value());
}

TEST(SteamInputTests, RefusesToBorrowAValueFromTheFollowingLine) {
  // A key Steam wrote without a value must not consume the next setting's
  // value and silently rewrite the wrong field.
  const std::string truncated =
    "\"UserLocalConfigStore\"\n{\n\t\"SteamController_XBoxSupport\"\n\t\"Other\"\t\t\"1\"\n}\n";
  EXPECT_FALSE(game_library::set_steam_input_xbox_support(truncated, false).has_value());
}

TEST(SteamInputTests, ReportsUnknownWhenNoProfileIsReadable) {
  const auto snapshot = game_library::inspect_steam_input_configs({
    lutris_test_root("steam_input_missing") / "missing.vdf"
  });

  EXPECT_EQ(snapshot.status, "unknown");
  EXPECT_EQ(snapshot.profiles_checked, 0);
  EXPECT_FALSE(snapshot.conflict());
}

TEST(LutrisLibraryScannerTests, CarriesPlaytimeSecondsFromTheListingJson) {
  const auto games = game_library::parse_lutris_list_games_json(
    R"json([{"name":"3DMark","slug":"3dmark","runner":"steam","playtimeSeconds":73.152428}])json");

  ASSERT_EQ(games.size(), 1u);
  EXPECT_EQ(games.front().playtime_seconds, 73);
}

TEST(FlatpakLibraryTests, RecognisesPathsInsideAFlatpakApplicationHome) {
  EXPECT_TRUE(game_library::path_is_under_flatpak_app(
    "/tmp/library-home/.var/app/net.lutris.Lutris/config/lutris/games/game.yml",
    "net.lutris.Lutris"));
  EXPECT_TRUE(game_library::path_is_under_flatpak_app(
    "/tmp/library-home/.var/app/com.heroicgameslauncher.hgl/config/heroic",
    "com.heroicgameslauncher.hgl"));

  // A different application's home, the native home, and a lookalike prefix are not it.
  EXPECT_FALSE(game_library::path_is_under_flatpak_app(
    "/tmp/library-home/.var/app/com.valvesoftware.Steam/config/lutris/games/game.yml",
    "net.lutris.Lutris"));
  EXPECT_FALSE(game_library::path_is_under_flatpak_app(
    "/tmp/library-home/.config/lutris/games/game.yml",
    "net.lutris.Lutris"));
  EXPECT_FALSE(game_library::path_is_under_flatpak_app(
    "/tmp/library-home/.var/app/net.lutris.Lutris.backup/config/lutris/games/game.yml",
    "net.lutris.Lutris"));
}

TEST(LutrisLibraryScannerTests, IncludesFlatpakGameAndArtDirectories) {
  const std::filesystem::path home = "/tmp/library-home";
  const auto dirs = game_library::lutris_game_config_dirs({home});
  const auto roots = game_library::lutris_art_roots({home});

  const auto contains = [](const auto &haystack, const std::filesystem::path &needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
  };

  EXPECT_TRUE(contains(dirs, home / ".config/lutris/games"));
  EXPECT_TRUE(contains(dirs, home / ".local/share/lutris/games"));
  EXPECT_TRUE(contains(dirs, home / ".var/app/net.lutris.Lutris/config/lutris/games"));
  EXPECT_TRUE(contains(dirs, home / ".var/app/net.lutris.Lutris/data/lutris/games"));

  EXPECT_TRUE(contains(roots, home / ".local/share/lutris"));
  EXPECT_TRUE(contains(roots, home / ".cache/lutris"));
  EXPECT_TRUE(contains(roots, home / ".var/app/net.lutris.Lutris/data/lutris"));
  EXPECT_TRUE(contains(roots, home / ".var/app/net.lutris.Lutris/cache/lutris"));
}

TEST(LutrisLibraryScannerTests, LaunchesAFlatpakConfigThroughFlatpak) {
  const auto root = lutris_test_root("flatpak_config");
  const auto games_dir = root / ".var/app/net.lutris.Lutris/config/lutris/games";
  std::filesystem::create_directories(games_dir);
  write_text(games_dir / "vintage-story.yml",
    "name: Vintage Story\n"
    "slug: vintage-story\n"
    "runner: linux\n");

  const auto game = game_library::parse_lutris_game_config(games_dir / "vintage-story.yml");
  ASSERT_TRUE(game.has_value());
  EXPECT_EQ(game->command, "setsid flatpak run net.lutris.Lutris lutris:rungame/vintage-story");

  // The same slug from a native install keeps the command it already had.
  EXPECT_EQ(game_library::lutris_launch_command("vintage-story"), "setsid lutris lutris:rungame/vintage-story");
}

TEST(HeroicLibraryScannerTests, ListsBothInstallsForEveryStoreFile) {
  const std::filesystem::path home = "/tmp/library-home";
  const auto installed = game_library::heroic_installed_files({home});
  const auto cached = game_library::heroic_cache_files({home});

  ASSERT_EQ(installed.size(), 4u);
  EXPECT_EQ(installed[0].path, home / ".config/heroic/gog_store/installed.json");
  EXPECT_EQ(installed[0].store, "gog");
  EXPECT_EQ(installed[0].install, game_library::launcher_install_t::native);
  EXPECT_EQ(installed[1].path, home / ".config/heroic/legendaryConfig/legendary/installed.json");
  EXPECT_EQ(installed[1].store, "epic");
  EXPECT_EQ(installed[2].path, home / ".var/app/com.heroicgameslauncher.hgl/config/heroic/gog_store/installed.json");
  EXPECT_EQ(installed[2].install, game_library::launcher_install_t::flatpak);
  EXPECT_EQ(installed[3].path,
    home / ".var/app/com.heroicgameslauncher.hgl/config/heroic/legendaryConfig/legendary/installed.json");

  ASSERT_EQ(cached.size(), 4u);
  EXPECT_EQ(cached[0].path, home / ".config/heroic/store_cache/gog_library.json");
  EXPECT_EQ(cached[1].path, home / ".config/heroic/store_cache/legendary_library.json");
  EXPECT_EQ(cached[2].path, home / ".var/app/com.heroicgameslauncher.hgl/config/heroic/store_cache/gog_library.json");
  EXPECT_EQ(cached[2].install, game_library::launcher_install_t::flatpak);
}

TEST(HeroicLibraryScannerTests, BuildsTheCommandForTheInstallThatHasTheTitle) {
  EXPECT_EQ(
    game_library::heroic_launch_command("gog", "1207658930", game_library::launcher_install_t::native),
    "setsid heroic --no-gui --no-sandbox 'heroic://launch?appName=1207658930&runner=gog'");
  EXPECT_EQ(
    game_library::heroic_launch_command("gog", "1207658930", game_library::launcher_install_t::flatpak),
    "setsid flatpak run com.heroicgameslauncher.hgl --no-gui --no-sandbox "
    "'heroic://launch?appName=1207658930&runner=gog'");

  const auto both = game_library::heroic_launch_commands("epic", "Snow");
  ASSERT_EQ(both.size(), 4u);
  EXPECT_EQ(
    both[0],
    "setsid heroic --no-gui --no-sandbox 'heroic://launch?appName=Snow&runner=legendary'");
  EXPECT_EQ(
    both[1],
    "setsid flatpak run com.heroicgameslauncher.hgl --no-gui --no-sandbox "
    "'heroic://launch?appName=Snow&runner=legendary'");
  EXPECT_EQ(both[2], "setsid heroic heroic://launch/epic/Snow");
  EXPECT_EQ(both[3], "setsid flatpak run com.heroicgameslauncher.hgl heroic://launch/epic/Snow");
}

TEST(HeroicLibraryScannerTests, MapsOnlySupportedStoresAndInstallNames) {
  EXPECT_EQ(game_library::heroic_runner_for_store("gog"), "gog");
  EXPECT_EQ(game_library::heroic_runner_for_store("epic"), "legendary");
  EXPECT_TRUE(game_library::heroic_runner_for_store("amazon").empty());
  EXPECT_TRUE(game_library::heroic_runner_for_store("legendary").empty());

  EXPECT_EQ(game_library::heroic_install_name(game_library::launcher_install_t::native), "native");
  EXPECT_EQ(game_library::heroic_install_name(game_library::launcher_install_t::flatpak), "flatpak");
  EXPECT_EQ(
    game_library::heroic_install_from_name("native"),
    game_library::launcher_install_t::native);
  EXPECT_EQ(
    game_library::heroic_install_from_name("flatpak"),
    game_library::launcher_install_t::flatpak);
  EXPECT_FALSE(game_library::heroic_install_from_name("system").has_value());
}

TEST(HeroicLibraryScannerTests, RebuildsImportsFromExactMetadataAndRejectsTampering) {
  const auto epic = game_library::heroic_game_from_metadata("Snow", "epic", "legendary", "flatpak");
  ASSERT_TRUE(epic.has_value());
  EXPECT_EQ(epic->runner, "legendary");
  EXPECT_EQ(epic->install, game_library::launcher_install_t::flatpak);
  EXPECT_EQ(
    epic->command,
    "setsid flatpak run com.heroicgameslauncher.hgl --no-gui --no-sandbox "
    "'heroic://launch?appName=Snow&runner=legendary'");

  EXPECT_FALSE(game_library::heroic_game_from_metadata("Snow", "epic", "epic", "flatpak").has_value());
  EXPECT_FALSE(game_library::heroic_game_from_metadata("Snow", "gog", "gog", "system").has_value());
  EXPECT_FALSE(game_library::heroic_game_from_metadata("Snow;id", "gog", "gog", "native").has_value());
  EXPECT_FALSE(game_library::heroic_game_from_metadata("Snow", "amazon", "nile", "native").has_value());
}

TEST(HeroicLibraryScannerTests, ParsesOnlyExactLegacyPolarisCommands) {
  const auto native = game_library::parse_legacy_heroic_launch_command(
    "setsid heroic heroic://launch/epic/Snow");
  ASSERT_TRUE(native.has_value());
  EXPECT_EQ(native->store, "epic");
  EXPECT_EQ(native->runner, "legendary");
  EXPECT_EQ(native->app_name, "Snow");
  EXPECT_EQ(native->install, game_library::launcher_install_t::native);

  const auto flatpak = game_library::parse_legacy_heroic_launch_command(
    "setsid flatpak run com.heroicgameslauncher.hgl heroic://launch/gog/1207658930");
  ASSERT_TRUE(flatpak.has_value());
  EXPECT_EQ(flatpak->install, game_library::launcher_install_t::flatpak);
  EXPECT_EQ(flatpak->command,
    "setsid flatpak run com.heroicgameslauncher.hgl --no-gui --no-sandbox "
    "'heroic://launch?appName=1207658930&runner=gog'");

  EXPECT_FALSE(game_library::parse_legacy_heroic_launch_command(
    "setsid heroic heroic://launch/epic/Snow/extra").has_value());
  EXPECT_FALSE(game_library::parse_legacy_heroic_launch_command(
    "heroic heroic://launch/epic/Snow").has_value());
  EXPECT_FALSE(game_library::parse_legacy_heroic_launch_command(
    "setsid heroic heroic://launch/amazon/Snow").has_value());
}

TEST(HeroicLibraryScannerTests, ParsesLegendaryInstalledFixture) {
  const auto legendary = game_library::parse_heroic_installed_json(R"json({
    "Snow": {"app_name":"Snow","title":"An Epic Game","is_dlc":false},
    "Tool": {"app_name":"Tool","title":"Tool","is_dlc":true},
    "MissingAppName": {"title":"Missing App Name","is_dlc":false},
    "MissingDlcState": {"app_name":"MissingDlcState","title":"Missing DLC State"},
    "WrongDlcState": {"app_name":"WrongDlcState","title":"Wrong DLC State","is_dlc":"no"},
    "MismatchedIdentity": {"app_name":"OtherIdentity","title":"Mismatched Identity","is_dlc":false},
    "metadata": {"last_updated":1234}
  })json", "epic", game_library::launcher_install_t::flatpak);
  ASSERT_EQ(legendary.size(), 1u);
  EXPECT_EQ(legendary[0].app_name, "Snow");
  EXPECT_EQ(legendary[0].runner, "legendary");
  EXPECT_EQ(legendary[0].install, game_library::launcher_install_t::flatpak);
}

TEST(HeroicLibraryScannerTests, JoinsRealGogInstalledAndCacheShapes) {
  const auto games = game_library::parse_heroic_gog_library_json(R"json({
    "installed": [
      {"appName":"1207658930","install_path":"/games/Alpha","is_dlc":false},
      {"appName":"installed-dlc","install_path":"/games/DLC","is_dlc":true},
      {"appName":"missing-metadata","install_path":"/games/Missing","is_dlc":false},
      {"appName":"bad;id","install_path":"/games/Unsafe","is_dlc":false}
    ]
  })json", R"json({"games":[
    {"app_name":"1207658930","title":"A GOG Game","is_installed":false,"install":{"is_dlc":false}},
    {"app_name":"installed-dlc","title":"Installed DLC","is_installed":false,"install":{"is_dlc":true}},
    {"app_name":"cloud-only","title":"Uninstalled Game","is_installed":false,"install":{"is_dlc":false}},
    {"app_name":"gog-redist","title":"Galaxy Common Redistributables","is_installed":true,"install":{"is_dlc":true}}
  ]})json", game_library::launcher_install_t::native);

  ASSERT_EQ(games.size(), 1u);
  EXPECT_EQ(games[0].name, "A GOG Game");
  EXPECT_EQ(games[0].app_name, "1207658930");
  EXPECT_EQ(games[0].runner, "gog");
}

TEST(HeroicLibraryScannerTests, ParsesOnlyInstalledNonDlcLegendaryCacheEntries) {
  const auto games = game_library::parse_heroic_cache_json(R"json({"library":[
    {"app_name":"Installed","title":"Installed Game","is_installed":true,"install":{"is_dlc":false},"art_square":"https://cdn1.epicgames.com/item/installed/poster","art_cover":"https://cdn1.epicgames.com/item/installed/hero"},
    {"app_name":"InstalledDlc","title":"Installed DLC","is_installed":true,"install":{"is_dlc":true}},
    {"app_name":"CloudOnly","title":"Cloud Game","is_installed":false,"install":{"is_dlc":false}},
    {"app_name":"WrongShape","title":"Wrong Shape","is_installed":"yes","install":{"is_dlc":false}}
  ]})json", "epic", game_library::launcher_install_t::native);

  ASSERT_EQ(games.size(), 1u);
  EXPECT_EQ(games[0].app_name, "Installed");
  EXPECT_EQ(games[0].runner, "legendary");
  EXPECT_EQ(games[0].poster_url, "https://cdn1.epicgames.com/item/installed/poster");
  EXPECT_EQ(games[0].hero_url, "https://cdn1.epicgames.com/item/installed/hero");
}

TEST(HeroicLibraryScannerTests, ResolvesArtworkFromTheExactHeroicInstallAndIdentity) {
  const auto home = lutris_test_root("heroic_cached_artwork");
  const auto cache = home / ".var/app/com.heroicgameslauncher.hgl/config/heroic/store_cache/legendary_library.json";
  std::filesystem::create_directories(cache.parent_path());
  write_text(cache, R"json({"library":[
    {"app_name":"AlanWake2","title":"Alan Wake 2","is_installed":true,"install":{"is_dlc":false},"art_square":"https://cdn1.epicgames.com/item/alan-wake-2/poster","art_cover":"https://cdn1.epicgames.com/item/alan-wake-2/hero"}
  ]})json");

  const auto game = game_library::find_heroic_cached_game(
    {home},
    "AlanWake2",
    "epic",
    game_library::launcher_install_t::flatpak
  );
  ASSERT_TRUE(game.has_value());
  EXPECT_EQ(game->name, "Alan Wake 2");
  EXPECT_EQ(game->poster_url, "https://cdn1.epicgames.com/item/alan-wake-2/poster");
  EXPECT_EQ(game->hero_url, "https://cdn1.epicgames.com/item/alan-wake-2/hero");

  EXPECT_FALSE(game_library::find_heroic_cached_game(
    {home},
    "AlanWake2",
    "epic",
    game_library::launcher_install_t::native
  ).has_value());
}

TEST(HeroicLibraryScannerTests, RefusesAppNamesThatWouldReachTheShell) {
  EXPECT_TRUE(game_library::is_heroic_app_name_safe("Grimoire.Manastone_1207658930"));
  EXPECT_FALSE(game_library::is_heroic_app_name_safe(""));
  EXPECT_FALSE(game_library::is_heroic_app_name_safe("app; rm -rf ~"));
  EXPECT_FALSE(game_library::is_heroic_app_name_safe("app$(id)"));

  EXPECT_TRUE(game_library::heroic_launch_command("gog", "app; rm -rf ~", game_library::launcher_install_t::native).empty());
  EXPECT_TRUE(game_library::heroic_launch_command("gog && id", "1207658930", game_library::launcher_install_t::native).empty());
  EXPECT_TRUE(game_library::heroic_launch_command("legendary", "Snow", game_library::launcher_install_t::native).empty());
  EXPECT_TRUE(game_library::heroic_launch_commands("gog", "app; rm -rf ~").empty());
}
