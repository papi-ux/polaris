/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#ifndef BOOST_PROCESS_VERSION
 #define BOOST_PROCESS_VERSION 1
#endif
// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

// local includes
#include "client_profiles.h"
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "confighttp.h"
#include "process.h"
#include "httpcommon.h"
#include "input.h"
#include "system_tray.h"
#include "stream.h"
#include "utility.h"
#include "video.h"
#include "uuid.h"

#ifdef _WIN32
  // from_utf8() string conversion function
  #include "platform/windows/misc.h"
  #include "platform/windows/utils.h"
  #include <Windows.h>

  // _SH constants for _wfsopen()
  #include <share.h>
#elif __linux__
  #include "platform/linux/virtual_display.h"
  #include "platform/linux/session_manager.h"
  #include "platform/linux/cage_display_router.h"
  #include "platform/linux/stream_display_policy.h"
  #include "platform/linux/input/inputtino_gamepad_isolation.h"
  #include <dirent.h>
  #include <fcntl.h>
  #include <signal.h>
  #include <poll.h>
  #include <sys/stat.h>
  #include <sys/syscall.h>
  #include <unistd.h>
#elif __APPLE__
  #include <mach-o/dyld.h>
#endif

#include "device_db.h"
#include "ai_optimizer.h"
#include "stream_stats.h"
#include "game_classifier.h"

#define DEFAULT_APP_IMAGE_PATH POLARIS_ASSETS_DIR "/box.png"

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  proc_t proc;

  int input_only_app_id = -1;
  std::string input_only_app_id_str;
  int terminate_app_id = -1;
  std::string terminate_app_id_str;

  session_stop_outcome_t evaluate_session_stop_request(
    bool can_launch,
    bool has_running_app,
    int active_sessions,
    bool owned_by_client,
    rtsp_stream::session_role_e requester_role,
    bool stop_in_progress,
    bool token_matches
  ) {
    if (!can_launch) {
      return session_stop_outcome_t::permission_denied;
    }
    if (stop_in_progress) {
      return session_stop_outcome_t::stop_in_progress;
    }
    if (!has_running_app && active_sessions <= 0) {
      return session_stop_outcome_t::no_active_session;
    }
    if (requester_role == rtsp_stream::session_role_e::viewer) {
      return session_stop_outcome_t::viewer_forbidden;
    }
    if (has_running_app && !owned_by_client) {
      return session_stop_outcome_t::other_owner;
    }
    if (!has_running_app && requester_role != rtsp_stream::session_role_e::controller) {
      return session_stop_outcome_t::uncontrolled_stream;
    }
    if (!token_matches) {
      return session_stop_outcome_t::token_mismatch;
    }
    return session_stop_outcome_t::allowed;
  }

  bool session_token_matches_active(std::string_view expected_token, std::string_view active_token) {
    return !expected_token.empty() &&
           !active_token.empty() &&
           crypto::constant_time_equals(expected_token, active_token);
  }

  bool session_stop_token_matches(
    bool require_exact_token,
    std::string_view expected_token,
    std::string_view active_token,
    const std::vector<std::string> &requester_rtsp_tokens
  ) {
    if (!require_exact_token) {
      return true;
    }
    if (session_token_matches_active(expected_token, active_token)) {
      return true;
    }
    return std::any_of(
      requester_rtsp_tokens.begin(),
      requester_rtsp_tokens.end(),
      [expected_token](const std::string &candidate) {
        return session_token_matches_active(expected_token, candidate);
      }
    );
  }

  void session_lifecycle_gate_t::begin_launch() {
    std::unique_lock lock(_mutex);
    _changed.wait(lock, [this]() {
      return _state == state_e::idle && !_stop_waiting && !_launch_to_stop_handoff;
    });
    _state = state_e::launching;
  }

  std::optional<std::uint64_t> session_lifecycle_gate_t::capture_launch_generation() const {
    std::unique_lock lock(_mutex);
    _changed.wait(lock, [this]() {
      return _state != state_e::snapshotting && !_launch_to_stop_handoff;
    });
    if (_state != state_e::idle || _stop_waiting) {
      return std::nullopt;
    }
    return _generation;
  }

  bool session_lifecycle_gate_t::try_begin_rtsp_launch() {
    std::unique_lock lock(_mutex);
    _changed.wait(lock, [this]() {
      return _state != state_e::snapshotting && !_launch_to_stop_handoff;
    });
    if (_state != state_e::idle || _stop_waiting) {
      return false;
    }
    _state = state_e::launching;
    return true;
  }

  bool session_lifecycle_gate_t::try_begin_rtsp_launch(std::uint64_t expected_generation) {
    std::unique_lock lock(_mutex);
    _changed.wait(lock, [this]() {
      return _state != state_e::snapshotting && !_launch_to_stop_handoff;
    });
    if (_state != state_e::idle || _stop_waiting || _generation != expected_generation) {
      return false;
    }
    _state = state_e::launching;
    return true;
  }

  bool session_lifecycle_gate_t::begin_stop() {
    std::unique_lock lock(_mutex);
    _changed.wait(lock, [this]() {
      return _state != state_e::snapshotting && !_launch_to_stop_handoff;
    });
    if (_state == state_e::stopping || _stop_waiting) {
      return false;
    }
    _stop_waiting = true;
    _last_stop_committed = false;
    ++_generation;
    _changed.notify_all();
    _changed.wait(lock, [this]() {
      return _state != state_e::launching;
    });
    _stop_waiting = false;
    _state = state_e::stopping;
    return true;
  }

  bool session_lifecycle_gate_t::transition_launch_to_stop() {
    std::unique_lock lock(_mutex);
    if (_state != state_e::launching) {
      return false;
    }
    if (_stop_waiting) {
      _launch_to_stop_handoff = true;
      _state = state_e::idle;
      _changed.notify_all();
      _changed.wait(lock, [this]() {
        return _state == state_e::idle && !_stop_waiting;
      });
      if (_last_stop_committed) {
        _launch_to_stop_handoff = false;
        _changed.notify_all();
        return false;
      }
      ++_generation;
      _state = state_e::stopping;
      _launch_to_stop_handoff = false;
      _changed.notify_all();
      return true;
    }
    ++_generation;
    _state = state_e::stopping;
    return true;
  }

  void session_lifecycle_gate_t::begin_snapshot() {
    std::unique_lock lock(_mutex);
    _changed.wait(lock, [this]() {
      return _state == state_e::idle && !_stop_waiting && !_launch_to_stop_handoff;
    });
    _state = state_e::snapshotting;
  }

  bool session_lifecycle_gate_t::try_begin_snapshot() {
    std::unique_lock lock(_mutex);
    _changed.wait(lock, [this]() {
      return _stop_waiting ||
             _launch_to_stop_handoff ||
             (_state != state_e::launching && _state != state_e::snapshotting);
    });
    if (_state != state_e::idle || _stop_waiting || _launch_to_stop_handoff) {
      return false;
    }
    _state = state_e::snapshotting;
    return true;
  }

  void session_lifecycle_gate_t::finish_snapshot() {
    std::lock_guard lock(_mutex);
    if (_state == state_e::snapshotting) {
      _state = state_e::idle;
      _changed.notify_all();
    }
  }

  session_snapshot_guard_t::session_snapshot_guard_t(session_lifecycle_gate_t &gate):
      _gate(&gate) {
    _gate->begin_snapshot();
  }

  session_snapshot_guard_t session_snapshot_guard_t::try_acquire(session_lifecycle_gate_t &gate) {
    session_snapshot_guard_t guard;
    if (gate.try_begin_snapshot()) {
      guard._gate = &gate;
    }
    return guard;
  }

  bool session_snapshot_guard_t::owns_snapshot() const {
    return _gate != nullptr;
  }

  session_snapshot_guard_t::~session_snapshot_guard_t() {
    if (_gate) {
      _gate->finish_snapshot();
    }
  }

  session_snapshot_guard_t::session_snapshot_guard_t(session_snapshot_guard_t &&other) noexcept:
      _gate(std::exchange(other._gate, nullptr)) {}

  void session_lifecycle_gate_t::finish_launch() {
    std::lock_guard lock(_mutex);
    if (_state == state_e::launching) {
      _state = state_e::idle;
      _changed.notify_all();
    }
  }

  void session_lifecycle_gate_t::finish_stop(bool committed) {
    std::lock_guard lock(_mutex);
    if (_state == state_e::stopping) {
      _last_stop_committed = committed;
      _state = state_e::idle;
      _changed.notify_all();
    }
  }

  bool session_lifecycle_gate_t::stop_in_progress() const {
    std::lock_guard lock(_mutex);
    return _state == state_e::stopping || _stop_waiting || _launch_to_stop_handoff;
  }

  std::string normalize_steam_launch_mode(std::string mode) {
    boost::trim(mode);
    boost::to_lower(mode);
    if (mode == "big-picture" || mode == "big_picture" || mode == "bigpicture" || mode == "gamepadui") {
      return std::string {STEAM_LAUNCH_MODE_BIG_PICTURE};
    }
    return std::string {STEAM_LAUNCH_MODE_DIRECT};
  }

  bool is_valid_steam_launch_mode(std::string_view mode) {
    const auto raw = boost::to_lower_copy(boost::trim_copy(std::string {mode}));
    return raw == "direct" ||
           raw == "big-picture" ||
           raw == "big_picture" ||
           raw == "bigpicture" ||
           raw == "gamepadui";
  }

  bool steam_launch_mode_is_big_picture(std::string_view mode) {
    return normalize_steam_launch_mode(std::string {mode}) == STEAM_LAUNCH_MODE_BIG_PICTURE;
  }

  namespace {
    bool should_publish_stream_ended_after_terminate(bool had_running_app, int active_sessions, std::string_view session_state) {
      return had_running_app &&
             active_sessions == 0 &&
             session_state == "paused"sv;
    }

    struct host_pause_session_classification_t {
      bool network_risk = false;
      bool pacing_risk = false;
      bool capture_fallback = false;
      bool encoder_risk = false;
      bool hdr_risk = false;
      bool decoder_risk = false;
      bool virtual_display_risk = false;
      bool host_render_limited = false;
      bool av1_codec = false;
      std::string capture_path;
      std::string codec_lower;
      std::string primary_issue = "steady";
      std::vector<std::string> issues;
      std::string health_grade = "good";
    };

    host_pause_session_classification_t classify_host_pause_session(
        const stream_stats::stats_t &stats,
        double target_fps,
        bool current_virtual_display) {
      host_pause_session_classification_t classification;
      const bool meaningful_fps_shortfall =
        stream_stats::is_meaningful_fps_shortfall(target_fps, stats.fps);
      classification.network_risk = stats.packet_loss >= 0.35 || stats.latency_ms >= 28.0;
      classification.pacing_risk =
        stats.frame_jitter_ms >= 2.2 ||
        stats.duplicate_frame_ratio >= 0.10 ||
        stats.dropped_frame_ratio >= 0.04 ||
        meaningful_fps_shortfall;
      classification.capture_fallback = stream_stats::capture_path_uses_cpu_copy(stats);
      classification.capture_path = stream_stats::capture_path_summary(stats);
      classification.encoder_risk = stats.encode_time_ms >= 11.0 || stats.avg_frame_age_ms >= 18.0;
      classification.hdr_risk = stats.dynamic_range > 0 &&
                                (classification.pacing_risk || classification.encoder_risk);
      classification.codec_lower = boost::algorithm::to_lower_copy(stats.codec);
      classification.av1_codec = classification.codec_lower.find("av1") != std::string::npos;
      classification.decoder_risk = classification.av1_codec &&
                                    classification.pacing_risk &&
                                    !classification.network_risk;
      classification.virtual_display_risk = current_virtual_display &&
                                            (classification.pacing_risk ||
                                             classification.capture_fallback ||
                                             classification.hdr_risk);
      classification.host_render_limited =
        classification.pacing_risk &&
        !classification.network_risk &&
        !classification.capture_fallback &&
        !classification.encoder_risk &&
        !classification.hdr_risk &&
        !classification.decoder_risk &&
        !classification.virtual_display_risk &&
        (meaningful_fps_shortfall ||
         stats.duplicate_frame_ratio >= 0.08 ||
         stats.dropped_frame_ratio >= 0.03);

      if (classification.network_risk) classification.primary_issue = "network_jitter";
      else if (classification.hdr_risk) classification.primary_issue = "hdr_path";
      else if (classification.virtual_display_risk) classification.primary_issue = "virtual_display_path";
      else if (classification.decoder_risk) classification.primary_issue = "decoder_path";
      else if (classification.capture_fallback) classification.primary_issue = "capture_fallback";
      else if (classification.host_render_limited) classification.primary_issue = "host_render_limited";
      else if (classification.pacing_risk) classification.primary_issue = "frame_pacing";
      else if (classification.encoder_risk) classification.primary_issue = "encoder_load";

      if (classification.network_risk) classification.issues.push_back("network_jitter");
      if (classification.host_render_limited) classification.issues.push_back("host_render_limited");
      else if (classification.pacing_risk) classification.issues.push_back("frame_pacing");
      if (classification.capture_fallback) classification.issues.push_back("capture_fallback");
      if (classification.encoder_risk) classification.issues.push_back("encoder_load");
      if (classification.hdr_risk) classification.issues.push_back("hdr_path");
      if (classification.decoder_risk) classification.issues.push_back("decoder_path");
      if (classification.virtual_display_risk) classification.issues.push_back("virtual_display_path");

      const int concern_count =
        static_cast<int>(classification.network_risk) +
        static_cast<int>(classification.pacing_risk) +
        static_cast<int>(classification.capture_fallback) +
        static_cast<int>(classification.encoder_risk) +
        static_cast<int>(classification.hdr_risk) +
        static_cast<int>(classification.decoder_risk) +
        static_cast<int>(classification.virtual_display_risk);
      classification.health_grade =
        concern_count >= 2 || classification.hdr_risk || classification.decoder_risk ? "degraded" :
        concern_count == 1 ? "watch" :
        "good";
      return classification;
    }

    void publish_stream_ended_after_terminate_if_needed(bool had_running_app) {
      if (!should_publish_stream_ended_after_terminate(had_running_app, stream::session::active_count(), confighttp::get_session_state())) {
        return;
      }

      confighttp::set_session_state(confighttp::session_state_e::idle);
      confighttp::emit_session_event("stream_ended", "All sessions ended");
    }

    std::string generate_session_token() {
      std::array<unsigned char, 16> raw {};
      RAND_bytes(raw.data(), raw.size());
      return util::hex_vec(raw, true);
    }

    std::optional<std::filesystem::path> executable_dir() {
#ifdef _WIN32
      wchar_t path[MAX_PATH];
      const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
      if (len == 0 || len == MAX_PATH) {
        return std::nullopt;
      }
      return std::filesystem::path(path).parent_path();
#elif __linux__
      std::array<char, 4096> path {};
      const auto len = readlink("/proc/self/exe", path.data(), path.size() - 1);
      if (len <= 0) {
        return std::nullopt;
      }
      path[len] = '\0';
      return std::filesystem::path(path.data()).parent_path();
#elif __APPLE__
      uint32_t size = 0;
      _NSGetExecutablePath(nullptr, &size);
      std::string buffer(size, '\0');
      if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
      }
      return std::filesystem::path(buffer.c_str()).parent_path();
#else
      return std::nullopt;
#endif
    }

    std::string resolve_bundled_asset(const std::string &asset_name) {
      const auto installed_path = std::filesystem::path(POLARIS_ASSETS_DIR) / asset_name;
      if (std::filesystem::exists(installed_path)) {
        return installed_path.string();
      }

      const auto exe_dir = executable_dir();
      if (exe_dir) {
        const auto local_build_path = *exe_dir / "assets" / asset_name;
        if (std::filesystem::exists(local_build_path)) {
          return local_build_path.string();
        }
      }

      return installed_path.string();
    }

    bool has_supported_image_signature(const std::filesystem::path &path, const std::string &extension) {
      std::ifstream input(path, std::ios::binary);
      if (!input) {
        return false;
      }

      std::array<unsigned char, 16> header {};
      input.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
      const auto bytes_read = static_cast<size_t>(input.gcount());

      if (extension == ".png") {
        static constexpr std::array<unsigned char, 8> png_signature {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        return bytes_read >= png_signature.size() &&
               std::equal(png_signature.begin(), png_signature.end(), header.begin());
      }

      if (extension == ".jpg" || extension == ".jpeg") {
        return bytes_read >= 3 &&
               header[0] == 0xFF &&
               header[1] == 0xD8 &&
               header[2] == 0xFF;
      }

      if (extension == ".webp") {
        static constexpr std::array<unsigned char, 4> riff_signature {'R', 'I', 'F', 'F'};
        static constexpr std::array<unsigned char, 4> webp_signature {'W', 'E', 'B', 'P'};
        return bytes_read >= 12 &&
               std::equal(riff_signature.begin(), riff_signature.end(), header.begin()) &&
               std::equal(webp_signature.begin(), webp_signature.end(), header.begin() + 8);
      }

      return false;
    }

    bool should_log_invalid_app_image_once(const std::string &path) {
      static std::mutex mutex;
      static std::unordered_set<std::string> logged_paths;

      std::lock_guard<std::mutex> lock(mutex);
      return logged_paths.emplace(path).second;
    }

    std::string json_app_label(const nlohmann::json &app) {
      if (app.is_object()) {
        if (const auto it = app.find("name"); it != app.end() && it->is_string()) {
          return it->get<std::string>();
        }
        if (const auto it = app.find("uuid"); it != app.end() && it->is_string()) {
          return it->get<std::string>();
        }
      }

      return "(unnamed)";
    }

    std::string normalized_json_string(const std::string &value) {
      auto normalized = boost::algorithm::trim_copy(value);
      boost::algorithm::to_lower(normalized);
      return normalized;
    }

    std::string json_string_member_or(const nlohmann::json &node, const char *key, const std::string &default_value = {}) {
      if (!node.is_object()) {
        return default_value;
      }

      const auto it = node.find(key);
      if (it == node.end() || !it->is_string()) {
        return default_value;
      }

      return it->get<std::string>();
    }

    bool is_steam_big_picture_name(const std::string &name) {
      return boost::iequals(boost::trim_copy(name), "Steam Big Picture");
    }

    bool is_lutris_library_app(const nlohmann::json &app) {
      const auto app_name = json_string_member_or(app, "name");
      if (boost::iequals(boost::trim_copy(app_name), "Lutris")) {
        return true;
      }

      const auto cmd = boost::trim_copy(json_string_member_or(app, "cmd"));
      if (boost::iequals(cmd, "setsid lutris") || boost::iequals(cmd, "lutris")) {
        return true;
      }

      if (!app.contains("detached") || !app["detached"].is_array()) {
        return false;
      }

      for (const auto &detached : app["detached"]) {
        if (!detached.is_string()) {
          continue;
        }
        const auto value = boost::trim_copy(detached.get<std::string>());
        if (boost::iequals(value, "setsid lutris") || boost::iequals(value, "lutris")) {
          return true;
        }
      }

      return false;
    }

    bool has_lutris_game_app(const nlohmann::json &app) {
      const auto source = json_string_member_or(app, "source");
      if (!boost::iequals(source, "lutris")) {
        return false;
      }

      return !json_string_member_or(app, "lutris-slug").empty();
    }

    nlohmann::json lutris_library_app() {
      return {
        {"name", "Lutris"},
        {"uuid", ""},
        {"cmd", ""},
        {"detached", {"setsid lutris"}},
        {"image-path", "lutris.png"},
        {"source", "lutris"},
        {"auto-detach", true},
        {"wait-all", true},
        {"exit-timeout", 5}
      };
    }

    bool command_contains_steam_big_picture_open(const std::string &cmd) {
      return boost::icontains(cmd, "steam://open/bigpicture");
    }

    bool command_contains_steam_big_picture_close(const std::string &cmd) {
      return boost::icontains(cmd, "steam://close/bigpicture");
    }

    bool command_contains_steam_game_launch_uri(const std::string &cmd) {
      return boost::icontains(cmd, "steam://rungameid/");
    }

    bool command_contains_steam_applaunch(const std::string &cmd) {
      return boost::icontains(cmd, "-applaunch");
    }

    bool command_contains_steam_gamepadui_flag(const std::string &cmd) {
      return boost::icontains(cmd, "steam -gamepadui");
    }

    bool command_uses_steam_gamepadui_flag(const std::string &cmd) {
      return command_contains_steam_gamepadui_flag(cmd) &&
             !command_contains_steam_applaunch(cmd) &&
             !command_contains_steam_game_launch_uri(cmd);
    }

    bool command_targets_steam_big_picture(const std::string &cmd) {
      return command_contains_steam_big_picture_open(cmd) ||
             command_contains_steam_big_picture_close(cmd) ||
             command_uses_steam_gamepadui_flag(cmd);
    }

    bool daemon_shutdown_requested() {
      if (!mail::man) {
        return false;
      }

      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      return shutdown_event && shutdown_event->peek();
    }

    bool is_steam_big_picture_app(const proc::ctx_t &ctx) {
      if (is_steam_big_picture_name(ctx.name)) {
        return true;
      }

      if ((!ctx.steam_appid.empty() || boost::iequals(ctx.source, "steam")) &&
          !is_steam_big_picture_name(ctx.name)) {
        return false;
      }

      if (command_targets_steam_big_picture(ctx.cmd)) {
        return true;
      }

      if (std::any_of(ctx.detached.begin(), ctx.detached.end(), command_targets_steam_big_picture)) {
        return true;
      }

      return std::any_of(ctx.prep_cmds.begin(), ctx.prep_cmds.end(), [](const proc::cmd_t &cmd) {
        return command_targets_steam_big_picture(cmd.do_cmd) ||
               command_targets_steam_big_picture(cmd.undo_cmd);
      });
    }

    std::string steam_big_picture_command_prefix(const std::string &cmd) {
      return boost::istarts_with(boost::trim_copy(cmd), "setsid ") ? "setsid " : "";
    }

    [[maybe_unused]] std::string canonical_steam_big_picture_command(const std::string &reference_cmd, std::string_view action) {
      return steam_big_picture_command_prefix(reference_cmd) + "steam steam://" + std::string(action);
    }

    std::string canonical_steam_shutdown_command(const std::string &reference_cmd) {
#ifdef __linux__
      if (boost::istarts_with(boost::trim_copy(reference_cmd), "setsid ")) {
        return "setsid -f steam -shutdown";
      }
#endif
      return steam_big_picture_command_prefix(reference_cmd) + "steam -shutdown";
    }

    std::string canonical_steam_library_bootstrap_command(const std::string &reference_cmd) {
      return steam_big_picture_command_prefix(reference_cmd.empty() ? "steam" : reference_cmd) + "steam -gamepadui";
    }

    std::optional<std::string> extract_steam_appid_from_command(const std::string &cmd) {
      auto extract_digits = [&](size_t pos) -> std::optional<std::string> {
        while (pos < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[pos]))) {
          ++pos;
        }

        size_t start = pos;
        while (pos < cmd.size() && std::isdigit(static_cast<unsigned char>(cmd[pos]))) {
          ++pos;
        }

        if (start == pos) {
          return std::nullopt;
        }

        return cmd.substr(start, pos - start);
      };

      std::string lower = boost::to_lower_copy(cmd);

      if (auto pos = lower.find("steam://rungameid/"); pos != std::string::npos) {
        return extract_digits(pos + std::string("steam://rungameid/").size());
      }

      if (auto pos = lower.find("-applaunch"); pos != std::string::npos) {
        return extract_digits(pos + std::string("-applaunch").size());
      }

      return std::nullopt;
    }

    std::string canonical_steam_library_big_picture_followup_command(const std::string &reference_cmd, const std::string &appid) {
      return steam_big_picture_command_prefix(reference_cmd.empty() ? "steam" : reference_cmd) +
        "bash -lc \"sleep 6; steam steam://rungameid/" + appid +
        " >/dev/null 2>&1 || true; sleep 4; exec steam -applaunch " + appid +
        " >/dev/null 2>&1 || true\"";
    }

    std::vector<std::string> canonical_steam_library_launch_commands(
      const std::string &reference_cmd,
      const std::string &appid,
      const std::string &mode = std::string {proc::STEAM_LAUNCH_MODE_DIRECT}
    ) {
      if (proc::steam_launch_mode_is_big_picture(mode)) {
        return {
          canonical_steam_library_bootstrap_command(reference_cmd),
          canonical_steam_library_big_picture_followup_command(reference_cmd, appid)
        };
      }

      return {
        steam_big_picture_command_prefix(reference_cmd.empty() ? "steam" : reference_cmd) +
          "steam steam://rungameid/" + appid
      };
    }

    bool command_is_steam_library_launch_component(const std::string &cmd) {
      return command_contains_steam_game_launch_uri(cmd) ||
             command_contains_steam_applaunch(cmd) ||
             command_contains_steam_gamepadui_flag(cmd);
    }

    bool command_requests_steam_shutdown(const std::string &cmd) {
      return boost::icontains(cmd, "steam -shutdown");
    }

#ifdef __linux__
    std::string strip_leading_setsid_for_cage(std::string cmd) {
      auto trimmed = boost::trim_left_copy(cmd);
      if (!boost::istarts_with(trimmed, "setsid")) {
        return cmd;
      }

      if (trimmed.size() > 6 && !std::isspace(static_cast<unsigned char>(trimmed[6]))) {
        return cmd;
      }

      auto rest = trimmed.substr(std::min<size_t>(trimmed.size(), 6));
      boost::trim_left(rest);

      if (boost::istarts_with(rest, "-f")) {
        if (rest.size() == 2 || std::isspace(static_cast<unsigned char>(rest[2]))) {
          rest = rest.substr(std::min<size_t>(rest.size(), 2));
          boost::trim_left(rest);
        }
      } else if (boost::istarts_with(rest, "--fork")) {
        if (rest.size() == 6 || std::isspace(static_cast<unsigned char>(rest[6]))) {
          rest = rest.substr(std::min<size_t>(rest.size(), 6));
          boost::trim_left(rest);
        }
      }

      return rest.empty() ? cmd : rest;
    }

    std::string cage_runtime_command(const std::string &cmd) {
      auto sanitized = strip_leading_setsid_for_cage(cmd);
      if (sanitized != cmd) {
        BOOST_LOG(info) << "process: stripped leading setsid for cage runtime command ["
                        << cmd << "] -> [" << sanitized << ']';
      }
      return sanitized;
    }

    bool proc_environ_contains_exact_entry(
      std::string_view environ,
      std::string_view key,
      std::string_view value
    ) {
      if (key.empty() || value.empty()) {
        return false;
      }
      const auto expected = std::string {key} + "=" + std::string {value};
      std::size_t start = 0;
      while (start < environ.size()) {
        const auto end = environ.find('\0', start);
        if (end == std::string_view::npos) {
          return false;
        }
        if (environ.substr(start, end - start) == expected) {
          return true;
        }
        start = end + 1;
      }
      return false;
    }

    bool proc_environ_matches_isolated_session(
      std::string_view environ,
      std::string_view session_instance_id
    ) {
      return proc_environ_contains_exact_entry(
        environ,
        "POLARIS_SESSION_INSTANCE_ID"sv,
        session_instance_id
      );
    }

    std::string steam_appid_for_context(const proc::ctx_t &ctx) {
      if (!ctx.steam_appid.empty()) {
        return ctx.steam_appid;
      }

      if (auto appid = extract_steam_appid_from_command(ctx.cmd)) {
        return *appid;
      }

      for (const auto &cmd : ctx.detached) {
        if (auto appid = extract_steam_appid_from_command(cmd)) {
          return *appid;
        }
      }

      return {};
    }

#endif

    bool prep_cmd_undo_stops_steam(const proc::cmd_t &cmd) {
      return command_contains_steam_big_picture_close(cmd.undo_cmd) ||
             command_requests_steam_shutdown(cmd.undo_cmd);
    }

    std::string steam_launch_reference_command(const proc::ctx_t &ctx) {
      if (!ctx.detached.empty()) {
        return ctx.detached.front();
      }

      if (!ctx.cmd.empty()) {
        return ctx.cmd;
      }

      return "steam";
    }

    void ensure_steam_cleanup_undo(proc::ctx_t &ctx, const char *label) {
      if (std::any_of(ctx.prep_cmds.begin(), ctx.prep_cmds.end(), prep_cmd_undo_stops_steam)) {
        return;
      }

      auto shutdown_cmd = canonical_steam_shutdown_command(steam_launch_reference_command(ctx));
      BOOST_LOG(info) << "process: added " << label << " cleanup undo command [" << shutdown_cmd << ']';
      ctx.prep_cmds.emplace_back(""s, std::move(shutdown_cmd), false);
    }

    bool is_steam_library_app(const proc::ctx_t &ctx) {
      if (is_steam_big_picture_app(ctx)) {
        return false;
      }

      if (!ctx.steam_appid.empty() || boost::iequals(ctx.source, "steam")) {
        return true;
      }

      if (command_contains_steam_game_launch_uri(ctx.cmd) || command_contains_steam_applaunch(ctx.cmd)) {
        return true;
      }

      return std::any_of(ctx.detached.begin(), ctx.detached.end(), [](const std::string &cmd) {
        return command_contains_steam_game_launch_uri(cmd) || command_contains_steam_applaunch(cmd);
      });
    }

#ifdef __linux__
    bool context_uses_steam(const proc::ctx_t &ctx) {
      return is_steam_big_picture_app(ctx) ||
             is_steam_library_app(ctx) ||
             !steam_appid_for_context(ctx).empty();
    }

    bool isolated_session_generation_blocks_launch(
      bool session_owned_cage,
      bool generation_available
    ) {
      return session_owned_cage || generation_available;
    }

    bool isolated_session_cleanup_resets_router(
      bool session_owned_cage,
      bool generation_available,
      bool exact_cleanup_complete
    ) {
      return session_owned_cage && generation_available && exact_cleanup_complete;
    }

    bool isolated_session_cleanup_clears_state(
      bool session_owned_cage,
      bool generation_available,
      bool exact_cleanup_complete
    ) {
      return !session_owned_cage || (generation_available && exact_cleanup_complete);
    }

    bool isolated_session_uses_legacy_group_termination(
      bool session_owned_cage,
      bool immediate
    ) {
      return !session_owned_cage && !immediate;
    }

    bool isolated_session_detaches_legacy_handles(bool session_owned_cage) {
      return session_owned_cage;
    }

    bool should_terminate_session_owned_steam_before_cage_stop(
      const proc::ctx_t &ctx,
      bool session_owned_cage,
      bool session_generation_available,
      bool session_owned_steam_client_active,
      bool unowned_steam_client_active,
      bool ownership_capture_complete
    ) {
      return session_owned_cage &&
             session_generation_available &&
             context_uses_steam(ctx) &&
             session_owned_steam_client_active &&
             !unowned_steam_client_active &&
             ownership_capture_complete;
    }

    struct pidfd_handle_t {
      pid_t pid = -1;
      int fd = -1;

      pidfd_handle_t() = default;
      pidfd_handle_t(pid_t process_id, int descriptor): pid(process_id), fd(descriptor) {}
      pidfd_handle_t(const pidfd_handle_t &) = delete;
      pidfd_handle_t &operator=(const pidfd_handle_t &) = delete;
      pidfd_handle_t(pidfd_handle_t &&other) noexcept: pid(other.pid), fd(other.fd) {
        other.pid = -1;
        other.fd = -1;
      }
      pidfd_handle_t &operator=(pidfd_handle_t &&other) noexcept {
        if (this != &other) {
          if (fd >= 0) {
            close(fd);
          }
          pid = other.pid;
          fd = other.fd;
          other.pid = -1;
          other.fd = -1;
        }
        return *this;
      }
      ~pidfd_handle_t() {
        if (fd >= 0) {
          close(fd);
        }
      }
    };

#ifdef POLARIS_TESTS
    thread_local pid_t forced_pidfd_open_failure_pid = -1;
    thread_local std::atomic<bool> *pidfd_wait_entered_for_tests = nullptr;
#endif

    std::optional<pidfd_handle_t> open_process_pidfd(pid_t pid, int &open_error) {
#ifdef POLARIS_TESTS
      if (pid == forced_pidfd_open_failure_pid) {
        open_error = EACCES;
        return std::nullopt;
      }
#endif
      const auto fd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
      if (fd < 0) {
        open_error = errno;
        return std::nullopt;
      }
      open_error = 0;
      return pidfd_handle_t {pid, fd};
    }

#ifdef POLARIS_TESTS
    thread_local int forced_pidfd_zero_poll_interruptions = 0;
    thread_local std::chrono::milliseconds forced_pidfd_zero_poll_interruption_delay {0};
#endif

    int poll_pidfd_now(pollfd &descriptor) {
#ifdef POLARIS_TESTS
      if (forced_pidfd_zero_poll_interruptions > 0) {
        --forced_pidfd_zero_poll_interruptions;
        std::this_thread::sleep_for(forced_pidfd_zero_poll_interruption_delay);
        errno = EINTR;
        return -1;
      }
#endif
      return poll(&descriptor, 1, 0);
    }

    bool pidfd_has_exited(const pidfd_handle_t &handle) {
      pollfd descriptor {handle.fd, POLLIN, 0};
      const int result = poll_pidfd_now(descriptor);
      return result == 1 && (descriptor.revents & POLLIN) != 0;
    }

    bool wait_for_pidfds_exit(
      const std::vector<pidfd_handle_t> &handles,
      std::chrono::milliseconds timeout
    ) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      for (;;) {
        std::vector<pollfd> pending;
        for (const auto &handle : handles) {
          if (!pidfd_has_exited(handle)) {
            pending.emplace_back(pollfd {handle.fd, POLLIN, 0});
          }
        }
        if (pending.empty()) {
          return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
          return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto timeout_ms = static_cast<int>(std::max<std::int64_t>(1, remaining.count()));
#ifdef POLARIS_TESTS
        if (pidfd_wait_entered_for_tests != nullptr) {
          pidfd_wait_entered_for_tests->store(true, std::memory_order_release);
        }
#endif
        const int result = poll(pending.data(), pending.size(), timeout_ms);
        if (result < 0) {
          if (errno == EINTR) {
            continue;
          }
          return false;
        }
        if (result == 0) {
          return false;
        }
      }
    }

    bool send_pidfd_signal(const pidfd_handle_t &handle, int signal_number) {
      if (pidfd_has_exited(handle)) {
        return true;
      }
      if (syscall(SYS_pidfd_send_signal, handle.fd, signal_number, nullptr, 0) == 0) {
        return true;
      }
      return errno == ESRCH;
    }

    bool terminate_pidfds(
      std::vector<pidfd_handle_t> &handles,
      std::chrono::milliseconds graceful_timeout,
      std::chrono::milliseconds kill_timeout,
      std::string_view label
    ) {
      for (const auto &handle : handles) {
        if (!send_pidfd_signal(handle, SIGTERM)) {
          BOOST_LOG(warning) << "process: pidfd SIGTERM failed for "sv << label
                             << " pid="sv << handle.pid << " error="sv << std::strerror(errno);
        }
      }
      if (wait_for_pidfds_exit(handles, graceful_timeout)) {
        return true;
      }

      BOOST_LOG(warning) << "process: "sv << label
                         << " did not exit after pidfd SIGTERM; sending PID-bound SIGKILL"sv;
      for (const auto &handle : handles) {
        if (!send_pidfd_signal(handle, SIGKILL)) {
          BOOST_LOG(warning) << "process: pidfd SIGKILL failed for "sv << label
                             << " pid="sv << handle.pid << " error="sv << std::strerror(errno);
        }
      }
      return wait_for_pidfds_exit(handles, kill_timeout);
    }

    bool proc_pid_dir_name(std::string_view name) {
      return !name.empty() && std::all_of(name.begin(), name.end(), [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch));
      });
    }

    struct proc_file_read_result_t {
      std::string bytes;
      int error = 0;

      bool ok() const {
        return error == 0;
      }
    };

    proc_file_read_result_t read_proc_status_file_result(pid_t pid, std::string_view name) {
      const auto path = "/proc/" + std::to_string(pid) + "/" + std::string {name};
      const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        return {{}, errno};
      }
      auto close_fd = util::fail_guard([fd]() {
        close(fd);
      });

      std::string bytes;
      char chunk[4096];
      for (;;) {
        const auto bytes_read = read(fd, chunk, sizeof(chunk));
        if (bytes_read > 0) {
          bytes.append(chunk, static_cast<std::size_t>(bytes_read));
          continue;
        }
        if (bytes_read == 0) {
          return {std::move(bytes), 0};
        }
        const int error = errno;
        if (error == EINTR) {
          continue;
        }
        return {{}, error};
      }
    }

    std::string read_proc_status_file(pid_t pid, std::string_view name) {
      return read_proc_status_file_result(pid, name).bytes;
    }

    bool process_vanished_during_proc_read(int error) {
      return error == ENOENT || error == ESRCH;
    }

    std::string proc_argv0_from_cmdline(std::string_view cmdline) {
      const auto end = cmdline.find('\0');
      return boost::to_lower_copy(std::string {cmdline.substr(0, end)});
    }

    std::string basename_from_path(std::string_view path) {
      auto value = std::string {path};
      if (const auto slash = value.find_last_of('/'); slash != std::string::npos) {
        value = value.substr(slash + 1);
      }
      return value;
    }

    bool path_has_suffix(std::string_view value, std::string_view suffix) {
      return value.size() >= suffix.size() &&
             value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool is_desktop_steam_client_process(std::string_view comm, std::string_view argv0_path) {
      const auto argv0 = basename_from_path(argv0_path);
      return comm == "steam" ||
             comm == "steamwebhelper" ||
             argv0 == "steam" ||
             argv0 == "steam.sh" ||
             argv0 == "steamwebhelper" ||
             path_has_suffix(argv0_path, "/steam.sh") ||
             path_has_suffix(argv0_path, "/steamwebhelper") ||
             path_has_suffix(argv0_path, "/ubuntu12_32/steam");
    }

    bool is_transient_steam_client_cmdline(std::string_view cmdline) {
      const auto lower_cmdline = boost::to_lower_copy(std::string {cmdline});
      return lower_cmdline.find("-shutdown") != std::string::npos ||
             lower_cmdline.find("-child-update-ui") != std::string::npos ||
             lower_cmdline.find("-srt-logger-opened") != std::string::npos;
    }

    bool proc_status_is_zombie(std::string_view status) {
      for (size_t index = 0; index + 5 < status.size(); ++index) {
        if (static_cast<unsigned char>(status[index]) != 83 ||
            static_cast<unsigned char>(status[index + 1]) != 116 ||
            static_cast<unsigned char>(status[index + 2]) != 97 ||
            static_cast<unsigned char>(status[index + 3]) != 116 ||
            static_cast<unsigned char>(status[index + 4]) != 101 ||
            static_cast<unsigned char>(status[index + 5]) != 58) {
          continue;
        }

        auto value_pos = index + 6;
        while (value_pos < status.size() && std::isspace(static_cast<unsigned char>(status[value_pos]))) {
          ++value_pos;
        }

        return value_pos < status.size() && status[value_pos] == static_cast<char>(90);
      }

      return false;
    }

    bool is_active_steam_shutdown_ownership_process(
      std::string_view comm,
      std::string_view argv0_path,
      std::string_view cmdline,
      std::string_view status
    ) {
      const auto effective_argv0 = argv0_path.empty() ? proc_argv0_from_cmdline(cmdline) : std::string {argv0_path};
      return is_desktop_steam_client_process(comm, effective_argv0) &&
             !proc_status_is_zombie(status);
    }

#ifdef POLARIS_TESTS
    bool steam_shutdown_process_is_unowned_active(
      std::string_view comm,
      std::string_view argv0_path,
      std::string_view cmdline,
      std::string_view status,
      std::string_view environ,
      std::string_view session_instance_id
    ) {
      return is_active_steam_shutdown_ownership_process(comm, argv0_path, cmdline, status) &&
             !proc_environ_matches_isolated_session(environ, session_instance_id);
    }
#endif

    std::optional<std::uint64_t> proc_start_time_from_stat(std::string_view stat) {
      const auto comm_end = stat.rfind(')');
      if (comm_end == std::string_view::npos || comm_end + 1 >= stat.size()) {
        return std::nullopt;
      }

      std::istringstream fields(std::string {stat.substr(comm_end + 1)});
      std::string token;
      if (!(fields >> token)) {
        return std::nullopt;
      }
      for (int field = 4; field <= 21; ++field) {
        if (!(fields >> token)) {
          return std::nullopt;
        }
      }
      std::uint64_t start_time = 0;
      if (!(fields >> start_time)) {
        return std::nullopt;
      }
      return start_time;
    }

    std::optional<std::uint64_t> proc_start_time_ticks(pid_t pid) {
      return proc_start_time_from_stat(read_proc_status_file(pid, "stat"));
    }

    std::optional<pid_t> proc_parent_pid_from_stat(std::string_view stat) {
      const auto comm_end = stat.rfind(')');
      if (comm_end == std::string_view::npos || comm_end + 1 >= stat.size()) {
        return std::nullopt;
      }
      std::istringstream fields(std::string {stat.substr(comm_end + 1)});
      std::string state;
      pid_t parent_pid = -1;
      if (!(fields >> state >> parent_pid) || parent_pid < 0) {
        return std::nullopt;
      }
      return parent_pid;
    }

#ifdef POLARIS_TESTS
    thread_local pid_t forced_proc_ancestry_unknown_pid = -1;
#endif

    std::optional<bool> proc_pid_descends_from(pid_t pid, pid_t ancestor_pid) {
      std::set<pid_t> visited;
      auto current = pid;
      while (current > 1) {
        if (!visited.insert(current).second) {
          return std::nullopt;
        }
#ifdef POLARIS_TESTS
        if (current == forced_proc_ancestry_unknown_pid) {
          return std::nullopt;
        }
#endif
        const auto parent = proc_parent_pid_from_stat(read_proc_status_file(current, "stat"));
        if (!parent) {
          return std::nullopt;
        }
        if (*parent == ancestor_pid) {
          return true;
        }
        current = *parent;
      }
      return false;
    }

    bool steam_pidfd_capture_identity_matches(
      const std::optional<std::uint64_t> &before,
      const std::optional<std::uint64_t> &after
    ) {
      return before && after && *before == *after;
    }

    struct isolated_session_process_snapshot_t {
      std::vector<pidfd_handle_t> owned;
      bool capture_complete = true;
    };

#ifdef POLARIS_TESTS
    thread_local pid_t forced_isolated_session_capture_failure_pid = -1;
    thread_local bool forced_proc_directory_enumeration_error = false;
    thread_local pid_t forced_proc_directory_error_after_capture_pid = -1;
    thread_local pid_t forced_reused_exact_generation_pid = -1;
    thread_local pid_t forced_steam_ownership_capture_failure_pid = -1;
#endif

    dirent *read_next_proc_entry(DIR *directory) {
      errno = 0;
#ifdef POLARIS_TESTS
      if (forced_proc_directory_enumeration_error) {
        forced_proc_directory_enumeration_error = false;
        errno = EIO;
        return nullptr;
      }
#endif
      return readdir(directory);
    }

    isolated_session_process_snapshot_t isolated_session_process_snapshot(
      std::string_view session_instance_id
    ) {
      isolated_session_process_snapshot_t snapshot;
      if (session_instance_id.empty()) {
        snapshot.capture_complete = false;
        return snapshot;
      }

      DIR *dir = opendir("/proc");
      if (!dir) {
        snapshot.capture_complete = false;
        return snapshot;
      }

      for (;;) {
        auto *entry = read_next_proc_entry(dir);
        if (entry == nullptr) {
          if (errno != 0) {
            snapshot.capture_complete = false;
          }
          break;
        }
        const std::string name = entry->d_name;
        if (!proc_pid_dir_name(name)) {
          continue;
        }
        const auto pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
        if (pid <= 1 || pid == getpid()) {
          continue;
        }

        const auto environ_before = read_proc_status_file_result(pid, "environ");
        if (!environ_before.ok()) {
          if (process_vanished_during_proc_read(environ_before.error) ||
              proc_status_is_zombie(read_proc_status_file(pid, "status"))) {
            continue;
          }
          const auto descends_from_polaris = proc_pid_descends_from(pid, getpid());
          if (!descends_from_polaris || *descends_from_polaris) {
            snapshot.capture_complete = false;
          }
          continue;
        }
        if (!proc_environ_matches_isolated_session(environ_before.bytes, session_instance_id)) {
          continue;
        }
#ifdef POLARIS_TESTS
        if (pid == forced_isolated_session_capture_failure_pid) {
          snapshot.capture_complete = false;
          continue;
        }
#endif
        const auto identity_before = proc_start_time_ticks(pid);
        if (!identity_before) {
          snapshot.capture_complete = false;
          continue;
        }

        int open_error = 0;
        auto handle = open_process_pidfd(pid, open_error);
        if (!handle) {
          if (open_error != ESRCH) {
            snapshot.capture_complete = false;
          }
          continue;
        }

        const auto environ_after = read_proc_status_file_result(pid, "environ");
        auto identity_after = proc_start_time_ticks(pid);
#ifdef POLARIS_TESTS
        if (pid == forced_reused_exact_generation_pid && identity_after) {
          ++*identity_after;
        }
#endif
        const bool identity_matches = steam_pidfd_capture_identity_matches(identity_before, identity_after);
        const bool environment_matches = environ_after.ok() &&
                                         proc_environ_matches_isolated_session(
                                           environ_after.bytes,
                                           session_instance_id
                                         );
        if (!identity_matches || !environment_matches) {
          bool captured_process_exited = pidfd_has_exited(*handle);
#ifdef POLARIS_TESTS
          if (pid == forced_reused_exact_generation_pid) {
            captured_process_exited = true;
          }
#endif
          const bool replacement_may_be_exact_generation =
            !identity_matches && identity_after && (!environ_after.ok() || environment_matches);
          if (!captured_process_exited || replacement_may_be_exact_generation) {
            snapshot.capture_complete = false;
          }
          continue;
        }
        snapshot.owned.emplace_back(std::move(*handle));
#ifdef POLARIS_TESTS
        if (pid == forced_proc_directory_error_after_capture_pid) {
          forced_proc_directory_enumeration_error = true;
        }
#endif
      }

      closedir(dir);
      return snapshot;
    }

    bool terminate_isolated_session_processes(
      std::string_view session_instance_id,
      std::string_view reason
    ) {
      if (session_instance_id.empty()) {
        return false;
      }

      for (int pass = 0; pass < 2; ++pass) {
        auto snapshot = isolated_session_process_snapshot(session_instance_id);
        if (!snapshot.capture_complete) {
          BOOST_LOG(warning) << "process: exact-generation isolated process capture was incomplete "sv << reason;
          return false;
        }
        if (snapshot.owned.empty()) {
          return true;
        }

        BOOST_LOG(info) << "process: terminating "sv << snapshot.owned.size()
                        << " exact-generation isolated process(es) "sv << reason;
        if (!terminate_pidfds(snapshot.owned, 2s, 1s, "isolated session process"sv)) {
          BOOST_LOG(warning) << "process: exact-generation isolated processes remained after bounded pidfd cleanup "sv
                             << reason;
        }
      }

      const auto final_snapshot = isolated_session_process_snapshot(session_instance_id);
      return final_snapshot.capture_complete && final_snapshot.owned.empty();
    }

    struct steam_client_ownership_snapshot_t {
      std::vector<pidfd_handle_t> owned;
      std::vector<pidfd_handle_t> unowned;
      bool capture_complete = true;
    };

    steam_client_ownership_snapshot_t steam_client_ownership_snapshot(std::string_view session_instance_id) {
      steam_client_ownership_snapshot_t snapshot;
      DIR *dir = opendir("/proc");
      if (!dir) {
        snapshot.capture_complete = false;
        return snapshot;
      }

      for (;;) {
        auto *entry = read_next_proc_entry(dir);
        if (entry == nullptr) {
          if (errno != 0) {
            snapshot.capture_complete = false;
          }
          break;
        }
        const std::string name = entry->d_name;
        if (!proc_pid_dir_name(name)) {
          continue;
        }
        const auto pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
        if (pid <= 1 || pid == getpid()) {
          continue;
        }

        const auto identity_before = proc_start_time_ticks(pid);
        const auto comm_before = read_proc_status_file_result(pid, "comm");
        const auto cmdline_before = read_proc_status_file_result(pid, "cmdline");
        const auto status_before = read_proc_status_file_result(pid, "status");
        auto comm = boost::to_lower_copy(comm_before.bytes);
        boost::trim(comm);
        auto cmdline = cmdline_before.bytes;
        auto argv0 = proc_argv0_from_cmdline(cmdline);
        auto status = status_before.bytes;
        const bool before_reads_complete = comm_before.ok() && cmdline_before.ok() && status_before.ok();
        if (!before_reads_complete) {
          const auto read_is_ok_or_vanished = [](const proc_file_read_result_t &result) {
            return result.ok() || process_vanished_during_proc_read(result.error);
          };
          const bool all_failures_are_vanished =
            read_is_ok_or_vanished(comm_before) &&
            read_is_ok_or_vanished(cmdline_before) &&
            read_is_ok_or_vanished(status_before);
          if (!all_failures_are_vanished) {
            const auto descends_from_polaris = proc_pid_descends_from(pid, getpid());
            if (is_desktop_steam_client_process(comm, argv0) ||
                !descends_from_polaris || *descends_from_polaris) {
              snapshot.capture_complete = false;
            }
          }
          continue;
        }
        if (!is_active_steam_shutdown_ownership_process(comm, argv0, cmdline, status)) {
          continue;
        }
#ifdef POLARIS_TESTS
        if (pid == forced_steam_ownership_capture_failure_pid) {
          snapshot.capture_complete = false;
          continue;
        }
#endif
        if (!identity_before) {
          snapshot.capture_complete = false;
          continue;
        }

        int open_error = 0;
        auto handle = open_process_pidfd(pid, open_error);
        if (!handle) {
          if (open_error != ESRCH) {
            snapshot.capture_complete = false;
          }
          continue;
        }

        const auto comm_after = read_proc_status_file_result(pid, "comm");
        const auto cmdline_after = read_proc_status_file_result(pid, "cmdline");
        const auto status_after = read_proc_status_file_result(pid, "status");
        comm = boost::to_lower_copy(comm_after.bytes);
        boost::trim(comm);
        cmdline = cmdline_after.bytes;
        argv0 = proc_argv0_from_cmdline(cmdline);
        status = status_after.bytes;
        const bool after_reads_complete = comm_after.ok() && cmdline_after.ok() && status_after.ok();
        if (!after_reads_complete) {
          const auto current_identity = proc_start_time_ticks(pid);
          if (!pidfd_has_exited(*handle) ||
              (current_identity && !steam_pidfd_capture_identity_matches(identity_before, current_identity))) {
            snapshot.capture_complete = false;
          }
          continue;
        }
        if (!is_active_steam_shutdown_ownership_process(comm, argv0, cmdline, status)) {
          continue;
        }
        auto identity_after = proc_start_time_ticks(pid);
#ifdef POLARIS_TESTS
        if (pid == forced_reused_exact_generation_pid && identity_after) {
          ++*identity_after;
        }
#endif
        if (!steam_pidfd_capture_identity_matches(identity_before, identity_after)) {
          bool captured_process_exited = pidfd_has_exited(*handle);
#ifdef POLARIS_TESTS
          if (pid == forced_reused_exact_generation_pid) {
            captured_process_exited = true;
          }
#endif
          if (!captured_process_exited || identity_after) {
            snapshot.capture_complete = false;
          }
          continue;
        }

        const auto environ = read_proc_status_file_result(pid, "environ");
        if (!environ.ok() || environ.bytes.empty()) {
          if (!pidfd_has_exited(*handle)) {
            snapshot.capture_complete = false;
          }
          continue;
        }
        if (proc_environ_matches_isolated_session(environ.bytes, session_instance_id)) {
          snapshot.owned.emplace_back(std::move(*handle));
        } else {
          snapshot.unowned.emplace_back(std::move(*handle));
        }
#ifdef POLARIS_TESTS
        if (pid == forced_proc_directory_error_after_capture_pid) {
          forced_proc_directory_enumeration_error = true;
        }
#endif
      }

      closedir(dir);
      return snapshot;
    }

    bool terminate_session_owned_steam_before_cage_stop_impl(
      const proc::ctx_t &app,
      bool session_owned_cage,
      std::string_view session_instance_id
    ) {
      const bool steam_context = context_uses_steam(app);
      if (!session_owned_cage || session_instance_id.empty() || !steam_context) {
        return false;
      }

      auto ownership = steam_client_ownership_snapshot(session_instance_id);
      if (!should_terminate_session_owned_steam_before_cage_stop(
            app,
            session_owned_cage,
            !session_instance_id.empty(),
            !ownership.owned.empty(),
            !ownership.unowned.empty(),
            ownership.capture_complete
          )) {
        if (!ownership.capture_complete) {
          BOOST_LOG(warning) << "process: skipping pre-cage private Steam termination because pidfd ownership capture was incomplete"sv;
        } else if (!ownership.unowned.empty()) {
          BOOST_LOG(warning) << "process: skipping pre-cage private Steam termination because "sv
                             << ownership.unowned.size() << " active Steam client process(es) are not session-owned"sv;
        } else if (ownership.owned.empty()) {
          BOOST_LOG(info) << "process: skipping pre-cage private Steam termination because no session-owned Steam client is active"sv;
        }
        return false;
      }

      // Ask Steam to shut down GRACEFULLY first. `steam -shutdown` goes through Steam's
      // own IPC: it closes the running game, flushes Steam Cloud, then exits cleanly. A
      // broadcast SIGTERM across the dozen-odd Steam processes (the terminate_pidfds
      // fallback below) does NOT do that — Steam gets signal-killed mid-game and reports
      // a fatal error on the next launch. Only fall back to the pidfd SIGTERM/SIGKILL
      // path if the graceful shutdown does not complete within the window (Steam closing
      // a running game can legitimately take several seconds).
      const auto shutdown_cmd = canonical_steam_shutdown_command(steam_launch_reference_command(app));
      BOOST_LOG(info) << "process: requesting graceful Steam shutdown ["sv << shutdown_cmd
                      << "] before isolated cage stop"sv;
      std::system((shutdown_cmd + " >/dev/null 2>&1").c_str());
      if (wait_for_pidfds_exit(ownership.owned, 20s)) {
        BOOST_LOG(info) << "process: session-owned Steam exited gracefully before isolated cage stop"sv;
        return true;
      }

      BOOST_LOG(warning) << "process: Steam did not exit within the graceful-shutdown window; terminating "sv
                         << ownership.owned.size() << " session-owned Steam process(es) through pidfd"sv;
      if (terminate_pidfds(ownership.owned, 5s, 1s, "private Steam"sv)) {
        BOOST_LOG(info) << "process: session-owned Steam exited before isolated cage stop"sv;
        return true;
      }

      BOOST_LOG(warning) << "process: session-owned Steam remained after graceful shutdown and bounded pidfd termination; continuing with isolated cage fallback cleanup"sv;
      return false;
    }

    bool is_active_desktop_steam_client_process(
      std::string_view comm,
      std::string_view argv0_path,
      std::string_view cmdline,
      std::string_view status
    ) {
      return is_desktop_steam_client_process(comm, argv0_path) &&
             !proc_status_is_zombie(status) &&
             !is_transient_steam_client_cmdline(cmdline);
    }

#ifdef POLARIS_TESTS
    thread_local bool forced_desktop_steam_proc_open_error = false;
    thread_local pid_t forced_desktop_steam_proc_read_error_pid = -1;
    thread_local pid_t forced_desktop_steam_scan_only_pid = -1;
#endif

    bool desktop_steam_client_active_impl() {
      DIR *dir = nullptr;
#ifdef POLARIS_TESTS
      if (forced_desktop_steam_proc_open_error) {
        errno = EACCES;
      } else {
        dir = opendir("/proc");
      }
#else
      dir = opendir("/proc");
#endif
      if (!dir) {
        return true;
      }
      auto close_dir = util::fail_guard([dir]() {
        closedir(dir);
      });

      const auto this_pid = getpid();
      for (;;) {
        auto *entry = read_next_proc_entry(dir);
        if (entry == nullptr) {
          return errno != 0;
        }
        const std::string name = entry->d_name;
        if (!proc_pid_dir_name(name)) {
          continue;
        }

        const auto pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
        if (pid <= 1 || pid == this_pid) {
          continue;
        }
#ifdef POLARIS_TESTS
        if (forced_desktop_steam_scan_only_pid > 0 && pid != forced_desktop_steam_scan_only_pid) {
          continue;
        }
#endif

        auto comm_result = read_proc_status_file_result(pid, "comm");
        auto cmdline_result = read_proc_status_file_result(pid, "cmdline");
        auto status_result = read_proc_status_file_result(pid, "status");
#ifdef POLARIS_TESTS
        if (pid == forced_desktop_steam_proc_read_error_pid) {
          comm_result = {{}, EACCES};
        }
#endif
        auto comm = boost::to_lower_copy(comm_result.bytes);
        boost::trim(comm);
        const auto cmdline = cmdline_result.bytes;
        const auto argv0 = proc_argv0_from_cmdline(cmdline);
        const auto status = status_result.bytes;
        const bool reads_complete = comm_result.ok() && cmdline_result.ok() && status_result.ok();
        if (!reads_complete) {
          if (is_active_desktop_steam_client_process(comm, argv0, cmdline, status)) {
            return true;
          }
          const auto read_is_ok_or_vanished = [](const proc_file_read_result_t &result) {
            return result.ok() || process_vanished_during_proc_read(result.error);
          };
          if (!read_is_ok_or_vanished(comm_result) ||
              !read_is_ok_or_vanished(cmdline_result) ||
              !read_is_ok_or_vanished(status_result)) {
            return true;
          }
          continue;
        }
        if (is_active_desktop_steam_client_process(comm, argv0, cmdline, status)) {
          return true;
        }
      }
    }

    proc::desktop_launch_safety_policy_t resolve_desktop_launch_safety_policy_impl(
      bool private_stream_requested,
      bool mirror_desktop_explicit,
      bool app_uses_steam,
      bool desktop_steam_active,
      bool active_desktop_game,
      bool force_private_after_desktop_steam_shutdown
    ) {
      proc::desktop_launch_safety_policy_t policy;
      policy.desktopSteamActive = desktop_steam_active && app_uses_steam;
      policy.physicalDisplayRisk = active_desktop_game || policy.desktopSteamActive;
      policy.canMirrorDesktop = true;
      policy.canForceCloseDesktopSteamForPrivateStream = policy.desktopSteamActive;
      policy.forcePrivateStreamLabel = "Close desktop Steam and start private stream";
      policy.privateStreamUnavailableReason = policy.desktopSteamActive ?
        "Desktop Steam is already active. Private launch is blocked unless you close desktop Steam first." :
        (active_desktop_game ? "A desktop game is already active on this host." : "");

      policy.canLaunchPrivateStream = !policy.physicalDisplayRisk;

      if (mirror_desktop_explicit && policy.canMirrorDesktop) {
        policy.recommendedAction = "mirror_desktop";
        return policy;
      }

      if (private_stream_requested &&
          force_private_after_desktop_steam_shutdown &&
          policy.canForceCloseDesktopSteamForPrivateStream) {
        policy.canLaunchPrivateStream = true;
        policy.recommendedAction = "force_private_stream_after_desktop_steam_shutdown";
        return policy;
      }

      policy.canLaunchPrivateStream = !policy.physicalDisplayRisk;
      if (private_stream_requested && policy.canLaunchPrivateStream) {
        policy.recommendedAction = "launch_private_stream";
      } else if (private_stream_requested && policy.physicalDisplayRisk) {
        policy.recommendedAction = "refuse_private_stream";
      } else {
        policy.recommendedAction = "launch_desktop_stream";
      }
      return policy;
    }

    bool should_skip_steam_shutdown_undo_after_cage_cleanup(const proc::ctx_t &,
                                                            const proc::cmd_t &cmd,
                                                            bool session_owned_cage) {
      return session_owned_cage &&
             command_requests_steam_shutdown(cmd.undo_cmd);
    }

#endif

    void normalize_steam_big_picture_app(proc::ctx_t &ctx) {
      if (!is_steam_big_picture_app(ctx)) {
        return;
      }

      auto normalize_open_command = [&](const std::string &cmd) {
#ifdef __linux__
        return canonical_steam_library_bootstrap_command(cmd.empty() ? "steam" : cmd);
#else
        return canonical_steam_big_picture_command(cmd.empty() ? "steam" : cmd, "open/bigpicture");
#endif
      };

      for (auto &cmd : ctx.detached) {
        if (!(command_uses_steam_gamepadui_flag(cmd) || command_contains_steam_big_picture_open(cmd))) {
          continue;
        }

        const auto normalized = normalize_open_command(cmd);
        if (normalized == cmd) {
          continue;
        }

        BOOST_LOG(info) << "process: normalized Steam Big Picture detached launch command from ["
                        << cmd << "] to [" << normalized << ']';
        cmd = normalized;
      }

      if (!ctx.cmd.empty() &&
          (command_uses_steam_gamepadui_flag(ctx.cmd) || command_contains_steam_big_picture_open(ctx.cmd))) {
        const auto normalized = normalize_open_command(ctx.cmd);
        if (std::find(ctx.detached.begin(), ctx.detached.end(), normalized) == ctx.detached.end()) {
          ctx.detached.emplace_back(normalized);
        }
        BOOST_LOG(info) << "process: converted Steam Big Picture primary command into detached launch ["
                        << normalized << ']';
        ctx.cmd.clear();
      }

      bool stripped_mangohud = false;
      for (const char *key : {"MANGOHUD", "MANGOHUD_DLSYM", "MANGOHUD_CONFIG"}) {
        if (ctx.env_vars.erase(key) > 0) {
          stripped_mangohud = true;
        }
      }

      if (stripped_mangohud) {
        BOOST_LOG(info) << "process: removed MangoHud environment from Steam Big Picture to avoid Steam helper crashes";
      }

      for (auto &cmd : ctx.prep_cmds) {
        if (command_contains_steam_big_picture_close(cmd.undo_cmd) || command_requests_steam_shutdown(cmd.undo_cmd)) {
          const auto shutdown_cmd = canonical_steam_shutdown_command(cmd.undo_cmd);
          if (shutdown_cmd != cmd.undo_cmd) {
            BOOST_LOG(info) << "process: upgraded Steam Big Picture cleanup command from ["
                            << cmd.undo_cmd << "] to [" << shutdown_cmd << ']';
            cmd.undo_cmd = shutdown_cmd;
          }
        }
      }

      ensure_steam_cleanup_undo(ctx, "Steam Big Picture");
    }

    void normalize_steam_library_app(proc::ctx_t &ctx) {
      if (!is_steam_library_app(ctx)) {
        return;
      }

      std::string appid = ctx.steam_appid;
      if (appid.empty()) {
        if (const auto detected = extract_steam_appid_from_command(ctx.cmd); detected) {
          appid = *detected;
        } else {
          for (const auto &cmd : ctx.detached) {
            if (const auto detected = extract_steam_appid_from_command(cmd); detected) {
              appid = *detected;
              break;
            }
          }
        }
      }

      if (appid.empty()) {
        return;
      }

      ctx.steam_appid = appid;
      ctx.source = "steam";
      ctx.steam_launch_mode = proc::normalize_steam_launch_mode(ctx.steam_launch_mode);

      std::string reference_cmd = ctx.cmd;
      std::vector<nlohmann::json> preserved_detached;
      preserved_detached.reserve(ctx.detached.size());

      for (const auto &cmd : ctx.detached) {
        if (command_is_steam_library_launch_component(cmd)) {
          if (reference_cmd.empty()) {
            reference_cmd = cmd;
          }
          continue;
        }
        preserved_detached.emplace_back(cmd);
      }

      if (!ctx.cmd.empty() && command_is_steam_library_launch_component(ctx.cmd)) {
        BOOST_LOG(info) << "process: converted Steam library primary command into detached launch components"sv;
        ctx.cmd.clear();
      }

      const auto canonical_commands = canonical_steam_library_launch_commands(reference_cmd, appid, ctx.steam_launch_mode);
      ctx.detached.clear();
      ctx.detached.reserve(canonical_commands.size() + preserved_detached.size());
      for (const auto &cmd : canonical_commands) {
        ctx.detached.emplace_back(cmd);
      }
      for (auto &cmd : preserved_detached) {
        ctx.detached.emplace_back(std::move(cmd));
      }

      for (auto &cmd : ctx.prep_cmds) {
        if (command_contains_steam_big_picture_close(cmd.undo_cmd) || command_requests_steam_shutdown(cmd.undo_cmd)) {
          const auto shutdown_cmd = canonical_steam_shutdown_command(cmd.undo_cmd);
          if (shutdown_cmd != cmd.undo_cmd) {
            BOOST_LOG(info) << "process: upgraded Steam cleanup command from ["
                            << cmd.undo_cmd << "] to [" << shutdown_cmd << ']';
            cmd.undo_cmd = shutdown_cmd;
          }
        }
      }

      ensure_steam_cleanup_undo(ctx, "Steam library");
    }

    std::optional<int> coerce_json_int(const nlohmann::json &value) {
      try {
        if (value.is_number_integer() || value.is_number_unsigned()) {
          return value.get<int>();
        }

        if (value.is_number_float()) {
          return static_cast<int>(value.get<double>());
        }

        if (value.is_boolean()) {
          return value.get<bool>() ? 1 : 0;
        }

        if (value.is_string()) {
          const auto normalized = normalized_json_string(value.get<std::string>());
          if (normalized.empty()) {
            return std::nullopt;
          }

          size_t parsed_chars = 0;
          const int parsed_value = std::stoi(normalized, &parsed_chars);
          if (parsed_chars == normalized.size()) {
            return parsed_value;
          }
        }
      } catch (const std::exception &) {
      }

      return std::nullopt;
    }

    bool coerce_json_bool(const nlohmann::json &value, bool default_value) {
      try {
        if (value.is_boolean()) {
          return value.get<bool>();
        }

        if (value.is_number()) {
          return value.get<double>() != 0.0;
        }

        if (value.is_string()) {
          const auto normalized = normalized_json_string(value.get<std::string>());
          if (normalized == "true" || normalized == "on" || normalized == "yes" || normalized == "1") {
            return true;
          }
          if (normalized == "false" || normalized == "off" || normalized == "no" || normalized == "0") {
            return false;
          }
          return default_value;
        }

        if (value.is_array()) {
          if (value.empty()) {
            return default_value;
          }
          return coerce_json_bool(value[0], default_value);
        }

        if (value.is_null()) {
          return default_value;
        }

        if (value.is_object()) {
          return !value.empty();
        }
      } catch (const std::exception &) {
      }

      return default_value;
    }

    int json_int_member_or(const nlohmann::json &node, const char *key, int default_value) {
      if (!node.is_object()) {
        return default_value;
      }

      const auto it = node.find(key);
      if (it == node.end()) {
        return default_value;
      }

      const auto parsed_value = coerce_json_int(*it);
      return parsed_value.value_or(default_value);
    }

    struct parsed_display_mode_t {
      int width = 0;
      int height = 0;
      int fps = 0;  // milliHz
    };

    struct resolved_session_optimization_t {
      std::optional<parsed_display_mode_t> display_mode;
      std::optional<int> color_range;
      std::optional<bool> hdr;
      std::optional<bool> virtual_display;
      std::optional<int> target_bitrate_kbps;
      std::optional<int> nvenc_tune;
      std::optional<std::string> preferred_codec;
      std::string display_mode_source;
      std::string color_range_source;
      std::string hdr_source;
      std::string virtual_display_source;
      std::string bitrate_source;
      std::string nvenc_tune_source;
      std::string preferred_codec_source;
      std::vector<std::string> reasoning;
      std::vector<std::string> layers;
      std::string confidence;
      std::string cache_status;
      std::string normalization_reason;
      int recommendation_version = 0;
    };

    struct optimization_locks_t {
      bool display_mode = false;
      bool color_range = false;
      bool hdr = false;
      bool virtual_display = false;
      bool bitrate = false;
      bool nvenc_tune = false;
      bool preferred_codec = false;
    };

    bool parse_display_mode_string(const std::string &value, parsed_display_mode_t &parsed) {
      if (value.empty()) {
        return false;
      }

      int width = 0;
      int height = 0;
      float fps = 60.0f;
      if (sscanf(value.c_str(), "%dx%dx%f", &width, &height, &fps) < 2) {
        return false;
      }

      if (width <= 0 || height <= 0 || fps <= 0.0f) {
        return false;
      }

      parsed.width = width;
      parsed.height = height;
      parsed.fps = fps < 1000.0f ? static_cast<int>(std::round(fps * 1000.0f)) : static_cast<int>(std::round(fps));
      return parsed.fps > 0;
    }

    std::string format_session_fps(int fps) {
      if (fps <= 0) {
        return "0";
      }

      const double fps_value = fps >= 1000 ? static_cast<double>(fps) / 1000.0 : static_cast<double>(fps);
      std::ostringstream stream;
      stream << std::fixed << std::setprecision(fps % 1000 == 0 ? 0 : 3) << fps_value;
      return stream.str();
    }

    std::string format_display_mode(const parsed_display_mode_t &mode) {
      return std::to_string(mode.width) + "x" + std::to_string(mode.height) + "x" + format_session_fps(mode.fps);
    }

    bool display_modes_equal(const parsed_display_mode_t &lhs, const parsed_display_mode_t &rhs) {
      return lhs.width == rhs.width && lhs.height == rhs.height && lhs.fps == rhs.fps;
    }

    parsed_display_mode_t clamp_optimized_display_mode_to_client_request(const parsed_display_mode_t &candidate,
                                                                         const parsed_display_mode_t &requested,
                                                                         std::string_view source) {
      auto clamped = candidate;

      if (requested.fps > 0 && clamped.fps > requested.fps) {
        clamped.fps = requested.fps;
      }

      if (requested.width > 0 && requested.height > 0 &&
          (clamped.width > requested.width || clamped.height > requested.height)) {
        clamped.width = requested.width;
        clamped.height = requested.height;
      }

      constexpr int balanced_floor_width = 1920;
      constexpr int balanced_floor_height = 1080;
      const bool recovery_profile = source.find("history_safe") != std::string_view::npos;
      if (!recovery_profile &&
          requested.width >= balanced_floor_width &&
          requested.height >= balanced_floor_height &&
          clamped.height < balanced_floor_height) {
        clamped.width = balanced_floor_width;
        clamped.height = balanced_floor_height;
      }

      return clamped;
    }

    void note_layer(resolved_session_optimization_t &resolved, const std::string &layer) {
      if (layer.empty() || std::find(resolved.layers.begin(), resolved.layers.end(), layer) != resolved.layers.end()) {
        return;
      }

      resolved.layers.push_back(layer);
    }

    void note_reasoning(resolved_session_optimization_t &resolved, const std::string &reasoning) {
      if (reasoning.empty()) {
        return;
      }

      resolved.reasoning.push_back(reasoning);
    }

    void append_normalization_reason(resolved_session_optimization_t &resolved, const std::string &reason) {
      if (reason.empty()) {
        return;
      }

      if (!resolved.normalization_reason.empty()) {
        resolved.normalization_reason += ' ';
      }
      resolved.normalization_reason += reason;
    }

    bool launch_bitrate_is_locked(int configured_max_bitrate,
                                   bool has_paired_target_bitrate,
                                   bool ai_auto_quality_enabled) {
      return configured_max_bitrate > 0 || has_paired_target_bitrate || !ai_auto_quality_enabled;
    }

    void apply_optimization_layer(resolved_session_optimization_t &resolved,
                                  const optimization_locks_t &locks,
                                  const device_db::optimization_t &optimization,
                                  const std::string &layer) {
      bool touched = false;

      if (optimization.display_mode && !locks.display_mode) {
        parsed_display_mode_t parsed;
        if (parse_display_mode_string(*optimization.display_mode, parsed)) {
          resolved.display_mode = parsed;
          resolved.display_mode_source = layer;
          touched = true;
        } else {
          BOOST_LOG(warning) << "session_optimization: Ignoring invalid display mode ["sv
                             << *optimization.display_mode << "] from "sv << layer;
        }
      }

      if (optimization.color_range && !locks.color_range) {
        resolved.color_range = *optimization.color_range;
        resolved.color_range_source = layer;
        touched = true;
      }

      if (optimization.hdr.has_value() && !locks.hdr) {
        resolved.hdr = *optimization.hdr;
        resolved.hdr_source = layer;
        touched = true;
      }

      if (optimization.virtual_display.has_value() && !locks.virtual_display) {
        resolved.virtual_display = *optimization.virtual_display;
        resolved.virtual_display_source = layer;
        touched = true;
      }

      if (optimization.target_bitrate_kbps && !locks.bitrate) {
        resolved.target_bitrate_kbps = *optimization.target_bitrate_kbps;
        resolved.bitrate_source = layer;
        touched = true;
      }

      if (optimization.nvenc_tune && !locks.nvenc_tune) {
        resolved.nvenc_tune = *optimization.nvenc_tune;
        resolved.nvenc_tune_source = layer;
        touched = true;
      }

      if (optimization.preferred_codec && !locks.preferred_codec) {
        resolved.preferred_codec = *optimization.preferred_codec;
        resolved.preferred_codec_source = layer;
        touched = true;
      }

      if (touched) {
        note_layer(resolved, layer);
        note_reasoning(resolved, optimization.reasoning);
        if (!optimization.confidence.empty()) {
          resolved.confidence = optimization.confidence;
        }
        if (!optimization.cache_status.empty()) {
          resolved.cache_status = optimization.cache_status;
        }
        if (!optimization.normalization_reason.empty()) {
          resolved.normalization_reason = optimization.normalization_reason;
        }
        if (optimization.recommendation_version > 0) {
          resolved.recommendation_version = optimization.recommendation_version;
        }
      }
    }

    std::string join_layers(const std::vector<std::string> &layers) {
      std::ostringstream stream;
      for (std::size_t index = 0; index < layers.size(); ++index) {
        if (index > 0) {
          stream << '+';
        }
        stream << layers[index];
      }
      return stream.str();
    }

    std::string join_reasons(const std::vector<std::string> &reasons) {
      std::ostringstream stream;
      for (std::size_t index = 0; index < reasons.size(); ++index) {
        if (index > 0) {
          stream << " | ";
        }
        stream << reasons[index];
      }
      return stream.str();
    }

    double session_grading_target_fps(const stream_stats::stats_t &stats) {
      if (stats.session_target_fps > 0.0) {
        return stats.session_target_fps;
      }
      if (stats.encode_target_fps > 0.0) {
        return stats.encode_target_fps;
      }
      if (stats.requested_client_fps > 0.0) {
        return stats.requested_client_fps;
      }
      return stats.fps > 0.0 ? stats.fps : 0.0;
    }

    std::string grade_session_quality(const stream_stats::stats_t &stats, double target_fps) {
      if (stats.fps <= 0.0) {
        return "F";
      }

      const double effective_target = target_fps > 0.0 ? target_fps : session_grading_target_fps(stats);
      const double fps_ratio =
        (effective_target > 0.0 && stats.fps > 0.0) ? std::clamp(stats.fps / effective_target, 0.0, 1.5) : 1.0;

      if (fps_ratio >= 0.95 && stats.packet_loss < 0.5 && stats.latency_ms < 20) {
        return "A";
      }
      if (fps_ratio >= 0.85 && stats.packet_loss < 2.0 && stats.latency_ms < 40) {
        return "B";
      }
      if (fps_ratio >= 0.70 && stats.packet_loss < 5.0) {
        return "C";
      }
      if (fps_ratio >= 0.50) {
        return "D";
      }
      return "F";
    }

    void set_session_env_var(boost::process::v1::environment &env,
                             std::unordered_set<std::string> &tracked_keys,
                             const std::string &key,
                             const std::string &value) {
      env[key] = value;
      platf::set_env(key, value);
      tracked_keys.insert(key);
    }

    void set_child_only_session_env_var(boost::process::v1::environment &env,
                                        std::unordered_set<std::string> &tracked_keys,
                                        const std::string &key,
                                        const std::string &value) {
      env[key] = value;
      tracked_keys.insert(key);
    }

#ifdef __linux__
    std::optional<std::string> copy_env_var(const char *key) {
      if (const char *value = std::getenv(key); value != nullptr) {
        return std::string(value);
      }

      return std::nullopt;
    }

    int normalized_audio_channel_count(int channel_count) {
      switch (channel_count) {
        case 6:
        case 8:
          return channel_count;
        case 2:
        default:
          return 2;
      }
    }

    void restore_env_var(const char *key, const std::optional<std::string> &value) {
      if (value) {
        platf::set_env(key, *value);
      } else {
        platf::unset_env(key);
      }
    }
#endif

    std::string env_value(const proc::ctx_t &app, const boost::process::v1::environment &env, const std::string &key) {
      if (const auto it = app.env_vars.find(key); it != app.env_vars.end()) {
        return it->second;
      }

      const char *current_value = getenv(key.c_str());
      if (current_value && *current_value) {
        return current_value;
      }

      const auto env_it = env.find(key);
      if (env_it != env.end()) {
        return env_it->to_string();
      }

      return {};
    }

    bool session_pacing_is_enabled(const proc::ctx_t &app,
                                   const rtsp_stream::launch_session_t &launch_session,
                                   const std::string &game_category) {
      if (launch_session.input_only) {
        return false;
      }
      if (game_category == "desktop") {
        return false;
      }
      if (app.cmd.empty() && app.detached.empty()) {
        return false;
      }
      const int target_fps = launch_session.fps >= 1000 ? static_cast<int>(std::round(static_cast<double>(launch_session.fps) / 1000.0)) : launch_session.fps;
      return target_fps > 0;
    }

    bool env_flag_enabled(const proc::ctx_t &app, const boost::process::v1::environment &env, const std::string &key) {
      const auto value = normalized_json_string(env_value(app, env, key));
      if (value.empty()) {
        return false;
      }

      return value != "0" && value != "false" && value != "off" && value != "no";
    }

    bool app_env_flag_enabled(const proc::ctx_t &app, const std::string &key) {
      const auto it = app.env_vars.find(key);
      if (it == app.env_vars.end()) {
        return false;
      }

      const auto value = normalized_json_string(it->second);
      if (value.empty()) {
        return false;
      }

      return value != "0" && value != "false" && value != "off" && value != "no";
    }

#ifdef __linux__
    bool cage_mangohud_allowed_for_session(const proc::ctx_t &app,
                                           bool use_cage_compositor,
                                           bool requested_headless) {
      if (!use_cage_compositor) {
        return true;
      }

      if (is_steam_big_picture_app(app)) {
        return false;
      }

      if (requested_headless) {
        return app_env_flag_enabled(app, "MANGOHUD");
      }

      return true;
    }

    enum class steam_game_process_event_kind_internal_e {
      none,
      started,
      stopped,
    };

    struct steam_game_process_event_internal_t {
      steam_game_process_event_kind_internal_e kind = steam_game_process_event_kind_internal_e::none;
      std::string appid;
    };

    struct steam_big_picture_guard_transition_internal_t {
      bool close_big_picture = false;
      bool open_big_picture = false;
      std::size_t active_games = 0;
    };

    std::optional<std::string> decimal_token_after(std::string_view line, std::string_view marker) {
      const auto marker_pos = line.find(marker);
      if (marker_pos == std::string_view::npos) {
        return std::nullopt;
      }

      const auto begin = marker_pos + marker.size();
      auto end = begin;
      while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) {
        ++end;
      }
      if (end == begin) {
        return std::nullopt;
      }
      return std::string {line.substr(begin, end - begin)};
    }

    steam_game_process_event_internal_t parse_steam_game_process_event(std::string_view line) {
      auto payload = line;
      if (line.starts_with('[')) {
        const auto timestamp_end = line.find("] "sv);
        if (timestamp_end == std::string_view::npos) {
          return {};
        }
        payload = line.substr(timestamp_end + 2);
      }

      if (const auto removed_appid = decimal_token_after(payload, "Remove "sv); removed_appid) {
        const auto marker = "Remove "s + *removed_appid + " from running list";
        if (payload == marker || payload == marker + '\r') {
          return {steam_game_process_event_kind_internal_e::stopped, *removed_appid};
        }
      }

      const auto tracked_appid = decimal_token_after(payload, "AppID "sv);
      const auto tracked_prefix = tracked_appid ? "AppID "s + *tracked_appid + " adding PID " : ""s;
      if (!tracked_appid || !payload.starts_with(tracked_prefix) ||
          payload.find("steam-launch-wrapper"sv) == std::string_view::npos) {
        return {};
      }

      const auto launched_appid = decimal_token_after(line, "SteamLaunch AppId="sv);
      if (!launched_appid || *launched_appid != *tracked_appid) {
        return {};
      }

      return {steam_game_process_event_kind_internal_e::started, *tracked_appid};
    }

    steam_big_picture_guard_transition_internal_t apply_steam_big_picture_guard_event(
      std::unordered_set<std::string> &active_appids,
      std::string_view line
    ) {
      steam_big_picture_guard_transition_internal_t transition;
      const auto event = parse_steam_game_process_event(line);
      if (event.kind == steam_game_process_event_kind_internal_e::started) {
        const bool was_empty = active_appids.empty();
        const bool inserted = active_appids.emplace(event.appid).second;
        transition.close_big_picture = inserted && was_empty;
      } else if (event.kind == steam_game_process_event_kind_internal_e::stopped) {
        const bool removed = active_appids.erase(event.appid) > 0;
        transition.open_big_picture = removed && active_appids.empty();
      }
      transition.active_games = active_appids.size();
      return transition;
    }

    bool steam_big_picture_input_guard_enabled(
      const proc::ctx_t &app,
      bool use_cage_compositor,
      bool mirror_desktop
    ) {
      return use_cage_compositor &&
             !mirror_desktop &&
             (is_steam_big_picture_app(app) ||
              proc::steam_launch_mode_is_big_picture(app.steam_launch_mode));
    }

    struct steam_game_process_log_identity_t {
      dev_t device = 0;
      ino_t inode = 0;

      bool operator==(const steam_game_process_log_identity_t &other) const {
        return device == other.device && inode == other.inode;
      }
    };

    std::optional<steam_game_process_log_identity_t> steam_game_process_log_identity(
      const std::filesystem::path &path
    ) {
      struct stat info {};
      if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::nullopt;
      }
      return steam_game_process_log_identity_t {info.st_dev, info.st_ino};
    }

    bool open_steam_game_process_log_from_pinned_descriptor(std::ifstream &log, int fd) {
      log.open("/proc/self/fd/"s + std::to_string(fd));
      return static_cast<bool>(log);
    }

    enum class steam_game_process_log_snapshot_status_e {
      captured,
      absent,
      error,
    };

    struct steam_game_process_log_snapshot_result_t {
      steam_game_process_log_snapshot_status_e status = steam_game_process_log_snapshot_status_e::error;
      std::optional<steam_game_process_log_identity_t> identity;
      std::uintmax_t offset = 0;
      std::error_code error;
    };

    steam_game_process_log_snapshot_result_t snapshot_steam_game_process_log(
      const std::filesystem::path &path,
      const std::function<void()> &after_open = {}
    ) {
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        const int open_errno = errno;
        if (open_errno == ENOENT) {
          return {steam_game_process_log_snapshot_status_e::absent, std::nullopt, 0, {}};
        }
        return {
          steam_game_process_log_snapshot_status_e::error,
          std::nullopt,
          0,
          std::error_code {open_errno, std::generic_category()},
        };
      }
      auto close_fd = util::fail_guard([fd]() {
        ::close(fd);
      });
      if (after_open) {
        after_open();
      }

      struct stat info {};
      if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        const int stat_errno = errno ? errno : EINVAL;
        return {
          steam_game_process_log_snapshot_status_e::error,
          std::nullopt,
          0,
          std::error_code {stat_errno, std::generic_category()},
        };
      }
      const auto end = ::lseek(fd, 0, SEEK_END);
      if (end < 0) {
        return {
          steam_game_process_log_snapshot_status_e::error,
          std::nullopt,
          0,
          std::error_code {errno, std::generic_category()},
        };
      }
      return {
        steam_game_process_log_snapshot_status_e::captured,
        steam_game_process_log_identity_t {info.st_dev, info.st_ino},
        static_cast<std::uintmax_t>(end),
        {},
      };
    }

    std::filesystem::path steam_game_process_log_path(
      const proc::ctx_t &app,
      const boost::process::v1::environment &env
    ) {
      const auto home = env_value(app, env, "HOME");
      if (home.empty()) {
        return {};
      }

      const std::array candidates {
        std::filesystem::path {home} / ".local/share/Steam/logs/gameprocess_log.txt",
        std::filesystem::path {home} / ".steam/steam/logs/gameprocess_log.txt",
      };
      for (const auto &candidate : candidates) {
        if (steam_game_process_log_identity(candidate)) {
          return candidate;
        }
      }
      return candidates.front();
    }

    bool dispatch_steam_big_picture_action(
      const std::string &reference_command,
      std::string_view action,
      const boost::process::v1::environment &env
    ) {
      const auto command = canonical_steam_big_picture_command(reference_command, action);
      try {
        std::error_code ec;
        boost::filesystem::path working_dir;
        auto child = platf::run_command(
          false,
          true,
          command,
          working_dir,
          env,
          nullptr,
          ec,
          nullptr
        );
        if (ec) {
          BOOST_LOG(warning) << "steam_input_guard: failed to dispatch ["sv << command
                             << "]: "sv << ec.message();
          return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (child.running(ec) && !ec && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(50ms);
        }
        if (!ec && child.running(ec)) {
          std::error_code terminate_ec;
          child.terminate(terminate_ec);
          child.detach();
          BOOST_LOG(warning) << "steam_input_guard: timed out dispatching ["sv << command << ']';
          return false;
        }
        if (ec || child.exit_code() != 0) {
          BOOST_LOG(warning) << "steam_input_guard: dispatch failed ["sv << command
                             << "] exit="sv << child.exit_code()
                             << (ec ? " error="s + ec.message() : ""s);
          return false;
        }

        BOOST_LOG(info) << "steam_input_guard: completed ["sv << command << ']';
        return true;
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "steam_input_guard: dispatch threw for ["sv << command
                           << "]: "sv << e.what();
        return false;
      } catch (...) {
        BOOST_LOG(warning) << "steam_input_guard: dispatch threw for ["sv << command << ']';
        return false;
      }
    }

    void strip_mangohud_env(boost::process::v1::environment &env) {
      for (const char *key : {"MANGOHUD", "MANGOHUD_DLSYM", "MANGOHUD_CONFIG"}) {
        env.erase(key);
      }
    }
#endif

    bool app_gamepad_override_is_supported(const std::string &value) {
      if (value == "disabled"sv) {
        return true;
      }

      const auto supported_gamepads = platf::supported_gamepads(nullptr);
      return std::any_of(supported_gamepads.begin(), supported_gamepads.end(), [&](const auto &gamepad) {
        return gamepad.name == value;
      });
    }
  }  // namespace

#ifdef __linux__
  struct steam_big_picture_guard_runtime_t {
    std::atomic<bool> stop_requested {false};
    std::thread worker;
  };

  struct steam_big_picture_guard_snapshot_t {
    std::filesystem::path log_path;
    std::optional<steam_game_process_log_identity_t> identity;
    std::uintmax_t offset = 0;
  };

  namespace {
    using steam_big_picture_action_dispatch_t = std::function<bool(std::string_view)>;

    void run_steam_big_picture_input_guard(
      const std::shared_ptr<steam_big_picture_guard_runtime_t> &runtime,
      const std::filesystem::path &log_path,
      std::optional<steam_game_process_log_identity_t> initial_identity,
      std::uintmax_t initial_offset,
      const steam_big_picture_action_dispatch_t &dispatch_action
    ) {
      std::ifstream log;
      int pinned_log_fd = -1;
      std::optional<steam_game_process_log_identity_t> opened_identity;
      auto expected_identity = initial_identity;
      std::uintmax_t cursor = initial_offset;
      std::unordered_set<std::string> active_appids;
      bool big_picture_closed = false;
      bool first_open = true;
      auto next_close_attempt = std::chrono::steady_clock::time_point {};
      auto next_restore_attempt = std::chrono::steady_clock::time_point {};

      auto close_big_picture = [&]() {
        if (big_picture_closed || active_appids.empty()) {
          return true;
        }
        if (!dispatch_action("close/bigpicture"sv)) {
          next_close_attempt = std::chrono::steady_clock::now() + 1s;
          return false;
        }
        big_picture_closed = true;
        BOOST_LOG(info) << "steam_input_guard: closed Big Picture after Steam started a tracked game"sv;
        return true;
      };

      auto restore_big_picture = [&]() {
        if (!big_picture_closed) {
          return true;
        }
        if (!dispatch_action("open/bigpicture"sv)) {
          next_restore_attempt = std::chrono::steady_clock::now() + 1s;
          return false;
        }
        big_picture_closed = false;
        BOOST_LOG(info) << "steam_input_guard: restored Big Picture after tracked game exit"sv;
        return true;
      };

      auto restore_until_stopped = [&]() {
        while (big_picture_closed && !runtime->stop_requested.load(std::memory_order_acquire)) {
          if (std::chrono::steady_clock::now() >= next_restore_attempt) {
            restore_big_picture();
          }
          if (big_picture_closed) {
            std::this_thread::sleep_for(100ms);
          }
        }
      };

      auto close_log = [&]() {
        log.close();
        log.clear();
        if (pinned_log_fd >= 0) {
          ::close(pinned_log_fd);
          pinned_log_fd = -1;
        }
        opened_identity.reset();
      };

      try {
        BOOST_LOG(info) << "steam_input_guard: monitoring new Steam game lifecycle records in ["
                        << log_path.string() << ']';
        while (!runtime->stop_requested.load(std::memory_order_acquire)) {
          if (!log.is_open()) {
            const int candidate_fd = ::open(log_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (candidate_fd < 0) {
              const int open_errno = errno;
              if (open_errno == ENOENT) {
                std::this_thread::sleep_for(100ms);
                continue;
              }
              BOOST_LOG(warning) << "steam_input_guard: Steam log open failed; compatibility guard disabled for this launch: "sv
                                 << std::error_code {open_errno, std::generic_category()}.message();
              break;
            }

            struct stat opened_info {};
            if (::fstat(candidate_fd, &opened_info) != 0 || !S_ISREG(opened_info.st_mode)) {
              const int stat_errno = errno ? errno : EINVAL;
              ::close(candidate_fd);
              BOOST_LOG(warning) << "steam_input_guard: Steam log identity is uncertain; compatibility guard disabled for this launch: "sv
                                 << std::error_code {stat_errno, std::generic_category()}.message();
              break;
            }
            const steam_game_process_log_identity_t candidate_identity {opened_info.st_dev, opened_info.st_ino};
            if (first_open && expected_identity && candidate_identity != *expected_identity) {
              ::close(candidate_fd);
              BOOST_LOG(warning) << "steam_input_guard: Steam log changed after the pre-launch snapshot; compatibility guard disabled for this launch"sv;
              break;
            }

            if (!open_steam_game_process_log_from_pinned_descriptor(log, candidate_fd)) {
              log.clear();
              ::close(candidate_fd);
              BOOST_LOG(warning) << "steam_input_guard: could not bind the Steam log stream to its pinned descriptor; compatibility guard disabled for this launch"sv;
              break;
            }
            pinned_log_fd = candidate_fd;
            opened_identity = candidate_identity;

            if (first_open && expected_identity) {
              log.seekg(static_cast<std::streamoff>(initial_offset), std::ios::beg);
              cursor = initial_offset;
            } else {
              log.seekg(0, std::ios::beg);
              cursor = 0;
            }
            if (!log) {
              close_log();
              BOOST_LOG(warning) << "steam_input_guard: could not seek the Steam log to its pre-launch cursor; compatibility guard disabled for this launch"sv;
              break;
            }
            first_open = false;
          }

          std::string line;
          while (!runtime->stop_requested.load(std::memory_order_acquire) && std::getline(log, line)) {
            if (log.eof()) {
              log.clear();
              log.seekg(static_cast<std::streamoff>(cursor), std::ios::beg);
              break;
            }
            cursor += line.size() + 1;
            const auto transition = apply_steam_big_picture_guard_event(active_appids, line);
            if (transition.close_big_picture) {
              close_big_picture();
            }
            if (transition.open_big_picture) {
              restore_big_picture();
            }
          }
          const bool log_read_failed = log.bad();
          log.clear();

          struct stat pinned_info {};
          const bool pinned_stat_failed = pinned_log_fd < 0 || ::fstat(pinned_log_fd, &pinned_info) != 0;
          const auto current_identity = steam_game_process_log_identity(log_path);
          const bool log_replaced = !current_identity || !opened_identity ||
                                    *current_identity != *opened_identity;
          const bool log_truncated = !pinned_stat_failed &&
                                     static_cast<std::uintmax_t>(pinned_info.st_size) < cursor;
          if (log_replaced || log_truncated || log_read_failed || pinned_stat_failed) {
            BOOST_LOG(warning) << "steam_input_guard: Steam game log became uncertain; restoring Big Picture and disabling the guard for this launch"sv;
            active_appids.clear();
            close_log();
            restore_until_stopped();
            break;
          }

          if (!big_picture_closed && !active_appids.empty() &&
              std::chrono::steady_clock::now() >= next_close_attempt) {
            close_big_picture();
          }
          if (big_picture_closed && active_appids.empty() &&
              std::chrono::steady_clock::now() >= next_restore_attempt) {
            restore_big_picture();
          }
          std::this_thread::sleep_for(100ms);
        }
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "steam_input_guard: watcher failed: "sv << e.what();
        if (big_picture_closed) {
          restore_big_picture();
        }
      } catch (...) {
        BOOST_LOG(error) << "steam_input_guard: watcher failed with an unknown exception"sv;
        if (big_picture_closed) {
          restore_big_picture();
        }
      }
      close_log();
    }
  }  // namespace
#endif

#if defined(POLARIS_TESTS)
#ifdef __linux__
  bool steam_big_picture_input_guard_enabled_for_tests(
    const proc::ctx_t &app,
    bool use_cage_compositor,
    bool mirror_desktop
  ) {
    return steam_big_picture_input_guard_enabled(app, use_cage_compositor, mirror_desktop);
  }

  std::string steam_big_picture_log_path_for_tests(
    const proc::ctx_t &app,
    const boost::process::v1::environment &env
  ) {
    return steam_game_process_log_path(app, env).string();
  }

  bool steam_big_picture_atomic_snapshot_survives_path_replacement_for_tests() {
    static std::atomic<std::uint64_t> sequence {0};
    const auto path = std::filesystem::temp_directory_path() /
                      ("polaris-steam-atomic-snapshot-"s + std::to_string(::getpid()) + '-' +
                       std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".log");
    const auto rotated_path = std::filesystem::path {path.string() + ".rotated"};
    const std::string old_content = "old-log\n";
    {
      std::ofstream old_log(path, std::ios::trunc);
      old_log << old_content;
    }

    const auto result = snapshot_steam_game_process_log(path, [&]() {
      std::filesystem::rename(path, rotated_path);
      std::ofstream replacement(path, std::ios::trunc);
      replacement << "replacement-log-is-longer\n";
    });
    const auto rotated_identity = steam_game_process_log_identity(rotated_path);
    const auto replacement_identity = steam_game_process_log_identity(path);
    std::error_code cleanup_ec;
    std::filesystem::remove(path, cleanup_ec);
    cleanup_ec.clear();
    std::filesystem::remove(rotated_path, cleanup_ec);

    return result.status == steam_game_process_log_snapshot_status_e::captured &&
           result.identity && rotated_identity && replacement_identity &&
           *result.identity == *rotated_identity &&
           !(*result.identity == *replacement_identity) &&
           result.offset == old_content.size();
  }

  bool steam_big_picture_pinned_stream_survives_path_replacement_for_tests() {
    static std::atomic<std::uint64_t> sequence {0};
    const auto path = std::filesystem::temp_directory_path() /
                      ("polaris-steam-pinned-stream-"s + std::to_string(::getpid()) + '-' +
                       std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".log");
    const auto rotated_path = std::filesystem::path {path.string() + ".rotated"};
    const std::string old_content = "old-log";
    {
      std::ofstream old_log(path, std::ios::trunc);
      old_log << old_content << '\n';
    }

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return false;
    }
    auto close_fd = util::fail_guard([fd]() {
      ::close(fd);
    });
    std::filesystem::rename(path, rotated_path);
    {
      std::ofstream replacement(path, std::ios::trunc);
      replacement << "replacement-log" << '\n';
    }

    std::ifstream pinned_stream;
    const bool opened = open_steam_game_process_log_from_pinned_descriptor(pinned_stream, fd);
    std::string line;
    if (opened) {
      std::getline(pinned_stream, line);
    }
    std::error_code cleanup_ec;
    std::filesystem::remove(path, cleanup_ec);
    cleanup_ec.clear();
    std::filesystem::remove(rotated_path, cleanup_ec);
    return opened && line == old_content;
  }

  steam_game_process_event_t parse_steam_game_process_event_for_tests(std::string_view line) {
    const auto parsed = parse_steam_game_process_event(line);
    steam_game_process_event_kind_e kind = steam_game_process_event_kind_e::none;
    if (parsed.kind == steam_game_process_event_kind_internal_e::started) {
      kind = steam_game_process_event_kind_e::started;
    } else if (parsed.kind == steam_game_process_event_kind_internal_e::stopped) {
      kind = steam_game_process_event_kind_e::stopped;
    }
    return {kind, parsed.appid};
  }

  steam_big_picture_guard_transition_t apply_steam_big_picture_guard_event_for_tests(
    std::unordered_set<std::string> &active_appids,
    std::string_view line
  ) {
    const auto transition = apply_steam_big_picture_guard_event(active_appids, line);
    return {
      transition.close_big_picture,
      transition.open_big_picture,
      transition.active_games,
    };
  }

  std::vector<std::string> run_steam_big_picture_guard_file_scenario_for_tests(
    steam_big_picture_guard_file_scenario_e scenario
  ) {
    static std::atomic<std::uint64_t> sequence {0};
    const auto path = std::filesystem::temp_directory_path() /
                      ("polaris-steam-input-guard-"s + std::to_string(::getpid()) + '-' +
                       std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".log");
    const auto start_line =
      "[2026-07-14 12:09:51] AppID 2416450 adding PID 1741315 as a tracked process "
      "\"/steam/steam-launch-wrapper -- /steam/reaper SteamLaunch AppId=2416450 -- MOUSE.exe\""s;
    const auto stop_line = "[2026-07-14 12:20:00] Remove 2416450 from running list"s;
    const auto rotated_path = std::filesystem::path {path.string() + ".rotated"};

    {
      std::ofstream initial(path, std::ios::trunc);
      if (!initial) {
        throw std::runtime_error("could not create Steam guard test log");
      }
      initial << start_line << '\n';
    }

    const auto initial_identity = steam_game_process_log_identity(path);
    std::error_code size_ec;
    const auto initial_offset = std::filesystem::file_size(path, size_ec);
    if (!initial_identity || size_ec) {
      std::filesystem::remove(path, size_ec);
      throw std::runtime_error("could not snapshot Steam guard test log");
    }

    if (scenario == steam_big_picture_guard_file_scenario_e::replacement_before_watcher_start) {
      std::error_code rotate_ec;
      std::filesystem::rename(path, rotated_path, rotate_ec);
      if (rotate_ec) {
        throw std::runtime_error("could not rotate Steam guard test log before watcher start");
      }
      std::ofstream replacement(path, std::ios::trunc);
      replacement << std::string(initial_offset, 'x') << '\n' << start_line << '\n';
    }

    auto runtime = std::make_shared<steam_big_picture_guard_runtime_t>();
    std::mutex actions_mutex;
    std::condition_variable actions_changed;
    std::vector<std::string> actions;
    bool rejected_first_close = false;
    bool rejected_first_open = false;
    const steam_big_picture_action_dispatch_t dispatch_action = [&](std::string_view action) {
      bool accept = true;
      {
        std::lock_guard lock(actions_mutex);
        actions.emplace_back(action);
        if (scenario == steam_big_picture_guard_file_scenario_e::close_dispatch_retry &&
            action == "close/bigpicture"sv && !rejected_first_close) {
          rejected_first_close = true;
          accept = false;
        }
        if (scenario == steam_big_picture_guard_file_scenario_e::open_dispatch_retry &&
            action == "open/bigpicture"sv && !rejected_first_open) {
          rejected_first_open = true;
          accept = false;
        }
      }
      actions_changed.notify_all();
      return accept;
    };

    auto stop_and_join = [&]() {
      runtime->stop_requested.store(true, std::memory_order_release);
      if (runtime->worker.joinable()) {
        runtime->worker.join();
      }
    };
    auto cleanup = [&]() {
      std::error_code cleanup_ec;
      std::filesystem::remove(path, cleanup_ec);
      cleanup_ec.clear();
      std::filesystem::remove(rotated_path, cleanup_ec);
    };
    auto wait_for_actions = [&](std::size_t count) {
      std::unique_lock lock(actions_mutex);
      if (!actions_changed.wait_for(lock, 3s, [&]() { return actions.size() >= count; })) {
        throw std::runtime_error("Steam guard test action timeout");
      }
    };

    try {
      runtime->worker = std::thread([
        runtime,
        path,
        initial_identity,
        initial_offset,
        dispatch_action
      ]() {
        run_steam_big_picture_input_guard(
          runtime,
          path,
          initial_identity,
          initial_offset,
          dispatch_action
        );
      });

      if (scenario == steam_big_picture_guard_file_scenario_e::replacement_before_watcher_start) {
        std::unique_lock lock(actions_mutex);
        actions_changed.wait_for(lock, 500ms, [&]() { return !actions.empty(); });
        lock.unlock();
        stop_and_join();
        cleanup();
        std::lock_guard actions_lock(actions_mutex);
        return actions;
      }
      {
        std::unique_lock lock(actions_mutex);
        if (actions_changed.wait_for(lock, 300ms, [&]() { return !actions.empty(); })) {
          throw std::runtime_error("Steam guard consumed a stale lifecycle record");
        }
      }
      {
        std::ofstream append(path, std::ios::app);
        append << start_line << '\n';
      }
      wait_for_actions(1);

      if (scenario == steam_big_picture_guard_file_scenario_e::close_dispatch_retry) {
        wait_for_actions(2);
      } else if (scenario == steam_big_picture_guard_file_scenario_e::open_dispatch_retry) {
        std::ofstream append(path, std::ios::app);
        append << stop_line << '\n';
        append.close();
        wait_for_actions(3);
      } else if (scenario == steam_big_picture_guard_file_scenario_e::appended_lifecycle) {
        std::ofstream append(path, std::ios::app);
        append << stop_line << '\n';
        append.close();
        wait_for_actions(2);
      } else if (scenario == steam_big_picture_guard_file_scenario_e::truncation_while_closed) {
        std::ofstream truncate(path, std::ios::trunc);
        truncate.close();
        wait_for_actions(2);
      } else if (scenario == steam_big_picture_guard_file_scenario_e::replacement_with_stale_records_while_closed) {
        std::error_code rotate_ec;
        std::filesystem::rename(path, rotated_path, rotate_ec);
        if (rotate_ec) {
          throw std::runtime_error("could not rotate Steam guard test log");
        }
        std::ofstream replacement(path, std::ios::trunc);
        replacement << start_line << '\n';
        replacement.close();
        wait_for_actions(2);
        std::unique_lock lock(actions_mutex);
        actions_changed.wait_for(lock, 500ms, [&]() { return actions.size() >= 3; });
      }

      stop_and_join();
      cleanup();
      std::lock_guard lock(actions_mutex);
      return actions;
    } catch (...) {
      stop_and_join();
      cleanup();
      throw;
    }
  }
#endif

  bool should_publish_stream_ended_after_terminate_for_tests(bool had_running_app, int active_sessions, std::string_view session_state) {
    return should_publish_stream_ended_after_terminate(had_running_app, active_sessions, session_state);
  }

  nlohmann::json classify_host_pause_session_for_tests(
      const stream_stats::stats_t &stats,
      double target_fps,
      bool current_virtual_display) {
    const auto classification = classify_host_pause_session(
      stats,
      target_fps,
      current_virtual_display
    );
    return {
      {"health_grade", classification.health_grade},
      {"primary_issue", classification.primary_issue},
      {"host_render_limited", classification.host_render_limited}
    };
  }

  std::optional<int> resolve_device_db_launch_bitrate_for_tests(
      int configured_max_bitrate,
      const std::optional<int> &paired_target_bitrate_kbps,
      bool ai_auto_quality_enabled,
      const std::string &device_name,
      const std::string &app_name) {
    optimization_locks_t locks;
    locks.bitrate = launch_bitrate_is_locked(
      configured_max_bitrate,
      paired_target_bitrate_kbps.has_value(),
      ai_auto_quality_enabled
    );

    resolved_session_optimization_t resolved;
    if (paired_target_bitrate_kbps.has_value()) {
      resolved.target_bitrate_kbps = *paired_target_bitrate_kbps;
      resolved.bitrate_source = "paired_client";
      note_layer(resolved, "paired_client");
    }

    const auto device_optimization = device_db::get_optimization(device_name, app_name);
    apply_optimization_layer(resolved, locks, device_optimization, "device_db");
    return resolved.target_bitrate_kbps;
  }
#endif

#if defined(__linux__)
  desktop_launch_safety_policy_t resolve_desktop_launch_safety_policy(
    bool private_stream_requested,
    bool mirror_desktop_explicit,
    const proc::ctx_t &app,
    bool desktop_steam_active,
    bool active_desktop_game
  ) {
    return resolve_desktop_launch_safety_policy(
      private_stream_requested,
      mirror_desktop_explicit,
      false,
      app,
      desktop_steam_active,
      active_desktop_game
    );
  }

  desktop_launch_safety_policy_t resolve_desktop_launch_safety_policy(
    bool private_stream_requested,
    bool mirror_desktop_explicit,
    bool force_private_after_desktop_steam_shutdown,
    const proc::ctx_t &app,
    bool desktop_steam_active,
    bool active_desktop_game
  ) {
    return resolve_desktop_launch_safety_policy_impl(
      private_stream_requested,
      mirror_desktop_explicit,
      context_uses_steam(app),
      desktop_steam_active,
      active_desktop_game,
      force_private_after_desktop_steam_shutdown
    );
  }

  nlohmann::json desktop_launch_safety_policy_to_json(const desktop_launch_safety_policy_t &policy) {
    return {
      {"desktopSteamActive", policy.desktopSteamActive},
      {"physicalDisplayRisk", policy.physicalDisplayRisk},
      {"canLaunchPrivateStream", policy.canLaunchPrivateStream},
      {"canMirrorDesktop", policy.canMirrorDesktop},
      {"canForceCloseDesktopSteamForPrivateStream", policy.canForceCloseDesktopSteamForPrivateStream},
      {"recommendedAction", policy.recommendedAction},
      {"privateStreamUnavailableReason", policy.privateStreamUnavailableReason},
      {"forcePrivateStreamLabel", policy.forcePrivateStreamLabel},
      {"desktopGameDetection", "polaris_running_app_only"},
    };
  }

  bool desktop_steam_client_active() {
    return desktop_steam_client_active_impl();
  }

  bool request_desktop_steam_shutdown_for_private_stream() {
    const auto command = canonical_steam_shutdown_command("steam");
    BOOST_LOG(info) << "process: explicit Nova request closing desktop Steam before private stream using [" << command << "]";
    auto env = boost::this_process::environment();
    boost::system::error_code ec;
    boost::filesystem::path working_dir {};
    auto child = platf::run_command(false, true, command, working_dir, env, nullptr, ec, nullptr);
    if (ec) {
      BOOST_LOG(warning) << "process: explicit desktop Steam shutdown command failed: " << ec.message();
      return false;
    }
    child.detach();
    for (int i = 0; i < 50; ++i) {
      if (!desktop_steam_client_active_impl()) {
        return true;
      }
      std::this_thread::sleep_for(100ms);
    }
    BOOST_LOG(warning) << "process: desktop Steam still active after explicit shutdown wait";
    return !desktop_steam_client_active_impl();
  }
#endif

#if defined(POLARIS_TESTS) && defined(__linux__)
  desktop_launch_safety_policy_t resolve_desktop_launch_safety_policy_for_tests(
    bool private_stream_requested,
    bool mirror_desktop_explicit,
    bool app_uses_steam,
    bool desktop_steam_active,
    bool active_desktop_game,
    bool force_private_after_desktop_steam_shutdown
  ) {
    return resolve_desktop_launch_safety_policy_impl(
      private_stream_requested,
      mirror_desktop_explicit,
      app_uses_steam,
      desktop_steam_active,
      active_desktop_game,
      force_private_after_desktop_steam_shutdown
    );
  }

  bool desktop_steam_client_process_for_tests(std::string_view comm,
                                               std::string_view argv0_path,
                                               std::string_view cmdline,
                                               std::string_view status) {
    return is_active_desktop_steam_client_process(comm, argv0_path, cmdline, status);
  }

  bool desktop_steam_proc_open_error_fails_closed_for_tests() {
    const auto previous = forced_desktop_steam_proc_open_error;
    auto restore = util::fail_guard([previous]() {
      forced_desktop_steam_proc_open_error = previous;
    });
    forced_desktop_steam_proc_open_error = true;
    return desktop_steam_client_active_impl();
  }

  bool desktop_steam_proc_enumeration_error_fails_closed_for_tests() {
    const auto previous = forced_proc_directory_enumeration_error;
    auto restore = util::fail_guard([previous]() {
      forced_proc_directory_enumeration_error = previous;
    });
    forced_proc_directory_enumeration_error = true;
    return desktop_steam_client_active_impl();
  }

  bool desktop_steam_proc_read_error_fails_closed_for_tests(pid_t forced_pid) {
    const auto previous_error_pid = forced_desktop_steam_proc_read_error_pid;
    const auto previous_only_pid = forced_desktop_steam_scan_only_pid;
    auto restore = util::fail_guard([previous_error_pid, previous_only_pid]() {
      forced_desktop_steam_proc_read_error_pid = previous_error_pid;
      forced_desktop_steam_scan_only_pid = previous_only_pid;
    });
    forced_desktop_steam_proc_read_error_pid = forced_pid;
    forced_desktop_steam_scan_only_pid = forced_pid;
    return desktop_steam_client_active_impl();
  }

  bool cage_mangohud_allowed_for_session_for_tests(const proc::ctx_t &app,
                                                   bool use_cage_compositor,
                                                   bool requested_headless) {
    return cage_mangohud_allowed_for_session(app, use_cage_compositor, requested_headless);
  }

  bool should_skip_steam_shutdown_undo_after_cage_cleanup_for_tests(const proc::ctx_t &app,
                                                                   const proc::cmd_t &cmd,
                                                                   bool use_cage_compositor) {
    return should_skip_steam_shutdown_undo_after_cage_cleanup(app, cmd, use_cage_compositor);
  }

  bool should_terminate_session_owned_steam_before_cage_stop_for_tests(
    const proc::ctx_t &app,
    bool session_owned_cage,
    bool session_generation_available,
    bool session_owned_steam_client_active,
    bool unowned_steam_client_active,
    bool ownership_capture_complete
  ) {
    return should_terminate_session_owned_steam_before_cage_stop(
      app,
      session_owned_cage,
      session_generation_available,
      session_owned_steam_client_active,
      unowned_steam_client_active,
      ownership_capture_complete
    );
  }

  bool steam_shutdown_process_is_unowned_active_for_tests(
    std::string_view comm,
    std::string_view argv0_path,
    std::string_view cmdline,
    std::string_view status,
    std::string_view environ,
    std::string_view session_instance_id
  ) {
    return steam_shutdown_process_is_unowned_active(
      comm, argv0_path, cmdline, status, environ, session_instance_id
    );
  }

  bool isolated_session_environ_matches_for_tests(
    std::string_view environ,
    std::string_view session_instance_id
  ) {
    return proc_environ_matches_isolated_session(environ, session_instance_id);
  }

  bool session_instance_environment_is_child_only_for_tests(
    std::string_view session_instance_id
  ) {
    constexpr auto key = "POLARIS_SESSION_INSTANCE_ID";
    const auto original = copy_env_var(key);
    platf::unset_env(key);

    boost::process::v1::environment env = boost::this_process::environment();
    std::unordered_set<std::string> tracked_keys;
    set_child_only_session_env_var(
      env,
      tracked_keys,
      key,
      std::string {session_instance_id}
    );

    const bool child_has_exact_value = env[key].to_string() == session_instance_id;
    const bool parent_is_untouched = std::getenv(key) == nullptr;
    const bool key_is_tracked = tracked_keys.contains(key);
    restore_env_var(key, original);
    return child_has_exact_value && parent_is_untouched && key_is_tracked;
  }

  std::optional<std::uint64_t> proc_start_time_from_stat_for_tests(std::string_view stat) {
    return proc_start_time_from_stat(stat);
  }

  bool steam_pidfd_capture_identity_matches_for_tests(
    std::optional<std::uint64_t> before,
    std::optional<std::uint64_t> after
  ) {
    return steam_pidfd_capture_identity_matches(before, after);
  }

  bool terminate_pid_with_pidfd_for_tests(
    pid_t pid,
    std::chrono::milliseconds graceful_timeout,
    std::chrono::milliseconds kill_timeout
  ) {
    int open_error = 0;
    auto handle = open_process_pidfd(pid, open_error);
    if (!handle) {
      return false;
    }
    std::vector<pidfd_handle_t> handles;
    handles.emplace_back(std::move(*handle));
    return terminate_pidfds(handles, graceful_timeout, kill_timeout, "test process"sv);
  }

  bool terminate_pid_with_pidfd_after_wait_entry_for_tests(
    pid_t pid,
    std::chrono::milliseconds graceful_timeout,
    std::chrono::milliseconds kill_timeout,
    std::atomic<bool> &wait_entered
  ) {
    const auto previous = pidfd_wait_entered_for_tests;
    auto restore = util::fail_guard([previous]() {
      pidfd_wait_entered_for_tests = previous;
    });
    pidfd_wait_entered_for_tests = &wait_entered;
    return terminate_pid_with_pidfd_for_tests(pid, graceful_timeout, kill_timeout);
  }

  bool terminate_pid_with_forced_zero_poll_eintr_for_tests(
    pid_t pid,
    std::chrono::milliseconds graceful_timeout,
    std::chrono::milliseconds kill_timeout,
    int forced_interruptions,
    std::chrono::milliseconds interruption_delay
  ) {
    const auto previous_interruptions = forced_pidfd_zero_poll_interruptions;
    const auto previous_delay = forced_pidfd_zero_poll_interruption_delay;
    forced_pidfd_zero_poll_interruptions = forced_interruptions;
    forced_pidfd_zero_poll_interruption_delay = interruption_delay;
    auto restore_fault = util::fail_guard([previous_interruptions, previous_delay]() {
      forced_pidfd_zero_poll_interruptions = previous_interruptions;
      forced_pidfd_zero_poll_interruption_delay = previous_delay;
    });
    return terminate_pid_with_pidfd_for_tests(pid, graceful_timeout, kill_timeout);
  }

  bool terminate_exact_generation_processes_for_tests(std::string_view session_instance_id) {
    return terminate_isolated_session_processes(session_instance_id, "during exact-generation test"sv);
  }

  bool exact_generation_proc_enumeration_error_fails_closed_for_tests(
    std::string_view session_instance_id
  ) {
    const auto previous = forced_proc_directory_enumeration_error;
    auto restore = util::fail_guard([previous]() {
      forced_proc_directory_enumeration_error = previous;
    });
    forced_proc_directory_enumeration_error = true;
    return !terminate_isolated_session_processes(session_instance_id, "during enumeration-error test"sv);
  }

  bool exact_generation_unknown_ancestry_fails_closed_for_tests(
    std::string_view session_instance_id,
    pid_t forced_unknown_pid
  ) {
    const auto previous = forced_proc_ancestry_unknown_pid;
    auto restore = util::fail_guard([previous]() {
      forced_proc_ancestry_unknown_pid = previous;
    });
    forced_proc_ancestry_unknown_pid = forced_unknown_pid;
    return !terminate_isolated_session_processes(session_instance_id, "during unknown-ancestry test"sv);
  }

  bool exact_generation_post_capture_enumeration_error_fails_closed_for_tests(
    std::string_view session_instance_id,
    pid_t captured_pid
  ) {
    const auto previous_after_pid = forced_proc_directory_error_after_capture_pid;
    const auto previous_error = forced_proc_directory_enumeration_error;
    auto restore = util::fail_guard([previous_after_pid, previous_error]() {
      forced_proc_directory_error_after_capture_pid = previous_after_pid;
      forced_proc_directory_enumeration_error = previous_error;
    });
    forced_proc_directory_error_after_capture_pid = captured_pid;
    return !terminate_isolated_session_processes(
      session_instance_id,
      "during post-capture enumeration-error test"sv
    );
  }

  bool exact_generation_reused_pid_fails_closed_for_tests(
    std::string_view session_instance_id,
    pid_t reused_pid
  ) {
    const auto previous = forced_reused_exact_generation_pid;
    auto restore = util::fail_guard([previous]() {
      forced_reused_exact_generation_pid = previous;
    });
    forced_reused_exact_generation_pid = reused_pid;
    return !terminate_isolated_session_processes(session_instance_id, "during reused-pid test"sv);
  }

  bool exact_generation_pidfd_open_error_fails_closed_for_tests(
    std::string_view session_instance_id,
    pid_t forced_pid
  ) {
    const auto previous = forced_pidfd_open_failure_pid;
    auto restore = util::fail_guard([previous]() {
      forced_pidfd_open_failure_pid = previous;
    });
    forced_pidfd_open_failure_pid = forced_pid;
    return !terminate_isolated_session_processes(session_instance_id, "during pidfd-open-error test"sv);
  }

  bool proc_t::isolated_session_capture_failure_retains_generation_for_tests(
    std::string_view session_instance_id,
    pid_t forced_capture_failure_pid
  ) {
    const auto previous_failure_pid = forced_isolated_session_capture_failure_pid;
    const auto previous_instance_id = _session_instance_id;
    const auto previous_used_cage = _session_used_cage_compositor;
    const auto previous_cleanup_complete = _exact_generation_cleanup_complete;
    forced_isolated_session_capture_failure_pid = forced_capture_failure_pid;
    _session_instance_id = session_instance_id;
    _session_used_cage_compositor = true;
    _exact_generation_cleanup_complete = true;
    auto restore_state = util::fail_guard([
      this,
      previous_failure_pid,
      previous_instance_id,
      previous_used_cage,
      previous_cleanup_complete
    ]() {
      forced_isolated_session_capture_failure_pid = previous_failure_pid;
      _session_instance_id = previous_instance_id;
      _session_used_cage_compositor = previous_used_cage;
      _exact_generation_cleanup_complete = previous_cleanup_complete;
    });

    terminate_isolated_session_generation();
    finish_isolated_session_generation_cleanup();
    return !_exact_generation_cleanup_complete &&
           _session_used_cage_compositor &&
           _session_instance_id == session_instance_id &&
           isolated_session_generation_blocks_launch(
             _session_used_cage_compositor,
             !_session_instance_id.empty()
           );
  }

  bool proc_t::terminate_session_owned_steam_before_cage_stop_for_tests(
    const ctx_t &app,
    bool session_owned_cage,
    std::string_view session_instance_id
  ) {
    const auto previous_app = _app;
    const auto previous_used_cage = _session_used_cage_compositor;
    const auto previous_instance_id = _session_instance_id;
    _app = app;
    _session_used_cage_compositor = session_owned_cage;
    _session_instance_id = session_instance_id;
    auto restore_state = util::fail_guard([
      this,
      previous_app,
      previous_used_cage,
      previous_instance_id
    ]() {
      _app = previous_app;
      _session_used_cage_compositor = previous_used_cage;
      _session_instance_id = previous_instance_id;
    });
    return terminate_session_owned_steam_before_cage_stop();
  }

  bool isolated_session_capture_failure_retains_generation_for_tests(
    std::string_view session_instance_id,
    pid_t forced_capture_failure_pid
  ) {
    boost::process::v1::environment env = boost::this_process::environment();
    proc_t test_process {std::move(env), {}};
    return test_process.isolated_session_capture_failure_retains_generation_for_tests(
      session_instance_id,
      forced_capture_failure_pid
    );
  }

  bool terminate_session_owned_steam_before_cage_stop_for_tests(
    const ctx_t &app,
    bool session_owned_cage,
    std::string_view session_instance_id
  ) {
    boost::process::v1::environment env = boost::this_process::environment();
    proc_t test_process {std::move(env), {}};
    return test_process.terminate_session_owned_steam_before_cage_stop_for_tests(
      app,
      session_owned_cage,
      session_instance_id
    );
  }

  bool terminate_session_owned_steam_with_forced_capture_failure_for_tests(
    const ctx_t &app,
    bool session_owned_cage,
    std::string_view session_instance_id,
    pid_t forced_pid
  ) {
    const auto previous = forced_steam_ownership_capture_failure_pid;
    auto restore = util::fail_guard([previous]() {
      forced_steam_ownership_capture_failure_pid = previous;
    });
    forced_steam_ownership_capture_failure_pid = forced_pid;
    return terminate_session_owned_steam_before_cage_stop_for_tests(
      app,
      session_owned_cage,
      session_instance_id
    );
  }

  bool terminate_session_owned_steam_with_pidfd_open_error_for_tests(
    const ctx_t &app,
    bool session_owned_cage,
    std::string_view session_instance_id,
    pid_t forced_pid
  ) {
    const auto previous = forced_pidfd_open_failure_pid;
    auto restore = util::fail_guard([previous]() {
      forced_pidfd_open_failure_pid = previous;
    });
    forced_pidfd_open_failure_pid = forced_pid;
    return terminate_session_owned_steam_before_cage_stop_for_tests(
      app,
      session_owned_cage,
      session_instance_id
    );
  }

  bool terminate_session_owned_steam_with_post_capture_enumeration_error_for_tests(
    const ctx_t &app,
    bool session_owned_cage,
    std::string_view session_instance_id,
    pid_t captured_pid
  ) {
    const auto previous_after_pid = forced_proc_directory_error_after_capture_pid;
    const auto previous_error = forced_proc_directory_enumeration_error;
    auto restore = util::fail_guard([previous_after_pid, previous_error]() {
      forced_proc_directory_error_after_capture_pid = previous_after_pid;
      forced_proc_directory_enumeration_error = previous_error;
    });
    forced_proc_directory_error_after_capture_pid = captured_pid;
    return terminate_session_owned_steam_before_cage_stop_for_tests(
      app,
      session_owned_cage,
      session_instance_id
    );
  }

  bool terminate_session_owned_steam_with_reused_pid_for_tests(
    const ctx_t &app,
    bool session_owned_cage,
    std::string_view session_instance_id,
    pid_t reused_pid
  ) {
    const auto previous = forced_reused_exact_generation_pid;
    auto restore = util::fail_guard([previous]() {
      forced_reused_exact_generation_pid = previous;
    });
    forced_reused_exact_generation_pid = reused_pid;
    return terminate_session_owned_steam_before_cage_stop_for_tests(
      app,
      session_owned_cage,
      session_instance_id
    );
  }

  bool isolated_session_generation_blocks_launch_for_tests(
    bool session_owned_cage,
    bool generation_available
  ) {
    return isolated_session_generation_blocks_launch(session_owned_cage, generation_available);
  }

  bool isolated_session_cleanup_resets_router_for_tests(
    bool session_owned_cage,
    bool generation_available,
    bool exact_cleanup_complete
  ) {
    return isolated_session_cleanup_resets_router(
      session_owned_cage,
      generation_available,
      exact_cleanup_complete
    );
  }

  bool isolated_session_cleanup_clears_state_for_tests(
    bool session_owned_cage,
    bool generation_available,
    bool exact_cleanup_complete
  ) {
    return isolated_session_cleanup_clears_state(
      session_owned_cage,
      generation_available,
      exact_cleanup_complete
    );
  }

  bool isolated_session_uses_legacy_group_termination_for_tests(
    bool session_owned_cage,
    bool immediate
  ) {
    return isolated_session_uses_legacy_group_termination(session_owned_cage, immediate);
  }

  bool isolated_session_detaches_legacy_handles_for_tests(bool session_owned_cage) {
    return isolated_session_detaches_legacy_handles(session_owned_cage);
  }

#endif

#ifdef _WIN32
  VDISPLAY::DRIVER_STATUS vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::UNKNOWN;

  void onVDisplayWatchdogFailed() {
    vDisplayDriverStatus = VDISPLAY::DRIVER_STATUS::WATCHDOG_FAILED;
    VDISPLAY::closeVDisplayDevice();
  }

  void initVDisplayDriver() {
    vDisplayDriverStatus = VDISPLAY::openVDisplayDevice();
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      if (!VDISPLAY::startPingThread(onVDisplayWatchdogFailed)) {
        onVDisplayWatchdogFailed();
        return;
      }
    }
  }
#endif

#ifdef __linux__
  // Linux virtual display state — holds the active virtual display instance, if any
  static std::optional<virtual_display::vdisplay_t> linux_vdisplay;

  /**
   * @brief Check whether Linux virtual display support is available.
   *
   * Deliberately NOT latched per-process: availability changes at runtime
   * (evdi module loaded/unloaded, libevdi installed, compositor restarted).
   * A once-only latch here meant a Polaris started before the prerequisites
   * were in place would report "no backend available" forever, even after
   * detect_backend() itself succeeded. detect_backend() already maintains a
   * short TTL cache, so calling it directly stays cheap.
   */
  bool isLinuxVDisplayAvailable() {
    return virtual_display::detect_backend() != virtual_display::backend_e::NONE;
  }

  namespace linux_display {
    /**
     * @brief Run a command and capture its stdout (trimmed). Empty on failure.
     */
    static std::string exec_capture(const std::string &cmd) {
      FILE *pipe = popen(cmd.c_str(), "r");
      if (!pipe) {
        return {};
      }
      char buf[512];
      std::string result;
      while (fgets(buf, sizeof(buf), pipe)) {
        result += buf;
      }
      pclose(pipe);
      return result;
    }

    /**
     * @brief Whether kscreen-doctor currently lists an output with exactly this name.
     *
     * kscreen-doctor omits disconnected connectors entirely, so mere presence on an
     * "Output:" line means the connector is enumerated and addressable by kscreen-doctor
     * (whether enabled or disabled). The name is matched as a whole token so "DP-2" does
     * not spuriously match "DP-20" and "DVI-I-1" does not match "DVI-I-10".
     */
    static bool kscreen_output_present(const std::string &name) {
      if (name.empty()) {
        return false;
      }
      const std::string out = exec_capture("kscreen-doctor -o 2>/dev/null");
      auto is_tok = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
      };
      for (size_t pos = 0; (pos = out.find(name, pos)) != std::string::npos; pos += name.size()) {
        const char before = pos > 0 ? out[pos - 1] : ' ';
        const size_t after_idx = pos + name.size();
        const char after = after_idx < out.size() ? out[after_idx] : ' ';
        if (!is_tok(before) && !is_tok(after)) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief Poll for a kscreen output to appear, absorbing KWin's async hotplug lag.
     *
     * A freshly-connected EVDI (or any hotplugged connector) can take a beat to show up
     * in KScreen's enumeration after the DRM connect. Retry briefly rather than assume
     * absence — but bounded, so a genuinely-absent output aborts promotion safely instead
     * of hanging the launch.
     */
    static bool wait_for_kscreen_output(const std::string &name, int attempts = 20, int delay_ms = 250) {
      for (int i = 0; i < attempts; ++i) {
        if (kscreen_output_present(name)) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      }
      return false;
    }

    /**
     * @brief Enable the streaming display and set display priorities for a streaming session.
     * Uses kscreen-doctor to enable the streaming output and set it as priority 1.
     */
    void enable_streaming_display() {
      const auto &cfg = config::video.linux_display;
      if (!cfg.auto_manage_displays || cfg.streaming_output.empty()) {
        return;
      }
      // A cage/isolated session (Family Mode, or the whole-host headless preset) owns its
      // own off-screen compositor and must NOT touch the physical monitor — otherwise the
      // global swap config would dim the host's screen during Family Mode. Skip here; the
      // cage runtime handles its own display.
      if (cfg.use_cage_compositor) {
        return;
      }

      // A display cannot be swapped onto itself. Guard against streaming_output ==
      // primary_output: without this the swap command would enable AND disable the same
      // output in one call, leaving it (and possibly the only monitor) dark.
      const bool distinct_outputs = cfg.streaming_output != cfg.primary_output;
      if (cfg.headless_swap_primary && !cfg.primary_output.empty() && !distinct_outputs) {
        BOOST_LOG(warning) << "Linux display management: headless_swap_primary set but streaming_output and "
                              "primary_output are the same output ["sv << cfg.streaming_output
                           << "] — cannot swap a display onto itself; leaving it enabled without swapping"sv;
      }

      std::string cmd = "kscreen-doctor output." + cfg.streaming_output + ".enable";
      if (cfg.headless_swap_primary && !cfg.primary_output.empty() && distinct_outputs) {
        // Swap the desktop ONTO the headless display: make it the primary and disable the
        // physical monitor, so the stream shows the real desktop while the physical screen
        // stays dark. disable_streaming_display() restores it on teardown. This is the
        // built-in replacement for manual kscreen-doctor prep-cmd swap scripts.
        cmd += " output." + cfg.streaming_output + ".priority.1";
        cmd += " output." + cfg.primary_output + ".disable";
      } else if (!cfg.primary_output.empty() && distinct_outputs) {
        // Legacy: enable the headless output as a SECONDARY, keep the physical primary
        // (KDE taskbar stays on the main display).
        cmd += " output." + cfg.primary_output + ".priority.1";
        cmd += " output." + cfg.streaming_output + ".priority.2";
      }

      BOOST_LOG(info) << "Linux display management: enabling streaming display ["sv << cmd << "]"sv;
      int ret = std::system(cmd.c_str());
      if (ret != 0) {
        BOOST_LOG(error) << "Linux display management: enable command failed with code ["sv << ret << "]"sv;
      }
      // Wait for display to come online before capture starts
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    /**
     * @brief Disable the streaming display and restore the primary display after a streaming session ends.
     * Uses kscreen-doctor to restore priority and disable the streaming output.
     */
    void disable_streaming_display() {
      const auto &cfg = config::video.linux_display;
      if (!cfg.auto_manage_displays || cfg.streaming_output.empty()) {
        return;
      }
      if (cfg.use_cage_compositor) {
        return;  // mirror enable_streaming_display(): cage sessions never swapped
      }

      // Mirror the enable-side guard: when streaming_output == primary_output no swap was
      // ever performed, so there is nothing to undo. Falling through would emit
      // output.X.disable on the shared output and black out the only display.
      const bool distinct_outputs = cfg.streaming_output != cfg.primary_output;
      if (cfg.headless_swap_primary && !cfg.primary_output.empty() && !distinct_outputs) {
        return;
      }

      std::string cmd = "kscreen-doctor";
      if (cfg.headless_swap_primary && !cfg.primary_output.empty() && distinct_outputs) {
        // Undo the swap: bring the physical monitor back as primary and turn the headless
        // display back off.
        cmd += " output." + cfg.primary_output + ".enable";
        cmd += " output." + cfg.primary_output + ".priority.1";
        cmd += " output." + cfg.streaming_output + ".disable";
      } else {
        if (!cfg.primary_output.empty() && distinct_outputs) {
          cmd += " output." + cfg.primary_output + ".priority.1";
        }
        cmd += " output." + cfg.streaming_output + ".priority.2 output." + cfg.streaming_output + ".disable";
      }

      BOOST_LOG(info) << "Linux display management: disabling streaming display ["sv << cmd << "]"sv;
      int ret = std::system(cmd.c_str());
      if (ret != 0) {
        BOOST_LOG(error) << "Linux display management: disable command failed with code ["sv << ret << "]"sv;
      }
    }
  }  // namespace linux_display
#endif

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
#ifdef __linux__
    virtual_display::cleanup_stale();
#endif
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (proc.valid()) {
      // avoid zombie process
      proc.detach();
    }
  }

  boost::filesystem::path find_working_directory(const std::string &cmd, const boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
#ifdef _WIN32
    auto parts = boost::program_options::split_winmain(cmd);
#else
    auto parts = boost::program_options::split_unix(cmd);
#endif
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).find("://") != std::string::npos) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  void proc_t::launch_input_only(std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    _session_lifecycle_gate->begin_launch();
    auto release_launch = util::fail_guard([this]() {
      _session_lifecycle_gate->finish_launch();
    });
    launch_input_only_impl(std::move(launch_session));
  }

  bool proc_t::launch_input_only_and_raise(std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    launch_input_only_impl(launch_session);
    return rtsp_stream::launch_session_raise(std::move(launch_session));
  }

  void proc_t::launch_input_only_impl(std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    ++_session_generation;
    _app_id = input_only_app_id;
    _app_name = "Remote Input";
    _app.uuid = REMOTE_INPUT_UUID;
    _app.terminate_on_pause = true;
    allow_client_commands = false;
    placebo = true;
    _launch_session = std::move(launch_session);
    _client_session_report_recorded = false;
    _client_session_report_recorded_at = {};
    _client_session_report_recorded_unique_id.clear();
    if (_launch_session && _launch_session->session_token.empty()) {
      _launch_session->session_token = generate_session_token();
    }

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
    system_tray::update_tray_playing(_app_name);
#endif
  }

  int proc_t::execute(const ctx_t& app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    _session_lifecycle_gate->begin_launch();
    auto release_launch = util::fail_guard([this]() {
      _session_lifecycle_gate->finish_launch();
    });
    const bool no_active_sessions_at_launch = rtsp_stream::session_count() == 0;
    return execute_impl(app, std::move(launch_session), no_active_sessions_at_launch);
  }

  int proc_t::execute_and_raise(
    const ctx_t& app,
    std::shared_ptr<rtsp_stream::launch_session_t> launch_session
  ) {
    const bool no_active_sessions_at_launch = rtsp_stream::session_count() == 0;
    const auto err = execute_impl(app, launch_session, no_active_sessions_at_launch);
    if (!err) {
      return rtsp_stream::launch_session_raise(std::move(launch_session)) ? 0 : 409;
    }
    return err;
  }

  bool proc_t::raise_session_for_admitted_launch(std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    return rtsp_stream::launch_session_raise(std::move(launch_session));
  }

  std::optional<std::uint64_t> proc_t::capture_session_launch_generation() const {
    return _session_lifecycle_gate->capture_launch_generation();
  }

  bool proc_t::try_begin_session_launch(std::uint64_t expected_generation) {
    if (!_session_lifecycle_gate->try_begin_rtsp_launch(expected_generation)) {
      return false;
    }
    if (rtsp_stream::session_snapshot({}).pending_sessions > 0) {
      _session_lifecycle_gate->finish_launch();
      return false;
    }
    return true;
  }

  void proc_t::finish_session_launch() {
    _session_lifecycle_gate->finish_launch();
  }

#ifdef __linux__
  void proc_t::stop_steam_big_picture_input_guard() {
    auto runtime = std::move(_steam_big_picture_guard);
    if (!runtime) {
      return;
    }

    runtime->stop_requested.store(true, std::memory_order_release);
    if (runtime->worker.joinable()) {
      runtime->worker.join();
    }
    BOOST_LOG(info) << "steam_input_guard: stopped"sv;
  }

  std::shared_ptr<const steam_big_picture_guard_snapshot_t> proc_t::snapshot_steam_big_picture_input_guard(
    bool use_cage_compositor,
    bool mirror_desktop
  ) const {
    if (!steam_big_picture_input_guard_enabled(_app, use_cage_compositor, mirror_desktop)) {
      return {};
    }

    auto snapshot = std::make_shared<steam_big_picture_guard_snapshot_t>();
    snapshot->log_path = steam_game_process_log_path(_app, _env);
    if (snapshot->log_path.empty()) {
      BOOST_LOG(warning) << "steam_input_guard: HOME is unavailable; compatibility guard disabled"sv;
      return {};
    }
    const auto log_snapshot = snapshot_steam_game_process_log(snapshot->log_path);
    if (log_snapshot.status == steam_game_process_log_snapshot_status_e::error) {
      BOOST_LOG(warning) << "steam_input_guard: initial Steam log metadata is uncertain; compatibility guard disabled: "sv
                         << log_snapshot.error.message();
      return {};
    }
    snapshot->identity = log_snapshot.identity;
    snapshot->offset = log_snapshot.offset;
    return snapshot;
  }

  void proc_t::start_steam_big_picture_input_guard(
    const boost::process::v1::environment &launch_env,
    std::shared_ptr<const steam_big_picture_guard_snapshot_t> snapshot
  ) {
    stop_steam_big_picture_input_guard();
    if (!snapshot) {
      return;
    }

    const auto reference_command = !_app.detached.empty() ? _app.detached.front() :
                                   !_app.cmd.empty() ? _app.cmd : "steam"s;
    auto runtime = std::make_shared<steam_big_picture_guard_runtime_t>();
    const steam_big_picture_action_dispatch_t dispatch_action = [reference_command, launch_env](std::string_view action) {
      auto action_env = launch_env;
      if (cage_display_router::is_running()) {
        const auto cage_socket = cage_display_router::get_wayland_socket();
        if (!cage_socket.empty()) {
          action_env["WAYLAND_DISPLAY"] = cage_socket;
          action_env["AT_SPI_BUS_ADDRESS"] = "";
        }
        const auto cage_display = cage_display_router::get_x11_display();
        if (!cage_display.empty()) {
          action_env["DISPLAY"] = cage_display;
        }
      }
      return dispatch_steam_big_picture_action(reference_command, action, action_env);
    };
    try {
      runtime->worker = std::thread([runtime, snapshot, dispatch_action]() {
        run_steam_big_picture_input_guard(
          runtime,
          snapshot->log_path,
          snapshot->identity,
          snapshot->offset,
          dispatch_action
        );
      });
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "steam_input_guard: could not start watcher; compatibility guard disabled: "sv
                         << e.what();
      return;
    } catch (...) {
      BOOST_LOG(warning) << "steam_input_guard: could not start watcher; compatibility guard disabled"sv;
      return;
    }
    _steam_big_picture_guard = std::move(runtime);
  }
#endif

  int proc_t::execute_impl(
    const ctx_t& app,
    std::shared_ptr<rtsp_stream::launch_session_t> launch_session,
    bool no_active_sessions_at_launch
  ) {
    auto &sync = session_lifecycle_sync();
    std::unique_lock<std::recursive_mutex> lifecycle_lock(sync.mutex);
    if (_app_id == input_only_app_id) {
      terminate_impl(false, false);
      std::this_thread::sleep_for(1s);
    } else {
      // Ensure starting from a clean slate
      terminate_impl(false, false);
    }

#ifdef __linux__
    if (isolated_session_generation_blocks_launch(
          _session_used_cage_compositor,
          !_session_instance_id.empty()
        )) {
      BOOST_LOG(error) << "process: refusing launch while an incompletely cleaned isolated session generation remains"sv;
      return 503;
    }
#endif

    ++_session_generation;
#ifdef __linux__
    _session_instance_id = generate_session_token();
    _session_used_cage_compositor = false;
#endif
    _app = app;
    _app_id = util::from_view(app.id);
    _app_name = app.name;
    _launch_session = launch_session;
    _client_session_report_recorded = false;
    _client_session_report_recorded_at = {};
    _client_session_report_recorded_unique_id.clear();
    if (_launch_session && _launch_session->session_token.empty()) {
      _launch_session->session_token = generate_session_token();
    }
    allow_client_commands = app.allow_client_commands;

#ifdef __linux__
    // Session-scoped overrides for per-app isolated sessions (family mode).
    // Forcing the globals makes every downstream read — the session snapshot
    // below, display policy, cage start, gamepad isolation, encoder-probe
    // deferral, audio capture — behave as a headless+cage session without
    // touching those call sites. Saved ONLY when the app opts in, so
    // mid-session config changes made during normal sessions are never
    // clobbered. restore_isolated_session_overrides() undoes this (hooked
    // into teardown on both the success and failure paths).
    if (_app.isolated_session && !(launch_session && launch_session->mirror_desktop)) {
      initial_headless_mode = config::video.linux_display.headless_mode;
      initial_use_cage_compositor = config::video.linux_display.use_cage_compositor;
      initial_prefer_gpu_native_capture = config::video.linux_display.prefer_gpu_native_capture;
      initial_audio_sink = config::audio.sink;
      initial_linux_display_saved = true;
      config::video.linux_display.headless_mode = true;
      config::video.linux_display.use_cage_compositor = true;
      // Keep the compositor truly off-screen: a GPU-native windowed fallback
      // would make the session visible on the host desktop, defeating the
      // isolation.
      config::video.linux_display.prefer_gpu_native_capture = false;
      // Audio isolation must hold in the CAPTURE pipeline too (rtsp copies
      // host_audio into the session flags), so force it at the source — and
      // clear any explicit sink so the virtual-sink env-tag routing path is
      // taken instead of switching the desktop user's default sink.
      launch_session->host_audio = false;
      config::audio.sink.clear();
      BOOST_LOG(info) << "process: app ["sv << _app.name
                      << "] opted into isolated off-screen session; forcing headless+cage and stream-only audio for this session"sv;
    }

    const bool use_cage_compositor_for_session =
      config::video.linux_display.use_cage_compositor &&
      !(launch_session && launch_session->mirror_desktop);
    const bool requested_headless_for_session =
      config::video.linux_display.headless_mode &&
      !(launch_session && launch_session->mirror_desktop);
    const auto steam_guard_snapshot = snapshot_steam_big_picture_input_guard(
      use_cage_compositor_for_session,
      launch_session && launch_session->mirror_desktop
    );
#endif

    this->initial_display = config::video.output_name;
    this->initial_color_range = config::video.color_range;
    this->initial_nvenc_tune = config::video.nvenc_tune;
    this->initial_max_bitrate = config::video.max_bitrate;
    this->initial_adaptive_max_bitrate = config::video.adaptive_bitrate.max_bitrate_kbps;
    this->initial_video_config_saved = true;

    launch_session->width = launch_session->requested_width;
    launch_session->height = launch_session->requested_height;
    launch_session->fps = launch_session->requested_fps;
    launch_session->target_bitrate_kbps.reset();
    launch_session->nvenc_tune.reset();
    launch_session->preferred_codec.reset();
    launch_session->optimization_source.clear();
    launch_session->optimization_reasoning.clear();
    launch_session->pacing_policy.clear();

    // Runtime game detection: extract Steam AppID and classify if not already set
    std::string game_category = _app.game_category;
    if (game_category.empty()) {
      std::string detected_appid = _app.steam_appid;
      if (detected_appid.empty()) {
        auto detect_from = [&](const std::string &s) {
          if (const auto detected = extract_steam_appid_from_command(s); detected) {
            detected_appid = *detected;
          }
        };
        detect_from(_app.cmd);
        if (detected_appid.empty()) {
          for (const auto &cmd : _app.detached) {
            detect_from(cmd);
            if (!detected_appid.empty()) break;
          }
        }
      }
      if (!detected_appid.empty()) {
        BOOST_LOG(debug) << "game_classifier: Detected Steam AppID "sv << detected_appid << " for \""sv << _app.name << '"';
      }
    }
    if (!game_category.empty() && game_category != "unknown") {
      BOOST_LOG(info) << "game_classifier: "sv << _app.name << " classified as "sv << game_category;
    }

    // Resolve session overrides in a strict order:
    // explicit user/client locks -> client profile -> device DB -> AI.
    optimization_locks_t optimization_locks;
    optimization_locks.display_mode = launch_session->user_locked_display_mode || !launch_session->enable_sops;
    optimization_locks.virtual_display = launch_session->user_locked_virtual_display;
    const bool auto_quality_enabled = ai_optimizer::is_enabled();
    optimization_locks.bitrate = launch_bitrate_is_locked(
      config::video.max_bitrate,
      launch_session->paired_target_bitrate_kbps.has_value(),
      auto_quality_enabled
    );
    if (!auto_quality_enabled && config::video.max_bitrate <= 0 &&
        !launch_session->paired_target_bitrate_kbps.has_value()) {
      BOOST_LOG(debug) << "session_optimization: AI Auto Quality disabled with max_bitrate=0; leaving bitrate to the client request"sv;
    }

    resolved_session_optimization_t resolved_optimization;
    if (launch_session->paired_target_bitrate_kbps.has_value()) {
      resolved_optimization.target_bitrate_kbps = *launch_session->paired_target_bitrate_kbps;
      resolved_optimization.bitrate_source = "paired_client";
      note_layer(resolved_optimization, "paired_client");
      BOOST_LOG(info) << "Client profile: overriding bitrate to "sv
                      << *launch_session->paired_target_bitrate_kbps << " kbps";
    }

    // Per-app output targeting: an app may pin capture to a specific output
    // (a dummy-plug connector or EVDI screen, by kernel connector name). Wins
    // over client-profile output preferences. config::video.output_name is
    // already saved to initial_display above and restored on teardown, so no
    // extra restore plumbing is needed for the capture target itself.
    const bool app_output_override = !_app.output_name.empty();
    if (app_output_override) {
      BOOST_LOG(info) << "process: app ["sv << _app.name
                      << "] pins capture output to ["sv << _app.output_name << ']';
      config::video.output_name = _app.output_name;
#ifdef __linux__
      // Let auto-managed display power follow the session's target: the pinned
      // output is enabled at session start and disabled at teardown. No-op
      // unless linux_auto_manage_displays is on. Never auto-manage the primary.
      if (_app.output_name != config::video.linux_display.primary_output) {
        initial_streaming_output = config::video.linux_display.streaming_output;
        initial_streaming_output_saved = true;
        config::video.linux_display.streaming_output = _app.output_name;
      }
#endif
    }

    auto client_profile = client_profiles::get_client_profile(launch_session->device_name);
    if (client_profile) {
      BOOST_LOG(info) << "Applying client profile for \""sv << launch_session->device_name << '"';

      if (!client_profile->output_name.empty() && !app_output_override) {
        BOOST_LOG(info) << "Client profile: overriding output_name to \""sv << client_profile->output_name << '"';
        config::video.output_name = client_profile->output_name;
      }

      if (client_profile->color_range.has_value()) {
        optimization_locks.color_range = true;
        resolved_optimization.color_range = client_profile->color_range;
        resolved_optimization.color_range_source = "client_profile";
        note_layer(resolved_optimization, "client_profile");
        BOOST_LOG(info) << "Client profile: overriding color_range to "sv << client_profile->color_range.value();
        config::video.color_range = client_profile->color_range.value();
      }

      if (client_profile->hdr.has_value()) {
        optimization_locks.hdr = true;
        resolved_optimization.hdr = client_profile->hdr;
        resolved_optimization.hdr_source = "client_profile";
        note_layer(resolved_optimization, "client_profile");
        BOOST_LOG(info) << "Client profile: overriding HDR to "sv << (client_profile->hdr.value() ? "enabled"sv : "disabled"sv);
        launch_session->enable_hdr = client_profile->hdr.value();
      }
    }

    auto device_optimization = device_db::get_optimization(launch_session->device_name, _app.name);
    apply_optimization_layer(resolved_optimization, optimization_locks, device_optimization, "device_db");

    std::optional<ai_optimizer::session_history_t> history;

    // AI optimizer: cached results override device DB; unknown devices get a sync request.
    if (auto_quality_enabled) {
      std::string gpu_info = config::video.adapter_name.empty()
        ? "NVIDIA GPU (NVENC)"s
        : config::video.adapter_name;
      history = ai_optimizer::get_session_history(launch_session->device_name, _app.name);
      auto device_info = device_db::get_device(launch_session->device_name);
      auto ai_opt = ai_optimizer::get_cached(launch_session->device_name, _app.name);
      if (ai_opt) {
        BOOST_LOG(info) << "ai_optimizer: Applying cached AI optimization for \""sv
                        << launch_session->device_name << "\" + \""sv << _app.name
                        << "\" — "sv << ai_opt->reasoning;
        apply_optimization_layer(resolved_optimization, optimization_locks, *ai_opt, ai_opt->source);
      } else if (!device_info) {
        BOOST_LOG(info) << "ai_optimizer: Unknown device \""sv << launch_session->device_name
                        << "\" — requesting sync AI optimization"sv;
        if (auto sync_opt = ai_optimizer::request_sync(
              launch_session->device_name, _app.name, gpu_info, game_category, history)) {
          BOOST_LOG(info) << "ai_optimizer: AI identified device — "sv << sync_opt->reasoning;
          apply_optimization_layer(resolved_optimization, optimization_locks, *sync_opt, sync_opt->source);
        }
      } else {
        bool applied_sync_ai = false;
        if (ai_optimizer::should_sync_on_cache_miss()) {
          BOOST_LOG(info) << "ai_optimizer: Cache miss for known device \""sv << launch_session->device_name
                          << "\" — requesting sync AI optimization before first session"sv;
          if (auto sync_opt = ai_optimizer::request_sync(
                launch_session->device_name, _app.name, gpu_info, game_category, history)) {
            BOOST_LOG(info) << "ai_optimizer: Applying fresh AI optimization for \""sv
                            << launch_session->device_name << "\" + \""sv << _app.name
                            << "\" — "sv << sync_opt->reasoning;
            apply_optimization_layer(resolved_optimization, optimization_locks, *sync_opt, sync_opt->source);
            applied_sync_ai = true;
          }
          if (!applied_sync_ai) {
            BOOST_LOG(warning) << "ai_optimizer: Sync optimization failed for known device \""sv
                               << launch_session->device_name << "\" — falling back to cached/device heuristics"sv;
          }
        }

        if (!applied_sync_ai) {
          if (auto history_opt = ai_optimizer::get_history_safe_fallback(
                launch_session->device_name, _app.name, history)) {
            BOOST_LOG(info) << "ai_optimizer: Applying history-safe fallback for \""sv
                            << launch_session->device_name << "\" + \""sv << _app.name
                            << "\" — "sv << history_opt->reasoning;
            apply_optimization_layer(resolved_optimization, optimization_locks, *history_opt, history_opt->source);
          }
          ai_optimizer::request_async(launch_session->device_name, _app.name, gpu_info, game_category, history);
          BOOST_LOG(info) << "ai_optimizer: Cache miss for known device \""sv << launch_session->device_name
                          << "\" — fired async request for next session"sv;
        }
      }
    }

    const double effective_history_safe_target_fps =
      history ? ai_optimizer::effective_history_safe_target_fps(launch_session->device_name, *history) : 0.0;
    if (history && effective_history_safe_target_fps > 0.0 &&
        !ai_optimizer::should_relax_history_safe_target_fps(*history)) {
      const int safe_fps =
        static_cast<int>(std::round(effective_history_safe_target_fps * 1000.0));
      if (safe_fps > 0 && launch_session->fps > safe_fps) {
        const parsed_display_mode_t safe_display_mode {
          static_cast<int>(launch_session->width),
          static_cast<int>(launch_session->height),
          safe_fps,
        };
        BOOST_LOG(info) << "session_optimization: history-safe pacing target lowered FPS from "sv
                        << format_session_fps(launch_session->fps) << " to "sv
                        << format_session_fps(safe_fps)
                        << " for \""sv << launch_session->device_name << "\" + \""sv << _app.name << '"';
        resolved_optimization.display_mode = safe_display_mode;
        resolved_optimization.display_mode_source = "history_safe";
        note_layer(resolved_optimization, "history_safe");
        note_reasoning(
          resolved_optimization,
          "Recent Nova pacing feedback lowered the next launch FPS target."
        );
        append_normalization_reason(
          resolved_optimization,
          "History-safe pacing overrode the paired display-mode FPS ceiling."
        );
      }
    } else if (history && effective_history_safe_target_fps > 0.0) {
      BOOST_LOG(info) << "session_optimization: relaxing history-safe "
                      << static_cast<int>(std::round(effective_history_safe_target_fps))
                      << " FPS cap for \""sv << launch_session->device_name << "\" + \""sv
                      << _app.name << "\" after a completed safe-target trial";
    }

    if (!optimization_locks.display_mode && resolved_optimization.display_mode.has_value()) {
      const parsed_display_mode_t requested_display_mode {
        static_cast<int>(launch_session->requested_width),
        static_cast<int>(launch_session->requested_height),
        launch_session->requested_fps,
      };
      const auto normalized_display_mode =
        clamp_optimized_display_mode_to_client_request(
          *resolved_optimization.display_mode,
          requested_display_mode,
          resolved_optimization.display_mode_source
        );

      if (!display_modes_equal(normalized_display_mode, *resolved_optimization.display_mode)) {
        BOOST_LOG(info) << "session_optimization: normalized display_mode from "sv
                        << format_display_mode(*resolved_optimization.display_mode)
                        << " to "sv << format_display_mode(normalized_display_mode)
                        << " to respect explicit client request "sv << format_display_mode(requested_display_mode);
        resolved_optimization.display_mode = normalized_display_mode;
        resolved_optimization.display_mode_source = "runtime_policy";
        note_layer(resolved_optimization, "runtime_policy");
        note_reasoning(
          resolved_optimization,
          "Explicit client display mode bounds cached optimization so launch resolution stays within a sane client range."
        );
        append_normalization_reason(
          resolved_optimization,
          "Runtime policy bounded optimized display mode to the explicit client request."
        );
      }
    }

    if (resolved_optimization.display_mode) {
      launch_session->width = resolved_optimization.display_mode->width;
      launch_session->height = resolved_optimization.display_mode->height;
      launch_session->fps = resolved_optimization.display_mode->fps;
    }

    if (resolved_optimization.color_range) {
      config::video.color_range = *resolved_optimization.color_range;
    }

    if (resolved_optimization.hdr.has_value()) {
      launch_session->enable_hdr = *resolved_optimization.hdr;
    }

    if (resolved_optimization.virtual_display.has_value()) {
      launch_session->virtual_display = *resolved_optimization.virtual_display;
    }

    if (resolved_optimization.target_bitrate_kbps.has_value()) {
      launch_session->target_bitrate_kbps = *resolved_optimization.target_bitrate_kbps;
      config::video.max_bitrate = *resolved_optimization.target_bitrate_kbps;
      if (config::video.adaptive_bitrate.enabled) {
        config::video.adaptive_bitrate.max_bitrate_kbps = *resolved_optimization.target_bitrate_kbps;
      }
    }

    if (resolved_optimization.nvenc_tune.has_value()) {
      launch_session->nvenc_tune = *resolved_optimization.nvenc_tune;
      config::video.nvenc_tune = *resolved_optimization.nvenc_tune;
    }

    auto normalized_preferred_codec = device_db::normalize_preferred_codec(
      launch_session->device_name,
      _app.name,
      resolved_optimization.preferred_codec,
      resolved_optimization.target_bitrate_kbps,
      resolved_optimization.hdr.value_or(launch_session->enable_hdr)
    );
    if (normalized_preferred_codec != resolved_optimization.preferred_codec) {
      BOOST_LOG(info) << "session_optimization: normalized preferred_codec from "sv
                      << resolved_optimization.preferred_codec.value_or("(none)"s)
                      << " to "sv
                      << normalized_preferred_codec.value_or("(client)"s)
                      << " for headless handheld UI streaming on the RAM capture path"sv;
      resolved_optimization.preferred_codec = normalized_preferred_codec;
      resolved_optimization.preferred_codec_source = "runtime_policy";
      note_layer(resolved_optimization, "runtime_policy");
      note_reasoning(
        resolved_optimization,
        "Headless labwc handheld UI sessions at modest bitrates prefer H.264 over forced HEVC"
      );
      append_normalization_reason(
        resolved_optimization,
        "Runtime policy adjusted codec selection for headless handheld UI streaming."
      );
    }

    if (resolved_optimization.preferred_codec.has_value()) {
      launch_session->preferred_codec = *resolved_optimization.preferred_codec;
    }

#ifdef __linux__
    const bool using_headless_cage_runtime =
      requested_headless_for_session &&
      use_cage_compositor_for_session;
    if (using_headless_cage_runtime && launch_session->virtual_display) {
      BOOST_LOG(info) << "session_optimization: normalized virtual_display from true to false for headless cage runtime"sv;
      launch_session->virtual_display = false;
      resolved_optimization.virtual_display = false;
      resolved_optimization.virtual_display_source = "runtime_policy";
      note_layer(resolved_optimization, "runtime_policy");
      note_reasoning(
        resolved_optimization,
        "Headless labwc already provides the effective stream output; extra virtual display disabled."
      );
      append_normalization_reason(
        resolved_optimization,
        "Headless cage runtime owns the stream output; virtual display disabled."
      );
    }
#endif

    launch_session->optimization_source = join_layers(resolved_optimization.layers);
    if (launch_session->optimization_source.empty()) {
      launch_session->optimization_source = "default";
    }
    launch_session->optimization_reasoning = join_reasons(resolved_optimization.reasoning);
    launch_session->optimization_confidence = resolved_optimization.confidence;
    launch_session->optimization_cache_status = resolved_optimization.cache_status;
    launch_session->optimization_normalization_reason = resolved_optimization.normalization_reason;
    launch_session->optimization_recommendation_version = resolved_optimization.recommendation_version;
    const auto preferred_codec = launch_session->preferred_codec.value_or("client"s);

    BOOST_LOG(info) << "session_optimization: requested="sv
                    << launch_session->requested_width << "x"sv
                    << launch_session->requested_height << "x"sv
                    << format_session_fps(launch_session->requested_fps)
                    << " selected="sv
                    << launch_session->width << "x"sv
                    << launch_session->height << "x"sv
                    << format_session_fps(launch_session->fps)
                    << " virtual_display="sv << (launch_session->virtual_display ? "true"sv : "false"sv)
                    << " color_range="sv << config::video.color_range
                    << " max_bitrate="sv << config::video.max_bitrate
                    << " nvenc_tune="sv << config::video.nvenc_tune
                    << " preferred_codec="sv << preferred_codec
                    << " layers="sv << launch_session->optimization_source;

    uint32_t client_width = launch_session->width ? launch_session->width : 1920;
    uint32_t client_height = launch_session->height ? launch_session->height : 1080;

    uint32_t render_width = client_width;
    uint32_t render_height = client_height;

    int scale_factor = launch_session->scale_factor;
    if (_app.scale_factor != 100) {
      scale_factor = _app.scale_factor;
    }

    if (scale_factor != 100) {
      render_width *= ((float)scale_factor / 100);
      render_height *= ((float)scale_factor / 100);

      // Chop the last bit to ensure the scaled resolution is even numbered
      // Most odd resolutions won't work well
      render_width &= ~1;
      render_height &= ~1;
    }

    launch_session->width = render_width;
    launch_session->height = render_height;

    // Polaris session setup: cage-as-window architecture
    // Games render inside cage (a Wayland compositor window on DP-3).
    // No display switching, no kscreen-doctor, no HDMI-A-1.
    confighttp::set_session_state(confighttp::session_state_e::initializing);
    confighttp::emit_session_event("session_starting", "Preparing streaming session");
#ifdef __linux__
    auto session_state = session_manager::save_state();

    // Inhibit lock screen during streaming
    if (session_manager::inhibit_lock()) {
      session_state.lock_inhibited = true;
    }

    // Cage start is deferred — it launches with the game command below
#endif

    // Executed when returning from function
    auto fg = util::fail_guard([&
#ifdef __linux__
      , session_state
#endif
    ]() {
      // Restore session-scoped config overrides before unwinding.
      config::video.output_name = this->initial_display;
      config::video.color_range = this->initial_color_range;
      config::video.nvenc_tune = this->initial_nvenc_tune;
      config::video.max_bitrate = this->initial_max_bitrate;
      if (this->initial_video_config_saved) {
        config::video.adaptive_bitrate.max_bitrate_kbps = this->initial_adaptive_max_bitrate;
      }
      terminate_impl(false, true);
      display_device::revert_configuration();
#ifdef __linux__
      confighttp::set_session_state(confighttp::session_state_e::tearing_down);
      confighttp::emit_session_event("session_ending", "Cleaning up");
      // terminate_impl() owns exact-generation cage cleanup. Do not follow it
      // with raw PID/group signaling from cage_display_router::stop().
      session_manager::restore_state(session_state);
#endif
    });

    if (!app.gamepad.empty() && app_gamepad_override_is_supported(app.gamepad)) {
      _saved_input_config = std::make_shared<config::input_t>(config::input);
      if (app.gamepad == "disabled") {
        config::input.controller = false;
        BOOST_LOG(info) << "process: virtual gamepad disabled for app ["sv << app.name << ']';
      } else {
        config::input.controller = true;
        config::input.gamepad = app.gamepad;
        BOOST_LOG(info) << "process: virtual gamepad override for app ["sv << app.name << "] set to ["sv << app.gamepad << ']';
      }
    } else if (!app.gamepad.empty()) {
      BOOST_LOG(warning) << "process: ignoring unsupported virtual gamepad override ["sv << app.gamepad << "] for app ["sv << app.name << ']';
    }

#ifdef _WIN32
    if (
      config::video.linux_display.headless_mode        // Headless mode
      || launch_session->virtual_display // User requested virtual display
      || (!launch_session->user_locked_virtual_display && _app.virtual_display) // App default when client didn't explicitly choose
      || !video::allow_encoder_probing() // No active display presents
    ) {
      if (vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
        // Try init driver again
        initVDisplayDriver();
      }

      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        // Try set the render adapter matching the capture adapter if user has specified one
        if (!config::video.adapter_name.empty()) {
          VDISPLAY::setRenderAdapterByName(platf::from_utf8(config::video.adapter_name));
        }

        std::string device_name;
        std::string device_uuid_str;
        uuid_util::uuid_t device_uuid;

        if (_app.use_app_identity) {
          device_name = _app.name;
          if (_app.per_client_app_identity) {
            device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
            auto app_uuid = uuid_util::uuid_t::parse(_app.uuid);

            // Use XOR to mix the two UUIDs
            device_uuid.b64[0] ^= app_uuid.b64[0];
            device_uuid.b64[1] ^= app_uuid.b64[1];

            device_uuid_str = device_uuid.string();
          } else {
            device_uuid_str = _app.uuid;
            device_uuid = uuid_util::uuid_t::parse(_app.uuid);
          }
        } else {
          device_name = launch_session->device_name;
          device_uuid_str = launch_session->unique_id;
          device_uuid = uuid_util::uuid_t::parse(launch_session->unique_id);
        }

        memcpy(&launch_session->display_guid, &device_uuid, sizeof(GUID));

        int target_fps = launch_session->fps ? launch_session->fps : 60000;

        if (target_fps < 1000) {
          target_fps *= 1000;
        }

        if (config::video.double_refreshrate) {
          target_fps *= 2;
        }

        std::wstring vdisplayName = VDISPLAY::createVirtualDisplay(
          device_uuid_str.c_str(),
          device_name.c_str(),
          render_width,
          render_height,
          target_fps,
          launch_session->display_guid
        );

        // No matter we get the display name or not, the virtual display might still be created.
        // We need to track it properly to remove the display when the session terminates.
        launch_session->virtual_display = true;

        if (!vdisplayName.empty()) {
          BOOST_LOG(info) << "Virtual Display created at " << vdisplayName;

          // Don't change display settings when no params are given
          if (launch_session->width && launch_session->height && launch_session->fps) {
            // Apply display settings
            VDISPLAY::changeDisplaySettings(vdisplayName.c_str(), render_width, render_height, target_fps);
          }

          // Check the ISOLATED DISPLAY configuration setting and rearrange the displays
          if (config::video.isolated_virtual_display_option == true) {
            // Apply the isolated display settings
            VDISPLAY::changeDisplaySettings2(vdisplayName.c_str(), render_width, render_height, target_fps, true);
          }

          // Set virtual_display to true when everything went fine
          this->virtual_display = true;
          this->display_name = platf::to_utf8(vdisplayName);

          // When using virtual display, we don't care which display user configured to use.
          // So we always set output_name to the newly created virtual display as a workaround for
          // empty name when probing graphics cards.

          config::video.output_name = display_device::map_display_name(this->display_name);
        } else {
          BOOST_LOG(warning) << "Virtual Display creation failed, or cannot get created display name in time!";
        }
      } else {
        // Driver isn't working so we don't need to track virtual display.
        launch_session->virtual_display = false;
      }
    }

    display_device::configure_display(config::video, *launch_session);

    // We should not preserve display state when using virtual display.
    // It is already handled by Windows properly.
    if (this->virtual_display) {
      display_device::reset_persistence();
    }

#elif __linux__

    // Linux virtual display creation — analogous to Windows SUDOVDA path above.
    // Resolve the display runtime once so launch, encoder probing, and UI report
    // the same effective policy.
    const auto display_policy = stream_display_policy::resolve_current(
      video::active_encoder_requires_gpu_native_capture()
    );
    const bool using_headless_cage =
      display_policy.requested_headless &&
      display_policy.use_cage_runtime;
    // headless_source=physical: the stream captures a real connector (HDMI
    // dongle) that the desktop compositor already drives — no virtual display
    // is created. Capture targets it deterministically by kernel connector
    // name via config::video.output_name (see kmsgrab named targeting).
    const bool physical_headless_source =
      config::video.linux_display.headless_source == "physical";
    const bool should_use_linux_virtual_display =
      !physical_headless_source && (
        display_policy.use_host_virtual_display ||
        launch_session->virtual_display ||
        (!launch_session->user_locked_virtual_display && _app.virtual_display));

    // EVDI-as-primary (no physical dongle needed): create the virtual display even
    // though auto-manage is on, then promote the desktop onto it. This is an
    // INDEPENDENT gate — deliberately not tied to should_use_linux_virtual_display
    // (which is false for the very no-dongle user this targets) — and it must never
    // fall into the !auto_manage_displays creation branch below.
    const bool evdi_promotion_requested =
      !display_policy.use_cage_runtime &&
      config::video.linux_display.auto_manage_displays &&
      config::video.linux_display.headless_swap_primary &&
      config::video.linux_display.headless_source == "evdi";
    // Promotion needs a physical monitor to disable (enable_streaming_display() cannot
    // swap without primary_output), and it must NOT clobber a more-specific per-app
    // output pin (which already redirected streaming_output at ~4402).
    const bool evdi_promotion =
      evdi_promotion_requested &&
      !config::video.linux_display.primary_output.empty() &&
      _app.output_name.empty();

    if (evdi_promotion_requested && !evdi_promotion) {
      if (config::video.linux_display.primary_output.empty()) {
        BOOST_LOG(warning) << "EVDI-as-primary requested but Primary Output is unset; set it to your "sv
                           << "physical monitor to enable promotion. Streaming without promotion."sv;
      } else if (!_app.output_name.empty()) {
        BOOST_LOG(info) << "EVDI-as-primary: per-app Output pin ["sv << _app.output_name
                        << "] takes precedence; not promoting the EVDI"sv;
      }
    }

    if (evdi_promotion) {
      // Neutralize the generic auto_manage swap below until we CONFIRM a live EVDI to
      // promote onto: clear streaming_output so enable_streaming_display() no-ops on any
      // abort path, saving the user's value first so teardown restores it.
      if (!this->initial_streaming_output_saved) {
        this->initial_streaming_output = config::video.linux_display.streaming_output;
        this->initial_streaming_output_saved = true;
      }
      config::video.linux_display.streaming_output.clear();

      // Fully undo a partially-created EVDI so capture falls back to the physical
      // desktop. Destroying the vdisplay unpublishes frame_grab, so
      // evdi_capture::available() becomes false and misc.cpp routes capture to the real
      // KMS/Wayland output instead of a blank, never-promoted EVDI (black stream).
      auto abort_promotion = [&](const std::string &why) {
        BOOST_LOG(warning) << "EVDI-as-primary: "sv << why
                           << "; aborting promotion, leaving displays untouched and capturing "sv
                           << "the physical desktop"sv;
        if (linux_vdisplay.has_value()) {
          virtual_display::destroy(*linux_vdisplay);
          linux_vdisplay.reset();
        }
        this->virtual_display = false;
        this->display_name.clear();
        this->evdi_promotion_active = false;
        launch_session->virtual_display = false;
        // streaming_output stays cleared (restored at teardown); output_name untouched.
      };

      if (!isLinuxVDisplayAvailable()) {
        BOOST_LOG(warning) << "EVDI-as-primary requested but no virtual display backend available; "sv
                           << "capturing the physical desktop"sv;
        launch_session->virtual_display = false;
      } else {
        int target_fps = launch_session->fps ? launch_session->fps : 60000;
        if (target_fps >= 1000) {
          target_fps /= 1000;
        }
        if (config::video.double_refreshrate) {
          target_fps *= 2;
        }

        auto vdisplay = virtual_display::create(render_width, render_height, target_fps);
        if (!vdisplay.has_value()) {
          BOOST_LOG(warning) << "EVDI-as-primary: virtual display creation failed on Linux; "sv
                             << "capturing the physical desktop"sv;
          launch_session->virtual_display = false;
        } else {
          linux_vdisplay = std::move(vdisplay);
          launch_session->virtual_display = true;
          this->virtual_display = true;
          this->display_name = linux_vdisplay->output_name;
          const std::string evdi_name = linux_vdisplay->output_name;

          BOOST_LOG(info) << "EVDI-as-primary: virtual display created ["sv << evdi_name
                          << "] ("sv << render_width << "x"sv << render_height << "@"sv
                          << target_fps << "Hz) via "sv
                          << virtual_display::backend_name(linux_vdisplay->backend);

          if (linux_vdisplay->backend != virtual_display::backend_e::EVDI) {
            // headless_source=evdi but create() fell back to a non-EVDI backend (Wayland
            // headless / kscreen-doctor). Native evdigrab does not apply — do not promote.
            abort_promotion("virtual display backend is not EVDI (EVDI unavailable?)");
          } else if (evdi_name.empty() || evdi_name == "VIRTUAL-1") {
            // find_evdi_output() could not resolve the real connector (fell back to the
            // synthetic "VIRTUAL-1"), which kscreen-doctor can never address for a swap.
            abort_promotion("could not resolve the EVDI's real connector name");
          } else if (!linux_display::wait_for_kscreen_output(evdi_name)) {
            // The swap could fail to light the EVDI yet still disable the physical
            // monitor, stranding the desktop. Presence check absorbs KWin hotplug lag.
            abort_promotion("EVDI output [" + evdi_name + "] never appeared in kscreen-doctor");
          } else {
            this->evdi_promotion_active = true;
            // Route capture to evdigrab: the misc.cpp dispatch matches
            // config::video.output_name against the RAW connector name evdi_capture
            // publishes, so set it directly (not via map_display_name, which can differ).
            config::video.output_name = evdi_name;
            // Point the auto_manage swap at the EVDI (enable_streaming_display reads
            // streaming_output). primary_output stays the physical monitor to disable.
            config::video.linux_display.streaming_output = evdi_name;
            BOOST_LOG(info) << "EVDI-as-primary: promoting desktop onto ["sv << evdi_name
                            << "]; physical monitor ["sv
                            << config::video.linux_display.primary_output
                            << "] will be disabled for the session"sv;
          }
        }
      }
    } else if (
      !display_policy.use_cage_runtime &&
      !config::video.linux_display.auto_manage_displays && (
        should_use_linux_virtual_display
      )
    ) {
      if (isLinuxVDisplayAvailable()) {
        int target_fps = launch_session->fps ? launch_session->fps : 60000;

        // Convert from milliHz to Hz if needed (Apollo uses milliHz internally)
        if (target_fps >= 1000) {
          target_fps /= 1000;
        }

        if (config::video.double_refreshrate) {
          target_fps *= 2;
        }

        auto vdisplay = virtual_display::create(render_width, render_height, target_fps);

        if (vdisplay.has_value()) {
          linux_vdisplay = std::move(vdisplay);
          launch_session->virtual_display = true;
          this->virtual_display = true;
          this->display_name = linux_vdisplay->output_name;

          BOOST_LOG(info) << "Virtual Display created: "sv << linux_vdisplay->output_name
                          << " ("sv << render_width << "x"sv << render_height
                          << "@"sv << target_fps << "Hz) via "sv
                          << virtual_display::backend_name(linux_vdisplay->backend);

          // Set output_name to the newly created virtual display so the
          // capture pipeline uses the correct output
          config::video.output_name = display_device::map_display_name(this->display_name);
        } else {
          BOOST_LOG(warning) << "Virtual Display creation failed on Linux"sv;
          launch_session->virtual_display = false;
        }
      } else {
        BOOST_LOG(warning) << "Virtual display requested but no backend available on Linux"sv;
        launch_session->virtual_display = false;
      }
    } else if (physical_headless_source && !display_policy.use_cage_runtime) {
      if (!config::video.output_name.empty()) {
        BOOST_LOG(info) << "Physical headless source: capturing connector ["sv
                        << config::video.output_name
                        << "] driven by the desktop compositor; no virtual display created"sv;
      } else {
        BOOST_LOG(warning) << "headless_source=physical but no Output Name is configured; "sv
                           << "set Output Name to the dongle's connector name (e.g. DP-3 or HDMI-A-2, "sv
                           << "see /sys/class/drm) so capture targets it deterministically"sv;
      }
    } else if (using_headless_cage) {
      BOOST_LOG(info) << "Linux virtual display: skipped because "sv
                      << display_policy.label
                      << " owns the streaming output ("sv
                      << display_policy.reason << ")"sv;
    }

    display_device::configure_display(config::video, *launch_session);

    // Reset persistence when using virtual display (same as Windows behavior)
    if (this->virtual_display) {
      display_device::reset_persistence();
    }

#else

    display_device::configure_display(config::video, *launch_session);

#endif

#ifdef __linux__
    // Enable streaming display BEFORE encoder probe so HDMI-A-1 is available
    if (config::video.linux_display.auto_manage_displays) {
      linux_display::enable_streaming_display();
    }
#endif

    // Probe encoders again before streaming to ensure our chosen
    // encoder matches the active GPU (which could have changed
    // due to hotplugging, driver crash, primary monitor change,
    // or any number of other factors).
#ifdef __linux__
    const bool delay_encoder_probe_until_cage =
      stream_display_policy::resolve(stream_display_policy::input_t {}).should_defer_encoder_probe;
#else
    constexpr bool delay_encoder_probe_until_cage = false;
#endif
    if (!delay_encoder_probe_until_cage &&
        no_active_sessions_at_launch &&
        video::probe_encoders()) {
      if (config::video.ignore_encoder_probe_failure) {
        BOOST_LOG(warning) << "Encoder probe failed, but continuing due to user configuration.";
      } else {
        return 503;
      }
    }

    std::string fps_str;
    char fps_buf[8];
    snprintf(fps_buf, sizeof(fps_buf), "%.3f", (float)launch_session->fps / 1000.0f);
    fps_str = fps_buf;

    // Add Stream-specific environment variables
    // Polaris Compatibility (legacy Sunshine env vars)
    _env["POLARIS_APP_ID"] = _app.id;
    _env["POLARIS_APP_NAME"] = _app.name;
    _env["POLARIS_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["POLARIS_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["POLARIS_CLIENT_FPS"] = config::sunshine.envvar_compatibility_mode ? std::to_string(std::round((float)launch_session->fps / 1000.0f)) : fps_str;
    _env["POLARIS_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["POLARIS_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["POLARIS_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["POLARIS_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    _env["POLARIS_APP_ID"] = _app.id;
    _env["POLARIS_APP_NAME"] = _app.name;
    _env["POLARIS_APP_UUID"] = _app.uuid;
    _env["POLARIS_APP_STATUS"] = "STARTING";
    _env["POLARIS_CLIENT_UUID"] = launch_session->unique_id;
    _env["POLARIS_CLIENT_NAME"] = launch_session->device_name;
    _env["POLARIS_CLIENT_WIDTH"] = std::to_string(render_width);
    _env["POLARIS_CLIENT_HEIGHT"] = std::to_string(render_height);
    _env["POLARIS_CLIENT_RENDER_WIDTH"] = std::to_string(launch_session->width);
    _env["POLARIS_CLIENT_RENDER_HEIGHT"] = std::to_string(launch_session->height);
    _env["POLARIS_CLIENT_SCALE_FACTOR"] = std::to_string(scale_factor);
    _env["POLARIS_CLIENT_FPS"] = fps_str;
    _env["POLARIS_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
    _env["POLARIS_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
    _env["POLARIS_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
    _env["POLARIS_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";

    int channelCount = launch_session->surround_info & 65535;
    switch (channelCount) {
      case 2:
        _env["POLARIS_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        _env["POLARIS_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
        break;
      case 6:
        _env["POLARIS_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        _env["POLARIS_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
        break;
      case 8:
        _env["POLARIS_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        _env["POLARIS_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
        break;
    }
    _env["POLARIS_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;
    _env["POLARIS_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;

#ifdef __linux__
    // Display-hint environment variables for game engines and runtimes.
    // These encourage games to fullscreen on the streaming display instead of the primary.
    if (!config::video.linux_display.streaming_output.empty()) {
      // Determine the streaming display's xrandr index by querying connected outputs.
      // After kscreen-doctor enables HDMI-A-1, xrandr lists it alongside DP-3.
      int display_index = 0;
      {
        FILE *pipe = popen("xrandr --query 2>/dev/null | grep ' connected' | awk '{print $1}'", "r");
        if (pipe) {
          char buf[128];
          int idx = 0;
          while (fgets(buf, sizeof(buf), pipe)) {
            std::string name(buf);
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
              name.pop_back();
            if (name == config::video.linux_display.streaming_output) {
              display_index = idx;
              break;
            }
            idx++;
          }
          pclose(pipe);
        }
      }

      // SDL: which display head to fullscreen on (0-indexed)
      _env["SDL_VIDEO_FULLSCREEN_HEAD"] = std::to_string(display_index);

      // Prevent SDL games from minimizing when focus moves to the main display
      _env["SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS"] = "0";

      BOOST_LOG(info) << "Display hints: SDL_VIDEO_FULLSCREEN_HEAD="sv << display_index
                      << ", SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0"sv;
    }
#endif

    if (!_app.output.empty() && _app.output != "null"sv) {
#ifdef _WIN32
      // fopen() interprets the filename as an ANSI string on Windows, so we must convert it
      // to UTF-16 and use the wchar_t variants for proper Unicode log file path support.
      auto woutput = platf::from_utf8(_app.output);

      // Use _SH_DENYNO to allow us to open this log file again for writing even if it is
      // still open from a previous execution. This is required to handle the case of a
      // detached process executing again while the previous process is still running.
      _pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      _pipe.reset(fopen(_app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    _app_prep_begin = std::begin(_app.prep_cmds);
    _app_prep_it = _app_prep_begin;

    for (; _app_prep_it != std::end(_app.prep_cmds); ++_app_prep_it) {
      auto &cmd = *_app_prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        confighttp::emit_session_event("warning", "Prep command failed: " + cmd.do_cmd);
        // Non-fatal: continue session even if prep-cmd fails
        continue;
      }

      child.wait();
      auto ret = child.exit_code();
      if (ret != 0) {
        BOOST_LOG(warning) << '[' << cmd.do_cmd << "] returned code ["sv << ret << ']';
        confighttp::emit_session_event("warning", "Prep command returned " + std::to_string(ret));
        // Non-fatal: continue session
      }
    }

    _env["POLARIS_APP_STATUS"] = "RUNNING";

    // Apply per-app environment variables (e.g., MANGOHUD=1, PROTON_NO_FSYNC=1)
    // Set on both boost _env (for non-cage launches) and real environ (for fork/exec in cage)
    for (const auto &[key, val] : _app.env_vars) {
      _env[key] = val;
      platf::set_env(key, val);
      BOOST_LOG(info) << "Per-app env: " << key << "=" << val;
    }
    _session_env_keys.clear();

#ifdef __linux__
    set_child_only_session_env_var(
      _env,
      _session_env_keys,
      "POLARIS_SESSION_INSTANCE_ID",
      _session_instance_id
    );
    _audio_context = {};
    if (config::audio.stream) {
      _audio_context = audio::get_audio_ctx_ref();
      if (_audio_context) {
        const auto session_audio_channels = normalized_audio_channel_count(channelCount);
        const auto sink = audio::select_sink_name(*_audio_context.get(), session_audio_channels, launch_session->host_audio);
        if (audio::should_route_session_sink_without_default(*_audio_context.get(), sink, launch_session->host_audio)) {
          set_session_env_var(_env, _session_env_keys, "PULSE_SINK", sink);
          set_session_env_var(_env, _session_env_keys, "POLARIS_SESSION_AUDIO_SINK", sink);
          BOOST_LOG(info) << "Linux audio isolation: routing launched apps to virtual sink ["sv
                          << sink << "] without changing the user's default sink"sv;
        }
      } else {
        BOOST_LOG(warning) << "Linux audio isolation: audio control unavailable; launched apps will use the current default sink"sv;
      }
    }
#endif

    // If MangoHud is enabled, also set DLSYM mode for safer Vulkan hooking
    // (prevents crashes in Wine utilities like d3ddriverquery64.exe)
    if (_app.env_vars.count("MANGOHUD") && _app.env_vars.at("MANGOHUD") == "1") {
      _env["MANGOHUD_DLSYM"] = "1";
      platf::set_env("MANGOHUD_DLSYM", "1");
      _session_env_keys.insert("MANGOHUD_DLSYM");
    }
    set_session_env_var(_env, _session_env_keys, "POLARIS_SESSION_REQUESTED_FPS", format_session_fps(launch_session->requested_fps));
    set_session_env_var(_env, _session_env_keys, "POLARIS_SESSION_TARGET_FPS", format_session_fps(launch_session->fps));
    set_session_env_var(_env, _session_env_keys, "POLARIS_SESSION_OPTIMIZATION_SOURCE", launch_session->optimization_source);

    const bool enable_session_pacing = session_pacing_is_enabled(_app, *launch_session, game_category);
    const int pacing_target_fps = launch_session->fps >= 1000 ?
      static_cast<int>(std::round(static_cast<double>(launch_session->fps) / 1000.0)) :
      launch_session->fps;
#ifdef __linux__
    const bool allow_cage_mangohud = cage_mangohud_allowed_for_session(
      _app,
      config::video.linux_display.use_cage_compositor,
      config::video.linux_display.headless_mode
    );
#else
    constexpr bool allow_cage_mangohud = true;
#endif
    launch_session->pacing_policy = enable_session_pacing ? "client_fps_limit" : "none";
    set_session_env_var(_env, _session_env_keys, "POLARIS_SESSION_PACING_POLICY", launch_session->pacing_policy);

    if (launch_session->target_bitrate_kbps) {
      set_session_env_var(_env, _session_env_keys, "POLARIS_SESSION_TARGET_BITRATE_KBPS",
                          std::to_string(*launch_session->target_bitrate_kbps));
    }

    if (enable_session_pacing) {
      if (env_value(_app, _env, "DXVK_FRAME_RATE").empty()) {
        set_session_env_var(_env, _session_env_keys, "DXVK_FRAME_RATE", std::to_string(pacing_target_fps));
      }

      const int requested_fps = launch_session->requested_fps >= 1000 ?
        static_cast<int>(std::round(static_cast<double>(launch_session->requested_fps) / 1000.0)) :
        launch_session->requested_fps;
      bool auto_mangohud_cap = false;
      if (allow_cage_mangohud &&
          pacing_target_fps > 0 &&
          (requested_fps <= 0 || pacing_target_fps + 1 < requested_fps) &&
          env_value(_app, _env, "MANGOHUD").empty()) {
        set_session_env_var(_env, _session_env_keys, "MANGOHUD", "1");
        set_session_env_var(_env, _session_env_keys, "MANGOHUD_DLSYM", "1");
        auto_mangohud_cap = true;
      }

      if (allow_cage_mangohud && env_flag_enabled(_app, _env, "MANGOHUD")) {
        if (env_value(_app, _env, "MANGOHUD_DLSYM").empty()) {
          set_session_env_var(_env, _session_env_keys, "MANGOHUD_DLSYM", "1");
        }

        auto mangohud_config = env_value(_app, _env, "MANGOHUD_CONFIG");
        if (mangohud_config.find("fps_limit=") == std::string::npos) {
          if (!mangohud_config.empty() && mangohud_config.back() != ',') {
            mangohud_config += ',';
          }
          mangohud_config += "fps_limit=" + std::to_string(pacing_target_fps);
        }
        if (auto_mangohud_cap && mangohud_config.find("no_display") == std::string::npos) {
          if (!mangohud_config.empty() && mangohud_config.back() != ',') {
            mangohud_config += ',';
          }
          mangohud_config += "no_display";
        }
        if (!mangohud_config.empty()) {
          set_session_env_var(_env, _session_env_keys, "MANGOHUD_CONFIG", mangohud_config);
        }
        if (auto_mangohud_cap) {
          BOOST_LOG(info) << "session_pacing: auto-enabled hidden MangoHud FPS cap target_fps="sv
                          << pacing_target_fps;
        }
      }

      BOOST_LOG(info) << "session_pacing: policy="sv << launch_session->pacing_policy
                      << " target_fps="sv << pacing_target_fps;
    }

#ifdef __linux__
    bool cage_started_with_detached_client = false;

    auto reprobe_encoders_for_cage = [&](bool strict_configured_encoder = false, bool save_successful_cache = true) -> bool {
      if (!config::video.linux_display.use_cage_compositor || !no_active_sessions_at_launch) {
        return true;
      }

      const auto cage_socket = cage_display_router::get_wayland_socket();
      if (cage_socket.empty()) {
        BOOST_LOG(error) << "session_manager: Cage compositor started without an active WAYLAND_DISPLAY socket for encoder reprobe"sv;
        if (config::video.ignore_encoder_probe_failure) {
          BOOST_LOG(warning) << "Encoder probe failed, but continuing due to user configuration."sv;
          return true;
        }
        return false;
      }

      BOOST_LOG(info) << "session_manager: Reprobing encoders against cage socket ["sv << cage_socket << ']';
      const auto original_wayland_display = copy_env_var("WAYLAND_DISPLAY");
      const auto original_at_spi_bus_address = copy_env_var("AT_SPI_BUS_ADDRESS");

      platf::set_env("WAYLAND_DISPLAY", cage_socket);
      platf::set_env("AT_SPI_BUS_ADDRESS", "");

      const int probe_status = video::probe_encoders(strict_configured_encoder, save_successful_cache);

      restore_env_var("AT_SPI_BUS_ADDRESS", original_at_spi_bus_address);
      restore_env_var("WAYLAND_DISPLAY", original_wayland_display);

      if (probe_status == 0) {
        BOOST_LOG(info) << "session_manager: Cage reprobe selected encoder ["
                        << video::active_encoder_name() << ']';
        return true;
      }

      if (config::video.ignore_encoder_probe_failure) {
        BOOST_LOG(warning) << "Encoder probe failed, but continuing due to user configuration."sv;
        return true;
      }

      return false;
    };

    const bool requested_headless = requested_headless_for_session;
    const bool prefer_gpu_native_capture = config::video.linux_display.prefer_gpu_native_capture;
    const bool encoder_requires_gpu_native_capture = video::active_encoder_requires_gpu_native_capture();
    const bool should_try_gpu_native_cage_probe =
      prefer_gpu_native_capture || encoder_requires_gpu_native_capture;
    const bool should_probe_windowed_cage_for_gpu_native =
      use_cage_compositor_for_session &&
      cage_display_router::should_attempt_windowed_gpu_native_probe(
        requested_headless,
        prefer_gpu_native_capture,
        should_try_gpu_native_cage_probe
      );
    stream_stats::reset_gpu_native_probe(should_probe_windowed_cage_for_gpu_native, no_active_sessions_at_launch);
    const auto cached_windowed_gpu_native_probe_result =
      should_probe_windowed_cage_for_gpu_native ?
        cage_display_router::cached_windowed_gpu_native_probe_result() :
        std::optional<bool> {};
    const auto cached_headless_extcopy_dmabuf_probe_result =
      should_probe_windowed_cage_for_gpu_native ?
        cage_display_router::cached_headless_extcopy_dmabuf_probe_result() :
        std::optional<bool> {};
    bool force_windowed_cage_for_gpu_native = false;
    bool headless_extcopy_dmabuf_selected = false;
    const int cage_refresh_hz = std::max(pacing_target_fps, 1);

    const int gpu_native_probe_fps = std::max(pacing_target_fps, 1);
    const video::config_t gpu_native_probe_config {
      static_cast<int>(render_width),
      static_cast<int>(render_height),
      gpu_native_probe_fps,
      1000,
      1,
      0,
      1,
      0,
      0,
      0,
      0,
      gpu_native_probe_fps,
      false,
    };

    auto log_runtime_state = []() {
      auto runtime_state = cage_display_router::runtime_state();
      stream_stats::update_runtime_state(runtime_state);
      BOOST_LOG(info) << "session_runtime: backend="sv << runtime_state.backend_name
                      << " requested_headless="sv << runtime_state.requested_headless
                      << " effective_headless="sv << runtime_state.effective_headless
                      << " gpu_native_override_active="sv << runtime_state.gpu_native_override_active;
    };

    auto start_cage_session = [&](const std::string &startup_cmd, bool force_windowed, bool strict_configured_encoder = false, bool save_successful_cache = true) -> bool {
      if (cage_display_router::is_running()) {
        cage_display_router::stop();
      }

      if (!cage_display_router::start(
            render_width,
            render_height,
            cage_refresh_hz,
            startup_cmd,
            force_windowed,
            allow_cage_mangohud,
            _session_instance_id
          )) {
        return false;
      }

      if (!reprobe_encoders_for_cage(strict_configured_encoder, save_successful_cache)) {
        cage_display_router::stop();
        return false;
      }

      log_runtime_state();
      const auto runtime_state = cage_display_router::runtime_state();
      if (!runtime_state.effective_headless) {
        // KDE edit mode only matters when labwc is a visible nested window on the desktop.
        session_manager::start_edit_mode_watchdog();
      }
      return true;
    };

    auto probe_headless_extcopy_dmabuf_cage = [&]() -> bool {
      if (!should_probe_windowed_cage_for_gpu_native || !no_active_sessions_at_launch) {
        return false;
      }

      auto encoder_name = video::active_encoder_name();
      auto encoder_label = encoder_name.empty() ? "unknown" : encoder_name;
      stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "running");
      BOOST_LOG(info) << "session_manager: Probing headless_extcopy_dmabuf with encoder ["
                      << encoder_label << ']';

      auto stop_probe_cage = util::fail_guard([&]() {
        if (cage_display_router::is_running()) {
          cage_display_router::stop();
        }
      });

      if (!start_cage_session("", false, true, false)) {
        cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
        stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "failed", "compositor_encoder_init", "compositor_or_encoder_init_failed");
        BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe could not initialize the compositor/encoder path"sv;
        return false;
      }

      const auto runtime_state = cage_display_router::runtime_state();
      if (!runtime_state.effective_headless) {
        cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
        stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "failed", "runtime_validation", "not_effective_headless");
        BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe did not start an effective headless runtime"sv;
        return false;
      }

      if (!video::active_encoder_runtime_supports_config(gpu_native_probe_config)) {
        cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
        stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "ineligible", "encoder_validation", "encoder_config_not_supported");
        BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe did not validate the active encoder configuration"sv;
        return false;
      }

      const bool extcopy_initialized =
        cage_display_router::cached_headless_extcopy_dmabuf_probe_result() == std::optional<bool> {true};
      const bool live_gpu_frame_converted =
        extcopy_initialized && video::active_encoder_runtime_supports_live_gpu_capture(gpu_native_probe_config);

      if (!cage_display_router::headless_extcopy_dmabuf_probe_succeeded(extcopy_initialized, live_gpu_frame_converted)) {
        cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
        if (!extcopy_initialized) {
          const auto gpu_profile = stream_stats::linux_gpu_profile_json(stream_stats::get_current());
          if (encoder_label.find("vaapi") != std::string::npos) {
            stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "ineligible", "policy", "vaapi_headless_dmabuf_disabled_for_stability");
          } else if (gpu_profile.value("adapter_pairing_status", std::string {}) == "mismatched") {
            stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "failed", "device_pairing", "cross_gpu_adapter_mismatch");
          } else {
            stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "failed", "capture_init", "dmabuf_capture_not_initialized");
          }
          BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe did not initialize DMA-BUF capture"sv;
        } else {
          stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "failed", "frame_conversion", "live_gpu_frame_conversion_failed");
          BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe did not validate live GPU frame conversion"sv;
        }
        return false;
      }

      cage_display_router::update_headless_extcopy_dmabuf_probe_result(true);
      stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "succeeded");
      BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe succeeded with encoder ["
                      << video::active_encoder_name() << ']';
      return true;
    };

    auto probe_windowed_gpu_native_cage = [&]() -> bool {
      if (!should_probe_windowed_cage_for_gpu_native || !no_active_sessions_at_launch) {
        return false;
      }

      auto encoder_name = video::active_encoder_name();
      auto encoder_label = encoder_name.empty() ? "unknown" : encoder_name;
      stream_stats::update_gpu_native_probe_attempt("windowed", "running");
      BOOST_LOG(info) << "session_manager: Probing windowed labwc for GPU-native capture with encoder ["
                      << encoder_label << ']';

      auto stop_probe_cage = util::fail_guard([&]() {
        if (cage_display_router::is_running()) {
          cage_display_router::stop();
        }
      });

      if (!start_cage_session("", true, true, false)) {
        cage_display_router::update_windowed_gpu_native_probe_result(false);
        stream_stats::update_gpu_native_probe_attempt("windowed", "failed", "compositor_encoder_init", "compositor_or_encoder_init_failed");
        BOOST_LOG(info) << "session_manager: Windowed GPU-native cage probe could not initialize the compositor/encoder path; staying headless"sv;
        return false;
      }

      if (!video::active_encoder_runtime_supports_live_gpu_capture(gpu_native_probe_config)) {
        cage_display_router::update_windowed_gpu_native_probe_result(false);
        stream_stats::update_gpu_native_probe_attempt("windowed", "failed", "first_frame", "no_live_dmabuf_frame");
        BOOST_LOG(info) << "session_manager: Windowed GPU-native cage probe did not deliver a live DMA-BUF frame; staying headless"sv;
        return false;
      }

      cage_display_router::update_windowed_gpu_native_probe_result(true);
      stream_stats::update_gpu_native_probe_attempt("windowed", "succeeded");
      BOOST_LOG(info) << "session_manager: Windowed GPU-native cage probe succeeded with encoder ["
                      << video::active_encoder_name() << ']';
      return true;
    };

    auto resolve_windowed_gpu_native_fallback = [&]() -> bool {
      if (cached_windowed_gpu_native_probe_result == std::optional<bool> {true}) {
        stream_stats::update_gpu_native_probe_attempt("windowed", "succeeded", {}, {}, true);
        BOOST_LOG(info) << "session_manager: Reusing cached windowed_dmabuf_override probe result"sv;
        return true;
      }

      if (cached_windowed_gpu_native_probe_result == std::optional<bool> {false}) {
        stream_stats::update_gpu_native_probe_attempt("windowed", "ineligible", "cache", "cached_unsupported", true);
        BOOST_LOG(info) << "session_manager: Cached windowed_dmabuf_override probe result indicates nested DMA-BUF capture is unavailable on this runtime"sv;
        return false;
      }

      return probe_windowed_gpu_native_cage();
    };

    if (cached_headless_extcopy_dmabuf_probe_result == std::optional<bool> {true}) {
      headless_extcopy_dmabuf_selected = true;
      stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "succeeded", {}, {}, true);
      BOOST_LOG(info) << "session_manager: Reusing cached headless_extcopy_dmabuf probe result; keeping true headless runtime"sv;
    } else if (cached_headless_extcopy_dmabuf_probe_result == std::optional<bool> {false}) {
      stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "ineligible", "cache", "cached_unsupported", true);
      BOOST_LOG(info) << "session_manager: Cached headless_extcopy_dmabuf probe result indicates headless DMA-BUF capture is unavailable; evaluating windowed fallback"sv;
      force_windowed_cage_for_gpu_native = resolve_windowed_gpu_native_fallback();
    } else if (probe_headless_extcopy_dmabuf_cage()) {
      headless_extcopy_dmabuf_selected = true;
      BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf selected; keeping true headless runtime"sv;
    } else if (should_probe_windowed_cage_for_gpu_native) {
      BOOST_LOG(info) << "session_manager: headless_shm_fallback would be used for true headless; evaluating windowed GPU-native fallback"sv;
      force_windowed_cage_for_gpu_native = resolve_windowed_gpu_native_fallback();
    }

    if (headless_extcopy_dmabuf_selected) {
      stream_stats::update_gpu_native_probe_selection("headless_extcopy_dmabuf");
    } else if (force_windowed_cage_for_gpu_native) {
      stream_stats::update_gpu_native_probe_selection("windowed_dmabuf_override");
    } else if (should_probe_windowed_cage_for_gpu_native) {
      stream_stats::update_gpu_native_probe_selection("headless_shm", "headless_shm");
    }

    auto encoder_name = video::active_encoder_name();
    auto encoder_label = encoder_name.empty() ? "unknown" : encoder_name;
    if (force_windowed_cage_for_gpu_native) {
      BOOST_LOG(warning) << "session_manager: windowed_dmabuf_override selected because headless_extcopy_dmabuf was unavailable and encoder ["
                         << encoder_label
                         << "] can use a GPU-native capture path"sv;
    }

    const bool has_launch_commands = !_app.detached.empty() || !_app.cmd.empty();
    platf::gamepad::isolation::strict_gamepad_isolation_plan_t gamepad_isolation_plan;
    bool gamepad_isolation_prepared = false;

    auto should_apply_headless_gamepad_isolation = [&]() {
      return has_launch_commands &&
             use_cage_compositor_for_session &&
             requested_headless_for_session &&
             !force_windowed_cage_for_gpu_native;
    };

    auto prepare_headless_gamepad_isolation = [&]() {
      if (gamepad_isolation_prepared || !should_apply_headless_gamepad_isolation()) {
        return;
      }
      gamepad_isolation_prepared = true;
      gamepad_isolation_plan = platf::gamepad::isolation::prepare_headless_labwc_launch();
      auto stats = stream_stats::get_current();
      std::string isolation_mode = "unknown";
      switch (gamepad_isolation_plan.mode) {
        case platf::gamepad::isolation::isolation_mode_e::disabled:
          isolation_mode = "disabled";
          break;
        case platf::gamepad::isolation::isolation_mode_e::strict_bwrap:
          isolation_mode = "strict_bwrap";
          break;
        case platf::gamepad::isolation::isolation_mode_e::best_effort_sdl:
          isolation_mode = "best_effort_sdl";
          break;
        case platf::gamepad::isolation::isolation_mode_e::unavailable:
          isolation_mode = "unavailable";
          break;
      }
      stream_stats::update_controller_input_state(
        stats.input_virtual_controller_created,
        stats.input_virtual_controller_number,
        stats.input_virtual_controller_kind,
        stats.input_virtual_controller_error,
        isolation_mode,
        gamepad_isolation_plan.reason,
        stats.input_haptics_supported,
        stats.input_haptics_detail
      );
    };

    auto apply_gamepad_sdl_env = [&](auto &env) {
      prepare_headless_gamepad_isolation();
      if (!gamepad_isolation_plan.fallback_applied()) {
        return;
      }
      for (const auto &[key, value] : gamepad_isolation_plan.fallback_sdl.env) {
        env[key] = value;
      }
    };

    auto command_with_gamepad_isolation = [&](const std::string &command) {
      prepare_headless_gamepad_isolation();
      return platf::gamepad::isolation::command_with_headless_gamepad_isolation(command, gamepad_isolation_plan);
    };
    auto start_cage_with_runtime_fallback = [&](const std::string &startup_cmd) -> bool {
      confighttp::set_session_state(confighttp::session_state_e::cage_starting);
      confighttp::emit_session_event("cage_starting", "Starting compositor");

      if (force_windowed_cage_for_gpu_native && start_cage_session(startup_cmd, true)) {
        return true;
      }

      if (force_windowed_cage_for_gpu_native) {
        cage_display_router::update_windowed_gpu_native_probe_result(false);
        stream_stats::update_gpu_native_probe_attempt("windowed", "failed", "session_start", "windowed_override_start_failed");
        stream_stats::update_gpu_native_probe_selection("headless_shm", "headless_shm");
        BOOST_LOG(warning) << "session_manager: windowed_dmabuf_override start failed; falling back to headless_shm_fallback if headless DMA-BUF remains unavailable"sv;
        force_windowed_cage_for_gpu_native = false;
      }

      return start_cage_session(command_with_gamepad_isolation(startup_cmd), false);
    };

    auto stop_guard_on_failed_launch = util::fail_guard([this]() {
      stop_steam_big_picture_input_guard();
    });
    start_steam_big_picture_input_guard(_env, steam_guard_snapshot);

    if (has_launch_commands) {
      input::preallocate_gamepad();
    }

    // Start cage with the first detached command as its primary client.
    // Cage is a kiosk compositor — it only displays its main process fullscreen.
    if (use_cage_compositor_for_session && !_app.detached.empty()) {
      std::string game_cmd = cage_runtime_command(_app.detached[0]);
      if (!start_cage_with_runtime_fallback(game_cmd)) {
        BOOST_LOG(error) << "session_manager: Failed to start cage with game"sv;
        confighttp::emit_session_event("error", "Failed to start cage compositor");
        return 503;
      } else {
        _session_used_cage_compositor = true;
        cage_started_with_detached_client = true;
        confighttp::set_session_state(confighttp::session_state_e::game_launching);
        confighttp::emit_session_event("game_launching", "Launching " + _app.name);
      }
      // Launch remaining detached commands (if any) with cage's WAYLAND_DISPLAY
      for (size_t i = 1; i < _app.detached.size(); ++i) {
        auto cmd = cage_runtime_command(_app.detached[i]);
        auto cmd_env = _env;
        if (!allow_cage_mangohud) {
          strip_mangohud_env(cmd_env);
        }
        apply_gamepad_sdl_env(cmd_env);
        auto cage_socket = cage_display_router::get_wayland_socket();
        if (!cage_socket.empty()) {
          cmd_env["WAYLAND_DISPLAY"] = cage_socket;
          cmd_env["AT_SPI_BUS_ADDRESS"] = "";
          auto cage_display = cage_display_router::get_x11_display();
          if (!cage_display.empty()) {
            cmd_env["DISPLAY"] = cage_display;
          } else {
            cmd_env.erase("DISPLAY");
          }
        }
        const auto launch_cmd = command_with_gamepad_isolation(cmd);
        boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                                find_working_directory(cmd, cmd_env) :
                                                boost::filesystem::path(_app.working_dir);
        BOOST_LOG(info) << "Spawning ["sv << launch_cmd << "] in ["sv << working_dir << ']';
        auto child = platf::run_command(_app.elevated, true, launch_cmd, working_dir, cmd_env, _pipe.get(), ec, &_process_group);
        if (ec) {
          BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
        } else {
          child.detach();
        }
      }
    } else if (use_cage_compositor_for_session) {
      // No detached commands — start cage empty, game will use _app.cmd
      if (!start_cage_with_runtime_fallback("")) {
        BOOST_LOG(error) << "session_manager: Failed to start cage compositor"sv;
        return 503;
      }
      _session_used_cage_compositor = true;
    } else
#endif
    {
      // Non-cage path: launch detached commands normally
      for (auto &cmd : _app.detached) {
        boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                                find_working_directory(cmd, _env) :
                                                boost::filesystem::path(_app.working_dir);
        BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
        auto child = platf::run_command(_app.elevated, true, cmd, working_dir, _env, _pipe.get(), ec, nullptr);
        if (ec) {
          BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
        } else {
          child.detach();
        }
      }
    }

#ifdef __linux__
    // Set cage environment for the app command (if cage is running and app has a cmd)
    std::string effective_cmd = _app.cmd;
    std::string working_dir_cmd = effective_cmd;
    auto launch_env = _env;
    bool app_command_uses_cage_runtime = false;
    if (use_cage_compositor_for_session && cage_display_router::is_running() && !_app.cmd.empty()) {
      effective_cmd = cage_runtime_command(_app.cmd);
      working_dir_cmd = effective_cmd;
      app_command_uses_cage_runtime = true;
      if (!allow_cage_mangohud) {
        strip_mangohud_env(launch_env);
      }
      apply_gamepad_sdl_env(launch_env);
      auto cage_socket = cage_display_router::get_wayland_socket();
      if (!cage_socket.empty()) {
        launch_env["WAYLAND_DISPLAY"] = cage_socket;
        launch_env["AT_SPI_BUS_ADDRESS"] = "";
        auto cage_display = cage_display_router::get_x11_display();
        if (!cage_display.empty()) {
          launch_env["DISPLAY"] = cage_display;
          BOOST_LOG(info) << "cage: Set WAYLAND_DISPLAY="sv << cage_socket
                          << " DISPLAY="sv << cage_display
                          << " for app command"sv;
        } else {
          launch_env.erase("DISPLAY");
          BOOST_LOG(info) << "cage: Set WAYLAND_DISPLAY="sv << cage_socket << " and cleared DISPLAY for app command"sv;
        }
      }
    }
    if (app_command_uses_cage_runtime) {
      effective_cmd = command_with_gamepad_isolation(effective_cmd);
    }
#else
    const std::string &effective_cmd = _app.cmd;
    const std::string &working_dir_cmd = effective_cmd;
    auto &launch_env = _env;
#endif

    if (_app.cmd.empty()) {
#ifdef __linux__
      if (cage_started_with_detached_client) {
        BOOST_LOG(info) << "App command is empty; continuing with cage startup client"sv;
      } else
#endif
      {
        BOOST_LOG(info) << "No commands configured, showing desktop..."sv;
      }
      placebo = true;
    } else {
      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(working_dir_cmd, launch_env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << effective_cmd << "] in ["sv << working_dir << ']';
      _process = platf::run_command(_app.elevated, true, effective_cmd, working_dir, launch_env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << effective_cmd << "]: System: "sv << ec.message();
        return -1;
      }
    }

#ifdef __linux__
    stop_guard_on_failed_launch.disable();
#endif

    _app_launch_time = std::chrono::steady_clock::now();

  #ifdef _WIN32
    auto resetHDRThread = std::thread([this, enable_hdr = launch_session->enable_hdr]{
      // Windows doesn't seem to be able to set HDR correctly when a display is just connected / changed resolution,
      // so we have tooggle HDR for the virtual display manually after a delay.
      auto retryInterval = 200ms;
      while (is_changing_settings_going_to_fail()) {
        if (retryInterval > 2s) {
          BOOST_LOG(warning) << "Restoring HDR settings failed due to retry timeout!";
          return;
        }
        std::this_thread::sleep_for(retryInterval);
        retryInterval *= 2;
      }

      retryInterval = 200ms;
      while (this->display_name.empty()) {
        if (retryInterval > 2s) {
          BOOST_LOG(warning) << "Not getting current display in time! HDR will not be toggled.";
          return;
        }
        std::this_thread::sleep_for(retryInterval);
        retryInterval *= 2;
      }

      // We should have got the actual streaming display by now
      std::string currentDisplay = this->display_name;
      auto currentDisplayW = platf::from_utf8(currentDisplay);

      initial_hdr = VDISPLAY::getDisplayHDRByName(currentDisplayW.c_str());

      if (config::video.dd.hdr_option == config::video_t::dd_t::hdr_option_e::automatic) {
        mode_changed_display = currentDisplay;

        // Try turn off HDR whatever
        // As we always have to apply the workaround by turining off HDR first
        VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), false);

        if (enable_hdr) {
          if (VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), true)) {
            BOOST_LOG(info) << "HDR enabled for display " << currentDisplay;
          } else {
            BOOST_LOG(info) << "HDR enable failed for display " << currentDisplay;
          }
        }
      } else if (initial_hdr) {
        if (VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), false) && VDISPLAY::setDisplayHDRByName(currentDisplayW.c_str(), true)) {
          BOOST_LOG(info) << "HDR toggled successfully for display " << currentDisplay;
        } else {
          BOOST_LOG(info) << "HDR toggle failed for display " << currentDisplay;
        }
      }
    });

    resetHDRThread.detach();
  #endif

    fg.disable();

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
    system_tray::update_tray_playing(_app.name);
#endif

    return 0;
  }

  int proc_t::running() {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
#ifndef _WIN32
    // On POSIX OSes, we must periodically wait for our children to avoid
    // them becoming zombies. This must be synchronized carefully with
    // calls to bp::wait() and platf::process_group_running() which both
    // invoke waitpid() under the hood.
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif

    if (placebo) {
      return _app_id;
    } else if (_app.wait_all && _process_group && platf::process_group_running((std::uintptr_t) _process_group.native_handle())) {
      // The app is still running if any process in the group is still running
      return _app_id;
    } else if (_process.running()) {
      // The app is still running only if the initial process launched is still running
      return _app_id;
    } else if (_app.auto_detach && std::chrono::steady_clock::now() - _app_launch_time < 5s) {
      BOOST_LOG(info) << "App exited with code ["sv << _process.native_exit_code() << "] within 5 seconds of launch. Treating the app as a detached command."sv;
      BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
      placebo = true;

    #if defined POLARIS_TRAY && POLARIS_TRAY >= 1
      if (_process.native_exit_code() != 0) {
        system_tray::update_tray_launch_error(proc::proc.get_last_run_app_name(), _process.native_exit_code());
      }
    #endif

      return _app_id;
    }

    // Perform cleanup actions now if needed
    if (_process) {
      terminate_impl(false, true);
    }

    return 0;
  }

  void proc_t::resume() {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    BOOST_LOG(info) << "Session resuming for app [" << _app_name << "].";
    _client_session_report_recorded = false;
    _client_session_report_recorded_at = {};
    _client_session_report_recorded_unique_id.clear();

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {

        _env["POLARIS_APP_STATUS"] = "RESUMING";

        std::error_code ec;
        auto _state_resume_it = std::begin(cmd_list);

        for (; _state_resume_it != std::end(cmd_list); ++_state_resume_it) {
          auto &cmd = *_state_resume_it;

          // Skip empty commands
          if (cmd.do_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.do_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Resume Cmd: ["sv << cmd.do_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.do_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }
  }

  void proc_t::pause() {
    auto &sync = session_lifecycle_sync();
    std::unique_lock<std::recursive_mutex> lifecycle_lock(sync.mutex);
    if (_app_id <= 0) {
      BOOST_LOG(info) << "Session already stopped, do not run pause commands.";
      return;
    }

    if (_app.terminate_on_pause) {
      const auto app_name = _app_name;
      lifecycle_lock.unlock();
      BOOST_LOG(info) << "Terminating app [" << app_name << "] when all clients are disconnected. Pause commands are skipped.";
      terminate();
      return;
    }

    BOOST_LOG(info) << "Session pausing for app [" << _app_name << "].";

    // Record session quality for AI feedback loop
    if (ai_optimizer::is_enabled() && _launch_session) {
      auto stats = stream_stats::get_current();
      if (_client_session_report_recorded) {
        BOOST_LOG(info) << "AI session feedback already recorded from the client report; skipping host-side duplicate for ["
                        << _launch_session->device_name << ":" << _app.name << "]";
      } else if (stats.streaming || stats.fps > 0) {
        ai_optimizer::session_history_t session;
        session.avg_fps = stats.fps;
        session.avg_latency_ms = stats.latency_ms;
        session.avg_bitrate_kbps = stats.bitrate_kbps;
        session.packet_loss_pct = stats.packet_loss;
        session.last_fps = stats.fps;
        session.last_target_fps = session_grading_target_fps(stats);
        session.last_latency_ms = stats.latency_ms;
        session.last_bitrate_kbps = stats.bitrate_kbps;
        session.last_packet_loss_pct = stats.packet_loss;
        session.last_codec = stats.codec;
        session.last_end_reason = "host_pause";
        session.session_count = 1;
        session.last_optimization_source = _launch_session->optimization_source;
        session.last_optimization_confidence = _launch_session->optimization_confidence;
        session.last_recommendation_version = _launch_session->optimization_recommendation_version;
        session.last_quality_grade = grade_session_quality(stats, session.last_target_fps);
        session.quality_grade = session.last_quality_grade;
        session.codec = session.last_codec;
        const auto device_profile = device_db::get_device(_launch_session->device_name);
        const bool mobile_client =
          device_profile &&
          (device_profile->type == "handheld" || device_profile->type == "phone");

        const auto classification = classify_host_pause_session(
          stats,
          session.last_target_fps,
          _launch_session->virtual_display
        );
        const bool network_risk = classification.network_risk;
        const bool pacing_risk = classification.pacing_risk;
        const bool capture_fallback = classification.capture_fallback;
        const auto &capture_path = classification.capture_path;
        const bool hdr_risk = classification.hdr_risk;
        const auto &codec_lower = classification.codec_lower;
        const bool av1_codec = classification.av1_codec;
        const bool decoder_risk = classification.decoder_risk;
        const bool virtual_display_risk = classification.virtual_display_risk;
        const bool host_render_limited = classification.host_render_limited;

        session.last_network_risk = network_risk ? "elevated" : "normal";
        session.last_decoder_risk = decoder_risk ? "elevated" : "normal";
        session.last_hdr_risk = hdr_risk ? "elevated" : "normal";
        if (_launch_session->virtual_display) {
          session.last_capture_path = capture_fallback ? "virtual_display_" + capture_path : "virtual_display";
        } else if (capture_fallback) {
          session.last_capture_path = capture_path;
        } else if (stats.runtime_effective_headless) {
          session.last_capture_path = "headless";
        } else {
          session.last_capture_path = capture_path == "gpu_native" ? "gpu_native" : "desktop";
        }
        session.last_primary_issue = classification.primary_issue;
        session.last_issues = classification.issues;
        session.last_health_grade = classification.health_grade;
        session.last_safe_bitrate_kbps =
          stats.adaptive_target_bitrate_kbps > 0 ? stats.adaptive_target_bitrate_kbps :
          stats.bitrate_kbps > 0 ? static_cast<int>(std::lround(static_cast<double>(stats.bitrate_kbps) * (session.last_health_grade == "good" ? 1.0 : 0.75))) :
          0;
        session.last_safe_codec =
          decoder_risk ? "hevc" :
          av1_codec ? "av1" :
          (codec_lower.find("hevc") != std::string::npos || codec_lower.find("h265") != std::string::npos) ? "hevc" :
          (codec_lower.find("h264") != std::string::npos || codec_lower.find("avc") != std::string::npos) ? "h264" :
          "";
        session.last_safe_display_mode =
          (_launch_session && _launch_session->mirror_desktop) ? "desktop_display" :
          (virtual_display_risk || config::video.linux_display.headless_mode) ? "headless" :
          (_launch_session->virtual_display ? "virtual_display" : "headless");
        session.last_safe_target_fps = ai_optimizer::derive_safe_target_fps(
          session.last_target_fps,
          session.last_fps,
          0.0,
          0.0,
          0.0,
          mobile_client,
          session.last_health_grade != "good",
          pacing_risk || host_render_limited
        );
        if (stats.dynamic_range > 0) {
          session.last_safe_hdr = !hdr_risk;
        }
        session.last_relaunch_recommended =
          hdr_risk ||
          decoder_risk ||
          virtual_display_risk ||
          (
            session.last_safe_target_fps > 0.0 &&
            session.last_target_fps > 0.0 &&
            session.last_safe_target_fps < session.last_target_fps
          );

        ai_optimizer::record_session(_launch_session->device_name, _app.name, session);
      }
    }

    if (!_app.state_cmds.empty()) {
      auto exec_thread = std::thread([cmd_list = _app.state_cmds, app_working_dir = _app.working_dir, _env = _env]() mutable {
        _env["POLARIS_APP_STATUS"] = "PAUSING";

        std::error_code ec;
        auto _state_pause_it = std::begin(cmd_list);

        for (; _state_pause_it != std::end(cmd_list); ++_state_pause_it) {
          auto &cmd = *_state_pause_it;

          // Skip empty commands
          if (cmd.undo_cmd.empty()) {
            continue;
          }

          boost::filesystem::path working_dir = app_working_dir.empty() ?
                                                  find_working_directory(cmd.undo_cmd, _env) :
                                                  boost::filesystem::path(app_working_dir);
          BOOST_LOG(info) << "Executing Pause Cmd: ["sv << cmd.undo_cmd << "] elevated: " << cmd.elevated;
          auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, nullptr, ec, nullptr);

          if (ec) {
            BOOST_LOG(error) << "Couldn't run ["sv << cmd.undo_cmd << "]: System: "sv << ec.message();
            break;
          }

          child.wait();

          auto ret = child.exit_code();
          if (ret != 0 && ec != std::errc::permission_denied) {
            BOOST_LOG(error) << '[' << cmd.undo_cmd << "] failed with code ["sv << ret << ']';
            break;
          }
        }
      });

      exec_thread.detach();
    }

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
    system_tray::update_tray_pausing(proc::proc.get_last_run_app_name());
#endif
  }

  void proc_t::restore_isolated_session_overrides() {
    // Per-app output pin: restore the auto-managed streaming output. Kept
    // independent of the isolated-session flags — an app can pin an output
    // without opting into an isolated session.
#ifdef __linux__
    if (initial_streaming_output_saved) {
      config::video.linux_display.streaming_output = initial_streaming_output;
      initial_streaming_output.clear();
      initial_streaming_output_saved = false;
    }
    if (!initial_linux_display_saved) {
      return;
    }
    config::video.linux_display.headless_mode = initial_headless_mode;
    config::video.linux_display.use_cage_compositor = initial_use_cage_compositor;
    config::video.linux_display.prefer_gpu_native_capture = initial_prefer_gpu_native_capture;
    config::audio.sink = initial_audio_sink;
    initial_audio_sink.clear();
    initial_linux_display_saved = false;
#endif
  }

  void proc_t::terminate(bool immediate, bool needs_refresh) {
    if (!_session_lifecycle_gate->begin_stop()) {
      return;
    }
    auto release_stop = util::fail_guard([this]() {
      _session_lifecycle_gate->finish_stop();
    });
    terminate_impl(immediate, needs_refresh);
  }

  void proc_t::terminate_from_admitted_launch() {
    if (!_session_lifecycle_gate->transition_launch_to_stop()) {
      return;
    }
    auto release_stop = util::fail_guard([this]() {
      _session_lifecycle_gate->finish_stop();
    });
    terminate_impl(false, true);
  }

#ifdef __linux__
  bool proc_t::terminate_session_owned_steam_before_cage_stop() {
    return terminate_session_owned_steam_before_cage_stop_impl(
      _app,
      _session_used_cage_compositor,
      _session_instance_id
    );
  }

  void proc_t::terminate_isolated_session_generation() {
    _exact_generation_cleanup_complete = true;
    if (!_session_used_cage_compositor) {
      return;
    }

    _exact_generation_cleanup_complete = terminate_isolated_session_processes(
      _session_instance_id,
      "after private Steam pre-cage termination"sv
    );
    if (isolated_session_cleanup_resets_router(
          _session_used_cage_compositor,
          !_session_instance_id.empty(),
          _exact_generation_cleanup_complete
        )) {
      cage_display_router::reset_after_external_stop();
    } else {
      BOOST_LOG(error) << "process: retaining immutable cage generation because exact-generation cleanup was incomplete"sv;
    }
  }

  void proc_t::finish_isolated_session_generation_cleanup() {
    if (isolated_session_cleanup_clears_state(
          _session_used_cage_compositor,
          !_session_instance_id.empty(),
          _exact_generation_cleanup_complete
        )) {
      _session_instance_id.clear();
      _session_used_cage_compositor = false;
    }
  }

#endif

  void proc_t::terminate_impl(bool immediate, bool needs_refresh) {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    std::error_code ec;
    placebo = false;

#ifdef __linux__
    stop_steam_big_picture_input_guard();
    if (!immediate) {
      terminate_session_owned_steam_before_cage_stop();
    }

    // The immutable launch generation, not mutable config, owns this cleanup.
    // Capture and terminate every exact-generation process through pidfds before
    // any legacy child/group handle can signal or be destroyed.
    terminate_isolated_session_generation();
#endif

#ifdef __linux__
    if (isolated_session_uses_legacy_group_termination(
          _session_used_cage_compositor,
          immediate
        )) {
      terminate_process_group(_process, _process_group, _app.exit_timeout);
    }

    if (isolated_session_detaches_legacy_handles(_session_used_cage_compositor)) {
      // Boost.Process child/group destructors signal attached processes. The
      // exact pidfd path above is the only signaling authority for cage-owned
      // sessions, so detach legacy bookkeeping before resetting its handles.
      if (_process.joinable()) {
        _process.detach();
      }
      if (_process_group.joinable()) {
        _process_group.detach();
      }
    }
#else
    if (!immediate) {
      terminate_process_group(_process, _process_group, _app.exit_timeout);
    }
#endif
    _process = boost::process::v1::child();
    _process_group = boost::process::v1::group();

    _env["POLARIS_APP_STATUS"] = "TERMINATING";

    // Clean up per-app env vars so they don't leak to the next session
    for (const auto &[key, val] : _app.env_vars) {
      platf::unset_env(key);
    }
    for (const auto &key : _session_env_keys) {
      _env.erase(key);
      platf::unset_env(key);
    }
    _session_env_keys.clear();
    _audio_context = {};

    for (; _app_prep_it != _app_prep_begin; --_app_prep_it) {
      auto &cmd = *(_app_prep_it - 1);

      if (cmd.undo_cmd.empty()) {
        continue;
      }

      if (daemon_shutdown_requested() && command_contains_steam_big_picture_close(cmd.undo_cmd)) {
        BOOST_LOG(info) << "Skipping Steam Big Picture undo command during daemon shutdown ["sv
                        << cmd.undo_cmd << ']';
        continue;
      }

#ifdef __linux__
      if (should_skip_steam_shutdown_undo_after_cage_cleanup(
            _app,
            cmd,
            _session_used_cage_compositor
          )) {
        BOOST_LOG(info) << "Skipping Steam shutdown undo after isolated cage Steam cleanup ["sv
                        << cmd.undo_cmd << ']';
        continue;
      }
#endif

      boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                              find_working_directory(cmd.undo_cmd, _env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << cmd.undo_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, _env, _pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }

      child.wait();
      auto ret = child.exit_code();

      if (ret != 0) {
        BOOST_LOG(warning) << "Return code ["sv << ret << ']';
      }
    }

#ifdef __linux__
    finish_isolated_session_generation_cleanup();

    // Disable streaming display after undo commands have run.
    // Skip if a virtual display was created (it will be destroyed below) — EXCEPT for an
    // EVDI-promotion session: it promoted the desktop onto the (active) vdisplay and MUST
    // run the un-promote here to restore the physical monitor BEFORE that vdisplay is
    // destroyed. The original !linux_vdisplay clause is preserved verbatim so the
    // Increment-1 physical-dongle restore is untouched; the evdi_promotion_active || is
    // additive.
    if (this->evdi_promotion_active || !linux_vdisplay.has_value() || !linux_vdisplay->active) {
      linux_display::disable_streaming_display();
    }
#endif

    _pipe.reset();

    bool has_run = _app_id > 0;

#ifdef _WIN32
    // Revert HDR state
    if (has_run && !mode_changed_display.empty()) {
      auto displayNameW = platf::from_utf8(mode_changed_display);
      if (VDISPLAY::setDisplayHDRByName(displayNameW.c_str(), initial_hdr)) {
        BOOST_LOG(info) << "HDR reverted for display " << mode_changed_display;
      } else {
        BOOST_LOG(info) << "HDR revert failed for display " << mode_changed_display;
      }
    }

    bool used_virtual_display = vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK && _launch_session && _launch_session->virtual_display;
    if (used_virtual_display) {
      if (VDISPLAY::removeVirtualDisplay(_launch_session->display_guid)) {
        BOOST_LOG(info) << "Virtual Display removed successfully";
      } else if (this->virtual_display) {
        BOOST_LOG(warning) << "Virtual Display remove failed";
      } else {
        BOOST_LOG(warning) << "Virtual Display remove failed, but it seems it was not created correctly either.";
      }
    }

    // Only show the Stopped notification if we actually have an app to stop
    // Since terminate() is always run when a new app has started
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
      if (used_virtual_display) {
        display_device::reset_persistence();
      } else {
        display_device::revert_configuration();
      }
#elif __linux__
    // Destroy Linux virtual display if one was created
    bool used_linux_vdisplay = linux_vdisplay.has_value() && linux_vdisplay->active;
    if (used_linux_vdisplay) {
      // For an EVDI-promotion session the un-promote above re-enabled the physical
      // monitor; give KDE a beat to bring it back online BEFORE destroying the EVDI,
      // otherwise there is a window with zero enabled CRTCs → a momentary black desktop.
      if (this->evdi_promotion_active) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
      virtual_display::destroy(*linux_vdisplay);
      linux_vdisplay.reset();
      BOOST_LOG(info) << "Linux Virtual Display removed successfully"sv;
    }

    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
      if (used_linux_vdisplay) {
        display_device::reset_persistence();
      } else {
        display_device::revert_configuration();
      }
#else
    if (proc::proc.get_last_run_app_name().length() > 0 && has_run) {
      display_device::revert_configuration();
#endif

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
      system_tray::update_tray_stopped(proc::proc.get_last_run_app_name());
#endif
    }

    // Load the configured output_name first
    // to prevent the value being write to empty when the initial terminate happens
    if (!has_run && initial_display.empty()) {
      initial_display = config::video.output_name;
    } else {
      // Restore output name to its original value
      config::video.output_name = initial_display;
    }
    if (initial_video_config_saved) {
      config::video.color_range = initial_color_range;
      config::video.nvenc_tune = initial_nvenc_tune;
      config::video.max_bitrate = initial_max_bitrate;
      config::video.adaptive_bitrate.max_bitrate_kbps = initial_adaptive_max_bitrate;
    }

    // Undo the per-app display-source overrides (isolated session / output
    // pin). Runs after disable_streaming_display() above, which must still see
    // the session's pinned output to power it off.
    restore_isolated_session_overrides();

    _app_id = -1;
    _app_name.clear();
    _app = {};
    display_name.clear();
    initial_display.clear();
    initial_color_range = 0;
    initial_nvenc_tune = 0;
    initial_max_bitrate = 0;
    initial_adaptive_max_bitrate = 0;
    initial_video_config_saved = false;
    evdi_promotion_active = false;
    mode_changed_display.clear();
    _launch_session.reset();
    virtual_display = false;
    allow_client_commands = false;

    if (_saved_input_config) {
      config::input = *_saved_input_config;
      _saved_input_config.reset();
    }

    cursor::set_visible(config::input.mouse_cursor_visible);

    publish_stream_ended_after_terminate_if_needed(has_run);

    if (needs_refresh) {
      reload_configuration_from_file(config::stream.file_apps);
    }
  }

  bool proc_t::reload_configuration_from_file(const std::string &file_name) {
    auto parsed = parse(file_name);
    if (!parsed) {
      return false;
    }
    reload_configuration(std::move(*parsed));
    return true;
  }

  void proc_t::reload_configuration(proc_t &&parsed) {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    _env = std::move(parsed._env);
    _apps = std::move(parsed._apps);
  }

#if defined(POLARIS_TESTS)
  std::pair<const void *, const void *> proc_t::session_lifecycle_identity_for_tests() const {
    return {_session_lifecycle_gate.get(), _session_lifecycle_sync.get()};
  }

  void proc_t::with_session_lifecycle_lock_for_tests(const std::function<void()> &callback) {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    callback();
  }

  bool proc_t::begin_session_stop_for_tests() {
    return _session_lifecycle_gate->begin_stop();
  }

  void proc_t::finish_session_stop_for_tests(bool committed) {
    _session_lifecycle_gate->finish_stop(committed);
  }
#endif

  std::vector<ctx_t> proc_t::get_apps() const {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return _apps;
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return _app_name;
  }

  std::string proc_t::get_running_app_uuid() {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return _app.uuid;
  }

  std::string proc_t::get_session_token() {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return _launch_session ? _launch_session->session_token : std::string {};
  }

  std::string proc_t::get_session_owner_unique_id() {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return _launch_session ? _launch_session->unique_id : std::string {};
  }

  std::string proc_t::get_session_owner_device_name() {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return _launch_session ? _launch_session->device_name : std::string {};
  }

  bool proc_t::is_session_owner(const std::string &unique_id) {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return !_launch_session || _launch_session->unique_id.empty() || _launch_session->unique_id == unique_id;
  }

  bool proc_t::session_uses_virtual_display() {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return virtual_display;
  }

  bool proc_t::session_allows_client_commands() {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return allow_client_commands;
  }

  void proc_t::mark_client_session_report_recorded(const std::string &unique_id) {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    if (!_launch_session) {
      return;
    }

    if (unique_id.empty() || _launch_session->unique_id.empty() || _launch_session->unique_id == unique_id) {
      _client_session_report_recorded = true;
      _client_session_report_recorded_at = std::chrono::steady_clock::now();
      _client_session_report_recorded_unique_id = unique_id;
    }
  }

  bool proc_t::client_session_report_recorded() const {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return _client_session_report_recorded;
  }

  bool proc_t::session_display_mode_is_explicit() const {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    return _launch_session && _launch_session->user_locked_virtual_display;
  }

  bool proc_t::current_app_has_mangohud() const {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    if (_app.uuid.empty()) {
      return false;
    }

    return env_flag_enabled(_app, _env, "MANGOHUD");
  }

  void proc_t::set_app_mangohud_configured(const std::string &uuid, bool enabled) {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    auto apply = [enabled](ctx_t &app) {
      if (enabled) {
        app.env_vars["MANGOHUD"] = "1";
      } else {
        app.env_vars.erase("MANGOHUD");
      }
    };

    for (auto &app : _apps) {
      if (app.uuid == uuid) {
        apply(app);
      }
    }

    if (_app.uuid == uuid) {
      apply(_app);
    }
  }

  void proc_t::set_app_steam_launch_mode_configured(const std::string &uuid, const std::string &mode) {
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
    const auto normalized = normalize_steam_launch_mode(mode);
    auto apply = [&normalized](ctx_t &app) {
      app.steam_launch_mode = normalized;
      normalize_steam_library_app(app);
    };

    for (auto &app : _apps) {
      if (app.uuid == uuid) {
        apply(app);
      }
    }

    if (_app.uuid == uuid) {
      _app.steam_launch_mode = normalized;
    }
  }

  proc_t::session_lifecycle_sync_t &proc_t::session_lifecycle_sync() const {
    return *_session_lifecycle_sync;
  }

  session_stop_snapshot_t proc_t::get_session_stop_snapshot_locked(
    const std::string &unique_id,
    bool can_launch,
    std::string_view expected_token,
    bool require_exact_token,
    const rtsp_stream::session_snapshot_t &rtsp_snapshot,
    bool stop_in_progress
  ) {
    session_stop_snapshot_t snapshot;
    snapshot.running_app_id = running();
    snapshot.had_running_app = snapshot.running_app_id > 0;
    snapshot.active_sessions = rtsp_snapshot.active_sessions + rtsp_snapshot.pending_sessions;
    snapshot.requester_role = rtsp_snapshot.requester_role;
    snapshot.stop_in_progress = stop_in_progress;
    snapshot.session_token = _launch_session ? _launch_session->session_token : std::string {};
    snapshot.token_available =
      !snapshot.session_token.empty() ||
      std::any_of(
        rtsp_snapshot.requester_session_tokens.begin(),
        rtsp_snapshot.requester_session_tokens.end(),
        [](const std::string &token) {
          return !token.empty();
        }
      );
    snapshot.owned_by_client =
      _launch_session &&
      !unique_id.empty() &&
      !_launch_session->unique_id.empty() &&
      _launch_session->unique_id == unique_id;
    snapshot.game = _app_name;
    const bool token_matches = session_stop_token_matches(
      require_exact_token,
      expected_token,
      snapshot.session_token,
      rtsp_snapshot.requester_session_tokens
    );
    snapshot.outcome = evaluate_session_stop_request(
      can_launch,
      snapshot.had_running_app,
      snapshot.active_sessions,
      snapshot.owned_by_client,
      snapshot.requester_role,
      snapshot.stop_in_progress,
      token_matches
    );
    return snapshot;
  }

  session_status_view_t proc_t::get_session_status_view(const std::string &unique_id, bool can_launch) {
    auto guard = session_snapshot_guard_t::try_acquire(*_session_lifecycle_gate);
    const bool stop_observed = !guard.owns_snapshot();
    const auto rtsp_snapshot = rtsp_stream::session_snapshot(unique_id);
    auto &sync = session_lifecycle_sync();
    std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);

    session_status_snapshot_t snapshot;
    snapshot.stop = get_session_stop_snapshot_locked(
      unique_id,
      can_launch,
      {},
      false,
      rtsp_snapshot,
      stop_observed
    );
    snapshot.viewer_count = rtsp_snapshot.viewer_count;
    snapshot.owner_unique_id = _launch_session ? _launch_session->unique_id : std::string {};
    snapshot.owner_device_name = _launch_session ? _launch_session->device_name : std::string {};
    snapshot.game = _app_name;
    snapshot.game_uuid = _app.uuid;
    snapshot.client_commands_enabled = allow_client_commands;
    snapshot.mangohud_configured = !_app.uuid.empty() && env_flag_enabled(_app, _env, "MANGOHUD");
    snapshot.virtual_display = virtual_display;
    snapshot.display_mode_explicit = _launch_session && _launch_session->user_locked_virtual_display;
    return {std::move(guard), std::move(snapshot)};
  }

  session_stop_snapshot_t proc_t::get_session_stop_snapshot(const std::string &unique_id, bool can_launch) {
    auto view = get_session_status_view(unique_id, can_launch);
    return std::move(view.snapshot.stop);
  }

  session_stop_result_t proc_t::request_session_shutdown(
    const std::string &unique_id,
    std::string_view expected_token,
    bool can_launch,
    bool require_exact_token
  ) {
    session_stop_result_t result;
    if (!can_launch) {
      result.snapshot.outcome = session_stop_outcome_t::permission_denied;
      return result;
    }
    if (!_session_lifecycle_gate->begin_stop()) {
      result.snapshot.outcome = session_stop_outcome_t::stop_in_progress;
      result.snapshot.stop_in_progress = true;
      return result;
    }
    bool stop_committed = false;
    auto release_stop = util::fail_guard([this, &stop_committed]() {
      _session_lifecycle_gate->finish_stop(stop_committed);
    });

    const auto rtsp_snapshot = rtsp_stream::session_snapshot(unique_id);
    std::uint64_t claimed_generation = 0;
    {
      auto &sync = session_lifecycle_sync();
      std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
      result.snapshot = get_session_stop_snapshot_locked(
        unique_id,
        can_launch,
        expected_token,
        require_exact_token,
        rtsp_snapshot,
        false
      );
      if (result.snapshot.outcome != session_stop_outcome_t::allowed) {
        return result;
      }
      claimed_generation = _session_generation;
    }

    if (result.snapshot.had_running_app || result.snapshot.active_sessions > 0) {
      confighttp::set_session_state(confighttp::session_state_e::tearing_down);
      confighttp::emit_session_event("stream_stopping", "Stopping session");
    }
    rtsp_stream::terminate_sessions();

    {
      auto &sync = session_lifecycle_sync();
      std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex);
      if (_session_generation != claimed_generation) {
        result.snapshot.outcome = session_stop_outcome_t::session_changed;
        return result;
      }
      if (result.snapshot.had_running_app && running() > 0) {
        terminate_impl(false, true);
      }
      display_device::revert_configuration();
      result.stopped = result.snapshot.had_running_app || result.snapshot.active_sessions > 0;
    }

    stop_committed = true;
    _session_lifecycle_gate->finish_stop(stop_committed);
    release_stop.disable();
    return result;
  }

  bool proc_t::session_shutdown_requested() const {
    return _session_lifecycle_gate->stop_in_progress();
  }

  boost::process::environment proc_t::get_env() {
    return _env;
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    assert(!placebo);
    assert(!_process.running());
  }

  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = std::find_if(env.cbegin(), env.cend(), [&](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return resolve_bundled_asset("box.png");
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if extension is not one we can safely serve
    if (image_extension != ".png" &&
        image_extension != ".jpg" &&
        image_extension != ".jpeg" &&
        image_extension != ".webp") {
      return resolve_bundled_asset("box.png");
    }

    if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return resolve_bundled_asset("steam.png");
    }

    // check if image is in assets directory
    if (!std::filesystem::path(app_image_path).is_absolute()) {
      const auto bundled_asset = resolve_bundled_asset(app_image_path);
      if (std::filesystem::exists(bundled_asset)) {
        return bundled_asset;
      }
    }

    // check if specified image exists
    std::error_code code;
    if (!std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      if (should_log_invalid_app_image_once(app_image_path)) {
        BOOST_LOG(info) << "Couldn't find app image at path ["sv << app_image_path << "]; using default box art"sv;
      }
      return resolve_bundled_asset("box.png");
    }

    if (!has_supported_image_signature(app_image_path, image_extension)) {
      if (should_log_invalid_app_image_once(app_image_path)) {
        BOOST_LOG(warning) << "App image at path ["sv << app_image_path << "] failed signature validation";
      }
      return resolve_bundled_asset("box.png");
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  /**
   * @brief Migrate the applications stored in the file tree by merging in a new app.
   *
   * This function updates the application entries in *fileTree_p* using the data in *inputTree_p*.
   * If an app in the file tree does not have a UUID, one is generated and inserted.
   * If an app with the same UUID as the new app is found, it is replaced.
   * Additionally, empty keys (such as "prep-cmd" or "detached") and keys no longer needed ("launching", "index")
   * are removed from the input.
   *
   * Legacy versions of Sunshine/Apollo stored boolean and integer values as strings.
   * The following keys are converted:
   *   - Boolean keys: "exclude-global-prep-cmd", "elevated", "auto-detach", "wait-all",
   *                     "use-app-identity", "per-client-app-identity", "virtual-display"
   *   - Integer keys: "exit-timeout"
   *
   * A migration version is stored in the file tree (under "version") so that future changes can be applied.
   *
   * @param fileTree_p Pointer to the JSON object representing the file tree.
   * @param inputTree_p Pointer to the JSON object representing the new app.
   */
  void migrate_apps(nlohmann::json* fileTree_p, nlohmann::json* inputTree_p) {
    std::string new_app_uuid;

    if (inputTree_p) {
      // If the input contains a non-empty "uuid", use it; otherwise generate one.
      new_app_uuid = json_string_member_or(*inputTree_p, "uuid");
      if (!new_app_uuid.empty()) {
        (*inputTree_p)["uuid"] = new_app_uuid;
      } else {
        new_app_uuid = uuid_util::uuid_t::generate().string();
        (*inputTree_p)["uuid"] = new_app_uuid;
      }

      // Remove "prep-cmd" if empty.
      if (inputTree_p->contains("prep-cmd") && (*inputTree_p)["prep-cmd"].empty()) {
        inputTree_p->erase("prep-cmd");
      }

      // Remove "detached" if empty.
      if (inputTree_p->contains("detached") && (*inputTree_p)["detached"].empty()) {
        inputTree_p->erase("detached");
      }

      // Remove keys that are no longer needed.
      inputTree_p->erase("launching");
      inputTree_p->erase("index");
    }

    // Get the current apps array; if it doesn't exist, create one.
    nlohmann::json newApps = nlohmann::json::array();
    if (fileTree_p->contains("apps") && (*fileTree_p)["apps"].is_array()) {
      for (auto &app : (*fileTree_p)["apps"]) {
        // For apps without a UUID, generate one and remove "launching".
        const auto existing_uuid = json_string_member_or(app, "uuid");
        if (existing_uuid.empty()) {
          const auto generated_uuid = uuid_util::uuid_t::generate().string();
          BOOST_LOG(warning) << "App [" << json_app_label(app) << "] has missing or invalid [uuid] in apps.json; generating a new UUID.";
          app["uuid"] = generated_uuid;
          app.erase("launching");
          newApps.push_back(std::move(app));
        } else {
          // If an app with the same UUID as the new app is found, replace it.
          if (!new_app_uuid.empty() && existing_uuid == new_app_uuid) {
            newApps.push_back(*inputTree_p);
            new_app_uuid.clear();
          } else {
            newApps.push_back(std::move(app));
          }
        }
      }
    }
    // If the new app's UUID has not been merged yet, add it.
    if (!new_app_uuid.empty() && inputTree_p) {
      newApps.push_back(*inputTree_p);
    }
    (*fileTree_p)["apps"] = newApps;
  }

  void migration_v2(nlohmann::json& fileTree) {
    static const int this_version = 2;
    // Determine the current migration version (default to 1 if not present).
    int file_version = json_int_member_or(fileTree, "version", 1);
    if (fileTree.contains("version") && !coerce_json_int(fileTree["version"]).has_value()) {
      BOOST_LOG(info) << "Cannot parse apps.json version, treating as v1.";
    }

    // If the version is less than this_version, perform legacy conversion.
    if (file_version < this_version) {
      BOOST_LOG(info) << "Migrating app list from v1 to v2...";
      migrate_apps(&fileTree, nullptr);

      // Keys to convert to booleans along with the parser defaults for invalid legacy values.
      const std::vector<std::pair<std::string, bool>> boolean_keys = {
        {"allow-client-commands", true},
        {"exclude-global-prep-cmd", false},
        {"exclude-global-state-cmd", false},
        {"elevated", false},
        {"auto-detach", true},
        {"wait-all", true},
        {"use-app-identity", false},
        {"per-client-app-identity", false},
        {"virtual-display", false},
        {"virtual-display-primary", false},
        {"terminate-on-pause", false}
      };

      // Keys to convert to integers along with the parser defaults for invalid legacy values.
      const std::vector<std::pair<std::string, int>> integer_keys = {
        {"exit-timeout", 5},
        {"scale-factor", 100},
        {"last-launched", 0}
      };

      // Walk through each app and convert legacy string values.
      if (!fileTree.contains("apps") || !fileTree["apps"].is_array()) {
        fileTree["apps"] = nlohmann::json::array();
      }

      for (auto &app : fileTree["apps"]) {
        for (const auto &[key, default_value] : boolean_keys) {
          if (app.contains(key)) {
            app[key] = coerce_json_bool(app[key], default_value);
          }
        }

        for (const auto &[key, default_value] : integer_keys) {
          if (app.contains(key)) {
            if (const auto parsed_value = coerce_json_int(app[key]); parsed_value.has_value()) {
              app[key] = *parsed_value;
            } else {
              BOOST_LOG(warning) << "App [" << json_app_label(app) << "] has invalid [" << key << "] in legacy apps.json; using default [" << default_value << "].";
              app[key] = default_value;
            }
          }
        }

        // For each entry in the "prep-cmd" array, convert "elevated" if necessary.
        if (app.contains("prep-cmd") && app["prep-cmd"].is_array()) {
          for (auto &prep : app["prep-cmd"]) {
            if (prep.contains("elevated")) {
              prep["elevated"] = coerce_json_bool(prep["elevated"], false);
            }
          }
        }
      }

      // Update migration version to this_version.
      fileTree["version"] = this_version;

      BOOST_LOG(info) << "Migrated app list from v1 to v2.";
    }
  }

  void migration_v3(nlohmann::json &fileTree) {
    static const int this_version = 7;
    int file_version = json_int_member_or(fileTree, "version", 0);
    if (fileTree.contains("version") && !coerce_json_int(fileTree["version"]).has_value()) {
      BOOST_LOG(info) << "Cannot parse apps.json version while checking Steam launch migration, treating as v0.";
    }

    if (file_version >= this_version) {
      return;
    }

    if (!fileTree.contains("apps") || !fileTree["apps"].is_array()) {
      fileTree["version"] = this_version;
      return;
    }

    for (auto &app : fileTree["apps"]) {
      const auto app_name = json_string_member_or(app, "name");
      if (is_steam_big_picture_name(app_name)) {
        continue;
      }

      auto steam_appid = json_string_member_or(app, "steam-appid");
      const auto source = json_string_member_or(app, "source", steam_appid.empty() ? "manual" : "steam");

      if (steam_appid.empty()) {
        if (const auto detected = extract_steam_appid_from_command(json_string_member_or(app, "cmd")); detected) {
          steam_appid = *detected;
        } else if (app.contains("detached") && app["detached"].is_array()) {
          for (const auto &detached_val : app["detached"]) {
            if (!detached_val.is_string()) {
              continue;
            }
            if (const auto detected = extract_steam_appid_from_command(detached_val.get<std::string>()); detected) {
              steam_appid = *detected;
              break;
            }
          }
        }
      }

      if (steam_appid.empty() && !boost::iequals(source, "steam")) {
        continue;
      }

      if (steam_appid.empty()) {
        continue;
      }

      app["source"] = "steam";
      app["steam-appid"] = steam_appid;

      std::string reference_cmd = json_string_member_or(app, "cmd");
      std::vector<nlohmann::json> preserved_detached;

      if (app.contains("detached") && app["detached"].is_array()) {
        for (const auto &detached_val : app["detached"]) {
          if (!detached_val.is_string()) {
            preserved_detached.emplace_back(detached_val);
            continue;
          }

          auto cmd = detached_val.get<std::string>();
          if (command_is_steam_library_launch_component(cmd)) {
            if (reference_cmd.empty()) {
              reference_cmd = cmd;
            }
            continue;
          }

          preserved_detached.emplace_back(std::move(cmd));
        }
      }

      if (app.contains("cmd")) {
        auto cmd = json_string_member_or(app, "cmd");
        if (!cmd.empty() && command_is_steam_library_launch_component(cmd)) {
          if (reference_cmd.empty()) {
            reference_cmd = cmd;
          }
          BOOST_LOG(info) << "Migrating Steam library primary command into detached launch components for ["
                          << json_app_label(app) << ']';
          app["cmd"] = "";
        }
      }

      const auto steam_launch_mode = proc::normalize_steam_launch_mode(json_string_member_or(app, "steam-launch-mode", "direct"));
      app["steam-launch-mode"] = steam_launch_mode;
      const auto canonical_commands = canonical_steam_library_launch_commands(reference_cmd, steam_appid, steam_launch_mode);
      nlohmann::json detached_commands = nlohmann::json::array();
      for (const auto &cmd : canonical_commands) {
        BOOST_LOG(info) << "Migrating Steam library launch command for [" << json_app_label(app)
                        << "] to [" << cmd << ']';
        detached_commands.push_back(cmd);
      }
      for (const auto &cmd : preserved_detached) {
        detached_commands.push_back(cmd);
      }
      app["detached"] = std::move(detached_commands);

      bool has_shutdown = false;
      if (app.contains("prep-cmd") && app["prep-cmd"].is_array()) {
        for (auto &prep : app["prep-cmd"]) {
          if (!prep.is_object()) {
            continue;
          }

          auto undo_cmd = json_string_member_or(prep, "undo");
          if (command_contains_steam_big_picture_close(undo_cmd)) {
            const auto shutdown_cmd = canonical_steam_shutdown_command(undo_cmd);
            BOOST_LOG(info) << "Migrating Steam cleanup command for [" << json_app_label(app)
                            << "] from [" << undo_cmd << "] to [" << shutdown_cmd << ']';
            prep["undo"] = shutdown_cmd;
            undo_cmd = shutdown_cmd;
          }

          if (command_requests_steam_shutdown(json_string_member_or(prep, "do")) ||
              command_requests_steam_shutdown(undo_cmd)) {
            has_shutdown = true;
          }
        }
      }

      if (!has_shutdown) {
        std::string reference_cmd = "steam";
        if (app.contains("detached") && app["detached"].is_array() && !app["detached"].empty() && app["detached"][0].is_string()) {
          reference_cmd = app["detached"][0].get<std::string>();
        }
        const auto shutdown_cmd = canonical_steam_shutdown_command(reference_cmd);
        if (!app.contains("prep-cmd") || !app["prep-cmd"].is_array()) {
          app["prep-cmd"] = nlohmann::json::array();
        }
        app["prep-cmd"].push_back({
          {"undo", shutdown_cmd}
        });
        BOOST_LOG(info) << "Migrating Steam library cleanup undo command for [" << json_app_label(app)
                        << "] [" << shutdown_cmd << ']';
      }
    }

    fileTree["version"] = this_version;
    BOOST_LOG(info) << "Migrated Steam library launch handling to v7.";
  }

  void migration_v4(nlohmann::json &fileTree) {
    static const int this_version = 8;
    int file_version = json_int_member_or(fileTree, "version", 0);
    if (fileTree.contains("version") && !coerce_json_int(fileTree["version"]).has_value()) {
      BOOST_LOG(info) << "Cannot parse apps.json version while checking Lutris library migration, treating as v0.";
    }

    if (file_version >= this_version) {
      return;
    }

    if (!fileTree.contains("apps") || !fileTree["apps"].is_array()) {
      fileTree["version"] = this_version;
      return;
    }

    const auto &apps = fileTree["apps"];
    const bool has_lutris_games = std::any_of(apps.begin(), apps.end(), [](const auto &app) {
      return has_lutris_game_app(app);
    });
    const bool has_lutris_launcher = std::any_of(apps.begin(), apps.end(), [](const auto &app) {
      return is_lutris_library_app(app);
    });

    if (has_lutris_games && !has_lutris_launcher) {
      auto app = lutris_library_app();
      migrate_apps(&fileTree, &app);
      BOOST_LOG(info) << "Migrated app list to add Lutris library launcher.";
    }

    fileTree["version"] = this_version;
  }

  void migrate(nlohmann::json& fileTree, const std::string& fileName) {
    int last_version = 8;

    int file_version = json_int_member_or(fileTree, "version", 0);
    if (fileTree.contains("version") && !coerce_json_int(fileTree["version"]).has_value()) {
      BOOST_LOG(info) << "Cannot parse apps.json version while checking migrations, treating as v0.";
    }

    if (file_version < last_version) {
      migration_v2(fileTree);
      migration_v3(fileTree);
      migration_v4(fileTree);
      file_handler::write_file(fileName.c_str(), fileTree.dump(4));
    }
  }

  std::optional<proc::proc_t> parse(const std::string &file_name) {

    // Prepare environment variables.
    auto this_env = boost::this_process::environment();

    std::set<std::string> ids;
    std::vector<proc::ctx_t> apps;
    int i = 0;

    size_t fail_count = 0;
    do {
      // Read the JSON file into a tree.
      nlohmann::json tree;
      try {
        std::string content = file_handler::read_file(file_name.c_str());
        tree = nlohmann::json::parse(content);
      } catch (const std::exception& e) {
        BOOST_LOG(warning) << "Couldn't read apps.json properly! Apps will not be loaded."sv;
        break;
      }

      try {
        migrate(tree, file_name);

        if (tree.contains("env") && tree["env"].is_object()) {
          for (auto &item : tree["env"].items()) {
            this_env[item.key()] = parse_env_val(this_env, item.value().get<std::string>());
          }
        }

        // Ensure the "apps" array exists.
        if (!tree.contains("apps") || !tree["apps"].is_array()) {
          BOOST_LOG(warning) << "No apps were defined in apps.json!!!"sv;
          break;
        }

        // Iterate over each application in the "apps" array.
        for (auto &app_node : tree["apps"]) {
          proc::ctx_t ctx;
          ctx.idx = std::to_string(i);
          ctx.uuid = app_node.at("uuid");

          // Build the list of preparation commands.
          std::vector<proc::cmd_t> prep_cmds;
          bool exclude_global_prep = app_node.value("exclude-global-prep-cmd", false);
          if (!exclude_global_prep) {
            prep_cmds.reserve(config::sunshine.prep_cmds.size());
            for (auto &prep_cmd : config::sunshine.prep_cmds) {
              auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(prep_cmd.elevated)
              );
            }
          }
          if (app_node.contains("prep-cmd") && app_node["prep-cmd"].is_array()) {
            for (auto &prep_node : app_node["prep-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              prep_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of pause/resume commands.
          std::vector<proc::cmd_t> state_cmds;
          bool exclude_global_state_cmds = app_node.value("exclude-global-state-cmd", false);
          if (!exclude_global_state_cmds) {
            state_cmds.reserve(config::sunshine.state_cmds.size());
            for (auto &state_cmd : config::sunshine.state_cmds) {
              auto do_cmd = parse_env_val(this_env, state_cmd.do_cmd);
              auto undo_cmd = parse_env_val(this_env, state_cmd.undo_cmd);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(state_cmd.elevated)
              );
            }
          }
          if (app_node.contains("state-cmd") && app_node["state-cmd"].is_array()) {
            for (auto &prep_node : app_node["state-cmd"]) {
              std::string do_cmd = parse_env_val(this_env, prep_node.value("do", ""));
              std::string undo_cmd = parse_env_val(this_env, prep_node.value("undo", ""));
              bool elevated = prep_node.value("elevated", false);
              state_cmds.emplace_back(
                std::move(do_cmd),
                std::move(undo_cmd),
                std::move(elevated)
              );
            }
          }

          // Build the list of detached commands.
          std::vector<std::string> detached;
          if (app_node.contains("detached") && app_node["detached"].is_array()) {
            for (auto &detached_val : app_node["detached"]) {
              detached.emplace_back(parse_env_val(this_env, detached_val.get<std::string>()));
            }
          }

          // Process other fields.
          if (app_node.contains("output"))
            ctx.output = parse_env_val(this_env, app_node.value("output", ""));
          std::string name = parse_env_val(this_env, app_node.value("name", ""));
          if (app_node.contains("cmd"))
            ctx.cmd = parse_env_val(this_env, app_node.value("cmd", ""));
          if (app_node.contains("working-dir")) {
            ctx.working_dir = parse_env_val(this_env, app_node.value("working-dir", ""));
    #ifdef _WIN32
            // The working directory, unlike the command itself, should not be quoted.
            boost::erase_all(ctx.working_dir, "\"");
            ctx.working_dir += '\\';
    #endif
          }
          if (app_node.contains("image-path"))
            ctx.image_path = parse_env_val(this_env, app_node.value("image-path", ""));

          ctx.elevated = app_node.value("elevated", false);
          ctx.auto_detach = app_node.value("auto-detach", true);
          ctx.wait_all = app_node.value("wait-all", true);
          ctx.exit_timeout = std::chrono::seconds { app_node.value("exit-timeout", 5) };
          ctx.virtual_display = app_node.value("virtual-display", false);
          ctx.isolated_session = app_node.value("isolated-session", false);
          ctx.output_name = app_node.value("output-name", "");

          // Display Source — the canonical per-app selector for where this
          // app's pixels come from: "" / "default" (primary display),
          // "isolated" (hidden compositor), "virtual" (created virtual
          // screen), "output" (a specific connector via output-name).
          // Legacy flags migrate silently; an explicit display-source wins
          // over legacy flags and normalizes them so every downstream read
          // (isolated_session / virtual_display / output_name) stays valid.
          ctx.display_source = app_node.value("display-source", "");
          if (ctx.display_source.empty()) {
            if (ctx.isolated_session) {
              ctx.display_source = "isolated";
            } else if (ctx.virtual_display) {
              ctx.display_source = "virtual";
            } else if (!ctx.output_name.empty()) {
              ctx.display_source = "output";
            } else {
              ctx.display_source = "default";
            }
          }
          ctx.isolated_session = (ctx.display_source == "isolated");
          ctx.virtual_display = (ctx.display_source == "virtual");
          if (ctx.display_source != "output") {
            ctx.output_name.clear();
          }

          ctx.scale_factor = app_node.value("scale-factor", 100);
          ctx.use_app_identity = app_node.value("use-app-identity", false);
          ctx.per_client_app_identity = app_node.value("per-client-app-identity", false);
          ctx.allow_client_commands = app_node.value("allow-client-commands", true);
          ctx.terminate_on_pause = app_node.value("terminate-on-pause", false);
          ctx.gamepad = app_node.value("gamepad", "");
          ctx.steam_appid = app_node.value("steam-appid", "");
          ctx.steam_launch_mode = proc::normalize_steam_launch_mode(app_node.value("steam-launch-mode", "direct"));
          ctx.game_category = app_node.value("game-category", "");
          ctx.source = app_node.value("source", ctx.steam_appid.empty() ? "manual" : "steam");
          ctx.last_launched = app_node.value("last-launched", (int64_t)0);
          if (app_node.contains("genres") && app_node["genres"].is_array()) {
            for (const auto &g : app_node["genres"]) {
              if (g.is_string()) ctx.genres.push_back(g.get<std::string>());
            }
          }
          if (app_node.contains("env") && app_node["env"].is_object()) {
            for (auto &[key, val] : app_node["env"].items()) {
              if (val.is_string()) ctx.env_vars[key] = val.get<std::string>();
            }
          }

          // Calculate a unique application id.
          auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
          if (ids.count(std::get<0>(possible_ids)) == 0) {
            ctx.id = std::get<0>(possible_ids);
          } else {
            ctx.id = std::get<1>(possible_ids);
          }
          ids.insert(ctx.id);

          ctx.name = std::move(name);
          ctx.prep_cmds = std::move(prep_cmds);
          ctx.state_cmds = std::move(state_cmds);
          ctx.detached = std::move(detached);
          normalize_steam_big_picture_app(ctx);
          normalize_steam_library_app(ctx);

          apps.emplace_back(std::move(ctx));
        }

        fail_count = 0;
      } catch (std::exception &e) {
        BOOST_LOG(error) << "Error happened during app loading: "sv << e.what();

        fail_count += 1;

        if (fail_count >= 3) {
          // No hope for recovering
          BOOST_LOG(warning) << "Couldn't parse/migrate apps.json properly! Apps will not be loaded."sv;
          break;
        }

        BOOST_LOG(warning) << "App format is still invalid! Trying to re-migrate the app list..."sv;

        // Always try migrating from scratch when error happened
        tree["version"] = 0;

        try {
          migrate(tree, file_name);
        } catch (std::exception &e) {
          BOOST_LOG(error) << "Error happened during migration: "sv << e.what();
          break;
        }

        this_env = boost::this_process::environment();
        ids.clear();
        apps.clear();
        i = 0;

        continue;
      }

      break;
    } while (fail_count < 3);

    if (fail_count > 0) {
      BOOST_LOG(warning) << "No applications configured, adding fallback Desktop entry.";
      proc::ctx_t ctx;
      ctx.idx = std::to_string(i);
      ctx.uuid = FALLBACK_DESKTOP_UUID; // Placeholder UUID
      ctx.name = "Desktop (fallback)";
      ctx.image_path = parse_env_val(this_env, "desktop-alt.png");
      ctx.virtual_display = false;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false; // Desktop doesn't have a specific command to wait for
      ctx.exit_timeout = 5s;

      // Calculate unique ID
      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      } else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }

    // Virtual Display entry
  #ifdef _WIN32
    if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
      proc::ctx_t ctx;
      ctx.idx = std::to_string(i);
      ctx.uuid = VIRTUAL_DISPLAY_UUID;
      ctx.name = "Virtual Display";
      ctx.image_path = parse_env_val(this_env, "virtual_desktop.png");
      ctx.virtual_display = true;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false;
      ctx.exit_timeout = 5s;

      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        // Avoid using index to generate id if possible
        ctx.id = std::get<0>(possible_ids);
      }
      else {
        // Fallback to include index on collision
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }
  #elif defined(__linux__)
    // Linux virtual display entry — shown when any virtual display backend is available
    if (isLinuxVDisplayAvailable()) {
      proc::ctx_t ctx;
      ctx.idx = std::to_string(i);
      ctx.uuid = VIRTUAL_DISPLAY_UUID;
      ctx.name = "Virtual Display";
      ctx.image_path = parse_env_val(this_env, "virtual_desktop.png");
      ctx.virtual_display = true;
      ctx.scale_factor = 100;
      ctx.use_app_identity = false;
      ctx.per_client_app_identity = false;
      ctx.allow_client_commands = false;
      ctx.terminate_on_pause = false;

      ctx.elevated = false;
      ctx.auto_detach = true;
      ctx.wait_all = false;
      ctx.exit_timeout = 5s;

      auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
      if (ids.count(std::get<0>(possible_ids)) == 0) {
        ctx.id = std::get<0>(possible_ids);
      }
      else {
        ctx.id = std::get<1>(possible_ids);
      }
      ids.insert(ctx.id);

      apps.emplace_back(std::move(ctx));
    }
  #endif

    if (config::input.enable_input_only_mode) {
      // Input Only entry
      {
        proc::ctx_t ctx;
        ctx.idx = std::to_string(i);
        ctx.uuid = REMOTE_INPUT_UUID;
        ctx.name = "Remote Input";
        ctx.image_path = parse_env_val(this_env, "input_only.png");
        ctx.virtual_display = false;
        ctx.scale_factor = 100;
        ctx.use_app_identity = false;
        ctx.per_client_app_identity = false;
        ctx.allow_client_commands = false;
        ctx.terminate_on_pause = true; // There's no need to keep an active input only session ongoing

        ctx.elevated = false;
        ctx.auto_detach = true;
        ctx.wait_all = true;
        ctx.exit_timeout = 5s;

        auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        }
        else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        input_only_app_id_str = ctx.id;
        input_only_app_id = util::from_view(ctx.id);

        apps.emplace_back(std::move(ctx));
      }

      // Terminate entry
      {
        proc::ctx_t ctx;
        ctx.idx = std::to_string(i);
        ctx.uuid = TERMINATE_APP_UUID;
        ctx.name = "Terminate";
        ctx.image_path = parse_env_val(this_env, "terminate.png");
        ctx.virtual_display = false;
        ctx.scale_factor = 100;
        ctx.use_app_identity = false;
        ctx.per_client_app_identity = false;
        ctx.allow_client_commands = false;
        ctx.terminate_on_pause = false;

        ctx.elevated = false;
        ctx.auto_detach = true;
        ctx.wait_all = true;
        ctx.exit_timeout = 5s;

        auto possible_ids = calculate_app_id(ctx.name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        }
        else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        // ids.insert(ctx.id);

        terminate_app_id_str = ctx.id;
        terminate_app_id = util::from_view(ctx.id);

        apps.emplace_back(std::move(ctx));
      }
    }

    return proc::proc_t {
      std::move(this_env),
      std::move(apps)
    };
  }

  void refresh(const std::string &file_name, bool needs_terminate) {
    if (needs_terminate) {
      proc.terminate(false, false);
    }

  #ifdef _WIN32
    size_t fail_count = 0;
    while (fail_count < 5 && vDisplayDriverStatus != VDISPLAY::DRIVER_STATUS::OK) {
      initVDisplayDriver();
      if (vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK) {
        break;
      }

      fail_count += 1;
      std::this_thread::sleep_for(1s);
    }
  #endif

    proc.reload_configuration_from_file(file_name);
  }
}  // namespace proc
