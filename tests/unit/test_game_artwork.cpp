#include <gtest/gtest.h>

#include <src/game_artwork.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {
  namespace fs = std::filesystem;

  constexpr std::string_view GAME_UUID = "123e4567-e89b-12d3-a456-426614174000";

  struct temp_dir_t {
    fs::path path;

    explicit temp_dir_t(std::string_view name):
        path(fs::temp_directory_path() / ("polaris-game-artwork-" + std::string(name))) {
      std::error_code ec;
      fs::remove_all(path, ec);
      fs::create_directories(path);
    }

    ~temp_dir_t() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  };

  void write_bytes(const fs::path &path, const std::vector<unsigned char> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.good());
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good());
  }

  void write_png(const fs::path &path, unsigned char marker = 0) {
    write_bytes(path, {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', marker});
  }

  void write_jpeg(const fs::path &path, unsigned char marker = 0) {
    write_bytes(path, {0xff, 0xd8, 0xff, 0xe0, marker});
  }
}  // namespace

TEST(GameArtworkPaths, StrictlyValidatesUuidKindAndRequestTargets) {
  EXPECT_TRUE(game_artwork::is_valid_uuid(GAME_UUID));
  EXPECT_TRUE(game_artwork::is_valid_uuid("123E4567-E89B-12D3-A456-426614174000"));
  EXPECT_FALSE(game_artwork::is_valid_uuid("../123e4567-e89b-12d3-a456-426614174000"));
  EXPECT_FALSE(game_artwork::is_valid_uuid("123e4567-e89b-12d3-a456-426614174000/.."));
  EXPECT_FALSE(game_artwork::is_valid_uuid("123e4567e89b12d3a456426614174000"));

  EXPECT_TRUE(game_artwork::parse_kind("poster").has_value());
  EXPECT_TRUE(game_artwork::parse_kind("hero").has_value());
  EXPECT_TRUE(game_artwork::parse_kind("logo").has_value());
  EXPECT_TRUE(game_artwork::parse_kind("icon").has_value());
  EXPECT_FALSE(game_artwork::parse_kind("cover").has_value());
  EXPECT_FALSE(game_artwork::parse_kind("../poster").has_value());
  EXPECT_FALSE(game_artwork::parse_kind("poster.png").has_value());

  const auto parsed = game_artwork::parse_asset_request_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/hero");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->uuid, GAME_UUID);
  EXPECT_EQ(parsed->kind, game_artwork::kind_e::hero);

  EXPECT_FALSE(game_artwork::parse_asset_request_target(
    "/polaris/v1/games/../../etc/passwd/artwork/poster").has_value());
  EXPECT_FALSE(game_artwork::parse_asset_request_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/poster/../../secret").has_value());
  EXPECT_FALSE(game_artwork::parse_asset_request_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/%2e%2e").has_value());
}

TEST(GameArtworkPaths, UsesDeterministicCachePathsWithoutAcceptingTraversal) {
  const fs::path appdata = "/var/lib/polaris";
  EXPECT_EQ(game_artwork::cache_root(appdata), appdata / "artwork" / "v1");

  const auto valid = game_artwork::cache_asset_path(
    appdata, GAME_UUID, game_artwork::kind_e::poster, game_artwork::source_e::local, ".png");
  ASSERT_TRUE(valid.has_value());
  EXPECT_EQ(*valid, appdata / "artwork" / "v1" / GAME_UUID / "poster.local.png");

  EXPECT_FALSE(game_artwork::cache_asset_path(
    appdata, "../../etc", game_artwork::kind_e::poster, game_artwork::source_e::local, ".png").has_value());
  EXPECT_FALSE(game_artwork::cache_asset_path(
    appdata, GAME_UUID, game_artwork::kind_e::poster, game_artwork::source_e::local, ".png/../../secret").has_value());
}

TEST(GameArtworkAllowlist, RequiresHttpsAndExactProviderHosts) {
  using game_artwork::provider_e;
  EXPECT_TRUE(game_artwork::is_allowed_provider_url(
    provider_e::steam, "https://cdn.akamai.steamstatic.com/steam/apps/10/library_hero.jpg"));
  EXPECT_TRUE(game_artwork::is_allowed_provider_url(
    provider_e::steamgriddb, "https://cdn2.steamgriddb.com/hero/abc.jpg"));
  EXPECT_TRUE(game_artwork::is_allowed_provider_url(
    provider_e::steamgriddb, "https://www.steamgriddb.com/api/v2/heroes/game/1"));

  EXPECT_FALSE(game_artwork::is_allowed_provider_url(
    provider_e::steam, "http://cdn.akamai.steamstatic.com/steam/apps/10/library_hero.jpg"));
  EXPECT_FALSE(game_artwork::is_allowed_provider_url(
    provider_e::steam, "https://cdn.akamai.steamstatic.com.evil.example/steam/apps/10/library_hero.jpg"));
  EXPECT_FALSE(game_artwork::is_allowed_provider_url(
    provider_e::steam, "https://evil-cdn.akamai.steamstatic.com/steam/apps/10/library_hero.jpg"));
  EXPECT_FALSE(game_artwork::is_allowed_provider_url(
    provider_e::steam, "https://cdn.akamai.steamstatic.com@evil.example/steam/apps/10/library_hero.jpg"));
  EXPECT_FALSE(game_artwork::is_allowed_provider_url(
    provider_e::steamgriddb, "https://cdn2.steamgriddb.com:443/hero/abc.jpg"));
}

TEST(GameArtworkLocalPoster, PrefersConfiguredImageThenHostUuidThenLegacySteamCover) {
  temp_dir_t temp("legacy-selection");
  const auto covers = temp.path / "covers";
  const auto configured = temp.path / "configured.png";
  const auto uuid_cover = covers / (std::string(GAME_UUID) + ".webp");
  const auto steam_cover = covers / "steam_620.jpg";
  write_png(configured);
  write_bytes(uuid_cover, {0x52, 0x49, 0x46, 0x46, 0, 0, 0, 0, 0x57, 0x45, 0x42, 0x50});
  write_jpeg(steam_cover);

  auto selected = game_artwork::select_legacy_poster(covers, GAME_UUID, "620", configured);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->path, configured);
  EXPECT_EQ(selected->source, game_artwork::source_e::local);

  fs::remove(configured);
  selected = game_artwork::select_legacy_poster(covers, GAME_UUID, "620", configured);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->path, uuid_cover);
  EXPECT_EQ(selected->source, game_artwork::source_e::host);

  fs::remove(uuid_cover);
  selected = game_artwork::select_legacy_poster(covers, GAME_UUID, "620", configured);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->path, steam_cover);
  EXPECT_EQ(selected->source, game_artwork::source_e::steam);
}

TEST(GameArtworkLocalPoster, PromotesLocalOverCachedSteamGridDb) {
  temp_dir_t temp("priority-upgrade");
  const auto old = game_artwork::cache_asset_path(temp.path, GAME_UUID, game_artwork::kind_e::poster, game_artwork::source_e::steamgriddb, ".jpg");
  ASSERT_TRUE(old.has_value());
  write_jpeg(*old, 1);
  const auto configured = temp.path / "configured.png";
  write_png(configured, 2);
  game_artwork::resolve_request_t request {.appdata = temp.path, .uuid = std::string(GAME_UUID)};
  request.local_posters.push_back({configured, game_artwork::source_e::local});
  const auto assets = game_artwork::resolve_missing_assets(request);
  const auto selected = game_artwork::find_cached_asset(temp.path, GAME_UUID, game_artwork::kind_e::poster);
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->source, game_artwork::source_e::local);
  EXPECT_EQ(assets.front().source, game_artwork::source_e::local);
  EXPECT_TRUE(fs::exists(*old));
}

TEST(GameArtworkManifest, SanitizesAssetMetadataAndSupportsPartialSuccess) {
  temp_dir_t temp("partial-manifest");
  const auto hero_path = temp.path / "artwork" / "v1" / GAME_UUID / "hero.steam.jpg";
  write_jpeg(hero_path);

  const std::vector<game_artwork::asset_t> assets {{
    .kind = game_artwork::kind_e::hero,
    .source = game_artwork::source_e::steam,
    .path = hero_path,
    .mime_type = "image/jpeg",
  }};
  const auto manifest = game_artwork::make_manifest(GAME_UUID, assets);

  EXPECT_EQ(manifest.at("version"), 1);
  EXPECT_TRUE(manifest.at("revision").is_string());
  EXPECT_EQ(manifest.at("state"), "partial");
  EXPECT_GT(manifest.at("cached_at").get<std::int64_t>(), 0);
  ASSERT_TRUE(manifest.at("override").is_object());
  EXPECT_EQ(manifest.at("override").at("active"), false);
  ASSERT_EQ(manifest.at("assets").size(), 1);
  ASSERT_TRUE(manifest.at("assets").contains("hero"));
  const auto &hero = manifest.at("assets").at("hero");
  EXPECT_EQ(hero.at("url"), "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/hero");
  EXPECT_EQ(hero.at("source"), "steam");
  EXPECT_EQ(hero.at("mime_type"), "image/jpeg");
  EXPECT_EQ(hero.at("cached"), true);
  EXPECT_FALSE(manifest.at("assets").contains("poster"));

  const auto serialized = manifest.dump();
  EXPECT_EQ(serialized.find(temp.path.string()), std::string::npos);
  EXPECT_EQ(serialized.find("steamgriddb_api_key"), std::string::npos);
  EXPECT_EQ(serialized.find("https://"), std::string::npos);
}

TEST(GameArtworkManifest, EmitsFallbackStateForEmptyCache) {
  const auto manifest = game_artwork::make_manifest(GAME_UUID, {});
  EXPECT_EQ(manifest.at("state"), "fallback");
  EXPECT_EQ(manifest.at("cached_at"), 0);
  EXPECT_EQ(manifest.at("override").at("active"), false);
  EXPECT_TRUE(manifest.at("assets").empty());
}

TEST(GameArtworkManifest, DetectsMimeAndKeepsRevisionStableUntilContentChanges) {
  temp_dir_t temp("mime-revision");
  const auto appdata = temp.path;
  const auto path = game_artwork::cache_asset_path(
    appdata, GAME_UUID, game_artwork::kind_e::poster, game_artwork::source_e::host, ".png");
  ASSERT_TRUE(path.has_value());
  write_png(*path, 1);

  auto assets = game_artwork::scan_cached_assets(appdata, GAME_UUID);
  ASSERT_EQ(assets.size(), 1);
  EXPECT_EQ(assets[0].mime_type, "image/png");
  const auto first = game_artwork::make_manifest(GAME_UUID, assets);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto again = game_artwork::make_manifest(
    GAME_UUID, game_artwork::scan_cached_assets(appdata, GAME_UUID));
  EXPECT_EQ(first.at("revision"), again.at("revision"));
  EXPECT_EQ(first.at("cached_at"), again.at("cached_at"));

  write_png(*path, 2);
  const auto changed = game_artwork::make_manifest(
    GAME_UUID, game_artwork::scan_cached_assets(appdata, GAME_UUID));
  EXPECT_NE(first.at("revision"), changed.at("revision"));

  write_bytes(*path, {'n', 'o', 't', '-', 'a', 'n', '-', 'i', 'm', 'a', 'g', 'e'});
  EXPECT_TRUE(game_artwork::scan_cached_assets(appdata, GAME_UUID).empty());
}
