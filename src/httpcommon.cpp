/**
 * @file src/httpcommon.cpp
 * @brief Definitions for common HTTP.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <filesystem>
#include <fstream>
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

    bool restrict_credentials_file_permissions(const fs::path &path) {
      std::error_code err_code {};
      fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, err_code);
      if (err_code) {
        BOOST_LOG(error) << "Couldn't change permissions of ["sv << path << "] :"sv << err_code.message();
        return false;
      }
      return true;
    }

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

    int persist_credentials_json(const fs::path &path, const nlohmann::json &payload) {
      try {
        std::ofstream out(path);
        out << payload.dump(4);
      } catch (std::exception &e) {
        BOOST_LOG(error) << "error writing to the credentials file, perhaps try this again as an administrator? Details: "sv << e.what();
        return -1;
      }

      return restrict_credentials_file_permissions(path) ? 0 : -1;
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
    nlohmann::json outputTree;

    if (fs::exists(file)) {
      try {
        std::ifstream in(file);
        in >> outputTree;
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Couldn't read user credentials: "sv << e.what();
        return -1;
      }
    }

    auto salt = crypto::rand_alphabet(16);
    std::string password_hash;
    if (!hash_password_for_scheme(password, salt, modern_password_hash_scheme, password_hash)) {
      BOOST_LOG(error) << "Couldn't derive password hash with the configured KDF";
      return -1;
    }
    outputTree["username"] = username;
    outputTree["salt"] = salt;
    outputTree["password_scheme"] = modern_password_hash_scheme;
    outputTree["password"] = password_hash;
    if (persist_credentials_json(file, outputTree) != 0) {
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
      // Save it back to the credentials file
      try {
        nlohmann::json outputTree;
        if (fs::exists(file)) {
          std::ifstream in(file);
          in >> outputTree;
        }
        outputTree["api_key"] = config::sunshine.api_key;
        if (persist_credentials_json(file, outputTree) != 0) {
          return -1;
        }
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Failed to save auto-generated API key: "sv << e.what();
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

  int upgrade_user_password_hash(const std::string &file, const std::string &username, const std::string &password) {
    if (!boost::iequals(username, config::sunshine.username)) {
      return -1;
    }
    if (config::sunshine.password_hash_scheme == modern_password_hash_scheme) {
      return 0;
    }

    BOOST_LOG(info) << "Upgrading stored web credentials to the stronger password KDF";
    if (save_user_creds(file, config::sunshine.username, password) != 0) {
      return -1;
    }
    return reload_user_creds(file);
  }

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

  bool download_file(const std::string &url, const std::string &file, long ssl_version) {
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

    FILE *fp = fopen(file.c_str(), "wb");
    if (!fp) {
      BOOST_LOG(error) << "Couldn't open ["sv << file << ']';
      curl_easy_cleanup(curl);
      return false;
    }

    curl_easy_setopt(curl, CURLOPT_SSLVERSION, ssl_version);  // NOSONAR
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
#ifdef _WIN32
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
    CURLcode result = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (result != CURLE_OK) {
      BOOST_LOG(error) << "Couldn't download ["sv << url << ", code:" << result << ']';
    }
    curl_easy_cleanup(curl);
    fclose(fp);

    const bool http_ok = response_code >= 200 && response_code < 300;
    if (result != CURLE_OK || !http_ok) {
      if (!http_ok) {
        BOOST_LOG(error) << "Download returned unexpected HTTP status for ["sv << url << "]: "sv << response_code;
      }
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
    curl_url_set(curlu, CURLUPART_URL, url.c_str(), static_cast<unsigned int>(url.length()));
    char *host;
    if (curl_url_get(curlu, CURLUPART_HOST, &host, 0) != CURLUE_OK) {
      curl_url_cleanup(curlu);
      return "";
    }
    std::string result(host);
    curl_free(host);
    curl_url_cleanup(curlu);
    return result;
  }
}  // namespace http
