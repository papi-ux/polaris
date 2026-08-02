#include <gtest/gtest.h>

#include <src/game_artwork_provider.h>

#include <algorithm>
#include <cmath>
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

TEST(GameArtworkProviderSteamGridDb, ParsesSanitizedManualMatchCandidatesInProviderOrder) {
  const auto candidates = game_artwork::providers::parse_steamgriddb_match_candidates(
    "  PORTAL---2 ",
    R"({"success":true,"data":[
      {
        "id":12345,
        "name":"Portal 2",
        "steam_appid":"620",
        "release_year":2011,
        "score":999999,
        "url":"https://evil.example/game/12345?api_key=leaked",
        "authorization":"Bearer do-not-copy"
      },
      {"id":77,"name":"Portal Two","steam_appid":730,"release_year":2100},
      {"id":99,"name":"Portal Stories: Mel","steam_appid":"not-numeric","release_year":2101}
    ]})",
    3
  );

  ASSERT_EQ(candidates.size(), 3);
  EXPECT_EQ(candidates[0].provider, "steamgriddb");
  EXPECT_EQ(candidates[0].provider_game_id, "12345");
  EXPECT_EQ(candidates[0].title, "Portal 2");
  EXPECT_EQ(candidates[0].steam_appid, "620");
  EXPECT_EQ(candidates[0].release_year, 2011);
  EXPECT_DOUBLE_EQ(candidates[0].confidence, 1.0);
  EXPECT_TRUE(std::isfinite(candidates[0].confidence));

  EXPECT_EQ(candidates[1].provider_game_id, "77");
  EXPECT_EQ(candidates[1].steam_appid, "730");
  EXPECT_EQ(candidates[1].release_year, 2100);
  EXPECT_GE(candidates[1].confidence, 0.0);
  EXPECT_LT(candidates[1].confidence, 1.0);

  EXPECT_EQ(candidates[2].provider_game_id, "99");
  EXPECT_FALSE(candidates[2].steam_appid.has_value());
  EXPECT_FALSE(candidates[2].release_year.has_value());
  EXPECT_GE(candidates[2].confidence, 0.0);
  EXPECT_LE(candidates[2].confidence, 1.0);

  // Untrusted provider fields, scores, absolute URLs, and credentials are not
  // represented by the typed parser result and cannot be copied through.
  for (const auto &candidate : candidates) {
    const auto sanitized = candidate.provider + candidate.provider_game_id + candidate.title +
                           candidate.steam_appid.value_or("");
    EXPECT_EQ(sanitized.find("evil.example"), std::string::npos);
    EXPECT_EQ(sanitized.find("api_key"), std::string::npos);
    EXPECT_EQ(sanitized.find("do-not-copy"), std::string::npos);
  }
}

TEST(GameArtworkProviderSteamGridDb, RejectsMalformedUnsafeAndDuplicateManualMatchCandidates) {
  const std::string oversized_title(161, 'x');
  const auto response = std::string {R"({"success":true,"data":[
    {"id":0,"name":"Zero"},
    {"id":-1,"name":"Negative"},
    {"id":"2","name":"Wrong ID type"},
    {"id":3.5,"name":"Floating ID"},
    {"id":4,"name":17},
    {"id":5,"name":""},
    {"id":6,"name":"Bad\nTitle"},
    {"id":7,"name":"Bad\u007fTitle"},
    {"id":81,"name":"Bad\u0085Title"},
    {"id":8,"name":")"} + oversized_title + R"("},
    {"id":9,"name":"Safe Title","steam_appid":"620/../10","release_year":1969},
    {"id":9,"name":"Duplicate ID"},
    {"id":10,"name":"Second Safe Title","steam_appid":0,"release_year":"2011"}
  ]})";

  const auto candidates = game_artwork::providers::parse_steamgriddb_match_candidates(
    "Safe Title",
    response,
    10
  );
  ASSERT_EQ(candidates.size(), 2);
  EXPECT_EQ(candidates[0].provider_game_id, "9");
  EXPECT_EQ(candidates[0].title, "Safe Title");
  EXPECT_FALSE(candidates[0].steam_appid.has_value());
  EXPECT_FALSE(candidates[0].release_year.has_value());
  EXPECT_EQ(candidates[1].provider_game_id, "10");
  EXPECT_EQ(candidates[1].title, "Second Safe Title");
  EXPECT_FALSE(candidates[1].steam_appid.has_value());
  EXPECT_FALSE(candidates[1].release_year.has_value());

  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_match_candidates(
    "query", "not json", 10).empty());
  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_match_candidates(
    "query", R"({"success":true,"data":{}})", 10).empty());
  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_match_candidates(
    "query", R"({"success":"true","data":[]})", 10).empty());
  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_match_candidates(
    "query", R"({"success":false,"data":[{"id":1,"name":"Ignored"}]})", 10).empty());
}

TEST(GameArtworkProviderSteamGridDb, EnforcesCallerAndHardManualMatchCandidateBounds) {
  std::string response = R"({"success":true,"data":[)";
  for (int id = 1; id <= 12; ++id) {
    if (id != 1) response += ',';
    response += R"({"id":)" + std::to_string(id) + R"(,"name":"Game )" +
                std::to_string(id) + R"("})";
  }
  response += "]}";

  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_match_candidates(
    "Game", response, 0).empty());
  const auto caller_bounded = game_artwork::providers::parse_steamgriddb_match_candidates(
    "Game", response, 3);
  ASSERT_EQ(caller_bounded.size(), 3);
  EXPECT_EQ(caller_bounded[0].provider_game_id, "1");
  EXPECT_EQ(caller_bounded[1].provider_game_id, "2");
  EXPECT_EQ(caller_bounded[2].provider_game_id, "3");

  const auto hard_bounded = game_artwork::providers::parse_steamgriddb_match_candidates(
    "Game", response, 1000);
  ASSERT_EQ(hard_bounded.size(), 10);
  EXPECT_EQ(hard_bounded.front().provider_game_id, "1");
  EXPECT_EQ(hard_bounded.back().provider_game_id, "10");
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

TEST(GameArtworkProviderSteamGridDb, PrefersIconThumbnailButKeepsPosterUrl) {
  const auto body = R"({"success":true,"data":[{"url":"https://cdn.steamgriddb.com/icon/raw.ico","thumb":"https://cdn2.steamgriddb.com/icon/thumb.png"}]})";
  const auto icon = game_artwork::providers::parse_steamgriddb_assets(kind_e::icon, body);
  ASSERT_EQ(icon.size(), 1);
  EXPECT_EQ(icon[0].url, "https://cdn2.steamgriddb.com/icon/thumb.png");
  const auto poster = game_artwork::providers::parse_steamgriddb_assets(kind_e::poster, body);
  ASSERT_EQ(poster.size(), 1);
  EXPECT_EQ(poster[0].url, "https://cdn.steamgriddb.com/icon/raw.ico");
}
