#include <gtest/gtest.h>

#include <src/game_artwork_provider.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

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

  // A SteamGridDB search answer with these results, in this order.
  std::string steamgriddb_search_answer(std::initializer_list<std::pair<int, const char *>> results) {
    nlohmann::json body {{"success", true}, {"data", nlohmann::json::array()}};
    for (const auto &[id, name] : results) body["data"].push_back({{"id", id}, {"name", name}});
    return body.dump();
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

TEST(GameArtworkProviderSteam, UsesCurrentAppScopedLibraryFilenamesWithoutTrustingUrlTemplates) {
  using namespace game_artwork::providers;
  const std::string digest(40, 'a');
  const nlohmann::json assets{
    {"asset_url_format", "https://private.example/${FILENAME}"},
    {"library_capsule", digest + "/library_600x900.jpg"},
    {"library_hero", digest + "/library_hero.jpg"}, {"community_icon", digest}};
  nlohmann::json payload;
  payload["response"]["store_items"] = nlohmann::json::array({{{"appid", 3527290}, {"assets", assets}}});
  auto plan = parse_steam_library_assets("3527290", payload.dump());
  ASSERT_EQ(plan.size(), 3U);
  const auto *poster = find_kind(plan, kind_e::poster);
  ASSERT_NE(poster, nullptr);
  EXPECT_EQ(poster->url, "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/3527290/" + digest + "/library_600x900.jpg");
  EXPECT_NE(find_kind(plan, kind_e::icon), nullptr);
  EXPECT_EQ(find_kind(plan, kind_e::logo), nullptr); // Not advertised by this game.
  for (const auto &request : plan) EXPECT_TRUE(game_artwork::is_allowed_provider_url(provider_e::steam, request.url));
  for (const auto &unsafe : {"../library_600x900.jpg", "https://evil.example/library_600x900.jpg",
       "other-game/library_600x900.jpg", "library_600x900.jpg?redirect=private", "%2e%2e/library_600x900.jpg"}) {
    auto changed = payload; changed["response"]["store_items"][0]["assets"]["library_capsule"] = unsafe;
    EXPECT_TRUE(parse_steam_library_assets("3527290", changed.dump()).empty());
  }
  EXPECT_TRUE(parse_steam_library_assets("620", payload.dump()).empty());
  EXPECT_TRUE(parse_steam_library_assets("99999999999999999999", payload.dump()).empty());
  EXPECT_TRUE(parse_steam_library_assets("3527290", std::string(1024U * 1024U + 1, 'x')).empty());
  unsigned calls = 0;
  plan = plan_steam_library_assets("3527290", [&](const request_t &request, std::uintmax_t maximum)
      -> std::optional<transport_response_t> {
    ++calls;
    EXPECT_EQ(maximum, 1024U * 1024U);
    EXPECT_EQ(request.operation, operation_e::list);
    EXPECT_FALSE(request.requires_authorization);
    EXPECT_TRUE(request.url.starts_with("https://api.steampowered.com/IStoreBrowseService/GetItems/v1/?input_json="));
    EXPECT_NE(request.url.find("%22ids%22%3A%5B%7B%22appid%22%3A3527290%7D%5D"), std::string::npos);
    const auto body = payload.dump();
    return transport_response_t{200, {body.begin(), body.end()}, request.url};
  });
  EXPECT_EQ(calls, 1U);
  ASSERT_EQ(plan.size(), 3U);
  EXPECT_NE(find_kind(plan, kind_e::poster), nullptr);
}

TEST(GameArtworkProviderSteam, MetadataFailureRetainsLegacyDownloadsAndRejectsRedirects) {
  using namespace game_artwork::providers;
  for (auto status : {200U, 404U, 503U}) {
    auto plan = plan_steam_library_assets("620", [=](const request_t &, std::uintmax_t)
        -> std::optional<transport_response_t> {
      return transport_response_t{status, {'{', '}'}, "https://private.example/redirect"};
    });
    ASSERT_EQ(plan.size(), 3U);
    EXPECT_EQ(find_kind(plan, kind_e::poster)->url, "https://cdn.cloudflare.steamstatic.com/steam/apps/620/library_600x900.jpg");
  }
  EXPECT_TRUE(plan_steam_library_assets("620/../10", {}).empty());
  EXPECT_FALSE(game_artwork::is_allowed_provider_url(provider_e::steam,
    "https://api.steampowered.com.evil.example/IStoreBrowseService/GetItems/v1/"));
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

TEST(GameArtworkProviderSteamGridDb, AutomaticLookupTakesOnlyAResultWithTheSameTitle) {
  using game_artwork::providers::select_steamgriddb_title_match;
  // SteamGridDB's autocomplete answers for these titles on 2026-09-16, in its order. The first
  // result is never taken as a guess: none of these is the entry that was searched.
  EXPECT_FALSE(select_steamgriddb_title_match("Low Res Desktop", steamgriddb_search_answer({
    {101, "Low Magic Age"}, {102, "Low Desert Punk"}, {103, "Low G Man: The Low Gravity Man"}, {104, "LOW-FI"},
  })).has_value());
  EXPECT_FALSE(select_steamgriddb_title_match("Desktop", steamgriddb_search_answer({
    {201, "Desktop Dungeons"}, {202, "Desktop Dynasties"}, {203, "Desktop Tree"},
  })).has_value());
  EXPECT_FALSE(select_steamgriddb_title_match("Steam Big Picture", steamgriddb_search_answer({
    {301, "Steam"}, {302, "Steam Hardware"}, {303, "Steam Summer Getaway"},
  })).has_value());

  // Real games still resolve, also when their exact title is not the first result.
  EXPECT_EQ(select_steamgriddb_title_match("Control Ultimate Edition", steamgriddb_search_answer({
    {28601, "Control Ultimate Edition"}, {402, "Ultimate Control Machine"}, {403, "Control"},
  })), 28601U);
  EXPECT_EQ(select_steamgriddb_title_match("Control", steamgriddb_search_answer({
    {28601, "Control Ultimate Edition"}, {402, "Ultimate Control Machine"}, {403, "Control"},
  })), 403U);
  EXPECT_EQ(select_steamgriddb_title_match("Disco Elysium", steamgriddb_search_answer({
    {18262, "Disco Elysium"}, {502, "Disco Elysium: Game Boy Edition"},
  })), 18262U);
  EXPECT_EQ(select_steamgriddb_title_match("Big Walk", steamgriddb_search_answer({
    {5438463, "Big Walk"}, {602, "Walking with Dinosaurs: The Ballad of Big Al"},
  })), 5438463U);
  EXPECT_EQ(select_steamgriddb_title_match("Lutris", steamgriddb_search_answer({
    {701, "Lutris"}, {702, "I.C.O. - Machina Lutris"},
  })), 701U);

  // Case, spacing, punctuation and trademark signs do not tell two titles apart.
  EXPECT_EQ(select_steamgriddb_title_match("DOOM  Eternal", steamgriddb_search_answer({{801, "Doom: Eternal™"}})), 801U);
  EXPECT_EQ(select_steamgriddb_title_match("Control®", steamgriddb_search_answer({{901, "CONTROL"}})), 901U);

  // Malformed or empty answers are refused without throwing.
  EXPECT_FALSE(select_steamgriddb_title_match("Control", "not json").has_value());
  EXPECT_FALSE(select_steamgriddb_title_match("Control", R"({"success":false,"data":[{"id":1,"name":"Control"}]})").has_value());
  EXPECT_FALSE(select_steamgriddb_title_match(
    "Control", R"({"success":true,"data":[{"id":0,"name":"Control"},{"id":"5","name":"Control"},{"id":6}]})").has_value());
  EXPECT_FALSE(select_steamgriddb_title_match(" - ", steamgriddb_search_answer({{1, " - "}})).has_value());
}

TEST(GameArtworkProviderSteamGridDb, LooksUpASteamGameByItsAppIdExactly) {
  using namespace game_artwork::providers;
  const auto request = plan_steamgriddb_steam_game("870780");
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->url, "https://www.steamgriddb.com/api/v2/games/steam/870780");
  EXPECT_EQ(request->provider, provider_e::steamgriddb);
  EXPECT_EQ(request->operation, operation_e::search);
  EXPECT_FALSE(request->kind.has_value());
  EXPECT_TRUE(request->requires_authorization);
  EXPECT_TRUE(game_artwork::is_allowed_provider_url(provider_e::steamgriddb, request->url));
  for (const auto *invalid : {"", "0", "087", "12a", "../1", "99999999999"}) {
    EXPECT_FALSE(plan_steamgriddb_steam_game(invalid).has_value()) << invalid;
  }

  // The shape SteamGridDB answered for games/steam/870780 on 2026-09-16.
  EXPECT_EQ(parse_steamgriddb_steam_game_id(
    R"({"success":true,"data":{"id":28601,"name":"Control Ultimate Edition","release_date":1566864000,"types":["steam"],"verified":true}})"), 28601U);
  EXPECT_FALSE(parse_steamgriddb_steam_game_id(R"({"success":false,"errors":["Game not found"]})").has_value());
  EXPECT_FALSE(parse_steamgriddb_steam_game_id(R"({"success":true,"data":[{"id":28601}]})").has_value());
  EXPECT_FALSE(parse_steamgriddb_steam_game_id(R"({"success":true,"data":{"id":-4}})").has_value());
  EXPECT_FALSE(parse_steamgriddb_steam_game_id("not json").has_value());
}

TEST(GameArtworkProviderSteamGridDb, AutomaticGameUsesTheAppIdThenAnExactTitleAndNeverGuesses) {
  using namespace game_artwork::providers;
  const std::string api = "https://www.steamgriddb.com/api/v2/";
  std::vector<std::string> asked;
  std::map<std::string, std::pair<unsigned int, std::string>> answers;
  const transport_t transport = [&](const request_t &request, std::uintmax_t) -> std::optional<transport_response_t> {
    asked.push_back(request.url);
    const auto found = answers.find(request.url);
    if (found == answers.end()) return std::nullopt;
    const auto &[status, body] = found->second;
    return transport_response_t {status, std::vector<unsigned char>(body.begin(), body.end()), {}};
  };

  // A Steam game SteamGridDB knows by app id: its name there may differ, and no title search runs.
  answers[api + "games/steam/632470"] = {200, R"({"success":true,"data":{"id":18262,"name":"Disco Elysium - The Final Cut"}})"};
  EXPECT_EQ(automatic_steamgriddb_game("Disco Elysium", "632470", transport), 18262U);
  EXPECT_EQ(asked, (std::vector<std::string> {api + "games/steam/632470"}));

  // SteamGridDB does not know the app id, so the title search runs and must match exactly.
  asked.clear();
  answers[api + "games/steam/1478500"] = {404, R"({"success":false,"errors":["Game not found"]})"};
  answers[api + "search/autocomplete/Big%20Walk"] = {200, steamgriddb_search_answer({{5438463, "Big Walk"}})};
  EXPECT_EQ(automatic_steamgriddb_game("Big Walk", "1478500", transport), 5438463U);
  EXPECT_EQ(asked, (std::vector<std::string> {api + "games/steam/1478500", api + "search/autocomplete/Big%20Walk"}));

  // No app id and no result with the entry's title: nothing, though SteamGridDB returned games.
  asked.clear();
  answers[api + "search/autocomplete/Low%20Res%20Desktop"] = {
    200, steamgriddb_search_answer({{101, "Low Magic Age"}, {102, "Low Desert Punk"}})};
  EXPECT_FALSE(automatic_steamgriddb_game("Low Res Desktop", "", transport).has_value());
  EXPECT_EQ(asked, (std::vector<std::string> {api + "search/autocomplete/Low%20Res%20Desktop"}));

  // An unanswered request, a missing transport, or an answer from a redirect off the allowlist
  // yields nothing too.
  EXPECT_FALSE(automatic_steamgriddb_game("Control", "", transport).has_value());
  EXPECT_FALSE(automatic_steamgriddb_game("Control", "870780", {}).has_value());
  const transport_t redirected = [](const request_t &, std::uintmax_t) -> std::optional<transport_response_t> {
    const auto body = steamgriddb_search_answer({{1, "Control"}});
    return transport_response_t {200, std::vector<unsigned char>(body.begin(), body.end()), "https://evil.example/api/v2/"};
  };
  EXPECT_FALSE(automatic_steamgriddb_game("Control", "", redirected).has_value());
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

TEST(GameArtworkProviderSteamGridDb, ParsesBoundedChoicesThatPreviewThumbnailsAndStoreWhatAMatchStores) {
  const auto body = R"({"success":true,"data":[
    {"url":"https://evil.example/grid/a.png","thumb":"https://cdn2.steamgriddb.com/thumb/a.jpg"},
    {"url":"https://cdn2.steamgriddb.com/grid/b.png","thumb":"https://cdn2.steamgriddb.com/thumb/b.jpg"},
    {"url":"https://cdn2.steamgriddb.com/grid/b.png","thumb":"https://cdn2.steamgriddb.com/thumb/b2.jpg"},
    {"url":"https://cdn.steamgriddb.com/grid/c.png"},
    {"url":"https://cdn2.steamgriddb.com/grid/d.png","thumb":"https://evil.example/thumb/d.jpg"},
    17,
    {"url":"https://cdn2.steamgriddb.com/grid/e.png","thumb":"https://cdn2.steamgriddb.com/thumb/e.jpg"}
  ]})";
  const auto choices = game_artwork::providers::parse_steamgriddb_choices(kind_e::poster, body, 5);
  ASSERT_EQ(choices.size(), 4);
  EXPECT_EQ(choices[0].asset_url, "https://cdn2.steamgriddb.com/grid/b.png");
  EXPECT_EQ(choices[0].preview_url, "https://cdn2.steamgriddb.com/thumb/b.jpg");
  EXPECT_EQ(choices[1].asset_url, "https://cdn.steamgriddb.com/grid/c.png");
  EXPECT_EQ(choices[1].preview_url, "https://cdn.steamgriddb.com/grid/c.png");
  EXPECT_EQ(choices[2].asset_url, "https://cdn2.steamgriddb.com/grid/d.png");
  EXPECT_EQ(choices[2].preview_url, "https://cdn2.steamgriddb.com/grid/d.png");
  EXPECT_EQ(choices[3].asset_url, "https://cdn2.steamgriddb.com/grid/e.png");
  EXPECT_EQ(choices[3].preview_url, "https://cdn2.steamgriddb.com/thumb/e.jpg");

  // A pick stores exactly the image a match by kinds would have stored from the same entry.
  const auto assets = game_artwork::providers::parse_steamgriddb_assets(kind_e::poster, body);
  ASSERT_EQ(assets.size(), choices.size());
  for (std::size_t index = 0; index < choices.size(); ++index) {
    EXPECT_EQ(choices[index].kind, kind_e::poster);
    EXPECT_EQ(choices[index].asset_url, assets[index].url);
  }

  EXPECT_EQ(game_artwork::providers::parse_steamgriddb_choices(kind_e::poster, body, 2).size(), 2);
  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_choices(kind_e::poster, body, 0).empty());
  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_choices(kind_e::poster, "not json", 5).empty());
  EXPECT_TRUE(game_artwork::providers::parse_steamgriddb_choices(
    kind_e::poster, R"({"success":false,"data":[{"url":"https://cdn.steamgriddb.com/grid/no.jpg"}]})", 5).empty());

  const auto icons = game_artwork::providers::parse_steamgriddb_choices(
    kind_e::icon,
    R"({"success":true,"data":[{"url":"https://cdn.steamgriddb.com/icon/raw.ico","thumb":"https://cdn2.steamgriddb.com/icon/thumb.png"}]})",
    5
  );
  ASSERT_EQ(icons.size(), 1);
  EXPECT_EQ(icons[0].asset_url, "https://cdn2.steamgriddb.com/icon/thumb.png");
  EXPECT_EQ(icons[0].preview_url, "https://cdn2.steamgriddb.com/icon/thumb.png");
}
