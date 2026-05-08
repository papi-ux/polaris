/**
 * @file src/browser_stream.cpp
 * @brief Browser Stream API and session metadata helpers.
 */

#include "browser_stream.h"

#include "audio.h"
#include "browser_stream_protocol.h"
#include "config.h"
#include "crypto.h"
#include "globals.h"
#include "httpcommon.h"
#include "input.h"
#include "logging.h"
#include "nvhttp.h"
#include "process.h"
#include "utility.h"
#include "video.h"

extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

#include <boost/endian/buffers.hpp>
#include <boost/process/v1/search_path.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifdef __linux__
  #include <boost/asio.hpp>
  #include <boost/asio/local/stream_protocol.hpp>
  #include <boost/process/v1/env.hpp>
  #include <boost/process/v1/handles.hpp>
  #include <boost/process/v1/io.hpp>
  #include <boost/process/v1/start_dir.hpp>
  #include "platform/linux/cage_display_router.h"
  #include <dirent.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace browser_stream {
  namespace {
    struct token_record_t {
      std::string remote_address;
      std::string app_uuid;
      std::chrono::steady_clock::time_point expires_at;
      bool consumed = false;
      bool owns_app = false;
    };

    std::mutex token_mutex;
    std::unordered_map<std::string, token_record_t> session_tokens;

    constexpr auto token_ttl = std::chrono::minutes(2);

    struct stream_profile_t {
      std::string_view id;
      std::string_view label;
      std::string_view description;
      int width;
      int height;
      int fps;
      int bitrate_kbps;
    };

    constexpr std::array stream_profiles {
      stream_profile_t {
        "low_latency",
        "Low Latency",
        "Keeps decoder queues tight for direct touch and mouse feel.",
        1280,
        720,
        60,
        8000,
      },
      stream_profile_t {
        "balanced",
        "Balanced",
        "Keeps the current quality target with a little more jitter margin.",
        1280,
        720,
        60,
        10000,
      },
      stream_profile_t {
        "battery_saver",
        "Battery Saver",
        "Reduces decode work and network use for handheld browser sessions.",
        960,
        540,
        30,
        4500,
      },
    };

    const stream_profile_t &stream_profile_by_id(std::string_view profile_id) {
      for (const auto &profile : stream_profiles) {
        if (profile.id == profile_id) {
          return profile;
        }
      }
      return stream_profiles[1];
    }

    nlohmann::json stream_profile_json(const stream_profile_t &profile) {
      return {
        {"id", std::string {profile.id}},
        {"label", std::string {profile.label}},
        {"description", std::string {profile.description}},
        {"video", {
          {"width", profile.width},
          {"height", profile.height},
          {"fps", profile.fps},
          {"bitrate_kbps", profile.bitrate_kbps},
        }},
      };
    }

    nlohmann::json stream_profiles_json() {
      auto output = nlohmann::json::array();
      for (const auto &profile : stream_profiles) {
        output.push_back(stream_profile_json(profile));
      }
      return output;
    }

#ifdef __linux__
    struct helper_state_t {
      boost::process::v1::child process;
      std::string ipc_socket;
      std::string ipc_token;
      std::string cert_hash;
    };

    std::mutex helper_mutex;
    helper_state_t helper_state;
    std::thread input_ipc_thread;
    std::atomic_bool input_ipc_running {false};
    std::string input_ipc_socket;
    std::string input_ipc_token;

    struct capture_state_t {
      safe::mail_t mail;
      std::shared_ptr<input::input_t> input;
      std::string session_token;
      video::packet_queue_t video_packets;
      audio::packet_queue_t audio_packets;
      std::thread video_capture_thread;
      std::thread video_packet_thread;
      std::thread audio_capture_thread;
      std::thread audio_packet_thread;
      std::atomic_bool running {false};
      std::atomic_bool audio_running {false};
    };

    std::mutex capture_mutex;
    std::unique_ptr<capture_state_t> capture_state;
    std::atomic_uint32_t media_sequence {0};

    void stop_input_ipc_server_locked();
#endif

    void prune_tokens_locked(std::chrono::steady_clock::time_point now) {
      for (auto it = session_tokens.begin(); it != session_tokens.end();) {
        if (it->second.consumed || it->second.expires_at <= now) {
          it = session_tokens.erase(it);
        } else {
          ++it;
        }
      }
    }

    std::optional<token_record_t> take_session_token(std::string_view token) {
      std::lock_guard lock(token_mutex);
      const auto it = session_tokens.find(std::string {token});
      if (it == session_tokens.end()) {
        return std::nullopt;
      }

      auto record = std::move(it->second);
      session_tokens.erase(it);
      return record;
    }

    bool erase_session_token(std::string_view token) {
      return take_session_token(token).has_value();
    }

    nlohmann::json codec_json() {
      return {
        {"video", {
          {"id", "h264"},
          {"webcodecs", "avc1.640020"},
          {"label", "H.264 SDR 8-bit"}
        }},
        {"audio", {
          {"id", "opus"},
          {"webcodecs", "opus"},
          {"label", "Opus"}
        }}
      };
    }

    std::string normalize_host(std::string_view host) {
      std::string normalized {host};
      if (!normalized.empty() && normalized.front() == '[') {
        const auto bracket = normalized.find(']');
        if (bracket != std::string::npos) {
          normalized.resize(bracket + 1);
        }
      } else {
        const auto colon = normalized.find(':');
        if (colon != std::string::npos) {
          normalized.resize(colon);
        }
      }
      if (normalized.empty()) {
        normalized = "127.0.0.1";
      }
      return normalized;
    }

    std::string webtransport_url(std::string_view host, std::string_view token) {
      return "https://"s + normalize_host(host) + ":"s + std::to_string(default_webtransport_port) +
             "/browser-stream?token="s + std::string {token};
    }

#ifdef __linux__
    std::optional<proc::ctx_t> find_app_by_uuid(std::string_view app_uuid) {
      for (const auto &app : proc::proc.get_apps()) {
        if (app.uuid == app_uuid) {
          return app;
        }
      }
      return std::nullopt;
    }

    std::string ascii_lower(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return value;
    }

    bool text_contains_steam_launch(std::string_view text) {
      auto lower = ascii_lower(std::string {text});
      return lower.find("steam://") != std::string::npos ||
             lower.find("-applaunch") != std::string::npos ||
             lower.find("steam -gamepadui") != std::string::npos;
    }

    bool app_uses_steam(const proc::ctx_t &app) {
      if (!app.steam_appid.empty() || ascii_lower(app.source) == "steam") {
        return true;
      }

      if (text_contains_steam_launch(app.cmd)) {
        return true;
      }

      return std::any_of(app.detached.begin(), app.detached.end(), text_contains_steam_launch);
    }

    bool is_proc_pid_dir(std::string_view name) {
      return !name.empty() && std::all_of(name.begin(), name.end(), [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch));
      });
    }

    std::string read_proc_file(pid_t pid, std::string_view name) {
      std::ifstream file("/proc/" + std::to_string(pid) + "/" + std::string {name}, std::ios::binary);
      if (!file) {
        return {};
      }

      std::ostringstream buffer;
      buffer << file.rdbuf();
      return buffer.str();
    }

    std::string argv0_basename(std::string_view cmdline) {
      const auto end = cmdline.find(' ');
      auto argv0 = std::string {cmdline.substr(0, end)};
      if (const auto slash = argv0.find_last_of('/'); slash != std::string::npos) {
        argv0 = argv0.substr(slash + 1);
      }
      return ascii_lower(argv0);
    }

    std::optional<std::string> running_steam_client_summary() {
      DIR *dir = opendir("/proc");
      if (!dir) {
        return std::nullopt;
      }

      const auto this_pid = getpid();
      while (auto *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (!is_proc_pid_dir(name)) {
          continue;
        }

        const auto pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
        if (pid <= 1 || pid == this_pid) {
          continue;
        }

        auto comm = ascii_lower(read_proc_file(pid, "comm"));
        while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r')) {
          comm.pop_back();
        }

        auto cmdline = read_proc_file(pid, "cmdline");
        std::replace(cmdline.begin(), cmdline.end(), '\0', ' ');
        auto lower_cmdline = ascii_lower(cmdline);
        auto argv0 = argv0_basename(cmdline);

        const bool steam_client =
          comm == "steam" ||
          comm == "steamwebhelper" ||
          argv0 == "steam" ||
          argv0 == "steam.sh" ||
          lower_cmdline.find("/steam.sh") != std::string::npos ||
          lower_cmdline.find("/steamwebhelper") != std::string::npos ||
          lower_cmdline.find("/ubuntu12_32/steam") != std::string::npos;
        if (!steam_client) {
          continue;
        }

        closedir(dir);
        auto summary = cmdline.empty() ? comm : cmdline;
        if (summary.size() > 160) {
          summary.resize(157);
          summary += "...";
        }
        return std::to_string(pid) + ": " + summary;
      }

      closedir(dir);
      return std::nullopt;
    }

    std::shared_ptr<rtsp_stream::launch_session_t> browser_launch_session() {
      crypto::named_cert_t named_cert {
        .name = "Browser Stream",
        .uuid = http::unique_id,
        .perm = crypto::PERM::_all,
      };

      nvhttp::args_t args;
      args.emplace("mode", "1280x720x60");
      args.emplace("sops", "0");
      args.emplace("virtualDisplay", "0");
      args.emplace("displayModeExplicit", "1");

      auto launch_session = nvhttp::make_launch_session(true, false, args, &named_cert);
      launch_session->virtual_display = false;
      launch_session->user_locked_virtual_display = true;
      return launch_session;
    }

    bool launch_isolated_app(const proc::ctx_t &app, std::string &error_out) {
      if (!config::video.linux_display.use_cage_compositor) {
        error_out = "Browser Stream app isolation requires the Linux private compositor runtime to be enabled.";
        return false;
      }

      if (app_uses_steam(app)) {
        if (auto running_steam = running_steam_client_summary()) {
          error_out = "Steam is already running on the host desktop. Quit Steam first, then start Browser Stream so Steam launches inside Headless Stream instead of reusing the host instance.";
          BOOST_LOG(warning) << "Browser Stream refused isolated Steam launch because a host Steam client is already running ["
                             << *running_steam << ']';
          return false;
        }
      }

      BOOST_LOG(info) << "Launching Browser Stream app session ["sv << app.name << ']';
      auto launch_session = browser_launch_session();
      const auto err = proc::proc.execute(app, launch_session);
      if (err) {
        error_out = err == 503 ?
          "Browser Stream could not initialize the isolated capture runtime." :
          "Browser Stream could not start the selected application.";
        return false;
      }

      if (!cage_display_router::is_running()) {
        error_out = "Browser Stream started the application, but the isolated compositor did not remain active.";
        proc::proc.terminate(false, false);
        return false;
      }

      return true;
    }

    bool helper_running_locked() {
      return helper_state.process.valid() &&
             helper_state.process.running() &&
             !helper_state.cert_hash.empty() &&
             input_ipc_running.load(std::memory_order_relaxed);
    }

    void stop_helper_locked() {
      if (helper_state.process.valid() && helper_state.process.running()) {
        helper_state.process.terminate();
      }
      stop_input_ipc_server_locked();
      helper_state = {};
    }

    std::optional<std::filesystem::path> helper_binary_path() {
      if (const char *override_path = std::getenv("POLARIS_BROWSER_STREAM_HELPER")) {
        std::filesystem::path path {override_path};
        if (std::filesystem::exists(path)) {
          return path;
        }
      }

      const std::array candidates {
        std::filesystem::path {POLARIS_ASSETS_DIR} / "polaris-browser-stream-helper",
        std::filesystem::current_path() / "polaris-browser-stream-helper",
        std::filesystem::current_path() / "build" / "polaris-browser-stream-helper",
      };
      for (const auto &candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
          return candidate;
        }
      }

      auto search_path = boost::process::v1::search_path("polaris-browser-stream-helper");
      if (!search_path.empty()) {
        return std::filesystem::path {search_path.string()};
      }
      return std::nullopt;
    }

    std::string read_text_file(const std::filesystem::path &path) {
      std::ifstream input(path);
      std::string value;
      std::getline(input, value);
      return value;
    }

    std::string base64_encode(const std::vector<std::uint8_t> &data) {
      static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

      std::string out;
      out.reserve(((data.size() + 2) / 3) * 4);

      std::size_t i = 0;
      while (i + 3 <= data.size()) {
        const std::uint32_t value =
          (static_cast<std::uint32_t>(data[i]) << 16) |
          (static_cast<std::uint32_t>(data[i + 1]) << 8) |
          static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(table[(value >> 18) & 0x3f]);
        out.push_back(table[(value >> 12) & 0x3f]);
        out.push_back(table[(value >> 6) & 0x3f]);
        out.push_back(table[value & 0x3f]);
        i += 3;
      }

      const auto remaining = data.size() - i;
      if (remaining == 1) {
        const std::uint32_t value = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(table[(value >> 18) & 0x3f]);
        out.push_back(table[(value >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
      } else if (remaining == 2) {
        const std::uint32_t value =
          (static_cast<std::uint32_t>(data[i]) << 16) |
          (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(table[(value >> 18) & 0x3f]);
        out.push_back(table[(value >> 12) & 0x3f]);
        out.push_back(table[(value >> 6) & 0x3f]);
        out.push_back('=');
      }

      return out;
    }

    void replace_into(std::vector<std::uint8_t> &result, std::string_view original, std::string_view old, std::string_view replacement) {
      if (old.empty()) {
        result.assign(original.begin(), original.end());
        return;
      }

      const auto match = std::search(original.begin(), original.end(), old.begin(), old.end());
      if (match == original.end()) {
        result.assign(original.begin(), original.end());
        return;
      }

      const auto prefix_len = static_cast<std::size_t>(match - original.begin());
      const auto suffix_start = match + old.size();
      const auto suffix_len = static_cast<std::size_t>(original.end() - suffix_start);

      result.resize(prefix_len + replacement.size() + suffix_len);
      std::memcpy(result.data(), original.data(), prefix_len);
      std::memcpy(result.data() + prefix_len, replacement.data(), replacement.size());
      std::memcpy(result.data() + prefix_len + replacement.size(), suffix_start, suffix_len);
    }

    bool send_ipc_message(const nlohmann::json &message, std::string &error) {
      try {
        boost::asio::io_context io;
        boost::asio::local::stream_protocol::socket socket(io);
        socket.connect(boost::asio::local::stream_protocol::endpoint(helper_state.ipc_socket));

        const auto payload = message.dump();
        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
          error = "Browser Stream IPC payload is too large";
          return false;
        }

        std::array<std::uint8_t, 4> length {
          static_cast<std::uint8_t>((payload.size() >> 24) & 0xff),
          static_cast<std::uint8_t>((payload.size() >> 16) & 0xff),
          static_cast<std::uint8_t>((payload.size() >> 8) & 0xff),
          static_cast<std::uint8_t>(payload.size() & 0xff),
        };
        boost::asio::write(socket, boost::asio::buffer(length));
        boost::asio::write(socket, boost::asio::buffer(payload));

        std::array<std::uint8_t, 4> response_length {};
        boost::asio::read(socket, boost::asio::buffer(response_length));
        const auto response_size =
          (static_cast<std::uint32_t>(response_length[0]) << 24) |
          (static_cast<std::uint32_t>(response_length[1]) << 16) |
          (static_cast<std::uint32_t>(response_length[2]) << 8) |
          static_cast<std::uint32_t>(response_length[3]);

        std::string response_payload(response_size, '\0');
        boost::asio::read(socket, boost::asio::buffer(response_payload));
        const auto response = nlohmann::json::parse(response_payload);
        if (response.value("status", "") != "ok") {
          error = response.value("error", "Browser Stream helper rejected the IPC message");
          return false;
        }
        return true;
      } catch (const std::exception &e) {
        error = e.what();
        return false;
      }
    }

    std::optional<nlohmann::json> read_framed_json(boost::asio::local::stream_protocol::socket &socket, std::string &error) {
      try {
        std::array<std::uint8_t, 4> length {};
        boost::asio::read(socket, boost::asio::buffer(length));
        const auto payload_size =
          (static_cast<std::uint32_t>(length[0]) << 24) |
          (static_cast<std::uint32_t>(length[1]) << 16) |
          (static_cast<std::uint32_t>(length[2]) << 8) |
          static_cast<std::uint32_t>(length[3]);
        if (payload_size == 0 || payload_size > 1024 * 1024) {
          error = "Browser Stream input IPC payload has an invalid size";
          return std::nullopt;
        }

        std::string payload(payload_size, '\0');
        boost::asio::read(socket, boost::asio::buffer(payload));
        return nlohmann::json::parse(payload);
      } catch (const std::exception &e) {
        error = e.what();
        return std::nullopt;
      }
    }

    void write_framed_json(boost::asio::local::stream_protocol::socket &socket, const nlohmann::json &message) {
      const auto payload = message.dump();
      std::array<std::uint8_t, 4> length {
        static_cast<std::uint8_t>((payload.size() >> 24) & 0xff),
        static_cast<std::uint8_t>((payload.size() >> 16) & 0xff),
        static_cast<std::uint8_t>((payload.size() >> 8) & 0xff),
        static_cast<std::uint8_t>(payload.size() & 0xff),
      };
      boost::asio::write(socket, boost::asio::buffer(length));
      boost::asio::write(socket, boost::asio::buffer(payload));
    }

    void write_input_ipc_response(boost::asio::local::stream_protocol::socket &socket, bool ok, const std::string &error) {
      nlohmann::json response {
        {"status", ok ? "ok" : "error"},
        {"error", error},
      };
      try {
        write_framed_json(socket, response);
      } catch (const std::exception &e) {
        BOOST_LOG(debug) << "Browser Stream input IPC response failed: "sv << e.what();
      }
    }

    void handle_input_ipc_socket(boost::asio::local::stream_protocol::socket socket, std::string expected_token) {
      std::string error;
      const auto message = read_framed_json(socket, error);
      if (!message) {
        if (!error.empty()) {
          BOOST_LOG(debug) << "Browser Stream input IPC read failed: "sv << error;
        }
        return;
      }

      if (message->value("ipc_token", "") != expected_token) {
        write_input_ipc_response(socket, false, "invalid IPC token");
        return;
      }

      if (message->value("type", "") != "input_events") {
        write_input_ipc_response(socket, false, "unknown input IPC message type");
        return;
      }

      const auto session_token = message->value("session_token", "");
      const auto events = message->contains("events") ? message->at("events") : nlohmann::json::array();
      const auto result = submit_input(session_token, events);
      if (!result.value("status", false)) {
        write_input_ipc_response(socket, false, result.value("error", "Browser Stream input was rejected"));
        return;
      }

      write_input_ipc_response(socket, true, "");
    }

    void input_ipc_server_loop(std::string socket_path, std::string expected_token, std::promise<std::string> ready) {
      bool ready_set = false;
      try {
        std::filesystem::remove(socket_path);

        boost::asio::io_context io;
        boost::asio::local::stream_protocol::acceptor acceptor(io);
        acceptor.open();
        acceptor.bind(boost::asio::local::stream_protocol::endpoint(socket_path));
        acceptor.listen();
        ::chmod(socket_path.c_str(), 0600);

        ready.set_value("");
        ready_set = true;

        while (input_ipc_running.load(std::memory_order_relaxed)) {
          boost::asio::local::stream_protocol::socket socket(io);
          boost::system::error_code ec;
          acceptor.accept(socket, ec);
          if (ec || !input_ipc_running.load(std::memory_order_relaxed)) {
            break;
          }
          std::thread {handle_input_ipc_socket, std::move(socket), expected_token}.detach();
        }

        acceptor.close();
      } catch (const std::exception &e) {
        if (!ready_set) {
          ready.set_value(e.what());
        } else {
          BOOST_LOG(warning) << "Browser Stream input IPC server stopped: "sv << e.what();
        }
      }

      std::filesystem::remove(socket_path);
    }

    void stop_input_ipc_server_locked() {
      input_ipc_running = false;
      if (!input_ipc_socket.empty()) {
        try {
          boost::asio::io_context io;
          boost::asio::local::stream_protocol::socket socket(io);
          socket.connect(boost::asio::local::stream_protocol::endpoint(input_ipc_socket));
        } catch (...) {
        }
      }
      if (input_ipc_thread.joinable()) {
        input_ipc_thread.join();
      }
      if (!input_ipc_socket.empty()) {
        std::filesystem::remove(input_ipc_socket);
      }
      input_ipc_socket.clear();
      input_ipc_token.clear();
    }

    bool start_input_ipc_server_locked(const std::string &socket_path, const std::string &token, std::string &error) {
      stop_input_ipc_server_locked();
      input_ipc_socket = socket_path;
      input_ipc_token = token;
      input_ipc_running = true;

      std::promise<std::string> ready;
      auto future = ready.get_future();
      input_ipc_thread = std::thread {input_ipc_server_loop, input_ipc_socket, input_ipc_token, std::move(ready)};
      if (future.wait_for(2s) != std::future_status::ready) {
        error = "Browser Stream input IPC server did not become ready before timeout";
        stop_input_ipc_server_locked();
        return false;
      }

      error = future.get();
      if (!error.empty()) {
        stop_input_ipc_server_locked();
        return false;
      }
      return true;
    }

    bool ensure_helper_running_locked(std::string_view host, std::string &error) {
      if (helper_running_locked()) {
        return true;
      }

      stop_helper_locked();

      const auto helper_path = helper_binary_path();
      if (!helper_path) {
        error = "Browser Stream helper binary was not found";
        return false;
      }

      const auto nonce = crypto::rand_alphabet(12, "abcdefghijklmnopqrstuvwxyz0123456789"sv);
      const auto base_path = std::filesystem::temp_directory_path() /
                             ("polaris-browser-stream-"s + std::to_string(::getpid()) + "-"s + nonce);
      helper_state.ipc_socket = base_path.string() + ".sock";
      helper_state.ipc_token = crypto::rand_alphabet(48, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"sv);
      const auto cert_hash_path = base_path.string() + ".sha256";
      const auto input_socket_path = base_path.string() + ".input.sock";
      const auto input_token = crypto::rand_alphabet(48, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"sv);
      const auto certificate_hosts = normalize_host(host);

      if (!start_input_ipc_server_locked(input_socket_path, input_token, error)) {
        return false;
      }

      const auto addr = ":"s + std::to_string(default_webtransport_port);
      auto env = proc::proc.get_env();
      auto working_dir = boost::filesystem::path(helper_path->parent_path().string());
      std::error_code ec;
      helper_state.process = boost::process::v1::child(
        helper_path->string(),
        "-addr", addr,
        "-hosts", certificate_hosts,
        "-ipc-socket", helper_state.ipc_socket,
        "-ipc-token", helper_state.ipc_token,
        "-input-ipc-socket", input_ipc_socket,
        "-input-ipc-token", input_ipc_token,
        "-cert-hash-file", cert_hash_path,
        env,
        boost::process::v1::start_dir(working_dir),
        boost::process::v1::std_in < boost::process::v1::null,
        boost::process::v1::std_out > boost::process::v1::null,
        boost::process::v1::std_err > boost::process::v1::null,
        boost::process::v1::limit_handles,
        ec
      );
      if (ec || !helper_state.process.valid()) {
        error = ec ? ec.message() : "Browser Stream helper process could not be started";
        stop_input_ipc_server_locked();
        return false;
      }

      for (int i = 0; i < 30; ++i) {
        if (!helper_state.process.running()) {
          error = "Browser Stream helper exited before becoming ready";
          stop_input_ipc_server_locked();
          return false;
        }

        if (std::filesystem::exists(cert_hash_path) && std::filesystem::exists(helper_state.ipc_socket)) {
          auto cert_hash = read_text_file(cert_hash_path);
          std::filesystem::remove(cert_hash_path);
          if (cert_hash.size() == 64) {
            helper_state.cert_hash = std::move(cert_hash);
            return true;
          }
        }

        std::this_thread::sleep_for(100ms);
      }

      error = "Browser Stream helper did not become ready before timeout";
      stop_input_ipc_server_locked();
      return false;
    }

    video::config_t browser_video_config(const stream_profile_t &profile) {
      return video::config_t {
        profile.width,  // width
        profile.height,  // height
        profile.fps,  // framerate
        profile.bitrate_kbps,  // bitrate kbps
        1,  // slicesPerFrame
        1,  // numRefFrames
        2,  // limited-range BT.709 SDR
        0,  // H.264
        0,  // SDR 8-bit
        0,  // 4:2:0
        0,  // intra refresh disabled
        profile.fps * 1000,  // encodingFramerate
        false,  // input_only
      };
    }

    audio::config_t browser_audio_config() {
      audio::config_t config {};
      config.packetDuration = 10;
      config.channels = 2;
      config.mask = 0x3;
      config.flags[audio::config_t::HOST_AUDIO] = false;
      config.input_only = false;
      return config;
    }

    input::touch_port_t browser_default_touch_port(const stream_profile_t &profile) {
      return input::touch_port_t {
        {0, 0, profile.width, profile.height},
        profile.width,
        profile.height,
        0.0f,
        0.0f,
        1.0f,
      };
    }

    template<typename Packet>
    std::vector<std::uint8_t> packet_bytes(const Packet &packet) {
      std::vector<std::uint8_t> bytes(sizeof(Packet));
      std::memcpy(bytes.data(), &packet, sizeof(Packet));
      return bytes;
    }

    void fill_input_header(NV_INPUT_HEADER &header, std::uint32_t magic, std::size_t packet_size) {
      header.size = util::endian::big(static_cast<std::uint32_t>(packet_size - sizeof(header.size)));
      header.magic = util::endian::little(magic);
    }

    void store_netfloat(netfloat target, float value) {
      const auto little = util::endian::little(value);
      std::memcpy(target, &little, sizeof(little));
    }

    std::int16_t clamp_packet_coord(double value) {
      return static_cast<std::int16_t>(std::clamp(std::lround(value), -32768l, 32767l));
    }

    std::int16_t clamp_packet_delta(double value) {
      return static_cast<std::int16_t>(std::clamp(std::lround(value), -32768l, 32767l));
    }

    std::uint8_t mouse_button_from_event(const nlohmann::json &event) {
      const auto button = event.value("button", static_cast<int>(BUTTON_LEFT));
      switch (button) {
        case BUTTON_MIDDLE:
        case BUTTON_RIGHT:
        case BUTTON_X1:
        case BUTTON_X2:
          return static_cast<std::uint8_t>(button);
        case BUTTON_LEFT:
        default:
          return BUTTON_LEFT;
      }
    }

    void queue_input_packet(std::shared_ptr<input::input_t> &input_ctx, std::vector<std::uint8_t> &&packet) {
      input::passthrough(input_ctx, std::move(packet), crypto::PERM::_all_inputs);
    }

    void queue_abs_mouse(
      std::shared_ptr<input::input_t> &input_ctx,
      double x,
      double y,
      double width,
      double height
    ) {
      NV_ABS_MOUSE_MOVE_PACKET packet {};
      fill_input_header(packet.header, MOUSE_MOVE_ABS_MAGIC, sizeof(packet));
      packet.x = util::endian::big(clamp_packet_coord(x));
      packet.y = util::endian::big(clamp_packet_coord(y));
      packet.width = util::endian::big(clamp_packet_coord(std::max(width, 1.0)));
      packet.height = util::endian::big(clamp_packet_coord(std::max(height, 1.0)));
      queue_input_packet(input_ctx, packet_bytes(packet));
    }

    void queue_mouse_button(std::shared_ptr<input::input_t> &input_ctx, std::uint8_t button, bool release) {
      NV_MOUSE_BUTTON_PACKET packet {};
      fill_input_header(
        packet.header,
        release ? MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5 : MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5,
        sizeof(packet)
      );
      packet.button = button;
      queue_input_packet(input_ctx, packet_bytes(packet));
    }

    void queue_scroll(std::shared_ptr<input::input_t> &input_ctx, double delta_x, double delta_y) {
      if (std::abs(delta_y) >= 0.5) {
        NV_SCROLL_PACKET packet {};
        fill_input_header(packet.header, SCROLL_MAGIC_GEN5, sizeof(packet));
        const auto amount = util::endian::big(clamp_packet_delta(-delta_y));
        packet.scrollAmt1 = amount;
        packet.scrollAmt2 = amount;
        queue_input_packet(input_ctx, packet_bytes(packet));
      }

      if (std::abs(delta_x) >= 0.5) {
        SS_HSCROLL_PACKET packet {};
        fill_input_header(packet.header, SS_HSCROLL_MAGIC, sizeof(packet));
        packet.scrollAmount = util::endian::big(clamp_packet_delta(-delta_x));
        queue_input_packet(input_ctx, packet_bytes(packet));
      }
    }

    void queue_keyboard(std::shared_ptr<input::input_t> &input_ctx, int key_code, bool release, int modifiers) {
      NV_KEYBOARD_PACKET packet {};
      fill_input_header(packet.header, release ? KEY_UP_EVENT_MAGIC : KEY_DOWN_EVENT_MAGIC, sizeof(packet));
      packet.keyCode = static_cast<short>(std::clamp(key_code, 0, 0xff));
      packet.modifiers = static_cast<char>(std::clamp(modifiers, 0, 0xff));
      queue_input_packet(input_ctx, packet_bytes(packet));
    }

    std::uint8_t touch_event_type(std::string_view action) {
      if (action == "down") {
        return LI_TOUCH_EVENT_DOWN;
      }
      if (action == "up") {
        return LI_TOUCH_EVENT_UP;
      }
      if (action == "cancel") {
        return LI_TOUCH_EVENT_CANCEL;
      }
      return LI_TOUCH_EVENT_MOVE;
    }

    void queue_touch_event(
      std::shared_ptr<input::input_t> &input_ctx,
      const nlohmann::json &event,
      std::string_view action,
      double x,
      double y,
      double width,
      double height
    ) {
      SS_TOUCH_PACKET packet {};
      fill_input_header(packet.header, SS_TOUCH_MAGIC, sizeof(packet));
      packet.eventType = touch_event_type(action);
      packet.rotation = util::endian::little(static_cast<std::uint16_t>(LI_ROT_UNKNOWN));
      packet.pointerId = util::endian::little(static_cast<std::uint32_t>(std::max(0, event.value("pointer_id", 0))));

      const auto safe_width = std::max(width, 1.0);
      const auto safe_height = std::max(height, 1.0);
      const auto pressure = event.contains("pressure") ? event.value("pressure", 1.0) : (action == "up" ? 0.0 : 1.0);
      store_netfloat(packet.x, static_cast<float>(std::clamp(x / safe_width, 0.0, 1.0)));
      store_netfloat(packet.y, static_cast<float>(std::clamp(y / safe_height, 0.0, 1.0)));
      store_netfloat(packet.pressureOrDistance, static_cast<float>(std::clamp(pressure, 0.0, 1.0)));
      store_netfloat(packet.contactAreaMajor, 0.02f);
      store_netfloat(packet.contactAreaMinor, 0.02f);
      queue_input_packet(input_ctx, packet_bytes(packet));
    }

    std::shared_ptr<input::input_t> active_input_for_token(std::string_view token, std::string &error_out) {
      std::lock_guard lock(capture_mutex);
      if (!capture_state || !capture_state->running || !capture_state->input) {
        error_out = "Browser Stream input is not active.";
        return nullptr;
      }
      if (capture_state->session_token != token) {
        error_out = "Browser Stream input token is not active.";
        return nullptr;
      }
      return capture_state->input;
    }

    bool dispatch_input_event(std::shared_ptr<input::input_t> &input_ctx, const nlohmann::json &event, std::string &error_out) {
      const auto type = event.value<std::string>("type", "");
      if (type == "pointer") {
        const auto action = event.value<std::string>("action", "move");
        const auto width = event.value("width", 0.0);
        const auto height = event.value("height", 0.0);
        if (width <= 0.0 || height <= 0.0) {
          error_out = "Pointer input is missing surface dimensions.";
          return false;
        }

        const auto x = std::clamp(event.value("x", 0.0), 0.0, width);
        const auto y = std::clamp(event.value("y", 0.0), 0.0, height);
        queue_abs_mouse(input_ctx, x, y, width, height);

        const auto pointer_type = event.value("pointer_type", event.value<std::string>("pointerType", "mouse"));
        if (pointer_type == "touch") {
          queue_touch_event(input_ctx, event, action, x, y, width, height);
        }

        if (action == "down") {
          queue_mouse_button(input_ctx, pointer_type == "touch" ? BUTTON_LEFT : mouse_button_from_event(event), false);
        } else if (action == "up" || action == "cancel") {
          queue_mouse_button(input_ctx, pointer_type == "touch" ? BUTTON_LEFT : mouse_button_from_event(event), true);
        }
        return true;
      }

      if (type == "wheel") {
        queue_scroll(input_ctx, event.value("delta_x", 0.0), event.value("delta_y", 0.0));
        return true;
      }

      if (type == "key") {
        const auto action = event.value<std::string>("action", "down");
        const auto key_code = event.value("key_code", 0);
        if (key_code <= 0 || key_code > 0xff) {
          error_out = "Keyboard input is missing a valid key code.";
          return false;
        }
        queue_keyboard(input_ctx, key_code, action == "up", event.value("modifiers", 0));
        return true;
      }

      error_out = "Unsupported Browser Stream input event type.";
      return false;
    }

    std::uint64_t timestamp_us_for_packet(const video::packet_raw_t &packet, std::chrono::steady_clock::time_point stream_start) {
      auto timestamp = packet.frame_timestamp.value_or(std::chrono::steady_clock::now());
      if (timestamp < stream_start) {
        timestamp = std::chrono::steady_clock::now();
      }
      return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(timestamp - stream_start).count()
      );
    }

    void publish_media_payload(
      protocol::media_kind kind,
      protocol::codec_id codec,
      std::uint8_t flags,
      std::uint64_t frame_id,
      std::uint64_t timestamp_us,
      std::string_view payload,
      std::string_view label
    ) {
      const auto chunk_count = (payload.size() + protocol::max_datagram_payload - 1) / protocol::max_datagram_payload;
      if (chunk_count == 0 || chunk_count > std::numeric_limits<std::uint16_t>::max()) {
        BOOST_LOG(warning) << "Dropping Browser Stream "sv << label
                           << " frame with unsupported chunk count: "sv << chunk_count;
        return;
      }

      nlohmann::json payloads = nlohmann::json::array();
      payloads.get_ref<nlohmann::json::array_t &>().reserve(chunk_count);

      for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const auto offset = chunk_index * protocol::max_datagram_payload;
        const auto size = std::min(protocol::max_datagram_payload, payload.size() - offset);
        const auto *chunk_begin = reinterpret_cast<const std::uint8_t *>(payload.data() + offset);

        protocol::media_envelope_t envelope;
        envelope.kind = kind;
        envelope.codec = codec;
        envelope.flags = flags;
        envelope.sequence = media_sequence.fetch_add(1, std::memory_order_relaxed);
        envelope.frame_id = frame_id;
        envelope.timestamp_us = timestamp_us;
        envelope.chunk_index = static_cast<std::uint16_t>(chunk_index);
        envelope.chunk_count = static_cast<std::uint16_t>(chunk_count);
        envelope.payload.assign(chunk_begin, chunk_begin + size);
        payloads.push_back(base64_encode(protocol::encode_datagram(envelope)));
      }

      std::lock_guard lock(helper_mutex);
      if (!helper_running_locked()) {
        return;
      }

      std::string error;
      nlohmann::json message {
        {"ipc_token", helper_state.ipc_token},
        {"type", "media_datagrams"},
        {"session_token", ""},
        {"payloads_b64", std::move(payloads)},
      };
      if (!send_ipc_message(message, error)) {
        BOOST_LOG(warning) << "Browser Stream media IPC send failed: "sv << error;
      }
    }

    void publish_video_packet(video::packet_raw_t &packet, std::chrono::steady_clock::time_point stream_start) {
      if (!packet.data() || packet.data_size() == 0) {
        return;
      }

      std::vector<std::uint8_t> replace_bufs[2];
      std::string_view payload {
        reinterpret_cast<const char *>(packet.data()),
        packet.data_size(),
      };

      if (packet.is_idr() && packet.replacements) {
        int replace_idx = 0;
        for (auto &replacement : *packet.replacements) {
          auto &buf = replace_bufs[replace_idx];
          replace_into(buf, payload, replacement.old, replacement._new);
          payload = {reinterpret_cast<const char *>(buf.data()), buf.size()};
          replace_idx ^= 1;
        }
      }

      const auto frame_index = packet.frame_index();
      publish_media_payload(
        protocol::media_kind::video,
        protocol::codec_id::h264,
        packet.is_idr() ? protocol::keyframe : 0,
        frame_index < 0 ? 0 : static_cast<std::uint64_t>(frame_index),
        timestamp_us_for_packet(packet, stream_start),
        payload,
        "video"
      );
    }

    void publish_audio_packet(const audio::buffer_t &packet, std::uint64_t frame_id, std::uint64_t timestamp_us) {
      if (packet.size() == 0) {
        return;
      }

      publish_media_payload(
        protocol::media_kind::audio,
        protocol::codec_id::opus,
        0,
        frame_id,
        timestamp_us,
        std::string_view {reinterpret_cast<const char *>(packet.begin()), packet.size()},
        "audio"
      );
    }

    std::unique_ptr<capture_state_t> take_capture_state_for_stop() {
      std::unique_ptr<capture_state_t> state;
      {
        std::lock_guard lock(capture_mutex);
        state = std::move(capture_state);
      }
      if (!state) {
        return nullptr;
      }

      state->running = false;
      if (state->mail) {
        state->mail->event<bool>(mail::shutdown)->raise(true);
      }
      if (state->video_packets) {
        state->video_packets->stop();
      }
      if (state->audio_packets) {
        state->audio_packets->stop();
      }
      return state;
    }

    void finish_video_capture_stop(std::unique_ptr<capture_state_t> state) {
      if (!state) {
        return;
      }
      if (state->input) {
        input::reset(state->input);
      }
      if (state->video_capture_thread.joinable()) {
        state->video_capture_thread.join();
      }
      if (state->audio_capture_thread.joinable()) {
        state->audio_capture_thread.join();
      }
      if (state->video_packets) {
        state->video_packets->stop();
      }
      if (state->audio_packets) {
        state->audio_packets->stop();
      }
      if (state->video_packet_thread.joinable()) {
        state->video_packet_thread.join();
      }
      if (state->audio_packet_thread.joinable()) {
        state->audio_packet_thread.join();
      }
    }

    void stop_video_capture() {
      finish_video_capture_stop(take_capture_state_for_stop());
    }

    void stop_video_capture_async() {
      auto state = take_capture_state_for_stop();
      if (!state) {
        return;
      }
      std::thread {[state = std::move(state)]() mutable {
        finish_video_capture_stop(std::move(state));
      }}.detach();
    }

    bool video_capture_active() {
      std::lock_guard lock(capture_mutex);
      return capture_state && capture_state->running;
    }

    bool audio_capture_active() {
      std::lock_guard lock(capture_mutex);
      return capture_state && capture_state->audio_running;
    }

    bool start_video_capture_if_needed(
      std::string_view session_token,
      const stream_profile_t &profile,
      std::string &error_out
    ) {
      if (video_capture_active()) {
        return true;
      }

      stop_video_capture();

      if (video::active_encoder_name().empty() && video::probe_encoders(false, false)) {
        error_out = "Browser Stream could not find a usable H.264 encoder";
        return false;
      }

      auto state = std::make_unique<capture_state_t>();
      state->mail = std::make_shared<safe::mail_raw_t>();
      state->input = input::alloc(state->mail);
      state->session_token = std::string {session_token};
      state->video_packets = state->mail->queue<video::packet_t>(mail::video_packets);
      state->audio_packets = state->mail->queue<audio::packet_t>(mail::audio_packets);
      state->running = true;
      state->mail->event<input::touch_port_t>(mail::touch_port)->raise(browser_default_touch_port(profile));

      auto *state_ptr = state.get();
      const auto video_config = browser_video_config(profile);
      const auto audio_config = browser_audio_config();
      state->video_packet_thread = std::thread {[state_ptr]() {
        const auto stream_start = std::chrono::steady_clock::now();
        auto next_idr_request = stream_start + 2s;
        auto idr_events = state_ptr->mail->event<bool>(mail::idr);
        while (auto packet = state_ptr->video_packets->pop()) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= next_idr_request) {
            idr_events->raise(true);
            next_idr_request = now + 2s;
          }
          publish_video_packet(*packet, stream_start);
        }
      }};

      state->audio_packet_thread = std::thread {[state_ptr, packet_duration_us = static_cast<std::uint64_t>(audio_config.packetDuration) * 1000]() {
        std::uint64_t frame_id = 0;
        std::uint64_t timestamp_us = 0;
        while (auto packet = state_ptr->audio_packets->pop()) {
          const auto &packet_data = packet->second;
          publish_audio_packet(packet_data, frame_id++, timestamp_us);
          timestamp_us += packet_duration_us;
        }
      }};

      state->video_capture_thread = std::thread {[state_ptr, video_config]() {
        BOOST_LOG(info) << "Starting Browser Stream H.264 video capture"sv;
        try {
          video::capture(state_ptr->mail, video_config, nullptr, state_ptr->video_packets);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Browser Stream video capture failed: "sv << e.what();
        }
        state_ptr->running = false;
        state_ptr->video_packets->stop();
        BOOST_LOG(info) << "Browser Stream H.264 video capture stopped"sv;
      }};

      state->audio_capture_thread = std::thread {[state_ptr, audio_config]() {
        BOOST_LOG(info) << "Starting Browser Stream Opus audio capture"sv;
        state_ptr->audio_running = true;
        try {
          audio::capture(state_ptr->mail, audio_config, nullptr, state_ptr->audio_packets);
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Browser Stream audio capture failed: "sv << e.what();
        }
        state_ptr->audio_running = false;
        state_ptr->audio_packets->stop();
        BOOST_LOG(info) << "Browser Stream Opus audio capture stopped"sv;
      }};

      std::lock_guard lock(capture_mutex);
      if (capture_state && capture_state->running) {
        state->mail->event<bool>(mail::shutdown)->raise(true);
        state->video_packets->stop();
        state->audio_packets->stop();
        if (state->video_capture_thread.joinable()) {
          state->video_capture_thread.join();
        }
        if (state->audio_capture_thread.joinable()) {
          state->audio_capture_thread.join();
        }
        if (state->video_packet_thread.joinable()) {
          state->video_packet_thread.join();
        }
        if (state->audio_packet_thread.joinable()) {
          state->audio_packet_thread.join();
        }
        return true;
      }

      capture_state = std::move(state);
      return true;
    }
#endif
  }  // namespace

  bool build_enabled() {
#ifdef POLARIS_ENABLE_BROWSER_STREAM
    return true;
#else
    return false;
#endif
  }

  session_token_t issue_session_token(std::string_view remote_address, std::string_view app_uuid, bool owns_app) {
    const auto now = std::chrono::steady_clock::now();
    const auto expires_at = now + token_ttl;
    auto token = crypto::rand_alphabet(48, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"sv);

    std::lock_guard lock(token_mutex);
    prune_tokens_locked(now);
    session_tokens[token] = token_record_t {
      .remote_address = std::string {remote_address},
      .app_uuid = std::string {app_uuid},
      .expires_at = expires_at,
      .consumed = false,
      .owns_app = owns_app,
    };
    return {std::move(token), expires_at};
  }

  bool consume_session_token(std::string_view token, std::string_view remote_address) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(token_mutex);
    prune_tokens_locked(now);

    const auto it = session_tokens.find(std::string {token});
    if (it == session_tokens.end()) {
      return false;
    }

    const bool valid = !it->second.consumed &&
                       it->second.expires_at > now &&
                       it->second.remote_address == remote_address;
    session_tokens.erase(it);
    return valid;
  }

  bool stop_session(std::string_view token) {
    auto record = take_session_token(token);
    bool stopped = record.has_value();
#ifdef __linux__
    bool should_stop_capture = false;
    bool should_stop_app = record && record->owns_app;
    {
      std::lock_guard lock(helper_mutex);
      if (stopped && helper_running_locked()) {
        std::string error;
        nlohmann::json message {
          {"ipc_token", helper_state.ipc_token},
          {"type", "stop_session"},
          {"session_token", std::string {token}},
        };
        send_ipc_message(message, error);
        should_stop_capture = true;
        stop_helper_locked();
      }
    }
    if (should_stop_capture) {
      stop_video_capture_async();
    }
    if (should_stop_app) {
      proc::proc.terminate(false, false);
    }
#endif
    return stopped;
  }

  nlohmann::json status_json() {
#ifdef __linux__
    const auto helper_ready = [&]() {
      std::lock_guard lock(helper_mutex);
      return helper_running_locked();
    }();
    const auto media_active = video_capture_active();
    const auto audio_active = audio_capture_active();
#else
    constexpr bool helper_ready = false;
    constexpr bool media_active = false;
    constexpr bool audio_active = false;
#endif

    nlohmann::json output;
    output["status"] = true;
    output["feature"] = "browser_stream";
    output["display_name"] = "Browser Stream";
    output["build_enabled"] = build_enabled();
    output["config_enabled"] = config::video.browser_streaming;
    output["available"] = build_enabled() && config::video.browser_streaming && helper_ready;
    output["state"] =
      !build_enabled() ? "not_built" :
      !config::video.browser_streaming ? "disabled" :
      helper_ready ? "transport_ready" :
      "backend_pending";
    output["message"] =
      !build_enabled() ?
        "This Polaris build was not compiled with Browser Stream support." :
      !config::video.browser_streaming ?
        "Browser Stream is disabled in configuration." :
      helper_ready ?
        "Browser Stream WebTransport helper is running." :
        "Browser Stream is configured, but WebTransport media fanout is not active yet.";
    output["transport"] = {
      {"protocol", "WebTransport"},
      {"port", default_webtransport_port},
      {"lan_only", true},
      {"wan_supported", false},
      {"helper_running", helper_ready},
    };
    output["media"] = {
      {"video_capture_active", media_active},
      {"video_codec", "h264"},
      {"audio_capture_active", audio_active},
      {"audio_codec", "opus"},
    };
    output["session"] = {
      {"app_id", proc::proc.running()},
      {"app_name", proc::proc.get_last_run_app_name()},
      {"app_uuid", proc::proc.get_running_app_uuid()},
#ifdef __linux__
      {"isolated", cage_display_router::is_running()},
      {"runtime", cage_display_router::is_running() ? "labwc" : ""},
#else
      {"isolated", false},
      {"runtime", ""},
#endif
    };
#ifdef __linux__
    output["isolation"] = {
      {"backend", "labwc"},
      {"available", config::video.linux_display.use_cage_compositor},
      {"active", cage_display_router::is_running()},
      {"headless", cage_display_router::runtime_state().effective_headless},
      {"wayland_socket", cage_display_router::get_wayland_socket()},
    };
#endif
    output["codecs"] = codec_json();
    output["profiles"] = stream_profiles_json();
    output["default_profile"] = "balanced";
    output["input"] = {
      {"keyboard", true},
      {"pointer", true},
      {"wheel", true},
      {"touch", true},
      {"gamepad", false},
      {"gamepad_reserved", true},
    };
    output["legacy_aliases"] = {"/webrtc", "/api/webrtc/status", "webrtc_browser_streaming", "POLARIS_ENABLE_WEBRTC"};
    return output;
  }

  nlohmann::json submit_input(std::string_view token, const nlohmann::json &events) {
    nlohmann::json output;
#ifdef __linux__
    if (token.empty()) {
      output["status"] = false;
      output["error"] = "Browser Stream input requires an active session token.";
      return output;
    }

    std::string error;
    auto input_ctx = active_input_for_token(token, error);
    if (!input_ctx) {
      output["status"] = false;
      output["error"] = error;
      return output;
    }

    const auto *event_array = events.is_array() ? &events : nullptr;
    nlohmann::json single_event_array;
    if (!event_array && events.is_object()) {
      single_event_array = nlohmann::json::array({events});
      event_array = &single_event_array;
    }

    if (!event_array) {
      output["status"] = false;
      output["error"] = "Browser Stream input payload must be an event or event array.";
      return output;
    }

    std::size_t accepted = 0;
    for (const auto &event : *event_array) {
      if (!event.is_object()) {
        continue;
      }
      if (dispatch_input_event(input_ctx, event, error)) {
        ++accepted;
      }
    }

    if (accepted == 0 && !event_array->empty()) {
      output["status"] = false;
      output["error"] = error.empty() ? "No Browser Stream input events were accepted." : error;
      output["accepted"] = 0;
      return output;
    }

    output["status"] = true;
    output["accepted"] = accepted;
#else
    output["status"] = false;
    output["error"] = "Browser Stream input is only implemented on Linux.";
#endif
    return output;
  }

  nlohmann::json create_session(
    std::string_view remote_address,
    std::string_view host,
    std::string_view app_uuid,
    std::string_view profile_id
  ) {
    nlohmann::json output;
    const auto &profile = stream_profile_by_id(profile_id);

#ifdef __linux__
    if (app_uuid.empty()) {
      output["status"] = false;
      output["error"] = "Choose an application before starting Browser Stream.";
      output["state"] = "app_required";
      return output;
    }

    if (video_capture_active()) {
      output["status"] = false;
      output["error"] = "A Browser Stream session is already active.";
      output["state"] = "session_active";
      return output;
    }

    const auto app = find_app_by_uuid(app_uuid);
    if (!app) {
      output["status"] = false;
      output["error"] = "Cannot find the selected Browser Stream application.";
      output["state"] = "app_not_found";
      return output;
    }

    std::string launch_error;
    if (!launch_isolated_app(*app, launch_error)) {
      output["status"] = false;
      output["error"] = launch_error;
      output["state"] = "app_launch_failed";
      return output;
    }

    {
      std::lock_guard lock(helper_mutex);
      std::string error;
      if (!ensure_helper_running_locked(host, error)) {
        proc::proc.terminate(false, false);
        output["status"] = false;
        output["error"] = error;
        output["state"] = "helper_unavailable";
        return output;
      }
    }
#else
    output["status"] = false;
    output["error"] = "Browser Stream helper launch is only implemented on Linux";
    output["state"] = "helper_unavailable";
    return output;
#endif

    auto token = issue_session_token(remote_address, app_uuid, true);

#ifdef __linux__
    {
      std::lock_guard lock(helper_mutex);
      std::string error;
      nlohmann::json message {
        {"ipc_token", helper_state.ipc_token},
        {"type", "register_session"},
        {"session_token", token.token},
      };
      if (!send_ipc_message(message, error)) {
        erase_session_token(token.token);
        proc::proc.terminate(false, false);
        output["status"] = false;
        output["error"] = "Browser Stream helper IPC registration failed: " + error;
        output["state"] = "helper_unavailable";
        return output;
      }
    }

    std::string capture_error;
    if (!start_video_capture_if_needed(token.token, profile, capture_error)) {
      {
        std::lock_guard lock(helper_mutex);
        std::string stop_error;
        nlohmann::json stop_message {
          {"ipc_token", helper_state.ipc_token},
          {"type", "stop_session"},
          {"session_token", token.token},
        };
        send_ipc_message(stop_message, stop_error);
      }
      erase_session_token(token.token);
      proc::proc.terminate(false, false);
      output["status"] = false;
      output["error"] = capture_error;
      output["state"] = "capture_unavailable";
      return output;
    }
#endif

    output["status"] = true;
    output["available"] = true;
    output["state"] = "transport_ready";
    output["message"] = "Browser Stream WebTransport session metadata created.";
    output["webtransport_url"] = webtransport_url(host, token.token);
    output["session_token"] = token.token;
    output["single_use_token"] = true;
    output["token_expires_in_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(token.expires_at - std::chrono::steady_clock::now()).count();
#ifdef __linux__
    {
      std::lock_guard lock(helper_mutex);
      output["cert_hash"] = helper_state.cert_hash;
      output["server_certificate_hashes"] = {
        {
          {"algorithm", "sha-256"},
          {"value", helper_state.cert_hash},
        }
      };
    }
#else
    output["cert_hash"] = nullptr;
    output["server_certificate_hashes"] = nlohmann::json::array();
#endif
    output["codecs"] = codec_json();
    output["profile"] = stream_profile_json(profile);
    output["profiles"] = stream_profiles_json();
    output["video"] = {
      {"width", profile.width},
      {"height", profile.height},
      {"fps", profile.fps},
      {"bitrate_kbps", profile.bitrate_kbps},
    };
    output["audio"] = {
      {"sample_rate", 48000},
      {"channels", 2},
      {"packet_duration_ms", 10},
      {"bitrate_kbps", 96},
    };
    output["transport"] = {
      {"protocol", "WebTransport"},
      {"port", default_webtransport_port},
      {"reliable_control_streams", true},
      {"unreliable_media_datagrams", true},
      {"lan_only", true},
    };
    output["session"] = {
      {"app_id", proc::proc.running()},
      {"app_name", proc::proc.get_last_run_app_name()},
      {"app_uuid", proc::proc.get_running_app_uuid()},
      {"isolated", true},
      {"runtime", "labwc"},
    };
#ifdef __linux__
    output["isolation"] = {
      {"backend", "labwc"},
      {"headless", cage_display_router::runtime_state().effective_headless},
      {"wayland_socket", cage_display_router::get_wayland_socket()},
    };
#endif
    output["deferred"] = {"browser_gamepad", "wan_traversal", "hdr", "hevc", "av1"};
    return output;
  }
}  // namespace browser_stream
