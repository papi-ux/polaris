/**
 * @file src/httpcommon.h
 * @brief Declarations for common HTTP.
 */
#pragma once

// lib includes
#include <curl/curl.h>

// local includes
#include "network.h"
#include "thread_safe.h"
#include "uuid.h"

namespace http {

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
#endif
  bool download_file(const std::string &url, const std::string &file, long ssl_version = CURL_SSLVERSION_TLSv1_2);
  std::string url_escape(const std::string &url);
  std::string url_get_host(const std::string &url);

  extern std::string unique_id;
  extern uuid_util::uuid_t uuid;
  extern net::net_e origin_web_ui_allowed;

}  // namespace http
