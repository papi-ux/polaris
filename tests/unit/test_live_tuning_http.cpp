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
#include <sys/stat.h>

namespace confighttp {
  void live_tuning_http_for_tests(std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Response>,
                                 std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Request>);
  void getConfig(std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Response>,
                 std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Request>);
  void saveConfig(std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Response>,
                  std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Request>);
  void patchConfig(std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Response>,
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

// #782: a settings file the store refused answered a bare 503, which the console
// read as a host that was down. The body now says which file, why and the fix,
// and never what the file holds.
TEST(ConfigHttp, RefusedSettingsReadAnswersWithTheFileTheReasonAndTheFix) {
  const auto old_config = config::sunshine;
  const auto old_backend = config::video.linux_display.virtual_display_backend;
  const auto directory = std::filesystem::temp_directory_path() /
    ("polaris-config-http-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  auto restore = util::fail_guard([&] {
    config::sunshine = old_config;
    config::video.linux_display.virtual_display_backend = old_backend;
    std::filesystem::remove_all(directory);
  });
  config::sunshine.username = "test-admin";
  config::sunshine.api_key = "isolated-test-api-key";
  // getConfig probes for a virtual display first. Keep that probe to a PATH
  // lookup; auto would try to load the EVDI module.
  config::video.linux_display.virtual_display_backend = "kscreen";
  const auto config_file = (directory / "polaris.conf").string();
  config::sunshine.config_file = config_file;
  ASSERT_TRUE(private_state_file::write_atomic(config_file, "sunshine_name = only-the-file-knows\n"));
  ASSERT_EQ(::chmod(config_file.c_str(), 0664), 0);
  const auto credentials = crypto::gen_creds("localhost", 2048);
  ASSERT_TRUE(private_state_file::write_atomic(directory / "cert.pem", credentials.x509));
  ASSERT_TRUE(private_state_file::write_atomic(directory / "key.pem", credentials.pkey));
  confighttp::with_web_session_for_tests(directory / "sessions.json", "test-csrf", [&](const std::string &cookie) {
    SimpleWeb::Server<SimpleWeb::HTTPS> server((directory / "cert.pem").string(), (directory / "key.pem").string());
    server.config.address = "127.0.0.1";
    server.config.port = 0;
    server.config.timeout_request = 5;
    server.config.timeout_content = 5;
    server.resource["^/api/config$"]["GET"] = confighttp::getConfig;
    std::atomic<unsigned short> port {0};
    std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
    auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
    for (int attempt = 0; attempt < 100 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
    ASSERT_NE(port, 0);
    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port.load()), false);
    client.config.timeout = 5;
    auto get = [&](SimpleWeb::CaseInsensitiveMultimap headers) {
      return client.request("GET", "/api/config", "", headers);
    };
    auto code = [](const auto &response) { return std::stoi(response->status_code); };

    // Authentication still comes first, and says nothing about the file.
    const auto anonymous = get({});
    EXPECT_EQ(code(anonymous), 401);
    EXPECT_EQ(anonymous->content.string().find(config_file), std::string::npos);

    const auto refused = get({{"Cookie", "auth=" + cookie}});
    ASSERT_EQ(code(refused), 503);
    const auto content_type = refused->header.find("Content-Type");
    ASSERT_NE(content_type, refused->header.end());
    EXPECT_EQ(content_type->second, "application/json");
    const auto text = refused->content.string();
    EXPECT_EQ(text.find("only-the-file-knows"), std::string::npos) << text;
    const auto body = nlohmann::json::parse(text);
    EXPECT_EQ(body["status"], false);
    EXPECT_EQ(body["error"], "config_unreadable");
    EXPECT_EQ(body["path"], config_file);
    EXPECT_EQ(body["reason"],
              "It is writable by its group (mode 0664), and the settings store refuses a file another user can change.");
    EXPECT_EQ(body["fix"], "Restrict it with \"chmod go-w " + config_file + "\".");

    ASSERT_EQ(::chmod(config_file.c_str(), 0644), 0);
    const auto restored = get({{"Cookie", "auth=" + cookie}});
    ASSERT_EQ(code(restored), 200);
    const auto settings = nlohmann::json::parse(restored->content.string());
    EXPECT_EQ(settings["status"], true);
    EXPECT_EQ(settings["sunshine_name"], "only-the-file-knows");
  });
}

// #782: saving a settings file the store refused answered 412 "Settings changed.
// Refresh before saving." when the save carried If-Match, because a refused file
// has an empty revision, and 400 "Failed to write config file" when it did not.
// Live Tuning answered 500 save_failed. Every save now answers the way the read
// does: 503 with the file, the reason and the fix, and the file is left alone.
TEST(ConfigHttp, RefusedSettingsSaveAnswersTheWayTheReadDoes) {
  const auto old_config = config::sunshine;
  const auto old_backend = config::video.linux_display.virtual_display_backend;
  const auto old_adaptive = config::video.adaptive_bitrate;
  const auto directory = std::filesystem::temp_directory_path() /
    ("polaris-config-save-http-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  auto restore = util::fail_guard([&] {
    config::sunshine = old_config;
    config::video.linux_display.virtual_display_backend = old_backend;
    config::video.adaptive_bitrate = old_adaptive;
    adaptive_bitrate::load_config();
    adaptive_bitrate::reset();
    std::filesystem::remove_all(directory);
  });
  config::sunshine.username = "test-admin";
  config::sunshine.api_key = "isolated-test-api-key";
  // getConfig probes for a virtual display first. Keep that probe to a PATH
  // lookup; auto would try to load the EVDI module.
  config::video.linux_display.virtual_display_backend = "kscreen";
  const auto config_file = (directory / "polaris.conf").string();
  config::sunshine.config_file = config_file;
  const std::string original = "sunshine_name = only-the-file-knows\nadaptive_bitrate_enabled = enabled\n";
  ASSERT_TRUE(private_state_file::write_atomic(config_file, original));
  ASSERT_EQ(::chmod(config_file.c_str(), 0664), 0);
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
    server.resource["^/api/config$"]["GET"] = confighttp::getConfig;
    server.resource["^/api/config$"]["POST"] = confighttp::saveConfig;
    server.resource["^/api/config$"]["PATCH"] = confighttp::patchConfig;
    server.resource["^/api/live-tuning$"]["POST"] = confighttp::live_tuning_http_for_tests;
    std::atomic<unsigned short> port {0};
    std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
    auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
    for (int attempt = 0; attempt < 100 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
    ASSERT_NE(port, 0);
    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port.load()), false);
    client.config.timeout = 5;
    auto send = [&](const std::string &method, const std::string &path, const std::string &content,
                    SimpleWeb::CaseInsensitiveMultimap headers) {
      headers.emplace("Cookie", "auth=" + cookie);
      headers.emplace("Content-Type", "application/json");
      return client.request(method, path, content, headers);
    };
    auto code = [](const auto &response) { return std::stoi(response->status_code); };
    // A revision the console could have kept from before the file was refused.
    const std::string kept = "\"" + std::string(64, 'a') + "\"";

    const auto read = send("GET", "/api/config", "", {});
    ASSERT_EQ(code(read), 503);
    const auto refusal = nlohmann::json::parse(read->content.string());
    ASSERT_EQ(refusal["error"], "config_unreadable");
    ASSERT_EQ(refusal["path"], config_file);
    ASSERT_FALSE(refusal["reason"].get<std::string>().empty());
    ASSERT_FALSE(refusal["fix"].get<std::string>().empty());

    const std::string settings = R"({"sunshine_name":"a-save-that-must-not-land"})";
    for (const auto &method : {"POST", "PATCH"}) {
      for (const bool with_revision : {true, false}) {
        SCOPED_TRACE(std::string {method} + (with_revision ? " with If-Match" : " without If-Match"));
        SimpleWeb::CaseInsensitiveMultimap headers;
        if (with_revision) headers.emplace("If-Match", kept);
        const auto saved = send(method, "/api/config", settings, headers);
        EXPECT_EQ(code(saved), 503);
        const auto content_type = saved->header.find("Content-Type");
        ASSERT_NE(content_type, saved->header.end());
        EXPECT_EQ(content_type->second, "application/json");
        const auto text = saved->content.string();
        EXPECT_EQ(text.find("only-the-file-knows"), std::string::npos) << text;
        EXPECT_EQ(nlohmann::json::parse(text), refusal);
      }
    }

    // The console's Live Tuning switch reads the same fields. Its own code stays beside them.
    const auto tuned = send("POST", "/api/live-tuning", R"({"enabled":false})",
                            {{"X-CSRF-Token", "test-csrf"}, {"If-Match", kept}});
    EXPECT_EQ(code(tuned), 503);
    const auto tuning = nlohmann::json::parse(tuned->content.string());
    EXPECT_EQ(tuning["status"], false);
    EXPECT_EQ(tuning["code"], "config_unreadable");
    EXPECT_EQ(tuning["error"], "config_unreadable");
    EXPECT_EQ(tuning["path"], refusal["path"]);
    EXPECT_EQ(tuning["reason"], refusal["reason"]);
    EXPECT_EQ(tuning["fix"], refusal["fix"]);
    EXPECT_TRUE(tuning.contains("live_tuning"));
    EXPECT_TRUE(adaptive_bitrate::get_state().configured_enabled);

    // Nothing was written, and the file is still the one the refusal describes.
    struct stat after {};
    ASSERT_EQ(::stat(config_file.c_str(), &after), 0);
    EXPECT_EQ(after.st_mode & 07777, 0664u);
    ASSERT_EQ(::chmod(config_file.c_str(), 0600), 0);
    EXPECT_EQ(private_state_file::read_secure(config_file, 4096).payload, original);

    // Once the file reads again, the same save goes through.
    const auto fixed = send("GET", "/api/config", "", {});
    ASSERT_EQ(code(fixed), 200);
    const auto revision = nlohmann::json::parse(fixed->content.string())["configuration_revision"].get<std::string>();
    const auto patched = send("PATCH", "/api/config", R"({"sunshine_name":"after-the-fix"})",
                              {{"If-Match", "\"" + revision + "\""}});
    ASSERT_EQ(code(patched), 200) << patched->content.string();
    EXPECT_NE(private_state_file::read_secure(config_file, 4096).payload.find("sunshine_name = after-the-fix"),
              std::string::npos);
  });
}
