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
  EXPECT_NE(configured.find("proc::validate_app_image_path"), std::string::npos);
  EXPECT_NE(promotion.find("game_artwork::source_e::local"), std::string::npos);
  EXPECT_NE(promotion.find("remove_cached_source_assets"), std::string::npos);
  EXPECT_NE(promotion.find("game_artwork::source_e::steamgriddb"), std::string::npos);
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
  EXPECT_NE(body.find("plan_steamgriddb_search(app->name)"), std::string::npos);
}
