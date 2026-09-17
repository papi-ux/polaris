#include <gtest/gtest.h>

#include <src/game_artwork_manual.h>
#include <src/game_artwork_override.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using game_artwork::kind_e;
  using game_artwork::providers::request_t;
  using game_artwork::providers::transport_response_t;

  constexpr std::string_view GAME_UUID = "123e4567-e89b-12d3-a456-426614174000";
  constexpr std::string_view OTHER_UUID = "123e4567-e89b-12d3-a456-426614174001";
  constexpr std::string_view CHOICE_BODY =
    R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","steam_appid":"620"})";
  constexpr std::string_view POSTER_LIST_URL =
    "https://www.steamgriddb.com/api/v2/grids/game/12345?dimensions=600x900&types=static&limit=5";
  constexpr std::string_view HERO_LIST_URL = "https://www.steamgriddb.com/api/v2/heroes/game/12345?types=static&limit=5";
  constexpr std::string_view LOGO_LIST_URL = "https://www.steamgriddb.com/api/v2/logos/game/12345?types=static&limit=5";
  constexpr std::string_view ICON_LIST_URL = "https://www.steamgriddb.com/api/v2/icons/game/12345?types=static&limit=5";

  std::vector<unsigned char> png(unsigned char marker = 0) {
    return {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, marker};
  }

  std::vector<unsigned char> jpeg(unsigned char marker = 0) {
    return {0xff, 0xd8, 0xff, marker};
  }

  struct temp_dir_t {
    fs::path path;

    explicit temp_dir_t(std::string_view name):
        path(fs::temp_directory_path() / ("polaris-artwork-manual-" + std::string(name))) {
      std::error_code error;
      fs::remove_all(path, error);
      fs::create_directories(path);
    }

    ~temp_dir_t() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  };

  void write_bytes(const fs::path &path, const std::vector<unsigned char> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  std::vector<unsigned char> read_bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  // Tokens in issue order, so a test can tell choices apart without trusting their text.
  game_artwork::manual::preview_cache_t sequential_cache(std::int64_t ttl_milliseconds = 60'000) {
    auto issued = std::make_shared<unsigned int>(0);
    return game_artwork::manual::preview_cache_t(
      game_artwork::manual::preview_cache_entries,
      game_artwork::manual::preview_cache_bytes,
      ttl_milliseconds,
      [issued] {
        std::array<char, 33> token {};
        std::snprintf(token.data(), token.size(), "%032x", ++*issued);
        return std::string(token.data());
      }
    );
  }

  // One image entry with every field SteamGridDB's v2 list endpoints return.
  nlohmann::json steamgriddb_image(int id, std::string url, std::optional<std::string> thumb) {
    nlohmann::json image {
      {"id", id},
      {"score", 0},
      {"style", "alternate"},
      {"width", 1920},
      {"height", 620},
      {"nsfw", false},
      {"humor", false},
      {"notes", nullptr},
      {"mime", "image/png"},
      {"language", "en"},
      {"url", std::move(url)},
      {"lock", false},
      {"epilepsy", false},
      {"upvotes", 0},
      {"downvotes", 0},
      {"author", {{"name", "artist"}, {"steam64", "76561197960265728"}, {"avatar", "https://avatars.steamstatic.com/avatar.jpg"}}},
    };
    if (thumb) image["thumb"] = std::move(*thumb);
    return image;
  }

  // SteamGridDB's API and CDN, answering by URL and recording every request.
  struct fake_steamgriddb_t {
    std::map<std::string, transport_response_t> responses;
    std::vector<request_t> requests;
    std::vector<std::uintmax_t> limits;

    void list(std::string_view url, const std::vector<nlohmann::json> &images) {
      const auto body = nlohmann::json {{"success", true}, {"data", images}}.dump();
      responses[std::string(url)] = {200, {body.begin(), body.end()}, {}};
    }

    void serve(std::string_view url, std::vector<unsigned char> bytes) {
      responses[std::string(url)] = {200, std::move(bytes), {}};
    }

    std::size_t requests_for(std::string_view url) const {
      return static_cast<std::size_t>(std::count_if(requests.begin(), requests.end(), [&](const request_t &request) {
        return request.url == url;
      }));
    }

    game_artwork::providers::transport_t transport() {
      return [this](const request_t &request, std::uintmax_t maximum_bytes) -> std::optional<transport_response_t> {
        requests.push_back(request);
        limits.push_back(maximum_bytes);
        const auto found = responses.find(request.url);
        if (found == responses.end()) return std::nullopt;
        return found->second;
      };
    }
  };

  std::string code_of(const std::optional<game_artwork::manual::search_failure_t> &failure) {
    return failure ? failure->code : std::string("no failure");
  }

  game_artwork::manual::match_selection_t choice_identity() {
    const auto identity = game_artwork::manual::parse_choice_request(CHOICE_BODY);
    if (!identity) throw std::logic_error("the choice body Nova sends must parse");
    return *identity;
  }

  // The body PolarisApiClient.buildArtworkSelectionBody sends.
  std::string nova_selection_body(const std::map<std::string, std::string> &picks, std::string_view provider_game_id = "12345") {
    nlohmann::json selections = nlohmann::json::object();
    for (const auto &[kind, token] : picks) selections[kind] = token;
    return nlohmann::json {
      {"provider", "steamgriddb"},
      {"provider_game_id", std::string(provider_game_id)},
      {"title", "Portal 2"},
      {"steam_appid", "620"},
      {"selections", selections},
    }.dump();
  }

  // PolarisApiClient.parseArtworkChoices, check for check: the tokens Nova keeps.
  std::vector<std::string> nova_choice_tokens(const nlohmann::json &json, std::string_view game_id, std::string_view kind) {
    std::vector<std::string> tokens;
    if (!json.is_object() || !json.contains("status") || json.at("status") != true) return tokens;
    if (!json.contains("kind") || !json.at("kind").is_string() || json.at("kind").get<std::string>() != kind) return tokens;
    if (!json.contains("choices") || !json.at("choices").is_array()) return tokens;
    const auto prefix = "/polaris/v1/games/" + std::string(game_id) + "/artwork/candidate/";
    const std::regex opaque_selection_token("[0-9a-f]{32}");
    for (const auto &item : json.at("choices")) {
      if (!item.is_object() || !item.contains("selection_token") || !item.at("selection_token").is_string()) continue;
      const auto token = item.at("selection_token").get<std::string>();
      if (!std::regex_match(token, opaque_selection_token)) continue;
      if (!item.contains("preview") || !item.at("preview").is_string() ||
          item.at("preview").get<std::string>() != prefix + token + "/" + std::string(kind)) continue;
      if (std::find(tokens.begin(), tokens.end(), token) != tokens.end()) continue;
      tokens.push_back(token);
      if (tokens.size() == 5) break;
    }
    return tokens;
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

TEST(GameArtworkManualRoutes, AcceptsChoiceListsForExactlyOneKind) {
  using game_artwork::manual::route_e;
  const auto route = game_artwork::manual::parse_route_target(
    "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/choices/hero");
  ASSERT_TRUE(route.has_value());
  EXPECT_EQ(route->route, route_e::choices);
  EXPECT_EQ(route->uuid, GAME_UUID);
  EXPECT_EQ(route->kind, kind_e::hero);
  EXPECT_FALSE(route->token.has_value());
  for (const auto &[name, kind] : std::array {
         std::pair {"poster", kind_e::poster}, std::pair {"logo", kind_e::logo}, std::pair {"icon", kind_e::icon}}) {
    const auto parsed = game_artwork::manual::parse_route_target(
      "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/choices/" + std::string(name));
    ASSERT_TRUE(parsed.has_value()) << name;
    EXPECT_EQ(parsed->kind, kind);
  }

  for (const auto *const suffix : {"choices", "choices/", "choices/trailer", "choices/Hero", "choices/hero/extra", "choices/hero/"}) {
    EXPECT_FALSE(game_artwork::manual::parse_route_target(
      "/polaris/v1/games/123e4567-e89b-12d3-a456-426614174000/artwork/" + std::string(suffix)).has_value()) << suffix;
  }
  EXPECT_FALSE(game_artwork::manual::parse_route_target("/polaris/v1/games/../artwork/choices/hero").has_value());
}

TEST(GameArtworkManualSelection, ChoiceListsTakeOnlyTheMatchIdentity) {
  const auto identity = game_artwork::manual::parse_choice_request(CHOICE_BODY);
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->provider, "steamgriddb");
  EXPECT_EQ(identity->provider_game_id, "12345");
  EXPECT_EQ(identity->title, "Portal 2");
  EXPECT_EQ(identity->steam_appid, "620");
  EXPECT_TRUE(identity->kinds.empty());
  EXPECT_TRUE(identity->selections.empty());

  const auto without_appid = game_artwork::manual::parse_choice_request(
    R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2"})");
  ASSERT_TRUE(without_appid.has_value());
  EXPECT_FALSE(without_appid->steam_appid.has_value());

  for (const auto *const body : {
         R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","kinds":["hero"]})",
         R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","selections":{"hero":"0123456789abcdef0123456789abcdef"}})",
         R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","api_key":"secret"})",
         R"({"provider":"evil","provider_game_id":"12345","title":"Portal 2"})",
         R"({"provider":"steamgriddb","provider_game_id":"0","title":"Portal 2"})",
         R"({"provider":"steamgriddb","provider_game_id":"12a","title":"Portal 2"})",
         R"({"provider":"steamgriddb","provider_game_id":12345,"title":"Portal 2"})",
         R"({"provider":"steamgriddb","provider_game_id":"12345","title":"BadTitle"})",
         R"({"provider":"steamgriddb","provider_game_id":"12345"})",
         R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","steam_appid":"abc"})",
         R"(["steamgriddb","12345","Portal 2"])",
         "{not-json",
         "",
       }) {
    EXPECT_FALSE(game_artwork::manual::parse_choice_request(body).has_value()) << body;
  }
  EXPECT_FALSE(game_artwork::manual::parse_choice_request(
    std::string(game_artwork::manual::maximum_match_body_bytes + 1, 'x')).has_value());
}

TEST(GameArtworkManualSelection, PicksCarryOnlyOpaqueTokensOnePerKind) {
  const std::string poster_token(32, 'a');
  const std::string hero_token = "0123456789abcdef0123456789abcdef";
  // Nova lists poster first. The host orders picks itself rather than trusting the body.
  const auto selection = game_artwork::manual::parse_match_selection(
    R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","steam_appid":"620",)"
    R"("selections":{"hero":"0123456789abcdef0123456789abcdef","poster":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})");
  ASSERT_TRUE(selection.has_value());
  EXPECT_EQ(selection->provider_game_id, "12345");
  EXPECT_EQ(selection->title, "Portal 2");
  EXPECT_EQ(selection->steam_appid, "620");
  EXPECT_EQ(selection->kinds, (std::vector<kind_e> {kind_e::poster, kind_e::hero}));
  EXPECT_EQ(selection->selections, (std::vector<game_artwork::manual::selected_choice_t> {
    {kind_e::poster, poster_token}, {kind_e::hero, hero_token}}));

  const auto with_selections = [](std::string_view selections) {
    return R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","selections":)" +
           std::string(selections) + "}";
  };
  for (const auto &body : {
         with_selections("{}"),
         with_selections(R"(["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"])"),
         with_selections(R"({"trailer":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})"),
         with_selections(R"({"Poster":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})"),
         with_selections(R"({"poster":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})"),
         with_selections(R"({"poster":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})"),
         with_selections(R"({"poster":12345})"),
         with_selections(R"({"poster":"https://cdn2.steamgriddb.com/grid/a.png"})"),
         with_selections(R"({"poster":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","hero":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})"),
         std::string(R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","kinds":["poster"],)"
                     R"("selections":{"poster":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})"),
         std::string(R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2"})"),
       }) {
    EXPECT_FALSE(game_artwork::manual::parse_match_selection(body).has_value()) << body;
  }
}

TEST(GameArtworkManualPreviewCache, ChoiceTokensAreOpaqueScopedAndExpire) {
  using game_artwork::manual::choice_source_t;
  using game_artwork::manual::preview_ttl_milliseconds;
  // The host's own cache: default bounds, random tokens.
  game_artwork::manual::preview_cache_t cache;
  const choice_source_t source {"12345", "https://cdn2.steamgriddb.com/hero/two.png"};
  const auto choice = cache.publish(GAME_UUID, kind_e::hero, jpeg(1), 5'000, source);
  ASSERT_TRUE(choice.has_value());
  EXPECT_TRUE(std::regex_match(choice->token, std::regex("[0-9a-f]{32}")));
  EXPECT_EQ(choice->expires_at, 5'000 + preview_ttl_milliseconds);

  const auto live = cache.lookup_choice(GAME_UUID, choice->token, kind_e::hero, 5'000 + preview_ttl_milliseconds - 1);
  ASSERT_TRUE(live.has_value());
  EXPECT_EQ(live->provider_game_id, "12345");
  EXPECT_EQ(live->asset_url, "https://cdn2.steamgriddb.com/hero/two.png");
  EXPECT_FALSE(cache.lookup_choice(GAME_UUID, choice->token, kind_e::poster, 5'001).has_value());
  EXPECT_FALSE(cache.lookup_choice(OTHER_UUID, choice->token, kind_e::hero, 5'001).has_value());
  EXPECT_FALSE(cache.lookup_choice(GAME_UUID, std::string(32, 'f'), kind_e::hero, 5'001).has_value());

  // A search preview is served the same way but cannot be applied.
  const auto preview = cache.publish(GAME_UUID, kind_e::poster, png(2), 5'000);
  ASSERT_TRUE(preview.has_value());
  EXPECT_FALSE(preview->choice.has_value());
  EXPECT_TRUE(cache.lookup(GAME_UUID, preview->token, kind_e::poster, 5'001).has_value());
  EXPECT_FALSE(cache.lookup_choice(GAME_UUID, preview->token, kind_e::poster, 5'001).has_value());

  // A source the host would not download never becomes a choice.
  EXPECT_FALSE(cache.publish(GAME_UUID, kind_e::hero, jpeg(3), 5'000,
    choice_source_t {"12345", "https://evil.example/hero.png"}).has_value());
  EXPECT_FALSE(cache.publish(GAME_UUID, kind_e::hero, jpeg(3), 5'000,
    choice_source_t {"0", "https://cdn2.steamgriddb.com/hero/two.png"}).has_value());
  EXPECT_FALSE(cache.publish(GAME_UUID, kind_e::hero, jpeg(3), 5'000,
    choice_source_t {"12345", "https://cdn2.steamgriddb.com/" +
      std::string(game_artwork::manual::maximum_choice_url_bytes, 'a')}).has_value());
  EXPECT_EQ(cache.size(), 2);

  // Expiry ends the preview and the pick together.
  EXPECT_FALSE(cache.lookup(GAME_UUID, choice->token, kind_e::hero, 5'000 + preview_ttl_milliseconds).has_value());
  EXPECT_FALSE(cache.lookup_choice(GAME_UUID, choice->token, kind_e::hero, 5'000 + preview_ttl_milliseconds).has_value());
  EXPECT_EQ(cache.size(), 0);
}

TEST(GameArtworkManualChoices, ListsBoundedAllowlistedChoicesInTheShapeNovaParses) {
  using game_artwork::manual::route_e;
  fake_steamgriddb_t steamgriddb;
  steamgriddb.list(HERO_LIST_URL, {
    steamgriddb_image(1, "https://evil.example/hero/one.png", "https://evil.example/hero_thumb/one.jpg"),
    steamgriddb_image(2, "https://cdn2.steamgriddb.com/hero/two.png", "https://cdn2.steamgriddb.com/hero_thumb/two.jpg"),
    // The same full image again under another entry.
    steamgriddb_image(3, "https://cdn2.steamgriddb.com/hero/two.png", "https://cdn2.steamgriddb.com/hero_thumb/three.jpg"),
    // No thumbnail, so the preview is the full image.
    steamgriddb_image(4, "https://cdn.steamgriddb.com/hero/four.png", std::nullopt),
    // The CDN never answers for this one.
    steamgriddb_image(5, "https://cdn2.steamgriddb.com/hero/five.png", "https://cdn2.steamgriddb.com/hero_thumb/five.jpg"),
    steamgriddb_image(6, "https://cdn2.steamgriddb.com/hero/six.webp", "https://cdn2.steamgriddb.com/hero_thumb/six.jpg"),
    steamgriddb_image(7, "https://cdn2.steamgriddb.com/hero/seven.png", "https://cdn2.steamgriddb.com/hero_thumb/seven.jpg"),
    // Past the five images a list offers.
    steamgriddb_image(8, "https://cdn2.steamgriddb.com/hero/eight.png", "https://cdn2.steamgriddb.com/hero_thumb/eight.jpg"),
  });
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero_thumb/two.jpg", jpeg(2));
  steamgriddb.serve("https://cdn.steamgriddb.com/hero/four.png", png(4));
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero_thumb/six.jpg", jpeg(6));
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero_thumb/seven.jpg", jpeg(7));
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero_thumb/eight.jpg", jpeg(8));

  auto cache = sequential_cache();
  const auto listing = game_artwork::manual::list_artwork_choices(
    cache, GAME_UUID, kind_e::hero, choice_identity(), steamgriddb.transport(), 1'000);
  ASSERT_FALSE(listing.failure.has_value());
  ASSERT_EQ(listing.choices.size(), 4);

  // One authorized list call with the match path's filters, then one bounded CDN call per image.
  ASSERT_EQ(steamgriddb.requests.size(), 6);
  EXPECT_EQ(steamgriddb.requests[0].url, HERO_LIST_URL);
  EXPECT_EQ(steamgriddb.requests[0].operation, game_artwork::providers::operation_e::list);
  EXPECT_TRUE(steamgriddb.requests[0].requires_authorization);
  EXPECT_EQ(steamgriddb.limits[0], game_artwork::manual::maximum_listing_bytes);
  for (std::size_t index = 1; index < steamgriddb.requests.size(); ++index) {
    EXPECT_EQ(steamgriddb.requests[index].operation, game_artwork::providers::operation_e::download);
    EXPECT_FALSE(steamgriddb.requests[index].requires_authorization);
    EXPECT_EQ(steamgriddb.limits[index], game_artwork::manual::maximum_preview_bytes);
    EXPECT_EQ(steamgriddb.requests[index].url.find("evil.example"), std::string::npos);
    EXPECT_EQ(steamgriddb.requests[index].url.find("eight"), std::string::npos);
  }

  const auto served = game_artwork::manual::artwork_choices_json(GAME_UUID, kind_e::hero, listing.choices).dump();
  const auto json = nlohmann::json::parse(served);
  EXPECT_EQ(json.size(), 3);
  ASSERT_TRUE(json.at("status").is_boolean());
  EXPECT_EQ(json.at("status"), true);
  EXPECT_EQ(json.at("kind").get<std::string>(), "hero");
  ASSERT_TRUE(json.at("choices").is_array());
  ASSERT_EQ(json.at("choices").size(), 4);
  for (const auto &item : json.at("choices")) {
    ASSERT_TRUE(item.is_object());
    EXPECT_EQ(item.size(), 3);
    ASSERT_TRUE(item.at("selection_token").is_string());
    const auto token = item.at("selection_token").get<std::string>();
    EXPECT_TRUE(std::regex_match(token, std::regex("[0-9a-f]{32}")));
    ASSERT_TRUE(item.at("preview").is_string());
    EXPECT_EQ(item.at("preview").get<std::string>(), "/polaris/v1/games/" + std::string(GAME_UUID) + "/artwork/candidate/" + token + "/hero");
    ASSERT_TRUE(item.at("expires_at").is_number_integer());
    EXPECT_EQ(item.at("expires_at").get<std::int64_t>(), 61'000);
  }
  EXPECT_EQ(nova_choice_tokens(json, GAME_UUID, "hero").size(), 4);
  // Nova drops a list that answers a different kind.
  EXPECT_TRUE(nova_choice_tokens(json, GAME_UUID, "poster").empty());
  // Provider URLs and everything SteamGridDB said about the images stay on the host.
  EXPECT_EQ(served.find("steamgriddb"), std::string::npos);
  EXPECT_EQ(served.find("https://"), std::string::npos);
  EXPECT_EQ(served.find("artist"), std::string::npos);

  // The existing candidate preview route serves each choice from the thumbnail bytes,
  // while the token remembers the full image an apply stores.
  const std::array preview_bytes {jpeg(2), png(4), jpeg(6), jpeg(7)};
  const std::array asset_urls {
    "https://cdn2.steamgriddb.com/hero/two.png",
    "https://cdn.steamgriddb.com/hero/four.png",
    "https://cdn2.steamgriddb.com/hero/six.webp",
    "https://cdn2.steamgriddb.com/hero/seven.png",
  };
  for (std::size_t index = 0; index < listing.choices.size(); ++index) {
    const auto route = game_artwork::manual::parse_route_target(json.at("choices")[index].at("preview").get<std::string>());
    ASSERT_TRUE(route.has_value());
    EXPECT_EQ(route->route, route_e::preview);
    EXPECT_EQ(route->token, listing.choices[index].token);
    EXPECT_EQ(route->kind, kind_e::hero);
    const auto preview = cache.lookup(route->uuid, *route->token, *route->kind, 2'000);
    ASSERT_TRUE(preview.has_value());
    EXPECT_EQ(preview->body, preview_bytes[index]);
    const auto source = cache.lookup_choice(GAME_UUID, listing.choices[index].token, kind_e::hero, 2'000);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->asset_url, asset_urls[index]);
    EXPECT_EQ(source->provider_game_id, "12345");
  }
}

TEST(GameArtworkManualChoices, ListingFailuresCarryTheSearchFailureCodes) {
  const auto identity = choice_identity();
  const auto failure_for = [&](const std::optional<transport_response_t> &list_response) {
    auto cache = sequential_cache();
    fake_steamgriddb_t steamgriddb;
    if (list_response) steamgriddb.responses[std::string(LOGO_LIST_URL)] = *list_response;
    const auto listing = game_artwork::manual::list_artwork_choices(
      cache, GAME_UUID, kind_e::logo, identity, steamgriddb.transport(), 1'000);
    EXPECT_TRUE(listing.choices.empty());
    EXPECT_EQ(cache.size(), 0);
    return listing.failure;
  };
  const auto status = [](unsigned int code) {
    return std::optional<transport_response_t> {transport_response_t {code, {}, {}}};
  };

  const auto unreachable = failure_for(std::nullopt);
  ASSERT_TRUE(unreachable.has_value());
  EXPECT_EQ(unreachable->code, "steamgriddb_unreachable");
  EXPECT_EQ(unreachable->http_status, 502);
  EXPECT_EQ(code_of(failure_for(status(401))), "steamgriddb_unauthorized");
  EXPECT_EQ(code_of(failure_for(status(403))), "steamgriddb_unauthorized");
  EXPECT_EQ(code_of(failure_for(status(429))), "steamgriddb_rate_limited");
  EXPECT_EQ(code_of(failure_for(status(500))), "steamgriddb_unavailable");
  const std::string listed = R"({"success":true,"data":[]})";
  EXPECT_EQ(code_of(failure_for(transport_response_t {200, {listed.begin(), listed.end()}, "https://evil.example/logos"})),
            "steamgriddb_unreachable");

  // Listed images the CDN will not hand over are an upstream failure, not an empty list.
  {
    auto cache = sequential_cache();
    fake_steamgriddb_t steamgriddb;
    steamgriddb.list(LOGO_LIST_URL, {
      steamgriddb_image(1, "https://cdn2.steamgriddb.com/logo/one.png", "https://cdn2.steamgriddb.com/logo_thumb/one.png")});
    steamgriddb.responses["https://cdn2.steamgriddb.com/logo_thumb/one.png"] = {404, {}, {}};
    const auto listing = game_artwork::manual::list_artwork_choices(
      cache, GAME_UUID, kind_e::logo, identity, steamgriddb.transport(), 1'000);
    ASSERT_TRUE(listing.failure.has_value());
    EXPECT_EQ(listing.failure->code, "steamgriddb_unavailable");
    EXPECT_EQ(cache.size(), 0);
  }

  // A game with no images of the kind is an honest empty list.
  {
    auto cache = sequential_cache();
    fake_steamgriddb_t steamgriddb;
    steamgriddb.list(LOGO_LIST_URL, {});
    const auto listing = game_artwork::manual::list_artwork_choices(
      cache, GAME_UUID, kind_e::logo, identity, steamgriddb.transport(), 1'000);
    EXPECT_FALSE(listing.failure.has_value());
    EXPECT_TRUE(listing.choices.empty());
    EXPECT_EQ(game_artwork::manual::artwork_choices_json(GAME_UUID, kind_e::logo, listing.choices),
              nlohmann::json::parse(R"({"status":true,"kind":"logo","choices":[]})"));
  }

  // An identity no SteamGridDB game can have is refused before any request.
  {
    auto cache = sequential_cache();
    fake_steamgriddb_t steamgriddb;
    const auto oversized = game_artwork::manual::parse_choice_request(
      R"({"provider":"steamgriddb","provider_game_id":"99999999999999999999","title":"Portal 2"})");
    ASSERT_TRUE(oversized.has_value());
    const auto listing = game_artwork::manual::list_artwork_choices(
      cache, GAME_UUID, kind_e::logo, *oversized, steamgriddb.transport(), 1'000);
    ASSERT_TRUE(listing.failure.has_value());
    EXPECT_EQ(listing.failure->code, "artwork_match_invalid");
    EXPECT_EQ(listing.failure->http_status, 400);
    EXPECT_TRUE(steamgriddb.requests.empty());
  }

  // A transport that throws or is missing reads as SteamGridDB being out of reach.
  {
    auto cache = sequential_cache();
    const game_artwork::providers::transport_t throwing = [](const request_t &, std::uintmax_t) -> std::optional<transport_response_t> {
      throw std::runtime_error("socket closed");
    };
    EXPECT_EQ(code_of(game_artwork::manual::list_artwork_choices(cache, GAME_UUID, kind_e::logo, identity, throwing, 1'000).failure),
              "steamgriddb_unreachable");
    EXPECT_EQ(code_of(game_artwork::manual::list_artwork_choices(cache, GAME_UUID, kind_e::logo, identity, {}, 1'000).failure),
              "steamgriddb_unreachable");
  }
}

TEST(GameArtworkManualApply, PicksResolveToTheImagesAMatchWouldStore) {
  fake_steamgriddb_t steamgriddb;
  steamgriddb.list(POSTER_LIST_URL, {
    steamgriddb_image(10, "https://cdn2.steamgriddb.com/grid/ten.png", "https://cdn2.steamgriddb.com/thumb/ten.jpg")});
  steamgriddb.list(ICON_LIST_URL, {
    steamgriddb_image(20, "https://cdn.steamgriddb.com/icon/twenty.ico", "https://cdn2.steamgriddb.com/icon_thumb/twenty.png")});
  steamgriddb.serve("https://cdn2.steamgriddb.com/thumb/ten.jpg", jpeg(10));
  steamgriddb.serve("https://cdn2.steamgriddb.com/icon_thumb/twenty.png", png(20));

  auto cache = sequential_cache();
  const auto transport = steamgriddb.transport();
  const auto posters = game_artwork::manual::list_artwork_choices(cache, GAME_UUID, kind_e::poster, choice_identity(), transport, 1'000);
  const auto icons = game_artwork::manual::list_artwork_choices(cache, GAME_UUID, kind_e::icon, choice_identity(), transport, 1'000);
  ASSERT_EQ(posters.choices.size(), 1);
  ASSERT_EQ(icons.choices.size(), 1);

  const auto selection = game_artwork::manual::parse_match_selection(nova_selection_body({
    {"poster", posters.choices[0].token},
    {"icon", icons.choices[0].token},
  }));
  ASSERT_TRUE(selection.has_value());
  const auto plan = game_artwork::manual::plan_selected_downloads(cache, GAME_UUID, *selection, 2'000);
  ASSERT_FALSE(plan.refusal.has_value());
  ASSERT_EQ(plan.downloads.size(), 2);
  EXPECT_EQ(plan.downloads[0].kind, kind_e::poster);
  EXPECT_EQ(plan.downloads[0].url, "https://cdn2.steamgriddb.com/grid/ten.png");
  EXPECT_EQ(plan.downloads[1].kind, kind_e::icon);
  EXPECT_EQ(plan.downloads[1].url, "https://cdn2.steamgriddb.com/icon_thumb/twenty.png");
  for (const auto &download : plan.downloads) {
    EXPECT_EQ(download.provider, game_artwork::provider_e::steamgriddb);
    EXPECT_EQ(download.operation, game_artwork::providers::operation_e::download);
    EXPECT_FALSE(download.requires_authorization);
  }
}

TEST(GameArtworkManualApply, RefusesUnknownExpiredForeignAndMismatchedPicks) {
  fake_steamgriddb_t steamgriddb;
  steamgriddb.list(HERO_LIST_URL, {
    steamgriddb_image(2, "https://cdn2.steamgriddb.com/hero/two.png", "https://cdn2.steamgriddb.com/hero_thumb/two.jpg")});
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero_thumb/two.jpg", jpeg(2));
  auto cache = sequential_cache();
  const auto listing = game_artwork::manual::list_artwork_choices(
    cache, GAME_UUID, kind_e::hero, choice_identity(), steamgriddb.transport(), 1'000);
  ASSERT_EQ(listing.choices.size(), 1);
  const auto hero = listing.choices[0].token;
  const auto search_preview = cache.publish(GAME_UUID, kind_e::poster, png(9), 1'000);
  ASSERT_TRUE(search_preview.has_value());

  const auto refusal = [&](const std::map<std::string, std::string> &picks, std::int64_t now,
                           std::string_view uuid = GAME_UUID, std::string_view provider_game_id = "12345") {
    const auto selection = game_artwork::manual::parse_match_selection(nova_selection_body(picks, provider_game_id));
    if (!selection) {
      ADD_FAILURE() << "Nova's selection body must parse";
      return std::optional<game_artwork::manual::search_failure_t> {};
    }
    const auto plan = game_artwork::manual::plan_selected_downloads(cache, uuid, *selection, now);
    EXPECT_TRUE(!plan.refusal || plan.downloads.empty());
    if (plan.refusal) EXPECT_EQ(plan.refusal->message.find("https://"), std::string::npos);
    return plan.refusal;
  };

  EXPECT_FALSE(refusal({{"hero", hero}}, 2'000).has_value());

  const auto unknown = refusal({{"hero", std::string(32, 'e')}}, 2'000);
  ASSERT_TRUE(unknown.has_value());
  EXPECT_EQ(unknown->code, "artwork_choice_expired");
  EXPECT_EQ(unknown->http_status, 409);
  EXPECT_NE(unknown->message.find("Load the alternatives again"), std::string::npos);
  EXPECT_EQ(code_of(refusal({{"poster", hero}}, 2'000)), "artwork_choice_expired");
  EXPECT_EQ(code_of(refusal({{"poster", search_preview->token}}, 2'000)), "artwork_choice_expired");
  EXPECT_EQ(code_of(refusal({{"hero", hero}}, 2'000, OTHER_UUID)), "artwork_choice_expired");
  EXPECT_EQ(code_of(refusal({{"hero", hero}, {"logo", std::string(32, 'e')}}, 2'000)), "artwork_choice_expired");

  const auto mismatch = refusal({{"hero", hero}}, 2'000, GAME_UUID, "54321");
  ASSERT_TRUE(mismatch.has_value());
  EXPECT_EQ(mismatch->code, "artwork_choice_mismatch");
  EXPECT_EQ(mismatch->http_status, 409);

  // Last, because expiry removes the pick for good.
  EXPECT_EQ(code_of(refusal({{"hero", hero}}, 61'000)), "artwork_choice_expired");
}

TEST(GameArtworkManualApply, StoresPicksAsTheOverrideAndKeepsCustomImagesItDidNotTouch) {
  temp_dir_t appdata("apply-picks");
  const auto game_directory = game_artwork::cache_root(appdata.path) / GAME_UUID;
  write_bytes(game_directory / "poster.override.jpg", jpeg('O'));
  write_bytes(game_directory / "logo.override.png", png('L'));
  const game_artwork::artwork_override_t previous {std::string(GAME_UUID), "steamgriddb", "111", "Portal", std::nullopt, true, 500};
  ASSERT_TRUE(game_artwork::save_artwork_override(appdata.path, previous));

  fake_steamgriddb_t steamgriddb;
  steamgriddb.list(HERO_LIST_URL, {
    steamgriddb_image(1, "https://cdn2.steamgriddb.com/hero/one.png", "https://cdn2.steamgriddb.com/hero_thumb/one.jpg"),
    steamgriddb_image(2, "https://cdn2.steamgriddb.com/hero/two.png", "https://cdn2.steamgriddb.com/hero_thumb/two.jpg"),
  });
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero_thumb/one.jpg", jpeg(0x11));
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero_thumb/two.jpg", jpeg(0x12));
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero/two.png", png(0x22));
  const auto transport = steamgriddb.transport();

  auto cache = sequential_cache();
  const auto listing = game_artwork::manual::list_artwork_choices(cache, GAME_UUID, kind_e::hero, choice_identity(), transport, 1'000);
  ASSERT_EQ(listing.choices.size(), 2);
  const auto selection = game_artwork::manual::parse_match_selection(nova_selection_body({{"hero", listing.choices[1].token}}));
  ASSERT_TRUE(selection.has_value());
  const auto plan = game_artwork::manual::plan_selected_downloads(cache, GAME_UUID, *selection, 2'000);
  ASSERT_FALSE(plan.refusal.has_value());
  const auto staging = game_artwork::create_artwork_staging_directory(appdata.path, std::string(32, 'a'));
  ASSERT_TRUE(staging.has_value());

  EXPECT_EQ(game_artwork::manual::publish_artwork_override(
              appdata.path, *staging, GAME_UUID, *selection, plan.downloads, transport, 3'000),
            game_artwork::manual::apply_stage_e::published);

  // The pick is stored from the full image. The thumbnail was only ever its preview.
  EXPECT_EQ(read_bytes(game_directory / "hero.override.png"), png(0x22));
  EXPECT_EQ(steamgriddb.requests_for("https://cdn2.steamgriddb.com/hero/two.png"), 1);
  EXPECT_EQ(steamgriddb.requests_for("https://cdn2.steamgriddb.com/hero/one.png"), 0);
  // Custom images the player did not pick are still published, byte for byte.
  EXPECT_EQ(read_bytes(game_directory / "poster.override.jpg"), jpeg('O'));
  EXPECT_EQ(read_bytes(game_directory / "logo.override.png"), png('L'));
  for (const auto kind : {kind_e::poster, kind_e::hero, kind_e::logo}) {
    const auto asset = game_artwork::find_cached_asset(appdata.path, GAME_UUID, kind);
    ASSERT_TRUE(asset.has_value());
    EXPECT_EQ(asset->source, game_artwork::source_e::override);
  }
  // The override names the match the picks came from.
  const game_artwork::artwork_override_t expected {std::string(GAME_UUID), "steamgriddb", "12345", "Portal 2", "620", true, 3'000};
  EXPECT_EQ(game_artwork::load_artwork_override(appdata.path, GAME_UUID), expected);
}

TEST(GameArtworkManualApply, LeavesPublishedArtworkAloneWhenAPickCannotLand) {
  temp_dir_t appdata("apply-picks-fail-closed");
  const auto game_directory = game_artwork::cache_root(appdata.path) / GAME_UUID;
  write_bytes(game_directory / "poster.override.jpg", jpeg('O'));
  const game_artwork::artwork_override_t previous {std::string(GAME_UUID), "steamgriddb", "111", "Portal", std::nullopt, true, 500};
  ASSERT_TRUE(game_artwork::save_artwork_override(appdata.path, previous));

  fake_steamgriddb_t steamgriddb;
  steamgriddb.list(HERO_LIST_URL, {
    steamgriddb_image(1, "https://cdn2.steamgriddb.com/hero/one.png", "https://cdn2.steamgriddb.com/hero_thumb/one.jpg")});
  steamgriddb.list(LOGO_LIST_URL, {
    steamgriddb_image(2, "https://cdn2.steamgriddb.com/logo/two.png", "https://cdn2.steamgriddb.com/logo_thumb/two.png")});
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero_thumb/one.jpg", jpeg(1));
  steamgriddb.serve("https://cdn2.steamgriddb.com/logo_thumb/two.png", png(2));
  steamgriddb.serve("https://cdn2.steamgriddb.com/hero/one.png", png(0x21));
  // The full logo never arrives.
  const auto transport = steamgriddb.transport();

  auto cache = sequential_cache();
  const auto heroes = game_artwork::manual::list_artwork_choices(cache, GAME_UUID, kind_e::hero, choice_identity(), transport, 1'000);
  const auto logos = game_artwork::manual::list_artwork_choices(cache, GAME_UUID, kind_e::logo, choice_identity(), transport, 1'000);
  ASSERT_EQ(heroes.choices.size(), 1);
  ASSERT_EQ(logos.choices.size(), 1);
  const auto selection = game_artwork::manual::parse_match_selection(nova_selection_body({
    {"hero", heroes.choices[0].token},
    {"logo", logos.choices[0].token},
  }));
  ASSERT_TRUE(selection.has_value());
  const auto plan = game_artwork::manual::plan_selected_downloads(cache, GAME_UUID, *selection, 2'000);
  ASSERT_FALSE(plan.refusal.has_value());
  const auto staging = game_artwork::create_artwork_staging_directory(appdata.path, std::string(32, 'b'));
  ASSERT_TRUE(staging.has_value());

  EXPECT_EQ(game_artwork::manual::publish_artwork_override(
              appdata.path, *staging, GAME_UUID, *selection, plan.downloads, transport, 3'000),
            game_artwork::manual::apply_stage_e::asset_download);
  EXPECT_EQ(read_bytes(game_directory / "poster.override.jpg"), jpeg('O'));
  EXPECT_FALSE(fs::exists(game_directory / "hero.override.png"));
  EXPECT_FALSE(fs::exists(game_directory / "logo.override.png"));
  EXPECT_EQ(game_artwork::load_artwork_override(appdata.path, GAME_UUID), previous);
}

TEST(GameArtworkManualApply, MatchByKindsStillReplacesTheWholeOverrideGeneration) {
  temp_dir_t appdata("apply-kinds-generation");
  const auto game_directory = game_artwork::cache_root(appdata.path) / GAME_UUID;
  write_bytes(game_directory / "poster.override.jpg", jpeg('O'));
  write_bytes(game_directory / "hero.override.jpg", jpeg('H'));

  fake_steamgriddb_t steamgriddb;
  steamgriddb.serve("https://cdn2.steamgriddb.com/grid/new.png", png(0x33));
  const auto selection = game_artwork::manual::parse_match_selection(
    R"({"provider":"steamgriddb","provider_game_id":"12345","title":"Portal 2","kinds":["poster"]})");
  ASSERT_TRUE(selection.has_value());
  const std::vector<request_t> downloads {{
    game_artwork::provider_e::steamgriddb,
    game_artwork::providers::operation_e::download,
    kind_e::poster,
    "https://cdn2.steamgriddb.com/grid/new.png",
    false,
  }};
  const auto staging = game_artwork::create_artwork_staging_directory(appdata.path, std::string(32, 'c'));
  ASSERT_TRUE(staging.has_value());

  EXPECT_EQ(game_artwork::manual::publish_artwork_override(
              appdata.path, *staging, GAME_UUID, *selection, downloads, steamgriddb.transport(), 3'000),
            game_artwork::manual::apply_stage_e::published);
  EXPECT_EQ(read_bytes(game_directory / "poster.override.png"), png(0x33));
  EXPECT_FALSE(fs::exists(game_directory / "poster.override.jpg"));
  EXPECT_FALSE(fs::exists(game_directory / "hero.override.jpg"));
  const auto stored = game_artwork::load_artwork_override(appdata.path, GAME_UUID);
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->provider_game_id, "12345");
}

namespace {
  constexpr std::string_view HEROIC_SEARCH_URL = "https://www.steamgriddb.com/api/v2/search/autocomplete/Heroic";
  constexpr std::string_view LAUNCHER_POSTER_LIST_URL =
    "https://www.steamgriddb.com/api/v2/grids/game/777?dimensions=600x900&types=static&limit=5";
  constexpr std::string_view LAUNCHER_POSTER_URL = "https://cdn2.steamgriddb.com/grid/heroic-launcher.png";

  transport_response_t json_answer(const nlohmann::json &body) {
    const auto text = body.dump();
    return {200, {text.begin(), text.end()}, {}};
  }

  nlohmann::json heroic_search_answer() {
    return {
      {"success", true},
      {"data", {
        {{"id", 12345}, {"name", "Heroic Quest"}},
        {{"id", 777}, {"name", "Heroic Games Launcher"}, {"release_year", 2021}},
      }},
    };
  }
}  // namespace

TEST(GameArtworkManualCandidates, ListsEveryMatchAndPreviewsTheFirstPosterOfEach) {
  // "Heroic" is not the launcher's SteamGridDB title. The console's old search took the first
  // autocomplete result only and found no cover; listing every match lets the player pick it.
  fake_steamgriddb_t steamgriddb;
  steamgriddb.responses[std::string(HEROIC_SEARCH_URL)] = json_answer(heroic_search_answer());
  steamgriddb.list(POSTER_LIST_URL, {});
  steamgriddb.list(LAUNCHER_POSTER_LIST_URL, {steamgriddb_image(9, std::string(LAUNCHER_POSTER_URL), std::nullopt)});
  steamgriddb.serve(LAUNCHER_POSTER_URL, png(7));

  auto cache = sequential_cache();
  const auto search = game_artwork::manual::search_match_candidates(cache, GAME_UUID, "Heroic", steamgriddb.transport(), 1'000);
  ASSERT_FALSE(search.invalid_query);
  ASSERT_FALSE(search.failure.has_value());
  ASSERT_EQ(search.candidates.size(), 2);

  // A match with no poster is still a match; only its preview is missing.
  EXPECT_EQ(search.candidates[0].candidate.title, "Heroic Quest");
  EXPECT_FALSE(search.candidates[0].poster_token.has_value());

  const auto &launcher = search.candidates[1];
  EXPECT_EQ(launcher.candidate.title, "Heroic Games Launcher");
  EXPECT_EQ(launcher.candidate.release_year, std::optional<unsigned int>(2021));
  ASSERT_TRUE(launcher.poster_token.has_value());
  EXPECT_EQ(launcher.preview_expires_at, 61'000);

  const auto preview = cache.lookup(GAME_UUID, *launcher.poster_token, kind_e::poster, 2'000);
  ASSERT_TRUE(preview.has_value());
  EXPECT_EQ(preview->body, png(7));
  EXPECT_EQ(preview->mime_type, "image/png");
  // A token belongs to the uuid it was searched for.
  EXPECT_FALSE(cache.lookup(OTHER_UUID, *launcher.poster_token, kind_e::poster, 2'000).has_value());

  // The authorized search and lists stay within the metadata bound, the CDN image within the preview bound.
  ASSERT_EQ(steamgriddb.requests.size(), 4);
  EXPECT_EQ(steamgriddb.requests[0].url, HEROIC_SEARCH_URL);
  EXPECT_TRUE(steamgriddb.requests[0].requires_authorization);
  EXPECT_EQ(steamgriddb.limits[0], game_artwork::manual::maximum_listing_bytes);
  EXPECT_EQ(steamgriddb.requests[3].url, LAUNCHER_POSTER_URL);
  EXPECT_FALSE(steamgriddb.requests[3].requires_authorization);
  EXPECT_EQ(steamgriddb.limits[3], game_artwork::manual::maximum_preview_bytes);
}

TEST(GameArtworkManualCandidates, FindCoverReadsPastMatchesWithoutAPosterAndStopsAtFive) {
  using game_artwork::manual::candidate_listing_e;
  const auto list_url = [](int id) {
    return "https://www.steamgriddb.com/api/v2/grids/game/" + std::to_string(id) + "?dimensions=600x900&types=static&limit=5";
  };
  const auto poster_url = [](int id) {
    return "https://cdn2.steamgriddb.com/grid/heroic-" + std::to_string(id) + ".png";
  };
  // Ten answers for "Heroic" in SteamGridDB's shape. There the first five have no 600x900 poster
  // and the launcher is seventh; here the sixth has none either, so no poster reaches Nova's five.
  const auto heroic = [&](fake_steamgriddb_t &steamgriddb, int first_with_poster) {
    auto data = nlohmann::json::array();
    for (int id = 1; id <= 10; ++id) {
      data.push_back({{"id", id}, {"name", id == 7 ? std::string("Heroic Games Launcher") : "Heroic " + std::to_string(id)}});
      if (id < first_with_poster) {
        steamgriddb.list(list_url(id), {});
      } else {
        steamgriddb.list(list_url(id), {steamgriddb_image(id, poster_url(id), std::nullopt)});
        steamgriddb.serve(poster_url(id), png(static_cast<unsigned char>(id)));
      }
    }
    steamgriddb.responses[std::string(HEROIC_SEARCH_URL)] = json_answer({{"success", true}, {"data", data}});
  };

  // Nova lists the first five matches as before, and none of them has a cover.
  fake_steamgriddb_t steamgriddb;
  heroic(steamgriddb, 7);
  auto cache = sequential_cache();
  const auto nova = game_artwork::manual::search_match_candidates(cache, GAME_UUID, "Heroic", steamgriddb.transport(), 1'000);
  ASSERT_EQ(nova.candidates.size(), 5);
  EXPECT_TRUE(std::none_of(nova.candidates.begin(), nova.candidates.end(), [](const auto &item) {
    return item.poster_token.has_value();
  }));
  EXPECT_EQ(steamgriddb.requests_for(list_url(6)), 0);

  // Find Cover reads every match and lists only the ones with a poster, the launcher first.
  const auto covers = game_artwork::manual::search_match_candidates(
    cache, GAME_UUID, "Heroic", steamgriddb.transport(), 1'000, candidate_listing_e::matches_with_posters);
  ASSERT_FALSE(covers.failure.has_value());
  ASSERT_EQ(covers.candidates.size(), 4);
  EXPECT_EQ(covers.candidates[0].candidate.title, "Heroic Games Launcher");
  EXPECT_TRUE(std::all_of(covers.candidates.begin(), covers.candidates.end(), [](const auto &item) {
    return item.poster_token.has_value();
  }));
  EXPECT_EQ(steamgriddb.requests_for(list_url(10)), 1);

  // Five posters are enough: the rest of the matches are never asked for.
  fake_steamgriddb_t plenty;
  heroic(plenty, 2);
  const auto first_five = game_artwork::manual::search_match_candidates(
    cache, GAME_UUID, "Heroic", plenty.transport(), 1'000, candidate_listing_e::matches_with_posters);
  ASSERT_EQ(first_five.candidates.size(), game_artwork::manual::maximum_candidate_count);
  EXPECT_EQ(first_five.candidates.front().candidate.provider_game_id, "2");
  EXPECT_EQ(first_five.candidates.back().candidate.provider_game_id, "6");
  EXPECT_EQ(plenty.requests_for(list_url(6)), 1);
  EXPECT_EQ(plenty.requests_for(list_url(7)), 0);
}

TEST(GameArtworkManualCandidates, KeepsAMatchWhosePosterCannotBeFetched) {
  fake_steamgriddb_t steamgriddb;
  steamgriddb.responses[std::string(HEROIC_SEARCH_URL)] = json_answer(heroic_search_answer());
  steamgriddb.list(LAUNCHER_POSTER_LIST_URL, {steamgriddb_image(9, std::string(LAUNCHER_POSTER_URL), std::nullopt)});
  steamgriddb.responses[std::string(LAUNCHER_POSTER_URL)] = {404, {}, {}};

  auto cache = sequential_cache();
  const auto search = game_artwork::manual::search_match_candidates(cache, GAME_UUID, "Heroic", steamgriddb.transport(), 1'000);
  ASSERT_FALSE(search.failure.has_value());
  ASSERT_EQ(search.candidates.size(), 2);
  EXPECT_FALSE(search.candidates[0].poster_token.has_value());
  EXPECT_FALSE(search.candidates[1].poster_token.has_value());
  EXPECT_EQ(cache.size(), 0);
}

TEST(GameArtworkManualCandidates, SearchFailuresCarryTheCodesNovaAndTheConsoleShow) {
  const auto failure_for = [](std::optional<transport_response_t> answer) {
    fake_steamgriddb_t steamgriddb;
    if (answer) steamgriddb.responses[std::string(HEROIC_SEARCH_URL)] = *answer;
    auto cache = sequential_cache();
    const auto search = game_artwork::manual::search_match_candidates(cache, GAME_UUID, "Heroic", steamgriddb.transport(), 1'000);
    EXPECT_TRUE(search.candidates.empty());
    EXPECT_EQ(cache.size(), 0);
    return search.failure;
  };
  const auto status = [](unsigned int code) {
    return std::optional<transport_response_t> {transport_response_t {code, {}, {}}};
  };

  const auto unreachable = failure_for(std::nullopt);
  ASSERT_TRUE(unreachable.has_value());
  EXPECT_EQ(unreachable->code, "steamgriddb_unreachable");
  EXPECT_EQ(unreachable->http_status, 502);
  EXPECT_EQ(code_of(failure_for(status(401))), "steamgriddb_unauthorized");
  EXPECT_EQ(code_of(failure_for(status(429))), "steamgriddb_rate_limited");
  EXPECT_EQ(code_of(failure_for(status(500))), "steamgriddb_unavailable");
  const auto body = heroic_search_answer().dump();
  // An answer redirected off the allowlist is never read. Nova's search has always named that
  // failure from the status it saw, and sharing the search keeps its answers as they were.
  EXPECT_EQ(code_of(failure_for(transport_response_t {200, {body.begin(), body.end()}, "https://evil.example/api/v2/"})),
            "steamgriddb_unavailable");

  fake_steamgriddb_t unused;
  auto cache = sequential_cache();
  const auto blank = game_artwork::manual::search_match_candidates(cache, GAME_UUID, "   ", unused.transport(), 1'000);
  EXPECT_TRUE(blank.invalid_query);
  EXPECT_TRUE(unused.requests.empty());
}
