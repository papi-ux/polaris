#include <gtest/gtest.h>

#include <src/game_artwork_provider.h>

#include <algorithm>
#include <string>

namespace {
  using game_artwork::kind_e;
  using game_artwork::provider_e;
  using game_artwork::source_e;
  using game_artwork::providers::operation_e;

  const game_artwork::providers::request_t *find_kind(
    const std::vector<game_artwork::providers::request_t> &requests,
    kind_e kind
  ) {
    const auto found = std::find_if(requests.begin(), requests.end(), [kind](const auto &request) {
      return request.kind == kind;
    });
    return found == requests.end() ? nullptr : &*found;
  }
}

TEST(GameArtworkProviderSteam, PlansOnlyDeterministicAllowlistedOfficialAssets) {
  const auto requests = game_artwork::providers::plan_steam_assets("620");
  ASSERT_EQ(requests.size(), 3);

  const auto *poster = find_kind(requests, kind_e::poster);
  const auto *hero = find_kind(requests, kind_e::hero);
  const auto *logo = find_kind(requests, kind_e::logo);
  ASSERT_NE(poster, nullptr);
  ASSERT_NE(hero, nullptr);
  ASSERT_NE(logo, nullptr);
  EXPECT_EQ(poster->url, "https://cdn.cloudflare.steamstatic.com/steam/apps/620/library_600x900.jpg");
  EXPECT_EQ(hero->url, "https://cdn.cloudflare.steamstatic.com/steam/apps/620/library_hero.jpg");
  EXPECT_EQ(logo->url, "https://cdn.cloudflare.steamstatic.com/steam/apps/620/logo.png");

  for (const auto &request : requests) {
    EXPECT_EQ(request.provider, provider_e::steam);
    EXPECT_EQ(request.operation, operation_e::download);
    EXPECT_TRUE(request.kind.has_value());
    EXPECT_FALSE(request.requires_authorization);
    EXPECT_TRUE(game_artwork::is_allowed_provider_url(provider_e::steam, request.url));
  }
  EXPECT_TRUE(game_artwork::providers::plan_steam_assets("").empty());
  EXPECT_TRUE(game_artwork::providers::plan_steam_assets("620/../10").empty());
}

TEST(GameArtworkProviderSteamGridDb, EscapesSearchTitlesWithoutPuttingSecretsInUrls) {
  const auto search = game_artwork::providers::plan_steamgriddb_search("  NieR: Automata/2?  ");
  ASSERT_TRUE(search.has_value());
  EXPECT_EQ(search->provider, provider_e::steamgriddb);
  EXPECT_EQ(search->operation, operation_e::search);
  EXPECT_FALSE(search->kind.has_value());
  EXPECT_TRUE(search->requires_authorization);
  EXPECT_EQ(
    search->url,
    "https://www.steamgriddb.com/api/v2/search/autocomplete/NieR%3A%20Automata%2F2%3F"
  );
  EXPECT_TRUE(game_artwork::is_allowed_provider_url(provider_e::steamgriddb, search->url));
  EXPECT_EQ(search->url.find("Authorization"), std::string::npos);
  EXPECT_FALSE(game_artwork::providers::plan_steamgriddb_search(" \t\n ").has_value());
}

TEST(GameArtworkProviderSteamGridDb, ParsesFirstValidPositiveSearchResultWithoutThrowing) {
  EXPECT_EQ(
    game_artwork::providers::parse_steamgriddb_game_id(
      R"({"success":true,"data":[{"id":null},{"id":12345},{"id":99999}]})"
    ),
    12345
  );
  EXPECT_FALSE(game_artwork::providers::parse_steamgriddb_game_id("not json").has_value());
  EXPECT_FALSE(game_artwork::providers::parse_steamgriddb_game_id(
    R"({"success":false,"data":[{"id":12345}]})").has_value());
  EXPECT_FALSE(game_artwork::providers::parse_steamgriddb_game_id(
    R"({"success":true,"data":[{"id":0},{"id":-1},{"id":"12345"}]})").has_value());
}

TEST(GameArtworkProviderSteamGridDb, PlansAuthenticatedPerKindMetadataLookups) {
  const auto requests = game_artwork::providers::plan_steamgriddb_assets(12345);
  ASSERT_EQ(requests.size(), 4);
  EXPECT_EQ(find_kind(requests, kind_e::poster)->url,
            "https://www.steamgriddb.com/api/v2/grids/game/12345?dimensions=600x900&types=static&limit=5");
  EXPECT_EQ(find_kind(requests, kind_e::hero)->url,
            "https://www.steamgriddb.com/api/v2/heroes/game/12345?types=static&limit=5");
  EXPECT_EQ(find_kind(requests, kind_e::logo)->url,
            "https://www.steamgriddb.com/api/v2/logos/game/12345?types=static&limit=5");
  EXPECT_EQ(find_kind(requests, kind_e::icon)->url,
            "https://www.steamgriddb.com/api/v2/icons/game/12345?types=static&limit=5");
  for (const auto &request : requests) {
    EXPECT_EQ(request.provider, provider_e::steamgriddb);
    EXPECT_EQ(request.operation, operation_e::list);
    EXPECT_TRUE(request.kind.has_value());
    EXPECT_TRUE(request.requires_authorization);
    EXPECT_TRUE(game_artwork::is_allowed_provider_url(provider_e::steamgriddb, request.url));
  }
  EXPECT_TRUE(game_artwork::providers::plan_steamgriddb_assets(0).empty());
}

TEST(GameArtworkProviderSteamGridDb, ParsesOnlyAllowlistedUniqueDownloadCandidates) {
  const auto candidates = game_artwork::providers::parse_steamgriddb_assets(
    kind_e::hero,
    R"({"success":true,"data":[
      {"url":"https://evil.example/hero.jpg"},
      {"url":17},
      {"url":"https://cdn.steamgriddb.com/hero/first.jpg"},
      {"url":"https://cdn.steamgriddb.com/hero/first.jpg"},
      {"url":"https://cdn2.steamgriddb.com/hero/second.webp"}
    ]})"
  );
  ASSERT_EQ(candidates.size(), 2);
  EXPECT_EQ(candidates[0].kind, kind_e::hero);
  EXPECT_EQ(candidates[0].source, source_e::steamgriddb);
  EXPECT_EQ(candidates[0].url, "https://cdn.steamgriddb.com/hero/first.jpg");
  EXPECT_EQ(candidates[1].url, "https://cdn2.steamgriddb.com/hero/second.webp");
  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_assets(kind_e::poster, "not json").empty());
  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_assets(
    kind_e::poster, R"({"success":false,"data":[{"url":"https://cdn.steamgriddb.com/grid/no.jpg"}]})").empty());
}
