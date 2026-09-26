/**
 * @file tests/unit/test_cover_sweep_routes.cpp
 * @brief The three cover sweep routes, driven without a network.
 *
 * The review that found this file missing found two defects it would have caught: a run with nothing
 * to look up answered with the previous run's proposals, so applying them a second time could write
 * over a cover somebody had picked by hand; and there was no coverage of what the routes answer at all.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "src/artwork_sweep.h"
#include "src/config.h"
#include "src/game_artwork_manual.h"
#include "src/game_artwork_override.h"
#include "src/nvhttp.h"
#include "src/crypto.h"
#include "src/file_handler.h"
#include "src/private_state_file.h"
#include "src/process.h"
#include "../tests_common.h"

#include <Simple-Web-Server/client_https.hpp>
#include <Simple-Web-Server/server_https.hpp>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Declared here rather than in a header, the way the ROM folder route tests declare theirs.
namespace confighttp {
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  void startCoverSweep(resp_https_t, req_https_t);
  void getCoverSweep(resp_https_t, req_https_t);
  void clearCoverSweep(resp_https_t, req_https_t);

  void set_cover_sweep_lookup_for_tests(artwork_sweep::lookup_fn_t lookup);
  bool wait_for_cover_sweep_for_tests(std::chrono::milliseconds timeout);
  void forget_cover_sweep_for_tests();
  void with_web_session_for_tests(const fs::path &path, const std::string &csrf,
                                  const std::function<void(const std::string &)> &run);
}  // namespace confighttp

namespace {

  nlohmann::json two_games(const std::string &one, const std::string &two) {
    return {
      {"apps",
       nlohmann::json::array({
         {{"uuid", one}, {"name", "Blank One"}, {"cmd", "/usr/bin/true"}, {"image-path", ""}},
         {{"uuid", two}, {"name", "Blank Two"}, {"cmd", "/usr/bin/true"}, {"image-path", ""}},
       })},
    };
  }

  /// A one pixel PNG. Validation reads the bytes, so only a real file satisfies it.
  void write_png(const fs::path &path) {
    static constexpr unsigned char png[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
      0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
      0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
      0x42, 0x60, 0x82};
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(png), sizeof(png));
  }

}  // namespace

TEST(CoverSweepRoutes, ARunIsStartedWatchedAndForgotten) {
  constexpr auto kOne = "11111111-1111-4111-8111-111111111111";
  constexpr auto kTwo = "22222222-2222-4222-8222-222222222222";

  const auto directory = fs::temp_directory_path() /
    ("cover-sweep-routes-" + std::to_string(::getpid()));
  fs::create_directories(directory);

  const auto old_config = config::sunshine;
  const auto old_file_apps = config::stream.file_apps;
  const auto old_key = config::steamgriddb_api_key();

  auto restore = util::fail_guard([&] {
    confighttp::forget_cover_sweep_for_tests();
    confighttp::set_cover_sweep_lookup_for_tests({});
    config::set_steamgriddb_api_key(old_key);
    config::sunshine = old_config;
    config::stream.file_apps = old_file_apps;
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  });

  // Without a username the handlers redirect to the welcome page rather than authenticating.
  config::sunshine.username = "test-admin";
  config::sunshine.api_key = "isolated-test-api-key";
  config::sunshine.config_file = (directory / "polaris.conf").string();
  config::stream.file_apps = (directory / "apps.json").string();
  private_state_file::write_atomic(config::stream.file_apps, two_games(kOne, kTwo).dump(2));
  proc::refresh(config::stream.file_apps, false);

  // Every lookup answers a match, with no network anywhere near it.
  confighttp::set_cover_sweep_lookup_for_tests([](const artwork_sweep::candidate_t &game) {
    artwork_sweep::lookup_t answer;
    artwork_sweep::match_t match;
    match.provider_game_id = "2254";
    match.title = game.name + " (matched)";
    match.confidence = 90;
    answer.match = match;
    return answer;
  });

  auto [certificate, key] = crypto::gen_creds("localhost", 2048);
  private_state_file::write_atomic((directory / "cert.pem").string(), certificate);
  private_state_file::write_atomic((directory / "key.pem").string(), key);

  confighttp::with_web_session_for_tests(directory / "sessions.json", "test-csrf", [&](const std::string &cookie) {
    SimpleWeb::Server<SimpleWeb::HTTPS> server((directory / "cert.pem").string(), (directory / "key.pem").string());
    server.config.address = "127.0.0.1";
    server.config.port = 0;
    server.config.timeout_request = 5;
    server.config.timeout_content = 5;
    server.resource["^/api/covers/sweep$"]["POST"] = confighttp::startCoverSweep;
    server.resource["^/api/covers/sweep$"]["GET"] = confighttp::getCoverSweep;
    server.resource["^/api/covers/sweep$"]["DELETE"] = confighttp::clearCoverSweep;

    std::atomic<unsigned short> port {0};
    std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
    auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
    for (int attempt = 0; attempt < 200 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
    ASSERT_NE(port, 0);

    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port.load()), false);
    client.config.timeout = 5;
    const auto request = [&](const std::string &method, const std::string &body, bool authenticated = true) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      if (authenticated) headers.emplace("Cookie", "auth=" + cookie);
      return client.request(method, "/api/covers/sweep", body, headers);
    };
    const auto code = [](const auto &response) {
      return std::stoi(response->status_code.substr(0, 3));
    };
    const auto body = [](const auto &response) {
      return nlohmann::json::parse(response->content.string(), nullptr, false);
    };

    // Unauthenticated, all three.
    EXPECT_EQ(code(request("POST", "{}", false)), 401);
    EXPECT_EQ(code(request("GET", "", false)), 401);
    EXPECT_EQ(code(request("DELETE", "", false)), 401);

    // Without a key there is nothing to ask, and the answer says so rather than starting a run.
    config::set_steamgriddb_api_key("");
    {
      const auto answer = request("POST", "{}");
      EXPECT_EQ(code(answer), 503);
      const auto out = body(answer);
      ASSERT_FALSE(out.is_discarded());
      EXPECT_EQ(out.value("status", true), false);
      EXPECT_EQ(out.value("code", std::string {}), "steamgriddb_key_missing");
    }

    config::set_steamgriddb_api_key("a-key-that-is-never-used");

    // Started.
    {
      const auto answer = request("POST", "{}");
      EXPECT_EQ(code(answer), 202);
      const auto out = body(answer);
      ASSERT_FALSE(out.is_discarded());
      EXPECT_TRUE(out.value("status", false));
      EXPECT_EQ(out["sweep"].value("total", 0), 2);
    }

    ASSERT_TRUE(confighttp::wait_for_cover_sweep_for_tests(30s));

    // Watched.
    {
      const auto out = body(request("GET", ""));
      ASSERT_FALSE(out.is_discarded());
      const auto &sweep = out["sweep"];
      EXPECT_EQ(sweep.value("state", std::string {}), "ready");
      EXPECT_EQ(sweep.value("looked_at", 0), 2);
      EXPECT_EQ(sweep.value("proposed", 0), 2);
      ASSERT_EQ(sweep["proposals"].size(), 2u);
      EXPECT_EQ(sweep["proposals"][0].value("outcome", std::string {}), "proposed");
      EXPECT_EQ(sweep["proposals"][0].value("title", std::string {}), "Blank One (matched)");
      EXPECT_EQ(sweep["proposals"][0].value("provider_game_id", std::string {}), "2254");
      EXPECT_EQ(sweep["proposals"][0].value("confidence", 0), 90);
    }

    // Forgotten.
    {
      const auto out = body(request("DELETE", ""));
      ASSERT_FALSE(out.is_discarded());
      EXPECT_TRUE(out.value("forgotten", false));
      EXPECT_EQ(out["sweep"].value("total", -1), 0);
      EXPECT_TRUE(out["sweep"]["proposals"].empty());
    }
  });
}

TEST(CoverSweepRoutes, NothingToLookUpForgetsTheRunBeforeIt) {
  constexpr auto kOne = "33333333-3333-4333-8333-333333333333";

  const auto directory = fs::temp_directory_path() /
    ("cover-sweep-nothing-" + std::to_string(::getpid()));
  fs::create_directories(directory);

  const auto old_config = config::sunshine;
  const auto old_file_apps = config::stream.file_apps;
  const auto old_key = config::steamgriddb_api_key();

  auto restore = util::fail_guard([&] {
    confighttp::forget_cover_sweep_for_tests();
    confighttp::set_cover_sweep_lookup_for_tests({});
    config::set_steamgriddb_api_key(old_key);
    config::sunshine = old_config;
    config::stream.file_apps = old_file_apps;
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  });

  config::sunshine.username = "test-admin";
  config::sunshine.api_key = "isolated-test-api-key";
  config::sunshine.config_file = (directory / "polaris.conf").string();
  config::stream.file_apps = (directory / "apps.json").string();
  config::set_steamgriddb_api_key("a-key-that-is-never-used");

  // One game with no cover: a run that proposes something.
  nlohmann::json one {
    {"apps", nlohmann::json::array({{{"uuid", kOne}, {"name", "Blank"}, {"cmd", "/usr/bin/true"}, {"image-path", ""}}})}};
  private_state_file::write_atomic(config::stream.file_apps, one.dump(2));
  proc::refresh(config::stream.file_apps, false);

  confighttp::set_cover_sweep_lookup_for_tests([](const artwork_sweep::candidate_t &) {
    artwork_sweep::lookup_t answer;
    artwork_sweep::match_t match;
    match.provider_game_id = "2254";
    match.title = "Something";
    answer.match = match;
    return answer;
  });

  auto [certificate, key] = crypto::gen_creds("localhost", 2048);
  private_state_file::write_atomic((directory / "cert.pem").string(), certificate);
  private_state_file::write_atomic((directory / "key.pem").string(), key);

  confighttp::with_web_session_for_tests(directory / "sessions.json", "test-csrf", [&](const std::string &cookie) {
    SimpleWeb::Server<SimpleWeb::HTTPS> server((directory / "cert.pem").string(), (directory / "key.pem").string());
    server.config.address = "127.0.0.1";
    server.config.port = 0;
    server.config.timeout_request = 5;
    server.config.timeout_content = 5;
    server.resource["^/api/covers/sweep$"]["POST"] = confighttp::startCoverSweep;

    std::atomic<unsigned short> port {0};
    std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
    auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
    for (int attempt = 0; attempt < 200 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
    ASSERT_NE(port, 0);

    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port.load()), false);
    client.config.timeout = 5;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Cookie", "auth=" + cookie);

    auto first = client.request("POST", "/api/covers/sweep", "{}", headers);
    ASSERT_EQ(std::stoi(first->status_code.substr(0, 3)), 202);
    ASSERT_TRUE(confighttp::wait_for_cover_sweep_for_tests(30s));

    // Now give that game a cover, so there is nothing left to look up. A real file rather than one of
    // the images Polaris ships: a bare name resolves against the installed asset directory, which a
    // build tree on a runner does not have, so the game would still read as having no cover.
    const auto cover = directory / "cover.png";
    write_png(cover);
    nlohmann::json covered = one;
    covered["apps"][0]["image-path"] = cover.string();
    private_state_file::write_atomic(config::stream.file_apps, covered.dump(2));
    proc::refresh(config::stream.file_apps, false);

    auto second = client.request("POST", "/api/covers/sweep", "{}", headers);
    EXPECT_EQ(std::stoi(second->status_code.substr(0, 3)), 200);
    const auto out = nlohmann::json::parse(second->content.string(), nullptr, false);
    ASSERT_FALSE(out.is_discarded());
    EXPECT_TRUE(out.value("nothing_to_do", false));
    // The previous run's proposals are about a game that now has a cover. Answering with them would
    // let the console offer to store one over it.
    EXPECT_EQ(out["sweep"].value("total", -1), 0);
    EXPECT_TRUE(out["sweep"]["proposals"].empty());
  });
}

namespace confighttp {
  void apply_missing_cover_http_for_tests(resp_https_t, req_https_t);
  void importGames(resp_https_t, req_https_t);
  void addLibrarySource(resp_https_t, req_https_t);
  void set_cover_apply_for_tests(game_artwork::providers::transport_t, std::optional<fs::path>);
}

TEST(CoverSweepRoutes, ImportCoversRespectFreshEntriesCancellationAndPersistence) {
  constexpr auto one = "77777777-7777-4777-8777-777777777777";
  constexpr auto two = "88888888-8888-4888-8888-888888888888";
  const auto directory = fs::temp_directory_path() / ("cover-import-" + std::to_string(::getpid()));
  fs::create_directories(directory);
  const auto old_config = config::sunshine;
  const auto old_apps = config::stream.file_apps;
  const auto old_key = config::steamgriddb_api_key();
  auto restore = util::fail_guard([&] {
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
    confighttp::forget_cover_sweep_for_tests();
    confighttp::set_cover_sweep_lookup_for_tests({});
    confighttp::set_cover_apply_for_tests({}, std::nullopt);
    nvhttp::artwork_candidate_previews().clear_game(one);
    config::sunshine = old_config;
    config::stream.file_apps = old_apps;
    config::set_steamgriddb_api_key(old_key);
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  });
  config::sunshine.username = "test-admin";
  config::sunshine.api_key = "isolated-test-api-key";
  config::sunshine.config_file = (directory / "polaris.conf").string();
  config::stream.file_apps = (directory / "apps.json").string();
  config::set_steamgriddb_api_key("unused-test-key");
  confighttp::set_cover_sweep_lookup_for_tests([](const auto &game) {
    artwork_sweep::lookup_t answer;
    answer.match = artwork_sweep::match_t {"2254", game.name, 100, std::nullopt};
    return answer;
  });
  write_png(directory / "fixture.png");
  const auto image_text = file_handler::read_file((directory / "fixture.png").c_str());
  const std::vector<unsigned char> image(image_text.begin(), image_text.end());
  std::function<void()> during_download;
  int downloads = 0;
  confighttp::set_cover_apply_for_tests([&](const auto &, auto) {
    ++downloads;
    if (during_download) during_download();
    return std::optional {game_artwork::providers::transport_response_t {200, image, {}}};
  }, directory);
  auto [certificate, key] = crypto::gen_creds("localhost", 2048);
  private_state_file::write_atomic(directory / "cert.pem", certificate);
  private_state_file::write_atomic(directory / "key.pem", key);

  confighttp::with_web_session_for_tests(directory / "sessions.json", "test-csrf", [&](const std::string &cookie) {
    SimpleWeb::Server<SimpleWeb::HTTPS> server((directory / "cert.pem").string(), (directory / "key.pem").string());
    server.config.address = "127.0.0.1";
    server.config.port = 0;
    server.resource["^/api/covers/sweep$"]["POST"] = confighttp::startCoverSweep;
    server.resource["^/api/covers/sweep$"]["DELETE"] = confighttp::clearCoverSweep;
    server.resource["^/api/covers/apply-missing$"]["POST"] = confighttp::apply_missing_cover_http_for_tests;
    server.resource["^/api/games/import$"]["POST"] = confighttp::importGames;
    server.resource["^/api/library/sources$"]["POST"] = confighttp::addLibrarySource;
    std::atomic<unsigned short> port {0};
    std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
    auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
    for (int attempt = 0; attempt < 200 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
    ASSERT_NE(port, 0);
    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port.load()), false);
    client.config.timeout = 5;
    const auto request = [&](const std::string &path, const nlohmann::json &payload,
                             bool auth = true, bool csrf = true, const std::string &method = "POST") {
      SimpleWeb::CaseInsensitiveMultimap headers {{"Content-Type", "application/json"}};
      if (auth) headers.emplace("Cookie", "auth=" + cookie);
      if (csrf) headers.emplace("X-CSRF-Token", "test-csrf");
      const auto response = client.request(method, path, payload.dump(), headers);
      return std::pair {std::stoi(response->status_code.substr(0, 3)), nlohmann::json::parse(response->content.string(), nullptr, false)};
    };
    const auto read_apps = [&] { return nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str())); };
    const auto write_apps = [&](const nlohmann::json &tree) {
      EXPECT_TRUE(private_state_file::write_atomic(config::stream.file_apps, tree.dump()));
    };
    const auto reset = [&] {
      during_download = {};
      private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
      confighttp::forget_cover_sweep_for_tests();
      EXPECT_TRUE(game_artwork::clear_artwork_override(directory, one));
      EXPECT_TRUE(game_artwork::enable_automatic_artwork_lookup(directory, one));
      write_apps(two_games(one, two));
      proc::refresh(config::stream.file_apps, false);
    };
    reset();
    // Invalid and empty scopes never fall back to the old whole-library behavior.
    for (const auto &scope : {nlohmann::json("bad"), nlohmann::json::array({"bad"}), nlohmann::json::array({1})}) {
      EXPECT_EQ(request("/api/covers/sweep", {{"uuids", scope}}).first, 400);
    }
    EXPECT_EQ(request("/api/covers/sweep", nlohmann::json::array()).first, 400);
    const auto empty = request("/api/covers/sweep", {{"uuids", nlohmann::json::array()}});
    EXPECT_EQ(empty.first, 200);
    EXPECT_TRUE(empty.second.value("nothing_to_do", false));
    EXPECT_EQ(empty.second["sweep"]["total"], 0);
    std::string last_id;
    const auto prepare = [&] {
      reset();
      const auto started = request("/api/covers/sweep", {{"uuids", {one, one}}});
      EXPECT_EQ(started.first, 202);
      EXPECT_EQ(started.second["sweep"]["total"], 1);
      EXPECT_EQ(started.second["sweep"]["proposals"][0]["uuid"], one);
      const auto id = started.second["sweep"]["id"].get<std::string>();
      EXPECT_EQ(id.size(), 32u);
      EXPECT_NE(id, last_id);
      last_id = id;
      EXPECT_TRUE(confighttp::wait_for_cover_sweep_for_tests(5s));
      const auto preview = nvhttp::artwork_candidate_previews().publish(one, game_artwork::kind_e::poster,
        image, nvhttp::artwork_clock_milliseconds(), game_artwork::manual::choice_source_t {"2254", "https://cdn2.steamgriddb.com/grid/test.png"});
      EXPECT_TRUE(preview.has_value());
      return nlohmann::json {{"uuid", one}, {"token", preview->token}, {"expected_name", "Blank One"}, {"run_id", id}};
    };
    auto payload = prepare();
    EXPECT_EQ(request("/api/covers/apply-missing", payload, false).first, 401);
    EXPECT_EQ(request("/api/covers/apply-missing", payload, true, false).first, 403);
    EXPECT_EQ(downloads, 0);
    EXPECT_EQ(request("/api/covers/apply-missing", { {"uuid", one} }).first, 400);
    auto stale = payload;
    stale["token"] = "expired";
    EXPECT_EQ(request("/api/covers/apply-missing", stale).first, 410);
    stale = payload;
    stale["run_id"] = "retired";
    EXPECT_TRUE(request("/api/covers/apply-missing", stale).second.value("skipped", false));
    EXPECT_EQ(downloads, 0);

    // The network callback edits unrelated fields after the operation began.
    during_download = [&] {
      auto tree = read_apps();
      tree["apps"][0]["cmd"] = "fresh-command";
      tree["apps"][1]["name"] = "Another edited game";
      write_apps(tree);
    };
    // An unsaved manual pick under the usual UUID must not be overwritten either.
    fs::create_directories(directory / "covers");
    write_png(directory / "covers" / (std::string(one) + ".png"));
    const auto applied = request("/api/covers/apply-missing", payload);
    EXPECT_EQ(applied.first, 200) << applied.second;
    EXPECT_FALSE(applied.second.value("skipped", false));
    const auto written = read_apps();
    EXPECT_EQ(written["apps"][0]["cmd"], "fresh-command");
    EXPECT_EQ(written["apps"][1]["name"], "Another edited game");
    EXPECT_TRUE(fs::is_regular_file(written["apps"][0]["image-path"].get<std::string>()));
    EXPECT_EQ(file_handler::read_file((directory / "covers" / (std::string(one) + ".png")).c_str()), image_text);

    for (const std::string change : {"rename", "delete", "duplicate", "cover", "manual-override", "remove-artwork", "retire-run"}) {
      SCOPED_TRACE(change);
      payload = prepare();
      during_download = [&] {
        auto tree = read_apps();
        if (change == "rename") tree["apps"][0]["name"] = "Renamed";
        if (change == "delete") tree["apps"].erase(tree["apps"].begin());
        if (change == "duplicate") tree["apps"].push_back(tree["apps"][0]);
        if (change == "cover") tree["apps"][0]["image-path"] = "an-explicit-unreadable-cover.png";
        if (change == "manual-override") {
          EXPECT_TRUE(game_artwork::save_artwork_override(directory,
            {one, "steamgriddb", "999", "Manual selection", std::nullopt, true, 1}));
        }
        if (change == "remove-artwork") {
          EXPECT_TRUE(game_artwork::remove_downloaded_artwork(directory, one));
        }
        if (change == "retire-run") confighttp::forget_cover_sweep_for_tests();
        write_apps(tree);
      };
      const auto answer = request("/api/covers/apply-missing", payload);
      EXPECT_EQ(answer.first, 200) << answer.second;
      EXPECT_TRUE(answer.second.value("skipped", false)) << answer.second;
      const auto tree = read_apps();
      for (const auto &entry : tree["apps"]) {
        EXPECT_EQ(entry.value("image-path", std::string {}),
          change == "cover" && entry["uuid"] == one ? "an-explicit-unreadable-cover.png" : "");
      }
    }

    // A canceled finished run cannot publish even though its proposals were previously available.
    payload = prepare();
    EXPECT_EQ(request("/api/covers/sweep", nlohmann::json::object(), true, true, "DELETE").first, 200);
    const auto before_cancelled = downloads;
    EXPECT_TRUE(request("/api/covers/apply-missing", payload).second.value("skipped", false));
    EXPECT_EQ(downloads, before_cancelled);

    // Failure before rename preserves the library and removes only this operation's new image.
    payload = prepare();
    const auto before_failure = file_handler::read_file(config::stream.file_apps.c_str());
    std::size_t before_files = 0;
    for (const auto &entry : fs::recursive_directory_iterator(directory / "covers")) if (entry.is_regular_file()) ++before_files;
    during_download = [] { private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::rename); };
    const auto failed = request("/api/covers/apply-missing", payload);
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
    EXPECT_EQ(failed.first, 500);
    EXPECT_EQ(file_handler::read_file(config::stream.file_apps.c_str()), before_failure);
    std::size_t after_files = 0;
    for (const auto &entry : fs::recursive_directory_iterator(directory / "covers")) if (entry.is_regular_file()) ++after_files;
    EXPECT_EQ(after_files, before_files);

    // A durability failure after rename must retain the image the committed JSON now references.
    payload = prepare();
    during_download = [] { private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::post_rename_durability); };
    const auto uncertain = request("/api/covers/apply-missing", payload);
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
    EXPECT_EQ(uncertain.first, 500);
    EXPECT_TRUE(fs::is_regular_file(read_apps()["apps"][0]["image-path"].get<std::string>()));

    // A custom ROM folder keeps this receipt check independent of installed emulator packages.
    reset();
    fs::create_directories(directory / "roms");
    const auto rom = directory / "roms" / "Fixture.sfc";
    { std::ofstream out(rom); out << "fixture"; }
    const auto added = request("/api/library/sources", {
      {"path", (directory / "roms").string()}, {"emulator", "custom"},
      {"command", "/usr/bin/true {rom}"}, {"extensions", "sfc"}
    });
    ASSERT_EQ(added.first, 200) << added.second;
    const auto source_id = added.second["sources"][0]["id"];
    const nlohmann::json rom_game {{"name", "Fixture"}, {"source", "emulator"},
      {"source_id", source_id}, {"rom_path", rom.string()}};
    const auto receipt = request("/api/games/import", {{"games", nlohmann::json::array({rom_game,
      {{"name", "Lutris Fixture"}, {"source", "lutris"}, {"slug", "polaris-auto-cover-receipt-fixture"}}
    })}});
    ASSERT_EQ(receipt.first, 200);
    ASSERT_TRUE(receipt.second.value("status", false)) << receipt.second;
    EXPECT_EQ(receipt.second["imported"], 2);
    ASSERT_EQ(receipt.second["imported_games"].size(), 2u);
    const auto imported_apps = read_apps()["apps"];
    EXPECT_GT(imported_apps.size(), 4u);  // original two, two imports and a generated launcher
    for (const auto &game : receipt.second["imported_games"]) {
      EXPECT_TRUE(game["name"] == "Fixture" || game["name"] == "Lutris Fixture");
      EXPECT_EQ(std::count_if(imported_apps.begin(), imported_apps.end(), [&](const auto &app) {
        return app["uuid"] == game["uuid"] && app["name"] == game["name"];
      }), 1);
    }
    const auto repeated = request("/api/games/import", {{"games", nlohmann::json::array({rom_game})}});
    EXPECT_EQ(repeated.second["imported"], 0);
    EXPECT_EQ(repeated.second["imported_games"], nlohmann::json::array());

  });
}
