/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTPS server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <regex>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <numeric>
#include <algorithm>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "adaptive_bitrate.h"
#include "browser_stream.h"
#include "client_support_report.h"
#include "confighttp.h"
#include "confighttp_benchmark_auth.h"
#include "confighttp_validation.h"
#include "crash_report.h"
#include "crypto.h"
#include "display_device.h"
#include "entry_handler.h"
#include "file_handler.h"
#include "game_artwork.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "log_tail_api.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "session_event_queue.h"
#include "settings_metadata.h"
#include "stream_recorder.h"
#include "stream_stats.h"
#include "update_status.h"
#include "utility.h"
#include "video.h"
#include "web_session_store.h"
#include "uuid.h"
#include "wol.h"
#include "client_profiles.h"
#include "device_db.h"
#include "doctor_actions.h"
#include "ai_optimizer.h"
#include "game_classifier.h"
#include "game_library_scanner.h"

#include <curl/curl.h>

#ifdef _WIN32
  #include "platform/windows/utils.h"
  #include <Windows.h>
#elif __linux__
  #include "platform/linux/session_media.h"
  #include <pwd.h>
  #include <sys/stat.h>
  #include <unistd.h>
#elif __APPLE__
  #include <mach-o/dyld.h>
#endif

#ifdef __linux__
  #include "platform/linux/executable_path.h"
  #include "platform/linux/virtual_display.h"
  #include "platform/linux/session_manager.h"
  #include "platform/linux/stream_runtime.h"
  #include "platform/linux/stream_display_policy.h"
  #include "platform/linux/display_topology.h"
  #include "platform/linux/wayland.h"
#endif

using namespace std::literals;

namespace confighttp {
  namespace fs = std::filesystem;

  std::uint16_t client_settings_endpoint_https_port() {
    return net::map_port(nvhttp::PORT_HTTPS);
  }

  std::string client_settings_endpoint_base_url(std::string_view request_host) {
    return client_settings_endpoint_base_url(request_host, client_settings_endpoint_https_port());
  }

  std::string client_settings_endpoint_url(std::string_view request_host) {
    return client_settings_endpoint_url(request_host, client_settings_endpoint_https_port());
  }

  class PolarisConfigHTTPSServer: public SimpleWeb::ServerBase<SimpleWeb::HTTPS> {
  public:
    PolarisConfigHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<SimpleWeb::HTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<void(std::shared_ptr<Request>, SSL*)> verify;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int, boost::asio::ssl::verify_context &) {
          // Allow the handshake to complete, then validate against the paired-client store.
          return true;
        });
      }
    }

    void accept() override {
      auto connection = create_connection(*io_service, context);

      acceptor->async_accept(connection->socket->lowest_layer(), [this, connection](const SimpleWeb::error_code &ec) {
        auto lock = connection->handler_runner->continue_lock();
        if (!lock) {
          return;
        }

        if (ec != SimpleWeb::error::operation_aborted) {
          this->accept();
        }

        auto session = std::make_shared<Session>(config.max_request_streambuf_size, connection);

        if (!ec) {
          boost::asio::ip::tcp::no_delay option(true);
          SimpleWeb::error_code set_option_ec;
          session->connection->socket->lowest_layer().set_option(option, set_option_ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &handshake_ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }

            if (!handshake_ec) {
              if (verify) {
                verify(session->request, session->connection->socket->native_handle());
              }
              this->read(session);
            } else if (this->on_error) {
              this->on_error(session->request, handshake_ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = PolarisConfigHTTPSServer;
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Keep the base enum for client operations.
  enum class op_e {
    ADD,    ///< Add client
    REMOVE  ///< Remove client
  };

  // CSRF TOKEN - generated at server startup
  static std::string csrfToken;

  // RATE LIMITING for login endpoint
  // Maps IP address -> (failed_attempts, last_failure_time)
  static std::unordered_map<std::string, std::pair<int, std::chrono::steady_clock::time_point>> login_rate_limits;
  static std::mutex rate_limit_mutex;
  constexpr int MAX_LOGIN_ATTEMPTS = 5;
  constexpr auto LOGIN_BLOCK_DURATION = std::chrono::seconds(60);
  static size_t append_string_curl_write_cb(void *contents, size_t size, size_t nmemb, std::string *out);

#ifdef __linux__
  /**
   * @brief One labwc façade snapshot for confighttp handlers (running + state + socket).
   * Avoids triplicated is_running/runtime_state/wayland_socket call clusters.
   */
  struct labwc_snapshot_t {
    bool running = false;
    platf::runtime_state_t state {};
    std::string socket;
    pid_t pid = 0;
    bool healthy = false;
  };

  labwc_snapshot_t snapshot_labwc() {
    labwc_snapshot_t snap;
    snap.running = stream_runtime::labwc::is_running();
    snap.state = stream_runtime::labwc::runtime_state();
    if (snap.running) {
      snap.socket = stream_runtime::labwc::wayland_socket();
      snap.pid = stream_runtime::labwc::pid();
      snap.healthy = stream_runtime::labwc::is_healthy();
    }
    return snap;
  }
#endif

  namespace {
    constexpr auto SESSION_IDLE_TIMEOUT = 12h;
    constexpr auto SESSION_TOUCH_INTERVAL = 5min;
    constexpr std::size_t SESSION_MAX_COUNT = 128;
    constexpr std::size_t SESSION_MAX_FILE_BYTES = 256 * 1024;
    std::unique_ptr<web_session_store::manager_t> s_web_sessions;
    bool s_loaded_credentials_available = true;
    std::recursive_mutex s_credential_lifecycle_mutex;
    std::mutex s_background_tasks_mutex;
    std::vector<std::future<void>> s_background_tasks;

    void prune_background_tasks_locked() {
      auto it = s_background_tasks.begin();
      while (it != s_background_tasks.end()) {
        if (it->wait_for(0s) == std::future_status::ready) {
          try {
            it->get();
          } catch (...) {
          }
          it = s_background_tasks.erase(it);
          continue;
        }
        ++it;
      }
    }

    template<typename Fn>
    void launch_background_task(Fn &&fn) {
      std::lock_guard lock(s_background_tasks_mutex);
      prune_background_tasks_locked();
      s_background_tasks.emplace_back(std::async(std::launch::async, std::forward<Fn>(fn)));
    }

    void wait_for_background_tasks() {
      std::vector<std::future<void>> tasks;
      {
        std::lock_guard lock(s_background_tasks_mutex);
        tasks.swap(s_background_tasks);
      }

      for (auto &task : tasks) {
        try {
          task.get();
        } catch (...) {
        }
      }
    }

    std::string bool_config_value(bool enabled) {
      return enabled ? "enabled"s : "disabled"s;
    }

    bool json_config_enabled(const nlohmann::json &value) {
      if (value.is_boolean()) {
        return value.get<bool>();
      }
      if (value.is_number_integer()) {
        return value.get<int>() != 0;
      }
      if (value.is_string()) {
        const auto text = boost::algorithm::to_lower_copy(value.get<std::string>());
        return text == "enabled" || text == "true" || text == "1" || text == "yes";
      }
      return false;
    }

    std::vector<std::string> steam_library_launch_commands(
      const std::string &appid,
      const std::string &mode = std::string {proc::STEAM_LAUNCH_MODE_DIRECT}
    ) {
      if (proc::steam_launch_mode_is_big_picture(mode)) {
        return {
          "setsid steam -gamepadui",
          "setsid bash -lc \"sleep 6; steam steam://rungameid/" + appid +
            " >/dev/null 2>&1 || true; sleep 4; exec steam -applaunch " + appid +
            " >/dev/null 2>&1 || true\""
        };
      }
      return { "setsid steam steam://rungameid/" + appid };
    }

    std::string safe_cover_extension_from_url(const std::string &url) {
      const auto query_pos = url.find_first_of("?#");
      const auto path_only = url.substr(0, query_pos);
      auto extension = fs::path(path_only).extension().string();
      boost::to_lower(extension);

      if (extension == ".jpg" || extension == ".jpeg" || extension == ".png" || extension == ".webp") {
        return extension;
      }

      if (path_only.find("storepagebackground/") != std::string::npos) {
        return ".webp";
      }

      return ".png";
    }

    std::string shell_escape(const std::string &value) {
      std::string escaped;
      escaped.reserve(value.size() + 2);
      escaped.push_back('\'');
      for (const char ch : value) {
        if (ch == '\'') {
          escaped += "'\\''";
        } else {
          escaped.push_back(ch);
        }
      }
      escaped.push_back('\'');
      return escaped;
    }

#ifdef __linux__
    constexpr auto PREVIEW_FAILURE_LOG_BACKOFF = std::chrono::seconds(60);
    // Minimum PNG size that is unlikely to be an empty/stub write (IHDR + tiny body).
    constexpr std::uintmax_t PREVIEW_MIN_BYTES = 256;
    constexpr auto PREVIEW_CACHE_MAX_AGE = std::chrono::minutes(30);

    struct preview_failure_log_state_t {
      std::chrono::steady_clock::time_point last_log {};
      std::uint32_t suppressed_count = 0;
    };

    std::mutex preview_failure_log_mutex;
    std::unordered_map<std::string, preview_failure_log_state_t> preview_failure_log_state;

    std::string preview_xdg_runtime_dir() {
      if (const char *xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) {
        return xdg;
      }
      return "/run/user/1000";
    }

    std::string preview_last_frame_cache_path() {
      return preview_xdg_runtime_dir() + "/polaris-last-preview.png";
    }

    bool preview_file_usable(const std::string &path) {
      std::error_code ec;
      const auto size = std::filesystem::file_size(path, ec);
      return !ec && size >= PREVIEW_MIN_BYTES;
    }

    void store_preview_last_frame(const std::string &src) {
      if (!preview_file_usable(src)) {
        return;
      }
      std::error_code ec;
      std::filesystem::copy_file(
        src,
        preview_last_frame_cache_path(),
        std::filesystem::copy_options::overwrite_existing,
        ec
      );
      if (ec) {
        BOOST_LOG(debug) << "display_preview: failed to update last-frame cache: "sv << ec.message();
      }
    }

    bool try_preview_last_frame_cache(const std::string &outfile) {
      const auto cache = preview_last_frame_cache_path();
      std::error_code ec;
      if (!preview_file_usable(cache)) {
        return false;
      }
      // Prefer cache while a session is live (preview during/just after stream).
      // Otherwise only serve if the file is still "fresh" by wall-clock mtime.
      const auto state = get_session_state();
      const bool stream_active =
        state == "streaming" || state == "paused" || state == "game_launching" ||
        state == "cage_starting" || state == "initializing" || state == "tearing_down" ||
        proc::proc.running() > 0;
      if (!stream_active) {
        // Age via status_last_write_time is awkward across clocks; use st_mtime via stat.
        struct stat st {};
        if (stat(cache.c_str(), &st) != 0) {
          return false;
        }
        const auto age = std::chrono::system_clock::now() - std::chrono::system_clock::from_time_t(st.st_mtime);
        if (age > PREVIEW_CACHE_MAX_AGE) {
          return false;
        }
      }
      std::filesystem::copy_file(
        cache,
        outfile,
        std::filesystem::copy_options::overwrite_existing,
        ec
      );
      return !ec && preview_file_usable(outfile);
    }

    bool preview_output_is_safe(const std::string &value) {
      return !value.empty() &&
             std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ':';
             });
    }

    std::string active_preview_output_name(const std::string &requested_output) {
      if (preview_output_is_safe(requested_output)) {
        return requested_output;
      }

      if (config::video.linux_display.use_cage_compositor) {
        const auto labwc = snapshot_labwc();
        if (labwc.running && labwc.state.backend_name == "labwc") {
          return labwc.state.effective_headless ? "HEADLESS-1" : "WL-1";
        }
      }

      return "";
    }

    std::string build_cage_preview_capture_command(const std::string &socket_name,
                                                   const std::string &output_name,
                                                   const std::string &outfile) {
      std::ostringstream cmd;
      cmd << "XDG_RUNTIME_DIR=" << shell_escape(preview_xdg_runtime_dir())
          << " WAYLAND_DISPLAY=" << shell_escape(socket_name)
          << " grim ";
      if (!output_name.empty()) {
        // preview_output_is_safe already restricts this to [alnum-_.:], so
        // nothing can escape today -- but its siblings above are escaped and
        // this one being the exception is how the restriction stops holding.
        cmd << "-o " << shell_escape(output_name) << ' ';
      }
      cmd << shell_escape(outfile) << " 2>/dev/null";
      return cmd.str();
    }

    /**
     * @brief Try preview capture for the active stream path (labwc / gamescope / host).
     * @return true on success. Prefer gamescopectl on gamescope (grim unsupported);
     *         fall back to last successful stream frame cache.
     */
    bool try_stream_path_preview_capture(const std::string &requested_output,
                                         const std::string &outfile,
                                         std::string &used_backend) {
      const auto xdg = preview_xdg_runtime_dir();
      auto try_grim_socket = [&](const std::string &socket_name, const std::string &output_name) {
        if (socket_name.empty()) {
          return false;
        }
        const auto sock_path = xdg + "/" + socket_name;
        if (access(sock_path.c_str(), F_OK) != 0) {
          return false;
        }
        std::remove(outfile.c_str());
        const auto cmd = build_cage_preview_capture_command(socket_name, output_name, outfile);
        if (std::system(cmd.c_str()) != 0) {
          return false;
        }
        return preview_file_usable(outfile);
      };

      // 1) Owned labwc private runtime
      if (config::video.linux_display.use_cage_compositor) {
        const auto labwc = snapshot_labwc();
        if (labwc.running) {
          const auto output = active_preview_output_name(requested_output);
          if (try_grim_socket(labwc.socket, output)) {
            used_backend = "labwc";
            store_preview_last_frame(outfile);
            return true;
          }
        }
      }

      // 2) Gamescope (owned or idle attach) — grim/wlr-screencopy is NOT
      // supported on gamescope ("compositor doesn't support the screen capture
      // protocol"). Prefer gamescopectl screenshot (async write to path).
      auto try_gamescope_screenshot = [&](const std::string &socket_name) {
        if (socket_name.empty()) {
          return false;
        }
        const auto sock_path = xdg + "/" + socket_name;
        if (access(sock_path.c_str(), F_OK) != 0) {
          return false;
        }
        // Remove stale target so we can detect a new write.
        std::remove(outfile.c_str());
        const std::string env =
          "GAMESCOPE_WAYLAND_DISPLAY=" + shell_escape(socket_name) +
          " WAYLAND_DISPLAY=" + shell_escape(socket_name) +
          " XDG_RUNTIME_DIR=" + shell_escape(xdg) + " ";
        // Nudge a frame then screenshot (async write inside gamescope).
        // shell-escape-checked: env is built above from shell_escape'd values,
        // and everything concatenated here is a literal.
        const int repaint_result = std::system(
          (env + "gamescopectl debug_force_repaint >/dev/null 2>&1").c_str()
        );
        if (repaint_result != 0) {
          BOOST_LOG(debug) << "ConfigUI: gamescope repaint request failed with exit code " << repaint_result;
        }
        std::ostringstream cmd;
        cmd << env << "gamescopectl screenshot " << shell_escape(outfile)
            << " >/dev/null 2>&1";
        if (std::system(cmd.str().c_str()) != 0) {
          return false;
        }
        // Screenshot thread is async — wait briefly for a non-empty write.
        for (int i = 0; i < 40; ++i) {
          if (preview_file_usable(outfile)) {
            return true;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return preview_file_usable(outfile);
      };

      for (const char *gs : {"gamescope-0", "gamescope-1"}) {
        if (try_gamescope_screenshot(gs)) {
          used_backend = std::string("gamescopectl:") + gs;
          store_preview_last_frame(outfile);
          return true;
        }
        // grim may still work on some nested/wlroots-compatible forks — try last.
        if (try_grim_socket(gs, "")) {
          used_backend = gs;
          store_preview_last_frame(outfile);
          return true;
        }
      }
      if (const char *gs_env = std::getenv("GAMESCOPE_WAYLAND_DISPLAY"); gs_env && *gs_env) {
        if (try_gamescope_screenshot(gs_env)) {
          used_backend = std::string("gamescopectl:") + gs_env;
          store_preview_last_frame(outfile);
          return true;
        }
        if (try_grim_socket(gs_env, "")) {
          used_backend = gs_env;
          store_preview_last_frame(outfile);
          return true;
        }
      }

      // 2b) Gamescope X11 nested surface (from polaris-gamescope.env / DISPLAY) via ImageMagick.
      // gamescopectl can fail idle (empty vulkan screenshot texture); X11 grab may still work mid-stream.
      {
        std::string x11_display;
        const auto env_file = xdg + "/polaris-gamescope.env";
        if (std::ifstream env_in(env_file); env_in) {
          std::string line;
          while (std::getline(env_in, line)) {
            if (line.rfind("DISPLAY=", 0) == 0) {
              x11_display = line.substr(8);
              break;
            }
          }
        }
        if (x11_display.empty()) {
          if (const char *d = std::getenv("DISPLAY"); d && *d) {
            x11_display = d;
          }
        }
        if (!x11_display.empty() && preview_output_is_safe(x11_display)) {
          std::remove(outfile.c_str());
          // Prefer magick import (IM7), fall back to import.
          const std::string magick_cmd =
            "DISPLAY=" + shell_escape(x11_display) +
            " magick import -window root " + shell_escape(outfile) + " 2>/dev/null";
          const std::string import_cmd =
            "DISPLAY=" + shell_escape(x11_display) +
            " import -window root " + shell_escape(outfile) + " 2>/dev/null";
          if (std::system(magick_cmd.c_str()) == 0 && preview_file_usable(outfile)) {
            used_backend = std::string("x11:") + x11_display;
            store_preview_last_frame(outfile);
            return true;
          }
          if (std::system(import_cmd.c_str()) == 0 && preview_file_usable(outfile)) {
            used_backend = std::string("x11:") + x11_display;
            store_preview_last_frame(outfile);
            return true;
          }
        }
      }

      // 3) Host Wayland (Mirror Desktop / dongle desktop) — skip gamescope sockets
      // already handled above (grim unsupported there).
      if (const char *wl = std::getenv("WAYLAND_DISPLAY"); wl && *wl) {
        const std::string wl_s {wl};
        const bool is_gamescope =
          wl_s == "gamescope-0" || wl_s == "gamescope-1" ||
          wl_s.rfind("gamescope-", 0) == 0;
        if (!is_gamescope) {
          std::string out_name = requested_output;
          if (out_name.empty() && !config::video.linux_display.streaming_output.empty() &&
              config::video.linux_display.stream_mode == "headless_dongle") {
            out_name = config::video.linux_display.streaming_output;
          }
          if (try_grim_socket(wl_s, out_name)) {
            used_backend = std::string("host:") + wl_s;
            store_preview_last_frame(outfile);
            return true;
          }
          // grim without -o
          if (try_grim_socket(wl_s, "")) {
            used_backend = std::string("host:") + wl_s;
            store_preview_last_frame(outfile);
            return true;
          }
        }
      }

      // 4) Host X11 / spectacle last resort
      {
        std::remove(outfile.c_str());
        const std::string cmd = "spectacle -b -n -o " + shell_escape(outfile) + " 2>/dev/null";
        if (std::system(cmd.c_str()) == 0 && preview_file_usable(outfile)) {
          used_backend = "spectacle";
          store_preview_last_frame(outfile);
          return true;
        }
      }

      // 5) grim on default session without explicit socket (some hosts)
      {
        std::remove(outfile.c_str());
        const std::string cmd = "grim " + shell_escape(outfile) + " 2>/dev/null";
        if (std::system(cmd.c_str()) == 0 && preview_file_usable(outfile)) {
          used_backend = "grim";
          store_preview_last_frame(outfile);
          return true;
        }
      }

      // 6) Last successful stream/preview frame (SB-1: gamescopectl idle often fails)
      if (try_preview_last_frame_cache(outfile)) {
        used_backend = "last_frame_cache";
        return true;
      }

      used_backend = "none";
      return false;
    }

    std::string preview_failure_log_key(const std::string &capture_kind,
                                        const std::string &socket_name,
                                        const std::string &output_name) {
      return capture_kind + "|" + socket_name + "|" + output_name;
    }

    [[maybe_unused]] void clear_cage_preview_capture_failure(const std::string &capture_kind,
                                                             const std::string &socket_name,
                                                             const std::string &output_name) {
      std::lock_guard lock(preview_failure_log_mutex);
      preview_failure_log_state.erase(preview_failure_log_key(capture_kind, socket_name, output_name));
    }

    [[maybe_unused]] void log_cage_preview_capture_failure(const std::string &capture_kind,
                                                           const std::string &socket_name,
                                                           const std::string &output_name) {
      const auto now = std::chrono::steady_clock::now();
      std::uint32_t suppressed_count = 0;
      bool should_log = false;

      {
        std::lock_guard lock(preview_failure_log_mutex);
        auto &state = preview_failure_log_state[preview_failure_log_key(capture_kind, socket_name, output_name)];
        if (state.last_log.time_since_epoch().count() == 0 ||
            now - state.last_log >= PREVIEW_FAILURE_LOG_BACKOFF) {
          suppressed_count = state.suppressed_count;
          state.suppressed_count = 0;
          state.last_log = now;
          should_log = true;
        } else {
          ++state.suppressed_count;
        }
      }

      if (!should_log) {
        return;
      }

      std::ostringstream message;
      message << "display_preview: Failed to capture cage " << capture_kind
              << " on socket " << socket_name
              << " output=" << (output_name.empty() ? "(auto)" : output_name)
              << "; preview capture is separate from the active stream, suppressing repeats for "
              << PREVIEW_FAILURE_LOG_BACKOFF.count() << "s";
      if (suppressed_count > 0) {
        message << " (" << suppressed_count << " repeat"
                << (suppressed_count == 1 ? "" : "s") << " suppressed)";
      }
      BOOST_LOG(info) << message.str();
    }
#endif

    struct steam_store_assets_t {
      std::string header_image;
      std::string capsule_image;
      std::string capsule_imagev5;
      std::string background;
      std::string background_raw;
    };

    bool steam_artwork_url_allowed(std::string_view url) {
      return game_artwork::is_allowed_provider_url(game_artwork::provider_e::steam, url);
    }

    bool cover_download_url_allowed(std::string_view url) {
      return steam_artwork_url_allowed(url) ||
             game_artwork::is_allowed_provider_url(game_artwork::provider_e::steamgriddb, url);
    }

    bool igdb_image_url_allowed(std::string_view url) {
      return http::url_is_https_host(url, "images.igdb.com");
    }

    std::optional<steam_store_assets_t> fetch_steam_store_assets(const std::string &appid) {
      const std::string url = "https://store.steampowered.com/api/appdetails?appids=" + appid;
      CURL *curl = curl_easy_init();
      if (!curl) {
        return std::nullopt;
      }

      std::string response;
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_string_curl_write_cb);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "Polaris/1.0");
#if LIBCURL_VERSION_NUM >= 0x075500
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
#else
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif

      const bool downloaded = http::redirect::follow(
        url,
        steam_artwork_url_allowed,
        [&](const std::string &current_url) {
          response.clear();
          curl_easy_setopt(curl, CURLOPT_URL, current_url.c_str());
          const CURLcode result = curl_easy_perform(curl);
          long response_code = 0;
          char *redirect_url = nullptr;
          if (result == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect_url);
          }
          return http::redirect::hop_result_t {
            result == CURLE_OK,
            response_code,
            redirect_url && *redirect_url ? std::optional<std::string> {redirect_url} : std::nullopt,
          };
        }
      );
      curl_easy_cleanup(curl);

      if (!downloaded) {
        return std::nullopt;
      }

      try {
        const auto data = nlohmann::json::parse(response);
        if (!data.contains(appid) ||
            !data[appid].value("success", false) ||
            !data[appid].contains("data") ||
            !data[appid]["data"].is_object()) {
          return std::nullopt;
        }

        const auto &app = data[appid]["data"];
        steam_store_assets_t assets;
        assets.header_image = app.value("header_image", "");
        assets.capsule_image = app.value("capsule_image", "");
        assets.capsule_imagev5 = app.value("capsule_imagev5", "");
        assets.background = app.value("background", "");
        assets.background_raw = app.value("background_raw", "");
        return assets;
      } catch (...) {
        return std::nullopt;
      }
    }

    bool synthesize_steam_cover_from_header(const fs::path &header_path, const fs::path &output_path) {
#ifdef __linux__
      const fs::path magick_path {platf::linux_util::find_executable_in_path("magick")};
#else
      const auto magick_path = boost::process::v1::search_path("magick");
#endif
      if (magick_path.empty()) {
        return false;
      }

#ifdef _WIN32
      constexpr const char *quiet_redirect = ">NUL 2>&1";
#else
      constexpr const char *quiet_redirect = ">/dev/null 2>&1";
#endif

      std::string command =
        shell_escape(magick_path.string()) +
        " \\( " + shell_escape(header_path.string()) + " -resize 600x900^ -gravity center -extent 600x900 -blur 0x22 -modulate 88,92,100 \\)" +
        " \\( " + shell_escape(header_path.string()) + " -resize 540x252 \\)" +
        " -gravity north -geometry +0+72 -compose over -composite " +
        shell_escape(output_path.string()) + " " + quiet_redirect;

      const int result = std::system(command.c_str());
      if (result != 0) {
        std::error_code ec;
        fs::remove(output_path, ec);
        return false;
      }

      std::error_code ec;
      if (!fs::exists(output_path, ec) || ec) {
        return false;
      }
      const auto output_size = fs::file_size(output_path, ec);
      return !ec && output_size > 0;
    }

    std::optional<std::string> download_best_steam_cover(const std::string &appid, const fs::path &coverdir, const std::string &stem) {
      fs::create_directories(coverdir);

      const std::string portrait_url = "https://steamcdn-a.akamaihd.net/steam/apps/" + appid + "/library_600x900_2x.jpg";
      const fs::path portrait_path = coverdir / (stem + safe_cover_extension_from_url(portrait_url));
      if (http::download_file(portrait_url, portrait_path.string(), steam_artwork_url_allowed)) {
        return portrait_path.string();
      }

      const auto assets = fetch_steam_store_assets(appid);
      if (!assets) {
        return std::nullopt;
      }

      if (!assets->header_image.empty()) {
        const auto tmp_stem = "steam_header_" + appid + "_" + uuid_util::uuid_t::generate().string();
        const fs::path header_tmp_path = fs::temp_directory_path() / (tmp_stem + safe_cover_extension_from_url(assets->header_image));
        if (http::download_file(assets->header_image, header_tmp_path.string(), steam_artwork_url_allowed)) {
          const fs::path generated_path = coverdir / (stem + ".jpg");
          if (synthesize_steam_cover_from_header(header_tmp_path, generated_path)) {
            std::error_code ec;
            fs::remove(header_tmp_path, ec);
            BOOST_LOG(info) << "Generated Steam cover fallback for [" << appid << "] from store header art";
            return generated_path.string();
          }

          std::error_code copy_error;
          fs::copy_file(header_tmp_path, generated_path, fs::copy_options::overwrite_existing, copy_error);
          std::error_code remove_error;
          fs::remove(header_tmp_path, remove_error);
          if (!copy_error) {
            BOOST_LOG(info) << "Fell back to Steam header art for [" << appid << "]";
            return generated_path.string();
          }
        }
      }

      for (const auto &candidate_url : {assets->capsule_image, assets->capsule_imagev5}) {
        if (candidate_url.empty()) {
          continue;
        }

        const fs::path candidate_path = coverdir / (stem + safe_cover_extension_from_url(candidate_url));
        if (http::download_file(candidate_url, candidate_path.string(), steam_artwork_url_allowed)) {
          BOOST_LOG(info) << "Fell back to Steam capsule art for [" << appid << "]";
          return candidate_path.string();
        }
      }

      return std::nullopt;
    }

    bool heroic_artwork_url_allowed(std::string_view url) {
      return game_artwork::is_allowed_provider_url(game_artwork::provider_e::epic, url);
    }

    std::optional<std::string> download_best_heroic_cover(
      const std::string &poster_url,
      const std::string &hero_url,
      const fs::path &coverdir,
      const std::string &stem
    ) {
      std::error_code error;
      fs::create_directories(coverdir, error);
      if (error) {
        return std::nullopt;
      }

      for (const auto extension : {".png", ".jpg", ".webp"}) {
        const auto cached_path = coverdir / (stem + extension);
        if (game_artwork::image_mime_type(cached_path)) {
          return cached_path.string();
        }
      }

      for (const auto &url : {poster_url, hero_url}) {
        if (url.empty() || !heroic_artwork_url_allowed(url)) {
          continue;
        }

        const auto temporary_path = coverdir / (stem + ".download");
        fs::remove(temporary_path, error);
        error.clear();
        if (!http::download_file(url, temporary_path.string(), heroic_artwork_url_allowed)) {
          continue;
        }

        const auto mime = game_artwork::image_mime_type(temporary_path);
        const auto extension = !mime ? std::string {} :
          *mime == "image/jpeg" ? ".jpg" :
          *mime == "image/png" ? ".png" :
          *mime == "image/webp" ? ".webp" : "";
        if (extension.empty()) {
          fs::remove(temporary_path, error);
          continue;
        }

        const auto final_path = coverdir / (stem + extension);
        fs::remove(final_path, error);
        error.clear();
        fs::rename(temporary_path, final_path, error);
        if (!error && game_artwork::image_mime_type(final_path)) {
          return final_path.string();
        }
        fs::remove(temporary_path, error);
        fs::remove(final_path, error);
      }

      return std::nullopt;
    }

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
      const auto exe_dir = executable_dir();
      if (exe_dir) {
        const fs::path local_build_path = *exe_dir / "assets" / "web" / relative_path;
        if (fs::exists(local_build_path)) {
          return local_build_path;
        }
      }

      const fs::path installed_path = fs::path(POLARIS_ASSETS_DIR) / "web" / relative_path;
      if (fs::exists(installed_path)) {
        return installed_path;
      }

      return installed_path;
    }

    constexpr std::array<std::string_view, 3> write_only_secret_config_keys {
      "ai_api_key"sv,
      "api_key"sv,
      "steamgriddb_api_key"sv,
    };

    bool is_write_only_secret_config_key(const std::string_view key) {
      return std::find(write_only_secret_config_keys.begin(), write_only_secret_config_keys.end(), key) != write_only_secret_config_keys.end();
    }

    void append_common_security_headers(SimpleWeb::CaseInsensitiveMultimap &headers) {
      headers.emplace("X-Frame-Options", "DENY");
      headers.emplace("X-Content-Type-Options", "nosniff");
      headers.emplace("Referrer-Policy", "no-referrer");
      headers.emplace("Permissions-Policy", "camera=(), microphone=(), geolocation=(), usb=(), payment=()");
      headers.emplace("Strict-Transport-Security", "max-age=31536000");
      headers.emplace("Content-Security-Policy", std::format("default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self' https://api.github.com https://*:{} https://*:{}; font-src 'self'; frame-ancestors 'none';", browser_stream::default_webtransport_port, client_settings_endpoint_https_port()));
    }

    void append_json_security_headers(SimpleWeb::CaseInsensitiveMultimap &headers) {
      headers.emplace("Content-Type", "application/json");
      append_common_security_headers(headers);
    }

    web_session_store::clock_snapshot_t web_session_clock_now() {
      return {
        .wall = std::chrono::system_clock::now(),
        .monotonic = std::chrono::steady_clock::now(),
      };
    }

    std::string session_cookie_hash(const std::string_view raw_cookie) {
      std::lock_guard credential_lock {s_credential_lifecycle_mutex};
      return util::hex(crypto::hash(std::string {raw_cookie} + config::sunshine.salt)).to_string();
    }

    std::string web_session_credential_fingerprint() {
      std::lock_guard credential_lock {s_credential_lifecycle_mutex};
      const auto material = std::string {"polaris-web-session-store-v1:"} + config::sunshine.salt;
      return util::hex(crypto::hash(material)).to_string();
    }

    web_session_store::policy_t web_session_policy() {
      return {
        .absolute_lifetime = SESSION_EXPIRE_DURATION,
        .idle_timeout = SESSION_IDLE_TIMEOUT,
        .touch_interval = SESSION_TOUCH_INTERVAL,
        .max_sessions = SESSION_MAX_COUNT,
        .max_file_bytes = SESSION_MAX_FILE_BYTES,
      };
    }

    bool ensure_web_session_fingerprint() {
      std::lock_guard credential_lock {s_credential_lifecycle_mutex};
      if (!s_web_sessions) {
        return false;
      }
      return s_web_sessions->rotate_credentials(
        web_session_credential_fingerprint(),
        web_session_clock_now()
      );
    }

    void initialize_web_session_store() {
      std::lock_guard credential_lock {s_credential_lifecycle_mutex};
      s_web_sessions = std::make_unique<web_session_store::manager_t>(
        platf::appdata() / "web_sessions.json",
        web_session_credential_fingerprint(),
        web_session_policy()
      );
      const auto status = s_web_sessions->load(web_session_clock_now());
      switch (status) {
        case web_session_store::load_status_e::loaded:
          BOOST_LOG(info) << "Loaded " << s_web_sessions->size() << " durable Web UI session(s)";
          break;
        case web_session_store::load_status_e::missing:
          BOOST_LOG(info) << "No durable Web UI session state found";
          break;
        case web_session_store::load_status_e::rejected:
          BOOST_LOG(warning) << "Rejected durable Web UI session state; starting with no sessions";
          break;
        case web_session_store::load_status_e::io_error:
          BOOST_LOG(error) << "Couldn't read durable Web UI session state; starting with no sessions";
          break;
      }
    }

    std::optional<std::string> create_web_session(const std::string_view expected_credential_fingerprint) {
      std::lock_guard credential_lock {s_credential_lifecycle_mutex};
      if (!s_web_sessions ||
          !s_web_sessions->rotate_credentials(
            std::string {expected_credential_fingerprint},
            web_session_clock_now()
          )) {
        return std::nullopt;
      }
      for (int attempt = 0; attempt < 3; ++attempt) {
        auto raw_cookie = crypto::rand_alphabet(64);
        const auto status = s_web_sessions->create_for_fingerprint(
          session_cookie_hash(raw_cookie),
          expected_credential_fingerprint,
          web_session_clock_now()
        );
        if (status == web_session_store::creation_status_e::created) {
          return raw_cookie;
        }
        if (status == web_session_store::creation_status_e::credential_mismatch) {
          BOOST_LOG(warning) << "Refused to create a Web UI session for a stale credential generation";
          return std::nullopt;
        }
      }
      BOOST_LOG(error) << "Couldn't durably create a Web UI session";
      return std::nullopt;
    }

    web_session_store::validation_status_e authenticate_web_session_cookie(const std::string_view raw_cookie) {
      std::lock_guard credential_lock {s_credential_lifecycle_mutex};
      if (raw_cookie.empty()) {
        return web_session_store::validation_status_e::invalid;
      }
      if (!s_web_sessions) {
        return web_session_store::validation_status_e::io_error;
      }
      return s_web_sessions->validate(session_cookie_hash(raw_cookie), web_session_clock_now());
    }

    bool invalidate_web_session_cookie(const std::string_view raw_cookie) {
      std::lock_guard credential_lock {s_credential_lifecycle_mutex};
      if (raw_cookie.empty()) {
        return true;
      }
      if (!ensure_web_session_fingerprint()) {
        return false;
      }
      return s_web_sessions->invalidate(session_cookie_hash(raw_cookie));
    }

    SimpleWeb::CaseInsensitiveMultimap make_auth_cookie_headers(std::string_view raw_cookie) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      auto cookie = std::string {"auth="} + std::string {raw_cookie} + "; Secure; HttpOnly; SameSite=Strict; Max-Age=2592000; Path=/";
      headers.emplace("Set-Cookie", std::move(cookie));
      append_common_security_headers(headers);
      return headers;
    }

    SimpleWeb::CaseInsensitiveMultimap clear_auth_cookie_headers() {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Set-Cookie", "auth=; Secure; HttpOnly; SameSite=Strict; Max-Age=0; Path=/");
      append_common_security_headers(headers);
      return headers;
    }

    void write_session_persistence_error(resp_https_t response, std::string_view message) {
      nlohmann::json output = {
        {"status", false},
        {"error", message},
      };
      SimpleWeb::CaseInsensitiveMultimap headers;
      append_json_security_headers(headers);
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error, output.dump(), headers);
    }

    void write_session_validation_unavailable(resp_https_t response) {
      nlohmann::json output = {
        {"status", false},
        {"error", "Web UI session validation is temporarily unavailable."},
      };
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Retry-After", "1");
      headers.emplace("Cache-Control", "no-store");
      append_json_security_headers(headers);
      response->write(SimpleWeb::StatusCode::server_error_service_unavailable, output.dump(), headers);
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
      const auto redact_header = boost::iequals(name, "Authorization") || boost::iequals(name, "Cookie");
      BOOST_LOG(debug) << name << " -- " << (redact_header ? "CREDENTIALS REDACTED" : val);
    }
    BOOST_LOG(debug) << " [--] "sv;
    for (auto &[name, val] : request->parse_query_string()) {
      const auto redact_query = boost::iequals(name, "sessiontoken");
      BOOST_LOG(debug) << name << " -- " << (redact_query ? "CREDENTIALS REDACTED" : val);
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
    append_json_security_headers(headers);
    response->write(output_tree.dump(), headers);
  }

  void send_response(resp_https_t response,
                     SimpleWeb::StatusCode status_code,
                     const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    append_json_security_headers(headers);
    response->write(status_code, output_tree.dump(), headers);
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
    SimpleWeb::CaseInsensitiveMultimap headers;
    append_json_security_headers(headers);
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
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Location", path);
    append_common_security_headers(headers);
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

  crypto::p_named_cert_t getVerifiedClientCert(
    const crypto::p_named_cert_t &candidate,
    std::string_view request_path
  ) {
    return nvhttp::resolve_authorized_client(candidate, request_path);
  }

  crypto::p_named_cert_t getVerifiedClientCert(const req_https_t &request) {
    const auto candidate = std::static_pointer_cast<crypto::named_cert_t>(request->userp);
    return getVerifiedClientCert(candidate, request->path);
  }

  bool hasVerifiedClientCert(const req_https_t &request) {
    return getVerifiedClientCert(request) != nullptr;
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
    std::lock_guard credential_lock {s_credential_lifecycle_mutex};
    if (!checkIPOrigin(response, request))
      return false;
    if (!s_loaded_credentials_available) {
      write_session_validation_unavailable(response);
      return false;
    }
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
        if (crypto::constant_time_equals(provided_key, config::sunshine.api_key)) {
          fg.disable();
          return true;
        }
      }
    }

    // Session cookie authentication
    auto cookies = request->header.find("cookie");
    if (cookies == request->header.end())
      return false;
    auto authCookie = getCookieValue(cookies->second, "auth");
    const auto validation = authenticate_web_session_cookie(authCookie);
    if (validation == web_session_store::validation_status_e::io_error) {
      fg.disable();
      write_session_validation_unavailable(response);
      return false;
    }
    if (validation != web_session_store::validation_status_e::valid) {
      return false;
    }
    fg.disable();
    return true;
  }

  bool authenticatePolarisSession(resp_https_t response, req_https_t request, bool needsRedirect = false) {
    if (hasVerifiedClientCert(request)) {
      return true;
    }
    return authenticate(response, request, needsRedirect);
  }

  namespace {
    // 20 requests per 10s per IP (~2/sec average) - generous enough for a
    // harness polling GET .../runs/{run_id} every 1-2s during a run, while
    // still bounding a runaway/malicious loop. Not spec-mandated (measurement-
    // spec-v1.md 6.4 only requires *some* rate limit exists), chosen as a
    // reasonable default.
    confighttp::benchmark_auth::rate_limiter_t &benchmark_control_rate_limiter() {
      static confighttp::benchmark_auth::rate_limiter_t limiter(20, std::chrono::seconds(10));
      return limiter;
    }
  }  // namespace

  /**
   * @brief Authorize an incoming request as the local benchmark harness for
   * the P0-5 control surface (measurement-spec-v1.md 6.4). Every layer the
   * spec requires beyond ordinary paired-client pairing:
   *  - loopback/origin policy (checkIPOrigin, same as the rest of the Web UI);
   *  - a dedicated per-IP request rate limit (separate from login's, whose
   *    escalating-lockout shape fits failed-password attempts, not the
   *    steady polling a benchmark harness does while a run is active);
   *  - an authenticated confighttp Web UI session or API key (authenticate() -
   *    deliberately NOT authenticatePolarisSession(), which also accepts any
   *    verified streaming-client cert; the spec is explicit that "viewer/
   *    watch certificates cannot activate or retrieve a benchmark run");
   *  - a CSRF token, but only for the session-cookie half of authenticate()'s
   *    two paths - a Bearer API key is never auto-attached to a request by a
   *    browser the way a cookie is, so it needs no CSRF defense, and a pure
   *    script/CLI harness using it has no CSRF token to send in the first
   *    place. Every other mutating confighttp route defends the cookie path
   *    with the same withCsrf() wrapper; this route can't just reuse that
   *    wrapper unconditionally without also breaking Bearer-token callers.
   *
   * Deliberately does NOT check stream_stats::benchmark_control_plane_enabled() -
   * every engine function (create/start/stop/get/delete_benchmark_run)
   * already checks that itself and reports a specific rejection reason, so
   * this gate's job is purely "is this caller even allowed to attempt a
   * benchmark control operation," not "is benchmarking itself turned on."
   *
   * Writes an appropriate error response and returns false on any failure;
   * callers must return immediately without writing anything further.
   */
  bool authorizeBenchmarkHarnessRequest(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return false;
    }

    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto &limiter = benchmark_control_rate_limiter();
    const auto now = std::chrono::steady_clock::now();

    const bool rate_limited = limiter.is_rate_limited(address, now);
    limiter.record_request(address, now);
    if (rate_limited) {
      BOOST_LOG(warning) << "Benchmark control: ["sv << address << "] -- rate limited"sv;
      nlohmann::json tree;
      tree["status"] = false;
      tree["error"] = "Too many requests. Please try again later.";
      SimpleWeb::CaseInsensitiveMultimap headers;
      append_json_security_headers(headers);
      response->write(SimpleWeb::StatusCode::client_error_too_many_requests, tree.dump(), headers);
      return false;
    }

    if (!authenticate(response, request, /*needsRedirect=*/false)) {
      BOOST_LOG(warning) << "Benchmark control: ["sv << address << "] -- unauthorized"sv;
      return false;
    }

    // Detected by header presence, not re-validation - authenticate() has
    // already succeeded overall by this point, and a real Bearer token is
    // the only way that's true when this header is present (a garbage
    // Authorization header would have made authenticate() fall through to
    // the cookie check, and fail if that also failed).
    const bool used_bearer_token_auth = request->header.find("authorization") != request->header.end();
    if (!used_bearer_token_auth && !validateCsrf(request)) {
      BOOST_LOG(warning) << "Benchmark control: ["sv << address << "] -- invalid CSRF token"sv;
      // Inlined rather than calling forbidden() - that helper is defined
      // later in this file, after this function.
      constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_forbidden;
      nlohmann::json tree;
      tree["status_code"] = static_cast<int>(code);
      tree["status"] = false;
      tree["error"] = "Invalid CSRF token";
      SimpleWeb::CaseInsensitiveMultimap headers;
      append_json_security_headers(headers);
      response->write(code, tree.dump(), headers);
      return false;
    }

    BOOST_LOG(info) << "Benchmark control: ["sv << address << "] -- authorized"sv;
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
    append_json_security_headers(headers);

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
    append_json_security_headers(headers);

    response->write(code, tree.dump(), headers);
  }

  void forbidden(resp_https_t response, [[maybe_unused]] req_https_t request, const std::string &error_message = "Forbidden") {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_forbidden;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["status"] = false;
    tree["error"] = error_message;
    SimpleWeb::CaseInsensitiveMultimap headers;
    append_json_security_headers(headers);

    response->write(code, tree.dump(), headers);
  }

  void conflict_response(resp_https_t response, const nlohmann::json &tree) {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_conflict;
    SimpleWeb::CaseInsensitiveMultimap headers;
    append_json_security_headers(headers);
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

  namespace {
    // Every outcome create_benchmark_run() can report - see its declaration
    // in stream_stats.h for what each one means. Kept as a plain string
    // (not an HTTP status code) so a harness can always parse a reliable
    // JSON body rather than branch on a status code fragmented across many
    // loosely-related meanings; the spec doesn't mandate specific codes
    // either. rejected_caller_not_authorized_as_harness can't actually
    // reach here - authorizeBenchmarkHarnessRequest() already gated on it
    // before this route calls create_benchmark_run() - but is named anyway
    // so this switch stays exhaustive against the real enum.
    std::string_view to_string(stream_stats::benchmark_run_create_result_e result) {
      switch (result) {
        case stream_stats::benchmark_run_create_result_e::created:
          return "created"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_control_plane_not_enabled:
          return "rejected_control_plane_not_enabled"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_caller_not_authorized_as_harness:
          return "rejected_caller_not_authorized_as_harness"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_not_exactly_one_active_session:
          return "rejected_not_exactly_one_active_session"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_session_already_has_an_active_run:
          return "rejected_session_already_has_an_active_run"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_duration_out_of_range:
          return "rejected_duration_out_of_range"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_duration_tolerance_out_of_range:
          return "rejected_duration_tolerance_out_of_range"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_drain_grace_out_of_range:
          return "rejected_drain_grace_out_of_range"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_target_fps_out_of_range:
          return "rejected_target_fps_out_of_range"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_nominal_sample_budget_too_small:
          return "rejected_nominal_sample_budget_too_small"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_capacity_below_nominal_budget:
          return "rejected_capacity_below_nominal_budget"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_capacity_exceeds_maximum:
          return "rejected_capacity_exceeds_maximum"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_run_id_already_used:
          return "rejected_run_id_already_used"sv;
        case stream_stats::benchmark_run_create_result_e::rejected_invalid_manifest_sha256_format:
          return "rejected_invalid_manifest_sha256_format"sv;
      }
      return "unknown"sv;
    }
  }  // namespace

  /**
   * @brief Create and arm a benchmark run (measurement-spec-v1.md 6.4's
   * POST /polaris/v1/session/timing/runs). The request body carries no
   * device_uuid - the harness isn't the streaming client itself, so this
   * arms a run for whichever single session happens to be active right
   * now (get_single_active_session_identity()), the same session
   * create_benchmark_run's own "exactly one active stream session"
   * precondition is about.
   *
   * Always responds 200 with a JSON body naming the outcome in `result`
   * (see to_string() above), whether created or rejected - a 400 is
   * reserved for requests this route itself can't even parse (bad JSON,
   * wrong content type, missing run_id), not for any precondition
   * create_benchmark_run itself evaluates.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void createBenchmarkRun(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authorizeBenchmarkHarnessRequest(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    try {
      nlohmann::json inputTree = nlohmann::json::parse(ss.str());

      stream_stats::benchmark_run_create_request_t create_request;
      create_request.run_id = inputTree.value("run_id", std::string {});
      create_request.manifest_sha256 = inputTree.value("manifest_sha256", std::string {});
      create_request.label = inputTree.value("label", std::string {});
      create_request.workload_id = inputTree.value("workload_id", std::string {});
      create_request.expected_duration_s = inputTree.value("expected_duration_s", 0);
      create_request.duration_tolerance_ms = inputTree.value("duration_tolerance_ms", 0);
      create_request.drain_grace_ms = inputTree.value("drain_grace_ms", 0);
      create_request.target_fps = inputTree.value("target_fps", 0);
      create_request.sample_capacity_frames = inputTree.value("sample_capacity_frames", std::size_t {0});

      if (create_request.run_id.empty()) {
        bad_request(response, request, "Missing required field: run_id");
        return;
      }

      const auto session_identity = stream_stats::get_single_active_session_identity();
      if (!session_identity) {
        nlohmann::json outputTree;
        outputTree["status"] = false;
        outputTree["run_id"] = create_request.run_id;
        outputTree["result"] = to_string(stream_stats::benchmark_run_create_result_e::rejected_not_exactly_one_active_session);
        send_response(response, outputTree);
        return;
      }

      const auto result = stream_stats::create_benchmark_run(
        create_request, session_identity->device_uuid, session_identity->session_generation, true);

      nlohmann::json outputTree;
      outputTree["status"] = result == stream_stats::benchmark_run_create_result_e::created;
      outputTree["run_id"] = create_request.run_id;
      outputTree["result"] = to_string(result);
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "CreateBenchmarkRun: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  namespace {
    std::string_view to_string(stream_stats::benchmark_run_start_result_e result) {
      switch (result) {
        case stream_stats::benchmark_run_start_result_e::started:
          return "started"sv;
        case stream_stats::benchmark_run_start_result_e::rejected_control_plane_not_enabled:
          return "rejected_control_plane_not_enabled"sv;
        case stream_stats::benchmark_run_start_result_e::rejected_caller_not_authorized_as_harness:
          return "rejected_caller_not_authorized_as_harness"sv;
        case stream_stats::benchmark_run_start_result_e::rejected_run_not_found:
          return "rejected_run_not_found"sv;
        case stream_stats::benchmark_run_start_result_e::rejected_wrong_session:
          return "rejected_wrong_session"sv;
        case stream_stats::benchmark_run_start_result_e::rejected_run_not_in_armed_state:
          return "rejected_run_not_in_armed_state"sv;
        case stream_stats::benchmark_run_start_result_e::rejected_population_changed_since_arm:
          return "rejected_population_changed_since_arm"sv;
        case stream_stats::benchmark_run_start_result_e::rejected_session_no_longer_active:
          return "rejected_session_no_longer_active"sv;
      }
      return "unknown"sv;
    }

    std::string_view to_string(stream_stats::benchmark_run_stop_result_e result) {
      switch (result) {
        case stream_stats::benchmark_run_stop_result_e::stopped:
          return "stopped"sv;
        case stream_stats::benchmark_run_stop_result_e::stopped_early_and_aborted:
          return "stopped_early_and_aborted"sv;
        case stream_stats::benchmark_run_stop_result_e::rejected_control_plane_not_enabled:
          return "rejected_control_plane_not_enabled"sv;
        case stream_stats::benchmark_run_stop_result_e::rejected_caller_not_authorized_as_harness:
          return "rejected_caller_not_authorized_as_harness"sv;
        case stream_stats::benchmark_run_stop_result_e::rejected_run_not_found:
          return "rejected_run_not_found"sv;
        case stream_stats::benchmark_run_stop_result_e::rejected_wrong_session:
          return "rejected_wrong_session"sv;
        case stream_stats::benchmark_run_stop_result_e::rejected_not_currently_active:
          return "rejected_not_currently_active"sv;
      }
      return "unknown"sv;
    }

    // start/stop both need a run_id (from the URL, not the body - see the
    // route registrations below) plus the identity of the one active
    // session, exactly like createBenchmarkRun(). A missing/ambiguous
    // active session here reports as the run's own
    // rejected_wrong_session/rejected_session_no_longer_active outcomes
    // rather than a bespoke error, since from the run's perspective a
    // caller with no resolvable session identity can never be its owning
    // session either way.
    struct benchmark_route_context_t {
      std::string run_id;
      std::optional<stream_stats::active_session_identity_t> session_identity;
    };

    benchmark_route_context_t extract_benchmark_route_context(const req_https_t &request) {
      benchmark_route_context_t context;
      if (request->path_match.size() > 1) {
        context.run_id = request->path_match[1].str();
      }
      context.session_identity = stream_stats::get_single_active_session_identity();
      return context;
    }
  }  // namespace

  /**
   * @brief Start an armed benchmark run (measurement-spec-v1.md 6.4's
   * POST /polaris/v1/session/timing/runs/{run_id}/start). Takes no request
   * body - run_id comes from the URL, and the owning session's identity is
   * looked up the same way createBenchmarkRun() does, not supplied by the
   * caller.
   *
   * Always responds 200 with a JSON body naming the outcome in `result`
   * (see to_string() above) - there is no create-style "this route
   * couldn't even parse the request" case here, since there's no body to
   * parse.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void startBenchmarkRun(resp_https_t response, req_https_t request) {
    if (!authorizeBenchmarkHarnessRequest(response, request)) {
      return;
    }

    print_req(request);

    const auto context = extract_benchmark_route_context(request);

    nlohmann::json outputTree;
    outputTree["run_id"] = context.run_id;

    if (!context.session_identity) {
      outputTree["status"] = false;
      outputTree["result"] = to_string(stream_stats::benchmark_run_start_result_e::rejected_session_no_longer_active);
      send_response(response, outputTree);
      return;
    }

    const auto result = stream_stats::start_benchmark_run(
      context.run_id, context.session_identity->device_uuid, context.session_identity->session_generation, true);

    outputTree["status"] = result == stream_stats::benchmark_run_start_result_e::started;
    outputTree["result"] = to_string(result);
    send_response(response, outputTree);
  }

  /**
   * @brief Stop an active benchmark run (measurement-spec-v1.md 6.4's
   * POST /polaris/v1/session/timing/runs/{run_id}/stop). Same shape as
   * startBenchmarkRun() - no request body, run_id from the URL, owning
   * session looked up the same way.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void stopBenchmarkRun(resp_https_t response, req_https_t request) {
    if (!authorizeBenchmarkHarnessRequest(response, request)) {
      return;
    }

    print_req(request);

    const auto context = extract_benchmark_route_context(request);

    nlohmann::json outputTree;
    outputTree["run_id"] = context.run_id;

    if (!context.session_identity) {
      outputTree["status"] = false;
      outputTree["result"] = to_string(stream_stats::benchmark_run_stop_result_e::rejected_wrong_session);
      send_response(response, outputTree);
      return;
    }

    const auto result = stream_stats::stop_benchmark_run(
      context.run_id, context.session_identity->device_uuid, context.session_identity->session_generation, true);

    // status=true only for the literal stopped outcome, matching
    // createBenchmarkRun's own status convention (true iff the
    // semantically-intended outcome happened, not merely "the stop
    // request was processed without error"). stopped_early_and_aborted
    // is a real outcome the caller triggered, not a request-processing
    // failure, but it's not what a naive status check should read as
    // success either - the run it produced is invalid for measurement.
    outputTree["status"] = result == stream_stats::benchmark_run_stop_result_e::stopped;
    outputTree["result"] = to_string(result);
    send_response(response, outputTree);
  }

  namespace {
    std::string_view to_string(stream_stats::benchmark_run_state_e state) {
      switch (state) {
        case stream_stats::benchmark_run_state_e::armed:
          return "armed"sv;
        case stream_stats::benchmark_run_state_e::active:
          return "active"sv;
        case stream_stats::benchmark_run_state_e::draining:
          return "draining"sv;
        case stream_stats::benchmark_run_state_e::frozen:
          return "frozen"sv;
        case stream_stats::benchmark_run_state_e::aborted:
          return "aborted"sv;
        case stream_stats::benchmark_run_state_e::expired:
          return "expired"sv;
      }
      return "unknown"sv;
    }

    std::string_view to_string(stream_stats::benchmark_abort_reason_e reason) {
      switch (reason) {
        case stream_stats::benchmark_abort_reason_e::none:
          return "none"sv;
        case stream_stats::benchmark_abort_reason_e::session_ended:
          return "session_ended"sv;
        case stream_stats::benchmark_abort_reason_e::session_generation_changed:
          return "session_generation_changed"sv;
        case stream_stats::benchmark_abort_reason_e::client_population_revision_changed:
          return "client_population_revision_changed"sv;
        case stream_stats::benchmark_abort_reason_e::sample_capacity_exceeded:
          return "sample_capacity_exceeded"sv;
        case stream_stats::benchmark_abort_reason_e::invalid_stage_duration:
          return "invalid_stage_duration"sv;
        case stream_stats::benchmark_abort_reason_e::stopped_before_duration_lower_bound:
          return "stopped_before_duration_lower_bound"sv;
        case stream_stats::benchmark_abort_reason_e::explicit_harness_abort:
          return "explicit_harness_abort"sv;
        case stream_stats::benchmark_abort_reason_e::internal_telemetry_failure:
          return "internal_telemetry_failure"sv;
      }
      return "unknown"sv;
    }

    std::string_view to_string(stream_stats::benchmark_run_get_result_e result) {
      switch (result) {
        case stream_stats::benchmark_run_get_result_e::found:
          return "found"sv;
        case stream_stats::benchmark_run_get_result_e::rejected_control_plane_not_enabled:
          return "rejected_control_plane_not_enabled"sv;
        case stream_stats::benchmark_run_get_result_e::rejected_caller_not_authorized_as_harness:
          return "rejected_caller_not_authorized_as_harness"sv;
        case stream_stats::benchmark_run_get_result_e::rejected_run_not_found:
          return "rejected_run_not_found"sv;
      }
      return "unknown"sv;
    }

    std::string_view to_string(stream_stats::benchmark_run_delete_result_e result) {
      switch (result) {
        case stream_stats::benchmark_run_delete_result_e::deleted:
          return "deleted"sv;
        case stream_stats::benchmark_run_delete_result_e::rejected_control_plane_not_enabled:
          return "rejected_control_plane_not_enabled"sv;
        case stream_stats::benchmark_run_delete_result_e::rejected_caller_not_authorized_as_harness:
          return "rejected_caller_not_authorized_as_harness"sv;
        case stream_stats::benchmark_run_delete_result_e::rejected_run_not_found:
          return "rejected_run_not_found"sv;
      }
      return "unknown"sv;
    }

    // Nanoseconds since steady_clock's own (unspecified, but process-stable)
    // epoch - comparable to any other steady_clock reading from this same
    // process instance, not a wall-clock timestamp. clock_domain in the
    // response body is what tells a consumer that.
    std::int64_t monotonic_ns(std::chrono::steady_clock::time_point tp) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    }

    nlohmann::json optional_monotonic_ns_json(const std::optional<std::chrono::steady_clock::time_point> &tp) {
      if (!tp) {
        return nullptr;
      }
      return monotonic_ns(*tp);
    }

    nlohmann::json benchmark_stage_capture_json(const stream_stats::benchmark_stage_capture_t &stage) {
      nlohmann::json output;
      output["accepted_count"] = stage.accepted_count;
      output["excluded_started_before_window"] = stage.excluded_started_before_window;
      output["excluded_completed_after_window"] = stage.excluded_completed_after_window;
      output["started_in_window_without_terminal_count"] = stage.started_in_window_without_terminal_count;
      output["overflow_count"] = stage.overflow_count;
      output["invalid_count"] = stage.invalid_count;
      output["start_offset_us"] = stage.start_offset_us;
      output["end_offset_us"] = stage.end_offset_us;
      output["duration_us"] = stage.duration_us;
      return output;
    }

    // The full authoritative run output (measurement-spec-v1.md 6.4's field
    // list) for a found run. schema_version/measurement_spec_id/clock_domain
    // are new here - no existing endpoint in this codebase has needed them
    // before (the older rolling-diagnostics GET on nvhttp's server predates
    // this spec and has no such fields). collector_version reuses the same
    // PROJECT_VERSION main.cpp's own startup log already uses, rather than
    // inventing a second version scheme.
    nlohmann::json benchmark_run_json(const stream_stats::benchmark_run_t &run) {
      nlohmann::json output;
      output["schema_version"] = 1;
      output["measurement_spec_id"] = "measurement-spec-v1";
      output["collector_version"] = PROJECT_VERSION;
      output["clock_domain"] = "CLOCK_MONOTONIC";

      output["run_id"] = run.run_id;
      output["manifest_sha256"] = run.manifest_sha256;
      output["state"] = to_string(run.state);
      output["abort_reason"] = to_string(run.abort_reason);
      output["process_instance_id"] = stream_stats::process_instance_id();
      output["session_id"] = run.owning_device_uuid;
      output["session_generation"] = run.owning_session_generation;
      output["client_population_revision_at_arm"] = run.client_population_revision_at_arm;
      output["client_population_revision_at_freeze"] = run.client_population_revision_at_freeze;
      // Only meaningful once frozen (client_population_revision_at_freeze is
      // still its 0 default before then) - reported as 0 rather than
      // underflowing an unsigned subtraction for a still-open run.
      output["population_change_count"] =
        run.client_population_revision_at_freeze > run.client_population_revision_at_arm
          ? run.client_population_revision_at_freeze - run.client_population_revision_at_arm
          : 0;

      output["armed_monotonic_ns"] = monotonic_ns(run.armed_monotonic);
      output["started_monotonic_ns"] = optional_monotonic_ns_json(run.started_monotonic);
      output["stopped_monotonic_ns"] = optional_monotonic_ns_json(run.stopped_monotonic);
      output["frozen_monotonic_ns"] = optional_monotonic_ns_json(run.frozen_monotonic);

      output["expected_duration_ns"] = run.expected_duration_ns.count();
      output["duration_tolerance_ns"] = run.duration_tolerance_ns.count();
      output["drain_grace_ns"] = run.drain_grace_ns.count();

      // actual_duration_ns/duration_within_tolerance (measurement-spec-v1.md
      // 6.4's formula) are only computable once both endpoints exist - null
      // otherwise, rather than a misleading 0 or a duration measured against
      // only one real timestamp.
      if (run.started_monotonic && run.stopped_monotonic) {
        const std::int64_t actual_duration_ns = monotonic_ns(*run.stopped_monotonic) - monotonic_ns(*run.started_monotonic);
        output["actual_duration_ns"] = actual_duration_ns;
        // Written out rather than std::abs() - this file includes both
        // <cmath> and headers that could plausibly pull in <cstdlib>, and
        // integer std::abs overload resolution between the two is exactly
        // the kind of thing not worth risking a compile error over.
        const std::int64_t duration_delta_ns = actual_duration_ns - run.expected_duration_ns.count();
        const std::int64_t duration_delta_ns_abs = duration_delta_ns < 0 ? -duration_delta_ns : duration_delta_ns;
        output["duration_within_tolerance"] = duration_delta_ns_abs <= run.duration_tolerance_ns.count();
      } else {
        output["actual_duration_ns"] = nullptr;
        output["duration_within_tolerance"] = nullptr;
      }

      output["target_fps"] = run.target_fps;
      output["workload_id"] = run.workload_id;
      output["sample_capacity"] = run.sample_capacity;

      output["capture_to_encode"] = benchmark_stage_capture_json(run.capture_to_encode);
      output["encode_to_send_release"] = benchmark_stage_capture_json(run.encode_to_send_release);
      output["capture_to_send_release"] = benchmark_stage_capture_json(run.capture_to_send_release);

      return output;
    }
  }  // namespace

  /**
   * @brief Get a run's full authoritative output (measurement-spec-v1.md
   * 6.4's field list, including raw per-stage sample arrays - "a
   * percentile-only response is not sufficient for forensic verification").
   * Unlike start/stop, this takes no device_uuid/session_generation at all -
   * the harness reads any run by its own ID directly, not scoped to "the
   * session it currently owns" (see get_benchmark_run's own doc comment in
   * stream_stats.h).
   *
   * On success, responds 200 with the full run body directly (no
   * status/result wrapper - the presence of a body at 200 already means
   * found). On rejection, responds 200 with the same {status, run_id,
   * result} shape every other benchmark route uses.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getBenchmarkRun(resp_https_t response, req_https_t request) {
    if (!authorizeBenchmarkHarnessRequest(response, request)) {
      return;
    }

    print_req(request);

    const std::string run_id = request->path_match.size() > 1 ? request->path_match[1].str() : std::string {};

    nlohmann::json found_body;
    const auto result = stream_stats::get_benchmark_run(run_id, true, [&](stream_stats::benchmark_run_t &run) {
      found_body = benchmark_run_json(run);
    });

    if (result == stream_stats::benchmark_run_get_result_e::found) {
      send_response(response, found_body);
      return;
    }

    nlohmann::json outputTree;
    outputTree["status"] = false;
    outputTree["run_id"] = run_id;
    outputTree["result"] = to_string(result);
    send_response(response, outputTree);
  }

  /**
   * @brief Delete a run's storage immediately (measurement-spec-v1.md 6.4's
   * "DELETE releases payload storage immediately"), regardless of its
   * current state. Unlike start/stop/get, takes no device_uuid/
   * session_generation either - same harness-reads-any-run-by-ID model as
   * get.
   *
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void deleteBenchmarkRun(resp_https_t response, req_https_t request) {
    if (!authorizeBenchmarkHarnessRequest(response, request)) {
      return;
    }

    print_req(request);

    const std::string run_id = request->path_match.size() > 1 ? request->path_match[1].str() : std::string {};
    const auto result = stream_stats::delete_benchmark_run(run_id, true);

    nlohmann::json outputTree;
    outputTree["status"] = result == stream_stats::benchmark_run_delete_result_e::deleted;
    outputTree["run_id"] = run_id;
    outputTree["result"] = to_string(result);
    send_response(response, outputTree);
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
    headers.emplace("Cache-Control", "no-store");
    append_common_security_headers(headers);
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
    append_common_security_headers(headers);
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
    append_common_security_headers(headers);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(query, base);
    return relPath.empty() || *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset from the node_modules directory.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getNodeModules(resp_https_t response, req_https_t request) {
    print_req(request);

    fs::path webDirPath = fs::weakly_canonical(resolve_web_asset_path(""));

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if file does not exist or is outside the web directory
    if (!isChildPath(webDirPath, filePath)) {
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
    append_common_security_headers(headers);
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
  std::vector<fs::path> lutris_art_roots();
  std::string lutris_image_path_for_app_node(const nlohmann::json &app);
  void hydrate_lutris_app_images(nlohmann::json &file_tree);

  void getApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);
      hydrate_lutris_app_images(file_tree);

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
      // Read the input JSON from the request body.
      nlohmann::json inputTree = nlohmann::json::parse(ss.str());
      std::string validation_error;
      if (!validation::validate_app_payload(inputTree, validation_error)) {
        bad_request(response, request, validation_error);
        return;
      }

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

  static size_t append_string_curl_write_cb(void *contents, size_t size, size_t nmemb, std::string *out) {
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_string_curl_write_cb);
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
   * @brief Parse libraryfolders.vdf to discover all configured Steam library paths.
   *
   * Returns a list of <library_root>/steamapps paths for every entry in the VDF.
   * The "path" key sits at depth==2 (inside a numbered section inside "libraryfolders").
   */
  static std::vector<std::string> parse_steam_library_folders(const std::string &vdf_path) {
    std::vector<std::string> paths;
    std::ifstream file(vdf_path);
    if (!file.is_open()) return paths;

    std::string line;
    int depth = 0;
    while (std::getline(file, line)) {
      auto start = line.find_first_not_of(" \t");
      if (start == std::string::npos) continue;
      line = line.substr(start);

      if (line == "{") { depth++; continue; }
      if (line == "}") { depth--; continue; }

      // "path" is at depth==2 (inside a numbered section inside "libraryfolders")
      if (depth != 2) continue;
      if (line.size() < 5 || line[0] != '"') continue;

      auto end_key = line.find('"', 1);
      if (end_key == std::string::npos) continue;
      std::string key = line.substr(1, end_key - 1);
      if (key != "path") continue;

      auto start_val = line.find('"', end_key + 1);
      if (start_val == std::string::npos) continue;
      auto end_val = line.find('"', start_val + 1);
      if (end_val == std::string::npos) continue;
      std::string val = line.substr(start_val + 1, end_val - start_val - 1);

      if (!val.empty()) paths.push_back(val + "/steamapps");
    }
    return paths;
  }

  /**
   * @brief Scan for installed Steam, Lutris, and Heroic games.
   */
  std::vector<fs::path> lutris_game_config_dirs() {
    return game_library::lutris_game_config_dirs(game_library::library_home_roots());
  }

  std::vector<fs::path> lutris_art_roots() {
    return game_library::lutris_art_roots(game_library::library_home_roots());
  }

  std::string lutris_image_path_for_app_node(const nlohmann::json &app) {
    if (!app.is_object()) {
      return {};
    }

    if (app.contains("image-path") && app["image-path"].is_string() && !app["image-path"].get<std::string>().empty()) {
      return {};
    }

    const auto source = app.contains("source") && app["source"].is_string() ? app["source"].get<std::string>() : "";
    if (!boost::iequals(source, "lutris")) {
      return {};
    }

    const auto slug = app.contains("lutris-slug") && app["lutris-slug"].is_string() ? app["lutris-slug"].get<std::string>() : "";
    if (!game_library::is_lutris_slug_safe(slug)) {
      return {};
    }

    return game_library::find_lutris_image_path(slug, lutris_art_roots());
  }

  void hydrate_lutris_app_images(nlohmann::json &file_tree) {
    if (!file_tree.contains("apps") || !file_tree["apps"].is_array()) {
      return;
    }

    for (auto &app : file_tree["apps"]) {
      auto image_path = lutris_image_path_for_app_node(app);
      if (!image_path.empty()) {
        app["image-path"] = image_path;
      }
    }
  }

  void ensure_lutris_library_app(nlohmann::json &file_tree) {
    if (!file_tree.contains("apps") || !file_tree["apps"].is_array()) {
      file_tree["apps"] = nlohmann::json::array();
    }

    for (const auto &app : file_tree["apps"]) {
      if (!app.is_object()) {
        continue;
      }

      const auto name = app.value("name", "");
      if (boost::iequals(boost::trim_copy(name), "Lutris")) {
        return;
      }

      const auto cmd = boost::trim_copy(app.value("cmd", ""));
      if (boost::iequals(cmd, "setsid lutris") || boost::iequals(cmd, "lutris")) {
        return;
      }

      if (!app.contains("detached") || !app["detached"].is_array()) {
        continue;
      }

      for (const auto &detached : app["detached"]) {
        if (!detached.is_string()) {
          continue;
        }
        const auto value = boost::trim_copy(detached.get<std::string>());
        if (boost::iequals(value, "setsid lutris") || boost::iequals(value, "lutris")) {
          return;
        }
      }
    }

    nlohmann::json app {
      {"name", "Lutris"},
      {"uuid", ""},
      {"cmd", ""},
      {"detached", nlohmann::json::array({"setsid lutris"})},
      {"image-path", "lutris.png"},
      {"source", "lutris"},
      {"auto-detach", true},
      {"wait-all", true},
      {"exit-timeout", 5}
    };
    proc::migrate_apps(&file_tree, &app);
  }

  void scanGames(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output;
    nlohmann::json steam_games = nlohmann::json::array();

    // Get existing apps to check for duplicates
    std::set<std::string> existing_cmds;
    std::set<std::string> existing_lutris_slugs;
    std::set<std::string> existing_heroic_keys;
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
          const auto app_source = app.contains("source") && app["source"].is_string() ? app["source"].get<std::string>() : "";
          if (boost::iequals(app_source, "lutris")) {
            const auto slug = app.contains("lutris-slug") && app["lutris-slug"].is_string() ? app["lutris-slug"].get<std::string>() : "";
            if (!slug.empty()) {
              existing_lutris_slugs.insert(slug);
            }
          } else if (boost::iequals(app_source, "heroic")) {
            const auto store = app.contains("heroic-store") && app["heroic-store"].is_string() ?
              app["heroic-store"].get<std::string>() : "";
            const auto app_name = app.contains("heroic-app-name") && app["heroic-app-name"].is_string() ?
              app["heroic-app-name"].get<std::string>() : "";
            if (!game_library::heroic_runner_for_store(store).empty() &&
                game_library::is_heroic_app_name_safe(app_name)) {
              existing_heroic_keys.insert(store + "/" + app_name);
            }
          }
        }
      }
    } catch (...) {}

    // Scan Steam appmanifest files
    std::vector<std::string> steam_paths;
    std::set<std::string> seen_steam_paths;
    std::set<std::string> seen_appids;
    auto append_steam_path = [&](const fs::path &path) {
      const auto value = path.lexically_normal().string();
      if (!value.empty() && seen_steam_paths.insert(value).second) {
        steam_paths.push_back(value);
      }
    };
    for (const auto &home : game_library::library_home_roots()) {
      append_steam_path(home / ".steam/steam/steamapps");
      append_steam_path(home / ".local/share/Steam/steamapps");

      const std::vector<std::string> vdf_candidates = {
        (home / ".steam/steam/steamapps/libraryfolders.vdf").string(),
        (home / ".local/share/Steam/steamapps/libraryfolders.vdf").string(),
        (home / ".local/share/Steam/config/libraryfolders.vdf").string(),
      };
      for (const auto &vdf : vdf_candidates) {
        for (const auto &lib_path : parse_steam_library_folders(vdf)) {
          append_steam_path(lib_path);
        }
      }
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

        // stoi stops at the first non-digit, so "99999 rm -rf" would pass the
        // range filter below with the junk still attached. The appid reaches a
        // launch command and a cover filename, so require it to be digits.
        if (!game_artwork::is_valid_steam_appid(fields["appid"])) continue;

        // Skip tools/redistributables (appid < 100000 or name contains "Redistributable" or "Proton")
        int appid_int = 0;
        try { appid_int = std::stoi(fields["appid"]); } catch (...) {}
        if (appid_int < 10000) continue;
        if (fields["name"].find("Redistributable") != std::string::npos) continue;
        if (fields["name"].find("Proton") != std::string::npos) continue;
        if (fields["name"].find("Steam Linux Runtime") != std::string::npos) continue;
        if (fields["name"].find("Steamworks") != std::string::npos) continue;

        auto launch_cmds = steam_library_launch_commands(fields["appid"]);
        bool already = std::all_of(launch_cmds.begin(), launch_cmds.end(), [&](const auto &cmd) {
          return existing_cmds.count(cmd) > 0;
        });

        nlohmann::json game;
        game["appid"] = fields["appid"];
        game["name"] = fields["name"];
        game["cover_url"] = "https://steamcdn-a.akamaihd.net/steam/apps/" + fields["appid"] + "/library_600x900_2x.jpg";
        game["cmd"] = launch_cmds.empty() ? "" : launch_cmds.front();
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
    const auto home_roots = game_library::library_home_roots();
    if (!home_roots.empty()) {
      auto lutris_yml_dirs = lutris_game_config_dirs();
      auto lutris_roots = lutris_art_roots();

      for (const auto &lutris_game : game_library::scan_lutris_library(lutris_yml_dirs)) {
        bool already = existing_cmds.count(lutris_game.command) > 0 ||
          existing_lutris_slugs.count(lutris_game.slug) > 0;

        nlohmann::json game;
        game["name"] = lutris_game.name;
        game["slug"] = lutris_game.slug;
        game["cmd"] = lutris_game.command;
        game["source"] = "lutris";
        game["already_imported"] = already;
        if (!lutris_game.runner.empty()) {
          game["runner"] = lutris_game.runner;
        }
        auto image_path = lutris_game.image_path.empty() ?
          game_library::find_lutris_image_path(lutris_game.slug, lutris_roots) :
          lutris_game.image_path;
        if (!image_path.empty()) {
          game["image_path"] = image_path;
          game["image-path"] = image_path;
          game["cover_path"] = image_path;
        }
        auto cat = game_classifier::classify(lutris_game.name, {});
        game["game_category"] = game_classifier::category_to_string(cat);
        lutris_games.push_back(game);
      }
    }

    // Scan Heroic Games Launcher (GOG + Epic via Legendary), native and Flatpak installs
    nlohmann::json heroic_games = nlohmann::json::array();
    std::set<std::string> seen_heroic_keys;
    const auto heroic_cache_key = [](game_library::launcher_install_t install,
                                     const std::string &store,
                                     const std::string &app_name) {
      return game_library::heroic_install_name(install) + "/" + store + "/" + app_name;
    };
    const auto append_heroic_artwork = [](nlohmann::json &game, const game_library::heroic_game_t &entry) {
      if (!entry.poster_url.empty()) {
        game["poster_url"] = entry.poster_url;
      }
      if (!entry.hero_url.empty()) {
        game["hero_url"] = entry.hero_url;
      }
      // What the title installs as and what will execute it. Absent rather than
      // guessed when Heroic did not record it.
      if (!entry.platform.empty()) {
        game["platform"] = entry.platform;
      }
      if (!entry.runtime.empty()) {
        game["runtime"] = entry.runtime;
      }
      if (!entry.runtime_name.empty()) {
        game["runtime_name"] = entry.runtime_name;
      }
    };
    // The wine or proton choice lives per game under the Heroic config root, two
    // levels above the library file the entries were parsed from.
    const auto attach_heroic_runtime = [](game_library::heroic_game_t &entry, const std::filesystem::path &library_path) {
      auto runtime = game_library::heroic_runtime_for_app(
        library_path.parent_path().parent_path(),
        entry.app_name,
        entry.platform
      );
      entry.runtime = std::move(runtime.runtime);
      entry.runtime_name = std::move(runtime.runtime_name);
    };
    const auto heroic_already_imported = [&existing_cmds, &existing_heroic_keys](
                                           const std::string &store,
                                           const std::string &app_name
                                         ) {
      if (existing_heroic_keys.count(store + "/" + app_name) > 0) {
        return true;
      }

      // A title can already be published under either install's command form, including one
      // a user typed by hand before this scan could find their Flatpak.
      for (const auto &candidate : game_library::heroic_launch_commands(store, app_name)) {
        if (existing_cmds.count(candidate) > 0) {
          return true;
        }
      }

      return false;
    };

    if (!home_roots.empty()) {
      // Legendary's installed manifest owns launch truth but does not include artwork.
      // Join its entries to the same-install store cache before de-duplicating the two
      // sources so an installed title keeps Heroic's official portrait and hero art.
      std::unordered_map<std::string, game_library::heroic_game_t> heroic_cache_metadata;
      for (const auto &[cache_path, store, install] : game_library::heroic_cache_files(home_roots)) {
        if (store != "epic" || !std::filesystem::exists(cache_path)) {
          continue;
        }
        try {
          std::ifstream cache_file(cache_path);
          std::stringstream cache_payload;
          cache_payload << cache_file.rdbuf();
          for (auto &entry : game_library::parse_heroic_cache_json(cache_payload.str(), store, install)) {
            attach_heroic_runtime(entry, cache_path);
            heroic_cache_metadata.emplace(
              heroic_cache_key(entry.install, entry.store, entry.app_name),
              std::move(entry)
            );
          }
        } catch (...) {}
      }

      for (const auto &[library_path, store, install] : game_library::heroic_installed_files(home_roots)) {
        if (!std::filesystem::exists(library_path)) continue;
        try {
          std::ifstream file(library_path);
          std::stringstream payload;
          payload << file.rdbuf();
          std::vector<game_library::heroic_game_t> entries;
          if (store == "gog") {
            // GOG's installed electron-store contains identities but no titles. Join it to
            // the pre-install-overlay games cache from the same Heroic installation.
            const auto cache_path = library_path.parent_path().parent_path() / "store_cache" / "gog_library.json";
            if (!std::filesystem::exists(cache_path)) continue;
            std::ifstream cache_file(cache_path);
            std::stringstream cache_payload;
            cache_payload << cache_file.rdbuf();
            entries = game_library::parse_heroic_gog_library_json(payload.str(), cache_payload.str(), install);
          } else {
            entries = game_library::parse_heroic_installed_json(payload.str(), store, install);
          }

          for (auto &entry : entries) {
            if (const auto cached = heroic_cache_metadata.find(
                  heroic_cache_key(entry.install, entry.store, entry.app_name)
                ); cached != heroic_cache_metadata.end()) {
              entry.poster_url = cached->second.poster_url;
              entry.hero_url = cached->second.hero_url;
              entry.platform = cached->second.platform;
              entry.runtime = cached->second.runtime;
              entry.runtime_name = cached->second.runtime_name;
            }

            // Both installs can hold the same title, and the launch command follows the
            // install this entry came from.
            if (!seen_heroic_keys.insert(entry.store + "/" + entry.app_name).second) continue;

            nlohmann::json game;
            game["name"] = entry.name;
            game["app_name"] = entry.app_name;
            game["store"] = entry.store;
            game["runner"] = entry.runner;
            game["install"] = game_library::heroic_install_name(entry.install);
            game["cmd"] = entry.command;
            game["source"] = "heroic";
            game["already_imported"] = heroic_already_imported(entry.store, entry.app_name);
            append_heroic_artwork(game, entry);
            heroic_games.push_back(game);
          }
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "Failed to parse Heroic library at " << library_path.string() << ": " << e.what();
        }
      }

      // Also check Heroic library.json (a combined library cache)
      for (const auto &[library_path, store, install] : game_library::heroic_cache_files(home_roots)) {
        // GOG cache entries are deliberately not marked installed. They were joined to
        // gog_store/installed.json above and must never be imported independently.
        if (store == "gog") continue;
        if (!std::filesystem::exists(library_path)) continue;
        try {
          std::ifstream file(library_path);
          std::stringstream payload;
          payload << file.rdbuf();
          for (const auto &entry : game_library::parse_heroic_cache_json(payload.str(), store, install)) {
            // Skip what installed.json or the other install already provided
            if (!seen_heroic_keys.insert(entry.store + "/" + entry.app_name).second) continue;

            nlohmann::json game;
            game["name"] = entry.name;
            game["app_name"] = entry.app_name;
            game["store"] = entry.store;
            game["runner"] = entry.runner;
            game["install"] = game_library::heroic_install_name(entry.install);
            game["cmd"] = entry.command;
            game["source"] = "heroic";
            game["already_imported"] = heroic_already_imported(entry.store, entry.app_name);
            append_heroic_artwork(game, entry);
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
      bool imported_lutris_game = false;
      for (const auto &game : body["games"]) {
        std::string name = game.value("name", "");
        std::string source = game.value("source", "steam");
        std::string appid = game.value("appid", "");

        if (name.empty()) continue;

        // A Steam appid is digits. It is interpolated into the launch command
        // bash -lc runs and into the cover filename, so anything else silently
        // writes outside the covers directory or changes what that shell line
        // means. Nothing here grants a caller more than the app API already
        // does -- "cmd" is a free-form string by design -- but the endpoint
        // should not depend on that to stay well behaved.
        if (source == "steam" && !appid.empty() && !game_artwork::is_valid_steam_appid(appid)) {
          bad_request(response, request, "Steam appid must be numeric: " + appid);
          return;
        }

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
          app["detached"] = steam_library_launch_commands(appid);
          app["prep-cmd"] = nlohmann::json::array({
            {
              {"undo", "setsid steam -shutdown"}
            }
          });
          app["steam-appid"] = appid;
          app["steam-launch-mode"] = std::string {proc::STEAM_LAUNCH_MODE_DIRECT};

          // Download cover art from Steam CDN to local covers directory
          const std::string coverdir = platf::appdata().string() + "/covers/";
          file_handler::make_directory(coverdir);
          if (const auto cover_path = download_best_steam_cover(appid, coverdir, "steam_" + appid); cover_path) {
            app["image-path"] = *cover_path;
          } else {
            app["image-path"] = "https://steamcdn-a.akamaihd.net/steam/apps/" + appid + "/library_600x900_2x.jpg";
          }
        } else if (source == "lutris") {
          std::string slug = game.value("slug", game.value("lutris-slug", ""));
          if (!game_library::is_lutris_slug_safe(slug)) {
            BOOST_LOG(warning) << "Skipping Lutris import for [" << name << "]: missing or unsafe slug";
            continue;
          }

          app["lutris-slug"] = slug;
          app["detached"] = nlohmann::json::array({ game_library::lutris_launch_command(slug) });

          std::string runner = game.value("runner", game.value("lutris-runner", ""));
          if (!runner.empty()) {
            app["lutris-runner"] = runner;
          }

          std::string image_path = game.value("image_path", game.value("image-path", game.value("cover_path", "")));
          if (image_path.empty()) {
            image_path = game_library::find_lutris_image_path(slug, lutris_art_roots());
          }
          if (!image_path.empty()) {
            app["image-path"] = image_path;
          }
          imported_lutris_game = true;
        } else if (source == "heroic") {
          // Heroic launch data crosses an authenticated browser boundary. Treat the
          // browser's free-form cmd as display-only and rebuild from exact scanner metadata.
          const auto app_name = game.contains("app_name") && game["app_name"].is_string() ?
            game["app_name"].get<std::string>() : "";
          const auto store = game.contains("store") && game["store"].is_string() ?
            game["store"].get<std::string>() : "";
          const auto runner = game.contains("runner") && game["runner"].is_string() ?
            game["runner"].get<std::string>() : "";
          const auto install_name = game.contains("install") && game["install"].is_string() ?
            game["install"].get<std::string>() : "";
          const auto heroic = game_library::heroic_game_from_metadata(app_name, store, runner, install_name);
          if (!heroic) {
            bad_request(response, request, "Heroic import metadata is missing or invalid");
            return;
          }

          const auto &command = heroic->command;

          bool already_imported = false;
          if (fileTree.contains("apps") && fileTree["apps"].is_array()) {
            const auto candidates = game_library::heroic_launch_commands(store, app_name);
            for (const auto &existing : fileTree["apps"]) {
              if (!existing.is_object()) continue;
              const auto string_field = [&existing](std::string_view key) {
                const auto value = existing.find(key);
                return value != existing.end() && value->is_string() ? value->get<std::string>() : "";
              };
              const auto existing_source = string_field("source");
              const auto existing_store = string_field("heroic-store");
              const auto existing_app_name = string_field("heroic-app-name");
              if (boost::iequals(existing_source, "heroic") &&
                  existing_store == store && existing_app_name == app_name) {
                already_imported = true;
                break;
              }

              const auto command_matches = [&candidates](const nlohmann::json &value) {
                return value.is_string() &&
                       std::find(candidates.begin(), candidates.end(), value.get<std::string>()) != candidates.end();
              };
              if ((existing.contains("cmd") && command_matches(existing["cmd"])) ||
                  (existing.contains("detached") && existing["detached"].is_array() &&
                   std::any_of(existing["detached"].begin(), existing["detached"].end(), command_matches))) {
                already_imported = true;
                break;
              }
            }
          }
          if (already_imported) {
            continue;
          }

          app["detached"] = nlohmann::json::array({command});
          app["heroic-app-name"] = app_name;
          app["heroic-store"] = store;
          app["heroic-runner"] = heroic->runner;
          app["heroic-install"] = install_name;

          if (const auto cached = game_library::find_heroic_cached_game(
                game_library::library_home_roots(),
                app_name,
                store,
                heroic->install
              ); cached) {
            const fs::path coverdir = platf::appdata() / "covers";
            if (const auto cover_path = download_best_heroic_cover(
                  cached->poster_url,
                  cached->hero_url,
                  coverdir,
                  "heroic_" + store + "_" + app_name
                ); cover_path) {
              app["image-path"] = *cover_path;
            }
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

      if (imported_lutris_game) {
        ensure_lutris_library_app(fileTree);
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
      int target_bitrate_kbps = input_tree.value("target_bitrate_kbps", 0);
      bool enable_legacy_ordering = input_tree.value("enable_legacy_ordering", true);
      bool allow_client_commands = input_tree.value("allow_client_commands", true);
      bool always_use_virtual_display = input_tree.value("always_use_virtual_display", false);
      auto do_cmds = nvhttp::extract_command_entries(input_tree, "do");
      auto undo_cmds = nvhttp::extract_command_entries(input_tree, "undo");
      auto perm = static_cast<crypto::PERM>(input_tree.value("perm", static_cast<uint32_t>(crypto::PERM::_no)) & static_cast<uint32_t>(crypto::PERM::_all));
      const auto result = nvhttp::update_device_info_result(
        uuid,
        name,
        display_mode,
        target_bitrate_kbps,
        do_cmds,
        undo_cmds,
        perm,
        enable_legacy_ordering,
        allow_client_commands,
        always_use_virtual_display
      );
      output_tree["status"] = result == nvhttp::client_mutation_result_t::success;
      if (result == nvhttp::client_mutation_result_t::not_found) {
        output_tree["error"] = "Paired client was not found";
      } else if (result == nvhttp::client_mutation_result_t::persistence_failed) {
        output_tree["error"] = "Paired-client update could not be persisted";
      }
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
      const auto result = nvhttp::unpair_client_result(uuid);
      output_tree["status"] = result == nvhttp::client_mutation_result_t::success;
      if (result == nvhttp::client_mutation_result_t::not_found) {
        output_tree["error"] = "Paired client was not found";
      } else if (result == nvhttp::client_mutation_result_t::persistence_failed) {
        output_tree["error"] = "Paired-client revocation could not be persisted";
      }
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

    const bool erased = nvhttp::erase_all_clients();
    if (erased) {
      proc::proc.terminate();
    }
    nlohmann::json output_tree;
    output_tree["status"] = erased;
    if (!erased) {
      output_tree["error"] = "Paired-client revocation could not be persisted";
    }
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

  void appendDeterministicDeviceSuggestionJson(nlohmann::json &output,
                                               const device_db::optimization_t &opt) {
    if (opt.display_mode) output["display_mode"] = *opt.display_mode;
    if (opt.color_range) output["color_range"] = *opt.color_range;
    if (opt.hdr.has_value()) output["hdr"] = *opt.hdr;
    if (opt.virtual_display.has_value()) output["virtual_display"] = *opt.virtual_display;
    if (opt.target_bitrate_kbps) output["target_bitrate_kbps"] = *opt.target_bitrate_kbps;
    if (opt.nvenc_tune) output["nvenc_tune"] = *opt.nvenc_tune;
    if (opt.preferred_codec) output["preferred_codec"] = *opt.preferred_codec;
    output["reasoning"] = opt.reasoning;
    output["reasoning_summary"] = opt.reasoning;
    output["source"] = opt.source;
    output["cache_status"] = opt.cache_status;
    output["confidence"] = opt.confidence;
    output["signals_used"] = opt.signals_used;
    output["normalization_reason"] = opt.normalization_reason;
    output["recommendation_version"] = opt.recommendation_version;
    output["generated_at"] = opt.generated_at;
    output["expires_at"] = opt.expires_at;
    output["authority"] = "deterministic_device_profile";
    output["may_define_settings"] = true;
  }

  void appendAiExplanationJson(nlohmann::json &output,
                               const device_db::optimization_t &result) {
    output["authority"] = "explanation_only";
    output["may_define_settings"] = false;
    output["reasoning"] = result.reasoning;
    output["reasoning_summary"] = result.reasoning;
    output["source"] = result.source;
    output["cache_status"] = result.cache_status;
    output["confidence"] = result.confidence;
    output["signals_used"] = result.signals_used;
    output["generated_at"] = result.generated_at;
    output["expires_at"] = result.expires_at;
  }

  void scrubAiSettingFields(nlohmann::json &value) {
    if (value.is_array()) {
      for (auto &entry : value) scrubAiSettingFields(entry);
      return;
    }
    if (!value.is_object()) return;
    for (const auto *field : {
           "display_mode", "color_range", "hdr", "virtual_display",
           "target_bitrate_kbps", "nvenc_tune", "preferred_codec",
           "safe_target_fps", "safe_bitrate_kbps", "safe_display_mode",
           "safe_codec", "safe_hdr"
         }) {
      value.erase(field);
    }
    for (auto &[key, entry] : value.items()) {
      (void) key;
      scrubAiSettingFields(entry);
    }
  }

  void getDeviceSuggestion(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    auto query = request->parse_query_string();
    std::string name = query.count("name") ? query.find("name")->second : "";
    std::string app = query.count("app") ? query.find("app")->second : "";

    nlohmann::json output;
    output["status"] = true;
    output["device_name"] = name;
    auto opt = device_db::get_optimization(name, app);
    appendDeterministicDeviceSuggestionJson(output, opt);
    send_response(response, output);
  }

  // ---- AI Optimizer API ----

  ai_optimizer::config_t parseAiDraftConfig(const nlohmann::json &body) {
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

    ai_optimizer::config_t ai_cfg;
    ai_cfg.enabled = get_enabled("ai_enabled", true);
    ai_cfg.provider = get_string("ai_provider");
    ai_cfg.model = get_string("ai_model");
    ai_cfg.auth_mode = get_string("ai_auth_mode");
    const bool clear_ai_api_key = body.value("clear_ai_api_key", false);
    ai_cfg.api_key = clear_ai_api_key
      ? std::string {}
      : body.contains("ai_api_key") && body["ai_api_key"].is_string() && !body["ai_api_key"].get<std::string>().empty()
        ? body["ai_api_key"].get<std::string>()
        : config::video.ai_optimizer.api_key;
    ai_cfg.base_url = get_string("ai_base_url");
    ai_cfg.use_subscription = get_enabled("ai_use_subscription", false);
    ai_cfg.codex_home = get_string("ai_codex_home", config::video.ai_optimizer.codex_home.c_str());
    ai_cfg.timeout_ms = body.value("ai_timeout_ms", config::video.ai_optimizer.timeout_ms);
    ai_cfg.cache_ttl_hours = body.value("ai_cache_ttl_hours", config::video.ai_optimizer.cache_ttl_hours);
    return ai_cfg;
  }

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
    auto cache = nlohmann::json::parse(
      ai_optimizer::get_cache_json(), nullptr, false
    );
    if (cache.is_discarded()) cache = nlohmann::json::object();
    scrubAiSettingFields(cache);
    response->write(cache.dump(), headers);
  }

  void getAiHistory(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    auto history = nlohmann::json::parse(
      ai_optimizer::get_history_json(), nullptr, false
    );
    if (history.is_discarded()) history = nlohmann::json::array();
    scrubAiSettingFields(history);
    response->write(history.dump(), headers);
  }

  void clearAiCache(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    ai_optimizer::clear_cache();
    nlohmann::json output;
    output["status"] = true;
    send_response(response, output);
  }

  /**
   * @brief Forget the host's recorded session outcomes.
   * @api_examples{/api/ai/history/clear| POST| null}
   */
  void clearAiHistory(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);
    ai_optimizer::clear_history();
    nlohmann::json output;
    output["status"] = true;
    send_response(response, output);
  }

  void getAiModels(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = nlohmann::json::parse(ss.str());

      auto ai_cfg = parseAiDraftConfig(body);

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(ai_optimizer::get_models_json_with_config(ai_cfg), headers);
    } catch (const std::exception &e) {
      nlohmann::json output;
      output["status"] = false;
      output["discovered"] = false;
      output["models"] = nlohmann::json::array();
      output["fallback_models"] = nlohmann::json::array();
      output["error"] = e.what();
      send_response(response, output);
    }
  }

  void testAiConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    nlohmann::json output;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = nlohmann::json::parse(ss.str());

      std::string device = body.value("device_name", std::string {"Test Device"});
      std::string app = body.value("app_name", std::string {"Test App"});
      std::string gpu = body.value("gpu_info", std::string {"NVIDIA RTX (NVENC)"});

      auto ai_cfg = parseAiDraftConfig(body);
      ai_cfg.enabled = true;

      auto test_result = ai_optimizer::test_provider_with_config(ai_cfg, device, app, gpu);
      if (test_result.explanation_json) {
        output = nlohmann::json::parse(*test_result.explanation_json);
        output["provider"] = ai_cfg.provider;
        output["model"] = ai_cfg.model;
        output["auth_mode"] = ai_cfg.auth_mode;
        output["base_url"] = ai_cfg.base_url;
        const auto explanation = output.value("explanation", nlohmann::json::object());
        output["reasoning"] = explanation.value("advanced_detail", explanation.value("likely_cause", std::string {}));
        output["confidence"] = explanation.value("confidence", std::string {"low"});
        output["signals_used"] = explanation.value("evidence", nlohmann::json::array());
      } else {
        output["status"] = false;
        output["code"] = test_result.code;
        output["error"] = test_result.error;
        output["detail"] = test_result.detail;
        output["action"] = test_result.action;
        output["retryable"] = test_result.retryable;
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
    send_response(response, {
      {"status", false},
      {"changed", false},
      {"state", "deprecated"},
      {"code", "ai_launch_policy_removed"},
      {"authority", "explanation_only"},
      {"may_define_settings", false},
      {"error", "AI-authored launch settings are disabled. Use a deterministic Launch preset; AI remains available only to explain Doctor evidence."}
    });
  }

  void explainDoctorWithAi(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) return;
    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = nlohmann::json::parse(ss.str());
      auto evidence = body.value("evidence", nlohmann::json::object());
      if (!evidence.is_object()) evidence = nlohmann::json::object();
      if (body.contains("deterministic_source_of_truth")) {
        evidence["deterministic_source_of_truth"] = body["deterministic_source_of_truth"];
      }
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      // Provider selection, credentials, and endpoint authority remain on the
      // host. The browser supplies only anonymized evidence to explain.
      response->write(ai_optimizer::explain_doctor_json(evidence.dump()), headers);
    } catch (const std::exception &e) {
      nlohmann::json output;
      output["status"] = false;
      output["error"] = e.what();
      send_response(response, output);
    }
  }

  namespace {
    // Doctor actions mutate the live stream, but their legitimate cadence is
    // tiny: one click, a verify re-post every 8 seconds, and possibly an undo.
    // The house 20-per-10s default (see benchmark_control_rate_limiter) never
    // brushes that while still bounding a runaway retry loop, and it runs
    // before authenticate() so unauthenticated floods are bounded too.
    confighttp::benchmark_auth::rate_limiter_t &doctor_action_rate_limiter() {
      static confighttp::benchmark_auth::rate_limiter_t limiter(20, std::chrono::seconds(10));
      return limiter;
    }
  }  // namespace

  void runDoctorAction(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json")) {
      return;
    }

    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto &limiter = doctor_action_rate_limiter();
    const auto now = std::chrono::steady_clock::now();
    const bool rate_limited = limiter.is_rate_limited(address, now);
    limiter.record_request(address, now);
    if (rate_limited) {
      BOOST_LOG(warning) << "Doctor action: ["sv << address << "] -- rate limited"sv;
      nlohmann::json tree;
      tree["status"] = false;
      tree["changed"] = false;
      tree["error"] = "Too many requests. Please try again later.";
      SimpleWeb::CaseInsensitiveMultimap headers;
      append_json_security_headers(headers);
      response->write(SimpleWeb::StatusCode::client_error_too_many_requests, tree.dump(), headers);
      return;
    }

    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      const auto body = nlohmann::json::parse(ss.str());
      const auto stats = stream_stats::get_current();
      const auto owner_uuid = proc::proc.get_session_owner_unique_id();
      const auto device_name = proc::proc.get_session_owner_device_name();
      const auto app_uuid = proc::proc.get_running_app_uuid();
      const auto app_name = proc::proc.get_last_run_app_name();
      const bool virtual_display = proc::proc.session_uses_virtual_display();
      const auto timing = stream_stats::get_session_timing(owner_uuid);
      const auto health = nvhttp::build_session_health_for_action(
        stats, virtual_display, device_name, app_name
      );
      doctor_actions::recovery_action_context_t recovery_context {
        .active_owner = !owner_uuid.empty() && proc::proc.is_session_owner(owner_uuid),
        .host_tuning_allowed = stats.streaming && !proc::proc.session_shutdown_requested(),
        .caller_is_viewer = false,
        .require_owner_scope = false,
        .enforce_request_scope = true,
        .owner_uuid = owner_uuid,
        .device_name = device_name,
        .app_uuid = app_uuid,
        .app_name = app_name,
        .launch_instance_id = proc::proc.get_session_token(),
        .session_generation = timing.session_generation,
        .effective_stream_display_mode = nvhttp::effective_stream_display_mode_for_action(
          stats, virtual_display
        ),
        .state_path = platf::appdata() / "recovery_profiles.json",
        .stats = stats,
        .health = health,
      };
      const auto output = doctor_actions::execute(body, recovery_context);
      send_response(
        response,
        static_cast<SimpleWeb::StatusCode>(doctor_actions::http_status_code(output)),
        output
      );
    } catch (const std::exception &e) {
      send_response(
        response,
        SimpleWeb::StatusCode::client_error_bad_request,
        {
          {"status", false},
          {"changed", false},
          {"state", "rejected"},
          {"code", "invalid_doctor_action_request"},
          {"error", e.what()}
        }
      );
    }
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
   * @brief Get update awareness metadata for the web Update Center.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/update-status| GET| null}
   */
  void getUpdateStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);
    send_response(response, update_status::host_update_status());
  }

  /**
   * @brief Config keys the web UI may apply without a stream relaunch.
   *
   * One source for GET /api/config and GET /api/settings/metadata so the two
   * surfaces can never disagree about which pending keys apply live.
   */
  nlohmann::json client_settings_live_config_fields_json() {
    return nlohmann::json::array({
      "max_bitrate",
      "adaptive_bitrate_enabled",
      "ai_enabled"
    });
  }

  /**
   * @brief Config keys that only take effect after a stream relaunch.
   */
  nlohmann::json client_settings_restart_config_fields_json() {
    return nlohmann::json::array({
      "linux_stream_mode",
      "fallback_mode"
    });
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

#ifdef __linux__
    const auto stream_display_mode_label = [](const std::string &selection) {
      const auto label = stream_display_policy::label_for_selection(selection);
      return label.empty() ? "Mirror Desktop"s : std::string {label};
    };
    const auto stats = stream_stats::get_current();
    // Shared resolve snapshot: one cached vdisplay probe, launch-equivalent
    // backend configuration, one labwc state, configured + effective.
    const auto labwc = snapshot_labwc();
    const auto vd_backend = virtual_display::detect_backend();
    const bool vd_available = virtual_display::backend_has_required_configuration(
      vd_backend,
      config::video.linux_display.streaming_output
    );
    const auto configured_policy = stream_display_policy::resolve(stream_display_policy::input_t {
      vd_available,
      false,
      false,
    });
    const auto effective_policy = stream_display_policy::resolve_effective(
      stream_display_policy::input_t {
        vd_available,
        false,
        stats.runtime_gpu_native_override_active,
      },
      stats.streaming,
      proc::proc.session_uses_virtual_display(),
      stats.runtime_effective_headless
    );
    // Path/runtime display: reuse configured unless live override needs a second resolve.
    const auto policy = labwc.state.gpu_native_override_active ?
      stream_display_policy::resolve(stream_display_policy::input_t {
        vd_available,
        false,
        true,
      }) :
      configured_policy;
    const auto &configured_mode = configured_policy.selection;
    const auto &effective_mode = effective_policy.selection;
#else
    const auto stream_display_mode_label = [](const std::string &selection) {
      if (selection == "headless_stream") {
        return "Private Stream"s;
      }
      if (selection == "host_virtual_display") {
        return "Host Virtual Display"s;
      }
      if (selection == "windowed_stream") {
        return "Private Stream (GPU-native)"s;
      }
      if (selection == "gamescope_stream") {
        return "Gamescope Stream"s;
      }
      return "Mirror Desktop"s;
    };
    const auto stats = stream_stats::get_current();
    const auto configured_mode = "desktop_display"s;
    const auto effective_mode = configured_mode;
#endif
    const bool client_settings_relaunch_required =
      stats.streaming && configured_mode != effective_mode;
    const auto host_header = request->header.find("host");
    const auto request_host = host_header == request->header.end() ? std::string {} : host_header->second;
    output_tree["client_settings_available"] = true;
    output_tree["client_settings_v1"] = true;
    output_tree["client_settings_endpoint"] = client_settings_endpoint_url(request_host);
    output_tree["client_settings_endpoint_path"] = std::string(CLIENT_SETTINGS_ENDPOINT);
    output_tree["client_settings_endpoint_origin"] = "gamestream_https";
    output_tree["client_settings_endpoint_same_origin"] = false;
    output_tree["client_settings_endpoint_https_port"] = client_settings_endpoint_https_port();
    output_tree["client_settings_endpoint_base_url"] = client_settings_endpoint_base_url(request_host);
    output_tree["client_settings_endpoint_url"] = client_settings_endpoint_url(request_host);
    output_tree["client_settings_sync_mode"] = "bidirectional";
    output_tree["client_settings_authority"] = "polaris_effective_runtime";
    output_tree["client_settings_relaunch_required"] = client_settings_relaunch_required;
    output_tree["client_settings_stream_display_mode"] = configured_mode;
    output_tree["client_settings_effective_stream_display_mode"] = effective_mode;
    output_tree["client_settings_stream_display_mode_label"] = stream_display_mode_label(configured_mode);
    output_tree["client_settings_effective_stream_display_mode_label"] = stream_display_mode_label(effective_mode);
    // Config keys, not GameStream sync-field names: the web UI checks its own
    // pending config keys against these to render apply-live vs restart badges.
    output_tree["client_settings_live_fields"] = client_settings_live_config_fields_json();
    output_tree["client_settings_restart_fields"] = client_settings_restart_config_fields_json();
    {
      auto response_only_keys = nlohmann::json::array();
      for (const auto key : validation::response_only_config_keys()) {
        response_only_keys.emplace_back(key);
      }
      output_tree["config_response_only_keys"] = std::move(response_only_keys);
    }
#ifdef _WIN32
    output_tree["vdisplayStatus"] = (int)proc::vDisplayDriverStatus;
#endif
#ifdef __linux__
    {
      output_tree["vdisplayAvailable"] = vd_available;
      output_tree["vdisplayBackend"] = virtual_display::backend_name(vd_backend);
      // Prefer live cage state when private labwc is running; otherwise policy backend
      // (portal / gamescope / host) so the UI never shows "Unknown".
      output_tree["runtime_backend"] = labwc.running && !labwc.state.backend_name.empty() ?
        labwc.state.backend_name :
        (policy.backend_name.empty() ? "none" : policy.backend_name);
      output_tree["runtime_requested_headless"] = policy.requested_headless;
      output_tree["runtime_effective_headless"] = labwc.running ?
        labwc.state.effective_headless :
        policy.effective_headless;
      output_tree["runtime_gpu_native_override_active"] = labwc.state.gpu_native_override_active;
      output_tree["stream_path_id"] = policy.selection;
      output_tree["stream_path_label"] = policy.label;
      output_tree["stream_display_mode_options"] = nlohmann::json::array();
      for (const auto &option : stream_display_policy::mode_options(vd_available)) {
        auto unavailable_reason = option.available ? std::string {} : option.unavailable_reason;
        if (!option.available && option.value == "host_virtual_display") {
          const auto backend_reason = virtual_display::unavailable_reason();
          if (!backend_reason.empty()) {
            unavailable_reason = backend_reason;
          }
        }
        if (!option.available && unavailable_reason.empty()) {
          unavailable_reason = "This mode is not available on this host right now.";
        }
        output_tree["stream_display_mode_options"].push_back({
          {"value", option.value},
          {"label", option.label},
          {"available", option.available},
          {"unavailable_reason", unavailable_reason},
          {"reason", option.reason},
          {"group", option.group},
          {"runtime", option.runtime},
          {"capture", option.capture},
          {"topology", option.topology},
          {"session_overridable", stream_display_policy::selection_session_overridable(option.value)},
        });
      }
    }
#else
    output_tree["stream_display_mode_options"] = nlohmann::json::array();
#endif
    auto vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
    for (auto &[name, value] : vars) {
      if (is_write_only_secret_config_key(name)) {
        if (name == "ai_api_key") {
          output_tree["has_ai_api_key"] = !value.empty();
        } else if (name == "steamgriddb_api_key") {
          output_tree["has_steamgriddb_api_key"] = !value.empty();
        } else if (name == "api_key") {
          output_tree["has_api_key"] = !value.empty();
        }
        output_tree[name] = "";
        continue;
      }
      output_tree[name] = value;
    }
    if (!output_tree.contains("browser_streaming") && output_tree.contains("webrtc_browser_streaming")) {
      output_tree["browser_streaming"] = output_tree["webrtc_browser_streaming"];
    }
    output_tree["has_ai_api_key"] = output_tree.value("has_ai_api_key", false);
    output_tree["has_steamgriddb_api_key"] = output_tree.value("has_steamgriddb_api_key", false);
    output_tree["has_api_key"] = output_tree.value("has_api_key", false);
    output_tree["ai_auto_quality_enabled"] = bool_config_value(ai_optimizer::is_enabled());
    output_tree["adaptive_bitrate_enabled"] = bool_config_value(adaptive_bitrate::is_enabled());
    output_tree["ai_enabled"] = bool_config_value(ai_optimizer::is_enabled());
    send_response(response, output_tree);
  }

  nlohmann::json build_settings_metadata_payload(const std::string &client_uuid, std::string &error) {
    auto projection = nvhttp::client_settings_projection(client_uuid);
    if (projection.empty()) {
      error = "unknown client";
      return nlohmann::json::object();
    }

    nlohmann::json output;
    output["status"] = true;
    output["version"] = 1;
    output["view"] = projection["view"];
    if (projection.contains("client")) {
      output["client"] = projection["client"];
    }
    output["fields"] = projection["fields"];
    output["sync"] = projection["sync"];
    output["stream_display"] = projection["stream_display"];
    output["write_paths"] = {
      {"web_ui", {{"endpoint", "/api/config"}, {"auth", "web_session"}}},
      {"gamestream", {{"endpoint", std::string(CLIENT_SETTINGS_ENDPOINT)}, {"auth", "paired_client_cert"}}},
      {"coordinated", false},
      {"note", "Both paths write the same underlying settings without cross-path locking; the last writer wins."}
    };
    {
      auto response_only_keys = nlohmann::json::array();
      for (const auto key : validation::response_only_config_keys()) {
        response_only_keys.emplace_back(key);
      }
      output["response_only_keys"] = std::move(response_only_keys);
    }
    output["live_fields"] = client_settings_live_config_fields_json();
    output["restart_fields"] = client_settings_restart_config_fields_json();
    output["field_map"] = {
      {"max_bitrate", "target_bitrate_kbps"},
      {"ai_enabled", "ai_optimizer_enabled"},
      {"adaptive_bitrate_enabled", "adaptive_bitrate_enabled"},
      {"linux_stream_mode", "stream_display_mode"},
      {"fallback_mode", "display_mode"},
      {"disconnect_resume_timeout_seconds", "disconnect_resume_timeout_seconds"}
    };
    output["modes"] = settings_metadata::stream_display_mode_options_json();
    output["tuning"] = settings_metadata::build_tuning_json(
      adaptive_bitrate::get_state(),
      stream_stats::get_current(),
      proc::proc.current_app_has_mangohud()
    );
    output["auto_quality"] = nvhttp::auto_quality_status_json();
    output["provenance"] = settings_metadata::config_write_provenance_json();
    if (client_uuid.empty()) {
      auto clients = nlohmann::json::array();
      for (const auto &client : nvhttp::get_all_clients()) {
        clients.push_back({
          {"uuid", client.value("uuid", std::string {})},
          {"name", client.value("name", std::string {})},
          {"connected", client.value("connected", false)},
          {"display_mode", client.value("display_mode", std::string {})},
          {"target_bitrate_kbps", client.value("target_bitrate_kbps", 0)}
        });
      }
      output["clients"] = std::move(clients);
    }
    return output;
  }

  /**
   * @brief Read-only projection of every settings surface for the Web UI.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/settings/metadata| GET| null}
   */
  void getSettingsMetadata(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::string client_uuid;
    for (const auto &[name, value] : request->parse_query_string()) {
      if (name == "client") {
        client_uuid = value;
      }
    }

    std::string error;
    auto output = build_settings_metadata_payload(client_uuid, error);
    if (!error.empty()) {
      send_response(
        response,
        SimpleWeb::StatusCode::client_error_not_found,
        nlohmann::json {{"status", false}, {"error", error}}
      );
      return;
    }
    send_response(response, output);
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
      std::stringstream config_stream;
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      std::string validation_error;
      for (auto it = input_tree.begin(); it != input_tree.end();) {
        if (validation::is_response_only_config_key(it.key())) {
          it = input_tree.erase(it);
        } else {
          ++it;
        }
      }
      if (!validation::validate_config_payload(input_tree, validation_error)) {
        bad_request(response, request, validation_error);
        return;
      }
      validation::normalize_write_only_secret_payload(input_tree);
      const auto existing_vars = config::parse_config(file_handler::read_file(config::sunshine.config_file.c_str()));
      for (const auto &[k, v] : input_tree.items()) {
        if (v.is_null()) {
          continue;
        }

        if (v.is_string() && v.get<std::string>().empty() && !is_write_only_secret_config_key(k)) {
          continue;
        }

        // v.dump() will dump valid json, which we do not want for strings in the config right now
        // we should migrate the config file to straight json and get rid of all this nonsense
        config_stream << k << " = " << (v.is_string() ? v.get<std::string>() : v.dump()) << std::endl;
      }
      for (const auto &[key, value] : existing_vars) {
        if (!is_write_only_secret_config_key(key) || input_tree.contains(key) || value.empty()) {
          continue;
        }
        config_stream << key << " = " << value << std::endl;
      }
      if (file_handler::write_file(config::sunshine.config_file.c_str(), config_stream.str()) != 0) {
        const std::string message = "Failed to write config file: " + config::sunshine.config_file;
        BOOST_LOG(error) << "SaveConfig: "sv << message;
        bad_request(response, request, message);
        return;
      }
      {
        std::vector<std::string> written_keys;
        for (const auto &[k, v] : input_tree.items()) {
          if (v.is_null() || (v.is_string() && v.get<std::string>().empty() && !is_write_only_secret_config_key(k))) {
            continue;
          }
          written_keys.push_back(k);
        }
        settings_metadata::note_config_write("web_ui", std::move(written_keys));
      }
      if (input_tree.contains("adaptive_bitrate_enabled")) {
        doctor_actions::set_adaptive_enabled(
          json_config_enabled(input_tree["adaptive_bitrate_enabled"])
        );
      }
      if (input_tree.contains("ai_enabled")) {
        ai_optimizer::set_enabled(json_config_enabled(input_tree["ai_enabled"]));
      }
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
        if (!igdb_image_url_allowed(url)) {
          bad_request(response, request, "Only images.igdb.com is allowed");
          return;
        }
        if (!http::download_file(url, path, igdb_image_url_allowed)) {
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
      try {
        std::string content = file_handler::read_file(config::stream.file_apps.c_str());
        auto file_tree = nlohmann::json::parse(content);
        if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
          for (const auto &app : file_tree["apps"]) {
            if (!app.is_object() || app.value("name", "") != name_it->second) {
              continue;
            }

            image_path = app.value("image-path", "");
            if (image_path.empty()) {
              image_path = lutris_image_path_for_app_node(app);
            }
            break;
          }
        }
      } catch (...) {}
    }

    if (image_path.empty()) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/plain");
      response->write(SimpleWeb::StatusCode::client_error_not_found, "No cover art", headers);
      return;
    }

    std::string resolved = proc::validate_app_image_path(image_path);
    std::string extension = fs::path(resolved).extension().string();
    boost::to_lower(extension);

    std::ifstream in(resolved, std::ios::binary);
    if (!in) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "text/plain");
      response->write(SimpleWeb::StatusCode::client_error_not_found, "Image file not found", headers);
      return;
    }

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    SimpleWeb::CaseInsensitiveMultimap headers;
    const auto content_type =
      extension == ".jpg" || extension == ".jpeg" ? "image/jpeg" :
      extension == ".webp" ? "image/webp" :
      "image/png";
    headers.emplace("Content-Type", content_type);
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
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_string_curl_write_cb);
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

      // Only allow SteamGridDB / Steam CDN URLs. A substring match let anything
      // through that merely contained the name -- "http://10.0.0.1/?x=steamgriddb.com"
      // passed, and download_file follows redirects. game_artwork already keeps
      // the parsed, exact-host allowlist for these two providers.
      if (!game_artwork::is_allowed_provider_url(game_artwork::provider_e::steamgriddb, url) &&
          !game_artwork::is_allowed_provider_url(game_artwork::provider_e::steam, url)) {
        output["status"] = false;
        output["error"] = "Only SteamGridDB/Steam CDN URLs allowed";
        send_response(response, output);
        return;
      }

      // The uuid becomes part of a filesystem path, so resolve it against the
      // app list and build from the stored value rather than the request's.
      // uploadCover escapes its key for this reason; this handler pasted the
      // request body straight in, so "../.." escaped the covers directory.
      const auto apps = proc::proc.get_apps();
      const auto app_entry = std::find_if(apps.begin(), apps.end(), [&](const proc::ctx_t &candidate) {
        return boost::iequals(candidate.uuid, app_uuid);
      });
      if (app_entry == apps.end()) {
        output["status"] = false;
        output["error"] = "Unknown app_uuid";
        send_response(response, output);
        return;
      }

      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);
      std::string cover_path = coverdir + http::url_escape(app_entry->uuid) + safe_cover_extension_from_url(url);

      if (!http::download_file(url, cover_path, cover_download_url_allowed)) {
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
   * @brief Get a versioned, bounded tail of the active log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Query parameters are strict decimal values: `max_bytes`, `max_lines`, and
   * optional `after` plus `after_generation`. The payload is Base64 encoded so
   * every source byte is representable in JSON. `reset` tells incremental
   * callers whether the returned content must replace their local state.
   *
   * @api_examples{/polaris/v1/diagnostics/logs/tail| GET| ?max_bytes=262144&max_lines=2000&after=0&after_generation=1}
   */
  void getLogTail(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    try {
      const auto query = request->parse_query_string();
      for (const auto &entry : query) {
        const auto &name = entry.first;
        if (name != "max_bytes" && name != "max_lines" && name != "after" && name != "after_generation") {
          bad_request(response, request, "Unknown query parameter: " + name);
          return;
        }
      }

      const auto query_value = [&](const std::string &name) -> std::optional<std::string_view> {
        if (query.count(name) > 1) {
          throw std::invalid_argument(name + " must appear at most once");
        }
        const auto value = query.find(name);
        if (value == query.end()) {
          return std::nullopt;
        }
        return value->second;
      };

      const auto tail_request = log_tail_api::parse_request(
        query_value("max_bytes"),
        query_value("max_lines"),
        query_value("after"),
        query_value("after_generation")
      );

      std::optional<file_handler::tail_result_t> stable_tail;
      std::uint64_t stable_generation = 0;
      for (int attempt = 0; attempt < 3; ++attempt) {
        const auto generation_before = logging::log_file_generation();
        auto candidate = file_handler::read_file_tail(config::sunshine.log_file.c_str(), tail_request.max_bytes);
        const auto generation_after = logging::log_file_generation();
        if (generation_before == generation_after) {
          stable_tail = std::move(candidate);
          stable_generation = generation_after;
          break;
        }
      }
      if (!stable_tail) {
        nlohmann::json output = {
          {"status", false},
          {"error", "The active log changed repeatedly; retry the request."},
        };
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Retry-After", "1");
        headers.emplace("Cache-Control", "no-store");
        append_json_security_headers(headers);
        response->write(SimpleWeb::StatusCode::server_error_service_unavailable, output.dump(), headers);
        return;
      }

      auto output = log_tail_api::serialize_response(log_tail_api::build_response(
        std::move(*stable_tail),
        tail_request,
        stable_generation
      ));
      output["media_type"] = "text/plain";
  #ifdef _WIN32
      output["charset"] = currentCodePageToCharset();
  #else
      output["charset"] = "utf-8";
  #endif

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Cache-Control", "no-store");
      append_json_security_headers(headers);
      response->write(SimpleWeb::StatusCode::success_ok, output.dump(), headers);
    } catch (const std::invalid_argument &error) {
      bad_request(response, request, error.what());
    }
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

    const auto tail = file_handler::read_file_tail(config::sunshine.log_file.c_str(), log_tail_api::legacy_max_bytes);
    SimpleWeb::CaseInsensitiveMultimap headers;
    std::string contentType = "text/plain";
  #ifdef _WIN32
    contentType += "; charset=";
    contentType += currentCodePageToCharset();
  #endif
    headers.emplace("Content-Type", contentType);
    headers.emplace("Cache-Control", "no-store");
    headers.emplace("X-Polaris-Log-Start-Offset", std::to_string(tail.start_offset));
    headers.emplace("X-Polaris-Log-End-Offset", std::to_string(tail.end_offset));
    headers.emplace("X-Polaris-Log-Truncated", tail.truncated ? "true" : "false");
    append_common_security_headers(headers);
    response->write(SimpleWeb::StatusCode::success_ok, tail.content, headers);
  }

  /**
   * @brief Get the retained log of the run before this one.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * A run that crashed cannot serve its own log: by the time anyone asks, the
   * process has restarted and the active log describes the new run. The bounded
   * backup preserved at startup is the only copy of what the failing run said,
   * which is what makes this the log a crash report needs.
   *
   * Served as plain bytes rather than the Base64 JSON of the tail endpoint,
   * matching `/api/logs`, because this is a whole-file fetch for an export
   * rather than an incremental reader that has to track offsets.
   *
   * @api_examples{/polaris/v1/diagnostics/logs/previous| GET| null}
   */
  void getPreviousLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    const auto backup_path = logging::backup_log_path(config::sunshine.log_file);
    std::error_code exists_error;
    const bool present = !backup_path.empty() && std::filesystem::exists(backup_path, exists_error);

    file_handler::tail_result_t tail;
    if (present) {
      tail = file_handler::read_file_tail(backup_path.c_str(), log_tail_api::legacy_max_bytes);
    }

    SimpleWeb::CaseInsensitiveMultimap headers;
    std::string contentType = "text/plain";
  #ifdef _WIN32
    contentType += "; charset=";
    contentType += currentCodePageToCharset();
  #endif
    headers.emplace("Content-Type", contentType);
    headers.emplace("Cache-Control", "no-store");
    // An absent backup and an empty one are different states: the first means
    // there was no prior run to preserve, the second that it logged nothing.
    headers.emplace("X-Polaris-Log-Present", present ? "true" : "false");
    headers.emplace("X-Polaris-Log-Start-Offset", std::to_string(tail.start_offset));
    headers.emplace("X-Polaris-Log-End-Offset", std::to_string(tail.end_offset));
    headers.emplace("X-Polaris-Log-Truncated", tail.truncated ? "true" : "false");
    append_common_security_headers(headers);
    response->write(SimpleWeb::StatusCode::success_ok, tail.content, headers);
  }

  /**
   * @brief Get how the run before this one ended.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Reports `clean`, `crashed`, `unclean`, or `unknown`, with the fatal signal
   * and captured backtrace when there was one. This is what lets a support
   * report state that Polaris crashed instead of leaving the user to work it
   * out from `coredumpctl` by hand.
   *
   * @api_examples{/polaris/v1/diagnostics/last-run| GET| null}
   */
  void getLastRun(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    auto output = crash_report::to_json(crash_report::previous_run());
    output["status"] = true;

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Cache-Control", "no-store");
    append_json_security_headers(headers);
    response->write(SimpleWeb::StatusCode::success_ok, output.dump(), headers);
  }

  /**
   * @brief Get support reports submitted by paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * Held in memory only and never written to host disk: this is content a paired
   * device supplied, and a support feature should not double as a way for one to
   * leave files behind.
   *
   * Values are passed through as the client sent them. The client redacts before
   * sending and the export layer redacts again on the way out, so this is not the
   * place for a third copy of those rules; it is the place that must not assume
   * the client got it right.
   *
   * @api_examples{/polaris/v1/diagnostics/client-reports| GET| null}
   */
  void getClientSupportReports(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    nlohmann::json output;
    output["status"] = true;
    output["reports"] = client_support_report::to_json();

    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Cache-Control", "no-store");
    append_json_security_headers(headers);
    response->write(SimpleWeb::StatusCode::success_ok, output.dump(), headers);
  }

  /**
   * @brief Clear the active log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs/clear| POST| null}
   */
  void clearLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json output;
    output["status"] = logging::clear_log_file();
    if (!output["status"].get<bool>()) {
      output["error"] = "Failed to clear active log file";
    }

    send_response(response, output);
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
    std::lock_guard credential_lock {s_credential_lifecycle_mutex};
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
        if (config::sunshine.username.empty() || http::verify_user_password(username, password)) {
          if (newPassword.empty() || newPassword != confirmPassword)
            errors.push_back("Password Mismatch");
          else {
            const auto save_result = http::save_user_creds(
              config::sunshine.credentials_file,
              newUsername,
              newPassword
            );
            if (save_result != 0) {
              errors.push_back("Failed to persist new credentials");
            } else {
              const auto reload_result = http::reload_user_creds(config::sunshine.credentials_file);
              if (reload_result != 0) {
                s_loaded_credentials_available = false;
                s_web_sessions.reset();
                write_session_persistence_error(
                  response,
                  "Credentials changed, but the Web UI authorization state could not be reloaded. Restart Polaris before signing in again."
                );
                return;
              }
              s_loaded_credentials_available = true;
              const auto credential_fingerprint = web_session_credential_fingerprint();
              const auto session = create_web_session(credential_fingerprint);
              if (!session) {
                write_session_persistence_error(response, "Credentials changed, but the replacement Web UI session could not be persisted.");
                return;
              }
              output_tree["status"] = true;
              auto headers = make_auth_cookie_headers(*session);
              headers.emplace("Content-Type", "application/json");
              response->write(output_tree.dump(), headers);
              return;
            }
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
      const auto access_preset_name = input_tree.value(
        "access_preset",
        std::string {nvhttp::DEFAULT_PAIRING_ACCESS_PRESET}
      );
      const auto access_preset = nvhttp::pairing_access_preset_from_view(access_preset_name);
      if (!access_preset) {
        bad_request(response, request, "Invalid access_preset");
        return;
      }

      output_tree["otp"] = nvhttp::request_otp(
        passphrase,
        deviceName,
        nvhttp::pairing_access_preset_perm(*access_preset)
      );
      output_tree["ip"] = platf::get_local_ip_for_gateway();
      output_tree["name"] = config::nvhttp.sunshine_name;
      output_tree["access_preset"] = std::string {nvhttp::pairing_access_preset_name(*access_preset)};
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
      const auto access_preset_name = input_tree.value(
        "access_preset",
        std::string {nvhttp::DEFAULT_PAIRING_ACCESS_PRESET}
      );
      const auto access_preset = nvhttp::pairing_access_preset_from_view(access_preset_name);
      if (!access_preset) {
        bad_request(response, request, "Invalid access_preset");
        return;
      }

      output_tree["status"] = nvhttp::pin(
        pin,
        name,
        nvhttp::pairing_access_preset_perm(*access_preset)
      );
      output_tree["access_preset"] = std::string {nvhttp::pairing_access_preset_name(*access_preset)};
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
   * @brief Clean up stale persisted virtual display state.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/virtual-display/cleanup-stale| POST| null}
   */
  void cleanupStaleVirtualDisplay(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
#ifdef __linux__
    output_tree["status"] = true;
    output_tree["cleaned"] = virtual_display::cleanup_stale();
#else
    output_tree["status"] = false;
    output_tree["error"] = "Virtual display cleanup is only available on Linux";
#endif
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

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["restarting"] = true;
    send_response(response, output_tree);

    std::thread([]() {
      std::this_thread::sleep_for(250ms);
      proc::proc.terminate();

      // We may not return from this call
      platf::restart();
    }).detach();
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
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true, "quit requested from the web UI");
    } else
#endif
    {
      lifetime::exit_sunshine(0, true, "quit requested from the web UI");
    }
    // If exit fails, write a response after 5 seconds.
    launch_background_task([response] {
      std::this_thread::sleep_for(5s);
      response->write();
    });
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
      auto launch_args = request->parse_query_string();
      const auto launch_mode = boost::to_lower_copy(input_tree.value("launchMode", std::string {}));
      bool mirror_desktop_explicit = input_tree.value("mirrorDesktop", false) ||
                                     input_tree.value("mirror_desktop", false) ||
                                     launch_mode == "mirror_desktop" ||
                                     launch_mode == "mirrordesktop";
      const bool force_private_after_desktop_steam_shutdown =
        input_tree.value("closeDesktopSteamForPrivate", false) ||
        input_tree.value("forcePrivateAfterSteamClose", false) ||
        launch_mode == "force_private_stream" ||
        launch_mode == "forceprivate";
      if (mirror_desktop_explicit || force_private_after_desktop_steam_shutdown) {
        launch_args.erase("mirrorDesktop");
        launch_args.erase("mirror_desktop");
        launch_args.erase("closeDesktopSteamForPrivate");
        launch_args.erase("forcePrivateAfterSteamClose");
        launch_args.erase("launchMode");
      }
      if (mirror_desktop_explicit) {
        launch_args.emplace("mirrorDesktop", "1");
        launch_args.emplace("launchMode", "mirror_desktop");
      } else if (force_private_after_desktop_steam_shutdown) {
        launch_args.emplace("closeDesktopSteamForPrivate", "1");
        launch_args.emplace("launchMode", "force_private_stream");
      }

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
#ifdef __linux__
          const bool private_stream_requested =
            config::video.linux_display.headless_mode &&
            config::video.linux_display.use_cage_compositor;
          const int running_app = proc::proc.running();
          const auto launch_policy = proc::resolve_desktop_launch_safety_policy(
            private_stream_requested,
            mirror_desktop_explicit,
            force_private_after_desktop_steam_shutdown,
            app,
            proc::desktop_steam_client_active(),
            running_app > 0 && running_app != proc::input_only_app_id
          );
          const auto launch_policy_json = proc::desktop_launch_safety_policy_to_json(launch_policy);
          if (launch_policy.recommendedAction == "refuse_private_stream") {
            BOOST_LOG(warning) << "launch_policy: refusing private stream; desktop_steam_active="sv
                               << launch_policy.desktopSteamActive
                               << " physical_display_risk="sv
                               << launch_policy.physicalDisplayRisk;
            nlohmann::json error_tree;
            error_tree["status_code"] = static_cast<int>(SimpleWeb::StatusCode::client_error_conflict);
            error_tree["status"] = false;
            error_tree["error"] = "Unsafe private stream launch refused because desktop Steam or a desktop game is active. Quit the desktop session or retry with explicit desktop mirroring.";
            error_tree["error_code"] = "desktop_active_private_stream_refused";
            error_tree["launchPolicy"] = launch_policy_json;
            conflict_response(response, error_tree);
            return;
          }
#endif
          auto launch_session = nvhttp::make_launch_session(true, false, launch_args, &named_cert);
          if (!launch_session) {
            bad_request(response, request, "Failed to build a launch session");
            return;
          }
          auto err = proc::proc.execute(app, launch_session);
          if (err) {
            nlohmann::json error_tree;
            error_tree["status_code"] = static_cast<int>(SimpleWeb::StatusCode::client_error_bad_request);
            error_tree["status"] = false;
            error_tree["error"] = err == 503 ?
              "Failed to initialize video capture/encoding. Is a display connected and turned on?" :
              "Failed to start the specified application";
#ifdef __linux__
            error_tree["launchPolicy"] = launch_policy_json;
#endif
            SimpleWeb::CaseInsensitiveMultimap headers;
            append_json_security_headers(headers);
            response->write(SimpleWeb::StatusCode::client_error_bad_request, error_tree.dump(), headers);
          } else {
            output_tree["status"] = true;
#ifdef __linux__
            output_tree["launchPolicy"] = launch_policy_json;
#endif
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

      // Host-operator path: WebUI disconnect fully ends the stream. Answer HTTPS
      // first, then session_media worker runs media teardown + nested kill.
      output_tree["status"] = true;
      output_tree["force"] = true;
      BOOST_LOG(info) << "WebUI disconnect: responding before capture/nested teardown"sv;
      send_response(response, output_tree);

#ifdef __linux__
      session_media::schedule([uuid = std::move(uuid)]() {
        try {
          BOOST_LOG(info) << "WebUI disconnect: async prepare capture/portal teardown"sv;
          session_media::prepare_for_stop();

          if (!uuid.empty()) {
            const auto shutdown = proc::proc.request_session_shutdown(uuid, {}, true, false);
            const bool stopped = shutdown.stopped ||
                                 shutdown.snapshot.outcome == proc::session_stop_outcome_t::allowed ||
                                 shutdown.snapshot.outcome == proc::session_stop_outcome_t::no_active_session;
            if (!stopped) {
              BOOST_LOG(info) << "WebUI disconnect: force-stop after outcome="
                              << static_cast<int>(shutdown.snapshot.outcome);
              rtsp_stream::terminate_sessions();
              proc::proc.terminate();
            }
            nvhttp::find_and_stop_session(uuid, true);
          }
          else {
            bool stopped = false;
            const auto owner = proc::proc.get_session_owner_unique_id();
            if (!owner.empty()) {
              const auto shutdown = proc::proc.request_session_shutdown(owner, {}, true, false);
              stopped = shutdown.stopped ||
                        shutdown.snapshot.outcome == proc::session_stop_outcome_t::allowed ||
                        shutdown.snapshot.outcome == proc::session_stop_outcome_t::no_active_session;
            }
            if (!stopped) {
              BOOST_LOG(info) << "WebUI disconnect: force-stop active session(s)"sv;
              rtsp_stream::terminate_sessions();
              if (proc::proc.running() > 0) {
                proc::proc.terminate();
              }
            }
          }
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "WebUI disconnect: nested force-stop failed: "sv << e.what();
        }
      });
#else
      // Non-Linux: keep previous force-stop path on a detached thread.
      std::thread {[uuid = std::move(uuid)]() {
        try {
          if (!uuid.empty()) {
            const auto shutdown = proc::proc.request_session_shutdown(uuid, {}, true, false);
            if (!shutdown.stopped) {
              rtsp_stream::terminate_sessions();
              proc::proc.terminate();
            }
            nvhttp::find_and_stop_session(uuid, true);
          }
          else {
            rtsp_stream::terminate_sessions();
            if (proc::proc.running() > 0) {
              proc::proc.terminate();
            }
          }
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "WebUI disconnect: nested force-stop failed: "sv << e.what();
        }
      }}.detach();
#endif
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

    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    const auto write_login_error = [&](SimpleWeb::StatusCode code, const std::string &error_message, bool record_failure) {
      if (record_failure) {
        recordLoginFailure(address);
      }

      nlohmann::json tree;
      tree["status_code"] = static_cast<int>(code);
      tree["status"] = false;
      tree["error"] = error_message;
      SimpleWeb::CaseInsensitiveMultimap headers;
      append_json_security_headers(headers);
      response->write(code, tree.dump(), headers);
    };

    // Rate limiting: check if this IP is blocked
    if (isLoginRateLimited(address)) {
      BOOST_LOG(warning) << "Web UI: ["sv << address << "] -- rate limited (too many failed login attempts)"sv;
      nlohmann::json tree;
      tree["status"] = false;
      tree["error"] = "Too many failed attempts. Please try again later.";
      SimpleWeb::CaseInsensitiveMultimap headers;
      append_json_security_headers(headers);
      response->write(SimpleWeb::StatusCode::client_error_too_many_requests, tree.dump(), headers);
      return;
    }

    std::lock_guard credential_lock {s_credential_lifecycle_mutex};
    if (!s_loaded_credentials_available) {
      write_session_validation_unavailable(response);
      return;
    }
    if (config::sunshine.username.empty()) {
      write_login_error(
        SimpleWeb::StatusCode::client_error_conflict,
        "No web credentials are configured yet. Finish setup on the welcome page.",
        false
      );
      return;
    }

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      std::string username = input_tree.value("username", "");
      std::string password = input_tree.value("password", "");
      bool needs_upgrade = false;
      if (!http::verify_user_password(username, password, &needs_upgrade)) {
        write_login_error(
          SimpleWeb::StatusCode::client_error_unauthorized,
          "Invalid username or password. If you reset credentials with --creds, restart Polaris before signing in again.",
          true
        );
        return;
      }

      if (needs_upgrade) {
        const auto upgrade_result = http::upgrade_user_password_hash(
          config::sunshine.credentials_file,
          username,
          password
        );
        if (upgrade_result == http::credential_upgrade_status_e::reload_failed) {
          s_loaded_credentials_available = false;
          s_web_sessions.reset();
          write_session_validation_unavailable(response);
          return;
        }
        if (upgrade_result == http::credential_upgrade_status_e::save_failed) {
          write_session_persistence_error(response, "The Web UI credential upgrade could not be persisted.");
          return;
        }
      }

      const auto credential_fingerprint = web_session_credential_fingerprint();
      const auto session = create_web_session(credential_fingerprint);
      if (!session) {
        write_session_persistence_error(response, "The Web UI session could not be persisted.");
        return;
      }
      clearLoginFailures(address);
      auto headers = make_auth_cookie_headers(*session);
      response->write(headers);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Web UI Login failed: ["sv << address
                               << "]: "sv << e.what();
      write_login_error(
        SimpleWeb::StatusCode::client_error_bad_request,
        "Invalid login request.",
        false
      );
      return;
    }
  }

  void logout(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    auto cookies = request->header.find("cookie");
    if (cookies != request->header.end() &&
        !invalidate_web_session_cookie(getCookieValue(cookies->second, "auth"))) {
      write_session_persistence_error(response, "The Web UI session could not be revoked.");
      return;
    }

    nlohmann::json output_tree;
    output_tree["status"] = true;
    auto headers = clear_auth_cookie_headers();
    headers.emplace("Content-Type", "application/json");
    response->write(output_tree.dump(), headers);
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

    session_manager::repair_desktop_session_environment();

    auto tid = std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::string tmpfile = "/tmp/polaris_preview_" + tid + ".png";
    std::string outfile = tmpfile;
    auto query = request->parse_query_string();
    std::string requested_output;
    if (const auto it = query.find("output"); it != query.end()) {
      requested_output = it->second;
    }

    std::string preview_backend;
    const bool captured = try_stream_path_preview_capture(requested_output, tmpfile, preview_backend);
    if (captured) {
      BOOST_LOG(debug) << "display_preview: captured via "sv << preview_backend;
    }

    if (!captured) {
      nlohmann::json err;
      err["status"] = false;
      err["error"] = "Screenshot capture failed for the active stream path (tried gamescopectl, labwc grim, host Wayland, last-frame cache, spectacle)";
      err["preview_backend"] = preview_backend;
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

    session_manager::repair_desktop_session_environment();

    // Parse params
    int target_fps = 5;
    auto query = request->parse_query_string();
    std::string requested_output;
    if (query.count("fps")) {
      try { target_fps = std::clamp(std::stoi(query.find("fps")->second), 1, 15); } catch (...) {}
    }
    if (const auto it = query.find("output"); it != query.end()) {
      requested_output = it->second;
    }

    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    response->close_connection_after_response = true;

    launch_background_task([response, target_fps, shutdown_event, requested_output]() {
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

        std::string preview_backend;
        const bool captured = try_stream_path_preview_capture(requested_output, tmpfile, preview_backend);
        if (!captured) {
          break;
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
    });
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

    const bool backend_detected = cached_backend != virtual_display::backend_e::NONE;
    const bool available = virtual_display::backend_has_required_configuration(
      cached_backend,
      config::video.linux_display.streaming_output
    );
    output_tree["available"] = available;
    const auto labwc = snapshot_labwc();
    const auto display_policy = stream_display_policy::resolve(stream_display_policy::input_t {
      available,
      video::active_encoder_requires_gpu_native_capture(),
      labwc.state.gpu_native_override_active,
    });
    output_tree["backend"] = virtual_display::backend_name(cached_backend);
    output_tree["backend_id"] = static_cast<int>(cached_backend);
    output_tree["backend_detected"] = backend_detected;
    output_tree["configuration_ready"] = available;
    output_tree["unavailable_reason"] = available ? "" : virtual_display::unavailable_reason();
    output_tree["configured_adapter"] = config::video.adapter_name;
    output_tree["policy_mode"] = display_policy.selection;
    output_tree["policy_label"] = display_policy.label;
    output_tree["policy_reason"] = display_policy.reason;
    output_tree["runtime_backend"] = labwc.state.backend_name;
    output_tree["runtime_effective_headless"] = labwc.state.effective_headless;

    send_response(response, output_tree);
  }

  void getBrowserStreamStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);
    send_response(response, browser_stream::status_json());
  }

  void postBrowserStreamStart(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    auto remote_address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (net::from_address(remote_address) > net::LAN) {
      forbidden(response, request, "Browser Stream is LAN-only");
      return;
    }

    if (!browser_stream::build_enabled()) {
      bad_request(response, request, "Browser Stream was not enabled at build time");
      return;
    }

    if (!config::video.browser_streaming) {
      bad_request(response, request, "Browser Stream is disabled in configuration");
      return;
    }

    std::string app_uuid;
    std::string stream_profile = "balanced";
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      if (auto body = ss.str(); !body.empty()) {
        const auto input = nlohmann::json::parse(body);
        app_uuid = input.value("app_uuid", input.value("uuid", ""));
        stream_profile = input.value("stream_profile", input.value("profile", stream_profile));
      }
    } catch (const std::exception &e) {
      bad_request(response, request, e.what());
      return;
    }

    const auto host_header = request->header.find("host");
    const auto host = host_header == request->header.end() ? remote_address : host_header->second;
    send_response(response, browser_stream::create_session(remote_address, host, app_uuid, stream_profile));
  }

  void postBrowserStreamStop(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);

    std::string token;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      if (auto body = ss.str(); !body.empty()) {
        const auto input = nlohmann::json::parse(body);
        token = input.value("session_token", "");
      }
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "BrowserStreamStop: "sv << e.what();
    }

    // Answer HTTPS before portal/PW work. Token stop without media; schedule
    // media teardown on the session_media worker after the response flushes.
    browser_stream::stop_session_result_t result;
    if (!token.empty()) {
      result = browser_stream::stop_session(
        token,
        /*terminate_owned_app=*/false,
        /*release_media=*/false
      );
    }

    nlohmann::json output;
    // Operator force-stop always acknowledges so the gate/WebUI get a body
    // even if residual media teardown is still draining PipeWire.
    output["status"] = true;
    output["stopped"] = true;
    output["token_matched"] = result.stopped;
    output["media_draining"] = true;
    BOOST_LOG(info) << "BrowserStreamStop: responding before teardown token_ok="sv
                    << result.stopped << " owns_app="sv << result.owns_app;
    send_response(response, output);

    const bool owns_app = result.owns_app;
#ifdef __linux__
    session_media::schedule([owns_app]() {
      try {
        BOOST_LOG(info) << "BrowserStreamStop: async prepare capture/portal teardown"sv;
        session_media::prepare_for_stop();
        if (owns_app) {
          BOOST_LOG(info) << "BrowserStreamStop: async terminate owned app"sv;
          proc::proc.terminate(false, false);
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "BrowserStreamStop: async teardown failed: "sv << e.what();
      }
    });
#else
    std::thread {[owns_app]() {
      try {
        browser_stream::prepare_for_session_teardown();
        if (owns_app) {
          proc::proc.terminate(false, false);
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "BrowserStreamStop: async teardown failed: "sv << e.what();
      }
    }}.detach();
#endif
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

    if (virtual_display::destroy(*ui_vdisplay)) {
      ui_vdisplay.reset();
      output_tree["status"] = true;
    } else {
      output_tree["status"] = false;
      output_tree["error"] = "Virtual display teardown could not be verified; recovery state was retained";
    }
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


#ifdef __linux__
  /**
   * @brief Detect the active GPU vendor.
   *
   * Prefers @c video::active_encoder_mem_type() as the primary signal since it reflects
   * what the system actually uses for encoding. Falls back to driver/sysfs presence checks
   * when no encoder has been chosen yet (i.e. before the first stream is started).
   * @return "nvidia", "amd", or "" if the vendor cannot be determined.
   */
  static std::string detect_gpu_vendor() {
    // amdgpu sysfs marker — used both for AMD detection and to distinguish AMD from Intel within vaapi
    auto has_amdgpu = []() {
      try {
        for (auto &e : fs::directory_iterator("/sys/class/drm")) {
          auto n = e.path().filename().string();
          if (n.size() < 5 || n.substr(0, 4) != "card" || n.find('-') != std::string::npos) continue;
          if (fs::exists(e.path() / "device" / "gpu_busy_percent")) return true;
        }
      } catch (...) {}
      return false;
    };

    // Prefer the active encoder type — it reflects what the system actually uses.
    switch (video::active_encoder_mem_type()) {
      case platf::mem_type_e::cuda:  return "nvidia";
      case platf::mem_type_e::vaapi: return has_amdgpu() ? "amd" : "intel";
      default: break;
    }

    // Encoder not chosen yet (no stream started) — fall back to driver presence.
    if (fs::exists("/proc/driver/nvidia")) return "nvidia";
    if (has_amdgpu())                      return "amd";
    return "";
  }

  /**
   * @brief Query NVIDIA GPU telemetry via nvidia-smi.
   * @return JSON object with GPU metrics, or null if nvidia-smi is unavailable or returns no data.
   */
  static nlohmann::json query_nvidia_gpu() {
    FILE *pipe = popen(
      "nvidia-smi --query-gpu=name,temperature.gpu,utilization.gpu,utilization.encoder,"
      "memory.used,memory.total,fan.speed,power.draw,clocks.gr,clocks.mem "
      "--format=csv,noheader,nounits 2>/dev/null",
      "r"
    );
    if (!pipe) return nullptr;

    nlohmann::json gpu = nullptr;
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
        gpu = nlohmann::json::object();
        gpu["name"] = fields[0];
        gpu["vendor"] = "nvidia";
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
      }
    }
    pclose(pipe);
    return gpu;
  }

  /**
   * @brief Query AMD GPU telemetry via the amdgpu kernel sysfs interface.
   *
   * Does not require ROCm. GPU name is resolved via lspci -vmm, with rocm-smi / amd-smi
   * as fallbacks when the local pci.ids database is outdated.
   * Encoder utilization (VCN) is only available on RDNA2+ hardware.
   * @return JSON object with GPU metrics, or null if no amdgpu device is found.
   */
  static nlohmann::json query_amd_gpu() {
    std::string dev;
    try {
      for (auto &e : fs::directory_iterator("/sys/class/drm")) {
        auto n = e.path().filename().string();
        if (n.size() < 5 || n.substr(0, 4) != "card" || n.find('-') != std::string::npos) continue;
        if (fs::exists(e.path() / "device" / "gpu_busy_percent")) {
          dev = (e.path() / "device").string();
          break;
        }
      }
    } catch (...) {}

    if (dev.empty()) return nullptr;

    auto read_sysfs = [](const std::string &path) -> std::string {
      std::ifstream f(path);
      if (!f) return {};
      std::string s;
      std::getline(f, s);
      return s;
    };

    std::string hwmon;
    try {
      for (auto &e : fs::directory_iterator(dev + "/hwmon"))
        { hwmon = e.path().string(); break; }
    } catch (...) {}

    auto active_mhz = [](const std::string &path) -> int {
      std::ifstream f(path);
      std::string line;
      while (std::getline(f, line)) {
        if (line.find('*') == std::string::npos) continue;
        auto mhz = line.find("Mhz");
        if (mhz == std::string::npos) continue;
        auto sp = line.rfind(' ', mhz);
        try { return std::stoi(line.substr(sp + 1, mhz - sp - 1)); } catch (...) {}
      }
      return 0;
    };

    nlohmann::json gpu = nlohmann::json::object();

    // Name: lspci -vmm first; if pci.ids is stale it returns a bare hex ID,
    // so fall back to rocm-smi (ROCm 5.x) then amd-smi (ROCm 6.x).
    {
      std::string name;

      std::ifstream ue(dev + "/uevent");
      std::string ln;
      while (std::getline(ue, ln)) {
        if (ln.substr(0, 14) != "PCI_SLOT_NAME=") continue;
        // Kernel-supplied, but escaped anyway so every concatenated shell call
        // in the tree escapes and the contract below needs no exception for it.
        FILE *lp = popen(("lspci -vmm -s " + shell_escape(ln.substr(14)) + " 2>/dev/null").c_str(), "r");
        if (lp) {
          char buf[256] = {};
          std::string dev_name, sdev_name;
          while (fgets(buf, sizeof(buf), lp)) {
            std::string f(buf);
            auto tab = f.find('\t');
            if (tab == std::string::npos) continue;
            std::string key = f.substr(0, tab);
            std::string val = f.substr(tab + 1);
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r')) val.pop_back();
            if (key == "Device:") dev_name = val;
            else if (key == "SDevice:") sdev_name = val;
          }
          pclose(lp);
          name = dev_name.empty() ? sdev_name : dev_name;
        }
        break;
      }

      // A bare hex string (e.g. "5327") means the device isn't in pci.ids — discard it.
      if (!name.empty() && name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
        name.clear();

      
      // pci.ids was stale — try ROCm tooling which ships its own device database.
      if (name.empty()) {
        auto extract = [&name](const char *cmd, const char *marker, std::size_t mlen) {
          FILE *p = popen(cmd, "r");
          if (!p) return;
          char buf[256] = {};
          while (fgets(buf, sizeof(buf), p)) {
            std::string line(buf);
            auto pos = line.find(marker);
            if (pos == std::string::npos) continue;
            auto vs = line.find_first_not_of(" \t", pos + mlen);
            if (vs == std::string::npos) continue;
            std::string n = line.substr(vs);
            while (!n.empty() && (n.back() == '\n' || n.back() == '\r' || n.back() == ' ')) n.pop_back();
            if (!n.empty() && n != "N/A") { name = n; break; }
          }
          pclose(p);
        };

        extract("rocm-smi --showproductname 2>/dev/null", "Card series:", 12);
        if (name.empty())
          extract("amd-smi static --asic 2>/dev/null",    "MARKET_NAME:", 12);
      }

      gpu["name"] = name.empty() ? "AMD GPU" : name;
      gpu["vendor"] = "amd";
    }

    try { gpu["temperature_c"] = std::stoi(read_sysfs(hwmon + "/temp1_input")) / 1000; } catch (...) {}
    try { gpu["utilization_pct"] = std::stoi(read_sysfs(dev + "/gpu_busy_percent")); } catch (...) {}
    try { gpu["encoder_pct"] = std::stoi(read_sysfs(dev + "/vcn_busy_percent")); } catch (...) {}
    try { gpu["vram_used_mb"] = std::stoll(read_sysfs(dev + "/mem_info_vram_used")) / (1024 * 1024); } catch (...) {}
    try { gpu["vram_total_mb"] = std::stoll(read_sysfs(dev + "/mem_info_vram_total")) / (1024 * 1024); } catch (...) {}
    try { gpu["fan_speed_pct"] = std::stoi(read_sysfs(hwmon + "/pwm1")) * 100 / 255; } catch (...) {}
    try { gpu["power_draw_w"] = std::stof(read_sysfs(hwmon + "/power1_average")) / 1e6f; } catch (...) {}

    int sclk = active_mhz(dev + "/pp_dpm_sclk");
    int mclk = active_mhz(dev + "/pp_dpm_mclk");
    if (sclk) gpu["clock_gpu_mhz"] = sclk;
    if (mclk) gpu["clock_mem_mhz"] = mclk;

    return gpu;
  }
#endif

  /**
   * @brief Get current stream statistics as JSON.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * 
   * @brief Get system hardware stats (GPU telemetry, display, and audio info).
   */
  void getSystemStats(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    nlohmann::json output;
    output["gpu"] = nullptr;
    output["video"]["active_encoder"] = video::active_encoder_name();
#ifdef __linux__
    const bool display_environment_repaired =
      session_manager::repair_desktop_session_environment() ||
      session_manager::desktop_session_environment_was_repaired();
    const char *wayland_display = std::getenv("WAYLAND_DISPLAY");
    const char *x11_display = std::getenv("DISPLAY");
    const char *desktop_name = std::getenv("XDG_CURRENT_DESKTOP");
    const char *session_type = std::getenv("XDG_SESSION_TYPE");
    const bool has_wayland_display = wayland_display && wayland_display[0] != '\0';
    const bool has_x11_display = x11_display && x11_display[0] != '\0';

    // Boot readiness: whether this account's Polaris survives a reboot with no
    // monitor or desktop login. Lingering plus a default.target want is what
    // `--setup-host --enable-headless-boot` arranges; either alone is not
    // enough, so both are reported.
    bool linger_enabled = false;
    bool boot_start_linked = false;
    {
      std::error_code boot_ec;
      const auto *pw = getpwuid(geteuid());
      if (pw && pw->pw_name && pw->pw_name[0] != '\0') {
        linger_enabled = fs::exists(fs::path("/var/lib/systemd/linger") / pw->pw_name, boot_ec);
      }
      fs::path config_home;
      if (const char *xdg_config = std::getenv("XDG_CONFIG_HOME"); xdg_config && *xdg_config) {
        config_home = xdg_config;
      } else if (const char *home = std::getenv("HOME"); home && *home) {
        config_home = fs::path(home) / ".config";
      } else if (pw && pw->pw_dir && pw->pw_dir[0] != '\0') {
        config_home = fs::path(pw->pw_dir) / ".config";
      }
      boot_start_linked = (!config_home.empty() && fs::exists(config_home / "systemd/user/default.target.wants/polaris.service", boot_ec)) ||
                          fs::exists("/etc/systemd/user/default.target.wants/polaris.service", boot_ec);
    }
    const bool boot_independent = linger_enabled && boot_start_linked;
    output["boot_readiness"]["linger_enabled"] = linger_enabled;
    output["boot_readiness"]["boot_start_linked"] = boot_start_linked;
    output["boot_readiness"]["status"] = boot_independent ? "boot_independent" : "session_bound";
    output["boot_readiness"]["summary"] = boot_independent ?
      "Polaris starts at boot with no monitor or desktop login." :
      "Polaris starts with the desktop session, so after a reboot it is unavailable until someone logs in.";
    output["boot_readiness"]["action"] = boot_independent ?
      "No action needed." :
      "For a monitor-less or Game Mode host, run: sudo -H polaris --setup-host --enable-headless-boot";

    output["display_session"]["status"] = has_wayland_display || has_x11_display ? "healthy" : "missing_display_environment";
    if (has_wayland_display || has_x11_display) {
      output["display_session"]["summary"] = has_wayland_display ?
        "Wayland desktop environment is available to Polaris." :
        "X11 desktop environment is available to Polaris.";
      output["display_session"]["action"] = "No action needed.";
    } else if (boot_independent) {
      // A headless-boot host has no desktop on purpose; telling it to restart
      // from the desktop session would be wrong advice.
      output["display_session"]["summary"] = "No desktop environment is attached, which is expected on a headless-boot host. Private Stream is unaffected.";
      output["display_session"]["action"] = "To stream the visible desktop instead, log into the desktop and restart Polaris so it inherits the graphical environment.";
    } else {
      output["display_session"]["summary"] = "Polaris could not find WAYLAND_DISPLAY or DISPLAY for desktop previews.";
      output["display_session"]["action"] = "Restart Polaris from the desktop session or run the user service so it inherits the graphical environment.";
    }
    output["display_session"]["environment_repaired"] = display_environment_repaired;
    output["display_session"]["session_type"] = session_type && session_type[0] ? std::string(session_type) : "unknown";
    output["display_session"]["desktop"] = desktop_name && desktop_name[0] ? std::string(desktop_name) : "unknown";
    output["display_session"]["wayland_available"] = has_wayland_display;
    output["display_session"]["x11_available"] = has_x11_display;
    {
      const auto labwc = snapshot_labwc();
      output["runtime"]["backend"] = labwc.state.backend_name;
      output["runtime"]["requested_headless"] = labwc.state.requested_headless;
      output["runtime"]["effective_headless"] = labwc.state.effective_headless;
      output["runtime"]["gpu_native_override_active"] = labwc.state.gpu_native_override_active;
    }
#endif

#ifdef __linux__
    auto vendor = detect_gpu_vendor();
    if (vendor == "nvidia")      output["gpu"] = query_nvidia_gpu();
    else if (vendor == "amd")    output["gpu"] = query_amd_gpu();

    // Prefer live Wayland monitor telemetry from the active runtime.
    {
      nlohmann::json displays = nlohmann::json::array();
      auto push_display = [&displays](
                            const std::string &name,
                            const std::string &friendly_name,
                            const std::string &device_id,
                            bool primary,
                            std::optional<int> width = std::nullopt,
                            std::optional<int> height = std::nullopt) {
        nlohmann::json display;
        display["name"] = name;
        display["friendly_name"] = friendly_name;
        display["device_id"] = device_id;

        if (!name.empty() && !friendly_name.empty()) {
          display["label"] = name + ": " + friendly_name;
        } else if (!friendly_name.empty()) {
          display["label"] = friendly_name;
        } else if (!name.empty()) {
          display["label"] = name;
        } else {
          display["label"] = device_id;
        }

        display["primary"] = primary;
        if (width) {
          display["width"] = *width;
        }
        if (height) {
          display["height"] = *height;
        }
        displays.push_back(display);
      };

      auto live_stats = stream_stats::get_current();
      std::vector<std::unique_ptr<wl::monitor_t>> wayland_monitors;
      if (live_stats.streaming && config::video.linux_display.use_cage_compositor) {
        const auto labwc = snapshot_labwc();
        if (labwc.running && !labwc.socket.empty()) {
          wayland_monitors = wl::monitors(labwc.socket.c_str());
        }
      }

      if (wayland_monitors.empty() && has_wayland_display) {
        wayland_monitors = wl::monitors();
      }

      for (std::size_t index = 0; index < wayland_monitors.size(); ++index) {
        const auto &monitor = wayland_monitors[index];
        push_display(
          monitor->name,
          monitor->description,
          monitor->name,
          index == 0,
          static_cast<int>(monitor->viewport.width),
          static_cast<int>(monitor->viewport.height)
        );
      }

      // Fallback to Polaris's configured display device registry.
      if (displays.empty()) {
        const auto enumerated_devices = display_device::enumerate_devices();
        for (const auto &device : enumerated_devices) {
          push_display(
            device.m_display_name,
            device.m_friendly_name,
            device.m_device_id,
            device.m_info ? device.m_info->m_primary : false
          );
        }
      }

      // Final fallback to xrandr when richer telemetry is unavailable.
      if (displays.empty()) {
        FILE *xpipe = popen("xrandr --query 2>/dev/null | grep ' connected'", "r");
        if (xpipe) {
          char buf[512];
          while (fgets(buf, sizeof(buf), xpipe)) {
            std::string line(buf);
            if (!line.empty() && line.back() == '\n') line.pop_back();
            // Format: "DP-3 connected primary 7680x2160+0+0 ..."
            std::istringstream iss(line);
            std::string name, status;
            iss >> name >> status;
            bool primary = false;
            std::optional<int> width;
            std::optional<int> height;

            // Check for "primary" keyword and resolution
            std::string token;
            while (iss >> token) {
              if (token == "primary") {
                primary = true;
              } else if (token.find('x') != std::string::npos && token.find('+') != std::string::npos) {
                // Resolution like "7680x2160+0+0"
                auto xpos = token.find('x');
                auto plus = token.find('+');
                if (xpos != std::string::npos && plus != std::string::npos) {
                  try {
                    width = std::stoi(token.substr(0, xpos));
                    height = std::stoi(token.substr(xpos + 1, plus - xpos - 1));
                  } catch (...) {}
                }
              }
            }
            push_display(name, "", name, primary, width, height);
          }
          pclose(xpipe);
        }
      }
      output["displays"] = displays;
    }

    // Query audio via pactl (PipeWire/PulseAudio)
    {
      nlohmann::json audio;
      FILE *apipe = popen("LC_ALL=C pactl info 2>/dev/null", "r");
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

  nlohmann::json build_network_path_port_probe(const net::network_path_probe_port_t &port) {
    nlohmann::json item;
    item["key"] = port.key;
    item["label"] = port.label;
    item["port"] = port.port;
    item["transport"] = port.transport;

    if (port.transport == "tcp") {
      try {
        boost::asio::io_context io;
        boost::asio::ip::tcp::socket socket {io};
        socket.connect({boost::asio::ip::make_address("127.0.0.1"), port.port});
        item["status"] = "open";
        item["detail"] = "Local Polaris host accepted a TCP connection on the mapped port.";
      } catch (const std::exception &e) {
        item["status"] = "closed";
        item["detail"] = "Local Polaris host did not accept a TCP connection on this mapped port.";
        item["error"] = e.what();
      }
    } else {
      item["status"] = "hint";
      item["detail"] = "UDP stream reachability cannot be proven from the Web UI server without sending test media; verify firewall/NAT allows this mapped port.";
    }

    return item;
  }

  void getNetworkPathProbe(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    const auto remote_address = net::addr_to_normalized_string(request->remote_endpoint().address());
    const auto host_header = request->header.find("host");
    const auto request_host = host_header == request->header.end() ? std::string {} : host_header->second;
    const auto configured_ports = net::network_path_probe_ports(static_cast<std::uint16_t>(config::sunshine.port));

    nlohmann::json ports = nlohmann::json::array();
    for (const auto &port : configured_ports) {
      ports.push_back(build_network_path_port_probe(port));
    }

    nlohmann::json output;
    output["status"] = true;
    output["kind"] = "network-path-probe";
    output["targetHost"] = remote_address;
    output["requestHost"] = request_host;
    output["classification"] = net::network_path_probe_classification(remote_address);
    output["hostReachable"] = true;
    output["mdnsAvailable"] = request_host.find(".local") != std::string::npos;
    output["ports"] = ports;
    output["samples"] = {
      {"latencyMs", nlohmann::json::array()},
      {"jitterMs", nullptr},
      {"packetLossPercent", nullptr},
      {"source", "stream telemetry when active; no synthetic packets sent"},
    };
    output["notes"] = nlohmann::json::array({
      "Server-side probe is read-only: it checks the requesting client address, local mapped TCP listeners, and UDP port contract hints.",
      "WAN/NAT UDP reachability still needs a client-side stream attempt or external port test."
    });

    send_response(response, output);
  }

  nlohmann::json augment_stream_stats_json(nlohmann::json stats_json, const stream_stats::stats_t &stats) {
    stats_json["tuning"] = settings_metadata::build_tuning_json(
      adaptive_bitrate::get_state(),
      stats,
      proc::proc.current_app_has_mangohud()
    );
    stats_json["auto_quality"] = nvhttp::auto_quality_status_json();
    return stats_json;
  }

  void getStreamStats(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request))
      return;

    print_req(request);

    auto stats = stream_stats::get_current();
    SimpleWeb::CaseInsensitiveMultimap headers;
    append_json_security_headers(headers);
    response->write(
      augment_stream_stats_json(nlohmann::json::parse(stats.to_json()), stats).dump(),
      headers
    );
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

    // Run the SSE stream in a tracked background task so shutdown can wait for it.
    launch_background_task([response]() {
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
        *response << "data: "
                  << augment_stream_stats_json(nlohmann::json::parse(stats.to_json()), stats).dump()
                  << "\n\n";

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
    });
  }

  // Session event bus (file-scope for access from emit_session_event and SSE handler)
  static std::mutex s_event_mtx;
  static session_event_queue::event_queue s_events;

  /**
   * @brief Start the HTTPS server.
   */
  void start() {
    // Generate CSRF token at server startup
    csrfToken = crypto::rand_alphabet(48,
      std::string_view {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"});
    BOOST_LOG(info) << "CSRF token generated for web UI session";

    initialize_web_session_store();

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
      ai_cfg.codex_home = config::video.ai_optimizer.codex_home;
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

    server.verify = [](req_https_t request, SSL *ssl) {
      if (auto named_cert_p = nvhttp::verify_client_cert(ssl)) {
        request->userp = named_cert_p;
      }
    };

    // Helper lambda to wrap a handler with CSRF validation.
    // API key (Bearer token) requests bypass CSRF since they are not cookie-based.
    // -----------------------------------------------------------------------
    // Polaris session event bus (SSE)
    // Uses file-scope globals (defined below start())
    // -----------------------------------------------------------------------

    auto getPolarisEventsSSE = [](resp_https_t response, req_https_t request) {
      if (!authenticatePolarisSession(response, request)) return;
      print_req(request);

      launch_background_task([response]() {
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

        uint64_t last_seq;
        {
          std::lock_guard lk(s_event_mtx);
          last_seq = s_events.cursor();
        }

        while (!shutdown_event->peek()) {
          std::vector<std::string> pending_events;
          uint64_t current_seq;
          {
            std::lock_guard lk(s_event_mtx);
            current_seq = s_events.cursor();
            pending_events = s_events.events_after(last_seq);
          }

          // Build current session state
          nlohmann::json state;
#ifdef __linux__
          {
            const auto labwc = snapshot_labwc();
            state["cage_running"] = labwc.running;
            state["cage_socket"] = labwc.socket;
          }
          state["screen_locked"] = session_manager::is_screen_locked();
#endif
          state["seq"] = current_seq;
          state["session_state"] = get_session_state();

          // Send only events emitted after this SSE client cursor.
          // Older terminal events may still be retained for other clients, but
          // they must not be replayed into a fresh Nova game activity.
          if (!pending_events.empty()) {
            for (const auto &evt : pending_events) {
              *response << "event: session\ndata: " << evt << "\n\n";
            }
            last_seq = current_seq;
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
      });
    };

    // -----------------------------------------------------------------------
    // Polaris session API
    // -----------------------------------------------------------------------

    auto getPolarisSession = [](resp_https_t response, req_https_t request) {
      if (!authenticatePolarisSession(response, request)) return;
      print_req(request);

      nlohmann::json output;

#ifdef __linux__
      {
        const auto labwc = snapshot_labwc();
        output["cage_running"] = labwc.running;
        output["cage_pid"] = labwc.pid;
        output["cage_socket"] = labwc.socket;
        output["cage_healthy"] = labwc.healthy;
      }
      output["screen_locked"] = session_manager::is_screen_locked();
#else
      output["cage_running"] = false;
#endif

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    auto postPolarisUnlock = [](resp_https_t response, req_https_t request) {
      if (!authenticatePolarisSession(response, request)) return;
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
        if (!has_bearer && !hasVerifiedClientCert(request) && !validateCsrf(request)) {
          BOOST_LOG(warning) << "CSRF token validation failed for "sv << request->path;
          forbidden(response, request, "CSRF token missing or invalid");
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
    server.resource["^/recover/?$"]["GET"] = getSpaPage;
    server.resource["^/troubleshooting/?$"]["GET"] = getSpaPage;
    server.resource["^/browser-stream/?$"]["GET"] = getSpaPage;
    server.resource["^/webrtc/?$"]["GET"] = getSpaPage;
    // Login is exempt from CSRF (it's the entry point; rate limiting protects it instead)
    server.resource["^/api/login"]["POST"] = login;
    server.resource["^/api/logout$"]["POST"] = withCsrf(logout);
    server.resource["^/api/pin$"]["POST"] = withCsrf(savePin);
    server.resource["^/api/otp$"]["POST"] = withCsrf(getOTP);
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = withCsrf(saveApp);
    server.resource["^/api/apps/reorder$"]["POST"] = withCsrf(reorderApps);
    server.resource["^/api/apps/delete$"]["POST"] = withCsrf(deleteApp);
    server.resource["^/api/apps/launch$"]["POST"] = withCsrf(launchApp);

    // P0-5 benchmark control surface (measurement-spec-v1.md 6.4). Deliberately
    // not under /api/ - these are harness-facing, not Web UI-facing. Not
    // wrapped in withCsrf() like the routes above - authorizeBenchmarkHarnessRequest()
    // is the real gate, and it already enforces the same CSRF check withCsrf()
    // would, but only for the cookie half of its two accepted auth methods
    // (see that function's own doc comment for why a Bearer-token caller is
    // exempt).
    server.resource["^/polaris/v1/session/timing/runs$"]["POST"] = createBenchmarkRun;
    server.resource["^/polaris/v1/session/timing/runs/([^/]+)/start$"]["POST"] = startBenchmarkRun;
    server.resource["^/polaris/v1/session/timing/runs/([^/]+)/stop$"]["POST"] = stopBenchmarkRun;
    server.resource["^/polaris/v1/session/timing/runs/([^/]+)$"]["GET"] = getBenchmarkRun;
    server.resource["^/polaris/v1/session/timing/runs/([^/]+)$"]["DELETE"] = deleteBenchmarkRun;
    server.resource["^/api/apps/close$"]["POST"] = withCsrf(closeApp);
    server.resource["^/api/games/scan$"]["GET"] = scanGames;
    server.resource["^/api/games/import$"]["POST"] = withCsrf(importGames);
    server.resource["^/polaris/v1/diagnostics/logs/tail$"]["GET"] = getLogTail;
    server.resource["^/polaris/v1/diagnostics/logs/previous$"]["GET"] = getPreviousLogs;
    server.resource["^/polaris/v1/diagnostics/last-run$"]["GET"] = getLastRun;
    server.resource["^/polaris/v1/diagnostics/client-reports$"]["GET"] = getClientSupportReports;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/logs/clear$"]["POST"] = withCsrf(clearLogs);
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/settings/metadata$"]["GET"] = getSettingsMetadata;
    server.resource["^/api/update-status$"]["GET"] = getUpdateStatus;
    server.resource["^/api/config$"]["POST"] = withCsrf(saveConfig);
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/restart$"]["POST"] = withCsrf(restart);
    server.resource["^/api/quit$"]["POST"] = withCsrf(quit);
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = withCsrf(resetDisplayDevicePersistence);
    server.resource["^/api/virtual-display/cleanup-stale$"]["POST"] = withCsrf(cleanupStaleVirtualDisplay);
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
    server.resource["^/api/ai/history/clear$"]["POST"] = withCsrf(clearAiHistory);
    server.resource["^/api/ai/models$"]["POST"] = withCsrf(getAiModels);
    server.resource["^/api/ai/test$"]["POST"] = withCsrf(testAiConfig);
    server.resource["^/api/ai/optimize$"]["POST"] = withCsrf(triggerAiOptimize);
    server.resource["^/api/ai/explain-doctor$"]["POST"] = withCsrf(explainDoctorWithAi);
    server.resource["^/api/doctor/action$"]["POST"] = withCsrf(runDoctorAction);
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
    server.resource["^/api/support/network-path-probe$"]["GET"] = getNetworkPathProbe;
    server.resource["^/api/recording/start$"]["POST"] = withCsrf(startRecording);
    server.resource["^/api/recording/stop$"]["POST"] = withCsrf(stopRecording);
    server.resource["^/api/recording/save-replay$"]["POST"] = withCsrf(saveReplay);
    server.resource["^/api/recording/status$"]["GET"] = getRecordingStatus;
#ifdef __linux__
    server.resource["^/api/browser-stream/status$"]["GET"] = getBrowserStreamStatus;
    server.resource["^/api/browser-stream/session/start$"]["POST"] = withCsrf(postBrowserStreamStart);
    server.resource["^/api/browser-stream/session/stop$"]["POST"] = withCsrf(postBrowserStreamStop);
    server.resource["^/api/webrtc/status$"]["GET"] = getBrowserStreamStatus;
    server.resource["^/api/display/screenshot$"]["GET"] = getDisplayScreenshot;
    server.resource["^/api/display/stream$"]["GET"] = getDisplayStream;
#ifdef __linux__
    server.resource["^/api/linux/display-outputs$"]["GET"] = [](resp_https_t response, req_https_t request) {
      if (!authenticate(response, request)) {
        return;
      }
      print_req(request);
      nlohmann::json output;
      output["status"] = true;
      nlohmann::json arr = nlohmann::json::array();
      for (const auto &o : display_topology::list_outputs()) {
        arr.push_back({
          {"name", o.name},
          {"drm_path", o.drm_path},
          {"connected", o.connected},
          {"enabled", o.enabled},
          {"likely_dongle", o.likely_dongle},
          {"suggested_primary", o.suggested_primary},
          {"suggested_streaming", o.suggested_streaming},
        });
      }
      output["outputs"] = std::move(arr);
      output["streaming_output"] = config::video.linux_display.streaming_output;
      output["primary_output"] = config::video.linux_display.primary_output;
      // Suggestions when config empty
      std::string sug_stream;
      std::string sug_primary;
      for (const auto &o : display_topology::list_outputs()) {
        if (o.suggested_streaming) {
          sug_stream = o.name;
        }
        if (o.suggested_primary) {
          sug_primary = o.name;
        }
      }
      output["suggested_streaming_output"] = sug_stream;
      output["suggested_primary_output"] = sug_primary;
      send_response(response, output);
    };
#endif
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
        server->start([port_https, address_family](unsigned short port) {
          const auto local_host = address_family == net::af_e::IPV4 ? "127.0.0.1"sv : "localhost"sv;
          BOOST_LOG(info) << "Configuration UI available at [https://"sv << local_host << ':' << port << ']';
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek())
          return;
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        lifetime::note_shutdown_reason("Configuration HTTPS server could not bind its port");
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread tcp { accept_and_run, &server };

    // Wait for any event
    shutdown_event->view();

    server.stop();
    wait_for_background_tasks();

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
    s_events.push(evt.dump());

    BOOST_LOG(info) << "session_event: "sv << event << " ["sv << get_session_state() << "] "sv << message;
  }

  void set_session_state(session_state_e state) {
    s_session_state = state;
  }

#ifdef POLARIS_TESTS
  bool config_request_authorized_for_tests(
    const crypto::p_named_cert_t &candidate,
    std::string_view request_path
  ) {
    return getVerifiedClientCert(candidate, request_path) != nullptr;
  }
#endif

  std::string get_session_state() {
    switch (s_session_state.load()) {
      case session_state_e::idle: return "idle";
      case session_state_e::initializing: return "initializing";
      case session_state_e::cage_starting: return "cage_starting";
      case session_state_e::game_launching: return "game_launching";
      case session_state_e::streaming: return "streaming";
      case session_state_e::paused: return "paused";
      case session_state_e::tearing_down: return "tearing_down";
      default: return "unknown";
    }
  }
}  // namespace confighttp
