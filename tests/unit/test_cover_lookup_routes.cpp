#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <fstream>
#include <mutex>
#include <boost/asio/post.hpp>
#include <stdexcept>
#include <thread>

#include <nlohmann/json.hpp>
#include <Simple-Web-Server/client_https.hpp>
#include <Simple-Web-Server/server_https.hpp>

#include "src/artwork_sweep.h"
#include "src/config.h"
#include "src/cover_lookup_workers.h"
#include "src/crypto.h"
#include "src/game_artwork_manual.h"
#include "src/game_artwork_provider.h"
#include "src/file_handler.h"
#include "src/game_artwork_override.h"
#include "src/nvhttp.h"
#include "src/private_state_file.h"
#include "src/process.h"
#include "src/utility.h"

namespace confighttp {
  using server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using response_t = std::shared_ptr<server_t::Response>;
  using request_t = std::shared_ptr<server_t::Request>;
  void registerCoverLookups(SimpleWeb::ServerBase<SimpleWeb::HTTPS> &, cover_lookup::workers_t &, game_artwork::providers::transport_t);
  void getCoverSweep(response_t, request_t);
  void register_cover_actions_for_tests(SimpleWeb::ServerBase<SimpleWeb::HTTPS> &, cover_lookup::workers_t &);
  void set_cover_post_for_tests(std::function<void(std::function<void()>)>);
  void set_cover_commit_observer_for_tests(std::function<void()>);
  bool revoke_cover_cookie_for_tests(const std::string &);
  void set_cover_csrf_for_tests(std::string);
  void set_cover_publication_fault_for_tests(std::string);
  void set_cover_apply_for_tests(game_artwork::providers::transport_t, std::optional<std::filesystem::path>);
  void set_cover_http_transports_for_tests(
    std::function<std::optional<long>(const std::string &, const std::string &, std::string &)>,
    std::function<bool(const std::string &, const std::string &)>);
  void set_cover_sweep_lookup_for_tests(artwork_sweep::lookup_fn_t);
  bool wait_for_cover_sweep_for_tests(std::chrono::milliseconds);
  void forget_cover_sweep_for_tests();
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
    std::filesystem::path directory;
    std::function<void(std::function<void()>)> main;

    response_t request(const std::string &method, const std::string &path, const std::string &body = {},
                       bool authorized = true, bool csrf = true, long timeout = 5, const std::string &bearer = {}) const {
      SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port), false);
      client.config.timeout = timeout;
      client.config.timeout_connect = timeout;
      SimpleWeb::CaseInsensitiveMultimap headers {{"Content-Type", "application/json"}};
      if (authorized) headers.emplace("Cookie", "auth=" + cookie);
      if (csrf) headers.emplace("X-CSRF-Token", "lookup-test-csrf");
      if (!bearer.empty()) headers.emplace("Authorization", "Bearer " + bearer);
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


namespace {
  constexpr auto action_uuid = "33333333-3333-4333-8333-333333333333";
  constexpr auto action_url = "https://cdn2.steamgriddb.com/grid/test.png";

  struct action_transports_t {
    transport_t image;
    std::function<std::optional<long>(const std::string &, const std::string &, std::string &)> key_check;
    std::function<bool(const std::string &, const std::string &)> download;
  };

  // Separate from the original lookup fixture so its seven route tests remain unchanged.
  // This copies only the cached catalog/environment, never its process/session owner.
  void with_action_routes(action_transports_t transports, const std::function<void(routes_t &)> &run) {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() /
      ("cover-action-routes-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(directory);
    const auto old_config = config::sunshine;
    const auto old_apps_file = config::stream.file_apps;
    const auto old_key = config::steamgriddb_api_key();
    proc::proc_t old_catalog {proc::proc.get_env(), proc::proc.get_apps()};
    auto restore = util::fail_guard([&] {
      confighttp::forget_cover_sweep_for_tests();
      confighttp::set_cover_sweep_lookup_for_tests({});
      confighttp::set_cover_apply_for_tests({}, std::nullopt);
      confighttp::set_cover_http_transports_for_tests({}, {});
      confighttp::set_cover_post_for_tests({});
      confighttp::set_cover_commit_observer_for_tests({});
      confighttp::set_cover_publication_fault_for_tests({});
      nvhttp::artwork_candidate_previews().clear_game(action_uuid);
      config::set_steamgriddb_api_key(old_key);
      config::sunshine = old_config;
      config::stream.file_apps = old_apps_file;
      // Reload only the copied model; do not invoke refresh's process termination default.
      proc::proc.reload_configuration(std::move(old_catalog));
      std::error_code ignored;
      fs::remove_all(directory, ignored);
    });
    config::sunshine.username = "test-admin";
    config::sunshine.config_file = (directory / "polaris.conf").string();
    config::stream.file_apps = (directory / "apps.json").string();
    config::set_steamgriddb_api_key("mock-only-key");
    const auto apps = nlohmann::json {
      {"version", 15}, {"apps", nlohmann::json::array({
        {{"uuid", action_uuid}, {"name", "Portal"}, {"cmd", "/usr/bin/true"}, {"image-path", ""}},
      })},
    };
    ASSERT_TRUE(private_state_file::write_atomic(config::stream.file_apps, apps.dump()));
    proc::refresh(config::stream.file_apps, false);
    const auto current_apps = proc::proc.get_apps();
    ASSERT_EQ(std::count_if(current_apps.begin(), current_apps.end(), [](const auto &app) {
      return app.uuid == action_uuid;
    }), 1);
    const auto lookup_transport = transports.image;
    confighttp::set_cover_apply_for_tests(std::move(transports.image), directory);
    confighttp::set_cover_http_transports_for_tests(std::move(transports.key_check), std::move(transports.download));
    confighttp::set_cover_sweep_lookup_for_tests([](const artwork_sweep::candidate_t &game) {
      artwork_sweep::lookup_t result;
      result.match = artwork_sweep::match_t {"620", game.name, 100, std::nullopt};
      return result;
    });
    auto [certificate, key] = crypto::gen_creds("localhost", 2048);
    ASSERT_TRUE(private_state_file::write_atomic((directory / "cert.pem").string(), certificate));
    ASSERT_TRUE(private_state_file::write_atomic((directory / "key.pem").string(), key));
    confighttp::with_web_session_for_tests(directory / "sessions.json", "lookup-test-csrf", [&](const std::string &cookie) {
      cover_lookup::workers_t workers;
      confighttp::server_t server((directory / "cert.pem").string(), (directory / "key.pem").string());
      server.config.address = "127.0.0.1";
      server.config.port = 0;
      server.config.thread_pool_size = 1;
      server.config.timeout_request = 5;
      server.config.timeout_content = 5;
      confighttp::register_cover_actions_for_tests(server, workers);
      confighttp::registerCoverLookups(server, workers, lookup_transport);
      std::atomic<unsigned short> port {0};
      std::jthread http([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
      const auto stop_server = [&] {
        workers.stop_accepting();
        server.stop();
        if (http.joinable()) http.join();
      };
      auto stop = util::fail_guard([&] { stop_server(); workers.shutdown(); });
      for (int i = 0; i < 200 && port == 0; ++i) std::this_thread::sleep_for(10ms);
      ASSERT_NE(port, 0);
      routes_t routes {port.load(), cookie, workers, stop_server};
      routes.directory = directory;
      routes.main = [&](std::function<void()> task) {
        std::promise<void> done;
        auto finished = done.get_future();
        boost::asio::post(*server.io_service, [&] {
          try { task(); done.set_value(); } catch (...) { done.set_exception(std::current_exception()); }
        });
        finished.get();
      };
      run(routes);
    });
  }

  std::optional<game_artwork::manual::preview_t> publish_action_choice() {
    // A scoped choice forces the real full-image transport path rather than a cached poster.
    return nvhttp::artwork_candidate_previews().publish(action_uuid, game_artwork::kind_e::poster,
      {0xff, 0xd8, 0xff, 0xe0, 1}, nvhttp::artwork_clock_milliseconds(),
      game_artwork::manual::choice_source_t {"620", action_url});
  }

  void expect_held_action_responsive(routes_t &r, const std::string &path, const nlohmann::json &payload,
                                     std::promise<void> &entered, std::promise<void> &release,
                                     std::atomic<int> &calls, int expected_code) {
    // First prove that the unrelated read works and rejected requests cannot invoke the seam.
    ASSERT_EQ(r.request("GET", "/api/covers/sweep").code, 200);
    ASSERT_EQ(r.request("POST", path, payload.dump(), false).code, 401);
    ASSERT_EQ(r.request("POST", path, payload.dump(), true, false).code, 403);
    ASSERT_EQ(calls, 0);
    auto pending = std::async(std::launch::async, [&] { return r.request("POST", path, payload.dump()); });
    auto release_on_failure = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready) << path;
    bool responsive = false;
    try {
      responsive = r.request("GET", "/api/covers/sweep", {}, true, true, 1).code == 200;
    } catch (const std::exception &) {
    }
    release.set_value();
    release_on_failure.disable();
    const auto reply = pending.get();
    EXPECT_EQ(reply.code, expected_code);
    EXPECT_FALSE(reply.body.value("status", true));
    EXPECT_EQ(calls, 1) << "The actual route must reach exactly one held provider call";
    EXPECT_TRUE(responsive) << path << " blocked the one-thread authenticated console while its provider was held";
  }
}

TEST(CoverLookupRoutes, SelectHeldImageLeavesTheOneThreadConsoleResponsive) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<int> calls {0};
  with_action_routes({[&](const auto &request, auto) -> std::optional<reply_t> {
    EXPECT_EQ(request.operation, game_artwork::providers::operation_e::download);
    EXPECT_EQ(request.url, action_url);
    if (++calls == 1) entered.set_value();
    released.wait();
    return std::nullopt;
  }, {}, {}}, [&](routes_t &r) {
    const auto preview = publish_action_choice();
    ASSERT_TRUE(preview);
    expect_held_action_responsive(r, "/api/covers/select", {{"uuid", action_uuid}, {"token", preview->token}},
      entered, release, calls, 502);
  });
}

TEST(CoverLookupRoutes, ApplyMissingHeldImageLeavesTheOneThreadConsoleResponsive) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<int> calls {0};
  with_action_routes({[&](const auto &request, auto) -> std::optional<reply_t> {
    EXPECT_EQ(request.operation, game_artwork::providers::operation_e::download);
    EXPECT_EQ(request.url, action_url);
    if (++calls == 1) entered.set_value();
    released.wait();
    return std::nullopt;
  }, {}, {}}, [&](routes_t &r) {
    const auto sweep = r.request("POST", "/api/covers/sweep", nlohmann::json {{"uuids", {action_uuid}}}.dump());
    ASSERT_EQ(sweep.code, 202);
    ASSERT_TRUE(confighttp::wait_for_cover_sweep_for_tests(5s));
    const auto preview = publish_action_choice();
    ASSERT_TRUE(preview);
    expect_held_action_responsive(r, "/api/covers/apply-missing", {
      {"uuid", action_uuid}, {"token", preview->token}, {"run_id", sweep.body.at("sweep").at("id")},
      {"expected_name", "Portal"},
    }, entered, release, calls, 502);
  });
}

TEST(CoverLookupRoutes, DownloadHeldImageLeavesTheOneThreadConsoleResponsive) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<int> calls {0};
  with_action_routes({{}, {}, [&](const std::string &url, const std::string &path) {
    EXPECT_EQ(url, action_url);
    EXPECT_EQ(std::filesystem::path(path).filename(), std::string(action_uuid) + ".png");
    if (++calls == 1) entered.set_value();
    released.wait();
    return false;
  }}, [&](routes_t &r) {
    expect_held_action_responsive(r, "/api/covers/download", {{"url", action_url}, {"app_uuid", action_uuid}},
      entered, release, calls, 200);
  });
}

TEST(CoverLookupRoutes, KeyCheckHeldProviderLeavesTheOneThreadConsoleResponsive) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<int> calls {0};
  with_action_routes({{}, [&](const std::string &key, const std::string &name, std::string &) -> std::optional<long> {
    EXPECT_EQ(key, "mock-only-key");
    EXPECT_EQ(name, "Portal");
    if (++calls == 1) entered.set_value();
    released.wait();
    return std::nullopt;
  }, {}}, [&](routes_t &r) {
    expect_held_action_responsive(r, "/api/covers/key/check", {{"steamgriddb_api_key", "mock-only-key"}},
      entered, release, calls, 200);
  });
}

namespace {
  const std::vector<unsigned char> downloaded_png {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 1};
  void write_image(const std::filesystem::path &path) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(downloaded_png.data()), downloaded_png.size());
    if (!file) throw std::runtime_error("Fixture image write failed");
  }
  std::string read_bytes(const std::filesystem::path &path) { return file_handler::read_file(path.string().c_str()); }
  std::size_t staging_count(const routes_t &r) {
    std::size_t count = 0;
    const auto covers = r.directory / "covers";
    if (std::filesystem::exists(covers)) for (const auto &file : std::filesystem::directory_iterator(covers))
      if (file.path().filename().string().starts_with(".download-")) ++count;
    return count;
  }
  response_t request_download(routes_t &r) {
    return r.request("POST", "/api/covers/download", nlohmann::json {{"url", action_url}, {"app_uuid", action_uuid}}.dump());
  }
  std::filesystem::path final_cover(const routes_t &r) { return r.directory / "covers" / (std::string(action_uuid) + ".png"); }
  void with_download(const std::function<void(routes_t &)> &run) {
    with_action_routes({{}, {}, [](const std::string &, const std::string &path) {
      write_image(path); return true;
    }}, run);
  }
}

TEST(CoverLookupRoutes, CompletionRevalidatesCookieBeforeAnyCoverPublication) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  with_action_routes({[&](const auto &request, auto) -> std::optional<reply_t> {
    entered.set_value(); released.wait(); return reply_t {200, downloaded_png, request.url};
  }, {}, {}}, [&](routes_t &r) {
    const auto preview = publish_action_choice(); ASSERT_TRUE(preview);
    auto pending = std::async(std::launch::async, [&] { return r.request("POST", "/api/covers/select", nlohmann::json {{"uuid", action_uuid}, {"token", preview->token}}.dump()); });
    auto cleanup = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    r.main([&] { EXPECT_TRUE(confighttp::revoke_cover_cookie_for_tests(r.cookie)); });
    release.set_value(); cleanup.disable();
    EXPECT_EQ(pending.get().code, 401);
    EXPECT_FALSE(std::filesystem::exists(final_cover(r)));
  });
}

TEST(CoverLookupRoutes, CompletionRevalidatesCsrfAfterProviderReturns) {
  std::promise<void> entered, release; auto released = release.get_future().share();
  with_action_routes({{}, [&](const auto &, const auto &, std::string &body) -> std::optional<long> {
    entered.set_value(); released.wait(); body = R"({"data":[]})"; return 200;
  }, {}}, [&](routes_t &r) {
    auto pending = std::async(std::launch::async, [&] { return r.request("POST", "/api/covers/key/check", R"({"steamgriddb_api_key":"mock-only-key"})"); });
    auto cleanup = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    r.main([] { confighttp::set_cover_csrf_for_tests("rotated-test-csrf"); });
    release.set_value(); cleanup.disable();
    EXPECT_EQ(pending.get().code, 403);
  });
}

TEST(CoverLookupRoutes, CompletionRevalidatesBearerAfterAdmissionBypass) {
  std::promise<void> entered, release; auto released = release.get_future().share();
  with_action_routes({{}, [&](const auto &, const auto &, std::string &body) -> std::optional<long> {
    entered.set_value(); released.wait(); body = R"({"data":[]})"; return 200;
  }, {}}, [&](routes_t &r) {
    r.main([] { config::sunshine.api_key = "mock-bearer-before"; });
    auto pending = std::async(std::launch::async, [&] { return r.request("POST", "/api/covers/key/check", R"({"steamgriddb_api_key":"mock-only-key"})", false, false, 5, "mock-bearer-before"); });
    auto cleanup = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    r.main([] { config::sunshine.api_key = "mock-bearer-after"; });
    release.set_value(); cleanup.disable();
    // The revoked bearer no longer earns the original CSRF bypass.
    EXPECT_EQ(pending.get().code, 403);
  });
}

TEST(CoverLookupRoutes, NewActionsAndExistingLookupsShareCapacityThroughMainCommit) {
  std::promise<void> queued, search_entered, release_search;
  auto search_release = release_search.get_future().share();
  std::mutex queue_mutex;
  std::function<void()> completion;
  std::atomic<int> key_calls {0};
  with_action_routes({[&](const auto &request, auto) -> std::optional<reply_t> {
    if (request.operation == game_artwork::providers::operation_e::download) return reply_t {200, downloaded_png, request.url};
    search_entered.set_value(); search_release.wait(); return std::nullopt;
  }, [&](const auto &, const auto &, auto &) -> std::optional<long> { ++key_calls; return std::nullopt; }, {}}, [&](routes_t &r) {
    r.main([&] { confighttp::set_cover_post_for_tests([&](std::function<void()> callback) {
      std::lock_guard lock(queue_mutex); completion = std::move(callback); queued.set_value();
    }); });
    const auto preview = publish_action_choice(); ASSERT_TRUE(preview);
    auto select = std::async(std::launch::async, [&] { return r.request("POST", "/api/covers/select", nlohmann::json {{"uuid", action_uuid}, {"token", preview->token}}.dump()); });
    auto cancel = util::fail_guard([&] { r.workers.stop_accepting(); });
    ASSERT_EQ(queued.get_future().wait_for(2s), std::future_status::ready);
    auto search = std::async(std::launch::async, [&] { return r.lookup(false); });
    auto release = util::fail_guard([&] { release_search.set_value(); });
    ASSERT_EQ(search_entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_EQ(r.request("GET", "/api/covers/sweep").code, 200);
    const auto refused = r.request("POST", "/api/covers/key/check", R"({"steamgriddb_api_key":"mock-only-key"})");
    EXPECT_EQ(refused.code, 503); EXPECT_EQ(refused.body.value("code", ""), "cover_lookup_busy"); EXPECT_EQ(key_calls, 0);
    r.main([&] { std::function<void()> callback; { std::lock_guard lock(queue_mutex); callback = std::move(completion); } callback(); });
    EXPECT_EQ(select.get().code, 200);
    release_search.set_value(); release.disable(); EXPECT_EQ(search.get().code, 502);
    cancel.disable();
  });
}

TEST(CoverLookupRoutes, StoppedExecutorCancelsQueuedDownloadAndCleansOwnedArtifacts) {
  std::promise<void> queued;
  std::mutex queue_mutex; std::function<void()> completion;
  with_download([&](routes_t &r) {
    const auto original = read_bytes(config::stream.file_apps);
    r.main([&] { confighttp::set_cover_post_for_tests([&](std::function<void()> callback) {
      std::lock_guard lock(queue_mutex); completion = std::move(callback); queued.set_value();
    }); });
    auto pending = std::async(std::launch::async, [&] { try { request_download(r); } catch (...) { } });
    auto cancel = util::fail_guard([&] { r.stop_server(); });
    ASSERT_EQ(queued.get_future().wait_for(2s), std::future_status::ready);
    ASSERT_EQ(staging_count(r), 1);
    r.stop_server(); cancel.disable();
    auto drain = std::async(std::launch::async, [&] { r.workers.shutdown(); });
    ASSERT_EQ(drain.wait_for(2s), std::future_status::ready); drain.get(); pending.get();
    EXPECT_EQ(staging_count(r), 0); EXPECT_FALSE(std::filesystem::exists(final_cover(r)));
    EXPECT_EQ(read_bytes(config::stream.file_apps), original);
    // A saved weak callback cannot publish after stop, even if invoked later.
    std::function<void()> callback; { std::lock_guard lock(queue_mutex); callback = std::move(completion); }
    callback(); EXPECT_EQ(read_bytes(config::stream.file_apps), original);
  });
}

TEST(CoverLookupRoutes, KeyAndHeadersAreSnapshottedBeforeWorkerReads) {
  std::promise<void> entered, release; auto released = release.get_future().share();
  std::thread::id provider_thread, http_thread;
  with_action_routes({{}, [&](const std::string &key, const auto &, std::string &body) -> std::optional<long> {
    provider_thread = std::this_thread::get_id(); EXPECT_EQ(key, "mock-only-key");
    entered.set_value(); released.wait(); body = R"({"data":[]})"; return 200;
  }, {}}, [&](routes_t &r) {
    r.main([&] { http_thread = std::this_thread::get_id(); config::sunshine.port = 31111;
      ASSERT_TRUE(private_state_file::write_atomic(config::sunshine.config_file, "steamgriddb_api_key = mock-only-key\n")); });
    auto pending = std::async(std::launch::async, [&] { return r.request("POST", "/api/covers/key/check", R"({"use_stored":true})"); });
    auto cleanup = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    r.main([] { config::set_steamgriddb_api_key("mock-key-after"); config::sunshine.port = 32222; });
    release.set_value(); cleanup.disable();
    const auto result = pending.get(); EXPECT_EQ(result.code, 200); EXPECT_TRUE(result.body.value("status", false));
    EXPECT_NE(provider_thread, http_thread);
    EXPECT_NE(result.security_policy.find("https://*:31106"), std::string::npos);
    EXPECT_EQ(result.security_policy.find("https://*:32217"), std::string::npos);
  });
}

TEST(CoverLookupRoutes, ApplyMissingFreshManualArtworkWinsAfterProviderReturns) {
  std::promise<void> entered, release; auto released = release.get_future().share();
  with_action_routes({[&](const auto &request, auto) -> std::optional<reply_t> {
    entered.set_value(); released.wait(); return reply_t {200, downloaded_png, request.url};
  }, {}, {}}, [&](routes_t &r) {
    const auto sweep = r.request("POST", "/api/covers/sweep", nlohmann::json {{"uuids", {action_uuid}}}.dump()); ASSERT_EQ(sweep.code, 202);
    ASSERT_TRUE(confighttp::wait_for_cover_sweep_for_tests(5s));
    const auto preview = publish_action_choice(); ASSERT_TRUE(preview);
    auto pending = std::async(std::launch::async, [&] { return r.request("POST", "/api/covers/apply-missing", nlohmann::json {
      {"uuid", action_uuid}, {"token", preview->token}, {"run_id", sweep.body.at("sweep").at("id")}, {"expected_name", "Portal"}}.dump()); });
    auto cleanup = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    r.main([] { auto tree = nlohmann::json::parse(read_bytes(config::stream.file_apps)); tree["apps"][0]["image-path"] = "manual-player-choice.png";
      ASSERT_TRUE(private_state_file::write_atomic(config::stream.file_apps, tree.dump())); });
    release.set_value(); cleanup.disable();
    const auto result = pending.get(); EXPECT_TRUE(result.body.value("skipped", false));
    EXPECT_EQ(nlohmann::json::parse(read_bytes(config::stream.file_apps))["apps"][0]["image-path"], "manual-player-choice.png");
    EXPECT_FALSE(std::filesystem::exists(r.directory / "covers" / "automatic"));
  });
}

TEST(CoverLookupRoutes, DownloadChangedEntryKeepsManualArtworkAndCleansStage) {
  std::promise<void> entered, release; auto released = release.get_future().share();
  with_action_routes({{}, {}, [&](const auto &, const std::string &path) {
    write_image(path); entered.set_value(); released.wait(); return true;
  }}, [&](routes_t &r) {
    auto pending = std::async(std::launch::async, [&] { return request_download(r); });
    auto cleanup = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    r.main([] { auto tree = nlohmann::json::parse(read_bytes(config::stream.file_apps)); tree["apps"][0]["image-path"] = "manual-player-choice.png";
      ASSERT_TRUE(private_state_file::write_atomic(config::stream.file_apps, tree.dump())); });
    release.set_value(); cleanup.disable(); EXPECT_FALSE(pending.get().body.value("status", true));
    EXPECT_FALSE(std::filesystem::exists(final_cover(r))); EXPECT_EQ(staging_count(r), 0);
    EXPECT_EQ(nlohmann::json::parse(read_bytes(config::stream.file_apps))["apps"][0]["image-path"], "manual-player-choice.png");
  });
}

TEST(CoverLookupRoutes, DownloadMergesFreshUnrelatedChangesOnMainExecutor) {
  std::promise<void> entered, release; auto released = release.get_future().share();
  std::thread::id provider_thread, publication_thread, http_thread;
  with_action_routes({{}, {}, [&](const auto &, const std::string &path) {
    provider_thread = std::this_thread::get_id(); write_image(path); entered.set_value(); released.wait(); return true;
  }}, [&](routes_t &r) {
    r.main([&] { http_thread = std::this_thread::get_id();
      confighttp::set_cover_commit_observer_for_tests([&] { publication_thread = std::this_thread::get_id(); }); });
    auto pending = std::async(std::launch::async, [&] { return request_download(r); });
    auto cleanup = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    r.main([&] { auto tree = nlohmann::json::parse(read_bytes(config::stream.file_apps));
      tree["fixture-unrelated"] = "fresh-value"; ASSERT_TRUE(private_state_file::write_atomic(config::stream.file_apps, tree.dump())); });
    release.set_value(); cleanup.disable(); const auto result = pending.get();
    EXPECT_TRUE(result.body.value("status", false)); EXPECT_NE(provider_thread, publication_thread); EXPECT_EQ(publication_thread, http_thread);
    const auto tree = nlohmann::json::parse(read_bytes(config::stream.file_apps));
    EXPECT_EQ(tree["fixture-unrelated"], "fresh-value"); EXPECT_EQ(tree["apps"][0]["image-path"], final_cover(r).string());
    EXPECT_EQ(read_bytes(final_cover(r)), std::string(downloaded_png.begin(), downloaded_png.end())); EXPECT_EQ(staging_count(r), 0);
  });
}

namespace {
  void expect_download_fault(const std::string &fault, bool committed, bool retained) {
    with_download([&](routes_t &r) {
      std::filesystem::create_directories(r.directory / "covers");
      { std::ofstream old(final_cover(r)); old << "old-owned-cover"; }
      const auto library = read_bytes(config::stream.file_apps);
      r.main([&] { confighttp::set_cover_publication_fault_for_tests(fault); });
      const auto result = request_download(r);
      EXPECT_EQ(result.code, 200); EXPECT_FALSE(result.body.value("status", true));
      r.main([] { confighttp::set_cover_publication_fault_for_tests({}); });
      EXPECT_EQ(staging_count(r), retained ? 1 : 0);
      if (committed) {
        EXPECT_EQ(read_bytes(final_cover(r)), std::string(downloaded_png.begin(), downloaded_png.end()));
        EXPECT_EQ(nlohmann::json::parse(read_bytes(config::stream.file_apps))["apps"][0]["image-path"], final_cover(r).string());
      } else {
        EXPECT_EQ(read_bytes(config::stream.file_apps), library);
        if (!retained) EXPECT_EQ(read_bytes(final_cover(r)), "old-owned-cover");
        else {
          bool backup = false;
          for (const auto &entry : std::filesystem::directory_iterator(r.directory / "covers")) if (entry.path().filename().string().starts_with(".download-")) {
            EXPECT_EQ(read_bytes(entry.path() / "previous-cover"), "old-owned-cover"); backup = true;
          }
          EXPECT_TRUE(backup);
        }
      }
    });
  }
}
TEST(CoverLookupRoutes, DownloadBackupFailureKeepsBytesAndCleansStaging) { expect_download_fault("backup", false, false); }
TEST(CoverLookupRoutes, DownloadPublishFailureKeepsBytesAndCleansStaging) { expect_download_fault("publish", false, false); }
TEST(CoverLookupRoutes, DownloadSaveFailureRestoresCoverAndLibrary) { expect_download_fault("save", false, false); }
TEST(CoverLookupRoutes, DownloadDurabilityUncertainKeepsReferencedNewCover) { expect_download_fault("durability", true, false); }
TEST(CoverLookupRoutes, DownloadRestoreFailureRetainsOwnedRecoveryBackup) { expect_download_fault("restore", false, true); }
TEST(CoverLookupRoutes, DownloadCleanupFailureReportsCommittedRecovery) { expect_download_fault("cleanup", true, true); }

TEST(CoverLookupRoutes, SelectUnsavedUUIDRetainsScopedPreviewContract) {
  constexpr auto unsaved = "44444444-4444-4444-8444-444444444444";
  with_action_routes({[](const auto &request, auto) -> std::optional<reply_t> { return reply_t {200, downloaded_png, request.url}; }, {}, {}}, [&](routes_t &r) {
    const auto preview = nvhttp::artwork_candidate_previews().publish(unsaved, game_artwork::kind_e::poster,
      {0xff, 0xd8, 0xff, 0xe0, 1}, nvhttp::artwork_clock_milliseconds(), game_artwork::manual::choice_source_t {"620", action_url});
    ASSERT_TRUE(preview);
    const auto result = r.request("POST", "/api/covers/select", nlohmann::json {{"uuid", unsaved}, {"token", preview->token}}.dump());
    EXPECT_EQ(result.code, 200); EXPECT_TRUE(result.body.value("status", false));
    EXPECT_TRUE(std::filesystem::exists(result.body.at("path").get<std::string>()));
    nvhttp::artwork_candidate_previews().clear_game(unsaved);
  });
}

TEST(CoverLookupRoutes, DownloadPathChangeRejectsCommitIntoEitherLibrary) {
  std::promise<void> entered, release; auto released = release.get_future().share();
  with_action_routes({{}, {}, [&](const auto &, const std::string &path) {
    write_image(path); entered.set_value(); released.wait(); return true;
  }}, [&](routes_t &r) {
    const auto original_path = config::stream.file_apps;
    const auto original_bytes = read_bytes(original_path);
    const auto successor = r.directory / "other-library.json";
    ASSERT_TRUE(private_state_file::write_atomic(successor, original_bytes));
    auto pending = std::async(std::launch::async, [&] { return request_download(r); });
    auto cleanup = util::fail_guard([&] { release.set_value(); });
    ASSERT_EQ(entered.get_future().wait_for(2s), std::future_status::ready);
    r.main([&] { config::stream.file_apps = successor.string(); });
    release.set_value(); cleanup.disable(); EXPECT_FALSE(pending.get().body.value("status", true));
    EXPECT_EQ(read_bytes(original_path), original_bytes); EXPECT_EQ(read_bytes(successor), original_bytes);
    EXPECT_FALSE(std::filesystem::exists(final_cover(r))); EXPECT_EQ(staging_count(r), 0);
  });
}

TEST(CoverLookupRoutes, ProviderExceptionsKeepActionFailuresAndCleanStaging) {
  with_action_routes({[](const auto &, auto) -> std::optional<reply_t> { throw std::runtime_error("mock image failure"); },
    [](const auto &, const auto &, auto &) -> std::optional<long> { throw std::runtime_error("mock key failure"); },
    [](const auto &, const auto &) -> bool { throw std::runtime_error("mock download failure"); }}, [&](routes_t &r) {
    const auto preview = publish_action_choice(); ASSERT_TRUE(preview);
    EXPECT_EQ(r.request("POST", "/api/covers/select", nlohmann::json {{"uuid", action_uuid}, {"token", preview->token}}.dump()).code, 502);
    EXPECT_FALSE(r.request("POST", "/api/covers/key/check", R"({"steamgriddb_api_key":"mock-only-key"})").body.value("status", true));
    EXPECT_FALSE(request_download(r).body.value("status", true)); EXPECT_EQ(staging_count(r), 0);
  });
}
