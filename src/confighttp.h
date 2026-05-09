/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <functional>
#include <chrono>
#include <string>

// local includes
#include "thread_safe.h"

#define WEB_DIR POLARIS_ASSETS_DIR "/web/"

using namespace std::chrono_literals;

namespace confighttp {
  constexpr auto PORT_HTTPS = 1;
  constexpr auto SESSION_EXPIRE_DURATION = 24h * 15;
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
