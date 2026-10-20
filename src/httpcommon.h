/**
 * @file src/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

// lib includes
#include <curl/curl.h>

// standard includes
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#ifdef POLARIS_TESTS
  #include <vector>
#endif

// local includes
#include "network.h"
#include "thread_safe.h"
#include "uuid.h"

namespace http {

  using download_url_validator_t = std::function<bool(std::string_view)>;

  namespace redirect {
    struct hop_result_t {
      bool transport_ok;
      long response_code;
      std::optional<std::string> redirect_url;
    };

    using request_t = std::function<hop_result_t(const std::string &url)>;

    /**
     * Follow at most five redirects, validating each target before requesting it.
     */
    bool follow(
      const std::string &initial_url,
      const download_url_validator_t &url_allowed,
      const request_t &request
    );
  }  // namespace redirect

  int init();
  int create_creds(const std::string &pkey, const std::string &cert);
  int save_user_creds(
    const std::string &file,
    const std::string &username,
    const std::string &password,
    bool run_our_mouth = false
  );

  enum class credential_upgrade_status_e : int {
    save_failed = -1,
    reload_failed = -2,
    unchanged = 0,
    upgraded = 1,
  };

  int reload_user_creds(const std::string &file);
  bool verify_user_password(const std::string &username, const std::string &password, bool *needs_upgrade = nullptr);
  credential_upgrade_status_e upgrade_user_password_hash(
    const std::string &file,
    const std::string &username,
    const std::string &password
  );

#ifdef POLARIS_TESTS
  void set_credential_upgrade_reload_failure_for_tests(bool enabled);

  /**
   * @brief Test-only transport override for download_file().
   *
   * Lets a test reach an owned loopback TLS listener through the production download path while the
   * URL keeps its own host for SNI and certificate verification, the caller's allowlist and the validated
   * redirect loop stay as they are, and peer and host-name verification stay strict. Only a bundle the
   * test owns is trusted, only the one fixture host bypasses a proxy environment, and the bounds exist so
   * a fixture can never hang a test process. The override is scoped to the installing thread and removed
   * by the guard's destructor; nothing here exists in the product build.
   */
  struct download_transport_override_t {
    /// CURLOPT_CAINFO: the fixture's own certificate file; empty keeps libcurl's default trust store.
    std::string ca_bundle_path;
    /// CURLOPT_CONNECT_TO entries, each "HOST:PORT:CONNECT-TO-HOST:CONNECT-TO-PORT" naming an owned loopback
    /// listener, for example "download.test:443:127.0.0.1:38421". A control may map a forbidden host to a
    /// second owned listener so that a refusal is proven to happen before any connection, never by DNS.
    std::vector<std::string> connect_to;
    /// CURLOPT_NOPROXY: exactly the fixture hosts, comma separated, so no proxy environment tunnels them.
    std::string no_proxy;
    /// CURLOPT_CONNECTTIMEOUT_MS when positive.
    long connect_timeout_ms = 0;
    /// CURLOPT_TIMEOUT_MS when positive.
    long total_timeout_ms = 0;
  };

  class scoped_download_transport_override_t {
  public:
    explicit scoped_download_transport_override_t(download_transport_override_t transport_override);
    ~scoped_download_transport_override_t();
    scoped_download_transport_override_t(const scoped_download_transport_override_t &) = delete;
    scoped_download_transport_override_t &operator=(const scoped_download_transport_override_t &) = delete;

  private:
    download_transport_override_t value_;
    const download_transport_override_t *previous_;
  };
#endif
  bool download_file(
    const std::string &url,
    const std::string &file,
    const download_url_validator_t &url_allowed,
    long ssl_version = CURL_SSLVERSION_TLSv1_2
  );
  std::string url_escape(const std::string &url);
  std::string url_get_host(const std::string &url);
  bool url_is_https_host(std::string_view url, std::string_view expected_host);

  extern std::string unique_id;
  extern uuid_util::uuid_t uuid;
  extern net::net_e origin_web_ui_allowed;

}  // namespace http
