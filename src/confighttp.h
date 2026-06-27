/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <functional>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

// local includes
#include "thread_safe.h"

#define WEB_DIR POLARIS_ASSETS_DIR "/web/"

using namespace std::chrono_literals;

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  constexpr auto SESSION_EXPIRE_DURATION = 24h * 15;
  inline constexpr std::string_view CLIENT_SETTINGS_ENDPOINT = "/polaris/v1/client-settings";

  namespace detail {
    inline std::string host_without_port(std::string_view host_header) {
      std::string host {host_header};
      if (host.empty()) {
        return "127.0.0.1";
      }

      if (host.front() == '[') {
        const auto closing_bracket = host.find(']');
        if (closing_bracket != std::string::npos) {
          return host.substr(0, closing_bracket + 1);
        }
        return host;
      }

      const auto first_colon = host.find(':');
      if (first_colon == std::string::npos) {
        return host;
      }
      if (host.find(':', first_colon + 1) != std::string::npos) {
        return host;
      }
      return host.substr(0, first_colon);
    }
  }  // namespace detail

  inline std::string client_settings_endpoint_base_url(std::string_view request_host, std::uint16_t https_port) {
    return "https://" + detail::host_without_port(request_host) + ":" + std::to_string(https_port);
  }

  inline std::string client_settings_endpoint_url(std::string_view request_host, std::uint16_t https_port) {
    return client_settings_endpoint_base_url(request_host, https_port) + std::string(CLIENT_SETTINGS_ENDPOINT);
  }

  std::uint16_t client_settings_endpoint_https_port();
  std::string client_settings_endpoint_base_url(std::string_view request_host);
  std::string client_settings_endpoint_url(std::string_view request_host);

  void start();

  /**
   * @brief Session lifecycle states.
   */
  enum class session_state_e {
    idle,            ///< No active session
    initializing,    ///< Preparing session (lock inhibit, env check)
    cage_starting,   ///< Cage compositor spawning
    game_launching,  ///< Game process starting inside cage
    streaming,       ///< Stream active, frames flowing
    paused,          ///< No active clients, app kept alive for resume
    tearing_down,    ///< Cleanup in progress
  };

  /**
   * @brief Emit a session lifecycle event and update state.
   */
  void emit_session_event(const std::string &event, const std::string &message = "");

  /**
   * @brief Set the current session state.
   */
  void set_session_state(session_state_e state);

  /**
   * @brief Get the current session state as a string.
   */
  std::string get_session_state();
}  // namespace confighttp

// mime types map
const std::map<std::string, std::string> mime_types = {
  {"css", "text/css"},
  {"gif", "image/gif"},
  {"htm", "text/html"},
  {"html", "text/html"},
  {"ico", "image/x-icon"},
  {"jpeg", "image/jpeg"},
  {"jpg", "image/jpeg"},
  {"js", "application/javascript"},
  {"json", "application/json"},
  {"png", "image/png"},
  {"svg", "image/svg+xml"},
  {"ttf", "font/ttf"},
  {"txt", "text/plain"},
  {"woff2", "font/woff2"},
  {"xml", "text/xml"},
};
