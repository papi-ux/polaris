#include <gtest/gtest.h>

#include <src/game_artwork_manual.h>

#include <array>
#include <string>
#include <vector>

namespace {
  constexpr std::string_view GAME_UUID = "123e4567-e89b-12d3-a456-426614174000";

  std::vector<unsigned char> png(unsigned char marker = 0) {
    return {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, marker};
  }

  std::vector<unsigned char> jpeg(unsigned char marker = 0) {
    return {0xff, 0xd8, 0xff, marker};
  }
}

TEST(GameArtworkManualRoutes, AcceptsOnlyExactSanitizedTargets) {
  using game_artwork::manual::route_e;
  auto route = game_artwork::manual::parse_route_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidates");
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->route, route_e::search);
  EXPECT_EQ(route->uuid, GAME_UUID);

  route = game_artwork::manual::parse_route_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/match");
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->route, route_e::apply);

  route = game_artwork::manual::parse_route_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/override");
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->route, route_e::clear);

  route = game_artwork::manual::parse_route_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidate/0123456789abcdef0123456789abcdef/poster");
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->route, route_e::preview);
  EXPECT_EQ(route->token, "0123456789abcdef0123456789abcdef");
  EXPECT_EQ(route->kind, game_artwork::kind_e::poster);

  EXPECT_FALSE(game_artwork::manual::parse_route_target(
    "/polaris/v1/games/../../etc/artwork/candidates").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_route_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidate/not-a-token/poster").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_route_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidate/extra").has_value());
}

TEST(GameArtworkManualSelection, AcceptsOnlySanitizedSteamGridDbIdentity) {
  const auto query = game_artwork::manual::sanitize_search_query("  Portal 2  ");
  ASSERT_TRUE(query.has_value());
  EXPECT_EQ(*query, "Portal 2");
  EXPECT_FALSE(game_artwork::manual::sanitize_search_query("Bad\nTitle").has_value());
  EXPECT_FALSE(game_artwork::manual::sanitize_search_query("Bad\u0085Title").has_value());
  EXPECT_FALSE(game_artwork::manual::sanitize_search_query(
    std::string(game_artwork::manual::maximum_search_query_bytes + 1, 'x')).has_value());

  const auto selected = game_artwork::manual::parse_match_selection(R"({
    "provider":"steamgriddb",
    "provider_game_id":"12345",
    "title":"Portal 2",
    "steam_appid":"620",
    "kinds":["poster","hero"]
  })");
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(selected->provider, "steamgriddb");
  EXPECT_EQ(selected->provider_game_id, "12345");
  EXPECT_EQ(selected->title, "Portal 2");
  EXPECT_EQ(selected->steam_appid, "620");
  EXPECT_EQ(selected->kinds, (std::vector<game_artwork::kind_e> {
    game_artwork::kind_e::poster, game_artwork::kind_e::hero}));

  EXPECT_FALSE(game_artwork::manual::parse_match_selection(
    R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","kinds":["poster"],"api_key":"secret"})").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_match_selection(
    R"({"provider":"evil","provider_game_id":"12345","title":"Portal 2"})").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_match_selection(
    R"({"provider":"steamgriddb","provider_game_id":"../1","title":"Portal 2"})").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_match_selection(
    R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Bad\u0085Title"})").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_match_selection(
    R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","kinds":[]})").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_match_selection(
    R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","kinds":["poster","poster"]})").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_match_selection(
    R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","kinds":["trailer"]})").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_match_selection("{not-json").has_value());
  EXPECT_FALSE(game_artwork::manual::parse_match_selection(
    std::string(game_artwork::manual::maximum_match_body_bytes + 1, 'x')).has_value());
}

TEST(GameArtworkManualLogging, RedactsCredentialFieldsAndOpaquePreviewTokens) {
  EXPECT_EQ(game_artwork::manual::request_log_value("Authorization", "Bearer secret"), "[REDACTED]");
  EXPECT_EQ(game_artwork::manual::request_log_value("X-Api-Key", "secret"), "[REDACTED]");
  EXPECT_EQ(game_artwork::manual::request_log_value("apiKey", "secret"), "[REDACTED]");
  EXPECT_EQ(game_artwork::manual::request_log_value("password", "secret"), "[REDACTED]");
  EXPECT_EQ(game_artwork::manual::request_log_value("cookie", "session=secret"), "[REDACTED]");
  EXPECT_EQ(game_artwork::manual::request_log_value("query", "Portal 2"), "Portal 2");
  const std::string preview_path =
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidate/0123456789abcdef0123456789abcdef/poster";
  EXPECT_EQ(game_artwork::manual::request_log_path(preview_path),
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidate/[REDACTED]/poster");
  EXPECT_EQ(game_artwork::manual::request_log_path(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidate/0123456789abcdef0123456789abcdef"),
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidate/[REDACTED]");
  EXPECT_EQ(game_artwork::manual::request_log_path(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidates"),
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidates");
  EXPECT_EQ(game_artwork::manual::request_log_target(
    preview_path, {{"token", "query-secret"}, {"query", "Portal 2"}}),
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/candidate/[REDACTED]/poster?token=[REDACTED]&query=Portal 2");
}

TEST(GameArtworkManualSearchFailure, NamesTheCauseWithAStableCode) {
  using game_artwork::manual::classify_search_failure;
  const auto missing = classify_search_failure(false, std::nullopt);
  EXPECT_EQ(missing.code, "steamgriddb_key_missing");
  EXPECT_EQ(missing.http_status, 503);
  EXPECT_NE(missing.message.find("Polaris settings"), std::string::npos);

  const auto rejected = classify_search_failure(true, 401L);
  EXPECT_EQ(rejected.code, "steamgriddb_unauthorized");
  EXPECT_EQ(rejected.http_status, 502);
  EXPECT_NE(rejected.message.find("rejected"), std::string::npos);
  EXPECT_EQ(classify_search_failure(true, 403L).code, "steamgriddb_unauthorized");
  EXPECT_EQ(classify_search_failure(true, 429L).code, "steamgriddb_rate_limited");
  EXPECT_EQ(classify_search_failure(true, 500L).code, "steamgriddb_unavailable");
  EXPECT_NE(classify_search_failure(true, 500L).message.find("500"), std::string::npos);
  EXPECT_EQ(classify_search_failure(true, std::nullopt).code, "steamgriddb_unreachable");
}

TEST(GameArtworkManualPreviewCache, IsBoundedScopedOpaqueAndExpiring) {
  const std::array tokens {
    std::string("00000000000000000000000000000001"),
    std::string("00000000000000000000000000000002"),
    std::string("00000000000000000000000000000003"),
  };
  std::size_t token_index = 0;
  game_artwork::manual::preview_cache_t cache(2, 32, 1000, [&] { return tokens.at(token_index++); });

  const auto first = cache.publish(GAME_UUID, game_artwork::kind_e::poster, png(1), 0);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->mime_type, "image/png");
  EXPECT_FALSE(first->token.find("Portal") != std::string::npos);
  EXPECT_TRUE(cache.lookup(GAME_UUID, first->token, game_artwork::kind_e::poster, 999).has_value());
  EXPECT_FALSE(cache.lookup("00000000-0000-0000-0000-000000000000", first->token,
                            game_artwork::kind_e::poster, 999).has_value());
  EXPECT_FALSE(cache.lookup(GAME_UUID, first->token, game_artwork::kind_e::hero, 999).has_value());

  const auto second = cache.publish(GAME_UUID, game_artwork::kind_e::hero, jpeg(2), 1);
  const auto third = cache.publish(GAME_UUID, game_artwork::kind_e::logo, png(3), 2);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(cache.size(), 2);
  EXPECT_FALSE(cache.lookup(GAME_UUID, first->token, game_artwork::kind_e::poster, 2).has_value());
  EXPECT_TRUE(cache.lookup(GAME_UUID, second->token, game_artwork::kind_e::hero, 2).has_value());
  EXPECT_FALSE(cache.publish(GAME_UUID, game_artwork::kind_e::icon, {'n', 'o'}, 3).has_value());
  EXPECT_FALSE(cache.lookup(GAME_UUID, second->token, game_artwork::kind_e::hero, 1002).has_value());

  cache.clear_game(GAME_UUID);
  EXPECT_EQ(cache.size(), 0);
}
