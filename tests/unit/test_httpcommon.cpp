/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test imports
#include "../tests_common.h"

// lib imports
#include <Simple-Web-Server/server_https.hpp>
#include <boost/asio.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

// local imports
#include <src/config.h>
#include <src/crypto.h>
#include <src/httpcommon.h>
#include <src/utility.h>

struct UrlEscapeTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlEscapeTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_escape(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlEscapeTests,
  UrlEscapeTest,
  testing::Values(
    std::make_tuple("igdb_0123456789", "igdb_0123456789"),
    std::make_tuple("../../../", "..%2F..%2F..%2F"),
    std::make_tuple("..*\\", "..%2A%5C")
  )
);

struct UrlGetHostTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlGetHostTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_get_host(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlGetHostTests,
  UrlGetHostTest,
  testing::Values(
    std::make_tuple("https://images.igdb.com/example.txt", "images.igdb.com"),
    std::make_tuple("http://localhost:8080", "localhost"),
    std::make_tuple("nonsense!!}{::", "")
  )
);

struct UrlIsHttpsHostTest: testing::TestWithParam<std::tuple<std::string, std::string, bool>> {};

TEST_P(UrlIsHttpsHostTest, Run) {
  const auto &[url, host, expected] = GetParam();
  EXPECT_EQ(http::url_is_https_host(url, host), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlIsHttpsHostTests,
  UrlIsHttpsHostTest,
  testing::Values(
    std::make_tuple("https://images.igdb.com/cover.png", "images.igdb.com", true),
    std::make_tuple("HTTPS://IMAGES.IGDB.COM/cover.png", "images.igdb.com", true),
    std::make_tuple("http://images.igdb.com/cover.png", "images.igdb.com", false),
    std::make_tuple("https://images.igdb.com.evil.example/cover.png", "images.igdb.com", false),
    std::make_tuple("https://user@images.igdb.com/cover.png", "images.igdb.com", false),
    std::make_tuple("https://images.igdb.com:8443/cover.png", "images.igdb.com", false),
    std::make_tuple("not a URL", "images.igdb.com", false)
  )
);

TEST(HttpCommonRedirectPolicy, RejectsInitialUrlBeforeRequestingIt) {
  int request_count = 0;
  const auto allowed = [](std::string_view url) {
    return http::url_is_https_host(url, "cdn.example");
  };

  EXPECT_FALSE(http::redirect::follow(
    "http://cdn.example/asset.jpg",
    allowed,
    [&](const std::string &) {
      ++request_count;
      return http::redirect::hop_result_t {true, 200, std::nullopt};
    }
  ));
  EXPECT_EQ(request_count, 0);
}

TEST(HttpCommonRedirectPolicy, RejectsRedirectBeforeRequestingDisallowedHost) {
  std::vector<std::string> requested;
  const auto allowed = [](std::string_view url) {
    return http::url_is_https_host(url, "cdn.example");
  };

  EXPECT_FALSE(http::redirect::follow(
    "https://cdn.example/asset.jpg",
    allowed,
    [&](const std::string &url) {
      requested.push_back(url);
      return http::redirect::hop_result_t {
        true,
        302,
        "https://metadata.internal/latest",
      };
    }
  ));
  EXPECT_EQ(requested, (std::vector<std::string> {"https://cdn.example/asset.jpg"}));
}

TEST(HttpCommonRedirectPolicy, AllowsFiveValidatedRedirects) {
  std::vector<std::string> requested;
  const auto allowed = [](std::string_view url) {
    return http::url_is_https_host(url, "cdn.example");
  };

  EXPECT_TRUE(http::redirect::follow(
    "https://cdn.example/0",
    allowed,
    [&](const std::string &url) {
      requested.push_back(url);
      if (requested.size() == 6) {
        return http::redirect::hop_result_t {true, 200, std::nullopt};
      }
      return http::redirect::hop_result_t {
        true,
        302,
        "https://cdn.example/" + std::to_string(requested.size()),
      };
    }
  ));
  EXPECT_EQ(requested.size(), 6);
}

TEST(HttpCommonRedirectPolicy, RejectsTheSixthRedirectWithoutRequestingItsTarget) {
  std::vector<std::string> requested;
  const auto allowed = [](std::string_view url) {
    return http::url_is_https_host(url, "cdn.example");
  };

  EXPECT_FALSE(http::redirect::follow(
    "https://cdn.example/0",
    allowed,
    [&](const std::string &url) {
      requested.push_back(url);
      return http::redirect::hop_result_t {
        true,
        302,
        "https://cdn.example/" + std::to_string(requested.size()),
      };
    }
  ));
  EXPECT_EQ(requested.size(), 6);
}

TEST(HttpCommonRedirectPolicy, RejectsTransportErrorsAndNonSuccessResponses) {
  const auto allowed = [](std::string_view url) {
    return http::url_is_https_host(url, "cdn.example");
  };
  EXPECT_FALSE(http::redirect::follow(
    "https://cdn.example/asset.jpg",
    allowed,
    [](const std::string &) {
      return http::redirect::hop_result_t {false, 0, std::nullopt};
    }
  ));
  EXPECT_FALSE(http::redirect::follow(
    "https://cdn.example/asset.jpg",
    allowed,
    [](const std::string &) {
      return http::redirect::hop_result_t {true, 404, std::nullopt};
    }
  ));
}

// The download cases exercise download_file() end to end against an owned loopback TLS listener instead of a
// public endpoint: a per-run self-signed certificate for the fictional host download.test (DNS subjectAltName)
// served on 127.0.0.1 port 0 and reached through the POLARIS_TESTS transport override, so the URL keeps its host
// for SNI and strict certificate and host-name verification, the production url_is_https_host validator, the
// production TLS minimum and the validated redirect loop run unchanged, and fixture bounds keep every case short.
// No skip, no public endpoint, no looser TLS: an untrusted certificate, a cross-host redirect and a stalled
// handshake are refused by the three DownloadFileTrust controls below, each of which also proves what the
// transport did (an accepted-then-failed handshake on the owned listener, zero connections to a second owned
// listener standing in for the forbidden host, exactly one accepted connection that never answered), so that a
// refusal for the wrong reason, or a stray DNS attempt, cannot satisfy them.
namespace {
  using namespace std::chrono_literals;
  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using https_response_t = std::shared_ptr<https_server_t::Response>;
  using https_request_t = std::shared_ptr<https_server_t::Request>;

  constexpr std::string_view k_fixture_host = "download.test";
  // The forbidden redirect target. Never trusted and never resolved: a control maps it to a second owned
  // listener so that an attempted hop would be an observable local connection, not an external DNS attempt.
  constexpr std::string_view k_forbidden_host = "other.test";
  constexpr std::string_view k_hello_path = "/base64/aGVsbG8h";
  constexpr std::string_view k_hello_body = "hello!";
  constexpr long k_fixture_connect_timeout_ms = 5000;
  constexpr long k_fixture_total_timeout_ms = 10000;
  // The stalled-handshake control waits for this bound, then the assertion allows a margin on a loaded runner.
  constexpr long k_stall_connect_timeout_ms = 1000;
  constexpr auto k_stall_margin = 3s;
  // How long a control waits for the listener thread to report an already-completed handshake failure before
  // the fixture is stopped and joined; the assertion still reads the count only after that stop.
  constexpr auto k_error_report_deadline = 2s;

  // A self-signed certificate for the fixture host carrying a DNS subjectAltName, which strict host-name
  // verification matches; crypto::gen_creds writes only a CN, and crypto.cpp stays unchanged.
  crypto::creds_t gen_fixture_creds(std::string_view host) {
    crypto::x509_t x509 {X509_new()};
    crypto::pkey_ctx_t ctx {EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr)};
    crypto::pkey_t pkey;
    EVP_PKEY_keygen_init(ctx.get());
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(), 2048);
    EVP_PKEY_keygen(ctx.get(), &pkey);

    X509_set_version(x509.get(), 2);
    crypto::bignum_t serial {BN_new()};
    BN_rand(serial.get(), 159, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY);
    BN_set_negative(serial.get(), 0);
    BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(x509.get()));
    X509_gmtime_adj(X509_getm_notBefore(x509.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509.get()), 60 * 60 * 24);
    X509_set_pubkey(x509.get(), pkey.get());

    auto name = X509_get_subject_name(x509.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>(host.data()),
                               static_cast<int>(host.size()), -1, 0);
    X509_set_issuer_name(x509.get(), name);

    const std::string san = "DNS:" + std::string {host};
    if (X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san.c_str())) {
      X509_add_ext(x509.get(), extension, -1);
      X509_EXTENSION_free(extension);
    }
    X509_sign(x509.get(), pkey.get(), EVP_sha256());
    return {crypto::pem(x509), crypto::pem(pkey)};
  }

  void write_fixture_file(const std::filesystem::path &path, const std::string &content) {
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output << content;
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  }

  // A private directory holding the fixture's certificate, key and downloaded files; removed with the fixture.
  std::filesystem::path make_fixture_directory() {
    const auto directory = std::filesystem::temp_directory_path() /
      ("polaris-http-download-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const auto credentials = gen_fixture_creds(k_fixture_host);
    write_fixture_file(directory / "cert.pem", credentials.x509);
    write_fixture_file(directory / "key.pem", credentials.pkey);
    return directory;
  }

  std::string fixture_url(std::string_view path) {
    return "https://" + std::string {k_fixture_host} + std::string {path};
  }

  // The production validator, pointed at the fixture host: https only, exact host, no port, no user info.
  bool fixture_host_allowed(std::string_view candidate) {
    return http::url_is_https_host(candidate, k_fixture_host);
  }

  std::optional<std::string> file_bytes(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return std::nullopt;
    }
    return std::string {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  // One owned HTTPS listener: the hello body at the download path, a same-host redirect to it, a cross-host
  // redirect, the record of every request it answered in order, and the record of every connection error the
  // listener reported (a failed TLS handshake on an accepted connection arrives here, through on_error, with an
  // asio.ssl error). Stopped and joined on every path, and counters are read only after that stop.
  struct download_fixture_t {
    std::filesystem::path directory;
    https_server_t server;
    std::atomic<unsigned short> port {0};
    std::jthread worker;
    std::mutex mutex;
    std::vector<std::string> requests;
    std::vector<std::string> errors;
    bool stopped = false;

    download_fixture_t():
        directory(make_fixture_directory()),
        server((directory / "cert.pem").string(), (directory / "key.pem").string()) {
      server.config.address = "127.0.0.1";
      server.config.port = 0;
      server.config.timeout_request = 5;
      server.config.timeout_content = 5;
      server.resource["^/base64/aGVsbG8h$"]["GET"] = [this](https_response_t response, https_request_t request) {
        record(request);
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "text/plain");
        response->write(SimpleWeb::StatusCode::success_ok, k_hello_body, headers);
      };
      server.resource["^/redirect-to$"]["GET"] = [this](https_response_t response, https_request_t request) {
        record(request);
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Location", fixture_url(k_hello_path));
        const auto status = request->query_string == "url=/base64/aGVsbG8h" ?
                              SimpleWeb::StatusCode::redirection_found :
                              SimpleWeb::StatusCode::client_error_not_found;
        response->write(status, "", headers);
      };
      server.resource["^/redirect-elsewhere$"]["GET"] = [this](https_response_t response, https_request_t request) {
        record(request);
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Location", "https://" + std::string {k_forbidden_host} + "/x");
        response->write(SimpleWeb::StatusCode::redirection_found, "", headers);
      };
      // Every accept, handshake or read error the listener sees, as "<category>:<value> <message>".
      server.on_error = [this](https_request_t, const SimpleWeb::error_code &ec) {
        std::lock_guard lock(mutex);
        errors.push_back(std::string {ec.category().name()} + ":" + std::to_string(ec.value()) + " " + ec.message());
      };
      worker = std::jthread([this] {
        server.start([this](unsigned short assigned) { port = assigned; });
      });
      for (int attempt = 0; attempt < 500 && port == 0; ++attempt) {
        std::this_thread::sleep_for(10ms);
      }
    }

    ~download_fixture_t() {
      stop();
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }

    // Stops the listener and joins its thread once; after it, no callback can run and the records are final.
    void stop() {
      if (stopped) {
        return;
      }
      stopped = true;
      server.stop();
      if (worker.joinable()) {
        worker.join();
      }
    }

    void record(const https_request_t &request) {
      std::lock_guard lock(mutex);
      requests.push_back(request->path + (request->query_string.empty() ? "" : "?" + request->query_string));
    }

    std::vector<std::string> seen() {
      std::lock_guard lock(mutex);
      return requests;
    }

    std::vector<std::string> reported_errors() {
      std::lock_guard lock(mutex);
      return errors;
    }

    // Errors from the TLS layer (asio.ssl, including a truncated stream): a certificate the client refused shows
    // up here as an accepted connection whose handshake failed, which no-connection never produces.
    std::size_t ssl_handshake_errors() {
      std::lock_guard lock(mutex);
      std::size_t count = 0;
      for (const auto &error : errors) {
        count += error.rfind("asio.ssl", 0) == 0;
      }
      return count;
    }

    // Bounded wait for the listener thread to deliver an already-completed handshake failure. It only gives the
    // other thread time to report; the control still asserts on the count read after stop().
    bool await_ssl_handshake_errors(std::size_t count, std::chrono::milliseconds deadline) {
      const auto until = std::chrono::steady_clock::now() + deadline;
      while (ssl_handshake_errors() < count && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(10ms);
      }
      return ssl_handshake_errors() >= count;
    }

    std::string error_log() {
      std::string joined;
      for (const auto &error : reported_errors()) {
        joined += error + "\n";
      }
      return joined;
    }

    std::string file(std::string_view name) const {
      return (directory / std::string {name}).string();
    }

    // The test-only transport: the listener as the destination of the URL host, no proxy for that host, the
    // fixture bounds, and the fixture's own certificate as the only trust anchor when @p trusted.
    http::download_transport_override_t transport(bool trusted) const {
      http::download_transport_override_t transport_override;
      transport_override.ca_bundle_path = trusted ? (directory / "cert.pem").string() : std::string {};
      transport_override.connect_to = {std::string {k_fixture_host} + ":443:127.0.0.1:" + std::to_string(port.load())};
      transport_override.no_proxy = std::string {k_fixture_host};
      transport_override.connect_timeout_ms = k_fixture_connect_timeout_ms;
      transport_override.total_timeout_ms = k_fixture_total_timeout_ms;
      return transport_override;
    }
  };

  // A raw TCP acceptor on 127.0.0.1 port 0 that accepts one connection and never answers, counting what it
  // accepted. It stands in for a stalled server (the handshake control) and for the forbidden redirect host
  // (the cross-host control, where its count must stay zero). Stopped and joined on every path; the count is
  // read only after stop().
  struct silent_listener_t {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor;
    boost::asio::ip::tcp::socket held;
    std::jthread worker;
    std::atomic<int> accepted {0};
    std::string accept_error;
    bool stopped = false;

    silent_listener_t():
        acceptor(io, boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0)),
        held(io) {
      acceptor.async_accept(held, [this](const boost::system::error_code &ec) {
        if (ec) {
          accept_error = ec.message();
          return;
        }
        ++accepted;
      });
      worker = std::jthread([this] { io.run(); });
    }

    ~silent_listener_t() {
      stop();
    }

    void stop() {
      if (stopped) {
        return;
      }
      stopped = true;
      io.stop();
      if (worker.joinable()) {
        worker.join();
      }
      boost::system::error_code ignored;
      held.close(ignored);
      acceptor.close(ignored);
    }

    unsigned short port() const {
      return acceptor.local_endpoint().port();
    }

    std::string connect_to_entry(std::string_view host) const {
      return std::string {host} + ":443:127.0.0.1:" + std::to_string(port());
    }
  };
}  // namespace

struct DownloadFileTest: testing::TestWithParam<std::tuple<std::string, std::string, std::vector<std::string>>> {};

TEST_P(DownloadFileTest, Run) {
  const auto &[path, filename, expected_requests] = GetParam();
  download_fixture_t fixture;
  ASSERT_NE(fixture.port.load(), 0) << "the owned listener did not start";
  const auto file = fixture.file(filename);
  const http::scoped_download_transport_override_t transport {fixture.transport(true)};

  // The production TLS minimum (the default argument) and the production validator, unchanged.
  ASSERT_TRUE(http::download_file(fixture_url(path), file, fixture_host_allowed));
  EXPECT_EQ(file_bytes(file), std::optional<std::string> {std::string {k_hello_body}});
  EXPECT_EQ(fixture.seen(), expected_requests);
}

INSTANTIATE_TEST_SUITE_P(
  DownloadFileTests,
  DownloadFileTest,
  testing::Values(
    std::make_tuple(std::string {"/base64/aGVsbG8h"}, std::string {"hello.txt"},
                    std::vector<std::string> {"/base64/aGVsbG8h"}),
    std::make_tuple(std::string {"/redirect-to?url=/base64/aGVsbG8h"}, std::string {"hello-redirect.txt"},
                    std::vector<std::string> {"/redirect-to?url=/base64/aGVsbG8h", "/base64/aGVsbG8h"})
  )
);

TEST(DownloadFileTrust, RefusesTheOwnedCertificateWithoutTheTestTrustSeam) {
  download_fixture_t fixture;
  ASSERT_NE(fixture.port.load(), 0) << "the owned listener did not start";
  const auto file = fixture.file("untrusted.txt");
  // Same listener, same destination, but only the default trust store: strict peer verification refuses it.
  const http::scoped_download_transport_override_t transport {fixture.transport(false)};

  EXPECT_FALSE(http::download_file(fixture_url(k_hello_path), file, fixture_host_allowed));
  EXPECT_FALSE(std::filesystem::exists(file));
  // The refusal happened on an owned connection: the listener accepted it and its TLS handshake failed. A
  // transport that never connected would report nothing, and a request would have been recorded.
  EXPECT_TRUE(fixture.await_ssl_handshake_errors(1, k_error_report_deadline)) << fixture.error_log();
  fixture.stop();
  EXPECT_GE(fixture.ssl_handshake_errors(), 1u) << fixture.error_log();
  EXPECT_TRUE(fixture.seen().empty()) << "the handshake must fail before any request reaches the listener";
}

TEST(DownloadFileTrust, RefusesARedirectToAnotherHostBeforeRequestingIt) {
  download_fixture_t fixture;
  ASSERT_NE(fixture.port.load(), 0) << "the owned listener did not start";
  // The forbidden host is mapped to a second owned listener and excluded from any proxy, so a hop that the
  // validator failed to refuse would be an observable local connection, never an external DNS or proxy attempt.
  silent_listener_t forbidden;
  const auto file = fixture.file("elsewhere.txt");
  auto cross_host_transport = fixture.transport(true);
  cross_host_transport.connect_to.push_back(forbidden.connect_to_entry(k_forbidden_host));
  cross_host_transport.no_proxy = std::string {k_fixture_host} + "," + std::string {k_forbidden_host};
  const http::scoped_download_transport_override_t transport {cross_host_transport};

  EXPECT_FALSE(http::download_file(fixture_url("/redirect-elsewhere"), file, fixture_host_allowed));
  forbidden.stop();
  EXPECT_FALSE(std::filesystem::exists(file));
  EXPECT_EQ(fixture.seen(), (std::vector<std::string> {"/redirect-elsewhere"}));
  EXPECT_EQ(forbidden.accepted.load(), 0) << "the forbidden hop was connected to instead of refused first";
  EXPECT_TRUE(forbidden.accept_error.empty()) << forbidden.accept_error;
}

TEST(DownloadFileTrust, AStalledHandshakeReturnsFalseWithinTheFixtureBound) {
  download_fixture_t fixture;
  ASSERT_NE(fixture.port.load(), 0) << "the owned listener did not start";
  silent_listener_t stalled;
  const auto file = fixture.file("stalled.txt");
  auto stalled_transport = fixture.transport(true);
  stalled_transport.connect_to = {stalled.connect_to_entry(k_fixture_host)};
  stalled_transport.connect_timeout_ms = k_stall_connect_timeout_ms;
  stalled_transport.total_timeout_ms = k_stall_connect_timeout_ms * 3;
  const http::scoped_download_transport_override_t transport {stalled_transport};

  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(http::download_file(fixture_url(k_hello_path), file, fixture_host_allowed));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  stalled.stop();
  EXPECT_GE(elapsed, std::chrono::milliseconds(k_stall_connect_timeout_ms) / 2) << "the bound was waited for";
  EXPECT_LT(elapsed, std::chrono::milliseconds(k_stall_connect_timeout_ms) + k_stall_margin);
  // The false came from the stalled owned connection, not from some other refusal: exactly one was accepted.
  EXPECT_EQ(stalled.accepted.load(), 1) << stalled.accept_error;
  EXPECT_FALSE(std::filesystem::exists(file));
  EXPECT_TRUE(fixture.seen().empty());
}

namespace {
  struct sunshine_creds_snapshot_t {
    std::string username;
    std::string password;
    std::string salt;
    std::string password_hash_scheme;
    std::string api_key;
  };

  sunshine_creds_snapshot_t capture_sunshine_creds() {
    return {
      .username = config::sunshine.username,
      .password = config::sunshine.password,
      .salt = config::sunshine.salt,
      .password_hash_scheme = config::sunshine.password_hash_scheme,
      .api_key = config::sunshine.api_key,
    };
  }

  void restore_sunshine_creds(const sunshine_creds_snapshot_t &snapshot) {
    config::sunshine.username = snapshot.username;
    config::sunshine.password = snapshot.password;
    config::sunshine.salt = snapshot.salt;
    config::sunshine.password_hash_scheme = snapshot.password_hash_scheme;
    config::sunshine.api_key = snapshot.api_key;
  }
}  // namespace

TEST(HttpCommonCredentialTests, SavesCredentialsWithModernPasswordScheme) {
  const auto snapshot = capture_sunshine_creds();
  const auto temp_dir = std::filesystem::temp_directory_path() / "polaris-httpcommon-tests";
  std::filesystem::create_directories(temp_dir);
  const auto creds_path = temp_dir / "modern-creds.json";

  ASSERT_EQ(http::save_user_creds(creds_path.string(), "admin", "hunter2"), 0);

  nlohmann::json stored;
  {
    std::ifstream in(creds_path);
    in >> stored;
  }

  EXPECT_EQ(stored.value("username", ""), "admin");
  EXPECT_EQ(stored.value("password_scheme", ""), "scrypt");
  EXPECT_NE(stored.value("password", ""), util::hex(crypto::hash(std::string {"hunter2"} + stored.value("salt", ""))).to_string());

  ASSERT_EQ(http::reload_user_creds(creds_path.string()), 0);
  bool needs_upgrade = true;
  EXPECT_TRUE(http::verify_user_password("admin", "hunter2", &needs_upgrade));
  EXPECT_FALSE(needs_upgrade);

  restore_sunshine_creds(snapshot);
  std::filesystem::remove(creds_path);
}

TEST(HttpCommonCredentialTests, ReportsDurableUpgradeWhenRuntimeReloadFails) {
  const auto snapshot = capture_sunshine_creds();
  const auto temp_dir = std::filesystem::temp_directory_path() / "polaris-httpcommon-tests";
  std::filesystem::create_directories(temp_dir);
  const auto creds_path = temp_dir / "upgrade-reload-failure.json";

  nlohmann::json legacy = {
    {"username", "legacy"},
    {"salt", "legacy-salt"},
    {"password", util::hex(crypto::hash(std::string {"secret"} + "legacy-salt")).to_string()},
    {"api_key", "existing-api-key"}
  };
  {
    std::ofstream out(creds_path);
    out << legacy.dump(4);
  }
  ASSERT_EQ(http::reload_user_creds(creds_path.string()), 0);

  http::set_credential_upgrade_reload_failure_for_tests(true);
  const auto outcome = http::upgrade_user_password_hash(creds_path.string(), "legacy", "secret");
  http::set_credential_upgrade_reload_failure_for_tests(false);

  EXPECT_EQ(
    static_cast<int>(outcome),
    static_cast<int>(http::credential_upgrade_status_e::reload_failed)
  );
  nlohmann::json stored;
  {
    std::ifstream in(creds_path);
    in >> stored;
  }
  EXPECT_EQ(stored.value("password_scheme", ""), "scrypt");
  bool needs_upgrade = false;
  EXPECT_TRUE(http::verify_user_password("legacy", "secret", &needs_upgrade));
  EXPECT_TRUE(needs_upgrade);

  restore_sunshine_creds(snapshot);
  std::filesystem::remove(creds_path);
}

TEST(HttpCommonCredentialTests, UpgradesLegacyCredentialsAfterSuccessfulVerification) {
  const auto snapshot = capture_sunshine_creds();
  const auto temp_dir = std::filesystem::temp_directory_path() / "polaris-httpcommon-tests";
  std::filesystem::create_directories(temp_dir);
  const auto creds_path = temp_dir / "legacy-creds.json";

  nlohmann::json legacy = {
    {"username", "legacy"},
    {"salt", "legacy-salt"},
    {"password", util::hex(crypto::hash(std::string {"secret"} + "legacy-salt")).to_string()},
    {"api_key", "existing-api-key"}
  };
  {
    std::ofstream out(creds_path);
    out << legacy.dump(4);
  }

  ASSERT_EQ(http::reload_user_creds(creds_path.string()), 0);

  bool needs_upgrade = false;
  ASSERT_TRUE(http::verify_user_password("legacy", "secret", &needs_upgrade));
  EXPECT_TRUE(needs_upgrade);
  ASSERT_EQ(
    http::upgrade_user_password_hash(creds_path.string(), "legacy", "secret"),
    http::credential_upgrade_status_e::upgraded
  );

  nlohmann::json upgraded;
  {
    std::ifstream in(creds_path);
    in >> upgraded;
  }

  EXPECT_EQ(upgraded.value("password_scheme", ""), "scrypt");
  EXPECT_EQ(upgraded.value("api_key", ""), "existing-api-key");

  restore_sunshine_creds(snapshot);
  std::filesystem::remove(creds_path);
}
