/**
 * @file src/browser_stream.h
 * @brief Browser Stream API and session metadata helpers.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace browser_stream {
  constexpr std::uint16_t default_webtransport_port = 47992;

  struct session_token_t {
    std::string token;
    std::chrono::steady_clock::time_point expires_at;
  };

  bool build_enabled();
  nlohmann::json status_json();
  nlohmann::json create_session(
    std::string_view remote_address,
    std::string_view host,
    std::string_view app_uuid = {},
    std::string_view profile_id = "balanced"
  );
  nlohmann::json submit_input(std::string_view token, const nlohmann::json &events);

  /**
   * @brief Result of stop_session — token match and whether an owned app remains.
   *
   * SB-2: HTTP stop handlers respond before nested app terminate; they call
   * stop_session(..., terminate_owned_app=false) then terminate after write.
   */
  struct stop_session_result_t {
    bool stopped = false;  ///< session token was valid / consumed
    bool owns_app = false;  ///< caller should terminate nested app if not done here
  };

  /**
   * @brief Stop a Browser Stream session (helper + optional capture/app).
   * @param terminate_owned_app When true (default), call proc::terminate if the
   *   session owned the nested app. HTTP handlers pass false and terminate after
   *   the response so :47990 cannot wedge on gamescope kill.
   * @param release_media When true (default), run prepare_for_session_teardown.
   *   HTTP handlers pass false, answer the client, then call prepare themselves
   *   so a hung portal/PW dtor cannot empty the stop body.
   */
  stop_session_result_t stop_session(std::string_view token, bool terminate_owned_app = true, bool release_media = true);

  /**
   * @brief Synchronously stop Browser Stream media capture before nested teardown.
   *
   * Order: signal shutdown → portal release → short front-end wait. The shared
   * session_media fence then waits for owned cleanup to reach terminal state.
   * Prefer session_media::prepare_for_stop() from process/confighttp so concurrent
   * stop paths do not double-enter. HTTP handlers should schedule after response.
   */
  void prepare_for_session_teardown();
  session_token_t issue_session_token(std::string_view remote_address, std::string_view app_uuid = {}, bool owns_app = false);
  bool consume_session_token(std::string_view token, std::string_view remote_address);
}  // namespace browser_stream
