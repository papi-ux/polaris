/**
 * @file src/httpcommon.cpp
 * @brief Definitions for common HTTP.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <utility>

// lib includes
#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <curl/curl.h>
#include <Simple-Web-Server/server_http.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "crypto.h"
#include "file_handler.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "utility.h"

namespace http {
  using namespace std::literals;
  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  int reload_user_creds(const std::string &file);
  bool user_creds_exist(const std::string &file);

  std::string unique_id;
  uuid_util::uuid_t uuid;
  net::net_e origin_web_ui_allowed;

  namespace {
    constexpr std::string_view legacy_password_hash_scheme = "sha256"sv;
    constexpr std::string_view modern_password_hash_scheme = "scrypt"sv;
#ifdef POLARIS_TESTS
    std::atomic_bool fail_credential_upgrade_reload {false};
#endif

    bool hash_password_for_scheme(
      const std::string_view password,
      const std::string_view salt,
      const std::string_view scheme,
      std::string &encoded_hash
    ) {
      if (scheme == modern_password_hash_scheme) {
        crypto::password_kdf_t derived_key {};
        if (!crypto::hash_password_scrypt(password, salt, derived_key)) {
          return false;
        }
        encoded_hash = util::hex(derived_key).to_string();
        return true;
      }

      encoded_hash = util::hex(crypto::hash(std::string {password} + std::string {salt})).to_string();
      return true;
    }

    int persist_credentials_json(
      const fs::path &path,
      const std::function<void(nlohmann::json &)> &mutation
    ) {
      if (!nvhttp::update_state_file(path.string(), mutation)) {
        BOOST_LOG(error) << "Error writing the credentials file; verify its directory is writable";
        return -1;
      }
      return 0;
    }
  }  // namespace

  int init() {
    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];
    origin_web_ui_allowed = net::from_enum_string(config::nvhttp.origin_web_ui_allowed);

    if (clean_slate) {
      uuid = uuid_util::uuid_t::generate();
      unique_id = uuid.string();
      auto dir = std::filesystem::temp_directory_path() / "Polaris"sv;
      config::nvhttp.cert = (dir / ("cert-"s + unique_id)).string();
      config::nvhttp.pkey = (dir / ("pkey-"s + unique_id)).string();
    }

    if ((!fs::exists(config::nvhttp.pkey) || !fs::exists(config::nvhttp.cert)) &&
        create_creds(config::nvhttp.pkey, config::nvhttp.cert)) {
      return -1;
    }
    if (!user_creds_exist(config::sunshine.credentials_file)) {
      BOOST_LOG(info) << "Open the Web UI to set your new username and password and getting started";
    } else if (reload_user_creds(config::sunshine.credentials_file)) {
      return -1;
    }
    return 0;
  }

  int save_user_creds(const std::string &file, const std::string &username, const std::string &password, bool run_our_mouth) {
    auto salt = crypto::rand_alphabet(16);
    std::string password_hash;
    if (!hash_password_for_scheme(password, salt, modern_password_hash_scheme, password_hash)) {
      BOOST_LOG(error) << "Couldn't derive password hash with the configured KDF";
      return -1;
    }
    if (persist_credentials_json(file, [&](nlohmann::json &outputTree) {
          outputTree["username"] = username;
          outputTree["salt"] = salt;
          outputTree["password_scheme"] = modern_password_hash_scheme;
          outputTree["password"] = password_hash;
        }) != 0) {
      return -1;
    }

    BOOST_LOG(info) << "New web UI credentials have been saved"sv;
    return 0;
  }

  bool user_creds_exist(const std::string &file) {
    if (!fs::exists(file)) {
      return false;
    }

    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
      return inputTree.find("username") != inputTree.not_found() &&
             inputTree.find("password") != inputTree.not_found() &&
             inputTree.find("salt") != inputTree.not_found();
    } catch (std::exception &e) {
      BOOST_LOG(error) << "validating user credentials: "sv << e.what();
    }

    return false;
  }

  int reload_user_creds(const std::string &file) {
    pt::ptree inputTree;
    try {
      pt::read_json(file, inputTree);
      config::sunshine.username = inputTree.get<std::string>("username");
      config::sunshine.password = inputTree.get<std::string>("password");
      config::sunshine.salt = inputTree.get<std::string>("salt");
      config::sunshine.password_hash_scheme = inputTree.get<std::string>("password_scheme", std::string {legacy_password_hash_scheme});
      config::sunshine.api_key = inputTree.get<std::string>("api_key", "");
    } catch (std::exception &e) {
      BOOST_LOG(error) << "loading user credentials: "sv << e.what();
      return -1;
    }

    // Auto-generate API key if not present
    if (config::sunshine.api_key.empty()) {
      config::sunshine.api_key = crypto::rand_alphabet(48,
        std::string_view {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"});
      BOOST_LOG(info) << "Generated new API key";
      // Save it back without racing pairing/timestamp updates in the shared file.
      if (persist_credentials_json(file, [&](nlohmann::json &outputTree) {
            outputTree["api_key"] = config::sunshine.api_key;
          }) != 0) {
        return -1;
      }
    }
    return 0;
  }

  bool verify_user_password(const std::string &username, const std::string &password, bool *needs_upgrade) {
    if (needs_upgrade) {
      *needs_upgrade = false;
    }

    if (!boost::iequals(username, config::sunshine.username)) {
      return false;
    }

    std::string expected_hash;
    if (!hash_password_for_scheme(password, config::sunshine.salt, config::sunshine.password_hash_scheme, expected_hash)) {
      BOOST_LOG(error) << "Failed to derive password hash while verifying credentials";
      return false;
    }

    const bool verified = crypto::constant_time_equals(expected_hash, config::sunshine.password);
    if (verified && needs_upgrade && config::sunshine.password_hash_scheme != modern_password_hash_scheme) {
      *needs_upgrade = true;
    }

    return verified;
  }

  credential_upgrade_status_e upgrade_user_password_hash(
    const std::string &file,
    const std::string &username,
    const std::string &password
  ) {
    if (!boost::iequals(username, config::sunshine.username)) {
      return credential_upgrade_status_e::save_failed;
    }
    if (config::sunshine.password_hash_scheme == modern_password_hash_scheme) {
      return credential_upgrade_status_e::unchanged;
    }

    BOOST_LOG(info) << "Upgrading stored web credentials to the stronger password KDF";
    if (save_user_creds(file, config::sunshine.username, password) != 0) {
      return credential_upgrade_status_e::save_failed;
    }
#ifdef POLARIS_TESTS
    if (fail_credential_upgrade_reload.load(std::memory_order_relaxed)) {
      return credential_upgrade_status_e::reload_failed;
    }
#endif
    if (reload_user_creds(file) != 0) {
      return credential_upgrade_status_e::reload_failed;
    }
    return credential_upgrade_status_e::upgraded;
  }

#ifdef POLARIS_TESTS
  void set_credential_upgrade_reload_failure_for_tests(bool enabled) {
    fail_credential_upgrade_reload.store(enabled, std::memory_order_relaxed);
  }
#endif

  int create_creds(const std::string &pkey, const std::string &cert) {
    fs::path pkey_path = pkey;
    fs::path cert_path = cert;

    auto creds = crypto::gen_creds("Polaris Gamestream Host"sv, 2048);

    auto pkey_dir = pkey_path;
    auto cert_dir = cert_path;
    pkey_dir.remove_filename();
    cert_dir.remove_filename();

    std::error_code err_code {};
    fs::create_directories(pkey_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << pkey_dir << "] :"sv << err_code.message();
      return -1;
    }

    fs::create_directories(cert_dir, err_code);
    if (err_code) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << cert_dir << "] :"sv << err_code.message();
      return -1;
    }

    if (file_handler::write_file(pkey.c_str(), creds.pkey)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.pkey << ']';
      return -1;
    }

    if (file_handler::write_file(cert.c_str(), creds.x509)) {
      BOOST_LOG(error) << "Couldn't open ["sv << config::nvhttp.cert << ']';
      return -1;
    }

    fs::permissions(pkey_path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.pkey << "] :"sv << err_code.message();
      return -1;
    }

    fs::permissions(cert_path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read | fs::perms::owner_write, fs::perm_options::replace, err_code);

    if (err_code) {
      BOOST_LOG(error) << "Couldn't change permissions of ["sv << config::nvhttp.cert << "] :"sv << err_code.message();
      return -1;
    }

    return 0;
  }

  namespace redirect {
    bool follow(
      const std::string &initial_url,
      const download_url_validator_t &url_allowed,
      const request_t &request
    ) {
      constexpr std::size_t maximum_redirects = 5;
      if (!url_allowed || !request || !url_allowed(initial_url)) {
        return false;
      }

      std::string current_url = initial_url;
      std::size_t redirect_count = 0;
      while (true) {
        const auto result = request(current_url);
        if (!result.transport_ok) {
          return false;
        }
        if (result.response_code >= 200 && result.response_code < 300) {
          return true;
        }
        if (result.response_code < 300 || result.response_code >= 400 ||
            !result.redirect_url || result.redirect_url->empty()) {
          return false;
        }
        if (redirect_count >= maximum_redirects) {
          BOOST_LOG(warning) << "Download exceeded the five-redirect limit";
          return false;
        }
        if (!url_allowed(*result.redirect_url)) {
          BOOST_LOG(warning) << "Refused download redirect to non-allowlisted host ["sv
                             << url_get_host(*result.redirect_url) << ']';
          return false;
        }

        current_url = *result.redirect_url;
        ++redirect_count;
      }
    }
  }  // namespace redirect

  bool download_file(
    const std::string &url,
    const std::string &file,
    const download_url_validator_t &url_allowed,
    long ssl_version
  ) {
    if (!url_allowed || !url_allowed(url)) {
      BOOST_LOG(warning) << "Refused non-allowlisted download URL host ["sv << url_get_host(url) << ']';
      return false;
    }

    // sonar complains about weak ssl and tls versions; however sonar cannot detect the fix
    CURL *curl = curl_easy_init();  // NOSONAR
    if (!curl) {
      BOOST_LOG(error) << "Couldn't create CURL instance";
      return false;
    }

    if (std::string file_dir = file_handler::get_parent_directory(file); !file_handler::make_directory(file_dir)) {
      BOOST_LOG(error) << "Couldn't create directory ["sv << file_dir << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, ssl_version);  // NOSONAR
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
#ifdef _WIN32
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif

    const bool downloaded = redirect::follow(
      url,
      url_allowed,
      [&](const std::string &current_url) {
        FILE *fp = fopen(file.c_str(), "wb");
        if (!fp) {
          BOOST_LOG(error) << "Couldn't open ["sv << file << ']';
          return redirect::hop_result_t {false, 0, std::nullopt};
        }

        curl_easy_setopt(curl, CURLOPT_URL, current_url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        const CURLcode result = curl_easy_perform(curl);
        long response_code = 0;
        char *redirect_url = nullptr;
        if (result == CURLE_OK) {
          curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
          curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect_url);
        }
        fclose(fp);

        if (result != CURLE_OK) {
          BOOST_LOG(error) << "Couldn't download from host ["sv << url_get_host(current_url)
                           << "], code:"sv << result;
        }
        return redirect::hop_result_t {
          result == CURLE_OK,
          response_code,
          redirect_url && *redirect_url ? std::optional<std::string> {redirect_url} : std::nullopt,
        };
      }
    );
    curl_easy_cleanup(curl);

    if (!downloaded) {
      std::error_code err_code;
      fs::remove(file, err_code);
      if (err_code) {
        BOOST_LOG(warning) << "Couldn't remove failed download ["sv << file << "] :"sv << err_code.message();
      }
      return false;
    }

    return true;
  }

  std::string url_escape(const std::string &url) {
    char *string = curl_easy_escape(nullptr, url.c_str(), static_cast<int>(url.length()));
    std::string result(string);
    curl_free(string);
    return result;
  }

  std::string url_get_host(const std::string &url) {
    CURLU *curlu = curl_url();
    if (!curlu || curl_url_set(curlu, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
      if (curlu) curl_url_cleanup(curlu);
      return "";
    }
    char *host = nullptr;
    if (curl_url_get(curlu, CURLUPART_HOST, &host, 0) != CURLUE_OK) {
      curl_url_cleanup(curlu);
      return "";
    }
    std::string result(host);
    curl_free(host);
    curl_url_cleanup(curlu);
    return result;
  }

  bool url_is_https_host(std::string_view url, std::string_view expected_host) {
    if (url.empty() || expected_host.empty()) {
      return false;
    }

    CURLU *curlu = curl_url();
    const std::string owned_url {url};
    if (!curlu || curl_url_set(curlu, CURLUPART_URL, owned_url.c_str(), 0) != CURLUE_OK) {
      if (curlu) curl_url_cleanup(curlu);
      return false;
    }

    char *scheme = nullptr;
    char *host = nullptr;
    char *forbidden = nullptr;
    const bool valid =
      curl_url_get(curlu, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
      curl_url_get(curlu, CURLUPART_HOST, &host, 0) == CURLUE_OK &&
      boost::iequals(scheme, "https") && boost::iequals(host, expected_host) &&
      curl_url_get(curlu, CURLUPART_PORT, &forbidden, 0) != CURLUE_OK &&
      curl_url_get(curlu, CURLUPART_USER, &forbidden, 0) != CURLUE_OK &&
      curl_url_get(curlu, CURLUPART_PASSWORD, &forbidden, 0) != CURLUE_OK;

    if (scheme) curl_free(scheme);
    if (host) curl_free(host);
    if (forbidden) curl_free(forbidden);
    curl_url_cleanup(curlu);
    return valid;
  }
}  // namespace http
