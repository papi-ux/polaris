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
#include <cmath>
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
  #include <dirent.h>
  #include <signal.h>
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

  namespace {
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

    std::string canonical_steam_library_followup_command(const std::string &reference_cmd, const std::string &appid) {
      return steam_big_picture_command_prefix(reference_cmd.empty() ? "steam" : reference_cmd) +
             "bash -lc \"sleep 6; "
             "steam steam://rungameid/" + appid + " >/dev/null 2>&1 || true; "
             "sleep 4; "
             "exec steam -applaunch " + appid + " >/dev/null 2>&1 || true\"";
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

    std::vector<std::string> canonical_steam_library_launch_commands(const std::string &reference_cmd, const std::string &appid) {
#ifdef __linux__
      return {
        canonical_steam_library_bootstrap_command(reference_cmd),
        canonical_steam_library_followup_command(reference_cmd, appid),
      };
#else
      return {
        steam_big_picture_command_prefix(reference_cmd.empty() ? "steam" : reference_cmd) +
          "steam steam://rungameid/" + appid
      };
#endif
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

    bool is_proc_pid_dir(std::string_view name) {
      return !name.empty() && std::all_of(name.begin(), name.end(), [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch));
      });
    }

    std::string read_proc_text(pid_t pid, std::string_view file_name) {
      std::ifstream file("/proc/" + std::to_string(pid) + "/" + std::string(file_name), std::ios::binary);
      if (!file) {
        return {};
      }

      std::ostringstream buffer;
      buffer << file.rdbuf();
      return buffer.str();
    }

    std::mutex isolated_browser_stream_steam_cleanup_mutex;
    std::chrono::steady_clock::time_point isolated_browser_stream_steam_settle_until {};

    std::string ascii_lower_copy(std::string_view value) {
      std::string lowered {value};
      std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return lowered;
    }

    std::string proc_argv0_lower(const std::string &cmdline) {
      const auto end = cmdline.find('\0');
      return ascii_lower_copy(std::string_view {cmdline.data(), end == std::string::npos ? cmdline.size() : end});
    }

    std::string proc_argv0_basename(std::string_view argv0_path) {
      auto argv0 = std::string {argv0_path};
      if (const auto slash = argv0.find_last_of('/'); slash != std::string::npos) {
        argv0 = argv0.substr(slash + 1);
      }
      return argv0;
    }

    bool path_ends_with(std::string_view value, std::string_view suffix) {
      return value.size() >= suffix.size() &&
             value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool process_is_steam_client(std::string_view comm, std::string_view argv0_path) {
      const auto argv0 = proc_argv0_basename(argv0_path);
      return comm == "steam" ||
             argv0 == "steam" ||
             argv0 == "steam.sh" ||
             path_ends_with(argv0_path, "/steam.sh") ||
             path_ends_with(argv0_path, "/ubuntu12_32/steam");
    }

    std::vector<pid_t> scan_proc_pids(const std::function<bool(pid_t)> &predicate) {
      std::vector<pid_t> pids;
      DIR *dir = opendir("/proc");
      if (!dir) {
        return pids;
      }

      while (auto *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (!is_proc_pid_dir(name)) {
          continue;
        }

        const auto pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
        if (pid <= 1 || pid == getpid()) {
          continue;
        }

        if (predicate(pid)) {
          pids.emplace_back(pid);
        }
      }

      closedir(dir);
      return pids;
    }

    std::vector<pid_t> transient_steam_client_pids() {
      return scan_proc_pids([](pid_t pid) {
        auto comm = ascii_lower_copy(read_proc_text(pid, "comm"));
        boost::trim(comm);

        const auto cmdline = read_proc_text(pid, "cmdline");
        const auto argv0 = proc_argv0_lower(cmdline);
        if (!process_is_steam_client(comm, argv0)) {
          return false;
        }

        const auto lower_cmdline = ascii_lower_copy(cmdline);
        return lower_cmdline.find("-shutdown") != std::string::npos ||
               lower_cmdline.find("-child-update-ui") != std::string::npos ||
               lower_cmdline.find("-srt-logger-opened") != std::string::npos;
      });
    }

    std::vector<pid_t> steam_runtime_probe_pids() {
      return scan_proc_pids([](pid_t pid) {
        const auto lower_cmdline = ascii_lower_copy(read_proc_text(pid, "cmdline"));
        const auto lower_environ = ascii_lower_copy(read_proc_text(pid, "environ"));
        return lower_cmdline.find("d3ddriverquery") != std::string::npos ||
               lower_environ.find("d3ddriverquery") != std::string::npos;
      });
    }

    bool pid_exists(pid_t pid) {
      return std::filesystem::exists(std::filesystem::path("/proc") / std::to_string(pid));
    }

    bool any_pid_exists(const std::vector<pid_t> &pids) {
      return std::any_of(pids.begin(), pids.end(), pid_exists);
    }

    void signal_processes_and_groups(const std::vector<pid_t> &pids, int signal) {
      std::set<pid_t> groups;
      for (auto pid : pids) {
        const auto pgid = getpgid(pid);
        if (pgid > 1 && pgid != getpgrp()) {
          groups.insert(pgid);
        }
      }

      for (auto pgid : groups) {
        kill(-pgid, signal);
      }
      for (auto pid : pids) {
        kill(pid, signal);
      }
    }

    void terminate_steam_runtime_probes(std::string_view reason) {
      auto pids = steam_runtime_probe_pids();
      if (pids.empty()) {
        return;
      }

      BOOST_LOG(info) << "process: terminating "sv << pids.size()
                      << " Steam runtime probe process(es) "sv << reason;
      signal_processes_and_groups(pids, SIGTERM);

      for (int i = 0; i < 10; ++i) {
        if (!any_pid_exists(pids)) {
          return;
        }
        std::this_thread::sleep_for(100ms);
      }

      pids = steam_runtime_probe_pids();
      if (!pids.empty()) {
        BOOST_LOG(warning) << "process: Steam runtime probe process(es) did not exit after SIGTERM; sending SIGKILL"sv;
        signal_processes_and_groups(pids, SIGKILL);
      }
    }

    bool wait_for_transient_steam_clients(std::chrono::milliseconds timeout, std::string_view reason) {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      bool logged = false;

      while (std::chrono::steady_clock::now() < deadline) {
        auto pids = transient_steam_client_pids();
        if (pids.empty()) {
          return true;
        }

        if (!logged) {
          BOOST_LOG(info) << "process: waiting for "sv << pids.size()
                          << " transient Steam client process(es) "sv << reason;
          logged = true;
        }
        std::this_thread::sleep_for(250ms);
      }

      auto pids = transient_steam_client_pids();
      if (!pids.empty()) {
        BOOST_LOG(warning) << "process: "sv << pids.size()
                           << " transient Steam client process(es) still running after settle window"sv;
        return false;
      }
      return true;
    }

    bool proc_text_contains_steam_appid(const std::string &text, const std::string &appid) {
      if (appid.empty() || text.empty()) {
        return false;
      }

      return text.find("SteamLaunch AppId=" + appid) != std::string::npos ||
             text.find("SteamAppId=" + appid) != std::string::npos ||
             text.find("SteamGameId=" + appid) != std::string::npos ||
             text.find("steam://rungameid/" + appid) != std::string::npos ||
             (text.find("-applaunch") != std::string::npos && text.find(appid) != std::string::npos);
    }

    std::vector<pid_t> steam_app_pids(const std::string &appid) {
      std::vector<pid_t> pids;
      DIR *dir = opendir("/proc");
      if (!dir) {
        return pids;
      }

      while (auto *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (!is_proc_pid_dir(name)) {
          continue;
        }

        const auto pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
        if (pid <= 1 || pid == getpid()) {
          continue;
        }

        if (proc_text_contains_steam_appid(read_proc_text(pid, "cmdline"), appid) ||
            proc_text_contains_steam_appid(read_proc_text(pid, "environ"), appid)) {
          pids.emplace_back(pid);
        }
      }

      closedir(dir);
      return pids;
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

    void terminate_steam_app_processes(const proc::ctx_t &ctx) {
      const auto appid = steam_appid_for_context(ctx);
      if (appid.empty()) {
        return;
      }

      auto pids = steam_app_pids(appid);
      if (pids.empty()) {
        return;
      }

      std::set<pid_t> groups;
      for (auto pid : pids) {
        const auto pgid = getpgid(pid);
        if (pgid > 1 && pgid != getpgrp()) {
          groups.insert(pgid);
        }
      }

      BOOST_LOG(info) << "process: terminating Steam app ["sv << appid
                      << "] process groups="sv << groups.size()
                      << " matched_pids="sv << pids.size();

      for (auto pgid : groups) {
        kill(-pgid, SIGTERM);
      }
      for (auto pid : pids) {
        kill(pid, SIGTERM);
      }

      for (int i = 0; i < 20; ++i) {
        if (steam_app_pids(appid).empty()) {
          return;
        }
        std::this_thread::sleep_for(100ms);
      }

      pids = steam_app_pids(appid);
      if (!pids.empty()) {
        BOOST_LOG(warning) << "process: Steam app ["sv << appid
                           << "] did not exit after SIGTERM; sending SIGKILL"sv;
      }

      groups.clear();
      for (auto pid : pids) {
        const auto pgid = getpgid(pid);
        if (pgid > 1 && pgid != getpgrp()) {
          groups.insert(pgid);
        }
      }

      for (auto pgid : groups) {
        kill(-pgid, SIGKILL);
      }
      for (auto pid : pids) {
        kill(pid, SIGKILL);
      }
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

    bool is_browser_stream_session(const std::shared_ptr<rtsp_stream::launch_session_t> &launch_session) {
      return launch_session && launch_session->device_name == "Browser Stream";
    }

    void settle_isolated_browser_stream_steam_cleanup(const proc::ctx_t &ctx) {
      if (!context_uses_steam(ctx)) {
        return;
      }

      terminate_steam_runtime_probes("after Browser Stream cleanup"sv);
      wait_for_transient_steam_clients(4s, "after Browser Stream cleanup"sv);

      std::lock_guard lock(isolated_browser_stream_steam_cleanup_mutex);
      isolated_browser_stream_steam_settle_until = std::chrono::steady_clock::now() + 30s;
    }

    void settle_recent_browser_stream_steam_cleanup_before_launch(const proc::ctx_t &next_app) {
      if (!context_uses_steam(next_app)) {
        return;
      }

      {
        std::lock_guard lock(isolated_browser_stream_steam_cleanup_mutex);
        if (std::chrono::steady_clock::now() >= isolated_browser_stream_steam_settle_until) {
          return;
        }
      }

      BOOST_LOG(info) << "process: settling previous Browser Stream Steam cleanup before launching ["sv
                      << next_app.name << ']';
      terminate_steam_runtime_probes("before next Steam launch"sv);
      wait_for_transient_steam_clients(8s, "before next Steam launch"sv);
      std::this_thread::sleep_for(500ms);

      std::lock_guard lock(isolated_browser_stream_steam_cleanup_mutex);
      isolated_browser_stream_steam_settle_until = {};
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

      const auto canonical_commands = canonical_steam_library_launch_commands(reference_cmd, appid);
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
                                                                         const parsed_display_mode_t &requested) {
      auto clamped = candidate;

      if (requested.fps > 0 && clamped.fps > requested.fps) {
        clamped.fps = requested.fps;
      }

      if (requested.width > 0 && requested.height > 0 &&
          (clamped.width > requested.width || clamped.height > requested.height)) {
        clamped.width = requested.width;
        clamped.height = requested.height;
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
  static bool linux_vdisplay_available_checked = false;
  static bool linux_vdisplay_available = false;

  /**
   * @brief Check and cache whether Linux virtual display support is available.
   */
  bool isLinuxVDisplayAvailable() {
    if (!linux_vdisplay_available_checked) {
      const auto backend = virtual_display::detect_backend();
      linux_vdisplay_available = (backend != virtual_display::backend_e::NONE);
      linux_vdisplay_available_checked = true;
    }
    return linux_vdisplay_available;
  }

  namespace linux_display {
    /**
     * @brief Enable the streaming display and set display priorities for a streaming session.
     * Uses kscreen-doctor to enable the streaming output and set it as priority 1.
     */
    void enable_streaming_display() {
      const auto &cfg = config::video.linux_display;
      if (!cfg.auto_manage_displays || cfg.streaming_output.empty()) {
        return;
      }

      // Enable streaming output but keep primary display as priority 1
      // so KDE taskbar stays on the main display
      std::string cmd = "kscreen-doctor output." + cfg.streaming_output + ".enable";
      if (!cfg.primary_output.empty()) {
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

      std::string cmd = "kscreen-doctor";
      if (!cfg.primary_output.empty()) {
        cmd += " output." + cfg.primary_output + ".priority.1";
      }
      cmd += " output." + cfg.streaming_output + ".priority.2 output." + cfg.streaming_output + ".disable";

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
    _app_id = input_only_app_id;
    _app_name = "Remote Input";
    _app.uuid = REMOTE_INPUT_UUID;
    _app.terminate_on_pause = true;
    allow_client_commands = false;
    placebo = true;
    _launch_session = std::move(launch_session);
    _client_session_report_recorded = false;
    if (_launch_session && _launch_session->session_token.empty()) {
      _launch_session->session_token = generate_session_token();
    }

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
    system_tray::update_tray_playing(_app_name);
#endif
  }

  int proc_t::execute(const ctx_t& app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    if (_app_id == input_only_app_id) {
      terminate(false, false);
      std::this_thread::sleep_for(1s);
    } else {
      // Ensure starting from a clean slate
      terminate(false, false);
    }

    _app = app;
    _app_id = util::from_view(app.id);
    _app_name = app.name;
    _launch_session = launch_session;
    _client_session_report_recorded = false;
    if (_launch_session && _launch_session->session_token.empty()) {
      _launch_session->session_token = generate_session_token();
    }
    allow_client_commands = app.allow_client_commands;

#ifdef __linux__
    settle_recent_browser_stream_steam_cleanup_before_launch(_app);
#endif

    this->initial_display = config::video.output_name;
    this->initial_color_range = config::video.color_range;
    this->initial_nvenc_tune = config::video.nvenc_tune;
    this->initial_max_bitrate = config::video.max_bitrate;
    this->initial_adaptive_max_bitrate = config::video.adaptive_bitrate.max_bitrate_kbps;

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
    optimization_locks.bitrate = config::video.max_bitrate > 0;

    resolved_session_optimization_t resolved_optimization;
    auto client_profile = client_profiles::get_client_profile(launch_session->device_name);
    if (client_profile) {
      BOOST_LOG(info) << "Applying client profile for \""sv << launch_session->device_name << '"';

      if (!client_profile->output_name.empty()) {
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

    // AI optimizer: cached results override device DB; unknown devices get a sync request.
    if (ai_optimizer::is_enabled()) {
      std::string gpu_info = config::video.adapter_name.empty()
        ? "NVIDIA GPU (NVENC)"s
        : config::video.adapter_name;
      auto history = ai_optimizer::get_session_history(launch_session->device_name, _app.name);
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

    if (!optimization_locks.display_mode && resolved_optimization.display_mode.has_value()) {
      const parsed_display_mode_t requested_display_mode {
        static_cast<int>(launch_session->requested_width),
        static_cast<int>(launch_session->requested_height),
        launch_session->requested_fps,
      };
      const auto normalized_display_mode =
        clamp_optimized_display_mode_to_client_request(*resolved_optimization.display_mode, requested_display_mode);

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
          "Explicit client display mode acts as a ceiling; optimization stayed within the requested resolution and frame rate."
        );
        append_normalization_reason(
          resolved_optimization,
          "Runtime policy capped optimized display mode to the explicit client request."
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
      config::video.linux_display.headless_mode &&
      config::video.linux_display.use_cage_compositor;
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
      config::video.adaptive_bitrate.max_bitrate_kbps = this->initial_adaptive_max_bitrate;
      terminate();
      display_device::revert_configuration();
#ifdef __linux__
      confighttp::set_session_state(confighttp::session_state_e::tearing_down);
      confighttp::emit_session_event("session_ending", "Cleaning up");
      // Stop cage compositor (kills the game running inside it)
      if (config::video.linux_display.use_cage_compositor) {
        cage_display_router::stop();
      }
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
    const bool should_use_linux_virtual_display =
      display_policy.use_host_virtual_display ||
      launch_session->virtual_display ||
      (!launch_session->user_locked_virtual_display && _app.virtual_display);

    if (
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
        rtsp_stream::session_count() == 0 &&
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

      if (env_flag_enabled(_app, _env, "MANGOHUD")) {
        auto mangohud_config = env_value(_app, _env, "MANGOHUD_CONFIG");
        if (mangohud_config.find("fps_limit=") == std::string::npos) {
          if (!mangohud_config.empty() && mangohud_config.back() != ',') {
            mangohud_config += ',';
          }
          mangohud_config += "fps_limit=" + std::to_string(pacing_target_fps);
          set_session_env_var(_env, _session_env_keys, "MANGOHUD_CONFIG", mangohud_config);
        }
      }

      BOOST_LOG(info) << "session_pacing: policy="sv << launch_session->pacing_policy
                      << " target_fps="sv << pacing_target_fps;
    }

#ifdef __linux__
    bool cage_started_with_detached_client = false;

    auto reprobe_encoders_for_cage = [&](bool strict_configured_encoder = false, bool save_successful_cache = true) -> bool {
      if (!config::video.linux_display.use_cage_compositor || rtsp_stream::session_count() != 0) {
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

    const bool requested_headless = config::video.linux_display.headless_mode;
    const bool prefer_gpu_native_capture = config::video.linux_display.prefer_gpu_native_capture;
    const bool encoder_requires_gpu_native_capture = video::active_encoder_requires_gpu_native_capture();
    const bool should_try_gpu_native_cage_probe =
      prefer_gpu_native_capture || encoder_requires_gpu_native_capture;
    const bool should_probe_windowed_cage_for_gpu_native =
      config::video.linux_display.use_cage_compositor &&
      cage_display_router::should_attempt_windowed_gpu_native_probe(
        requested_headless,
        prefer_gpu_native_capture,
        should_try_gpu_native_cage_probe
      );
    const auto cached_windowed_gpu_native_probe_result =
      should_probe_windowed_cage_for_gpu_native ?
        cage_display_router::cached_windowed_gpu_native_probe_result() :
        std::optional<bool> {};
    const auto cached_headless_extcopy_dmabuf_probe_result =
      should_probe_windowed_cage_for_gpu_native ?
        cage_display_router::cached_headless_extcopy_dmabuf_probe_result() :
        std::optional<bool> {};
    bool force_windowed_cage_for_gpu_native = false;
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

      if (!cage_display_router::start(render_width, render_height, cage_refresh_hz, startup_cmd, force_windowed)) {
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
      if (!should_probe_windowed_cage_for_gpu_native || rtsp_stream::session_count() != 0) {
        return false;
      }

      auto encoder_name = video::active_encoder_name();
      auto encoder_label = encoder_name.empty() ? "unknown" : encoder_name;
      BOOST_LOG(info) << "session_manager: Probing headless_extcopy_dmabuf with encoder ["
                      << encoder_label << ']';

      auto stop_probe_cage = util::fail_guard([&]() {
        if (cage_display_router::is_running()) {
          cage_display_router::stop();
        }
      });

      if (!start_cage_session("", false, true, false)) {
        cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
        BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe could not initialize the compositor/encoder path"sv;
        return false;
      }

      const auto runtime_state = cage_display_router::runtime_state();
      if (!runtime_state.effective_headless) {
        cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
        BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe did not start an effective headless runtime"sv;
        return false;
      }

      if (!video::active_encoder_runtime_supports_config(gpu_native_probe_config)) {
        cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
        BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe did not validate the active encoder configuration"sv;
        return false;
      }

      if (cage_display_router::cached_headless_extcopy_dmabuf_probe_result() != std::optional<bool> {true}) {
        cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
        BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe did not initialize DMA-BUF capture"sv;
        return false;
      }

      cage_display_router::update_headless_extcopy_dmabuf_probe_result(true);
      BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf probe succeeded with encoder ["
                      << video::active_encoder_name() << ']';
      return true;
    };

    auto probe_windowed_gpu_native_cage = [&]() -> bool {
      if (!should_probe_windowed_cage_for_gpu_native || rtsp_stream::session_count() != 0) {
        return false;
      }

      auto encoder_name = video::active_encoder_name();
      auto encoder_label = encoder_name.empty() ? "unknown" : encoder_name;
      BOOST_LOG(info) << "session_manager: Probing windowed labwc for GPU-native capture with encoder ["
                      << encoder_label << ']';

      auto stop_probe_cage = util::fail_guard([&]() {
        if (cage_display_router::is_running()) {
          cage_display_router::stop();
        }
      });

      if (!start_cage_session("", true, true, false)) {
        cage_display_router::update_windowed_gpu_native_probe_result(false);
        BOOST_LOG(info) << "session_manager: Windowed GPU-native cage probe could not initialize the compositor/encoder path; staying headless"sv;
        return false;
      }

      if (!video::active_encoder_runtime_supports_live_gpu_capture(gpu_native_probe_config)) {
        cage_display_router::update_windowed_gpu_native_probe_result(false);
        BOOST_LOG(info) << "session_manager: Windowed GPU-native cage probe did not deliver a live DMA-BUF frame; staying headless"sv;
        return false;
      }

      cage_display_router::update_windowed_gpu_native_probe_result(true);
      BOOST_LOG(info) << "session_manager: Windowed GPU-native cage probe succeeded with encoder ["
                      << video::active_encoder_name() << ']';
      return true;
    };

    auto resolve_windowed_gpu_native_fallback = [&]() -> bool {
      if (cached_windowed_gpu_native_probe_result == std::optional<bool> {true}) {
        BOOST_LOG(info) << "session_manager: Reusing cached windowed_dmabuf_override probe result"sv;
        return true;
      }

      if (cached_windowed_gpu_native_probe_result == std::optional<bool> {false}) {
        BOOST_LOG(info) << "session_manager: Cached windowed_dmabuf_override probe result indicates nested DMA-BUF capture is unavailable on this runtime"sv;
        return false;
      }

      return probe_windowed_gpu_native_cage();
    };

    if (cached_headless_extcopy_dmabuf_probe_result == std::optional<bool> {true}) {
      BOOST_LOG(info) << "session_manager: Reusing cached headless_extcopy_dmabuf probe result; keeping true headless runtime"sv;
    } else if (cached_headless_extcopy_dmabuf_probe_result == std::optional<bool> {false}) {
      BOOST_LOG(info) << "session_manager: Cached headless_extcopy_dmabuf probe result indicates headless DMA-BUF capture is unavailable; evaluating windowed fallback"sv;
      force_windowed_cage_for_gpu_native = resolve_windowed_gpu_native_fallback();
    } else if (probe_headless_extcopy_dmabuf_cage()) {
      BOOST_LOG(info) << "session_manager: headless_extcopy_dmabuf selected; keeping true headless runtime"sv;
    } else if (should_probe_windowed_cage_for_gpu_native) {
      BOOST_LOG(info) << "session_manager: headless_shm_fallback would be used for true headless; evaluating windowed GPU-native fallback"sv;
      force_windowed_cage_for_gpu_native = resolve_windowed_gpu_native_fallback();
    }

    auto encoder_name = video::active_encoder_name();
    auto encoder_label = encoder_name.empty() ? "unknown" : encoder_name;
    if (force_windowed_cage_for_gpu_native) {
      BOOST_LOG(warning) << "session_manager: windowed_dmabuf_override selected because headless_extcopy_dmabuf was unavailable and encoder ["
                         << encoder_label
                         << "] can use a GPU-native capture path"sv;
    }

    auto start_cage_with_runtime_fallback = [&](const std::string &startup_cmd) -> bool {
      confighttp::set_session_state(confighttp::session_state_e::cage_starting);
      confighttp::emit_session_event("cage_starting", "Starting compositor");

      if (force_windowed_cage_for_gpu_native && start_cage_session(startup_cmd, true)) {
        return true;
      }

      if (force_windowed_cage_for_gpu_native) {
        cage_display_router::update_windowed_gpu_native_probe_result(false);
        BOOST_LOG(warning) << "session_manager: windowed_dmabuf_override start failed; falling back to headless_shm_fallback if headless DMA-BUF remains unavailable"sv;
        force_windowed_cage_for_gpu_native = false;
      }

      return start_cage_session(startup_cmd, false);
    };

    if (!_app.detached.empty() || !_app.cmd.empty()) {
      input::preallocate_gamepad();
    }

    // Start cage with the first detached command as its primary client.
    // Cage is a kiosk compositor — it only displays its main process fullscreen.
    if (config::video.linux_display.use_cage_compositor && !_app.detached.empty()) {
      std::string game_cmd = cage_runtime_command(_app.detached[0]);
      if (!start_cage_with_runtime_fallback(game_cmd)) {
        BOOST_LOG(error) << "session_manager: Failed to start cage with game"sv;
        confighttp::emit_session_event("error", "Failed to start cage compositor");
        return 503;
      } else {
        cage_started_with_detached_client = true;
        confighttp::set_session_state(confighttp::session_state_e::game_launching);
        confighttp::emit_session_event("game_launching", "Launching " + _app.name);
      }
      // Launch remaining detached commands (if any) with cage's WAYLAND_DISPLAY
      for (size_t i = 1; i < _app.detached.size(); ++i) {
        auto cmd = cage_runtime_command(_app.detached[i]);
        auto cmd_env = _env;
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
        boost::filesystem::path working_dir = _app.working_dir.empty() ?
                                                find_working_directory(cmd, cmd_env) :
                                                boost::filesystem::path(_app.working_dir);
        BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
        auto child = platf::run_command(_app.elevated, true, cmd, working_dir, cmd_env, _pipe.get(), ec, &_process_group);
        if (ec) {
          BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
        } else {
          child.detach();
        }
      }
    } else if (config::video.linux_display.use_cage_compositor) {
      // No detached commands — start cage empty, game will use _app.cmd
      if (!start_cage_with_runtime_fallback("")) {
        BOOST_LOG(error) << "session_manager: Failed to start cage compositor"sv;
        return 503;
      }
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
    auto launch_env = _env;
    if (config::video.linux_display.use_cage_compositor && cage_display_router::is_running() && !_app.cmd.empty()) {
      effective_cmd = cage_runtime_command(_app.cmd);
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
#else
    const std::string &effective_cmd = _app.cmd;
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
                                              find_working_directory(effective_cmd, launch_env) :
                                              boost::filesystem::path(_app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << effective_cmd << "] in ["sv << working_dir << ']';
      _process = platf::run_command(_app.elevated, true, effective_cmd, working_dir, launch_env, _pipe.get(), ec, &_process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << effective_cmd << "]: System: "sv << ec.message();
        return -1;
      }
    }

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
      terminate();
    }

    return 0;
  }

  void proc_t::resume() {
    BOOST_LOG(info) << "Session resuming for app [" << _app_name << "].";
    _client_session_report_recorded = false;

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
    if (!running()) {
      BOOST_LOG(info) << "Session already stopped, do not run pause commands.";
      return;
    }

    if (_app.terminate_on_pause) {
      BOOST_LOG(info) << "Terminating app [" << _app_name << "] when all clients are disconnected. Pause commands are skipped.";
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
        session.session_count = 1;
        session.last_optimization_source = _launch_session->optimization_source;
        session.last_optimization_confidence = _launch_session->optimization_confidence;
        session.last_recommendation_version = _launch_session->optimization_recommendation_version;
        session.last_quality_grade = grade_session_quality(stats, session.last_target_fps);
        session.quality_grade = session.last_quality_grade;
        session.codec = session.last_codec;

        const double fps_gap =
          session.last_target_fps > 0.0 ? std::max(0.0, session.last_target_fps - session.last_fps) : 0.0;
        const bool network_risk = stats.packet_loss >= 0.35 || stats.latency_ms >= 28.0;
        const bool pacing_risk =
          stats.frame_jitter_ms >= 2.2 ||
          stats.duplicate_frame_ratio >= 0.10 ||
          stats.dropped_frame_ratio >= 0.04 ||
          fps_gap >= 4.0;
        const bool capture_fallback =
          stream_stats::capture_path_uses_cpu_copy(stats);
        const auto capture_path = stream_stats::capture_path_summary(stats);
        const bool encoder_risk = stats.encode_time_ms >= 11.0 || stats.avg_frame_age_ms >= 18.0;
        const bool hdr_risk = stats.dynamic_range > 0 && (pacing_risk || encoder_risk);
        const std::string codec_lower = boost::algorithm::to_lower_copy(session.last_codec);
        const bool av1_codec = codec_lower.find("av1") != std::string::npos;
        const bool decoder_risk = av1_codec && (pacing_risk || fps_gap >= 4.0) && !network_risk;
        const bool virtual_display_risk = _launch_session->virtual_display && (pacing_risk || capture_fallback || hdr_risk);

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
        if (network_risk) session.last_primary_issue = "network_jitter";
        else if (hdr_risk) session.last_primary_issue = "hdr_path";
        else if (virtual_display_risk) session.last_primary_issue = "virtual_display_path";
        else if (decoder_risk) session.last_primary_issue = "decoder_path";
        else if (capture_fallback) session.last_primary_issue = "capture_fallback";
        else if (pacing_risk) session.last_primary_issue = "frame_pacing";
        else if (encoder_risk) session.last_primary_issue = "encoder_load";
        else session.last_primary_issue = "steady";
        if (network_risk) session.last_issues.push_back("network_jitter");
        if (pacing_risk) session.last_issues.push_back("frame_pacing");
        if (capture_fallback) session.last_issues.push_back("capture_fallback");
        if (encoder_risk) session.last_issues.push_back("encoder_load");
        if (hdr_risk) session.last_issues.push_back("hdr_path");
        if (decoder_risk) session.last_issues.push_back("decoder_path");
        if (virtual_display_risk) session.last_issues.push_back("virtual_display_path");
        const int concern_count =
          static_cast<int>(network_risk) +
          static_cast<int>(pacing_risk) +
          static_cast<int>(capture_fallback) +
          static_cast<int>(encoder_risk) +
          static_cast<int>(hdr_risk) +
          static_cast<int>(decoder_risk) +
          static_cast<int>(virtual_display_risk);
        session.last_health_grade =
          concern_count >= 2 || hdr_risk || decoder_risk ? "degraded" :
          concern_count == 1 ? "watch" :
          "good";
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
          (virtual_display_risk || config::video.linux_display.headless_mode) ? "headless" :
          (_launch_session->virtual_display ? "virtual_display" : "headless");
        if (stats.dynamic_range > 0) {
          session.last_safe_hdr = !hdr_risk;
        }
        session.last_relaunch_recommended = hdr_risk || decoder_risk || virtual_display_risk;

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

  void proc_t::terminate(bool immediate, bool needs_refresh) {
    std::error_code ec;
    placebo = false;

    if (!immediate) {
      terminate_process_group(_process, _process_group, _app.exit_timeout);
    }

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

#ifdef __linux__
    const bool should_settle_browser_stream_steam =
      is_browser_stream_session(_launch_session) &&
      config::video.linux_display.use_cage_compositor &&
      context_uses_steam(_app);

    // Stop labwc compositor — this kills all games running inside it.
    // Without this, games launched with setsid escape the process group
    // and keep running after the stream ends.
    if (config::video.linux_display.use_cage_compositor) {
      cage_display_router::stop();
      terminate_steam_app_processes(_app);
      if (should_settle_browser_stream_steam) {
        settle_isolated_browser_stream_steam_cleanup(_app);
      }
    }
#endif

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
      if (config::video.linux_display.use_cage_compositor &&
          !steam_appid_for_context(_app).empty() &&
          command_requests_steam_shutdown(cmd.undo_cmd)) {
        BOOST_LOG(info) << "Skipping Steam shutdown undo after isolated cage Steam app cleanup ["sv
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
    // Disable streaming display after undo commands have run.
    // Skip if a virtual display was created (it will be destroyed below).
    if (!linux_vdisplay.has_value() || !linux_vdisplay->active) {
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
    config::video.color_range = initial_color_range;
    config::video.nvenc_tune = initial_nvenc_tune;
    config::video.max_bitrate = initial_max_bitrate;
    config::video.adaptive_bitrate.max_bitrate_kbps = initial_adaptive_max_bitrate;

    _app_id = -1;
    _app_name.clear();
    _app = {};
    display_name.clear();
    initial_display.clear();
    initial_color_range = 0;
    initial_nvenc_tune = 0;
    initial_max_bitrate = 0;
    initial_adaptive_max_bitrate = 0;
    mode_changed_display.clear();
    _launch_session.reset();
    virtual_display = false;
    _session_shutdown_requested = false;
    allow_client_commands = false;

    if (_saved_input_config) {
      config::input = *_saved_input_config;
      _saved_input_config.reset();
    }

    cursor::set_visible(config::input.mouse_cursor_visible);

    if (needs_refresh) {
      refresh(config::stream.file_apps, false);
    }
  }

  const std::vector<ctx_t> &proc_t::get_apps() const {
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
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
    return _app_name;
  }

  std::string proc_t::get_running_app_uuid() {
    return _app.uuid;
  }

  std::string proc_t::get_session_token() {
    return _launch_session ? _launch_session->session_token : std::string {};
  }

  std::string proc_t::get_session_owner_unique_id() {
    return _launch_session ? _launch_session->unique_id : std::string {};
  }

  std::string proc_t::get_session_owner_device_name() {
    return _launch_session ? _launch_session->device_name : std::string {};
  }

  bool proc_t::is_session_owner(const std::string &unique_id) {
    return !_launch_session || _launch_session->unique_id.empty() || _launch_session->unique_id == unique_id;
  }

  void proc_t::mark_client_session_report_recorded(const std::string &unique_id) {
    if (!_launch_session) {
      return;
    }

    if (unique_id.empty() || _launch_session->unique_id.empty() || _launch_session->unique_id == unique_id) {
      _client_session_report_recorded = true;
    }
  }

  bool proc_t::client_session_report_recorded() const {
    return _client_session_report_recorded;
  }

  bool proc_t::session_display_mode_is_explicit() const {
    return _launch_session && _launch_session->user_locked_virtual_display;
  }

  bool proc_t::current_app_has_mangohud() const {
    if (_app.uuid.empty()) {
      return false;
    }

    return env_flag_enabled(_app, _env, "MANGOHUD");
  }

  void proc_t::set_app_mangohud_configured(const std::string &uuid, bool enabled) {
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

  void proc_t::set_session_shutdown_requested(bool requested) {
    _session_shutdown_requested = requested;
  }

  bool proc_t::session_shutdown_requested() const {
    return _session_shutdown_requested;
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
        BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
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

      const auto canonical_commands = canonical_steam_library_launch_commands(reference_cmd, steam_appid);
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
          ctx.scale_factor = app_node.value("scale-factor", 100);
          ctx.use_app_identity = app_node.value("use-app-identity", false);
          ctx.per_client_app_identity = app_node.value("per-client-app-identity", false);
          ctx.allow_client_commands = app_node.value("allow-client-commands", true);
          ctx.terminate_on_pause = app_node.value("terminate-on-pause", false);
          ctx.gamepad = app_node.value("gamepad", "");
          ctx.steam_appid = app_node.value("steam-appid", "");
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

    auto proc_opt = proc::parse(file_name);

    if (proc_opt) {
      proc = std::move(*proc_opt);
    }
  }
}  // namespace proc
