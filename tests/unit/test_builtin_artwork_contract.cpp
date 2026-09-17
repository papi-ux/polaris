/**
 * @file tests/unit/test_builtin_artwork_contract.cpp
 * @brief Built-in utility entries keep their packaged identity instead of
 *        being guessed as similarly named games by an automatic provider.
 */
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {
  std::string read_nvhttp_source() {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / "src/nvhttp.cpp");
    EXPECT_TRUE(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  std::string function_body(const std::string &source, const std::string &signature) {
    const auto start = source.find(signature);
    EXPECT_NE(start, std::string::npos) << signature;
    if (start == std::string::npos) return {};
    const auto next = source.find("\n    }\n", start);
    EXPECT_NE(next, std::string::npos) << signature;
    return next == std::string::npos ? std::string {} : source.substr(start, next - start);
  }
}  // namespace

TEST(BuiltinArtworkContract, ResolvesThePackagedPosterAndRetiresOnlyAutomaticMatches) {
  const auto source = read_nvhttp_source();
  const auto policy = function_body(source, "bool uses_bundled_utility_artwork(");
  const auto configured = function_body(source, "fs::path configured_artwork_image(");
  const auto promotion = function_body(source, "void promote_local_artwork_poster(");
  ASSERT_FALSE(policy.empty());
  ASSERT_FALSE(configured.empty());
  ASSERT_FALSE(promotion.empty());

  EXPECT_NE(policy.find("VIRTUAL_DISPLAY_UUID"), std::string::npos);
  // Desktop entries are not games either (Low Res Desktop once took Low Magic Age's artwork).
  EXPECT_NE(policy.find("app.desktop_mirror"), std::string::npos);
  // An upgraded host's apps.json predates the flag, so an entry that launches nothing counts too.
  EXPECT_NE(policy.find("proc::launches_nothing(app)"), std::string::npos);
  EXPECT_NE(configured.find("proc::validate_app_image_path"), std::string::npos);
  // A replaced or retargeted image refreshes its copy; a cleared one retires it.
  EXPECT_NE(promotion.find("game_artwork::local_poster_needs_copy(appdata, app.uuid, candidates.front())"), std::string::npos);
  EXPECT_EQ(promotion.find("needs_source_upgrade"), std::string::npos);
  EXPECT_NE(promotion.find("retire_orphaned_local_poster(appdata, app.uuid, candidates)"), std::string::npos);
  EXPECT_NE(promotion.find("candidate_already_cached"), std::string::npos);
  EXPECT_NE(promotion.find("bundled_utility && !candidate_already_cached"), std::string::npos);
  EXPECT_NE(promotion.find("remove_cached_source_assets"), std::string::npos);
  EXPECT_NE(promotion.find("game_artwork::source_e::steamgriddb"), std::string::npos);
  // The automatic cache is retired whether or not the bundled poster could be copied.
  EXPECT_EQ(promotion.find("bundled_poster_ready"), std::string::npos);
  EXPECT_EQ(promotion.find("game_artwork::source_e::override"), std::string::npos);
}

TEST(BuiltinArtworkContract, AutomaticResolutionDoesNotSearchGamesForUtilityEntries) {
  const auto source = read_nvhttp_source();
  const auto handler = source.find("auto polarisResolveGameArtwork =");
  ASSERT_NE(handler, std::string::npos);
  const auto next_handler = source.find("auto polarisSearchGameArtworkMatches =", handler);
  ASSERT_NE(next_handler, std::string::npos);
  const auto body = source.substr(handler, next_handler - handler);

  EXPECT_NE(body.find("bundled_utility && kind != game_artwork::kind_e::poster"), std::string::npos);
  EXPECT_NE(body.find("any_kind_missing && !bundled_utility"), std::string::npos);
  // The exact app id lookup, then only a result carrying the entry's own title; never the first hit.
  EXPECT_NE(body.find("automatic_steamgriddb_game(app->name, app->steam_appid, transport)"), std::string::npos);
  EXPECT_EQ(body.find("parse_steamgriddb_game_id"), std::string::npos);
  // Remove artwork turns automatic lookup off for Steam and SteamGridDB alike.
  EXPECT_NE(body.find("automatic_artwork_lookup_enabled(appdata, app->uuid)"), std::string::npos);
  EXPECT_NE(body.find("automatic_lookup && game_artwork::is_valid_steam_appid"), std::string::npos);
  EXPECT_NE(body.find("!automatic_lookup ||"), std::string::npos);
  EXPECT_NE(body.find("artwork_manifest_for(appdata, *app)"), std::string::npos);
}

TEST(BuiltinArtworkContract, UtilityEntriesNeverAdvertiseOrServeAnAutomaticMatch) {
  const auto source = read_nvhttp_source();
  const auto manifest = function_body(source, "nlohmann::json artwork_manifest_for(");
  ASSERT_FALSE(manifest.empty());
  EXPECT_NE(manifest.find("uses_bundled_utility_artwork(app)"), std::string::npos);
  EXPECT_NE(manifest.find("asset.source == game_artwork::source_e::steamgriddb"), std::string::npos);
  EXPECT_NE(source.find("game[\"artwork\"] = artwork_manifest_for(platf::appdata(), app);"), std::string::npos);

  const auto handler = source.find("auto polarisGameArtwork =");
  ASSERT_NE(handler, std::string::npos);
  const auto next_handler = source.find("auto polarisResolveGameArtwork =", handler);
  ASSERT_NE(next_handler, std::string::npos);
  const auto asset_route = source.substr(handler, next_handler - handler);
  EXPECT_NE(asset_route.find("asset->source == game_artwork::source_e::steamgriddb && uses_bundled_utility_artwork(*app)"),
            std::string::npos);
}
