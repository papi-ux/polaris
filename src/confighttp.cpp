/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTPS server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <regex>
#include <future>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <numeric>
#include <algorithm>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "confighttp.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "stream_recorder.h"
#include "stream_stats.h"
#include "utility.h"
#include "uuid.h"
#include "wol.h"
#include "client_profiles.h"
#include "device_db.h"
#include "ai_optimizer.h"
#include "game_classifier.h"

#include <curl/curl.h>

#ifdef _WIN32
  #include "platform/windows/utils.h"
  #include <Windows.h>
#elif __linux__
  #include <unistd.h>
#elif __APPLE__
  #include <mach-o/dyld.h>
#endif

#ifdef __linux__
  #include "platform/linux/virtual_display.h"
  #include "platform/linux/session_manager.h"
  #include "platform/linux/cage_display_router.h"
#endif

using namespace std::literals;

namespace confighttp {
  namespace fs = std::filesystem;

  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Keep the base enum for client operations.
  enum class op_e {
    ADD,    ///< Add client
    REMOVE  ///< Remove client
  };

  // SESSION COOKIE
  std::string sessionCookie;
  static std::chrono::time_point<std::chrono::steady_clock> cookie_creation_time;

  // CSRF TOKEN - generated at server startup
  static std::string csrfToken;

  // RATE LIMITING for login endpoint
  // Maps IP address -> (failed_attempts, last_failure_time)
  static std::unordered_map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> login_rate_limits;
  static std::mutex rate_limit_mutex;
  constexpr int MAX_LOGIN_ATTEMPTS = 5;
  constexpr auto LOGIN_BLOCK_DURATION = std::chrono::seconds(60);

  namespace {
    std::optional<fs::path> executable_dir() {
#ifdef _WIN32
      wchar_t path[MAX_PATH];
      const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
      if (len == 0 || len == MAX_PATH) {
        return std::nullopt;
      }
      return fs::path(path).parent_path();
#elif __linux__
      std::array<char, 4096> path {};
      const auto len = readlink("/proc/self/exe", path.data(), path.size() - 1);
      if (len <= 0) {
        return std::nullopt;
      }
      path[len] = '\0';
      return fs::path(path.data()).parent_path();
#elif __APPLE__
      uint32_t size = 0;
      _NSGetExecutablePath(nullptr, &size);
      std::string buffer(size, '\0');
      if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
      }
      return fs::path(buffer.c_str()).parent_path();
#else
      return std::nullopt;
#endif
    }

    fs::path resolve_web_asset_path(const fs::path &relative_path) {
      const fs::path installed_path = fs::path(POLARIS_ASSETS_DIR) / "web" / relative_path;
      if (fs::exists(installed_path)) {
        return installed_path;
      }

      const auto exe_dir = executable_dir();
      if (exe_dir) {
        const fs::path local_build_path = *exe_dir / "assets" / "web" / relative_path;
        if (fs::exists(local_build_path)) {
          return local_build_path;
        }
      }

      return installed_path;
    }
  }  // namespace

  /**
   * @brief Log the request details.
   * @param request The HTTP request object.
   */
  void print_req(const req_https_t &request) {
    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;
    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << (name == "Authorization" ? "CREDENTIALS REDACTED" : val);
    }
    BOOST_LOG(debug) << " [--] "sv;
    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }
    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(resp_https_t response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Send a 401 Unauthorized response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void send_unauthorized(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(debug) << "Web UI: ["sv << address << "] -- not authorized"sv;
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_unauthorized;
    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = "Unauthorized";
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';"}
    };
    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a redirect response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param path The path to redirect to.
   */
  void send_redirect(resp_https_t response, req_https_t request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- redirecting"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Location", path},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';"}
    };
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  /**
   * @brief Retrieve the value of a key from a cookie string.
   * @param cookieString The cookie header string.
   * @param key The key to search.
   * @return The value if found, empty string otherwise.
   */
  std::string getCookieValue(const std::string& cookieString, const std::string& key) {
    std::string keyWithEqual = key + "=";
    std::size_t startPos = cookieString.find(keyWithEqual);
    if (startPos == std::string::npos)
      return "";
    startPos += keyWithEqual.length();
    std::size_t endPos = cookieString.find(";", startPos);
    if (endPos == std::string::npos)
      return cookieString.substr(startPos);
    return cookieString.substr(startPos, endPos - startPos);
  }

  /**
   * @brief Check if the IP origin is allowed.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return True if allowed, false otherwise.
   */
  bool checkIPOrigin(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);
    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }
    return true;
  }

  /**
   * @brief Validate the CSRF token on a mutating request (POST/PUT/DELETE).
   * @param request The HTTP request object.
   * @return True if the CSRF token is valid or the request method is GET/HEAD, false otherwise.
   */
  bool validateCsrf(const req_https_t &request) {
    // Only enforce on mutating methods
    if (request->method == "GET" || request->method == "HEAD" || request->method == "OPTIONS") {
      return true;
    }
    auto it = request->header.find("X-CSRF-Token");
    if (it == request->header.end() || it->second != csrfToken) {
      return false;
    }
    return true;
  }

  /**
   * @brief Check if an IP is rate-limited for login attempts.
   * @param ip The IP address to check.
   * @return True if the IP is blocked, false otherwise.
   */
  bool isLoginRateLimited(const std::string &ip) {
    std::lock_guard<std::mutex> lock(rate_limit_mutex);
    auto now = std::chrono::steady_clock::now();

    // Clean up expired entries while we're here
    for (auto it = login_rate_limits.begin(); it != login_rate_limits.end();) {
      if (now - it->second.second > LOGIN_BLOCK_DURATION) {
        it = login_rate_limits.erase(it);
      } else {
        ++it;
      }
    }

    auto it = login_rate_limits.find(ip);
    if (it == login_rate_limits.end()) {
      return false;
    }
    return it->second.first >= MAX_LOGIN_ATTEMPTS &&
           (now - it->second.second) <= LOGIN_BLOCK_DURATION;
  }

  /**
   * @brief Record a failed login attempt for an IP.
   * @param ip The IP address.
   */
  void recordLoginFailure(const std::string &ip) {
    std::lock_guard<std::mutex> lock(rate_limit_mutex);
    auto now = std::chrono::steady_clock::now();
    auto &entry = login_rate_limits[ip];
    // If the previous block window expired, reset the counter
    if (now - entry.second > LOGIN_BLOCK_DURATION) {
      entry.first = 0;
    }
    entry.first++;
    entry.second = now;
  }

  /**
   * @brief Clear failed login attempts for an IP after a successful login.
   * @param ip The IP address.
   */
  void clearLoginFailures(const std::string &ip) {
    std::lock_guard<std::mutex> lock(rate_limit_mutex);
    login_rate_limits.erase(ip);
  }

  /**
   * @brief Authenticate the request.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param needsRedirect Whether to redirect on failure.
   * @return True if authenticated, false otherwise.
   *
   * This function uses session cookies (if set) and ensures they have not expired.
   * It also supports API key authentication via the Authorization: Bearer header.
   */
  bool authenticate(resp_https_t response, req_https_t request, bool needsRedirect = false) {
    if (!checkIPOrigin(response, request))
      return false;
    // If credentials not set, redirect to welcome.
    if (config::sunshine.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }
    // Guard: on failure, redirect if requested.
    auto fg = util::fail_guard([&]() {
      if (needsRedirect) {
        std::string redir_path = "/login?redir=.";
        redir_path += request->path;
        send_redirect(response, request, redir_path.c_str());
      } else {
        send_unauthorized(response, request);
      }
    });

    // Check for API key authentication via Bearer token
    auto auth_header = request->header.find("authorization");
    if (auth_header != request->header.end()) {
      auto &auth_value = auth_header->second;
      if (auth_value.substr(0, 7) == "Bearer " && !config::sunshine.api_key.empty()) {
        auto provided_key = auth_value.substr(7);
        if (provided_key == config::sunshine.api_key) {
          fg.disable();
          return true;
        }
      }
    }

    // Session cookie authentication
    if (sessionCookie.empty())
      return false;
    // Check for expiry
    if (std::chrono::steady_clock::now() - cookie_creation_time > SESSION_EXPIRE_DURATION) {
      sessionCookie.clear();
      return false;
    }
    auto cookies = request->header.find("cookie");
    if (cookies == request->header.end())
      return false;
    auto authCookie = getCookieValue(cookies->second, "auth");
    if (authCookie.empty() ||
        util::hex(crypto::hash(authCookie + config::sunshine.salt)).to_string() != sessionCookie)
      return false;
    fg.disable();
    return true;
  }

  /**
   * @brief Send a 404 Not Found response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void not_found(resp_https_t response, [[maybe_unused]] req_https_t request) {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_not_found;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["error"] = "Not Found";
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a 400 Bad Request response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message.
   */
  void bad_request(resp_https_t response, [[maybe_unused]] req_https_t request, const std::string &error_message = "Bad Request") {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_bad_request;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["status"] = false;
    tree["error"] = error_message;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }


  /**
   * @brief Validate the request content type and send bad request when mismatch.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param contentType The required content type.
   */
  bool validateContentType(resp_https_t response, req_https_t request, const std::string_view& contentType) {
    auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      bad_request(response, request, "Content type not provided");
      return false;
    }

    // Extract the media type part before any parameters (e.g., charset)
    std::string actualContentType = requestContentType->second;
    size_t semicolonPos = actualContentType.find(';');
    if (semicolonPos != std::string::npos) {
      actualContentType = actualContentType.substr(0, semicolonPos);
    }

    // Trim whitespace and convert to lowercase for case-insensitive comparison
    boost::algorithm::trim(actualContentType);
    boost::algorithm::to_lower(actualContentType);

    std::string expectedContentType(contentType);
    boost::algorithm::to_lower(expectedContentType);

    if (actualContentType != expectedContentType) {
      bad_request(response, request, "Content type mismatch");
      return false;
    }
    return true;

    return true;
  }

  /**
   * @brief Serve the SPA index.html for any non-API, non-asset route.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * With hash-based routing, the Vue Router handles all page navigation
   * client-side via the URL hash. The server only needs to serve index.html
   * for the root path and any direct page access (for backward compatibility).
   */
  void getSpaPage(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(resolve_web_asset_path("index.html").string().c_str());
    // Inject CSRF token meta tag into the HTML head
    std::string csrfMeta = "<meta name=\"csrf-token\" content=\"" + csrfToken + "\">";
    auto headPos = content.find("</head>");
    if (headPos != std::string::npos) {
      content.insert(headPos, csrfMeta + "\n");
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the favicon image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getFaviconImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(resolve_web_asset_path("images/polaris.ico"), std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/x-icon");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the Apollo logo image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @todo combine function with getFaviconImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getApolloLogoImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(resolve_web_asset_path("images/logo-polaris-45.png"), std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(base, query);
    return *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset from the node_modules directory.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getNodeModules(resp_https_t response, req_https_t request) {
    print_req(request);

    fs::path webDirPath = resolve_web_asset_path("");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if file does not exist or is outside the web directory
    if (!isChildPath(filePath, webDirPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      bad_request(response, request);
      return;
    }

    if (!fs::exists(filePath)) {
      not_found(response, request);
      return;
    }

    auto relPath = fs::relative(filePath, webDirPath);
    // get the mime type from the file extension mime_types map
    // remove the leading period from the extension
    auto mimeType = mime_types.find(relPath.extension().string().substr(1));
    if (mimeType == mime_types.end()) {
      bad_request(response, request);
      return;
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';");
    std::ifstream in(filePath.string(), std::ios::binary);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the list of available applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps| GET| null}
   */
  void getApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);

      file_tree["current_app"] = proc::proc.get_running_app_uuid();
      file_tree["host_uuid"] = http::unique_id;
      file_tree["host_name"] = config::nvhttp.sunshine_name;

      send_response(response, file_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Save an application. To save a new application the UUID must be empty.
   *        To update an existing application, you must provide the current UUID of the application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "name": "Application Name",
   *   "output": "Log Output Path",
   *   "cmd": "Command to run the application",
   *   "exclude-global-prep-cmd": false,
   *   "elevated": false,
   *   "auto-detach": true,
   *   "wait-all": true,
   *   "exit-timeout": 5,
   *   "prep-cmd": [
   *     {
   *       "do": "Command to prepare",
   *       "undo": "Command to undo preparation",
   *       "elevated": false
   *     }
   *   ],
   *   "detached": [
   *     "Detached command"
   *   ],
   *   "image-path": "Full path to the application image. Must be a png file.",
   *   "uuid": "aaaa-bbbb"
   * }
   * @endcode
   *
   * @api_examples{/api/apps| POST| {"name":"Hello, World!","uuid": "aaaa-bbbb"}}
   */
  void saveApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    BOOST_LOG(info) << config::stream.file_apps;
    try {
      // TODO: Input Validation

      // Read the input JSON from the request body.
      nlohmann::json inputTree = nlohmann::json::parse(ss.str());

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Migrate/merge the new app into the file tree.
      proc::migrate_apps(&fileTree, &inputTree);

      // Write the updated file tree back to disk.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      proc::refresh(config::stream.file_apps);

      // Prepare and send the output response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Close the currently running application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/close| POST| null}
   */
  void closeApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Reorder applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/reorder| POST| {"order": ["aaaa-bbbb", "cccc-dddd"]}}
   */
  void reorderApps(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();

      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Get the desired order of UUIDs from the request.
      if (!input_tree.contains("order") || !input_tree["order"].is_array()) {
        throw std::runtime_error("Missing or invalid 'order' array in request body");
      }
      const auto& order_uuids_json = input_tree["order"];

      // Get the original apps array from the fileTree.
      // Default to an empty array if "apps" key is missing or if it's present but not an array (after logging an error).
      nlohmann::json original_apps_list = nlohmann::json::array();
      if (fileTree.contains("apps")) {
        if (fileTree["apps"].is_array()) {
          original_apps_list = fileTree["apps"];
        } else {
          // "apps" key exists but is not an array. This is a malformed state.
          BOOST_LOG(error) << "ReorderApps: 'apps' key in apps configuration file ('" << config::stream.file_apps
                           << "') is present but not an array.";
          throw std::runtime_error("'apps' in file is not an array, cannot reorder.");
        }
      } else {
        // "apps" key is missing. Treat as an empty list. Reordering an empty list is valid.
        BOOST_LOG(debug) << "ReorderApps: 'apps' key missing in apps configuration file ('" << config::stream.file_apps
                         << "'). Treating as an empty list for reordering.";
        // original_apps_list is already an empty array, so no specific action needed here.
      }

      nlohmann::json reordered_apps_list = nlohmann::json::array();
      std::vector<bool> item_moved(original_apps_list.size(), false);

      // Phase 1: Place apps according to the 'order' array from the request.
      // Iterate through the desired order of UUIDs.
      for (const auto& uuid_json_value : order_uuids_json) {
        if (!uuid_json_value.is_string()) {
          BOOST_LOG(warning) << "ReorderApps: Encountered a non-string UUID in the 'order' array. Skipping this entry.";
          continue;
        }
        std::string target_uuid = uuid_json_value.get<std::string>();
        bool found_match_for_ordered_uuid = false;

        // Find the first unmoved app in the original list that matches the current target_uuid.
        for (size_t i = 0; i < original_apps_list.size(); ++i) {
          if (item_moved[i]) {
            continue; // This specific app object has already been placed.
          }

          const auto& app_item = original_apps_list[i];
          // Ensure the app item is an object and has a UUID to match against.
          if (app_item.is_object() && app_item.contains("uuid") && app_item["uuid"].is_string()) {
            if (app_item["uuid"].get<std::string>() == target_uuid) {
              reordered_apps_list.push_back(app_item); // Add the found app object to the new list.
              item_moved[i] = true;                    // Mark this specific object as moved.
              found_match_for_ordered_uuid = true;
              break; // Found an app for this UUID, move to the next UUID in the 'order' array.
            }
          }
        }

        if (!found_match_for_ordered_uuid) {
          // This means a UUID specified in the 'order' array was not found in the original_apps_list
          // among the currently available (unmoved) app objects.
          // Per instruction "If the uuid is missing from the original json file, omit it."
          BOOST_LOG(debug) << "ReorderApps: UUID '" << target_uuid << "' from 'order' array not found in available apps list or its matching app was already processed. Omitting.";
        }
      }

      // Phase 2: Append any remaining apps from the original list that were not explicitly ordered.
      // These are app objects that were not marked 'item_moved' in Phase 1.
      for (size_t i = 0; i < original_apps_list.size(); ++i) {
        if (!item_moved[i]) {
          reordered_apps_list.push_back(original_apps_list[i]);
        }
      }

      // Update the fileTree with the new, reordered list of apps.
      fileTree["apps"] = reordered_apps_list;

      // Write the modified fileTree back to the apps configuration file.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));

      // Notify relevant parts of the system that the apps configuration has changed.
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "ReorderApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Delete an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/delete | POST| { uuid: 'aaaa-bbbb' }}
   */
  void deleteApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      // Check for required uuid field in body
      if (!input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      auto uuid = input_tree["uuid"].get<std::string>();

      // Read the apps file into a nlohmann::json object.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Remove any app with the matching uuid directly from the "apps" array.
      if (fileTree.contains("apps") && fileTree["apps"].is_array()) {
        auto& apps = fileTree["apps"];
        apps.erase(
          std::remove_if(apps.begin(), apps.end(), [&uuid](const nlohmann::json& app) {
            return app.value("uuid", "") == uuid;
          }),
          apps.end()
        );
      }

      // Write the updated JSON back to the file.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      proc::refresh(config::stream.file_apps);

      // Prepare and send the response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  // ---- Game Scanner API ----

  // Steam genre cache: appid -> genre strings
  static std::unordered_map<std::string, std::vector<std::string>> steam_genre_cache;
  static std::mutex genre_cache_mutex;

  static std::filesystem::path genre_cache_path() {
    return platf::appdata() / "steam_genres_cache.json";
  }

  static void load_genre_cache() {
    std::lock_guard<std::mutex> lock(genre_cache_mutex);
    if (!steam_genre_cache.empty()) return;  // already loaded
    std::ifstream file(genre_cache_path());
    if (!file.is_open()) return;
    try {
      auto root = nlohmann::json::parse(file);
      for (auto &[appid, genres] : root.items()) {
        std::vector<std::string> vec;
        for (const auto &g : genres) {
          if (g.is_string()) vec.push_back(g.get<std::string>());
        }
        steam_genre_cache[appid] = vec;
      }
    } catch (...) {}
  }

  static void save_genre_cache() {
    std::lock_guard<std::mutex> lock(genre_cache_mutex);
    nlohmann::json root = nlohmann::json::object();
    for (const auto &[appid, genres] : steam_genre_cache) {
      root[appid] = genres;
    }
    std::ofstream file(genre_cache_path());
    if (file.is_open()) file << root.dump(2);
  }

  static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, std::string *out) {
    out->append(static_cast<char *>(contents), size * nmemb);
    return size * nmemb;
  }

  /**
   * @brief Fetch genre tags for a Steam appid from the Steam Store API.
   * @return Vector of genre description strings, empty on failure.
   */
  static std::vector<std::string> fetch_steam_genres(const std::string &appid) {
    // Check cache first
    {
      std::lock_guard<std::mutex> lock(genre_cache_mutex);
      auto it = steam_genre_cache.find(appid);
      if (it != steam_genre_cache.end()) return it->second;
    }

    std::string url = "https://store.steampowered.com/api/appdetails?appids=" + appid + "&filters=genres";

    CURL *curl = curl_easy_init();
    if (!curl) return {};

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Polaris/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    std::vector<std::string> genres;

    if (res != CURLE_OK) return genres;

    try {
      auto data = nlohmann::json::parse(response);
      if (data.contains(appid) &&
          data[appid].value("success", false) &&
          data[appid].contains("data") &&
          data[appid]["data"].contains("genres")) {
        for (const auto &g : data[appid]["data"]["genres"]) {
          if (g.contains("description")) {
            genres.push_back(g["description"].get<std::string>());
          }
        }
      }
    } catch (...) {}

    // Cache the result (even if empty, to avoid re-fetching failures)
    {
      std::lock_guard<std::mutex> lock(genre_cache_mutex);
      steam_genre_cache[appid] = genres;
    }

    return genres;
  }

  /**
   * @brief Parse a Valve ACF (KeyValues) file for top-level AppState fields.
   */
  static std::unordered_map<std::string, std::string> parse_acf(const std::string &path) {
    std::unordered_map<std::string, std::string> fields;
    std::ifstream file(path);
    if (!file.is_open()) return fields;

    std::string line;
    int depth = 0;
    while (std::getline(file, line)) {
      // Trim
      auto start = line.find_first_not_of(" \t");
      if (start == std::string::npos) continue;
      line = line.substr(start);

      if (line == "{") { depth++; continue; }
      if (line == "}") { depth--; continue; }

      // Only parse top-level key-value pairs (depth == 1, inside AppState)
      if (depth != 1) continue;

      // Parse "key"  "value" format
      if (line.size() < 5 || line[0] != '"') continue;
      auto end_key = line.find('"', 1);
      if (end_key == std::string::npos) continue;
      std::string key = line.substr(1, end_key - 1);

      auto start_val = line.find('"', end_key + 1);
      if (start_val == std::string::npos) continue;
      auto end_val = line.find('"', start_val + 1);
      if (end_val == std::string::npos) continue;
      std::string val = line.substr(start_val + 1, end_val - start_val - 1);

      fields[key] = val;
    }
    return fields;
  }

  /**
   * @brief Scan for installed Steam and Lutris games.
   */
  void scanGames(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output;
    nlohmann::json steam_games = nlohmann::json::array();

    // Get existing apps to check for duplicates
    std::set<std::string> existing_cmds;
    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      auto apps_tree = nlohmann::json::parse(content);
      if (apps_tree.contains("apps") && apps_tree["apps"].is_array()) {
        for (const auto &app : apps_tree["apps"]) {
          if (app.contains("detached") && app["detached"].is_array()) {
            for (const auto &cmd : app["detached"]) {
              if (cmd.is_string()) existing_cmds.insert(cmd.get<std::string>());
            }
          }
          if (app.contains("cmd") && app["cmd"].is_string()) {
            existing_cmds.insert(app["cmd"].get<std::string>());
          }
        }
      }
    } catch (...) {}

    // Scan Steam appmanifest files
    std::vector<std::string> steam_paths;
    std::set<std::string> seen_appids;
    const char *home = std::getenv("HOME");
    if (home) {
      steam_paths.push_back(std::string(home) + "/.steam/steam/steamapps");
      steam_paths.push_back(std::string(home) + "/.local/share/Steam/steamapps");
    }

    for (const auto &steam_path : steam_paths) {
      if (!std::filesystem::exists(steam_path)) continue;

      for (const auto &entry : std::filesystem::directory_iterator(steam_path)) {
        if (!entry.is_regular_file()) continue;
        auto fname = entry.path().filename().string();
        if (fname.rfind("appmanifest_", 0) != 0 || fname.find(".acf") == std::string::npos) continue;

        auto fields = parse_acf(entry.path().string());
        if (fields.empty() || fields["name"].empty() || fields["appid"].empty()) continue;

        // StateFlags 4 = fully installed
        int state = 0;
        try { state = std::stoi(fields["StateFlags"]); } catch (...) {}
        if (state != 4) continue;

        // Skip duplicates (symlinked library paths)
        if (seen_appids.count(fields["appid"])) continue;
        seen_appids.insert(fields["appid"]);

        // Skip tools/redistributables (appid < 100000 or name contains "Redistributable" or "Proton")
        int appid_int = 0;
        try { appid_int = std::stoi(fields["appid"]); } catch (...) {}
        if (appid_int < 10000) continue;
        if (fields["name"].find("Redistributable") != std::string::npos) continue;
        if (fields["name"].find("Proton") != std::string::npos) continue;
        if (fields["name"].find("Steam Linux Runtime") != std::string::npos) continue;
        if (fields["name"].find("Steamworks") != std::string::npos) continue;

        std::string launch_cmd = "setsid steam steam://rungameid/" + fields["appid"];
        bool already = existing_cmds.count(launch_cmd) > 0;

        nlohmann::json game;
        game["appid"] = fields["appid"];
        game["name"] = fields["name"];
        game["cover_url"] = "https://steamcdn-a.akamaihd.net/steam/apps/" + fields["appid"] + "/library_600x900_2x.jpg";
        game["cmd"] = launch_cmd;
        game["source"] = "steam";
        game["already_imported"] = already;
        steam_games.push_back(game);
      }
    }

    // Fetch Steam genres and classify each game
    load_genre_cache();
    bool cache_dirty = false;
    for (auto &game : steam_games) {
      std::string appid = game["appid"].get<std::string>();
      auto genres = fetch_steam_genres(appid);
      if (!genres.empty()) cache_dirty = true;

      nlohmann::json genre_arr = nlohmann::json::array();
      for (const auto &g : genres) genre_arr.push_back(g);
      game["genres"] = genre_arr;

      auto cat = game_classifier::classify(game["name"].get<std::string>(), genres);
      game["game_category"] = game_classifier::category_to_string(cat);
    }
    if (cache_dirty) save_genre_cache();

    // Scan Lutris games
    nlohmann::json lutris_games = nlohmann::json::array();
    if (home) {
      std::string lutris_db_path = std::string(home) + "/.local/share/lutris/pga.db";
      std::string lutris_yml_dir = std::string(home) + "/.config/lutris/games/";

      // Scan Lutris YAML game configs
      if (std::filesystem::exists(lutris_yml_dir)) {
        for (const auto &entry : std::filesystem::directory_iterator(lutris_yml_dir)) {
          if (!entry.is_regular_file()) continue;
          auto fname = entry.path().filename().string();
          if (fname.find(".yml") == std::string::npos) continue;

          // Extract slug from filename (e.g., "doom-eternal.yml" -> "doom-eternal")
          auto slug = fname.substr(0, fname.rfind('.'));

          // Read the YAML file to get the game name
          std::ifstream yml_file(entry.path());
          std::string game_name;
          std::string line;
          while (std::getline(yml_file, line)) {
            // Simple YAML parsing — find "name: ..." or "game:" section
            if (line.find("name:") == 0 || line.find("name: ") == 0) {
              game_name = line.substr(line.find(':') + 1);
              // Trim whitespace and quotes
              while (!game_name.empty() && (game_name.front() == ' ' || game_name.front() == '\'' || game_name.front() == '"'))
                game_name.erase(0, 1);
              while (!game_name.empty() && (game_name.back() == ' ' || game_name.back() == '\'' || game_name.back() == '"'))
                game_name.pop_back();
              break;
            }
          }
          if (game_name.empty()) {
            // Use slug as fallback name, replacing hyphens with spaces
            game_name = slug;
            std::replace(game_name.begin(), game_name.end(), '-', ' ');
          }

          std::string launch_cmd = "setsid lutris lutris:rungame/" + slug;
          bool already = existing_cmds.count(launch_cmd) > 0;

          nlohmann::json game;
          game["name"] = game_name;
          game["slug"] = slug;
          game["cmd"] = launch_cmd;
          game["source"] = "lutris";
          game["already_imported"] = already;
          lutris_games.push_back(game);
        }
      }
    }

    // Scan Heroic Games Launcher (GOG + Epic via Legendary)
    nlohmann::json heroic_games = nlohmann::json::array();
    if (home) {
      // Heroic stores installed game info in library JSON files
      std::vector<std::pair<std::string, std::string>> heroic_paths = {
        {std::string(home) + "/.config/heroic/gog_store/installed.json", "gog"},
        {std::string(home) + "/.config/heroic/legendaryConfig/legendary/installed.json", "epic"},
      };

      for (const auto &[path, store] : heroic_paths) {
        if (!std::filesystem::exists(path)) continue;
        try {
          std::ifstream f(path);
          auto data = nlohmann::json::parse(f);

          // Heroic installed.json: { "installed": [ { "app_name": "...", "title": "...", ... } ] }
          // OR for Legendary: object keyed by app_name
          nlohmann::json game_list;
          if (data.contains("installed") && data["installed"].is_array()) {
            game_list = data["installed"];
          } else if (data.is_object()) {
            // Legendary format: { "app_name": { ... }, ... }
            for (auto &[key, val] : data.items()) {
              if (val.is_object()) game_list.push_back(val);
            }
          }

          for (const auto &entry : game_list) {
            std::string app_name = entry.value("app_name", "");
            std::string title = entry.value("title", entry.value("app_name", ""));
            if (title.empty() || app_name.empty()) continue;

            // Skip DLC / tools
            if (entry.contains("is_dlc") && entry["is_dlc"].get<bool>()) continue;

            std::string launch_cmd = "setsid heroic heroic://launch/" + store + "/" + app_name;
            bool already = existing_cmds.count(launch_cmd) > 0;

            nlohmann::json game;
            game["name"] = title;
            game["app_name"] = app_name;
            game["store"] = store;
            game["cmd"] = launch_cmd;
            game["source"] = "heroic";
            game["already_imported"] = already;
            heroic_games.push_back(game);
          }
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Failed to parse Heroic library at " << path << ": " << e.what();
        }
      }

      // Also check Heroic library.json (a combined library cache)
      std::string heroic_lib = std::string(home) + "/.config/heroic/store_cache/gog_library.json";
      std::string heroic_egs = std::string(home) + "/.config/heroic/store_cache/egs_library.json";
      for (const auto &[path, store] : std::vector<std::pair<std::string, std::string>>{
        {heroic_lib, "gog"}, {heroic_egs, "epic"}
      }) {
        if (!std::filesystem::exists(path)) continue;
        try {
          std::ifstream f(path);
          auto data = nlohmann::json::parse(f);
          auto &lib = data.contains("library") ? data["library"] : data;
          if (!lib.is_array()) continue;
          for (const auto &entry : lib) {
            std::string app_name = entry.value("app_name", "");
            std::string title = entry.value("title", entry.value("app_name", ""));
            if (title.empty() || app_name.empty()) continue;
            // Skip if already found from installed.json
            bool dup = false;
            for (const auto &existing : heroic_games) {
              if (existing["app_name"] == app_name) { dup = true; break; }
            }
            if (dup) continue;
            if (!entry.value("is_installed", false)) continue;

            std::string launch_cmd = "setsid heroic heroic://launch/" + store + "/" + app_name;
            bool already = existing_cmds.count(launch_cmd) > 0;

            nlohmann::json game;
            game["name"] = title;
            game["app_name"] = app_name;
            game["store"] = store;
            game["cmd"] = launch_cmd;
            game["source"] = "heroic";
            game["already_imported"] = already;
            heroic_games.push_back(game);
          }
        } catch (...) {}
      }
    }

    // Sort all by name
    std::sort(steam_games.begin(), steam_games.end(),
      [](const nlohmann::json &a, const nlohmann::json &b) {
        return a["name"].get<std::string>() < b["name"].get<std::string>();
      });
    std::sort(lutris_games.begin(), lutris_games.end(),
      [](const nlohmann::json &a, const nlohmann::json &b) {
        return a["name"].get<std::string>() < b["name"].get<std::string>();
      });
    std::sort(heroic_games.begin(), heroic_games.end(),
      [](const nlohmann::json &a, const nlohmann::json &b) {
        return a["name"].get<std::string>() < b["name"].get<std::string>();
      });

    output["steam_games"] = steam_games;
    output["lutris_games"] = lutris_games;
    output["heroic_games"] = heroic_games;
    output["status"] = true;
    send_response(response, output);
  }

  /**
   * @brief Import selected games as Polaris apps.
   */
  void importGames(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = nlohmann::json::parse(ss.str());

      if (!body.contains("games") || !body["games"].is_array()) {
        output["status"] = false;
        output["error"] = "Missing 'games' array";
        send_response(response, output);
        return;
      }

      // Read existing apps file
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      int imported = 0;
      for (const auto &game : body["games"]) {
        std::string name = game.value("name", "");
        std::string source = game.value("source", "steam");
        std::string appid = game.value("appid", "");

        if (name.empty()) continue;

        // Build the app entry
        nlohmann::json app;
        app["name"] = name;
        app["uuid"] = ""; // Will be generated by migrate_apps
        app["cmd"] = "";
        app["auto-detach"] = true;
        app["wait-all"] = true;
        app["exit-timeout"] = 5;
        app["virtual-display"] = true;

        app["source"] = source;

        if (source == "steam" && !appid.empty()) {
          app["detached"] = nlohmann::json::array({ "setsid steam steam://rungameid/" + appid });
          app["steam-appid"] = appid;

          // Download cover art from Steam CDN to local covers directory
          std::string cover_url = "https://steamcdn-a.akamaihd.net/steam/apps/" + appid + "/library_600x900_2x.jpg";
          const std::string coverdir = platf::appdata().string() + "/covers/";
          file_handler::make_directory(coverdir);
          std::string cover_path = coverdir + "steam_" + appid + ".png";
          if (http::download_file(cover_url, cover_path)) {
            app["image-path"] = cover_path;
          } else {
            app["image-path"] = cover_url;  // fallback to URL
          }
        } else if (source == "lutris" || source == "heroic") {
          // Use the launch command provided by the scanner
          std::string cmd = game.value("cmd", "");
          if (!cmd.empty()) {
            app["detached"] = nlohmann::json::array({ cmd });
          }
        }

        // Persist game classification metadata
        if (game.contains("game_category") && game["game_category"].is_string()) {
          app["game-category"] = game["game_category"];
        }
        if (game.contains("genres") && game["genres"].is_array()) {
          app["genres"] = game["genres"];
        }

        // Merge into apps file
        proc::migrate_apps(&fileTree, &app);
        imported++;
      }

      // Write back
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      proc::refresh(config::stream.file_apps);

      output["status"] = true;
      output["imported"] = imported;
    } catch (const std::exception &e) {
      output["status"] = false;
      output["error"] = e.what();
    }
    send_response(response, output);
  }

  /**
   * @brief Get the list of paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/list| GET| null}
   */
  void getClients(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json named_certs = nvhttp::get_all_clients();
    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
#ifdef _WIN32
    output_tree["platform"] = "windows";
#endif
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Update client information.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "uuid": "<uuid>",
   *   "name": "<Friendly Name>",
   *   "display_mode": "1920x1080x59.94",
   *   "do": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "undo": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "perm": <uint32_t>
   * }
   * @endcode
   */
  void updateClient(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      std::string name = input_tree.value("name", "");
      std::string display_mode = input_tree.value("display_mode", "");
      bool enable_legacy_ordering = input_tree.value("enable_legacy_ordering", true);
      bool allow_client_commands = input_tree.value("allow_client_commands", true);
      bool always_use_virtual_display = input_tree.value("always_use_virtual_display", false);
      auto do_cmds = nvhttp::extract_command_entries(input_tree, "do");
      auto undo_cmds = nvhttp::extract_command_entries(input_tree, "undo");
      auto perm = static_cast<crypto::PERM>(input_tree.value("perm", static_cast<uint32_t>(crypto::PERM::_no)) & static_cast<uint32_t>(crypto::PERM::_all));
      output_tree["status"] = nvhttp::update_device_info(
        uuid,
        name,
        display_mode,
        do_cmds,
        undo_cmds,
        perm,
        enable_legacy_ordering,
        allow_client_commands,
        always_use_virtual_display
      );
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Update Client: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *  "uuid": "<uuid>"
   * }
   * @endcode
   *
   * @api_examples{/api/clients/unpair| POST| {"uuid":"1234"}}
   */
  void unpair(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::unpair_client(uuid);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair all clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/unpair-all| POST| null}
   */
  void unpairAll(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nvhttp::erase_all_clients();
    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  // ---- Client Profile CRUD API ----

  // ---- Device Database API ----

  void getDevices(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(device_db::get_all_devices_json(), headers);
  }

  void getDeviceSuggestion(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    auto query = request->parse_query_string();
    std::string name = query.count("name") ? query.find("name")->second : "";
    std::string app = query.count("app") ? query.find("app")->second : "";

    auto opt = device_db::get_optimization(name, app);
    nlohmann::json output;
    output["status"] = true;
    output["device_name"] = name;
    if (opt.display_mode) output["display_mode"] = *opt.display_mode;
    if (opt.color_range) output["color_range"] = *opt.color_range;
    if (opt.hdr) output["hdr"] = *opt.hdr;
    if (opt.virtual_display) output["virtual_display"] = *opt.virtual_display;
    if (opt.target_bitrate_kbps) output["target_bitrate_kbps"] = *opt.target_bitrate_kbps;
    if (opt.nvenc_tune) output["nvenc_tune"] = *opt.nvenc_tune;
    output["reasoning"] = opt.reasoning;
    output["source"] = opt.source;
    send_response(response, output);
  }

  // ---- AI Optimizer API ----

  void getAiStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(ai_optimizer::get_status_json(), headers);
  }

  void getAiCache(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(ai_optimizer::get_cache_json(), headers);
  }

  void getAiHistory(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(ai_optimizer::get_history_json(), headers);
  }

  void clearAiCache(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    ai_optimizer::clear_cache();
    nlohmann::json output;
    output["status"] = true;
    send_response(response, output);
  }

  void testAiConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = nlohmann::json::parse(ss.str());

      auto get_string = [&](const char *key, const char *fallback = "") {
        return body.value(key, std::string {fallback});
      };
      auto get_enabled = [&](const char *key, bool fallback = false) {
        if (!body.contains(key)) return fallback;
        const auto &value = body[key];
        if (value.is_boolean()) return value.get<bool>();
        if (value.is_string()) {
          auto text = boost::algorithm::to_lower_copy(value.get<std::string>());
          return text == "enabled" || text == "true" || text == "1" || text == "yes";
        }
        return fallback;
      };

      std::string device = get_string("device_name", "Test Device");
      std::string app = get_string("app_name", "Test App");
      std::string gpu = get_string("gpu_info", "NVIDIA RTX (NVENC)");

      ai_optimizer::config_t ai_cfg;
      ai_cfg.enabled = true;
      ai_cfg.provider = get_string("ai_provider");
      ai_cfg.model = get_string("ai_model");
      ai_cfg.auth_mode = get_string("ai_auth_mode");
      ai_cfg.api_key = get_string("ai_api_key");
      ai_cfg.base_url = get_string("ai_base_url");
      ai_cfg.use_subscription = get_enabled("ai_use_subscription", false);
      ai_cfg.timeout_ms = body.value("ai_timeout_ms", config::video.ai_optimizer.timeout_ms);
      ai_cfg.cache_ttl_hours = body.value("ai_cache_ttl_hours", config::video.ai_optimizer.cache_ttl_hours);

      auto result = ai_optimizer::request_sync_with_config(ai_cfg, device, app, gpu);
      if (result) {
        output["status"] = true;
        output["provider"] = ai_cfg.provider;
        output["model"] = ai_cfg.model;
        output["auth_mode"] = ai_cfg.auth_mode;
        output["base_url"] = ai_cfg.base_url;
        if (result->display_mode) output["display_mode"] = *result->display_mode;
        if (result->color_range) output["color_range"] = *result->color_range;
        if (result->hdr) output["hdr"] = *result->hdr;
        if (result->virtual_display) output["virtual_display"] = *result->virtual_display;
        if (result->target_bitrate_kbps) output["target_bitrate_kbps"] = *result->target_bitrate_kbps;
        if (result->nvenc_tune) output["nvenc_tune"] = *result->nvenc_tune;
        if (result->preferred_codec) output["preferred_codec"] = *result->preferred_codec;
        output["reasoning"] = result->reasoning;
        output["source"] = result->source;
      } else {
        output["status"] = false;
        output["error"] = "Connection test failed — check provider settings and logs";
      }
    } catch (const std::exception &e) {
      output["status"] = false;
      output["error"] = e.what();
    }
    send_response(response, output);
  }

  void triggerAiOptimize(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = nlohmann::json::parse(ss.str());

      std::string device = body.value("device_name", "");
      std::string app = body.value("app_name", "");
      std::string gpu = body.value("gpu_info", "NVIDIA RTX (NVENC)");

      if (device.empty()) {
        output["status"] = false;
        output["error"] = "Missing device_name";
        send_response(response, output);
        return;
      }

      auto result = ai_optimizer::request_sync(device, app, gpu);
      if (result) {
        output["status"] = true;
        if (result->display_mode) output["display_mode"] = *result->display_mode;
        if (result->color_range) output["color_range"] = *result->color_range;
        if (result->hdr) output["hdr"] = *result->hdr;
        if (result->virtual_display) output["virtual_display"] = *result->virtual_display;
        if (result->target_bitrate_kbps) output["target_bitrate_kbps"] = *result->target_bitrate_kbps;
        if (result->nvenc_tune) output["nvenc_tune"] = *result->nvenc_tune;
        output["reasoning"] = result->reasoning;
        output["source"] = result->source;
      } else {
        output["status"] = false;
        output["error"] = "AI optimization failed — check provider settings and logs";
      }
    } catch (const std::exception &e) {
      output["status"] = false;
      output["error"] = e.what();
    }
    send_response(response, output);
  }

  // ---- Client Profile CRUD API ----

  void getClientProfiles(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    response->write(client_profiles::get_all_profiles_json(), headers);
  }

  void updateClientProfile(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = nlohmann::json::parse(ss.str());

      std::string name = body.value("name", "");
      if (name.empty()) {
        output_tree["status"] = false;
        output_tree["error"] = "Missing 'name' field";
        send_response(response, output_tree);
        return;
      }

      client_profiles::client_profile_t profile;
      profile.output_name = body.value("output_name", "");
      if (body.contains("color_range")) profile.color_range = body["color_range"].get<int>();
      if (body.contains("hdr")) profile.hdr = body["hdr"].get<bool>();
      profile.mac_address = body.value("mac_address", "");

      client_profiles::save_client_profile(name, profile);
      output_tree["status"] = true;
    } catch (const std::exception &e) {
      output_tree["status"] = false;
      output_tree["error"] = e.what();
    }
    send_response(response, output_tree);
  }

  void deleteClientProfile(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = nlohmann::json::parse(ss.str());

      std::string name = body.value("name", "");
      if (name.empty()) {
        output_tree["status"] = false;
        output_tree["error"] = "Missing 'name' field";
        send_response(response, output_tree);
        return;
      }

      bool deleted = client_profiles::delete_client_profile(name);
      output_tree["status"] = deleted;
      if (!deleted) output_tree["error"] = "Profile not found";
    } catch (const std::exception &e) {
      output_tree["status"] = false;
      output_tree["error"] = e.what();
    }
    send_response(response, output_tree);
  }

  /**
   * @brief Get the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = POLARIS_PLATFORM;
    output_tree["version"] = PROJECT_VERSION;
#ifdef _WIN32
    output_tree["vdisplayStatus"] = (int)proc::vDisplayDriverStatus;
#endif
#ifdef __linux__
    {
      auto vd_backend = virtual_display::detect_backend();
      output_tree["vdisplayAvailable"] = (vd_backend != virtual_display::backend_e::NONE);
      output_tree["vdisplayBackend"] = virtual_display::backend_name(vd_backend);
      auto runtime_state = cage_display_router::runtime_state();
      output_tree["runtime_backend"] = runtime_state.backend_name;
      output_tree["runtime_requested_headless"] = runtime_state.requested_headless;
      output_tree["runtime_effective_headless"] = runtime_state.effective_headless;
      output_tree["runtime_gpu_native_override_active"] = runtime_state.gpu_native_override_active;
    }
#endif
    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    for (auto &[name, value] : vars) {
      output_tree[name] = value;
    }
    send_response(response, output_tree);
  }

  /**
   * @brief Get the locale setting.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/configLocale| GET| null}
   */
  void getLocale(resp_https_t response, req_https_t request) {
    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["locale"] = config::sunshine.locale;
    send_response(response, output_tree);
  }

  /**
   * @brief Save the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "value"
   * }
   * @endcode
   *
   * @attention{It is recommended to ONLY save the config settings that differ from the default behavior.}
   *
   * @api_examples{/api/config| POST| {"key":"value"}}
   */
  void saveConfig(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      std::stringstream config_stream;
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      for (const auto &[k, v] : input_tree.items()) {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
          continue;
        }

        // v.dump() will dump valid json, which we do not want for strings in the config right now
        // we should migrate the config file to straight json and get rid of all this nonsense
        config_stream << k << " = " << (v.is_string() ? v.get<std::string>() : v.dump()) << std::endl;
      }
      file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str());
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Upload a cover image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/covers/upload| POST| {"key":"igdb_1234","url":"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/abc123.png"}}
   */
  void uploadCover(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    std::stringstream ss;

    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string key = input_tree.value("key", "");
      if (key.empty()) {
        bad_request(response, request, "Cover key is required");
        return;
      }
      std::string url = input_tree.value("url", "");
      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);
      std::string path = coverdir + http::url_escape(key) + ".png";
      if (!url.empty()) {
        if (http::url_get_host(url) != "images.igdb.com") {
          bad_request(response, request, "Only images.igdb.com is allowed");
          return;
        }
        if (!http::download_file(url, path)) {
          bad_request(response, request, "Failed to download cover");
          return;
        }
      } else {
        auto data = SimpleWeb::Crypto::Base64::decode(input_tree.value("data", ""));
        std::ofstream imgfile(path);
        imgfile.write(data.data(), static_cast<int>(data.size()));
      }
      output_tree["status"] = true;
      output_tree["path"] = path;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Serve a cover art image by app name.
   * Looks up the app's image-path and serves the PNG file.
   */
  void getCoverImage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;

    auto args = request->parse_query_string();
    auto name_it = args.find("name");
    if (name_it == args.end()) {
      bad_request(response, request, "Missing name parameter");
      return;
    }

    std::string image_path;
    for (const auto &app : proc::proc.get_apps()) {
      if (app.name == name_it->second) {
        image_path = app.image_path;
        break;
      }
    }

    if (image_path.empty()) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/plain");
      response->write(SimpleWeb::StatusCode::client_error_not_found, "No cover art", headers);
      return;
    }

    std::string resolved = proc::validate_app_image_path(image_path);

    std::ifstream in(resolved, std::ios::binary);
    if (!in) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/plain");
      response->write(SimpleWeb::StatusCode::client_error_not_found, "Image file not found", headers);
      return;
    }

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("Cache-Control", "max-age=86400");
    response->write(content, headers);
  }

  /**
   * @brief Search SteamGridDB for cover art by game name.
   * Returns a list of cover art URLs that can be downloaded.
   * Requires `steamgriddb_api_key` to be set in config.
   *
   * @api_examples{/api/covers/search| GET| ?name=Elden+Ring}
   */
  void searchCovers(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output;

    if (config::sunshine.steamgriddb_api_key.empty()) {
      output["status"] = false;
      output["error"] = "SteamGridDB API key not configured. Set steamgriddb_api_key in config.";
      output["covers"] = nlohmann::json::array();
      send_response(response, output);
      return;
    }

    auto args = request->parse_query_string();
    auto name_it = args.find("name");
    if (name_it == args.end() || name_it->second.empty()) {
      output["status"] = false;
      output["error"] = "Missing name parameter";
      send_response(response, output);
      return;
    }

    std::string game_name = name_it->second;
    std::string api_key = config::sunshine.steamgriddb_api_key;

    // Step 1: Search for the game by name
    std::string search_url = "https://www.steamgriddb.com/api/v2/search/autocomplete/" +
      http::url_escape(game_name);

    CURL *curl = curl_easy_init();
    if (!curl) {
      output["status"] = false;
      output["error"] = "Failed to init HTTP client";
      send_response(response, output);
      return;
    }

    std::string search_response;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, search_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &search_response);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Polaris/1.0");

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
      curl_easy_cleanup(curl);
      curl_slist_free_all(headers);
      output["status"] = false;
      output["error"] = "SteamGridDB search failed";
      send_response(response, output);
      return;
    }

    nlohmann::json covers = nlohmann::json::array();
    try {
      auto search_data = nlohmann::json::parse(search_response);
      if (search_data.contains("data") && search_data["data"].is_array() && !search_data["data"].empty()) {
        int game_id = search_data["data"][0]["id"].get<int>();

        // Step 2: Get grids (cover art) for the game
        std::string grid_url = "https://www.steamgriddb.com/api/v2/grids/game/" +
          std::to_string(game_id) + "?dimensions=600x900&limit=5";

        std::string grid_response;
        curl_easy_setopt(curl, CURLOPT_URL, grid_url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &grid_response);

        res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
          auto grid_data = nlohmann::json::parse(grid_response);
          if (grid_data.contains("data") && grid_data["data"].is_array()) {
            for (const auto &grid : grid_data["data"]) {
              nlohmann::json cover;
              cover["url"] = grid.value("url", "");
              cover["thumb"] = grid.value("thumb", grid.value("url", ""));
              cover["width"] = grid.value("width", 600);
              cover["height"] = grid.value("height", 900);
              cover["author"] = grid.contains("author") ? grid["author"].value("name", "") : "";
              covers.push_back(cover);
            }
          }
        }
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "SteamGridDB search error: " << e.what();
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    output["status"] = true;
    output["covers"] = covers;
    output["game_name"] = game_name;
    send_response(response, output);
  }

  /**
   * @brief Download a SteamGridDB cover art and save it for an app.
   *
   * @api_examples{/api/covers/download| POST| {"url":"https://...","app_uuid":"aaaa-bbbb"}}
   */
  void downloadCover(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = nlohmann::json::parse(ss.str());

      std::string url = body.value("url", "");
      std::string app_uuid = body.value("app_uuid", "");
      if (url.empty() || app_uuid.empty()) {
        output["status"] = false;
        output["error"] = "url and app_uuid required";
        send_response(response, output);
        return;
      }

      // Only allow SteamGridDB URLs
      if (url.find("steamgriddb.com") == std::string::npos &&
          url.find("steamcdn") == std::string::npos) {
        output["status"] = false;
        output["error"] = "Only SteamGridDB/Steam CDN URLs allowed";
        send_response(response, output);
        return;
      }

      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);
      std::string cover_path = coverdir + app_uuid + ".png";

      if (!http::download_file(url, cover_path)) {
        output["status"] = false;
        output["error"] = "Failed to download cover";
        send_response(response, output);
        return;
      }

      // Update the app's image-path in apps.json
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      auto file_tree = nlohmann::json::parse(content);
      if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
        for (auto &app : file_tree["apps"]) {
          if (app.value("uuid", "") == app_uuid) {
            app["image-path"] = cover_path;
            break;
          }
        }
        file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
        proc::refresh(config::stream.file_apps);
      }

      output["status"] = true;
      output["path"] = cover_path;
    } catch (const std::exception &e) {
      output["status"] = false;
      output["error"] = e.what();
    }
    send_response(response, output);
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void getLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);
    std::string content = file_handler::read_file(config::sunshine.log_file.c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    std::string contentType = "text/plain";
  #ifdef _WIN32
    contentType += "; charset=";
    contentType += currentCodePageToCharset();
  #endif
    headers.emplace("Content-Type", contentType);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  /**
   * @brief Update existing credentials.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "currentUsername": "Current Username",
   *   "currentPassword": "Current Password",
   *   "newUsername": "New Username",
   *   "newPassword": "New Password",
   *   "confirmNewPassword": "Confirm New Password"
   * }
   * @endcode
   *
   * @api_examples{/api/password| POST| {"currentUsername":"admin","currentPassword":"admin","newUsername":"admin","newPassword":"admin","confirmNewPassword":"admin"}}
   */
  void savePassword(resp_https_t response, req_https_t request) {
    if ((!config::sunshine.username.empty() && !authenticate(response, request)) || !validateContentType(response, request, "application/json"))
      return;
    print_req(request);
    std::vector<std::string> errors;
    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string username = input_tree.value("currentUsername", "");
      std::string newUsername = input_tree.value("newUsername", "");
      std::string password = input_tree.value("currentPassword", "");
      std::string newPassword = input_tree.value("newPassword", "");
      std::string confirmPassword = input_tree.value("confirmNewPassword", "");
      if (newUsername.empty())
        newUsername = username;
      if (newUsername.empty()) {
        errors.push_back("Invalid Username");
      } else {
        auto hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
        if (config::sunshine.username.empty() ||
            (boost::iequals(username, config::sunshine.username) && hash == config::sunshine.password)) {
          if (newPassword.empty() || newPassword != confirmPassword)
            errors.push_back("Password Mismatch");
          else {
            http::save_user_creds(config::sunshine.credentials_file, newUsername, newPassword);
            http::reload_user_creds(config::sunshine.credentials_file);
            sessionCookie.clear(); // force re-login
            output_tree["status"] = true;
          }
        } else {
          errors.push_back("Invalid Current Credentials");
        }
      }
      if (!errors.empty()) {
        std::string error = std::accumulate(errors.begin(), errors.end(), std::string(),
                                              [](const std::string &a, const std::string &b) {
                                                return a.empty() ? b : a + ", " + b;
                                              });
        bad_request(response, request, error);
        return;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get a one-time password (OTP).
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/otp| GET| null}
   */
  void getOTP(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      std::string passphrase = input_tree.value("passphrase", "");
      if (passphrase.empty())
        throw std::runtime_error("Passphrase not provided!");
      if (passphrase.size() < 4)
        throw std::runtime_error("Passphrase too short!");

      std::string deviceName = input_tree.value("deviceName", "");
      output_tree["otp"] = nvhttp::request_otp(passphrase, deviceName);
      output_tree["ip"] = platf::get_local_ip_for_gateway();
      output_tree["name"] = config::nvhttp.sunshine_name;
      output_tree["status"] = true;
      output_tree["message"] = "OTP created, effective within 3 minutes.";
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "OTP creation failed: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Send a PIN code to the host.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pin": "<pin>",
   *   "name": "Friendly Client Name"
   * }
   * @endcode
   *
   * @api_examples{/api/pin| POST| {"pin":"1234","name":"My PC"}}
   */
  void savePin(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string pin = input_tree.value("pin", "");
      std::string name = input_tree.value("name", "");
      output_tree["status"] = nvhttp::pin(pin, name);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Reset the display device persistence.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/reset-display-device-persistence| POST| null}
   */
  void resetDisplayDevicePersistence(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = display_device::reset_persistence();
    send_response(response, output_tree);
  }

  /**
   * @brief Restart Apollo.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/restart| POST| null}
   */
  void restart(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();

    // We may not return from this call
    platf::restart();
  }

  /**
   * @brief Quit Apollo.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * On Windows, if running in a service, a special shutdown code is returned.
   */
  void quit(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    BOOST_LOG(warning) << "Requested quit from config page!"sv;

    proc::proc.terminate();

#ifdef _WIN32
    if (GetConsoleWindow() == NULL) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
    } else
#endif
    {
      lifetime::exit_sunshine(0, true);
    }
    // If exit fails, write a response after 5 seconds.
    std::thread write_resp([response]{
      std::this_thread::sleep_for(5s);
      response->write();
    });
    write_resp.detach();
  }

  /**
   * @brief Launch an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void launchApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      // Check for required uuid field in body
      if (!input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      std::string uuid = input_tree["uuid"].get<std::string>();

      nlohmann::json output_tree;
      const auto &apps = proc::proc.get_apps();
      for (auto &app : apps) {
        if (app.uuid == uuid) {
          crypto::named_cert_t named_cert {
            .name = "",
            .uuid = http::unique_id,
            .perm = crypto::PERM::_all,
          };
          BOOST_LOG(info) << "Launching app ["sv << app.name << "] from web UI"sv;
          auto launch_session = nvhttp::make_launch_session(true, false, request->parse_query_string(), &named_cert);
          auto err = proc::proc.execute(app, launch_session);
          if (err) {
            bad_request(response, request, err == 503 ?
                        "Failed to initialize video capture/encoding. Is a display connected and turned on?" :
                        "Failed to start the specified application");
          } else {
            output_tree["status"] = true;
            send_response(response, output_tree);
          }
          return;
        }
      }
      BOOST_LOG(error) << "Couldn't find app with uuid ["sv << uuid << ']';
      bad_request(response, request, "Cannot find requested application");
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "LaunchApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Disconnect a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void disconnect(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::find_and_stop_session(uuid, true);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Disconnect: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Send a Wake-on-LAN magic packet to a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in one of these formats:
   * @code{.json}
   * { "name": "RP6" }
   * @endcode
   * or:
   * @code{.json}
   * { "mac": "AA:BB:CC:DD:EE:FF" }
   * @endcode
   *
   * When "name" is provided, the MAC address is looked up from the client profile.
   * When "mac" is provided directly, it is used as-is.
   */
  void sendWol(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;

      std::string mac;
      if (input_tree.contains("mac") && input_tree["mac"].is_string()) {
        mac = input_tree["mac"].get<std::string>();
      }
      else if (input_tree.contains("name") && input_tree["name"].is_string()) {
        auto name = input_tree["name"].get<std::string>();
        auto profile = client_profiles::get_client_profile(name);
        if (profile && !profile->mac_address.empty()) {
          mac = profile->mac_address;
        }
      }

      if (mac.empty()) {
        output_tree["status"] = false;
        output_tree["error"] = "MAC address not found";
        send_response(response, output_tree);
        return;
      }

      bool ok = wol::send_magic_packet(mac);
      output_tree["status"] = ok;
      output_tree["mac"] = mac;
      send_response(response, output_tree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "WoL: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Login the user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "username": "<username>",
   *   "password": "<password>"
   * }
   * @endcode
   */
  void login(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request) || !validateContentType(response, request, "application/json")) {
      return;
    }

    // Rate limiting: check if this IP is blocked
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (isLoginRateLimited(address)) {
      BOOST_LOG(warning) << "Web UI: ["sv << address << "] -- rate limited (too many failed login attempts)"sv;
      nlohmann::json tree;
      tree["status"] = false;
      tree["error"] = "Too many failed attempts. Please try again later.";
      const SimpleWeb::CaseInsensitiveMultimap headers {
        {"Content-Type", "application/json"}
      };
      response->write(SimpleWeb::StatusCode::client_error_too_many_requests, tree.dump(), headers);
      return;
    }

    auto fg = util::fail_guard([&]{
      recordLoginFailure(address);
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
    });

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      std::string username = input_tree.value("username", "");
      std::string password = input_tree.value("password", "");
      std::string hash = util::hex(crypto::hash(password + config::sunshine.salt)).to_string();
      if (!boost::iequals(username, config::sunshine.username) || hash != config::sunshine.password)
        return;
      clearLoginFailures(address);
      std::string sessionCookieRaw = crypto::rand_alphabet(64);
      sessionCookie = util::hex(crypto::hash(sessionCookieRaw + config::sunshine.salt)).to_string();
      cookie_creation_time = std::chrono::steady_clock::now();
      const SimpleWeb::CaseInsensitiveMultimap headers {
        { "Set-Cookie", "auth=" + sessionCookieRaw + "; Secure; SameSite=Strict; Max-Age=2592000; Path=/" }
      };
      response->write(headers);
      fg.disable();
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Web UI Login failed: ["sv << address
                               << "]: "sv << e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      fg.disable();
      return;
    }
  }

  // -------------------------------------------------------------------------
  // Virtual Display API (Linux only)
  // -------------------------------------------------------------------------
#ifdef __linux__
  /**
   * @brief Get virtual display status.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Returns JSON with available backends, active backend, and overall status.
   * @api_examples{/api/vdisplay/status| GET| null}
   */
  // ---- Display Preview API ----

  /**
   * @brief Capture a single screenshot and return as JPEG.
   * Uses spectacle (KDE) or grim (wlroots) for Wayland-compatible capture.
   */
  void getDisplayScreenshot(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    auto tid = std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::string tmpfile = "/tmp/polaris_preview_" + tid + ".png";
    std::string outfile = tmpfile;

    // Prefer grim on labwc's socket (works in both headless and windowed mode)
    bool captured = false;
    if (config::video.linux_display.use_cage_compositor) {
      auto cage_socket = cage_display_router::get_wayland_socket();
      if (!cage_socket.empty() && cage_display_router::is_running()) {
        std::string cmd = "WAYLAND_DISPLAY=" + cage_socket + " grim " + tmpfile + " 2>/dev/null";
        if (std::system(cmd.c_str()) == 0) {
          captured = true;
        }
      }
    }

    // Fallback: spectacle on desktop (for non-cage setups)
    if (!captured) {
      std::string cmd = "spectacle -b -n -o " + tmpfile + " 2>/dev/null";
      captured = (std::system(cmd.c_str()) == 0);
    }

    if (!captured) {
      nlohmann::json err;
      err["status"] = false;
      err["error"] = "Screenshot capture failed";
      send_response(response, err);
      return;
    }

    // Read the file and send as JPEG
    std::ifstream file(outfile, std::ios::binary);
    if (!file.is_open()) {
      nlohmann::json err;
      err["status"] = false;
      err["error"] = "Failed to read screenshot";
      send_response(response, err);
      std::remove(tmpfile.c_str());
      return;
    }

    std::vector<char> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    file.close();
    std::remove(tmpfile.c_str());
    if (outfile != tmpfile) std::remove(outfile.c_str());

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("Cache-Control", "no-store");
    response->write(std::string(data.begin(), data.end()), headers);
  }

  /**
   * @brief Stream display as MJPEG (multipart/x-mixed-replace).
   * Captures frames at the requested FPS using spectacle/grim.
   */
  void getDisplayStream(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    // Parse params
    int target_fps = 5;
    auto query = request->parse_query_string();
    if (query.count("fps")) {
      try { target_fps = std::clamp(std::stoi(query.find("fps")->second), 1, 15); } catch (...) {}
    }

    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    response->close_connection_after_response = true;

    std::thread([response, target_fps, shutdown_event]() {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "multipart/x-mixed-replace; boundary=frame");
      headers.emplace("Cache-Control", "no-cache");
      headers.emplace("X-Accel-Buffering", "no");
      response->write(headers);

      // Flush headers
      std::promise<bool> header_sent;
      response->send([&header_sent](const SimpleWeb::error_code &ec) {
        header_sent.set_value(static_cast<bool>(ec));
      });
      if (header_sent.get_future().get()) return;

      auto frame_interval = std::chrono::milliseconds(1000 / target_fps);
      std::string tmpfile = "/tmp/polaris_mjpeg_" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".jpg";

      while (!shutdown_event->peek()) {
        auto start = std::chrono::steady_clock::now();

        // Capture frame
        std::string cmd = "spectacle -b -n -o " + tmpfile + " 2>/dev/null";
        if (std::system(cmd.c_str()) != 0) {
          cmd = "grim " + tmpfile + " 2>/dev/null";
          if (std::system(cmd.c_str()) != 0) break;
        }

        // Read frame
        std::ifstream file(tmpfile, std::ios::binary);
        if (!file.is_open()) break;
        std::string data((std::istreambuf_iterator<char>(file)),
                          std::istreambuf_iterator<char>());
        file.close();

        // Write MJPEG multipart frame
        std::string frame_header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
          + std::to_string(data.size()) + "\r\n\r\n";

        std::promise<bool> send_ok;
        response->write(frame_header + data + "\r\n");
        response->send([&send_ok](const SimpleWeb::error_code &ec) {
          send_ok.set_value(!static_cast<bool>(ec));
        });

        if (!send_ok.get_future().get()) break; // Client disconnected

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto sleep_time = frame_interval - elapsed;
        if (sleep_time > std::chrono::milliseconds(0)) {
          std::this_thread::sleep_for(sleep_time);
        }
      }

      std::remove(tmpfile.c_str());
    }).detach();
  }

  void getVDisplayStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    // Cache backend detection for 30 seconds to avoid spamming EVDI/kscreen-doctor probes
    static virtual_display::backend_e cached_backend = virtual_display::backend_e::NONE;
    static std::chrono::steady_clock::time_point cache_time;
    static bool cache_valid = false;

    auto now = std::chrono::steady_clock::now();
    if (!cache_valid || (now - cache_time) > std::chrono::seconds(30)) {
      cached_backend = virtual_display::detect_backend();
      cache_time = now;
      cache_valid = true;
    }

    nlohmann::json output_tree;
    output_tree["status"] = true;

    bool available = cached_backend != virtual_display::backend_e::NONE;
    output_tree["available"] = available;
    output_tree["backend"] = virtual_display::backend_name(cached_backend);
    output_tree["backend_id"] = static_cast<int>(cached_backend);

    send_response(response, output_tree);
  }

  /**
   * @brief List available virtual display backends with their status.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Returns an array of backend objects, each with name, id, and available flag.
   * @api_examples{/api/vdisplay/backends| GET| null}
   */
  void getVDisplayBackends(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    auto detected = virtual_display::detect_backend();

    nlohmann::json backends = nlohmann::json::array();

    // Report each backend's availability
    struct backend_info {
      virtual_display::backend_e id;
      const char *name;
      const char *description;
    };

    std::vector<backend_info> all_backends = {
      { virtual_display::backend_e::EVDI,
        "EVDI",
        "Extensible Virtual Display Interface - true virtual DRM connector" },
      { virtual_display::backend_e::WAYLAND_WLR,
        "Wayland (headless output)",
        "Wayland compositor headless output (wlr-randr / hyprctl / kwin)" },
      { virtual_display::backend_e::KSCREEN_DOCTOR,
        "kscreen-doctor",
        "KDE kscreen-doctor - manages existing physical displays" },
    };

    for (const auto &b : all_backends) {
      nlohmann::json entry;
      entry["id"] = static_cast<int>(b.id);
      entry["name"] = b.name;
      entry["description"] = b.description;
      entry["detected"] = (detected == b.id);
      backends.push_back(entry);
    }

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["backends"] = backends;
    output_tree["active_backend"] = virtual_display::backend_name(detected);

    send_response(response, output_tree);
  }
  // Static virtual display instance for web UI-managed displays
  static std::optional<virtual_display::vdisplay_t> ui_vdisplay;

  /**
   * @brief Create a virtual display from the web UI.
   *
   * Accepts JSON body: { "width": 1920, "height": 1080, "fps": 60 }
   * Returns the created display info or error.
   */
  void createVDisplay(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    nlohmann::json output_tree;

    if (ui_vdisplay.has_value() && ui_vdisplay->active) {
      output_tree["status"] = false;
      output_tree["error"] = "A virtual display is already active. Destroy it first.";
      send_response(response, output_tree);
      return;
    }

    // Parse request body
    int width = 1920, height = 1080, fps = 60;
    try {
      std::string body;
      auto ss = std::make_shared<std::stringstream>();
      *ss << request->content.rdbuf();
      body = ss->str();
      if (!body.empty()) {
        auto j = nlohmann::json::parse(body);
        if (j.contains("width")) width = j["width"].get<int>();
        if (j.contains("height")) height = j["height"].get<int>();
        if (j.contains("fps")) fps = j["fps"].get<int>();
      }
    } catch (...) {
      // Use defaults
    }

    auto result = virtual_display::create(width, height, fps);
    if (result.has_value()) {
      ui_vdisplay = result;
      output_tree["status"] = true;
      output_tree["output_name"] = result->output_name;
      output_tree["width"] = result->width;
      output_tree["height"] = result->height;
      output_tree["fps"] = result->fps;
      output_tree["backend"] = virtual_display::backend_name(result->backend);
    } else {
      output_tree["status"] = false;
      output_tree["error"] = "Failed to create virtual display";
    }

    send_response(response, output_tree);
  }

  /**
   * @brief Destroy the web UI-managed virtual display.
   */
  void destroyVDisplay(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    nlohmann::json output_tree;

    if (!ui_vdisplay.has_value() || !ui_vdisplay->active) {
      output_tree["status"] = false;
      output_tree["error"] = "No active virtual display to destroy";
      send_response(response, output_tree);
      return;
    }

    virtual_display::destroy(*ui_vdisplay);
    ui_vdisplay.reset();

    output_tree["status"] = true;
    send_response(response, output_tree);
  }
#endif  // __linux__

  // ---- Recording / Replay Buffer API ----

  /**
   * @brief Start continuous stream recording.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/recording/start| POST| null}
   */
  void startRecording(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    stream_recorder::start_recording();

    nlohmann::json tree;
    tree["status"] = true;
    tree["recording"] = stream_recorder::is_recording();
    tree["file"] = stream_recorder::current_file();
    send_response(response, tree);
  }

  /**
   * @brief Stop continuous stream recording.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/recording/stop| POST| null}
   */
  void stopRecording(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    stream_recorder::stop_recording();

    nlohmann::json tree;
    tree["status"] = true;
    tree["recording"] = stream_recorder::is_recording();
    send_response(response, tree);
  }

  /**
   * @brief Save the replay buffer to a file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/recording/save-replay| POST| null}
   */
  void saveReplay(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    auto file = stream_recorder::save_replay();

    nlohmann::json tree;
    tree["status"] = !file.empty();
    tree["file"] = file;
    send_response(response, tree);
  }

  /**
   * @brief Get the current recording status.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/recording/status| GET| null}
   */
  void getRecordingStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    auto mode = stream_recorder::current_mode();
    nlohmann::json tree;
    tree["status"] = true;
    tree["recording"] = stream_recorder::is_recording();
    tree["mode"] = (mode == stream_recorder::mode_t::disabled      ? "disabled" :
                    mode == stream_recorder::mode_t::continuous     ? "continuous" :
                                                                      "replay_buffer");
    tree["file"] = stream_recorder::current_file();
    send_response(response, tree);
  }

  /**
   * @brief Get current stream statistics as JSON.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  /**
   * @brief Get system hardware stats (GPU telemetry via nvidia-smi).
   *
   * Returns JSON with GPU name, temperature, utilization, VRAM, encoder load,
   * power draw, and clock speeds. Linux/NVIDIA only.
   */
  void getSystemStats(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    nlohmann::json output;
    output["gpu"] = nullptr;

#ifdef __linux__
    // Query nvidia-smi for GPU telemetry
    FILE *pipe = popen(
      "nvidia-smi --query-gpu=name,temperature.gpu,utilization.gpu,utilization.encoder,"
      "memory.used,memory.total,fan.speed,power.draw,clocks.gr,clocks.mem "
      "--format=csv,noheader,nounits 2>/dev/null",
      "r"
    );
    if (pipe) {
      char buf[512];
      if (fgets(buf, sizeof(buf), pipe)) {
        std::string line(buf);
        // Remove trailing newline
        if (!line.empty() && line.back() == '\n') line.pop_back();

        // Parse CSV: name,temp,gpu_util,enc_util,vram_used,vram_total,fan,power,clock_gpu,clock_mem
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) {
          // Trim leading/trailing whitespace
          auto start = field.find_first_not_of(" \t");
          auto end = field.find_last_not_of(" \t");
          fields.push_back(start != std::string::npos ? field.substr(start, end - start + 1) : "");
        }

        if (fields.size() >= 10) {
          nlohmann::json gpu;
          gpu["name"] = fields[0];
          try {
            gpu["temperature_c"] = std::stoi(fields[1]);
            gpu["utilization_pct"] = std::stoi(fields[2]);
            gpu["encoder_pct"] = std::stoi(fields[3]);
            gpu["vram_used_mb"] = std::stoi(fields[4]);
            gpu["vram_total_mb"] = std::stoi(fields[5]);
            gpu["fan_speed_pct"] = std::stoi(fields[6]);
            gpu["power_draw_w"] = std::stof(fields[7]);
            gpu["clock_gpu_mhz"] = std::stoi(fields[8]);
            gpu["clock_mem_mhz"] = std::stoi(fields[9]);
          } catch (...) {
            // If any field fails to parse, return what we have
          }
          output["gpu"] = gpu;
        }
      }
      pclose(pipe);
    }

    // Query connected displays via xrandr
    {
      nlohmann::json displays = nlohmann::json::array();
      FILE *xpipe = popen("xrandr --query 2>/dev/null | grep ' connected'", "r");
      if (xpipe) {
        char buf[512];
        while (fgets(buf, sizeof(buf), xpipe)) {
          std::string line(buf);
          if (!line.empty() && line.back() == '\n') line.pop_back();

          nlohmann::json display;
          // Format: "DP-3 connected primary 7680x2160+0+0 ..."
          std::istringstream iss(line);
          std::string name, status, extra;
          iss >> name >> status;

          display["name"] = name;
          display["primary"] = false;

          // Check for "primary" keyword and resolution
          std::string token;
          while (iss >> token) {
            if (token == "primary") {
              display["primary"] = true;
            } else if (token.find('x') != std::string::npos && token.find('+') != std::string::npos) {
              // Resolution like "7680x2160+0+0"
              auto xpos = token.find('x');
              auto plus = token.find('+');
              if (xpos != std::string::npos && plus != std::string::npos) {
                try {
                  display["width"] = std::stoi(token.substr(0, xpos));
                  display["height"] = std::stoi(token.substr(xpos + 1, plus - xpos - 1));
                } catch (...) {}
              }
              break;
            }
          }
          displays.push_back(display);
        }
        pclose(xpipe);
      }
      output["displays"] = displays;
    }

    // Query audio via pactl (PipeWire/PulseAudio)
    {
      nlohmann::json audio;
      FILE *apipe = popen("pactl info 2>/dev/null", "r");
      if (apipe) {
        char buf[512];
        while (fgets(buf, sizeof(buf), apipe)) {
          std::string line(buf);
          if (!line.empty() && line.back() == '\n') line.pop_back();
          auto colon = line.find(':');
          if (colon == std::string::npos) continue;
          std::string key = line.substr(0, colon);
          std::string val = line.substr(colon + 1);
          // Trim val
          auto vs = val.find_first_not_of(" \t");
          if (vs != std::string::npos) val = val.substr(vs);

          if (key == "Server Name") audio["server"] = val;
          else if (key == "Default Sink") audio["sink"] = val;
          else if (key == "Default Source") audio["source"] = val;
        }
        pclose(apipe);
      }
      output["audio"] = audio;
    }

    // Session type (Wayland vs X11)
    {
      const char *xdg = std::getenv("XDG_SESSION_TYPE");
      output["session_type"] = xdg ? std::string(xdg) : "unknown";
    }
#endif

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Cache-Control", "no-cache");
    response->write(output.dump(), headers);
  }

  void getStreamStats(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    auto stats = stream_stats::get_current();
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; font-src 'self'; frame-ancestors 'none';");
    response->write(stats.to_json(), headers);
  }

  /**
   * @brief Stream stats via Server-Sent Events (SSE).
   *
   * Holds the connection open and pushes stats JSON every second as SSE messages.
   * The client uses the EventSource API which handles reconnection automatically.
   * The connection closes when the server shuts down or the client disconnects.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getStreamStatsSSE(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    // Spawn a detached thread that streams SSE events.
    // The shared_ptr to response keeps the connection alive.
    std::thread([response]() {
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);

      // Tell SimpleWeb this is a streaming response with no content-length
      response->close_connection_after_response = true;

      // Send the SSE headers
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/event-stream");
      headers.emplace("Cache-Control", "no-cache");
      headers.emplace("X-Accel-Buffering", "no");
      response->write(headers);

      // Flush headers to the client
      std::promise<bool> header_sent;
      response->send([&header_sent](const SimpleWeb::error_code &ec) {
        header_sent.set_value(static_cast<bool>(ec));
      });
      if (header_sent.get_future().get()) {
        // Header send failed, client likely disconnected
        return;
      }

      // Stream stats every second until shutdown or client disconnect
      while (!shutdown_event->peek()) {
        auto stats = stream_stats::get_current();
        *response << "data: " << stats.to_json() << "\n\n";

        std::promise<bool> send_error;
        response->send([&send_error](const SimpleWeb::error_code &ec) {
          send_error.set_value(static_cast<bool>(ec));
        });

        if (send_error.get_future().get()) {
          // Client disconnected
          BOOST_LOG(debug) << "SSE stats client disconnected";
          break;
        }

        // Wait 1 second between updates, but check shutdown more frequently
        for (int i = 0; i < 10 && !shutdown_event->peek(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    }).detach();
  }

  // Session event bus (file-scope for access from emit_session_event and SSE handler)
  static std::mutex s_event_mtx;
  static std::vector<std::string> s_events;
  static std::atomic<uint64_t> s_event_seq{0};

  /**
   * @brief Start the HTTPS server.
   */
  void start() {
    // Generate CSRF token at server startup
    csrfToken = crypto::rand_alphabet(48,
      std::string_view {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"});
    BOOST_LOG(info) << "CSRF token generated for web UI session";

    // Initialize AI optimizer with config
    {
      ai_optimizer::config_t ai_cfg;
      ai_cfg.enabled = config::video.ai_optimizer.enabled;
      ai_cfg.provider = config::video.ai_optimizer.provider;
      ai_cfg.model = config::video.ai_optimizer.model;
      ai_cfg.auth_mode = config::video.ai_optimizer.auth_mode;
      ai_cfg.api_key = config::video.ai_optimizer.api_key;
      ai_cfg.base_url = config::video.ai_optimizer.base_url;
      ai_cfg.use_subscription = config::video.ai_optimizer.use_subscription;
      ai_cfg.timeout_ms = config::video.ai_optimizer.timeout_ms;
      ai_cfg.cache_ttl_hours = config::video.ai_optimizer.cache_ttl_hours;
      ai_optimizer::init(ai_cfg);
    }

    // Initialize device database
    device_db::load();

    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);
    https_server_t server { config::nvhttp.cert, config::nvhttp.pkey };

    // Helper lambda to wrap a handler with CSRF validation.
    // API key (Bearer token) requests bypass CSRF since they are not cookie-based.
    // -----------------------------------------------------------------------
    // Polaris session event bus (SSE)
    // Uses file-scope globals (defined below start())
    // -----------------------------------------------------------------------

    auto getPolarisEventsSSE = [](resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);

      std::thread([response]() {
        auto shutdown_event = mail::man->event<bool>(mail::shutdown);
        response->close_connection_after_response = true;

        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "text/event-stream");
        headers.emplace("Cache-Control", "no-cache");
        headers.emplace("X-Accel-Buffering", "no");
        response->write(headers);

        std::promise<bool> header_sent;
        response->send([&header_sent](const SimpleWeb::error_code &ec) {
          header_sent.set_value(static_cast<bool>(ec));
        });
        if (header_sent.get_future().get()) return;

        uint64_t last_seq = s_event_seq;

        while (!shutdown_event->peek()) {
          // Build current session state
          nlohmann::json state;
#ifdef __linux__
          state["cage_running"] = cage_display_router::is_running();
          state["cage_socket"] = cage_display_router::get_wayland_socket();
          state["screen_locked"] = session_manager::is_screen_locked();
#endif
          state["seq"] = s_event_seq.load();
          state["session_state"] = get_session_state();

          // Check for new events
          {
            std::lock_guard lk(s_event_mtx);
            if (!s_events.empty() && s_event_seq > last_seq) {
              for (auto &evt : s_events) {
                *response << "event: session\ndata: " << evt << "\n\n";
              }
              s_events.clear();
              last_seq = s_event_seq;
            }
          }

          // Always send heartbeat with state
          *response << "event: state\ndata: " << state.dump() << "\n\n";

          std::promise<bool> send_err;
          response->send([&send_err](const SimpleWeb::error_code &ec) {
            send_err.set_value(static_cast<bool>(ec));
          });
          if (send_err.get_future().get()) break;

          for (int i = 0; i < 20 && !shutdown_event->peek(); ++i) {
            std::this_thread::sleep_for(100ms);
          }
        }
      }).detach();
    };

    // -----------------------------------------------------------------------
    // Polaris session API
    // -----------------------------------------------------------------------

    auto getPolarisSession = [](resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);

      nlohmann::json output;

#ifdef __linux__
      output["cage_running"] = cage_display_router::is_running();
      output["cage_pid"] = cage_display_router::get_pid();
      output["cage_socket"] = cage_display_router::get_wayland_socket();
      output["cage_healthy"] = cage_display_router::is_healthy();
      output["screen_locked"] = session_manager::is_screen_locked();
#else
      output["cage_running"] = false;
#endif

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    auto postPolarisUnlock = [](resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) return;
      print_req(request);

      nlohmann::json output;
#ifdef __linux__
      bool was_locked = session_manager::is_screen_locked();
      bool success = session_manager::unlock_screen();
      output["success"] = success;
      output["was_locked"] = was_locked;
#else
      output["success"] = false;
      output["was_locked"] = false;
#endif

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    auto withCsrf = [](auto handler) {
      return [handler](resp_https_t response, req_https_t request) {
        // Skip CSRF check for API key auth (Bearer token)
        auto auth_header = request->header.find("authorization");
        bool has_bearer = auth_header != request->header.end() &&
                          auth_header->second.substr(0, 7) == "Bearer ";
        if (!has_bearer && !validateCsrf(request)) {
          BOOST_LOG(warning) << "CSRF token validation failed for "sv << request->path;
          bad_request(response, request, "CSRF token missing or invalid");
          return;
        }
        handler(response, request);
      };
    };

    server.default_resource["DELETE"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PATCH"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["POST"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PUT"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["GET"] = not_found;
    // SPA: serve index.html for all page routes (Vue Router handles navigation via hash)
    server.resource["^/$"]["GET"] = getSpaPage;
    server.resource["^/pin/?$"]["GET"] = getSpaPage;
    server.resource["^/apps/?$"]["GET"] = getSpaPage;
    server.resource["^/config/?$"]["GET"] = getSpaPage;
    server.resource["^/password/?$"]["GET"] = getSpaPage;
    server.resource["^/welcome/?$"]["GET"] = getSpaPage;
    server.resource["^/login/?$"]["GET"] = getSpaPage;
    server.resource["^/troubleshooting/?$"]["GET"] = getSpaPage;
    // Login is exempt from CSRF (it's the entry point; rate limiting protects it instead)
    server.resource["^/api/login"]["POST"] = login;
    server.resource["^/api/pin$"]["POST"] = withCsrf(savePin);
    server.resource["^/api/otp$"]["POST"] = withCsrf(getOTP);
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = withCsrf(saveApp);
    server.resource["^/api/apps/reorder$"]["POST"] = withCsrf(reorderApps);
    server.resource["^/api/apps/delete$"]["POST"] = withCsrf(deleteApp);
    server.resource["^/api/apps/launch$"]["POST"] = withCsrf(launchApp);
    server.resource["^/api/apps/close$"]["POST"] = withCsrf(closeApp);
    server.resource["^/api/games/scan$"]["GET"] = scanGames;
    server.resource["^/api/games/import$"]["POST"] = withCsrf(importGames);
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = withCsrf(saveConfig);
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/restart$"]["POST"] = withCsrf(restart);
    server.resource["^/api/quit$"]["POST"] = withCsrf(quit);
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = withCsrf(resetDisplayDevicePersistence);
    server.resource["^/api/password$"]["POST"] = withCsrf(savePassword);
    server.resource["^/api/clients/unpair-all$"]["POST"] = withCsrf(unpairAll);
    server.resource["^/api/clients/list$"]["GET"] = getClients;
    server.resource["^/api/clients/update$"]["POST"] = withCsrf(updateClient);
    server.resource["^/api/clients/unpair$"]["POST"] = withCsrf(unpair);
    server.resource["^/api/clients/disconnect$"]["POST"] = withCsrf(disconnect);
    server.resource["^/api/clients/wol$"]["POST"] = withCsrf(sendWol);
    server.resource["^/api/ai/status$"]["GET"] = getAiStatus;
    server.resource["^/api/ai/cache$"]["GET"] = getAiCache;
    server.resource["^/api/ai/history$"]["GET"] = getAiHistory;
    server.resource["^/api/ai/cache/clear$"]["POST"] = withCsrf(clearAiCache);
    server.resource["^/api/ai/test$"]["POST"] = withCsrf(testAiConfig);
    server.resource["^/api/ai/optimize$"]["POST"] = withCsrf(triggerAiOptimize);
    server.resource["^/api/devices$"]["GET"] = getDevices;
    server.resource["^/api/devices/suggest$"]["GET"] = getDeviceSuggestion;
    server.resource["^/api/clients/profiles$"]["GET"] = getClientProfiles;
    server.resource["^/api/clients/profiles/update$"]["POST"] = withCsrf(updateClientProfile);
    server.resource["^/api/clients/profiles/delete$"]["POST"] = withCsrf(deleteClientProfile);
    server.resource["^/api/covers/upload$"]["POST"] = withCsrf(uploadCover);
    server.resource["^/api/covers/image$"]["GET"] = getCoverImage;
    server.resource["^/api/covers/search$"]["GET"] = searchCovers;
    server.resource["^/api/covers/download$"]["POST"] = withCsrf(downloadCover);
    server.resource["^/api/stats/system$"]["GET"] = getSystemStats;
    server.resource["^/api/stats/stream$"]["GET"] = getStreamStats;
    server.resource["^/api/stats/stream-sse$"]["GET"] = getStreamStatsSSE;
    server.resource["^/api/recording/start$"]["POST"] = withCsrf(startRecording);
    server.resource["^/api/recording/stop$"]["POST"] = withCsrf(stopRecording);
    server.resource["^/api/recording/save-replay$"]["POST"] = withCsrf(saveReplay);
    server.resource["^/api/recording/status$"]["GET"] = getRecordingStatus;
#ifdef __linux__
    server.resource["^/api/display/screenshot$"]["GET"] = getDisplayScreenshot;
    server.resource["^/api/display/stream$"]["GET"] = getDisplayStream;
    server.resource["^/api/vdisplay/status$"]["GET"] = getVDisplayStatus;
    server.resource["^/api/vdisplay/backends$"]["GET"] = getVDisplayBackends;
    server.resource["^/api/vdisplay/create$"]["POST"] = withCsrf(createVDisplay);
    server.resource["^/api/vdisplay/destroy$"]["POST"] = withCsrf(destroyVDisplay);
#endif
    // Polaris session endpoints
    server.resource["^/api/polaris/session$"]["GET"] = getPolarisSession;
    server.resource["^/api/polaris/unlock$"]["POST"] = withCsrf(postPolarisUnlock);
    server.resource["^/api/polaris/events$"]["GET"] = getPolarisEventsSSE;

    server.resource["^/images/polaris.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-polaris-45.png$"]["GET"] = getApolloLogoImage;
    server.resource["^/assets\\/.+$"]["GET"] = getNodeModules;
    server.resource["^/images\\/.+$"]["GET"] = getNodeModules;
    server.resource["^/sw\\.js$"]["GET"] = getNodeModules;
    server.config.reuse_address = true;
    server.config.address = net::af_to_any_address_string(address_family);
    server.config.port = port_https;

    auto accept_and_run = [&](auto *server) {
      try {
        server->start([port_https](unsigned short port) {
          BOOST_LOG(info) << "Configuration UI available at [https://localhost:"sv << port << "]";
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek())
          return;
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread tcp { accept_and_run, &server };

    // Wait for any event
    shutdown_event->view();

    server.stop();

    tcp.join();
  }
  static std::atomic<session_state_e> s_session_state{session_state_e::idle};

  void emit_session_event(const std::string &event, const std::string &message) {
    nlohmann::json evt;
    evt["event"] = event;
    evt["message"] = message;
    evt["state"] = get_session_state();
    evt["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard lk(s_event_mtx);
    s_events.push_back(evt.dump());
    s_event_seq++;

    BOOST_LOG(info) << "session_event: "sv << event << " ["sv << get_session_state() << "] "sv << message;
  }

  void set_session_state(session_state_e state) {
    s_session_state = state;
  }

  std::string get_session_state() {
    switch (s_session_state.load()) {
      case session_state_e::idle: return "idle";
      case session_state_e::initializing: return "initializing";
      case session_state_e::cage_starting: return "cage_starting";
      case session_state_e::game_launching: return "game_launching";
      case session_state_e::streaming: return "streaming";
      case session_state_e::tearing_down: return "tearing_down";
      default: return "unknown";
    }
  }
}  // namespace confighttp
