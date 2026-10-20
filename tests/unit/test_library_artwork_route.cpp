/**
 * @file tests/unit/test_library_artwork_route.cpp
 * @brief The console's GET /api/apps/{uuid}/artwork/{hero|logo}: the host artwork cache Nova reads,
 *        and the one background Steam fill a missing image starts.
 */
#include "../tests_common.h"

#include <Simple-Web-Server/client_https.hpp>
#include <src/config.h>
#include <src/confighttp.h>
#include <src/crypto.h>
#include <src/game_artwork.h>
#include <src/game_artwork_override.h>
#include <src/game_artwork_provider.h>
#include <src/process.h>
#include <src/utility.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
  namespace fs = std::filesystem;
  using reply_t = game_artwork::providers::transport_response_t;
  using provider_request_t = game_artwork::providers::request_t;
  using provider_t = std::function<std::optional<reply_t>(const provider_request_t &)>;

  constexpr auto GAME_UUID = "4C1B7E2A-9F0D-4E55-8A31-6B2F0C9D1E77";  // a Steam title
  constexpr auto OFF_UUID = "7D3E0B6C-2A41-4F88-9C15-0E6A5B4D3C21";  // a Steam title after Remove artwork
  constexpr auto DESKTOP_UUID = "A0B1C2D3-E4F5-4A6B-8C7D-8E9F0A1B2C3D";  // mirrors the desktop
  constexpr auto LOCAL_UUID = "5E6F7A8B-9C0D-4E1F-A2B3-C4D5E6F7A8B9";  // a game without a Steam app id
  constexpr auto UNKNOWN_UUID = "0F1E2D3C-4B5A-4968-8776-655443322110";
  constexpr auto STEAM_APPID = "620";
  constexpr auto STEAM_ROOT = "https://cdn.cloudflare.steamstatic.com/steam/apps/620/";

  const std::vector<unsigned char> JPEG {0xff, 0xd8, 0xff, 0xe0, 'h', 'e', 'r', 'o'};
  const std::vector<unsigned char> PNG {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 'l', 'o', 'g', 'o'};

  std::string text(const std::vector<unsigned char> &bytes) {
    return {bytes.begin(), bytes.end()};
  }

  void write_bytes(const fs::path &path, const std::vector<unsigned char> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  fs::path cached(const fs::path &appdata, const char *uuid, const char *file) {
    return game_artwork::cache_root(appdata) / uuid / file;
  }

  struct http_reply_t {
    int code = 0;
    std::string body;
    SimpleWeb::CaseInsensitiveMultimap header;

    std::string value(const std::string &name) const {
      const auto found = header.find(name);
      return found == header.end() ? std::string {} : found->second;
    }
  };

  struct library_artwork_route: testing::Test {
    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      directory = fs::temp_directory_path() / ("polaris-library-artwork-" + std::to_string(nonce));
      appdata = directory / "appdata";
      fs::create_directories(appdata);
      old_config = config::sunshine;
      old_apps_file = config::stream.file_apps;
      old_steamgriddb_key = config::steamgriddb_api_key();
      old_catalog.emplace(proc::proc.get_env(), proc::proc.get_apps());
      config::sunshine.username = "artwork-fixture-user";
      config::sunshine.api_key = "artwork-fixture-key";
      config::sunshine.config_file = (directory / "polaris.conf").string();
      // A fill must not use it: the console never reaches SteamGridDB.
      config::set_steamgriddb_api_key("mock-only-key");
      config::stream.file_apps = (directory / "apps.json").string();
      const nlohmann::json apps {
        {"version", 15},
        {"apps", nlohmann::json::array({
          {{"uuid", GAME_UUID}, {"name", "Portal"}, {"cmd", "/usr/bin/true"}, {"steam-appid", STEAM_APPID}},
          {{"uuid", OFF_UUID}, {"name", "Portal 2"}, {"cmd", "/usr/bin/true"}, {"steam-appid", "640"}},
          {{"uuid", DESKTOP_UUID}, {"name", "My Desktop"}, {"cmd", "/usr/bin/true"}, {"steam-appid", "660"}, {"desktop-mirror", true}},
          {{"uuid", LOCAL_UUID}, {"name", "Homebrew"}, {"cmd", "/usr/bin/true"}},
        })},
      };
      std::ofstream(config::stream.file_apps) << apps.dump();
      proc::refresh(config::stream.file_apps, false);

      confighttp::set_library_artwork_for_tests([this](const provider_request_t &request, std::uintmax_t) -> std::optional<reply_t> {
        provider_t provider;
        {
          std::lock_guard lock(mutex);
          requests.push_back(request);
          provider = this->provider;
        }
        return provider ? provider(request) : std::nullopt;
      }, appdata);

      const auto credentials = crypto::gen_creds("localhost", 2048);
      const auto cert = directory / "cert.pem", key = directory / "key.pem";
      std::ofstream(cert) << credentials.x509;
      std::ofstream(key) << credentials.pkey;
      fs::permissions(key, fs::perms::owner_read | fs::perms::owner_write);
      server = std::async(std::launch::async, [&, cert, key]() {
        try {
          confighttp::run_library_artwork_routes_for_tests(cert.string(), key.string(),
            [&](unsigned short port, std::function<void()> stop) {
              stop_server = std::move(stop);
              listening.set_value(port);
            });
        } catch (...) {
          if (!stop_server) listening.set_exception(std::current_exception());
          else throw;
        }
      });
      port = listening.get_future().get();
    }

    void TearDown() override {
      if (stop_server) stop_server();
      if (server.valid()) server.get();
      confighttp::set_library_artwork_for_tests({}, std::nullopt);
      confighttp::forget_library_artwork_fills_for_tests();
      config::set_steamgriddb_api_key(old_steamgriddb_key);
      config::sunshine = old_config;
      config::stream.file_apps = old_apps_file;
      // Reload only the copied model; do not invoke refresh's process termination default.
      if (old_catalog) proc::proc.reload_configuration(std::move(*old_catalog));
      std::error_code ignored;
      fs::remove_all(directory, ignored);
    }

    http_reply_t get(const std::string &path, const std::string &token = "artwork-fixture-key",
                     const std::string &if_none_match = {}) const {
      SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(port), false};
      client.config.timeout = 2;
      client.config.timeout_connect = 2;
      SimpleWeb::CaseInsensitiveMultimap headers;
      if (!token.empty()) headers.emplace("Authorization", "Bearer " + token);
      if (!if_none_match.empty()) headers.emplace("If-None-Match", if_none_match);
      const auto response = client.request("GET", path, "", headers);
      return {std::stoi(response->status_code.substr(0, 3)), response->content.string(), response->header};
    }

    static std::string artwork(const char *uuid, const char *kind) {
      return std::string("/api/apps/") + uuid + "/artwork/" + kind;
    }

    std::vector<provider_request_t> seen() {
      std::lock_guard lock(mutex);
      return requests;
    }

    void set_provider(provider_t value) {
      std::lock_guard lock(mutex);
      provider = std::move(value);
    }

    void expect_miss(const http_reply_t &reply) {
      EXPECT_EQ(reply.code, 404);
      EXPECT_EQ(reply.body, "No artwork");
      EXPECT_EQ(reply.value("Content-Type"), "text/plain");
      EXPECT_EQ(reply.value("Cache-Control"), "no-store");
      EXPECT_EQ(reply.value("X-Content-Type-Options"), "nosniff");
      EXPECT_FALSE(reply.value("Content-Security-Policy").empty());
    }

    fs::path directory, appdata;
    config::sunshine_t old_config;
    std::string old_apps_file, old_steamgriddb_key;
    std::optional<proc::proc_t> old_catalog;
    std::mutex mutex;
    std::vector<provider_request_t> requests;
    provider_t provider;
    std::promise<unsigned short> listening;
    std::function<void()> stop_server;
    std::future<void> server;
    unsigned short port = 0;
  };
}  // namespace

TEST_F(library_artwork_route, ServesTheCachedHeroAndRevalidatesItByEntityTag) {
  const auto hero = cached(appdata, GAME_UUID, "hero.steam.jpg");
  write_bytes(hero, JPEG);
  write_bytes(cached(appdata, GAME_UUID, "logo.steam.png"), PNG);

  const auto first = get(artwork(GAME_UUID, "hero"));
  EXPECT_EQ(first.code, 200);
  EXPECT_EQ(first.body, text(JPEG));
  EXPECT_EQ(first.value("Content-Type"), "image/jpeg");
  EXPECT_EQ(first.value("Cache-Control"), "private, no-cache");
  EXPECT_EQ(first.value("X-Content-Type-Options"), "nosniff");
  EXPECT_FALSE(first.value("Content-Security-Policy").empty());
  const auto etag = first.value("ETag");
  ASSERT_FALSE(etag.empty());
  EXPECT_EQ(etag.front(), '"');

  // The console may name the entry in either case; the cache is the entry's own uuid.
  std::string lower = GAME_UUID;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  EXPECT_EQ(get("/api/apps/" + lower + "/artwork/hero").body, text(JPEG));

  const auto unchanged = get(artwork(GAME_UUID, "hero"), "artwork-fixture-key", etag);
  EXPECT_EQ(unchanged.code, 304);
  EXPECT_TRUE(unchanged.body.empty());
  EXPECT_EQ(unchanged.value("ETag"), etag);
  EXPECT_EQ(unchanged.value("Cache-Control"), "private, no-cache");

  // A rewritten image is a new tag under the same URL.
  auto longer = JPEG;
  longer.insert(longer.end(), {'-', 'n', 'e', 'w'});
  write_bytes(hero, longer);
  const auto rewritten = get(artwork(GAME_UUID, "hero"), "artwork-fixture-key", etag);
  EXPECT_EQ(rewritten.code, 200);
  EXPECT_EQ(rewritten.body, text(longer));
  EXPECT_NE(rewritten.value("ETag"), etag);

  const auto logo = get(artwork(GAME_UUID, "logo"));
  EXPECT_EQ(logo.code, 200);
  EXPECT_EQ(logo.value("Content-Type"), "image/png");
  EXPECT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_TRUE(seen().empty()) << "A cached image never asks Steam";
}

TEST_F(library_artwork_route, APickMadeInNovaWinsOverSteam) {
  write_bytes(cached(appdata, GAME_UUID, "hero.steam.jpg"), JPEG);
  write_bytes(cached(appdata, GAME_UUID, "hero.override.png"), PNG);
  write_bytes(cached(appdata, GAME_UUID, "logo.steam.png"), PNG);
  const auto reply = get(artwork(GAME_UUID, "hero"));
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, text(PNG));
  EXPECT_EQ(reply.value("Content-Type"), "image/png");
  // The entry's own art also outranks Steam's.
  write_bytes(cached(appdata, GAME_UUID, "logo.local.jpg"), JPEG);
  EXPECT_EQ(get(artwork(GAME_UUID, "logo")).body, text(JPEG));
  EXPECT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_TRUE(seen().empty());
}

TEST_F(library_artwork_route, AnUnknownEntryIsAMissThatFetchesNothing) {
  // Even with another entry's images cached under that uuid's name.
  write_bytes(cached(appdata, UNKNOWN_UUID, "hero.steam.jpg"), JPEG);
  expect_miss(get(artwork(UNKNOWN_UUID, "hero")));
  expect_miss(get(artwork(UNKNOWN_UUID, "logo")));
  EXPECT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_TRUE(seen().empty());
}

TEST_F(library_artwork_route, RefusesOtherKindsMalformedIdsAndStrangers) {
  write_bytes(cached(appdata, GAME_UUID, "poster.steam.jpg"), JPEG);
  write_bytes(cached(appdata, GAME_UUID, "icon.steam.png"), PNG);
  // The poster is the entry's own image, served by /api/covers/image; this route has none.
  EXPECT_EQ(get(artwork(GAME_UUID, "poster")).code, 404);
  EXPECT_EQ(get(artwork(GAME_UUID, "icon")).code, 404);
  EXPECT_EQ(get(artwork(GAME_UUID, "HERO")).code, 404);
  EXPECT_EQ(get(artwork("not-a-uuid", "hero")).code, 400);
  EXPECT_EQ(get("/api/apps/..%2F..%2F..%2Fetc%2Fpasswd/artwork/hero").code, 400);
  EXPECT_EQ(get("/api/apps/%2E%2E/artwork/logo").code, 400);
  EXPECT_EQ(get("/api/apps/../artwork/hero").code, 400);
  EXPECT_EQ(get(artwork(GAME_UUID, "hero"), "").code, 401);
  EXPECT_EQ(get(artwork(GAME_UUID, "hero"), "wrong-key").code, 401);
  EXPECT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_TRUE(seen().empty()) << "No refused request may start a fill";
}

TEST_F(library_artwork_route, RemoveArtworkMeansTheConsoleNeverFetchesAgain) {
  ASSERT_TRUE(game_artwork::remove_downloaded_artwork(appdata, OFF_UUID));
  expect_miss(get(artwork(OFF_UUID, "hero")));
  expect_miss(get(artwork(OFF_UUID, "logo")));
  // The entry's own image still shows.
  write_bytes(cached(appdata, OFF_UUID, "logo.local.png"), PNG);
  EXPECT_EQ(get(artwork(OFF_UUID, "logo")).code, 200);
  EXPECT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_TRUE(seen().empty());
}

TEST_F(library_artwork_route, ADesktopEntryShowsNoSteamGridDbMatchAndNeverFetches) {
  const auto apps = proc::proc.get_apps();
  const auto desktop = std::find_if(apps.begin(), apps.end(), [](const auto &app) { return app.uuid == DESKTOP_UUID; });
  ASSERT_NE(desktop, apps.end());
  ASSERT_TRUE(proc::uses_bundled_utility_artwork(*desktop));
  ASSERT_EQ(desktop->steam_appid, "660");

  write_bytes(cached(appdata, DESKTOP_UUID, "hero.steamgriddb.png"), PNG);
  expect_miss(get(artwork(DESKTOP_UUID, "hero")));
  write_bytes(cached(appdata, DESKTOP_UUID, "logo.override.png"), PNG);
  const auto logo = get(artwork(DESKTOP_UUID, "logo"));
  EXPECT_EQ(logo.code, 200);
  EXPECT_EQ(logo.body, text(PNG));
  // Nor does a game Steam does not know start one.
  expect_miss(get(artwork(LOCAL_UUID, "hero")));
  EXPECT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_TRUE(seen().empty());
}

TEST_F(library_artwork_route, AMissingImageAnswersAtOnceAndShowsAfterOneBackgroundFill) {
  // The held fill owns its gate, so a failed assertion here cannot leave it waiting on this frame.
  struct gate_t {
    std::promise<void> entered, release;
    std::shared_future<void> released = release.get_future().share();
    std::atomic<bool> first {true};
  };
  const auto gate = std::make_shared<gate_t>();
  auto entered = gate->entered.get_future();
  set_provider([gate](const provider_request_t &request) -> std::optional<reply_t> {
    if (gate->first.exchange(false)) gate->entered.set_value();
    gate->released.wait();
    if (request.url == std::string(STEAM_ROOT) + "library_hero.jpg") return reply_t {200, JPEG, {}};
    if (request.url == std::string(STEAM_ROOT) + "logo.png") return reply_t {200, PNG, {}};
    return reply_t {404, {}, {}};
  });
  auto release_on_failure = util::fail_guard([gate] { gate->release.set_value(); });

  expect_miss(get(artwork(GAME_UUID, "hero")));
  ASSERT_EQ(entered.wait_for(2s), std::future_status::ready);
  // Held upstream, the one-thread console still answers, and starts nothing more for this entry.
  expect_miss(get(artwork(GAME_UUID, "logo")));
  expect_miss(get(artwork(GAME_UUID, "hero")));
  EXPECT_EQ(seen().size(), 1u);

  gate->release.set_value();
  release_on_failure.disable();
  ASSERT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));

  const auto requests = seen();
  ASSERT_EQ(requests.size(), 2u);
  std::multiset<std::string> urls;
  for (const auto &request : requests) {
    urls.insert(request.url);
    EXPECT_EQ(request.provider, game_artwork::provider_e::steam);
    EXPECT_EQ(request.operation, game_artwork::providers::operation_e::download);
    EXPECT_FALSE(request.requires_authorization) << request.url;
    EXPECT_TRUE(request.url.starts_with("https://cdn.cloudflare.steamstatic.com/")) << request.url;
  }
  EXPECT_EQ(urls, (std::multiset<std::string> {std::string(STEAM_ROOT) + "library_hero.jpg", std::string(STEAM_ROOT) + "logo.png"}));

  const auto hero = get(artwork(GAME_UUID, "hero"));
  EXPECT_EQ(hero.code, 200);
  EXPECT_EQ(hero.body, text(JPEG));
  EXPECT_EQ(hero.value("Content-Type"), "image/jpeg");
  const auto logo = get(artwork(GAME_UUID, "logo"));
  EXPECT_EQ(logo.code, 200);
  EXPECT_EQ(logo.body, text(PNG));
  EXPECT_TRUE(fs::exists(cached(appdata, GAME_UUID, "hero.steam.jpg")));
  EXPECT_TRUE(fs::exists(cached(appdata, GAME_UUID, "logo.steam.png")));
  EXPECT_FALSE(fs::exists(cached(appdata, GAME_UUID, "poster.steam.jpg"))) << "The poster is never fetched";
  EXPECT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_EQ(seen().size(), 2u);

  // Remove artwork, then Find artwork again: a fill that got everything left no retry window, so
  // the next view fills both again, and not while the lookup is off.
  ASSERT_TRUE(game_artwork::remove_downloaded_artwork(appdata, GAME_UUID));
  expect_miss(get(artwork(GAME_UUID, "hero")));
  ASSERT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_EQ(seen().size(), 2u);
  ASSERT_TRUE(game_artwork::enable_automatic_artwork_lookup(appdata, GAME_UUID));
  expect_miss(get(artwork(GAME_UUID, "hero")));
  ASSERT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_EQ(seen().size(), 4u);
  EXPECT_EQ(get(artwork(GAME_UUID, "hero")).code, 200);
  EXPECT_EQ(get(artwork(GAME_UUID, "logo")).code, 200);
}

TEST_F(library_artwork_route, AFailedFillWaitsAnHourBeforeAskingSteamAgain) {
  set_provider([](const provider_request_t &) -> std::optional<reply_t> { return reply_t {404, {}, {}}; });
  expect_miss(get(artwork(GAME_UUID, "hero")));
  ASSERT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_EQ(seen().size(), 2u);

  expect_miss(get(artwork(GAME_UUID, "hero")));
  expect_miss(get(artwork(GAME_UUID, "logo")));
  ASSERT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_EQ(seen().size(), 2u) << "A title Steam has no art for is asked once an hour, not once a view";

  // The window, not anything else, is what held the second view back.
  confighttp::forget_library_artwork_fills_for_tests();
  expect_miss(get(artwork(GAME_UUID, "logo")));
  ASSERT_TRUE(confighttp::wait_for_library_artwork_fills_for_tests(5s));
  EXPECT_EQ(seen().size(), 4u);
}

TEST(LibraryArtworkFillPlan, OnlyASteamTitlesMissingBackgroundAndLogoFromSteam) {
  const auto appdata = fs::temp_directory_path() /
    ("polaris-library-artwork-plan-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(appdata);
  auto cleanup = util::fail_guard([&] {
    std::error_code ignored;
    fs::remove_all(appdata, ignored);
  });
  proc::ctx_t game {};
  game.uuid = GAME_UUID;
  game.name = "Portal";
  game.cmd = "/usr/bin/true";
  game.steam_appid = STEAM_APPID;

  const auto plan = confighttp::library_artwork_fill_plan(appdata, game);
  ASSERT_EQ(plan.size(), 2u);
  EXPECT_EQ(plan[0].kind, game_artwork::kind_e::hero);
  EXPECT_EQ(plan[0].url, std::string(STEAM_ROOT) + "library_hero.jpg");
  EXPECT_EQ(plan[1].kind, game_artwork::kind_e::logo);
  EXPECT_EQ(plan[1].url, std::string(STEAM_ROOT) + "logo.png");
  for (const auto &request : plan) {
    EXPECT_EQ(request.provider, game_artwork::provider_e::steam);
    EXPECT_EQ(request.operation, game_artwork::providers::operation_e::download);
    EXPECT_FALSE(request.requires_authorization);
  }

  // What the cache holds from any source is not asked for again; a poster never is.
  write_bytes(cached(appdata, GAME_UUID, "hero.steamgriddb.png"), PNG);
  const auto logo_only = confighttp::library_artwork_fill_plan(appdata, game);
  ASSERT_EQ(logo_only.size(), 1u);
  EXPECT_EQ(logo_only[0].kind, game_artwork::kind_e::logo);
  write_bytes(cached(appdata, GAME_UUID, "logo.local.png"), PNG);
  EXPECT_TRUE(confighttp::library_artwork_fill_plan(appdata, game).empty());

  proc::ctx_t other = game;
  other.uuid = OFF_UUID;
  EXPECT_EQ(confighttp::library_artwork_fill_plan(appdata, other).size(), 2u);
  ASSERT_TRUE(game_artwork::remove_downloaded_artwork(appdata, OFF_UUID));
  EXPECT_TRUE(confighttp::library_artwork_fill_plan(appdata, other).empty()) << "Remove artwork";
  ASSERT_TRUE(game_artwork::enable_automatic_artwork_lookup(appdata, OFF_UUID));
  EXPECT_EQ(confighttp::library_artwork_fill_plan(appdata, other).size(), 2u) << "Find artwork again";

  proc::ctx_t desktop = other;
  desktop.desktop_mirror = true;
  EXPECT_TRUE(confighttp::library_artwork_fill_plan(appdata, desktop).empty());
  proc::ctx_t utility = other;
  utility.uuid = DESKTOP_UUID;
  utility.cmd.clear();
  utility.steam_appid.clear();
  EXPECT_TRUE(confighttp::library_artwork_fill_plan(appdata, utility).empty());
  for (const auto *appid : {"", " ", "abc", "620; rm", "123456789012345678901"}) {
    proc::ctx_t unknown = other;
    unknown.steam_appid = appid;
    EXPECT_TRUE(confighttp::library_artwork_fill_plan(appdata, unknown).empty()) << appid;
  }
  for (const auto *uuid : {"", "not-a-uuid", "../7D3E0B6C-2A41-4F88-9C15-0E6A5B4D3C21"}) {
    proc::ctx_t malformed = other;
    malformed.uuid = uuid;
    EXPECT_TRUE(confighttp::library_artwork_fill_plan(appdata, malformed).empty()) << uuid;
  }
}
