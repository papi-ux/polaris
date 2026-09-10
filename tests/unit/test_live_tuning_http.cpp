#include "../tests_common.h"
#include "src/config.h"
#include "src/adaptive_bitrate.h"
#include "src/crypto.h"
#include "src/private_state_file.h"
#include <Simple-Web-Server/server_https.hpp>
#include <Simple-Web-Server/client_https.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <thread>

namespace confighttp {
  void live_tuning_http_for_tests(std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Response>,
                                 std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Request>);
  void with_web_session_for_tests(const std::filesystem::path &, const std::string &,
                                 const std::function<void(const std::string &)> &);
}

TEST(LiveTuningHttp, RealTlsHandlerRequiresAuthenticationCsrfAndCurrentRevision) {
  const auto old_config = config::sunshine;
  const auto old_adaptive = config::video.adaptive_bitrate;
  const auto directory = std::filesystem::temp_directory_path() /
    ("polaris-tuning-http-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  auto restore = util::fail_guard([&] {
    config::sunshine = old_config;
    config::video.adaptive_bitrate = old_adaptive;
    adaptive_bitrate::load_config();
    adaptive_bitrate::reset();
    std::filesystem::remove_all(directory);
  });
  config::sunshine.username = "test-admin";
  config::sunshine.api_key = "isolated-test-api-key";
  config::sunshine.config_file = (directory / "polaris.conf").string();
  ASSERT_TRUE(private_state_file::write_atomic(config::sunshine.config_file, "adaptive_bitrate_enabled = enabled\n"));
  config::video.adaptive_bitrate.enabled = true;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  const auto credentials = crypto::gen_creds("localhost", 2048);
  ASSERT_TRUE(private_state_file::write_atomic(directory / "cert.pem", credentials.x509));
  ASSERT_TRUE(private_state_file::write_atomic(directory / "key.pem", credentials.pkey));
  confighttp::with_web_session_for_tests(directory / "sessions.json", "test-csrf", [&](const std::string &cookie) {
    SimpleWeb::Server<SimpleWeb::HTTPS> server((directory / "cert.pem").string(), (directory / "key.pem").string());
    server.config.address = "127.0.0.1";
    server.config.port = 0;
    server.config.timeout_request = 5;
    server.config.timeout_content = 5;
    server.resource["^/api/live-tuning$"]["GET"] = confighttp::live_tuning_http_for_tests;
    server.resource["^/api/live-tuning$"]["POST"] = confighttp::live_tuning_http_for_tests;
    std::atomic<unsigned short> port {0};
    std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
    auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
    for (int attempt = 0; attempt < 100 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
    ASSERT_NE(port, 0);
    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port.load()), false);
    client.config.timeout = 5;
    const std::string body = R"({"enabled":false})";
    auto request = [&](std::string method, std::string content, SimpleWeb::CaseInsensitiveMultimap headers) {
      headers.emplace("Content-Type", "application/json");
      return client.request(method, "/api/live-tuning", content, headers);
    };
    auto code = [](const auto &response) { return std::stoi(response->status_code); };
    EXPECT_EQ(code(request("GET", "", {})), 401);
    EXPECT_EQ(code(request("POST", body, {})), 403);
    EXPECT_EQ(code(request("POST", body, {{"X-CSRF-Token", "test-csrf"}})), 401);
    auto authenticated = request("GET", "", {{"Cookie", "auth=" + cookie}});
    ASSERT_EQ(code(authenticated), 200);
    const auto revision = nlohmann::json::parse(authenticated->content.string())["live_tuning"]["configuration_revision"].get<std::string>();
    const std::string match = "\"" + revision + "\"";
    // An invalid bearer prefix must not let a valid cookie bypass CSRF.
    EXPECT_EQ(code(request("POST", body, {{"Cookie", "auth=" + cookie}, {"Authorization", "Bearer wrong"}, {"If-Match", match}})), 403);
    EXPECT_EQ(code(request("POST", body, {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", "test-csrf"}})), 412);
    EXPECT_EQ(code(request("POST", R"({"enabled":"false"})", {{"Authorization", "Bearer isolated-test-api-key"}, {"If-Match", match}})), 400);
    EXPECT_EQ(code(request("POST", body, {{"Cookie", "auth=" + cookie}, {"X-CSRF-Token", "test-csrf"}, {"If-Match", match}})), 200);
    EXPECT_FALSE(adaptive_bitrate::get_state().configured_enabled);
    EXPECT_EQ(code(request("POST", R"({"enabled":true})", {{"Authorization", "Bearer isolated-test-api-key"}, {"If-Match", match}})), 412);
    EXPECT_FALSE(adaptive_bitrate::get_state().configured_enabled);
    auto current = request("GET", "", {{"Authorization", "Bearer isolated-test-api-key"}});
    const auto fresh = nlohmann::json::parse(current->content.string())["live_tuning"]["configuration_revision"].get<std::string>();
    EXPECT_EQ(code(request("POST", R"({"enabled":true})", {{"Authorization", "Bearer isolated-test-api-key"}, {"If-Match", "\"" + fresh + "\""}})), 200);
    EXPECT_TRUE(adaptive_bitrate::get_state().configured_enabled);
  });
}
