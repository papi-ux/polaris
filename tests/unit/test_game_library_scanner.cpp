#include "../tests_common.h"
#include "../tests_paths.h"

#include <src/game_library_scanner.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <vector>

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

  ASSERT_EQ(cached.size(), 8u);
  EXPECT_EQ(cached[0].path, home / ".config/heroic/store_cache/gog_library.json");
  EXPECT_EQ(cached[1].path, home / ".config/heroic/store_cache/legendary_library.json");
  EXPECT_EQ(cached[2].path, home / ".config/heroic/store_cache/nile_library.json");
  EXPECT_EQ(cached[2].store, "amazon");
  EXPECT_EQ(cached[3].path, home / ".config/heroic/sideload_apps/library.json");
  EXPECT_EQ(cached[3].store, "sideload");
  EXPECT_EQ(cached[4].path, home / ".var/app/com.heroicgameslauncher.hgl/config/heroic/store_cache/gog_library.json");
  EXPECT_EQ(cached[4].install, game_library::launcher_install_t::flatpak);
  EXPECT_EQ(cached[6].path, home / ".var/app/com.heroicgameslauncher.hgl/config/heroic/store_cache/nile_library.json");
  EXPECT_EQ(cached[7].path, home / ".var/app/com.heroicgameslauncher.hgl/config/heroic/sideload_apps/library.json");
}

TEST(HeroicLibraryScannerTests, BuildsTheCommandForTheInstallThatHasTheTitle) {
  EXPECT_EQ(
    game_library::heroic_launch_command("gog", "1207658930", game_library::launcher_install_t::native),
    "setsid heroic --no-gui --no-sandbox 'heroic://launch?appName=1207658930&runner=gog'");
  EXPECT_EQ(
    game_library::heroic_launch_command("gog", "1207658930", game_library::launcher_install_t::flatpak),
    "setsid flatpak run com.heroicgameslauncher.hgl "
    "'heroic://launch?appName=1207658930&runner=gog'");

  const auto both = game_library::heroic_launch_commands("epic", "Snow");
  ASSERT_EQ(both.size(), 5u);
  EXPECT_EQ(
    both[0],
    "setsid heroic --no-gui --no-sandbox 'heroic://launch?appName=Snow&runner=legendary'");
  EXPECT_EQ(
    both[1],
    "setsid flatpak run com.heroicgameslauncher.hgl "
    "'heroic://launch?appName=Snow&runner=legendary'");
  EXPECT_EQ(both[2], "setsid heroic heroic://launch/epic/Snow");
  EXPECT_EQ(
    both[3],
    "setsid flatpak run com.heroicgameslauncher.hgl --no-gui --no-sandbox "
    "'heroic://launch?appName=Snow&runner=legendary'");
  EXPECT_EQ(both[4], "setsid flatpak run com.heroicgameslauncher.hgl heroic://launch/epic/Snow");

  const auto flatpak_only = game_library::heroic_launch_commands_for_install(
    "gog",
    "1207658930",
    game_library::launcher_install_t::flatpak
  );
  ASSERT_EQ(flatpak_only.size(), 3u);
  EXPECT_EQ(flatpak_only.front(),
    "setsid flatpak run com.heroicgameslauncher.hgl "
    "'heroic://launch?appName=1207658930&runner=gog'");
  EXPECT_EQ(flatpak_only[1],
    "setsid flatpak run com.heroicgameslauncher.hgl --no-gui --no-sandbox "
    "'heroic://launch?appName=1207658930&runner=gog'");
}

TEST(HeroicLibraryScannerTests, MapsOnlySupportedStoresAndInstallNames) {
  EXPECT_EQ(game_library::heroic_runner_for_store("gog"), "gog");
  EXPECT_EQ(game_library::heroic_runner_for_store("epic"), "legendary");
  EXPECT_EQ(game_library::heroic_runner_for_store("amazon"), "nile");
  EXPECT_EQ(game_library::heroic_runner_for_store("sideload"), "sideload");
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
    "setsid flatpak run com.heroicgameslauncher.hgl "
    "'heroic://launch?appName=Snow&runner=legendary'");

  EXPECT_FALSE(game_library::heroic_game_from_metadata("Snow", "epic", "epic", "flatpak").has_value());
  EXPECT_FALSE(game_library::heroic_game_from_metadata("Snow", "gog", "gog", "system").has_value());
  EXPECT_FALSE(game_library::heroic_game_from_metadata("Snow;id", "gog", "gog", "native").has_value());
  EXPECT_FALSE(game_library::heroic_game_from_metadata("Snow", "amazon", "amazon", "native").has_value());
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
    "setsid flatpak run com.heroicgameslauncher.hgl "
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

TEST(HeroicLibraryScannerTests, DiscoversInstalledAmazonAndSideloadedGames) {
  // Heroic's Nile cache has no is_dlc field; its sideload store uses games instead of library.
  const auto amazon = game_library::parse_heroic_cache_json(R"json({"library":[
    {"app_name":"amzn1.adg.product.fixture-id","title":"Amazon Game","runner":"nile",
     "is_installed":true,"install":{"platform":"Windows"},"art_square":"https://example.invalid/poster.jpg"}
  ]})json", "amazon", game_library::launcher_install_t::flatpak);
  ASSERT_EQ(amazon.size(), 1u);
  EXPECT_EQ(amazon.front().store, "amazon");
  EXPECT_EQ(amazon.front().runner, "nile");
  EXPECT_EQ(amazon.front().platform, "windows");
  EXPECT_EQ(amazon.front().command,
    "setsid flatpak run com.heroicgameslauncher.hgl 'heroic://launch?appName=amzn1.adg.product.fixture-id&runner=nile'");

  const auto sideload = game_library::parse_heroic_cache_json(R"json({"games":[
    {"app_name":"custom-game-1","title":"Local Game","runner":"sideload","is_installed":true,
     "install":{"platform":"linux","is_dlc":false,"executable":"/games/not-used-by-polaris"}}
  ]})json", "sideload", game_library::launcher_install_t::native);
  ASSERT_EQ(sideload.size(), 1u);
  EXPECT_EQ(sideload.front().runner, "sideload");
  EXPECT_EQ(sideload.front().platform, "linux");
  EXPECT_EQ(sideload.front().command,
    "setsid heroic --no-gui --no-sandbox 'heroic://launch?appName=custom-game-1&runner=sideload'");
}

TEST(HeroicLibraryScannerTests, AdditionalStoresRejectUninstalledDlcAndMismatchedIdentities) {
  using json = nlohmann::json;
  for (const auto store : {"amazon", "sideload"}) {
    const auto runner = std::string(store) == "amazon" ? "nile" : "sideload";
    const auto key = std::string(store) == "amazon" ? "library" : "games";
    const json valid {{"app_name", "safe-id"}, {"title", "Installed Game"}, {"runner", runner},
      {"is_installed", true}, {"install", {{"platform", "Windows"}, {"is_dlc", false}}}};
    for (const auto &[field, value] : std::vector<std::pair<std::string, json>> {
           {"is_installed", false}, {"is_installed", "yes"}, {"runner", "legendary"}, {"runner", 1},
           {"app_name", "bad;command"}, {"app_name", "../escape"}, {"app_name", 7}, {"title", ""}, {"title", 7},
           {"is_dlc", true}, {"is_dlc", "false"},
           {"install", nullptr}, {"install", {{"is_dlc", true}}}, {"install", {{"is_dlc", "false"}}}}) {
      auto bad = valid;
      bad[field] = value;
      const json payload {{key, json::array({bad, valid})}};
      const auto games = game_library::parse_heroic_cache_json(payload.dump(), store, game_library::launcher_install_t::native);
      ASSERT_EQ(games.size(), 1u) << store << ' ' << field;
      EXPECT_EQ(games.front().app_name, "safe-id");
    }
  }
}

TEST(HeroicLibraryScannerTests, NewStoresUseOnlyCurrentCommandFormsAndExactRunnerMetadata) {
  for (const auto store : {"amazon", "sideload"}) {
    const auto runner = std::string(store) == "amazon" ? "nile" : "sideload";
    for (const auto install : {game_library::launcher_install_t::native, game_library::launcher_install_t::flatpak}) {
      const auto game = game_library::heroic_game_from_metadata("safe-id", store, runner, game_library::heroic_install_name(install));
      ASSERT_TRUE(game);
      const auto commands = game_library::heroic_launch_commands_for_install(store, "safe-id", install);
      ASSERT_EQ(commands.size(), 1u);
      EXPECT_EQ(commands.front(), game->command);
      EXPECT_FALSE(game_library::heroic_game_from_metadata("safe-id", store, "legendary", game_library::heroic_install_name(install)));
    }
    EXPECT_FALSE(game_library::parse_legacy_heroic_launch_command("setsid heroic heroic://launch/" + std::string(store) + "/safe-id"));
  }
}

TEST(HeroicLibraryScannerTests, FindsNewStoresOnlyWithinTheRequestedHeroicInstallation) {
  const auto home = lutris_test_root("heroic_additional_stores");
  const auto heroic = home / ".var/app/com.heroicgameslauncher.hgl/config/heroic";
  std::filesystem::create_directories(heroic / "store_cache");
  std::filesystem::create_directories(heroic / "sideload_apps");
  write_text(heroic / "store_cache/nile_library.json", R"json({"library":[
    {"app_name":"amazon-id","title":"Amazon Game","runner":"nile","is_installed":true,"install":{"platform":"Windows"}}
  ]})json");
  write_text(heroic / "sideload_apps/library.json", R"json({"games":[
    {"app_name":"sideload-id","title":"Local Game","runner":"sideload","is_installed":true,"install":{"is_dlc":false}}
  ]})json");
  for (const auto store : {"amazon", "sideload"}) {
    const auto id = std::string(store) + "-id";
    EXPECT_TRUE(game_library::find_heroic_cached_game({home}, id, store, game_library::launcher_install_t::flatpak));
    EXPECT_FALSE(game_library::find_heroic_cached_game({home}, id, store, game_library::launcher_install_t::native));
    EXPECT_FALSE(game_library::find_heroic_cached_game({home}, "different-id", store, game_library::launcher_install_t::flatpak));
  }
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

// Heroic records what a title installs as, and separately what will execute it.
// Polaris reports both or neither, never a guess: the Library page and Nova use
// this to say "Windows via Proton-GE" instead of leaving the operator to find out
// at launch.
TEST(HeroicRuntimeTests, NormalisesThePlatformHeroicRecorded) {
  EXPECT_EQ(game_library::normalize_heroic_platform("Windows", false), "windows");
  EXPECT_EQ(game_library::normalize_heroic_platform("windows", false), "windows");
  EXPECT_EQ(game_library::normalize_heroic_platform("Linux", false), "linux");
  EXPECT_EQ(game_library::normalize_heroic_platform("Mac", false), "mac");
}

TEST(HeroicRuntimeTests, FallsBackToTheNativeFlagRatherThanGuessingWindows) {
  EXPECT_EQ(game_library::normalize_heroic_platform("", true), "linux");
  EXPECT_TRUE(game_library::normalize_heroic_platform("", false).empty());
  EXPECT_TRUE(game_library::normalize_heroic_platform("Nintendo", false).empty());
}

TEST(HeroicRuntimeTests, ReportsProtonAndWineWithTheirOwnLabel) {
  const auto proton = game_library::heroic_runtime_from_config("proton", "Proton-GE Latest", "windows");
  EXPECT_EQ(proton.runtime, "proton");
  EXPECT_EQ(proton.runtime_name, "Proton-GE Latest");

  const auto wine = game_library::heroic_runtime_from_config("wine", "Wine-GE 8", "windows");
  EXPECT_EQ(wine.runtime, "wine");
  EXPECT_EQ(wine.runtime_name, "Wine-GE 8");
}

TEST(HeroicRuntimeTests, CallsALinuxTitleNativeWhateverAStaleWineEntrySays) {
  const auto native = game_library::heroic_runtime_from_config("proton", "Proton-GE Latest", "linux");
  EXPECT_EQ(native.runtime, "native");
  EXPECT_TRUE(native.runtime_name.empty());
}

TEST(HeroicRuntimeTests, ReportsNoRuntimeRatherThanInventingOne) {
  const auto unknown = game_library::heroic_runtime_from_config("", "", "windows");
  EXPECT_TRUE(unknown.runtime.empty());
  EXPECT_TRUE(unknown.runtime_name.empty());
  EXPECT_EQ(unknown.platform, "windows");
}

TEST(HeroicRuntimeTests, ReadsTheRuntimeFromTheHeroicPerGameConfig) {
  // The shape here mirrors a real Heroic GamesConfig entry: settings nested
  // under the app name, with wineVersion carrying the choice.
  const auto config_root = std::filesystem::temp_directory_path() / "polaris-heroic-runtime-test";
  std::filesystem::remove_all(config_root);
  std::filesystem::create_directories(config_root / "GamesConfig");
  {
    std::ofstream config(config_root / "GamesConfig" / "dc9d2e595d0e4650b35d659f90d41059.json");
    config << R"({
      "dc9d2e595d0e4650b35d659f90d41059": {
        "useGameMode": true,
        "wineVersion": {
          "bin": "/opt/compatibilitytools.d/Proton-GE Latest/proton",
          "name": "Proton-GE Latest",
          "type": "proton"
        },
        "winePrefix": "/games/Heroic/Prefixes/Example Game"
      }
    })";
  }

  const auto runtime = game_library::heroic_runtime_for_app(config_root, "dc9d2e595d0e4650b35d659f90d41059", "windows");
  EXPECT_EQ(runtime.runtime, "proton");
  EXPECT_EQ(runtime.runtime_name, "Proton-GE Latest");
  std::filesystem::remove_all(config_root);
}

TEST(HeroicRuntimeTests, SaysNothingWhenTheHeroicConfigIsMissingOrUnreadable) {
  const auto config_root = std::filesystem::temp_directory_path() / "polaris-heroic-runtime-absent";
  std::filesystem::remove_all(config_root);
  const auto missing = game_library::heroic_runtime_for_app(config_root, "nothing-here", "windows");
  EXPECT_TRUE(missing.runtime.empty());
  EXPECT_EQ(missing.platform, "windows");

  std::filesystem::create_directories(config_root / "GamesConfig");
  {
    std::ofstream broken(config_root / "GamesConfig" / "broken.json");
    broken << "{ not json";
  }
  const auto unreadable = game_library::heroic_runtime_for_app(config_root, "broken", "windows");
  EXPECT_TRUE(unreadable.runtime.empty());
  std::filesystem::remove_all(config_root);
}

TEST(HeroicRuntimeTests, ParsesThePlatformOutOfTheLegendaryLibraryEntry) {
  // Trimmed from the real legendary_library.json on a host with Alan Wake 2.
  const std::string payload = R"({"library":[{
    "app_name": "dc9d2e595d0e4650b35d659f90d41059",
    "title": "Alan Wake 2",
    "runner": "legendary",
    "is_installed": true,
    "is_linux_native": false,
    "install": {"executable": "AlanWake2.exe", "install_path": "/games/Heroic/ExampleGame", "is_dlc": false, "platform": "Windows"}
  }]})";

  const auto games = game_library::parse_heroic_cache_json(payload, "epic", game_library::launcher_install_t::flatpak);
  ASSERT_EQ(games.size(), 1u);
  EXPECT_EQ(games[0].name, "Alan Wake 2");
  EXPECT_EQ(games[0].platform, "windows");
}

TEST(HeroicRuntimeTests, FindsTheConfigRootWhateverDepthTheLibraryFileSitsAt) {
  // Heroic's own layout: store_cache is one level under the root, while
  // legendaryConfig/legendary is two. Assuming a fixed depth silently picked the
  // wrong root for Epic's installed manifest.
  const auto config_root = std::filesystem::temp_directory_path() / "polaris-heroic-root-test";
  std::filesystem::remove_all(config_root);
  std::filesystem::create_directories(config_root / "GamesConfig");
  std::filesystem::create_directories(config_root / "store_cache");
  std::filesystem::create_directories(config_root / "legendaryConfig" / "legendary");

  EXPECT_EQ(
    game_library::heroic_config_root_for_library(config_root / "store_cache" / "legendary_library.json"),
    config_root
  );
  EXPECT_EQ(
    game_library::heroic_config_root_for_library(config_root / "legendaryConfig" / "legendary" / "installed.json"),
    config_root
  );
  std::filesystem::remove_all(config_root);
}

TEST(HeroicRuntimeTests, ReportsNoConfigRootWhenHeroicIsNotThere) {
  const auto stray = std::filesystem::temp_directory_path() / "polaris-heroic-no-root" / "store_cache" / "library.json";
  std::filesystem::remove_all(stray.parent_path().parent_path());
  std::filesystem::create_directories(stray.parent_path());
  EXPECT_TRUE(game_library::heroic_config_root_for_library(stray).empty());
  std::filesystem::remove_all(stray.parent_path().parent_path());
}

// An imported Heroic title launches straight into the game, which leaves no way to
// reach Heroic itself from a stream to install something or fix a login. The
// launcher entry exists for that, so unlike a game launch it must keep the window.
TEST(HeroicLauncherCommandTests, OpensTheLauncherItselfForEachInstall) {
  EXPECT_EQ(game_library::heroic_launcher_command(game_library::launcher_install_t::native), "setsid heroic");
  EXPECT_EQ(
    game_library::heroic_launcher_command(game_library::launcher_install_t::flatpak),
    "setsid flatpak run com.heroicgameslauncher.hgl"
  );
}

TEST(HeroicLauncherCommandTests, KeepsTheWindowThatAGameLaunchSuppresses) {
  // A game launch passes --no-gui because it dispatches a protocol URL. Carrying
  // that flag here would open nothing at all.
  const auto launcher = game_library::heroic_launcher_command(game_library::launcher_install_t::native);
  EXPECT_EQ(launcher.find("--no-gui"), std::string::npos);
  EXPECT_EQ(launcher.find("heroic://"), std::string::npos);

  const auto game = game_library::heroic_launch_command("epic", "abc123", game_library::launcher_install_t::native);
  EXPECT_NE(game.find("--no-gui"), std::string::npos);
}

TEST(HeroicLauncherCommandTests, MatchesTheInstallTheLibraryEntriesCameFrom) {
  // A Flatpak-only host has no heroic on PATH, so the launcher entry has to follow
  // the packaging that produced the import.
  const auto flatpak = game_library::heroic_launcher_command(game_library::launcher_install_t::flatpak);
  EXPECT_NE(flatpak.find("flatpak run"), std::string::npos);
  EXPECT_NE(
    game_library::heroic_launch_command("epic", "abc123", game_library::launcher_install_t::flatpak).find("flatpak run"),
    std::string::npos
  );
}

TEST(HeroicRuntimeSnapshotTests, KeepsNativeAndFlatpakSettingsSeparate) {
  const auto home = lutris_test_root("heroic_runtime_installs");
  const auto native = home / ".config/heroic";
  const auto flatpak = home / ".var/app/com.heroicgameslauncher.hgl/config/heroic";
  for (const auto &root : {native, flatpak}) {
    std::filesystem::create_directories(root / "legendaryConfig/legendary");
    std::filesystem::create_directories(root / "GamesConfig");
    write_text(root / "legendaryConfig/legendary/installed.json", R"({"SameId":{
      "app_name":"SameId","title":"Game","is_dlc":false,"platform":"Windows"}})");
  }
  write_text(native / "GamesConfig/SameId.json", R"({"SameId":{"wineVersion":{"type":"wine","name":"Wine-GE"}}})");
  write_text(flatpak / "GamesConfig/SameId.json", R"({"SameId":{"wineVersion":{"type":"proton","name":"GE-Proton"}}})");
  const auto snapshot = game_library::read_heroic_runtime_snapshot({home});
  ASSERT_EQ(snapshot.size(), 2u);
  const auto &native_runtime = snapshot.at({game_library::launcher_install_t::native, "epic", "SameId"});
  const auto &flatpak_runtime = snapshot.at({game_library::launcher_install_t::flatpak, "epic", "SameId"});
  EXPECT_EQ(native_runtime.platform, "windows");
  EXPECT_EQ(native_runtime.runtime, "wine");
  EXPECT_EQ(native_runtime.runtime_name, "Wine-GE");
  EXPECT_EQ(flatpak_runtime.platform, "windows");
  EXPECT_EQ(flatpak_runtime.runtime, "proton");
  EXPECT_EQ(flatpak_runtime.runtime_name, "GE-Proton");
}

TEST(HeroicRuntimeSnapshotTests, InstalledPlatformWinsOverStaleCacheWithoutNeedingGamesConfig) {
  const auto home = lutris_test_root("heroic_runtime_platform");
  const auto root = home / ".config/heroic";
  std::filesystem::create_directories(root / "legendaryConfig/legendary");
  std::filesystem::create_directories(root / "store_cache");
  write_text(root / "legendaryConfig/legendary/installed.json", R"({"SameId":{
    "app_name":"SameId","title":"Game","is_dlc":false,"platform":"Linux"}})");
  write_text(root / "store_cache/legendary_library.json", R"({"library":[{
    "app_name":"SameId","title":"Game","is_installed":true,"install":{"is_dlc":false,"platform":"Windows"}}]})");
  const auto snapshot = game_library::read_heroic_runtime_snapshot({home});
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(snapshot.begin()->second.platform, "linux");
  EXPECT_EQ(snapshot.begin()->second.runtime, "native");
}

TEST(HeroicRuntimeSnapshotTests, GogUsesInstalledPlatformInsteadOfUninstalledCacheMetadata) {
  const auto home = lutris_test_root("heroic_runtime_gog");
  const auto root = home / ".config/heroic";
  std::filesystem::create_directories(root / "gog_store");
  std::filesystem::create_directories(root / "store_cache");
  write_text(root / "gog_store/installed.json", R"({"installed":[
    {"appName":"123","is_dlc":false,"platform":"linux"}]})");
  write_text(root / "store_cache/gog_library.json", R"({"games":[
    {"app_name":"123","title":"Native Game","install":{"is_dlc":false,"platform":"Windows"}},
    {"app_name":"456","title":"Not Installed","install":{"is_dlc":false,"platform":"Windows"}}]})");
  const auto snapshot = game_library::read_heroic_runtime_snapshot({home});
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(std::get<1>(snapshot.begin()->first), "gog");
  EXPECT_EQ(std::get<2>(snapshot.begin()->first), "123");
  EXPECT_EQ(snapshot.begin()->second.platform, "linux");
  EXPECT_EQ(snapshot.begin()->second.runtime, "native");
}

TEST(HeroicRuntimeSnapshotTests, RefreshesSettingsAndLeavesUnreadableOrMissingRuntimeUnknown) {
  const auto home = lutris_test_root("heroic_runtime_refresh");
  const auto root = home / ".config/heroic";
  std::filesystem::create_directories(root / "store_cache");
  std::filesystem::create_directories(root / "GamesConfig");
  write_text(root / "store_cache/legendary_library.json", R"({"library":[{
    "app_name":"SameId","title":"Game","is_installed":true,"install":{"is_dlc":false,"platform":"Windows"}}]})");
  for (const auto &payload : {std::string {}, std::string {"broken"}, std::string {R"({"SameId":{"wineVersion":{"type":"new-runner","name":"Unknown"}}})"}}) {
    write_text(root / "GamesConfig/SameId.json", payload);
    const auto snapshot = game_library::read_heroic_runtime_snapshot({home});
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot.begin()->second.platform, "windows");
    EXPECT_TRUE(snapshot.begin()->second.runtime.empty());
    EXPECT_TRUE(snapshot.begin()->second.runtime_name.empty());
  }
  write_text(root / "GamesConfig/SameId.json", R"({"SameId":{"wineVersion":{"type":"proton","name":"Updated Proton"}}})");
  const auto refreshed = game_library::read_heroic_runtime_snapshot({home});
  EXPECT_EQ(refreshed.begin()->second.runtime_name, "Updated Proton");
  std::filesystem::remove(root / "GamesConfig/SameId.json");
  const auto missing = game_library::read_heroic_runtime_snapshot({home});
  EXPECT_TRUE(missing.begin()->second.runtime.empty());
  std::filesystem::remove(root / "store_cache/legendary_library.json");
  std::filesystem::create_directory(root / "store_cache/legendary_library.json");
  EXPECT_TRUE(game_library::read_heroic_runtime_snapshot({home}).empty());
}

TEST(HeroicRuntimeSnapshotTests, DoesNotBorrowRuntimeFromAnotherHomeOrImportUninstalledEntries) {
  const auto first = lutris_test_root("heroic_runtime_home1");
  const auto second = lutris_test_root("heroic_runtime_home2");
  std::filesystem::create_directories(first / ".config/heroic/store_cache");
  std::filesystem::create_directories(second / ".config/heroic/GamesConfig");
  write_text(first / ".config/heroic/store_cache/legendary_library.json", R"({"library":[
    {"app_name":"SameId","title":"Game","is_installed":true,"install":{"is_dlc":false,"platform":"Windows"}},
    {"app_name":"NotInstalled","title":"Other","is_installed":false,"install":{"is_dlc":false,"platform":"Linux"}},
    {"app_name":"../escape","title":"Unsafe","is_installed":true,"install":{"is_dlc":false,"platform":"Linux"}}]})");
  write_text(second / ".config/heroic/GamesConfig/SameId.json", R"({"SameId":{"wineVersion":{"type":"wine","name":"Unrelated"}}})");
  const auto snapshot = game_library::read_heroic_runtime_snapshot({first, second});
  ASSERT_EQ(snapshot.size(), 1u);
  EXPECT_EQ(std::get<2>(snapshot.begin()->first), "SameId");
  EXPECT_EQ(snapshot.begin()->second.platform, "windows");
  EXPECT_TRUE(snapshot.begin()->second.runtime.empty());
}

namespace {
  constexpr std::uint32_t k_appinfo_v39_magic = 0x07564427;
  constexpr std::uint32_t k_appinfo_v40_magic = 0x07564428;
  constexpr std::uint32_t k_appinfo_v41_magic = 0x07564429;

  template<class T>
  std::string little_endian(T value) {
    std::string bytes;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      bytes.push_back(static_cast<char>((static_cast<std::uint64_t>(value) >> (8 * i)) & 0xff));
    }
    return bytes;
  }

  /// Writes Steam's binary key/values, with key names inline or in v41's string table.
  class appinfo_writer_t {
  public:
    explicit appinfo_writer_t(std::uint32_t magic): magic_ {magic} {
    }

    std::string name(const std::string &text) {
      if (magic_ != k_appinfo_v41_magic) {
        return text + std::string(1, '\0');
      }
      const auto found = std::find(strings_.begin(), strings_.end(), text);
      const auto index = static_cast<std::uint32_t>(found - strings_.begin());
      if (found == strings_.end()) {
        strings_.push_back(text);
      }
      return little_endian(index);
    }

    std::string map(const std::string &key, const std::string &children) {
      return std::string(1, '\x00') + name(key) + children + std::string(1, '\x08');
    }

    std::string text(const std::string &key, const std::string &value) {
      return std::string(1, '\x01') + name(key) + value + std::string(1, '\0');
    }

    std::string number(const std::string &key, std::int32_t value) {
      return std::string(1, '\x02') + name(key) + little_endian(value);
    }

    std::string entry(std::uint32_t appid, const std::string &vdf) {
      const auto hashes = std::string(20, 'h') + little_endian<std::uint32_t>(1) + (magic_ == k_appinfo_v39_magic ? std::string {} : std::string(20, 'b'));
      const auto body = little_endian<std::uint32_t>(0) + little_endian<std::uint32_t>(0) + little_endian<std::uint64_t>(0) + hashes + vdf;
      return little_endian(appid) + little_endian(static_cast<std::uint32_t>(body.size())) + body;
    }

    std::string file(const std::string &entries) {
      std::string file = little_endian(magic_) + little_endian<std::uint32_t>(1);
      if (magic_ != k_appinfo_v41_magic) {
        return file + entries + little_endian<std::uint32_t>(0);
      }
      const auto offset = file.size() + 8 + entries.size() + 4;
      file += little_endian(static_cast<std::int64_t>(offset)) + entries + little_endian<std::uint32_t>(0);
      file += little_endian(static_cast<std::uint32_t>(strings_.size()));
      for (const auto &text : strings_) {
        file += text + std::string(1, '\0');
      }
      return file;
    }

    /// An app's appinfo with these launch options, as appinfo/config/launch.
    std::string app(const std::vector<std::string> &options) {
      std::string launch;
      for (std::size_t i = 0; i < options.size(); ++i) {
        launch += map(std::to_string(i), options[i]);
      }
      return map("appinfo", number("appid", 1) + map("common", text("name", "A game")) + map("config", text("installdir", "A game") + map("launch", launch)));
    }

    std::string option(const std::string &executable, const std::string &oslist) {
      return text("executable", executable) + text("type", "default") + map("config", text("oslist", oslist));
    }

  private:
    std::uint32_t magic_;
    std::vector<std::string> strings_;
  };

  std::map<std::string, int> counts_of(const std::string &file, const std::set<std::string> &appids) {
    std::istringstream in {file, std::ios::binary};
    return game_library::parse_steam_launch_option_counts(in, appids);
  }
}  // namespace

class SteamLaunchOptionsTest: public ::testing::TestWithParam<std::uint32_t> {};

// Hades as the device session saw it: DirectX, Vulkan and 32-bit, all on Windows, between other apps.
TEST_P(SteamLaunchOptionsTest, CountsEveryOptionSteamCanOfferForTheTitlesAskedFor) {
  appinfo_writer_t writer {GetParam()};
  const auto hades = writer.app({
    writer.option("x64/Hades.exe", "windows"),
    writer.option("x64Vk/Hades.exe", "windows"),
    writer.option("x86/Hades.exe", "windows"),
  });
  const auto one = writer.app({writer.option("Game.exe", "windows")});
  const auto not_asked = writer.app({writer.option("a", "windows"), writer.option("b", "windows")});
  const auto file = writer.file(writer.entry(10, not_asked) + writer.entry(1145360, hades) + writer.entry(20, one));

  const auto counts = counts_of(file, {"1145360", "20", "999"});
  EXPECT_EQ(counts.at("1145360"), 3);
  EXPECT_EQ(counts.at("20"), 1);
  // Not asked for, so not parsed, and absent from the file, so unknown rather than zero.
  EXPECT_FALSE(counts.contains("10"));
  EXPECT_FALSE(counts.contains("999"));
}

// Every launch option of Hades (1145360) as a host's appinfo.vdf lists them, with only the fields
// that decide what Steam offers. On a 64-bit desktop Steam's dialog offered three: SELECT LAUNCH
// OPTION (3) with DirectX, Vulkan and 32-bit, which are the first, 14th and 15th rows here.
TEST_P(SteamLaunchOptionsTest, CountsTheThreeOptionsSteamOffersForHades) {
  struct row_t {
    const char *description, *oslist, *osarch, *beta, *steam_deck;
  };
  const std::vector<row_t> rows {
    {"Hades (DirectX - Default)", "windows", "64", "", ""},
    {"32-bit", "windows", "32", "", ""},
    {"Debug", "windows", "64", "nightly-qa", ""},
    {"Debug x86", "windows", "", "nightly-qa", ""},
    {"Debug", "windows", "64", "prev-nightly", ""},
    {"Debug", "windows", "32", "prev-nightly", ""},
    {"Release", "windows", "64", "nightly-qa", ""},
    {"Ship", "windows", "64", "nightly-qa", ""},
    {"Play Hades", "windows", "64", "techbeta", ""},
    {"", "macos", "", "", ""},
    {"Vulkan Debug", "windows", "64", "nightly-qa", ""},
    {"Vulkan Release", "windows", "64", "nightly-qa", ""},
    {"Vulkan Ship", "windows", "64", "nightly-qa", ""},
    {"Hades (Vulkan)", "windows", "64", "", "0"},
    {"Hades (32-bit)", "windows", "64", "", "0"},
    {"Hades (Vulkan - Steam Deck)", "windows", "64", "", "1"},
    {"Hades (Apple Silicon)", "macos", "", "nightly-qa", ""},
    {"Hades (Intel Mac)", "macos", "", "nightly-qa", ""}
  };
  appinfo_writer_t writer {GetParam()};
  std::vector<std::string> options;
  for (const auto &row : rows) {
    auto config = writer.text("oslist", row.oslist);
    if (*row.osarch) config += writer.text("osarch", row.osarch);
    if (*row.beta) config += writer.text("BetaKey", row.beta);
    if (*row.steam_deck) config += writer.text("steamdeck", row.steam_deck);
    options.push_back(writer.text("description", row.description) + writer.map("config", config));
  }
  EXPECT_EQ(rows.size(), 18u);
  EXPECT_EQ(counts_of(writer.file(writer.entry(1145360, writer.app(options))), {"1145360"}).at("1145360"), 3);
}

// One build for each kind of system, not a choice for the player.
TEST_P(SteamLaunchOptionsTest, LeavesOutA32BitBuildAndCountsSteamDecksOwnApart) {
  appinfo_writer_t writer {GetParam()};
  const auto bitness = writer.app({
    writer.map("config", writer.text("oslist", "windows") + writer.text("osarch", "64")),
    writer.map("config", writer.text("oslist", "windows") + writer.text("osarch", "32")),
  });
  const auto per_device = writer.app({
    writer.map("config", writer.text("oslist", "windows") + writer.text("steamdeck", "0")),
    writer.map("config", writer.text("oslist", "windows") + writer.text("steamdeck", "1")),
  });
  const auto integer_flag = writer.app({
    writer.map("config", writer.text("oslist", "windows") + writer.number("steamdeck", 0)),
    writer.map("config", writer.text("oslist", "windows") + writer.number("steamdeck", 1)),
  });
  const auto counts = counts_of(writer.file(writer.entry(1, bitness) + writer.entry(2, per_device) + writer.entry(3, integer_flag)), {"1", "2", "3"});
  EXPECT_EQ(counts.at("1"), 1);
  EXPECT_EQ(counts.at("2"), 1);
  EXPECT_EQ(counts.at("3"), 1);
}

TEST_P(SteamLaunchOptionsTest, ReadsSteamsKeysInWhateverCaseATitleWritesThem) {
  appinfo_writer_t writer {GetParam()};
  const auto gated = writer.app({
    writer.option("Game.exe", "windows"),
    writer.map("config", writer.text("oslist", "windows") + writer.text("BetaKey", "nightly")),
    writer.map("config", writer.text("OSList", "macos")),
    writer.map("config", writer.text("oslist", "windows") + writer.text("OwnsDlc", "12345")),
  });
  EXPECT_EQ(counts_of(writer.file(writer.entry(1, gated)), {"1"}).at("1"), 1);
}

TEST_P(SteamLaunchOptionsTest, LeavesOutOptionsThatNeedABetaOrADlc) {
  appinfo_writer_t writer {GetParam()};
  const auto gated = writer.app({
    writer.option("Game.exe", "windows"),
    writer.text("executable", "Beta.exe") + writer.map("config", writer.text("oslist", "windows") + writer.text("betakey", "public-beta")),
    writer.text("executable", "Dlc.exe") + writer.map("config", writer.text("oslist", "windows") + writer.text("ownsdlc", "12345")),
    writer.text("executable", "Dlc2.exe") + writer.map("config", writer.text("oslist", "windows") + writer.number("ownsdlc", 12345)),
  });
  EXPECT_EQ(counts_of(writer.file(writer.entry(1, gated)), {"1"}).at("1"), 1);
}

TEST_P(SteamLaunchOptionsTest, CountsTheLargerPlatformSetBecauseEitherCanRun) {
  appinfo_writer_t writer {GetParam()};
  const auto split = writer.app({writer.option("a.exe", "windows"), writer.option("b.exe", "windows"), writer.option("c", "linux")});
  const auto both = writer.app({writer.text("executable", "a"), writer.option("b", "linux,windows")});
  const auto counts = counts_of(writer.file(writer.entry(1, split) + writer.entry(2, both)), {"1", "2"});
  EXPECT_EQ(counts.at("1"), 2);
  EXPECT_EQ(counts.at("2"), 2);
}

TEST_P(SteamLaunchOptionsTest, ReportsNothingForAnEntryItCannotUnderstand) {
  appinfo_writer_t writer {GetParam()};
  const auto good = writer.app({writer.option("a", "windows"), writer.option("b", "windows")});
  // An unknown value type stops that entry; the next one still reads.
  const auto unknown_type = writer.map("appinfo", std::string(1, '\x7f') + writer.name("mystery"));
  const auto file = writer.file(writer.entry(1, unknown_type) + writer.entry(2, good));
  const auto counts = counts_of(file, {"1", "2"});
  EXPECT_FALSE(counts.contains("1"));
  EXPECT_EQ(counts.at("2"), 2);
  // A nested map that never closes is not a complete entry.
  auto open_ended = writer.app({writer.option("a", "windows")});
  open_ended.pop_back();
  open_ended.pop_back();
  EXPECT_FALSE(counts_of(writer.file(writer.entry(3, open_ended)), {"3"}).contains("3"));
}

TEST_P(SteamLaunchOptionsTest, StopsAtATruncatedFileWithoutReadingPastIt) {
  appinfo_writer_t writer {GetParam()};
  const auto app = writer.app({writer.option("a", "windows"), writer.option("b", "windows")});
  const auto file = writer.file(writer.entry(1, app));
  for (std::size_t length = 0; length < 60 && length < file.size(); ++length) {
    EXPECT_FALSE(counts_of(file.substr(0, length), {"1"}).contains("1")) << length;
  }
}

INSTANTIATE_TEST_SUITE_P(EachAppinfoVersion, SteamLaunchOptionsTest, ::testing::Values(k_appinfo_v39_magic, k_appinfo_v40_magic, k_appinfo_v41_magic));

TEST(SteamLaunchOptions, RefusesAFileOfAnotherFormat) {
  EXPECT_TRUE(counts_of("", {"1"}).empty());
  EXPECT_TRUE(counts_of(little_endian<std::uint32_t>(0x12345678) + little_endian<std::uint32_t>(1) + little_endian<std::uint32_t>(0), {"1"}).empty());
}

TEST(SteamLaunchOptions, AV41KeyOutsideTheStringTableReportsNothing) {
  appinfo_writer_t writer {k_appinfo_v41_magic};
  const auto name_past_the_table = std::string(1, '\x00') + little_endian<std::uint32_t>(9999) + std::string(1, '\x08');
  EXPECT_FALSE(counts_of(writer.file(writer.entry(1, name_past_the_table)), {"1"}).contains("1"));
}

#ifndef _WIN32
TEST(SteamLaunchOptions, ReadsThisHostsFileWithoutChangingItAndNoticesWhenSteamDoes) {
  const auto home = test_paths::root() / "steam_launch_options_home";
  std::filesystem::remove_all(home);
  const auto appcache = home / ".local" / "share" / "Steam" / "appcache";
  std::filesystem::create_directories(appcache);
  const auto path = appcache / "appinfo.vdf";

  appinfo_writer_t writer {k_appinfo_v41_magic};
  const auto hades = writer.file(writer.entry(1145360, writer.app({writer.option("a", "windows"), writer.option("b", "windows"), writer.option("c", "windows")})));
  std::ofstream(path, std::ios::binary) << hades;
  // Ahead of any real Steam under the account's own home, which the lookup also reads.
  const auto newest = std::filesystem::file_time_type::clock::now() + std::chrono::hours(24);
  std::filesystem::last_write_time(path, newest);

  const char *previous = std::getenv("HOME");
  const std::string saved_home = previous ? previous : "";
  ASSERT_EQ(setenv("HOME", home.c_str(), 1), 0);

  EXPECT_EQ(game_library::steam_launch_option_counts({"1145360"}).at("1145360"), 3);
  EXPECT_TRUE(game_library::steam_launch_option_counts({}).empty());
  std::ifstream written(path, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(written), {}), hades);
  EXPECT_EQ(std::filesystem::last_write_time(path), newest);

  // Steam rewriting the file is noticed, and a smaller answer replaces the larger one.
  appinfo_writer_t rewriter {k_appinfo_v41_magic};
  std::ofstream(path, std::ios::binary | std::ios::trunc) << rewriter.file(rewriter.entry(1145360, rewriter.app({rewriter.option("a", "windows")})));
  std::filesystem::last_write_time(path, newest + std::chrono::hours(1));
  EXPECT_EQ(game_library::steam_launch_option_counts({"1145360"}).at("1145360"), 1);

  if (previous) {
    setenv("HOME", saved_home.c_str(), 1);
  } else {
    unsetenv("HOME");
  }
  std::filesystem::remove_all(home);
}
#endif

TEST(SteamLaunchOptions, NotesOnlyATitleSteamHasSomethingToAskAbout) {
  EXPECT_EQ(game_library::steam_launch_choice_note(0), "");
  EXPECT_EQ(game_library::steam_launch_choice_note(1), "");
  const auto note = game_library::steam_launch_choice_note(3);
  EXPECT_NE(note.find("may ask"), std::string::npos);
  EXPECT_NE(note.find("several launch options"), std::string::npos);
}
