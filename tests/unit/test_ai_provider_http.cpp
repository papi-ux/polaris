/**
 * @file tests/unit/test_ai_provider_http.cpp
 * @brief Exercise provider requests through libcurl against a bounded local HTTP fixture.
 */
#include "src/ai_optimizer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#ifdef __linux__
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

namespace {
  using json = nlohmann::json;

  class loopback_proxy_bypass_t {
  public:
    loopback_proxy_bypass_t(): lower_(saved("no_proxy")), upper_(saved("NO_PROXY")) {
      ready_ = setenv("no_proxy", "127.0.0.1", 1) == 0 && setenv("NO_PROXY", "127.0.0.1", 1) == 0;
    }
    ~loopback_proxy_bypass_t() {
      restore("no_proxy", lower_);
      restore("NO_PROXY", upper_);
    }
    bool ready() const { return ready_; }

  private:
    static std::optional<std::string> saved(const char *name) {
      const auto value = std::getenv(name);
      return value ? std::optional<std::string> {value} : std::nullopt;
    }
    static void restore(const char *name, const std::optional<std::string> &value) {
      if (value) (void) setenv(name, value->c_str(), 1);
      else (void) unsetenv(name);
    }
    std::optional<std::string> lower_;
    std::optional<std::string> upper_;
    bool ready_ = false;
  };

  // One loopback request per fixture, with bounded accept/read waits and RAII
  // shutdown even if the production request is rejected before network I/O.
  class provider_server_t {
  public:
    explicit provider_server_t(std::function<std::string(const std::string &)> reply): reply_(std::move(reply)) {
      if (!proxy_bypass_.ready()) return;
      listener_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (listener_ < 0) return;
      sockaddr_in address {};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      socklen_t size = sizeof(address);
      if (bind(listener_, reinterpret_cast<sockaddr *>(&address), size) != 0 ||
          listen(listener_, 1) != 0 ||
          getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size) != 0) return;
      port_ = ntohs(address.sin_port);
      thread_ = std::thread([this] { serve(); });
    }

    ~provider_server_t() {
      finish();
      if (listener_ >= 0) close(listener_);
    }

    void finish() {
      stop_.store(true);
      if (thread_.joinable()) thread_.join();
    }

    std::string base_url(const std::string &path = "/v1/") const { return "http://127.0.0.1:" + std::to_string(port_) + path; }
    bool ready() const { return port_ != 0; }
    const std::string &request() const { return request_; } // Read only after finish().

    static std::string response(const json &body, int status = 200) {
      const auto data = body.dump();
      return "HTTP/1.1 " + std::to_string(status) + " Fixture\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(data.size()) + "\r\nConnection: close\r\n\r\n" + data;
    }

    static json body(const std::string &request) {
      return json::parse(request.substr(request.find("\r\n\r\n") + 4));
    }

  private:
    bool readable(int fd) {
      pollfd poll_fd {fd, POLLIN, 0};
      return poll(&poll_fd, 1, 50) > 0 && (poll_fd.revents & POLLIN);
    }

    void serve() {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
        if (!readable(listener_)) continue;
        const int client = accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) return;
        timeval timeout {1, 0};
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        try {
          while (!stop_.load() && std::chrono::steady_clock::now() < deadline && request_.size() < 65536) {
            if (!readable(client)) continue;
            char buffer[4096];
            const auto count = recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            request_.append(buffer, count);
            const auto end = request_.find("\r\n\r\n");
            if (end == std::string::npos) continue;
            const auto length_header = request_.find("Content-Length: ");
            const auto length = length_header < end ? std::stoul(request_.substr(length_header + 16)) : 0;
            if (request_.size() < end + 4 + length) continue;
            const auto reply = reply_(request_);
            size_t sent = 0;
            while (sent < reply.size()) {
              const auto count_sent = send(client, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
              if (count_sent <= 0) break;
              sent += count_sent;
            }
            break;
          }
        } catch (...) {
          // A malformed fixture request gets no valid response, so the caller
          // fails its ordinary result assertions without an escaped thread.
        }
        close(client);
        return;
      }
    }

    // GTest runs these synchronous provider cases serially. Override only the
    // loopback bypass during each fixture and restore the caller's environment;
    // inherited HTTP/ALL_PROXY settings must never route synthetic test traffic.
    loopback_proxy_bypass_t proxy_bypass_;
    int listener_ = -1;
    unsigned short port_ = 0;
    std::function<std::string(const std::string &)> reply_;
    std::atomic<bool> stop_ {false};
    std::string request_;
    std::thread thread_;
  };

  json explanation() {
    return {
      {"likely_cause", "Synthetic provider test"},
      {"evidence", json::array({"Synthetic evidence"})},
      {"try_first", json::array({"Review Doctor"})},
      {"advanced_detail", "Doctor retains authority."},
      {"confidence", "low"},
      {"destructive_action_allowed", false}
    };
  }

  std::string completion(const std::string &content) {
    return provider_server_t::response({{"choices", json::array({{
      {"message", {{"content", content}}}, {"finish_reason", "stop"}
    }})}});
  }

  ai_optimizer::config_t config_for(const provider_server_t &server, std::string provider = "deepseek") {
    ai_optimizer::config_t config;
    config.enabled = true;
    config.provider = std::move(provider);
    config.auth_mode = "api_key";
    config.api_key = "synthetic-fixture-key";
    config.base_url = server.base_url();
    config.timeout_ms = 1500;
    return config;
  }

  auto test_provider(const ai_optimizer::config_t &config) {
    return ai_optimizer::test_provider_with_config(config, "Synthetic device", "Synthetic app", "Synthetic GPU");
  }
}

TEST(AiProviderHttp, DeepSeekDiscoversModelsWithBearerAuthAtConfiguredEndpoint) {
  provider_server_t server([](const auto &) {
    return provider_server_t::response({{"data", json::array({{{"id", "deepseek-v4-flash"}}, {{"id", "deepseek-v4-pro"}}})}});
  });
  ASSERT_TRUE(server.ready());
  const auto catalog = json::parse(ai_optimizer::get_models_json_with_config(config_for(server)));
  server.finish();
  ASSERT_TRUE(catalog.value("discovered", false)) << catalog.dump();
  EXPECT_EQ(catalog.at("provider"), "deepseek");
  EXPECT_EQ(catalog.at("model"), "deepseek-v4-flash");
  EXPECT_EQ(catalog.at("models").size(), 2);
  EXPECT_EQ(server.request().find("GET /v1/models HTTP/1.1\r\n"), 0);
  EXPECT_NE(server.request().find("Authorization: Bearer synthetic-fixture-key\r\n"), std::string::npos);
  EXPECT_EQ(server.request().find("x-api-key:"), std::string::npos);
}

TEST(AiProviderHttp, DeepSeekExplanationUsesJsonObjectAndDisablesThinking) {
  provider_server_t server([](const auto &request) {
    const auto body = provider_server_t::body(request);
    if (body.at("response_format").at("type") != "json_object" || body.at("thinking").at("type") != "disabled") {
      return provider_server_t::response({{"error", "Unsupported response format"}}, 400);
    }
    return completion(explanation().dump());
  });
  ASSERT_TRUE(server.ready());
  const auto result = test_provider(config_for(server));
  server.finish();
  ASSERT_TRUE(result.explanation_json.has_value()) << result.error;
  const auto parsed = json::parse(*result.explanation_json);
  EXPECT_EQ(parsed.at("authority"), "explanation_only");
  EXPECT_FALSE(parsed.at("may_define_settings").get<bool>());
  const auto request = provider_server_t::body(server.request());
  EXPECT_EQ(server.request().find("POST /v1/chat/completions HTTP/1.1\r\n"), 0);
  EXPECT_NE(server.request().find("Authorization: Bearer synthetic-fixture-key\r\n"), std::string::npos);
  EXPECT_EQ(request.at("model"), "deepseek-v4-flash");
  EXPECT_EQ(request.at("max_tokens"), 1024);
  const auto prompt = request.at("messages")[0].at("content").get<std::string>();
  EXPECT_NE(prompt.find("\"additionalProperties\":false"), std::string::npos);
  EXPECT_NE(prompt.find("\"const\":false"), std::string::npos);
}

TEST(AiProviderHttp, DeepSeekRuntimeExplanationUsesSameContractAsDraftTest) {
  provider_server_t server([](const auto &request) {
    const auto body = provider_server_t::body(request);
    if (body.at("response_format").at("type") != "json_object") return provider_server_t::response({}, 400);
    return completion(explanation().dump());
  });
  ASSERT_TRUE(server.ready());
  auto config = config_for(server);
  config.base_url = server.base_url("");
  const auto result = json::parse(ai_optimizer::explain_doctor_json_with_config(
    config, R"({"doctor":{"simple_state":"Synthetic evidence"}})"));
  server.finish();
  EXPECT_EQ(server.request().find("POST /chat/completions HTTP/1.1\r\n"), 0);
  EXPECT_TRUE(result.value("status", false)) << result.dump();
  EXPECT_EQ(result.at("authority"), "explanation_only");
  EXPECT_FALSE(result.at("may_define_settings").get<bool>());
}

TEST(AiProviderHttp, DeepSeekOptimizationAlsoUsesCompatibleFormatAndPreservesSelectedModel) {
  provider_server_t server([](const auto &request) {
    const auto body = provider_server_t::body(request);
    if (body.at("response_format").at("type") != "json_object" || body.at("thinking").at("type") != "disabled") {
      return provider_server_t::response({}, 400);
    }
    return completion(json {
      {"display_mode", "1280x720x60"}, {"color_range", 0}, {"hdr", false}, {"virtual_display", false},
      {"target_bitrate_kbps", 15000}, {"nvenc_tune", 2}, {"preferred_codec", "h264"},
      {"recommended_mode", nullptr}, {"reasoning", "Synthetic baseline"}
    }.dump());
  });
  ASSERT_TRUE(server.ready());
  auto config = config_for(server);
  config.model = "deepseek-v4-pro";
  const auto result = ai_optimizer::request_sync_with_config(config, "Synthetic device", "Synthetic app", "Synthetic GPU");
  server.finish();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->display_mode, "1280x720x60");
  const auto body = provider_server_t::body(server.request());
  EXPECT_EQ(body.at("model"), "deepseek-v4-pro");
  EXPECT_EQ(body.at("max_tokens"), 1024);
}

TEST(AiProviderHttp, DeepSeekRejectsInvalidExplanationAndNeverGrantsSettingsAuthority) {
  const auto valid = explanation();
  std::vector<json> invalid;
  auto candidate = valid;
  candidate["destructive_action_allowed"] = true;
  invalid.push_back(candidate);
  candidate = valid;
  candidate["display_mode"] = "1920x1080x120";
  invalid.push_back(candidate);
  candidate = valid;
  candidate.erase("confidence");
  invalid.push_back(candidate);
  candidate = valid;
  candidate["evidence"] = "wrong type";
  invalid.push_back(candidate);
  candidate = valid;
  candidate["confidence"] = "certain";
  invalid.push_back(candidate);
  for (const auto &output : invalid) {
    SCOPED_TRACE(output.dump());
    provider_server_t server([&](const auto &) { return completion(output.dump()); });
    ASSERT_TRUE(server.ready());
    const auto result = test_provider(config_for(server));
    EXPECT_FALSE(result.explanation_json.has_value());
    EXPECT_EQ(result.code, "invalid_response");
  }
}

TEST(AiProviderHttp, DeepSeekEmptyAndTruncatedOutputFailClosed) {
  for (const auto &output : {std::string {}, std::string {"{\"likely_cause\":\"unfinished"}}) {
    provider_server_t server([&](const auto &) { return completion(output); });
    ASSERT_TRUE(server.ready());
    const auto result = test_provider(config_for(server));
    EXPECT_FALSE(result.explanation_json.has_value());
    EXPECT_EQ(result.code, output.empty() ? "empty_response" : "invalid_response");
  }
}

TEST(AiProviderHttp, DeepSeekAuthenticationFailuresDoNotEchoProviderBodies) {
  provider_server_t server([](const auto &) {
    return provider_server_t::response({{"error", "private-provider-echo"}}, 401);
  });
  ASSERT_TRUE(server.ready());
  const auto result = test_provider(config_for(server));
  EXPECT_FALSE(result.explanation_json.has_value());
  EXPECT_EQ(result.code, "authentication_failed");
  EXPECT_FALSE(result.retryable);
  EXPECT_EQ((result.error + result.detail + result.action).find("private-provider-echo"), std::string::npos);
}

TEST(AiProviderHttp, DeepSeekRequiresApiKeyEvenWithLegacySubscriptionOrNone) {
  for (const auto &mode : {"subscription", "none", "api_key"}) {
    provider_server_t server([](const auto &) { return completion(explanation().dump()); });
    ASSERT_TRUE(server.ready());
    auto config = config_for(server);
    config.auth_mode = mode;
    config.use_subscription = true;
    config.api_key.clear();
    const auto result = test_provider(config);
    server.finish();
    EXPECT_EQ(result.code, "configuration_not_ready");
    EXPECT_TRUE(server.request().empty());
  }
}

TEST(AiProviderHttp, DeepSeekDefaultCatalogNeedsNoNetworkWithoutKey) {
  ai_optimizer::config_t config;
  config.provider = "DeepSeek";
  const auto result = json::parse(ai_optimizer::get_models_json_with_config(config));
  EXPECT_EQ(result.at("provider"), "deepseek");
  EXPECT_EQ(result.at("base_url"), "https://api.deepseek.com");
  EXPECT_EQ(result.at("model"), "deepseek-v4-flash");
  EXPECT_EQ(result.at("auth_mode"), "api_key");
  EXPECT_FALSE(result.at("discovered").get<bool>());
}

TEST(AiProviderHttp, OtherCompatibleProvidersRetainStrictSchemaAndNoThinkingOverride) {
  for (const auto &provider : {"openai", "gemini", "local"}) {
    SCOPED_TRACE(provider);
    provider_server_t server([](const auto &) { return completion(explanation().dump()); });
    ASSERT_TRUE(server.ready());
    EXPECT_TRUE(test_provider(config_for(server, provider)).explanation_json.has_value());
    server.finish();
    const auto body = provider_server_t::body(server.request());
    EXPECT_EQ(body.at("response_format").at("type"), "json_schema");
    EXPECT_FALSE(body.contains("thinking"));
    EXPECT_EQ(body.at("max_tokens"), 600);
  }
}

TEST(AiProviderHttp, AnthropicExplanationReportsSupportedDeepSeekProfileWithoutSendingKey) {
  provider_server_t server([](const auto &) { return completion(explanation().dump()); });
  ASSERT_TRUE(server.ready());
  const auto result = test_provider(config_for(server, "anthropic"));
  server.finish();
  EXPECT_EQ(result.code, "explanation_transport_unsupported");
  EXPECT_NE(result.action.find("DeepSeek profile"), std::string::npos);
  EXPECT_TRUE(server.request().empty());
}
#endif
