#include "../tests_common.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/private_state_file.h"
#include "src/process.h"
#include <Simple-Web-Server/server_https.hpp>
#include <Simple-Web-Server/client_https.hpp>
#include <atomic>
#include <thread>

namespace confighttp {
  using setup_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  void epicSetup(std::shared_ptr<setup_server_t::Response>, std::shared_ptr<setup_server_t::Request>);
  void epic_setup_http_for_tests(std::shared_ptr<setup_server_t::Response>, std::shared_ptr<setup_server_t::Request>,
    const std::function<proc::epic_setup_read_t(const std::optional<epic_library::identity_t> &)> &, bool);
  void with_web_session_for_tests(const std::filesystem::path &, const std::string &,
    const std::function<void(const std::string &)> &);
  void with_web_session_store_for_tests(const std::filesystem::path &, const std::function<void()> &);
}

namespace {
  using namespace std::chrono_literals;
  using client_t = SimpleWeb::Client<SimpleWeb::HTTPS>;
  using json = nlohmann::json;
  class EpicSetupHttp: public testing::Test {
  protected:
    config::sunshine_t previous = config::sunshine;
    std::filesystem::path directory;
    std::atomic<int> calls {0};
    bool credentials_available = true;
    std::function<proc::epic_setup_read_t(const std::optional<epic_library::identity_t> &)> reader =
      [&](const auto &) { ++calls; return proc::epic_setup_read_t {}; };
    const SimpleWeb::CaseInsensitiveMultimap bearer {{"Authorization", "Bearer isolated-setup-key"}};

    void SetUp() override {
      directory = std::filesystem::temp_directory_path() /
        ("polaris-epic-setup-http-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      ASSERT_TRUE(std::filesystem::create_directory(directory));
      std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
      config::sunshine.username = "setup-owner";
      config::sunshine.api_key = "isolated-setup-key";
      const auto credentials = crypto::gen_creds("localhost", 2048);
      ASSERT_TRUE(private_state_file::write_atomic(directory / "cert.pem", credentials.x509));
      ASSERT_TRUE(private_state_file::write_atomic(directory / "key.pem", credentials.pkey));
    }
    void TearDown() override {
      config::sunshine = previous;
      std::filesystem::remove_all(directory);
    }
    void run(const std::function<void(client_t &, const std::string &)> &check, bool certificate = false) {
      confighttp::with_web_session_for_tests(directory / "sessions.json", "setup-csrf", [&](const std::string &cookie) {
        confighttp::setup_server_t server((directory / "cert.pem").string(), (directory / "key.pem").string());
        server.config.address = "127.0.0.1";
        server.config.port = 0;
        server.config.timeout_request = 5;
        server.config.timeout_content = 5;
        server.resource["^/api/epic/setup$"]["GET"] = [&](auto response, auto request) {
          // Model an already authenticated paired TLS peer. Owner auth must ignore this authority.
          if (certificate) request->userp = std::make_shared<crypto::named_cert_t>();
          confighttp::epic_setup_http_for_tests(response, request, reader, credentials_available);
        };
        std::atomic<unsigned short> port {0};
        std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
        auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
        for (int attempt = 0; attempt < 100 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
        ASSERT_NE(port, 0);
        client_t client("127.0.0.1:" + std::to_string(port.load()), false);
        client.config.timeout = 5;
        check(client, cookie);
      });
    }
    json get(client_t &client, const std::string &query, int status,
             const SimpleWeb::CaseInsensitiveMultimap &headers) {
      const auto response = client.request("GET", "/api/epic/setup" + query, "", headers);
      EXPECT_EQ(std::stoi(response->status_code), status);
      EXPECT_EQ(response->header.at("Cache-Control"), "private, no-store");
      EXPECT_EQ(response->header.at("Content-Type"), "application/json");
      EXPECT_EQ(response->header.at("X-Content-Type-Options"), "nosniff");
      return json::parse(response->content.string());
    }
  };
}

TEST_F(EpicSetupHttp, AnonymousInvalidCookieAndInvalidBearerNeverCallNative) {
  run([&](auto &client, const auto &) {
    for (const auto &headers : {SimpleWeb::CaseInsensitiveMultimap {},
         SimpleWeb::CaseInsensitiveMultimap {{"Cookie", "auth=invalid"}},
         SimpleWeb::CaseInsensitiveMultimap {{"Authorization", "Bearer invalid"}}})
      EXPECT_EQ(get(client, "?pid=123", 401, headers).at("error"), "authentication_required");
    EXPECT_EQ(calls, 0);
  });
}
TEST_F(EpicSetupHttp, PairedCertificateAloneIsNotOwnerAuthentication) {
  run([&](auto &client, const auto &) { get(client, "", 401, {}); EXPECT_EQ(calls, 0); }, true);
}
TEST_F(EpicSetupHttp, OwnerCookieAndConfiguredBearerReachPassiveReader) {
  run([&](auto &client, const auto &cookie) {
    EXPECT_EQ(get(client, "", 200, {{"Cookie", "auth=" + cookie}}).at("version"), 1);
    get(client, "", 200, bearer);
    EXPECT_EQ(calls, 2);
  });
}
TEST_F(EpicSetupHttp, ExpiredCookieCannotReadNative) {
  run([&](auto &client, const auto &cookie) {
    const auto path = directory / "sessions.json";
    auto secure = private_state_file::read_secure(path, 65536);
    ASSERT_TRUE(secure);
    auto data = json::parse(secure.payload);
    for (auto &session : data["sessions"]) { session["created_at"] = 1; session["last_seen_at"] = 1; }
    ASSERT_TRUE(private_state_file::write_atomic(path, data.dump()));
    confighttp::with_web_session_store_for_tests(path, [&] {
      get(client, "", 401, {{"Cookie", "auth=" + cookie}});
      EXPECT_EQ(calls, 0);
    });
  });
}
TEST_F(EpicSetupHttp, UnreadableCredentialStoreAnswersPrivateGenericFailure) {
  credentials_available = false; // A failed credential reload, before owner auth can inspect native inputs.
  run([&](auto &client, const auto &cookie) {
      const auto body = get(client, "", 503, {{"Cookie", "auth=" + cookie}});
      EXPECT_EQ(body, (json {{"status", false}, {"error", "authentication_unavailable"}}));
      EXPECT_EQ(calls, 0);
  });
}
TEST_F(EpicSetupHttp, MalformedOversizedDuplicateAndExtraAuthorityAreRejected) {
  run([&](auto &client, const auto &) {
    const std::string selected = "?namespace=ns&item_id=item&artifact_id=game";
    for (const auto &query : std::vector<std::string> {"?namespace=ns", selected + "&pid=12", selected + "&namespace=ns",
        selected + "&done=true", selected + "&", std::string("?namespace=%2Ftmp&item_id=item&artifact_id=game"),
        std::string("?namespace=ns&item_id=item&artifact_id=/tmp/game"), std::string("?x=") + std::string(1025, 'x')})
      get(client, query, 400, bearer);
    EXPECT_EQ(calls, 0);
  });
}
TEST_F(EpicSetupHttp, ValidatedExactTitleIsPassedWithoutPathsOrClientAuthority) {
  reader = [&](const auto &title) {
    ++calls;
    EXPECT_TRUE(title);
    if (title) EXPECT_EQ(*title, (epic_library::identity_t {"ns", "item", "game"}));
    return proc::epic_setup_read_t {};
  };
  run([&](auto &client, const auto &) {
    EXPECT_EQ(get(client, "?namespace=ns&item_id=item&artifact_id=game", 200, bearer).at("selected"),
      (json {{"namespace_id", "ns"}, {"item_id", "item"}, {"artifact_id", "game"}}));
    EXPECT_EQ(calls, 1);
  });
}
TEST_F(EpicSetupHttp, UnsupportedResultIsStableAndNeverReadyByOmission) {
  run([&](auto &client, const auto &) {
    const auto body = get(client, "", 200, bearer);
    EXPECT_EQ(body.at("supported"), false);
    EXPECT_EQ(body.at("reason"), "unsupported");
    EXPECT_EQ(body.at("state"), "unknown");
    EXPECT_EQ(body.at("prerequisites"), "unknown");
    EXPECT_EQ(body.at("selected"), nullptr);
  });
}
TEST_F(EpicSetupHttp, RawBackendReasonsAndLabelsNeverSerialize) {
  reader = [&](const auto &) {
    ++calls;
    proc::epic_setup_read_t out;
    out.supported = true;
    out.facts.reason = "/home/private/game pid=123 cgroup=secret Orca.AppImage";
    return out;
  };
  run([&](auto &client, const auto &) {
    const auto body = get(client, "", 200, bearer);
    EXPECT_EQ(body.at("reason"), "check_unresolved");
    for (const auto *secret : {"/home", "pid=", "cgroup", "Orca"}) EXPECT_EQ(body.dump().find(secret), std::string::npos);
  });
}
TEST_F(EpicSetupHttp, BusyUnavailableMetadataAndSuccessfulEmptyScanRemainDistinct) {
  for (int scenario = 0; scenario < 3; ++scenario) {
    reader = [&, scenario](const auto &) {
      ++calls;
      proc::epic_setup_read_t out;
      out.supported = true;
      out.epoch = "opaque-epoch";
      out.facts.state = scenario == 0 ? platf::epic::readiness_state_t::busy : platf::epic::readiness_state_t::unknown;
      out.metadata_available = scenario == 2;
      out.last_scan = scenario == 2 ? platf::epic::scan_state_t::complete : platf::epic::scan_state_t::metadata_unavailable;
      out.receipt = scenario == 2 ? platf::epic::receipt_state_t::complete : platf::epic::receipt_state_t::required;
      return out;
    };
    run([&](auto &client, const auto &) {
      const auto body = get(client, "", 200, bearer);
      EXPECT_EQ(body.at("metadata_available"), scenario == 2);
      EXPECT_EQ(body.at("receipt"), scenario == 2 ? "complete" : "required");
      EXPECT_EQ(body.at("state"), scenario == 0 ? "busy" : "unknown");
    });
  }
}
TEST_F(EpicSetupHttp, ReaderExceptionReturnsPrivateGenericUnavailable) {
  reader = [&](const auto &) -> proc::epic_setup_read_t { ++calls; throw std::runtime_error("private path"); };
  run([&](auto &client, const auto &) { EXPECT_EQ(get(client, "", 503, bearer).at("error"), "status_unavailable"); });
}
