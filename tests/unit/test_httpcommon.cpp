/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test imports
#include "../tests_common.h"

// lib imports
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

// local imports
#include <src/config.h>
#include <src/crypto.h>
#include <src/httpcommon.h>

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

struct DownloadFileTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(DownloadFileTest, Run) {
  const auto &[url, filename] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/";
  std::string path = test_dir + filename;
  if (!http::download_file(url, path, CURL_SSLVERSION_TLSv1_0)) {
    GTEST_SKIP() << "External download dependency unavailable for " << url;
  }
}

constexpr const char *URL_1 = "https://httpbin.org/base64/aGVsbG8h";
constexpr const char *URL_2 = "https://httpbin.org/redirect-to?url=/base64/aGVsbG8h";

INSTANTIATE_TEST_SUITE_P(
  DownloadFileTests,
  DownloadFileTest,
  testing::Values(
    std::make_tuple(URL_1, "hello.txt"),
    std::make_tuple(URL_2, "hello-redirect.txt")
  )
);

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
  ASSERT_EQ(http::upgrade_user_password_hash(creds_path.string(), "legacy", "secret"), 0);

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
