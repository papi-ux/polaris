#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>
#include <Simple-Web-Server/client_https.hpp>
#include <Simple-Web-Server/server_https.hpp>

#include "src/config.h"
#include "src/cover_lookup_workers.h"
#include "src/crypto.h"
#include "src/game_artwork_manual.h"
#include "src/game_artwork_provider.h"
#include "src/nvhttp.h"
#include "src/private_state_file.h"
#include "src/utility.h"

namespace confighttp {
  using server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using response_t = std::shared_ptr<server_t::Response>;
  using request_t = std::shared_ptr<server_t::Request>;
  void registerCoverLookups(SimpleWeb::ServerBase<SimpleWeb::HTTPS> &, cover_lookup::workers_t &, game_artwork::providers::transport_t);
  void getCoverSweep(response_t, request_t);
  void with_web_session_for_tests(const std::filesystem::path &, const std::string &,
                                  const std::function<void(const std::string &)> &);
}

namespace {
  using namespace std::chrono_literals;
  using transport_t = game_artwork::providers::transport_t;
  using reply_t = game_artwork::providers::transport_response_t;
  constexpr auto uuid = "11111111-1111-4111-8111-111111111111";

  struct response_t {
    int code;
    nlohmann::json body;
    std::string security_policy;
  };

  struct routes_t {
    unsigned short port;
    std::string cookie;
    cover_lookup::workers_t &workers;
    std::function<void()> stop_server;

    response_t request(const std::string &method, const std::string &path, const std::string &body = {},
                       bool authorized = true, bool csrf = true, long timeout = 5) const {
      SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port), false);
      client.config.timeout = timeout;
      client.config.timeout_connect = timeout;
      SimpleWeb::CaseInsensitiveMultimap headers {{"Content-Type", "application/json"}};
      if (authorized) headers.emplace("Cookie", "auth=" + cookie);
      if (csrf) headers.emplace("X-CSRF-Token", "lookup-test-csrf");
      const auto result = client.request(method, path, body, headers);
      const auto policy = result->header.find("Content-Security-Policy");
      return {std::stoi(result->status_code.substr(0, 3)), nlohmann::json::parse(result->content.string()),
              policy == result->header.end() ? std::string {} : policy->second};
    }
    response_t lookup(bool choices) const {
      return choices ? request("POST", "/api/covers/choices", choice_body())
                     : request("GET", search_path());
    }
    static std::string search_path() { return std::string("/api/covers/search?name=Portal&uuid=") + uuid; }
    static std::string choice_body() {
      return nlohmann::json {{"uuid", uuid}, {"provider_game_id", "620"}, {"title", "Portal"}}.dump();
    }
  };

  void with_routes(transport_t transport, const std::function<void(routes_t &)> &run) {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() /
      ("cover-lookup-routes-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const auto old_config = config::sunshine;
    const auto old_key = config::steamgriddb_api_key();
    auto restore = util::fail_guard([&] {
      config::set_steamgriddb_api_key(old_key);
      config::sunshine = old_config;
      std::error_code ignored;
      fs::remove_all(directory, ignored);
    });
    config::sunshine.username = "test-admin";
    config::sunshine.config_file = (directory / "polaris.conf").string();
    config::set_steamgriddb_api_key("mock-only-key");
    auto [certificate, key] = crypto::gen_creds("localhost", 2048);
    private_state_file::write_atomic((directory / "cert.pem").string(), certificate);
    private_state_file::write_atomic((directory / "key.pem").string(), key);
    confighttp::with_web_session_for_tests(directory / "sessions.json", "lookup-test-csrf", [&](const std::string &cookie) {
      cover_lookup::workers_t workers;
      confighttp::server_t server((directory / "cert.pem").string(), (directory / "key.pem").string());
      server.config.address = "127.0.0.1";
      server.config.port = 0;
      server.config.thread_pool_size = 1;
      server.config.timeout_request = 5;
      server.config.timeout_content = 5;
      confighttp::registerCoverLookups(server, workers, std::move(transport));
      server.resource["^/api/covers/sweep$"]["GET"] = confighttp::getCoverSweep;
      std::atomic<unsigned short> port {0};
      std::jthread http([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
      const auto stop_server = [&] {
        workers.stop_accepting();
        server.stop();
        if (http.joinable()) http.join();
      };
      auto stop = util::fail_guard([&] {
        stop_server();
        workers.shutdown();
      });
      for (int i = 0; i < 200 && port == 0; ++i) std::this_thread::sleep_for(10ms);
      ASSERT_NE(port, 0);
      routes_t routes {port.load(), cookie, workers, stop_server};
      run(routes);
    });
  }
}

TEST(CoverLookupRoutes, AuthenticationAndValidationDoNotReachTheProvider) {
  std::atomic<int> calls {0};
  with_routes([&](const auto &, auto) -> std::optional<reply_t> { ++calls; return std::nullopt; }, [&](routes_t &r) {
    EXPECT_EQ(r.request("GET", routes_t::search_path(), {}, false).code, 401);
    EXPECT_EQ(r.request("POST", "/api/covers/choices", routes_t::choice_body(), false).code, 401);
    EXPECT_EQ(r.request("POST", "/api/covers/choices", routes_t::choice_body(), true, false).code, 403);
    EXPECT_EQ(r.request("GET", "/api/covers/search?name=Portal&uuid=bad").code, 400);
    EXPECT_EQ(r.request("GET", std::string("/api/covers/search?uuid=") + uuid).code, 400);
    EXPECT_EQ(r.request("POST", "/api/covers/choices", "{").code, 400);
    EXPECT_EQ(r.request("POST", "/api/covers/choices", "{}").code, 400);
    config::set_steamgriddb_api_key("");
    for (bool choices : {false, true}) {
      const auto reply = r.lookup(choices);
      EXPECT_EQ(reply.code, 503);
      EXPECT_EQ(reply.body.value("code", ""), "steamgriddb_key_missing");
    }
    EXPECT_EQ(calls, 0);
  });
}

TEST(CoverLookupRoutes, BlockedProviderLeavesAuthenticatedConsoleReadResponsive) {
  for (bool choices : {false, true}) {
    SCOPED_TRACE(choices ? "choices" : "search");
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    with_routes([&](const auto &, auto) -> std::optional<reply_t> {
      entered.set_value();
      released.wait();
      return std::nullopt;
    }, [&](routes_t &r) {
      auto slow = std::async(std::launch::async, [&] { return r.lookup(choices); });
      auto release_on_failure = util::fail_guard([&] { release.set_value(); });
      ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
      bool responsive = false;
      try {
        responsive = r.request("GET", "/api/covers/sweep", {}, true, true, 1).code == 200;
      } catch (const std::exception &) {
      }
      release.set_value();
      release_on_failure.disable();
      EXPECT_EQ(slow.get().code, 502);
      EXPECT_TRUE(responsive) << "A provider lookup blocked the one-thread console server";
    });
  }
}

TEST(CoverLookupRoutes, SearchAndChoicesShareABoundAndRefuseDuringShutdown) {
  std::atomic<int> calls {0};
  std::promise<void> release;
  auto released = release.get_future().share();
  with_routes([&](const auto &, auto) -> std::optional<reply_t> {
    ++calls;
    released.wait();
    return std::nullopt;
  }, [&](routes_t &r) {
    auto search = std::async(std::launch::async, [&] { return r.lookup(false); });
    auto choices = std::async(std::launch::async, [&] { return r.lookup(true); });
    auto release_on_failure = util::fail_guard([&] { release.set_value(); });
    for (int i = 0; i < 200 && calls != 2; ++i) std::this_thread::sleep_for(10ms);
    ASSERT_EQ(calls, 2);
    for (bool list : {false, true}) {
      const auto busy = r.lookup(list);
      EXPECT_EQ(busy.code, 503);
      EXPECT_EQ(busy.body.value("code", ""), "cover_lookup_busy");
      EXPECT_TRUE(busy.body.at(list ? "choices" : "candidates").empty());
    }
    r.workers.stop_accepting();
    const auto stopping = r.lookup(false);
    EXPECT_EQ(stopping.code, 503);
    EXPECT_EQ(stopping.body.value("code", ""), "cover_lookup_stopping");
    EXPECT_EQ(calls, 2);
    release.set_value();
    release_on_failure.disable();
    EXPECT_EQ(search.get().code, 502);
    EXPECT_EQ(choices.get().code, 502);
    r.workers.shutdown();
    EXPECT_EQ(r.lookup(true).body.value("code", ""), "cover_lookup_stopping");
    EXPECT_EQ(calls, 2);
  });
}

TEST(CoverLookupRoutes, WorkersUseTheSecurityHeadersObservedAtAdmission) {
  for (bool choices : {false, true}) {
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    with_routes([&](const auto &, auto) -> std::optional<reply_t> {
      entered.set_value();
      released.wait();
      return std::nullopt;
    }, [&](routes_t &r) {
      config::sunshine.port = 31111;
      auto pending = std::async(std::launch::async, [&] { return r.lookup(choices); });
      auto release_on_failure = util::fail_guard([&] { release.set_value(); });
      ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
      config::sunshine.port = 32222;
      release.set_value();
      release_on_failure.disable();
      const auto reply = pending.get();
      EXPECT_EQ(reply.code, 502);
      EXPECT_NE(reply.security_policy.find("https://*:31106"), std::string::npos);
      EXPECT_EQ(reply.security_policy.find("https://*:32217"), std::string::npos);
    });
  }
}

TEST(CoverLookupRoutes, ServerStopDrainsAHeldLookupWithoutWaitingForNetworkCallbacks) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<bool> provider_finished {false};
  with_routes([&](const auto &, auto) -> std::optional<reply_t> {
    entered.set_value();
    released.wait();
    provider_finished = true;
    return std::nullopt;
  }, [&](routes_t &r) {
    auto pending = std::async(std::launch::async, [&] {
      try { r.lookup(false); } catch (const std::exception &) { }
    });
    auto release_on_failure = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    r.stop_server();
    EXPECT_EQ(r.workers.submit([] {}), cover_lookup::admission_e::stopping);
    auto draining = std::async(std::launch::async, [&] { r.workers.shutdown(); });
    EXPECT_EQ(draining.wait_for(30ms), std::future_status::timeout);
    EXPECT_FALSE(provider_finished);
    release.set_value();
    release_on_failure.disable();
    draining.get();
    pending.get();
    EXPECT_TRUE(provider_finished);
    EXPECT_EQ(r.workers.submit([] {}), cover_lookup::admission_e::stopping);
  });
}

TEST(CoverLookupRoutes, ProviderFailuresAndExceptionsKeepTheirResponseContract) {
  for (unsigned int code : {0u, 401u, 429u, 500u}) {
    with_routes([&](const auto &, auto) -> std::optional<reply_t> {
      if (code == 0) throw std::runtime_error("mock provider failure");
      return reply_t {code, {}, {}};
    }, [&](routes_t &r) {
      for (bool choices : {false, true}) {
        const auto reply = r.lookup(choices);
        EXPECT_EQ(reply.code, 502);
        EXPECT_FALSE(reply.body.value("status", true));
        EXPECT_EQ(reply.body.value("code", ""), code == 0 ? "steamgriddb_unreachable" :
          code == 401 ? "steamgriddb_unauthorized" : code == 429 ? "steamgriddb_rate_limited" : "steamgriddb_unavailable");
        EXPECT_TRUE(reply.body.at(choices ? "choices" : "candidates").empty());
      }
    });
  }
}

TEST(CoverLookupRoutes, SuccessfulCandidatesAndChoicesKeepScopedLocalPreviews) {
  with_routes([](const auto &request, auto) -> std::optional<reply_t> {
    using operation_e = game_artwork::providers::operation_e;
    if (request.operation == operation_e::download) return reply_t {200, {0xff, 0xd8, 0xff, 0xe0, 1}, request.url};
    const auto data = request.operation == operation_e::search
      ? nlohmann::json::array({{{"id", 620}, {"name", "Portal"}}})
      : nlohmann::json::array({{{"id", 1}, {"url", "https://cdn2.steamgriddb.com/grid/one.png"},
                               {"thumb", "https://cdn2.steamgriddb.com/thumb/one.jpg"},
                               {"width", 600}, {"height", 900}, {"mime", "image/png"}}});
    const auto body = nlohmann::json {{"success", true}, {"data", data}}.dump();
    return reply_t {200, {body.begin(), body.end()}, request.url};
  }, [&](routes_t &r) {
    for (bool choices : {false, true}) {
      const auto reply = r.lookup(choices);
      EXPECT_EQ(reply.code, 200);
      ASSERT_TRUE(reply.body.value("status", false));
      const auto &entries = reply.body.at(choices ? "choices" : "candidates");
      ASSERT_EQ(entries.size(), 1);
      const auto token = entries[0].at("token").get<std::string>();
      EXPECT_EQ(token.size(), 32);
      EXPECT_EQ(entries[0].at("preview"), "./api/covers/preview/" + token + "?uuid=" + uuid);
      const auto now = nvhttp::artwork_clock_milliseconds();
      EXPECT_TRUE(nvhttp::artwork_candidate_previews().lookup(uuid, token, game_artwork::kind_e::poster, now));
      EXPECT_FALSE(nvhttp::artwork_candidate_previews().lookup("22222222-2222-4222-8222-222222222222", token, game_artwork::kind_e::poster, now));
      if (!choices) {
        EXPECT_EQ(reply.body.at("query"), "Portal");
        EXPECT_EQ(entries[0].at("title"), "Portal");
        EXPECT_EQ(entries[0].at("provider_game_id"), "620");
      }
    }
    nvhttp::artwork_candidate_previews().clear_game(uuid);
  });
}
