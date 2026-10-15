/**
 * @file src/confighttp.h
 * @brief Declarations for the Web UI Config HTTP server.
 */
#pragma once

// standard includes
#include <filesystem>
#include <functional>
#include <optional>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "artwork_sweep.h"
#include "game_artwork_provider.h"
#include "process.h"
#include "stream_recorder.h"
#include "thread_safe.h"

namespace stream_stats {
  struct stats_t;
}

namespace game_library {
  enum class launcher_install_t;
}

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

#ifdef POLARIS_TESTS
  // Serve the production read-only handlers on an owned loopback TLS listener. Before `ready`,
  // `network_queued` (when given) receives a wait on this server's own network collector: true
  // once that many accepted readouts are queued for a read not yet started. It is valid only until
  // this function returns.
  void run_apps_stats_server_for_tests(
    const std::string &cert, const std::string &key,
    const std::function<void(unsigned short, std::function<void()>)> &ready,
    const std::function<void()> &before_apps,
    const std::function<void(std::function<bool(std::size_t, std::chrono::milliseconds)>)> &network_queued = {}
  );
  // Serve the production console app list, save and launch handlers (#204 entry tests) on an owned
  // loopback TLS listener.
  void run_console_app_routes_for_tests(
    const std::string &cert, const std::string &key,
    const std::function<void(unsigned short, std::function<void()>)> &ready
  );
  // Serve the production recording routes, registered by the same function start() uses (CSRF
  // wrapper and handler authentication included), on an owned loopback TLS listener.
  void run_recording_routes_for_tests(
    const std::string &cert, const std::string &key,
    const std::function<void(unsigned short, std::function<void()>)> &ready
  );
  // Configure only before starting a test listener; reset after its owned drain.
  void set_recording_post_for_tests(std::function<void(std::function<void()>)> post);
  void set_recording_commit_observer_for_tests(std::function<void()> observer);
  void set_recording_before_launch_for_tests(std::function<void()> hook);
  // Serve the production library artwork route, registered by the same function start() uses, on
  // an owned loopback TLS listener. Its background fills are drained before this returns.
  void run_library_artwork_routes_for_tests(
    const std::string &cert, const std::string &key,
    const std::function<void(unsigned short, std::function<void()>)> &ready
  );
  // Configure only before starting a test listener; reset after its owned drain. The transport
  // replaces Steam's CDN for background fills, and appdata the host artwork cache's root.
  void set_library_artwork_for_tests(game_artwork::providers::transport_t transport, std::optional<std::filesystem::path> appdata);
  // True once no background fill is running, false when one still is after the timeout.
  bool wait_for_library_artwork_fills_for_tests(std::chrono::milliseconds timeout);
  // Forget every running mark and retry window, so the next view of any entry may fill again.
  void forget_library_artwork_fills_for_tests();
#endif

  /**
   * @brief The Steam downloads a console view of an entry's background or logo may start.
   *
   * Only the background and logo the host artwork cache does not hold yet, for an entry with a
   * Steam app id, from Steam's public CDN. Nothing for a Desktop or utility entry, nor for one whose
   * artwork somebody removed with Remove artwork. Never SteamGridDB, and never a poster: the console
   * shows the entry's own image for that.
   */
  std::vector<game_artwork::providers::request_t> library_artwork_fill_plan(
    const std::filesystem::path &appdata,
    const proc::ctx_t &app
  );

  /**
   * @brief Assemble the GET /api/recording/status payload from one recorder snapshot.
   *
   * The legacy keys (status, recording, mode, file) keep their meaning; `replay` is additive.
   * `recording` and `mode` are independent facts. Durations are whole milliseconds by arrival
   * time, not presentation time; a buffered run is stored raw video starting at a keyframe, not
   * a save guarantee or a playable clip. No session identity, output directory or error text.
   */
  nlohmann::json build_recording_status_json(const stream_recorder::recorder_status_t &status);

  /**
   * @brief Assemble the POST /api/recording/save-replay payload.
   *
   * The legacy keys (status, file) keep their meaning. `outcome` is always present; `captured`
   * is the admitted snapshot or null; `kind` is present only when a raw video file was saved.
   * No error text, errno or session identity.
   */
  nlohmann::json build_save_replay_json(const stream_recorder::detail::save_result_t &result);

  /**
   * @brief Assemble the GET /api/settings/metadata payload.
   *
   * @param client_uuid Optional paired-client uuid; empty selects the host view.
   * @param error Set to a serve-ready message when the uuid is unknown.
   * @return The payload, or an empty object when error is set.
   */
  nlohmann::json build_settings_metadata_payload(const std::string &client_uuid, std::string &error);

  /**
   * @brief Add the live tuning and auto-quality blocks to a stream stats payload.
   *
   * Both GET /api/stats/stream and its SSE variant serve this augmented shape;
   * the keys are additive so bare stats consumers keep working unchanged.
   */
  nlohmann::json augment_stream_stats_json(nlohmann::json stats_json, const stream_stats::stats_t &stats);

  /**
   * @brief The stream stats, with their Doctor given the encoder selection
   *        nvhttp::stream_stats_encoder_selection_json() answers.
   */
  nlohmann::json stream_stats_json(const stream_stats::stats_t &stats);

  /**
   * @brief What GET /api/stats/stream and its SSE variant serve: stream_stats_json() with the
   *        blocks augment_stream_stats_json() adds.
   */
  std::string stream_stats_payload(const stream_stats::stats_t &stats);

  /**
   * @brief What the first-run GPU step shows: each GPU, the encoder Polaris will use and why,
   *        the encoders that can start, and what is missing for hardware encoding.
   */
  nlohmann::json setup_hardware_report();

  /**
   * @brief The private LAN networks the first-run network step can trust with one click.
   */
  nlohmann::json setup_networks_report();

  /**
   * @brief The app uuid in a Remove artwork or Find artwork again request.
   *
   * The body is a JSON object with exactly one key, uuid, holding an app uuid. Anything else,
   * including a body over 1024 bytes, is refused.
   */
  std::optional<std::string> decode_app_artwork_request(std::string_view body);

  /**
   * @brief The games a cover sweep should ask a provider about.
   *
   * An entry qualifies when it has no cover, is not one of the entries Polaris ships artwork for, and
   * has not had automatic artwork lookup turned off. That last one is a choice somebody made with
   * Remove artwork, and a sweep that quietly undid it would be worse than no sweep.
   *
   * "No cover" means validation answers with the generic box art, which is what it does both for an
   * empty path and for a path it cannot use. `hydrated` is the app list as the console reads it, so a
   * Lutris entry whose image is filled in at read time counts as having one.
   */
  std::vector<artwork_sweep::candidate_t> games_without_a_cover(
    const std::filesystem::path &appdata,
    const nlohmann::json &hydrated,
    const std::vector<proc::ctx_t> &apps
  );

  /**
   * @brief Store a cover picked from a search's preview as `<uuid>.<ext>` under coverdir.
   *
   * Only a PNG, JPEG or WebP whose bytes match the given type is stored, under a validated uuid,
   * through a temporary file; the same uuid's covers in other formats are removed, except `keep`,
   * the image the entry still names, because a pick the player abandons must not delete their cover.
   * @return The stored path, or nothing when the image or the write is refused.
   */
  std::optional<std::string> store_selected_cover(
    const std::filesystem::path &coverdir,
    std::string_view uuid,
    std::string_view mime_type,
    const std::vector<unsigned char> &body,
    const std::filesystem::path &keep = {}
  );

  /**
   * @brief The entity tag a console cover image is served with.
   *
   * Built from the image's path, size and modification time, so a new cover for an entry, in a
   * new file or rewritten in place, is fetched again under the same name-keyed URL.
   * @return A quoted tag, or nothing when the file cannot be read.
   */
  std::optional<std::string> cover_image_etag(const std::filesystem::path &image);

  /**
   * @brief Publish a Heroic launcher entry once a Heroic game has been imported, unless one exists.
   */
  void ensure_heroic_library_app(nlohmann::json &file_tree, game_library::launcher_install_t install);

  /**
   * @brief The uuids of the apps in an apps.json tree whose automatic artwork lookup is off.
   */
  nlohmann::json apps_with_artwork_lookup_off(const std::filesystem::path &appdata, const nlohmann::json &apps_tree);

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
