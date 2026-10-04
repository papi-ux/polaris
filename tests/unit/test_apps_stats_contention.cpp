/** Real authenticated console requests must not queue behind app teardown. */
#include "../tests_common.h"

#include <Simple-Web-Server/client_https.hpp>
#include <src/config.h>
#include <src/confighttp.h>
#include <src/crypto.h>
#include <src/process.h>
#include <src/stream_stats.h>
#include <src/utility.h>

#include <atomic>
#include <fstream>
#include <future>

using namespace std::chrono_literals;

namespace {
  struct apps_stats_contention: testing::Test {
    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      directory = std::filesystem::temp_directory_path() / ("polaris-apps-http-" + std::to_string(nonce));
      std::filesystem::create_directory(directory);
      old_username = std::exchange(config::sunshine.username, "http-fixture-user");
      old_key = std::exchange(config::sunshine.api_key, "http-fixture-key");
      old_apps_file = std::exchange(config::stream.file_apps, (directory / "apps.json").string());
      std::ofstream(config::stream.file_apps) << R"({"apps":[{"uuid":"app-a","name":"Fixture A"},{"uuid":"app-b","name":"Fixture B"}]})";
      const auto credentials = crypto::gen_creds("localhost", 2048);
      const auto cert = directory / "cert.pem", key = directory / "key.pem";
      std::ofstream(cert) << credentials.x509;
      std::ofstream(key) << credentials.pkey;
      std::filesystem::permissions(key, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
      server = std::async(std::launch::async, [&, cert, key]() {
        try {
          confighttp::run_apps_stats_server_for_tests(cert.string(), key.string(),
            [&](unsigned short port, std::function<void()> stop) {
              stop_server = std::move(stop);
              listening.set_value(port);
            }, [&]() {
              if (!entered.exchange(true)) apps_entered.set_value();
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
      proc::proc.clear_running_app_for_tests();
      config::sunshine.username = old_username;
      config::sunshine.api_key = old_key;
      config::stream.file_apps = old_apps_file;
      std::filesystem::remove_all(directory);
    }

    auto request(const std::string &path, const std::string &token = "http-fixture-key") {
      return std::async(std::launch::async, [=, this]() {
        SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(port), false};
        client.config.timeout = 2;
        client.config.timeout_connect = 2;
        SimpleWeb::CaseInsensitiveMultimap headers {{"Authorization", "Bearer " + token}};
        auto response = client.request("GET", path, "", headers);
        return std::pair {response->status_code, response->content.string()};
      });
    }

    void set_app(std::string uuid) {
      proc::ctx_t app {};
      app.id = "167";
      app.uuid = std::move(uuid);
      proc::proc.set_running_app_for_tests(app);
    }

    std::filesystem::path directory;
    std::string old_username, old_key, old_apps_file;
    std::promise<unsigned short> listening;
    std::promise<void> apps_entered;
    std::atomic<bool> entered {false};
    std::function<void()> stop_server;
    std::future<void> server;
    unsigned short port = 0;
  };
}

TEST_F(apps_stats_contention, AppsAndStatsReturnWhileTeardownHoldsTheLifecycleLock) {
  set_app("app-a");
  stream_stats::add_client("127.0.0.167", "Ended HTTP fixture", 920167);
  stream_stats::remove_client("127.0.0.167", 920167);
  const auto history = nlohmann::json::parse(stream_stats::get_current().to_json()).at("last_session");
  std::promise<void> locked, release;
  auto released = release.get_future().share();
  auto teardown = std::async(std::launch::async, [&]() {
    proc::proc.with_session_lifecycle_lock_for_tests([&]() { locked.set_value(); released.wait(); });
  });
  auto unlock = util::fail_guard([&]() { release.set_value(); teardown.get(); });
  locked.get_future().wait();
  auto apps = request("/api/apps");
  ASSERT_EQ(apps_entered.get_future().wait_for(2s), std::future_status::ready);
  auto stats = request("/api/stats/stream");
  const auto apps_ready = apps.wait_for(500ms), stats_ready = stats.wait_for(500ms);
  // Unblock the original implementation before reporting its failure.
  release.set_value(); teardown.get(); unlock.disable();
  const auto [apps_status, apps_body] = apps.get();
  const auto [stats_status, stats_body] = stats.get();
  EXPECT_EQ(apps_ready, std::future_status::ready);
  EXPECT_EQ(stats_ready, std::future_status::ready);
  ASSERT_TRUE(apps_status.starts_with("200"));
  ASSERT_TRUE(stats_status.starts_with("200"));
  const auto apps_json = nlohmann::json::parse(apps_body), stats_json = nlohmann::json::parse(stats_body);
  ASSERT_TRUE(apps_json.contains("current_app_available"));
  EXPECT_FALSE(apps_json.at("current_app_available").get<bool>());
  EXPECT_EQ(apps_json.at("current_app"), "");
  EXPECT_EQ(apps_json.at("apps").size(), 2);
  EXPECT_EQ(stats_json.at("auto_quality").at("blocked_reason"), "session_changing");
  EXPECT_EQ(stats_json.at("last_session"), history);
  EXPECT_TRUE(stats_json.contains("doctor"));

  auto recovered = nlohmann::json::parse(request("/api/apps").get().second);
  EXPECT_TRUE(recovered.at("current_app_available").get<bool>());
  EXPECT_EQ(recovered.at("current_app"), "app-a");
  set_app("app-b");
  recovered = nlohmann::json::parse(request("/api/apps").get().second);
  EXPECT_EQ(recovered.at("current_app"), "app-b");
  proc::proc.clear_running_app_for_tests();
  recovered = nlohmann::json::parse(request("/api/apps").get().second);
  EXPECT_TRUE(recovered.at("current_app_available").get<bool>());
  EXPECT_EQ(recovered.at("current_app"), "");
}

TEST_F(apps_stats_contention, BothReadOnlyHandlersStillRequireAuthentication) {
  proc::proc.with_session_lifecycle_lock_for_tests([&]() {
    for (const auto *path : {"/api/apps", "/api/stats/stream"}) {
      auto reply = request(path, "wrong-fixture-key");
      ASSERT_EQ(reply.wait_for(500ms), std::future_status::ready);
      const auto [status, body] = reply.get();
      EXPECT_TRUE(status.starts_with("401"));
      EXPECT_EQ(body.find("Fixture A"), std::string::npos);
    }
  });
}
