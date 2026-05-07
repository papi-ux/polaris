#include "../tests_common.h"
#include "../tests_paths.h"

#include <src/game_library_scanner.h>

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

  const auto games = game_library::scan_lutris_games({config_dir, data_dir});
  ASSERT_EQ(games.size(), 2);
  EXPECT_EQ(games[0].slug, "config-game");
  EXPECT_EQ(games[1].slug, "data-game");
  EXPECT_EQ(games[1].runner, "wine");
}

TEST(LutrisLibraryScannerTests, ParsesLutrisListGamesJson) {
  const auto games = game_library::parse_lutris_list_games_json(R"json([
    {"id": 3, "slug": "arc-raiders", "name": "ARC Raiders", "runner": "steam", "coverPath": "/home/papi/.local/share/lutris/coverart/arc-raiders.jpg"},
    {"id": 14, "slug": "vam-vr", "name": "VaM (VR)", "runner": "wine"},
    {"id": 99, "slug": "bad slug", "name": "Unsafe", "runner": "linux"},
    {"id": 100, "slug": "missing-name", "runner": "linux"}
  ])json");

  ASSERT_EQ(games.size(), 2);
  EXPECT_EQ(games[0].name, "ARC Raiders");
  EXPECT_EQ(games[0].slug, "arc-raiders");
  EXPECT_EQ(games[0].runner, "steam");
  EXPECT_EQ(games[0].command, "setsid lutris lutris:rungame/arc-raiders");
  EXPECT_EQ(games[0].image_path, "/home/papi/.local/share/lutris/coverart/arc-raiders.jpg");
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
