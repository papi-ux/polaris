/**
 * @file tests/unit/test_config_refusal_http.cpp
 * @brief Private-state parent refusal at the real login/config HTTP routes (#550).
 */
#include "../tests_common.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <sys/stat.h>

#include <nlohmann/json.hpp>
#include <Simple-Web-Server/client_https.hpp>
#include <Simple-Web-Server/server_https.hpp>

#include "src/config.h"
#include "src/crypto.h"
#include "src/httpcommon.h"
#include "src/private_state_file.h"
#include "src/utility.h"

namespace config {
  void path_f(std::unordered_map<std::string, std::string> &, const std::string &, std::filesystem::path &);
}
namespace confighttp {
  using server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  void login(std::shared_ptr<server_t::Response>, std::shared_ptr<server_t::Request>);
  void logout(std::shared_ptr<server_t::Response>, std::shared_ptr<server_t::Request>);
  void getConfig(std::shared_ptr<server_t::Response>, std::shared_ptr<server_t::Request>);
  void with_web_session_store_for_tests(const std::filesystem::path &, const std::function<void()> &);
}

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;

  class ConfigParentRefusalHttp: public testing::Test {
  protected:
    void SetUp() override {
      previous = config::sunshine;
      directory = fs::temp_directory_path() /
        ("polaris-config-refusal-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      ASSERT_TRUE(fs::create_directory(directory));
      fs::permissions(directory, fs::perms::owner_all);
      parent = directory / ".config";
      state = parent / "polaris";
      const auto credentials = (directory / "credentials.json").string();
      ASSERT_EQ(http::save_user_creds(credentials, "refusal-test", "fixture-password"), 0);
      ASSERT_EQ(http::reload_user_creds(credentials), 0);
      config::sunshine.credentials_file = credentials;
      config::sunshine.config_file = (state / "polaris.conf").string();
    }

    void TearDown() override {
      config::sunshine = previous;
      std::error_code ignored;
      fs::remove_all(directory, ignored);
    }

    void create_state_through_config() {
      // Exercise the production path used for state-file directories, rather
      // than creating a private test directory independently of startup.
      std::unordered_map<std::string, std::string> vars;
      fs::path path = state / "polaris.conf";
      config::path_f(vars, "fixture_path", path);
      ASSERT_TRUE(private_state_file::write_atomic(path, "# fixture\n"));
    }

    unsigned mode(const fs::path &path) {
      struct stat metadata {};
      EXPECT_EQ(::stat(path.c_str(), &metadata), 0);
      return metadata.st_mode & 0777;
    }

    void with_server(const std::function<void(unsigned short)> &run) {
      const auto [certificate, key] = crypto::gen_creds("localhost", 2048);
      ASSERT_TRUE(private_state_file::write_atomic(directory / "cert.pem", certificate));
      ASSERT_TRUE(private_state_file::write_atomic(directory / "key.pem", key));
      confighttp::with_web_session_store_for_tests(state / "web_sessions.json", [&] {
        confighttp::server_t server((directory / "cert.pem").string(), (directory / "key.pem").string());
        server.config.address = "127.0.0.1";
        server.config.port = 0;
        server.config.thread_pool_size = 1;
        server.config.timeout_request = 5;
        server.config.timeout_content = 5;
        server.resource["^/api/login$"]["POST"] = confighttp::login;
        server.resource["^/api/logout$"]["POST"] = confighttp::logout;
        server.resource["^/api/config$"]["GET"] = confighttp::getConfig;
        std::atomic<unsigned short> port {0};
        std::jthread http([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
        auto stop = util::fail_guard([&] { server.stop(); if (http.joinable()) http.join(); });
        for (int i = 0; i < 200 && port == 0; ++i) std::this_thread::sleep_for(10ms);
        ASSERT_NE(port, 0);
        run(port.load());
      });
    }

    fs::path directory, parent, state;
    decltype(config::sunshine) previous;
  };

  auto login(unsigned short port, const std::string &password = "fixture-password") {
    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port), false);
    client.config.timeout = 5;
    client.config.timeout_connect = 5;
    return client.request("POST", "/api/login",
      nlohmann::json {{"username", "refusal-test"}, {"password", password}}.dump(),
      {{"Content-Type", "application/json"}});
  }
}

TEST_F(ConfigParentRefusalHttp, ExistingGroupWritableParentExplainsLoginRefusalAndReadiness) {
  create_state_through_config();
  fs::permissions(parent, static_cast<fs::perms>(0775));
  with_server([&](unsigned short port) {
    const auto denied = login(port, "wrong-password");
    EXPECT_EQ(denied->status_code.substr(0, 3), "401");
    EXPECT_EQ(denied->content.string().find(parent.string()), std::string::npos);

    const auto response = login(port);
    ASSERT_EQ(response->status_code.substr(0, 3), "503");
    const auto body = nlohmann::json::parse(response->content.string());
    EXPECT_EQ(body.at("status"), false);
    EXPECT_EQ(body.at("code"), "private_state_unavailable");
    EXPECT_NE(body.at("error").get<std::string>().find(parent.string()), std::string::npos);
    EXPECT_NE(body.at("error").get<std::string>().find("chmod 700 " + parent.string()), std::string::npos);
    EXPECT_EQ(response->header.count("Set-Cookie"), 0u);
    const auto cache_control = response->header.find("Cache-Control");
    ASSERT_NE(cache_control, response->header.end());
    EXPECT_EQ(cache_control->second, "no-store");
    EXPECT_EQ(mode(parent), 0775u);
    EXPECT_FALSE(fs::exists(state / "web_sessions.json"));

    // Mission Control reads /api/config. Bearer authentication remains valid
    // independently of cookies, and its existing refusal feeds the app banner.
    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port), false);
    client.config.timeout = 5;
    client.config.timeout_connect = 5;
    for (const auto &[method, path] : {std::pair {"GET", "/api/config"}, std::pair {"POST", "/api/logout"}}) {
      const auto unavailable = client.request(method, path, "", {{"Cookie", "auth=unavailable-fixture-session"}});
      ASSERT_EQ(unavailable->status_code.substr(0, 3), "503");
      const auto detail = nlohmann::json::parse(unavailable->content.string());
      EXPECT_EQ(detail.at("code"), "private_state_unavailable");
      EXPECT_EQ(detail.at("fix"), body.at("fix"));
      EXPECT_EQ(unavailable->header.count("Set-Cookie"), 0u);
    }
    const auto readiness = client.request("GET", "/api/config", "",
      {{"Authorization", "Bearer " + config::sunshine.api_key}});
    ASSERT_EQ(readiness->status_code.substr(0, 3), "503");
    const auto refusal = nlohmann::json::parse(readiness->content.string());
    EXPECT_EQ(refusal.at("error"), "config_unreadable");
    EXPECT_EQ(refusal.at("reason"), body.at("reason"));
    EXPECT_EQ(refusal.at("fix"), body.at("fix"));
  });
}

TEST_F(ConfigParentRefusalHttp, MissingParentIsPrivateUnderUmask002AndLoginWorks) {
  ASSERT_FALSE(fs::exists(parent));
  const auto previous_mask = ::umask(0002);
  auto restore_mask = util::fail_guard([&] { ::umask(previous_mask); });
  create_state_through_config();
  EXPECT_EQ(mode(parent), 0700u);
  EXPECT_EQ(mode(state), 0700u);
  with_server([&](unsigned short port) {
    const auto response = login(port);
    EXPECT_EQ(response->status_code.substr(0, 3), "200");
    EXPECT_EQ(response->header.count("Set-Cookie"), 1u);
    EXPECT_TRUE(private_state_file::read_secure(state / "web_sessions.json", 65536));
  });
}

TEST_F(ConfigParentRefusalHttp, ExistingPrivateParentStillAllowsLogin) {
  ASSERT_TRUE(fs::create_directory(parent));
  fs::permissions(parent, fs::perms::owner_all);
  create_state_through_config();
  with_server([&](unsigned short port) {
    const auto response = login(port);
    EXPECT_EQ(response->status_code.substr(0, 3), "200");
    EXPECT_EQ(response->header.count("Set-Cookie"), 1u);
    EXPECT_EQ(mode(parent), 0700u);
    EXPECT_TRUE(private_state_file::read_secure(state / "web_sessions.json", 65536));
  });
}
