/**
 * @file src/nvhttp.cpp
 * @brief Definitions for the nvhttp (GameStream) server.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <string>

// lib includes (JSON for last-launched persistence)
#include <nlohmann/json.hpp>

// lib includes
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/context_base.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <Simple-Web-Server/server_http.hpp>

// local includes
#include "config.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "process.h"
#include "rtsp.h"
#include "stream.h"
#include "system_tray.h"
#include "utility.h"
#include "adaptive_bitrate.h"
#include "ai_optimizer.h"
#include "device_db.h"
#include "confighttp.h"
#include "stream_stats.h"
#ifdef __linux__
  #include "platform/linux/cage_display_router.h"
  #include "platform/linux/session_manager.h"
  #include "platform/linux/virtual_display.h"
#endif
#include "uuid.h"
#include "video.h"
#include "zwpad.h"

#ifdef _WIN32
  #include "platform/windows/virtual_display.h"
#endif

using namespace std::literals;

namespace nvhttp {

  namespace fs = std::filesystem;
  namespace pt = boost::property_tree;

  namespace {
    void append_optimization_json(nlohmann::json &output, const device_db::optimization_t &optimization) {
      if (optimization.display_mode) {
        output["display_mode"] = *optimization.display_mode;
      }
      if (optimization.color_range) {
        output["color_range"] = *optimization.color_range;
      }
      if (optimization.hdr.has_value()) {
        output["hdr"] = *optimization.hdr;
      }
      if (optimization.virtual_display.has_value()) {
        output["virtual_display"] = *optimization.virtual_display;
      }
      if (optimization.target_bitrate_kbps) {
        output["target_bitrate_kbps"] = *optimization.target_bitrate_kbps;
      }
      if (optimization.nvenc_tune) {
        output["nvenc_tune"] = *optimization.nvenc_tune;
      }
      if (optimization.preferred_codec) {
        output["preferred_codec"] = *optimization.preferred_codec;
      }
      output["reasoning"] = optimization.reasoning;
      output["reasoning_summary"] = optimization.reasoning;
      output["source"] = optimization.source;
      output["cache_status"] = optimization.cache_status;
      output["confidence"] = optimization.confidence;
      output["signals_used"] = optimization.signals_used;
      output["normalization_reason"] = optimization.normalization_reason;
      output["recommendation_version"] = optimization.recommendation_version;
      output["generated_at"] = optimization.generated_at;
      output["expires_at"] = optimization.expires_at;
    }
  }  // namespace

#ifdef __linux__
  namespace {
    std::mutex deferred_cage_capability_probe_mutex;

    std::optional<std::string> copy_env_var(const char *key) {
      if (const char *value = getenv(key)) {
        return std::string {value};
      }

      return std::nullopt;
    }

    void restore_env_var(const char *key, const std::optional<std::string> &value) {
      if (value) {
        platf::set_env(key, *value);
      } else {
        unsetenv(key);
      }
    }

    bool should_defer_encoder_probe_until_cage() {
      return config::video.linux_display.use_cage_compositor;
    }

    bool prime_deferred_headless_codec_capabilities() {
      if (!config::video.linux_display.use_cage_compositor ||
          !config::video.linux_display.headless_mode ||
          video::advertised_codec_capability_state_ready()) {
        return true;
      }

      if (rtsp_stream::session_count() != 0 || cage_display_router::is_running()) {
        return false;
      }

      std::scoped_lock lock {deferred_cage_capability_probe_mutex};
      if (video::advertised_codec_capability_state_ready()) {
        return true;
      }

      if (rtsp_stream::session_count() != 0 || cage_display_router::is_running()) {
        return false;
      }

      BOOST_LOG(info) << "nvhttp: Priming deferred headless encoder capabilities using a temporary cage runtime"sv;
      if (!cage_display_router::start()) {
        BOOST_LOG(warning) << "nvhttp: Temporary cage runtime failed to start for deferred capability probe"sv;
        return false;
      }

      auto stop_guard = util::fail_guard([]() {
        cage_display_router::stop();
        video::reset_encoder_probe_state();
      });

      const auto cage_socket = cage_display_router::get_wayland_socket();
      if (cage_socket.empty()) {
        BOOST_LOG(warning) << "nvhttp: Temporary cage runtime did not expose a WAYLAND_DISPLAY for deferred capability probe"sv;
        return false;
      }

      const auto original_wayland_display = copy_env_var("WAYLAND_DISPLAY");
      const auto original_at_spi_bus_address = copy_env_var("AT_SPI_BUS_ADDRESS");

      platf::set_env("WAYLAND_DISPLAY", cage_socket);
      platf::set_env("AT_SPI_BUS_ADDRESS", "");

      const int probe_status = video::probe_encoders();

      restore_env_var("AT_SPI_BUS_ADDRESS", original_at_spi_bus_address);
      restore_env_var("WAYLAND_DISPLAY", original_wayland_display);

      if (probe_status != 0) {
        BOOST_LOG(warning) << "nvhttp: Deferred headless capability probe failed"sv;
        return false;
      }

      BOOST_LOG(info) << "nvhttp: Deferred headless capability probe primed encoder cache"sv;
      return true;
    }
  }  // namespace
#endif

  namespace {
    video::codec_capability_state_t advertised_codec_support_for_http(bool allow_deferred_headless_prime = false) {
#ifdef __linux__
      if (allow_deferred_headless_prime) {
        (void) prime_deferred_headless_codec_capabilities();
      }
#endif
      return video::advertised_codec_capability_state();
    }

    std::optional<int> rounded_refresh_rate_hz(const display_device::FloatingPoint &value) {
      return std::visit(
        [](const auto &refresh_rate) -> std::optional<int> {
          using value_t = std::decay_t<decltype(refresh_rate)>;
          if constexpr (std::is_same_v<value_t, display_device::Rational>) {
            if (refresh_rate.m_denominator == 0) {
              return std::nullopt;
            }

            return static_cast<int>(std::lround(static_cast<double>(refresh_rate.m_numerator) /
                                                static_cast<double>(refresh_rate.m_denominator)));
          } else {
            return static_cast<int>(std::lround(refresh_rate));
          }
        },
        value
      );
    }

    std::optional<int> active_output_refresh_rate_hz_hint() {
      const auto configured_display_name = display_device::map_output_name(config::video.output_name);
      const auto enumerated_devices = display_device::enumerate_devices();

      std::optional<int> primary_refresh_rate;
      std::optional<int> any_active_refresh_rate;

      for (const auto &device : enumerated_devices) {
        if (!device.m_info) {
          continue;
        }

        const auto refresh_rate_hz = rounded_refresh_rate_hz(device.m_info->m_refresh_rate);
        if (!refresh_rate_hz || *refresh_rate_hz <= 0) {
          continue;
        }

        const bool matches_configured_output = !configured_display_name.empty() &&
                                              (device.m_display_name == configured_display_name ||
                                               device.m_device_id == config::video.output_name);
        if (matches_configured_output) {
          return refresh_rate_hz;
        }

        if (device.m_info->m_primary &&
            (!primary_refresh_rate || *refresh_rate_hz > *primary_refresh_rate)) {
          primary_refresh_rate = refresh_rate_hz;
        }

        if (!any_active_refresh_rate || *refresh_rate_hz > *any_active_refresh_rate) {
          any_active_refresh_rate = refresh_rate_hz;
        }
      }

      return primary_refresh_rate ? primary_refresh_rate : any_active_refresh_rate;
    }

    int advertised_max_launch_refresh_rate_for_http() {
#ifdef __linux__
      if (config::video.linux_display.use_cage_compositor &&
          config::video.linux_display.headless_mode) {
        // Headless cage sessions aren't tied to a physical panel refresh, so advertise the
        // highest stock launch rate Nova currently exposes without custom entry.
        return 120;
      }
#endif

      if (const auto refresh_rate_hz = active_output_refresh_rate_hz_hint()) {
        return *refresh_rate_hz;
      }

      return 60;
    }

    std::string_view codec_name_for_video_format(int video_format) {
      switch (video_format) {
        case 1:
          return "hevc"sv;
        case 2:
          return "av1"sv;
        default:
          return "h264"sv;
      }
    }

    std::string format_profile_fps(int fps) {
      if (fps <= 0) {
        return "0"s;
      }
      if (fps % 1000 == 0) {
        return std::to_string(fps / 1000);
      }
      return std::format("{:.3f}", static_cast<double>(fps) / 1000.0);
    }

    std::string format_watch_profile(const stream::session_profile_t &profile) {
      return std::format("{}x{}@{} {} {}-bit {}kbps",
                         profile.width,
                         profile.height,
                         format_profile_fps(profile.session_target_fps),
                         codec_name_for_video_format(profile.video_format),
                         profile.dynamic_range > 0 ? 10 : 8,
                         profile.bitrate_kbps);
    }

    std::optional<stream::session_profile_t> active_owner_watch_profile() {
      const auto owner_uuid = proc::proc.get_session_owner_unique_id();
      if (owner_uuid.empty()) {
        return std::nullopt;
      }

      const auto owner_session = rtsp_stream::find_session(owner_uuid);
      if (!owner_session || stream::session::is_watch_only(*owner_session)) {
        return std::nullopt;
      }

      return stream::session::profile(*owner_session);
    }

    std::optional<std::pair<int, std::string>> pin_watch_session_to_active_profile(rtsp_stream::launch_session_t &launch_session) {
      if (!launch_session.watch_only) {
        return std::nullopt;
      }

      const auto owner_profile = active_owner_watch_profile();
      if (!owner_profile) {
        return std::make_pair(409, "No active owner stream is available to watch"s);
      }

      const int requested_dynamic_range = launch_session.enable_hdr ? 1 : 0;
      if (launch_session.requested_width != owner_profile->width ||
          launch_session.requested_height != owner_profile->height ||
          launch_session.requested_fps != owner_profile->session_target_fps ||
          requested_dynamic_range != owner_profile->dynamic_range) {
        return std::make_pair(
          412,
          std::format("Watch mode must match the active stream profile ({})", format_watch_profile(*owner_profile))
        );
      }

      launch_session.requested_width = owner_profile->width;
      launch_session.requested_height = owner_profile->height;
      launch_session.requested_fps = owner_profile->session_target_fps;
      launch_session.width = owner_profile->width;
      launch_session.height = owner_profile->height;
      launch_session.fps = owner_profile->session_target_fps;
      launch_session.enable_hdr = owner_profile->dynamic_range > 0;
      launch_session.target_bitrate_kbps = owner_profile->bitrate_kbps;
      launch_session.preferred_codec = std::string {codec_name_for_video_format(owner_profile->video_format)};
      launch_session.optimization_source = "watch_owner_profile";

      BOOST_LOG(info) << "Watch session pinned to active owner profile ["sv
                      << owner_profile->device_name << "]: "sv
                      << format_watch_profile(*owner_profile);

      return std::nullopt;
    }

    nlohmann::json launch_mode_contract_for_app(const proc::ctx_t &app) {
      bool host_virtual_display_available = false;
      bool host_prefers_headless = false;

#ifdef __linux__
      host_virtual_display_available = virtual_display::is_available();
      host_prefers_headless =
        config::video.linux_display.use_cage_compositor &&
        config::video.linux_display.headless_mode;
#elif defined(_WIN32)
      host_virtual_display_available =
        vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK ||
        vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::UNKNOWN;
#endif

      const bool app_prefers_virtual_display = app.virtual_display;
      const std::string preferred_mode = app_prefers_virtual_display ? "virtual_display" : "headless";
      std::string recommended_mode = preferred_mode;
      std::string mode_reason;

      auto allowed_modes = nlohmann::json::array();
      allowed_modes.push_back("headless");
      if (host_virtual_display_available) {
        allowed_modes.push_back("virtual_display");
      }

      const bool steam_big_picture = boost::iequals(boost::trim_copy(app.name), "Steam Big Picture");

      if (app_prefers_virtual_display && host_virtual_display_available) {
        recommended_mode = "virtual_display";
        mode_reason = steam_big_picture ?
          "Steam Big Picture is configured to prefer a dedicated virtual display on this host." :
          "This app is configured to prefer a dedicated virtual display on the host.";
      } else if (app_prefers_virtual_display && !host_virtual_display_available) {
        recommended_mode = "headless";
        mode_reason =
          "This app prefers Virtual Display, but Polaris does not currently have a virtual display backend available, so the non-virtual path is recommended.";
      } else if (host_prefers_headless) {
        recommended_mode = "headless";
        mode_reason =
          "Headless is recommended because this Polaris host is already configured for headless streaming.";
      } else if (host_virtual_display_available) {
        recommended_mode = "headless";
        mode_reason =
          "This app defaults to the non-virtual path. Virtual Display is available when you want a dedicated display for the session.";
      } else {
        recommended_mode = "headless";
        mode_reason =
          "This app defaults to the non-virtual path on this host.";
      }

      nlohmann::json launch_mode;
      launch_mode["preferred_mode"] = preferred_mode;
      launch_mode["recommended_mode"] = recommended_mode;
      launch_mode["allowed_modes"] = std::move(allowed_modes);
      launch_mode["mode_reason"] = mode_reason;
      return launch_mode;
    }
  }  // namespace

  using p_named_cert_t = crypto::p_named_cert_t;
  using PERM = crypto::PERM;

  struct client_t {
    std::vector<p_named_cert_t> named_devices;
  };

  struct pair_session_t;

  crypto::cert_chain_t cert_chain;
  static std::string one_time_pin;
  static std::string otp_passphrase;
  static std::string otp_device_name;
  static std::chrono::time_point<std::chrono::steady_clock> otp_creation_time;

  class PolarisHTTPSServer: public SimpleWeb::ServerBase<PolarisHTTPS> {
  public:
    PolarisHTTPSServer(const std::string &certification_file, const std::string &private_key_file):
        ServerBase<PolarisHTTPS>::ServerBase(443),
        context(boost::asio::ssl::context::tls_server) {
      // Disabling TLS 1.0 and 1.1 (see RFC 8996)
      context.set_options(boost::asio::ssl::context::no_tlsv1);
      context.set_options(boost::asio::ssl::context::no_tlsv1_1);
      context.use_certificate_chain_file(certification_file);
      context.use_private_key_file(private_key_file, boost::asio::ssl::context::pem);
    }

    std::function<bool(std::shared_ptr<Request>, SSL*)> verify;
    std::function<void(std::shared_ptr<Response>, std::shared_ptr<Request>)> on_verify_failed;

  protected:
    boost::asio::ssl::context context;

    void after_bind() override {
      if (verify) {
        context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert | boost::asio::ssl::verify_client_once);
        context.set_verify_callback([](int verified, boost::asio::ssl::verify_context &ctx) {
          // To respond with an error message, a connection must be established
          return 1;
        });
      }
    }

    // This is Server<HTTPS>::accept() with SSL validation support added
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
          SimpleWeb::error_code ec;
          session->connection->socket->lowest_layer().set_option(option, ec);

          session->connection->set_timeout(config.timeout_request);
          session->connection->socket->async_handshake(boost::asio::ssl::stream_base::server, [this, session](const SimpleWeb::error_code &ec) {
            session->connection->cancel_timeout();
            auto lock = session->connection->handler_runner->continue_lock();
            if (!lock) {
              return;
            }
            if (!ec) {
              if (verify && !verify(session->request, session->connection->socket->native_handle())) {
                this->write(session, on_verify_failed);
              } else {
                this->read(session);
              }
            } else if (this->on_error) {
              this->on_error(session->request, ec);
            }
          });
        } else if (this->on_error) {
          this->on_error(session->request, ec);
        }
      });
    }
  };

  using https_server_t = PolarisHTTPSServer;
  using http_server_t = SimpleWeb::Server<SimpleWeb::HTTP>;

  struct conf_intern_t {
    std::string servercert;
    std::string pkey;
  } conf_intern;

  // uniqueID, session
  std::unordered_map<std::string, pair_session_t> map_id_sess;
  client_t client_root;
  std::atomic<uint32_t> session_id_counter;

  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<PolarisHTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<PolarisHTTPS>::Request>;
  using resp_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Response>;
  using req_http_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTP>::Request>;

  namespace {
    bool watch_requested(const args_t &args) {
      return util::from_view(get_arg(args, "watch", "0"));
    }

    bool session_token_matches_request(const args_t &args) {
      const auto expected_token = get_arg(args, "sessiontoken", "");
      const auto active_token = proc::proc.get_session_token();
      return expected_token.empty() || active_token.empty() || expected_token == active_token;
    }

    void append_current_game_session_fields(pt::ptree &tree, const crypto::named_cert_t *named_cert_p) {
      const auto current_session_token = proc::proc.get_session_token();
      const auto current_session_owner = proc::proc.get_session_owner_unique_id();
      const bool has_current_owner = !current_session_token.empty() && !current_session_owner.empty();

      tree.put("root.currentgamesessiontoken", current_session_token);
      tree.put("root.currentgameowner", current_session_owner);
      tree.put("root.currentgameviewercount", rtsp_stream::viewer_count());
      tree.put(
        "root.currentgameowned",
        has_current_owner && named_cert_p && proc::proc.is_session_owner(named_cert_p->uuid) ? 1 : 0
      );
    }
  }  // namespace

  /**
   * @brief Check if an IP address falls within any configured trusted subnet.
   * Used for TOFU (Trust-on-First-Use) LAN pairing.
   */
  bool is_in_trusted_subnet(const boost::asio::ip::address &addr) {
    if (config::nvhttp.trusted_subnets.empty()) {
      return false;
    }

    for (const auto &subnet_str : config::nvhttp.trusted_subnets) {
      auto slash = subnet_str.find('/');
      if (slash == std::string::npos) {
        continue;
      }

      try {
        auto net_str = subnet_str.substr(0, slash);
        auto prefix = static_cast<unsigned short>(std::stoi(subnet_str.substr(slash + 1)));

        if (addr.is_v4()) {
          auto network = boost::asio::ip::make_network_v4(
            boost::asio::ip::make_address_v4(net_str), prefix);
          auto client_v4 = addr.to_v4();
          auto net_addr = network.network().to_uint();
          auto client_uint = client_v4.to_uint();
          auto mask = network.netmask().to_uint();
          if ((client_uint & mask) == (net_addr & mask)) {
            return true;
          }
        }
        else if (addr.is_v6()) {
          // Handle IPv4-mapped IPv6 addresses (e.g., ::ffff:10.0.0.1)
          auto v6 = addr.to_v6();
          if (v6.is_v4_mapped()) {
            auto v4 = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
            auto network = boost::asio::ip::make_network_v4(
              boost::asio::ip::make_address_v4(net_str), prefix);
            auto net_addr = network.network().to_uint();
            auto client_uint = v4.to_uint();
            auto mask = network.netmask().to_uint();
            if ((client_uint & mask) == (net_addr & mask)) {
              return true;
            }
          }
        }
      }
      catch (...) {
        BOOST_LOG(warning) << "TOFU: Failed to parse trusted subnet: " << subnet_str;
        continue;
      }
    }

    return false;
  }

  enum class op_e {
    ADD,  ///< Add certificate
    REMOVE  ///< Remove certificate
  };

  std::string get_arg(const args_t &args, const char *name, const char *default_value) {
    auto it = args.find(name);
    if (it == std::end(args)) {
      if (default_value != nullptr) {
        return std::string(default_value);
      }

      throw std::out_of_range(name);
    }
    return it->second;
  }

  // Helper function to extract command entries from a JSON object.
  cmd_list_t extract_command_entries(const nlohmann::json& j, const std::string& key) {
    cmd_list_t commands;

    // Check if the key exists in the JSON.
    if (j.contains(key)) {
      // Ensure that the value for the key is an array.
      try {
        for (const auto& item : j.at(key)) {
          try {
            // Extract "cmd" and "elevated" fields from the JSON object.
            std::string cmd = item.at("cmd").get<std::string>();
            bool elevated = util::get_non_string_json_value<bool>(item, "elevated", false);

            // Add the command entry to the list.
            commands.push_back({cmd, elevated});
          } catch (const std::exception& e) {
            BOOST_LOG(warning) << "Error parsing command entry: " << e.what();
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Error retrieving key \"" << key << "\": " << e.what();
      }
    } else {
      BOOST_LOG(debug) << "Key \"" << key << "\" not found in the JSON.";
    }

    return commands;
  }

  void save_state() {
    nlohmann::json root = nlohmann::json::object();
    // If the state file exists, try to read it.
    if (fs::exists(config::nvhttp.file_state)) {
      try {
        std::ifstream in(config::nvhttp.file_state);
        in >> root;
      } catch (std::exception &e) {
        BOOST_LOG(warning) << "Couldn't read existing state "sv << config::nvhttp.file_state
                           << ": "sv << e.what() << "; rewriting from in-memory state";
        root = nlohmann::json::object();
      }
    }

    // Erase any previous "root" key.
    root.erase("root");

    // Create a new "root" object and set the unique id.
    root["root"] = nlohmann::json::object();
    root["root"]["uniqueid"] = http::unique_id;

    client_t &client = client_root;
    nlohmann::json named_cert_nodes = nlohmann::json::array();

    std::unordered_set<std::string> unique_certs;
    std::unordered_map<std::string, int> name_counts;

    for (auto &named_cert_p : client.named_devices) {
      // Only add each unique certificate once.
      if (unique_certs.insert(named_cert_p->cert).second) {
        nlohmann::json named_cert_node = nlohmann::json::object();
        std::string base_name = named_cert_p->name;
        // Remove any pending id suffix (e.g., " (2)") if present.
        size_t pos = base_name.find(" (");
        if (pos != std::string::npos) {
          base_name = base_name.substr(0, pos);
        }
        int count = name_counts[base_name]++;
        std::string final_name = base_name;
        if (count > 0) {
          final_name += " (" + std::to_string(count + 1) + ")";
        }
        named_cert_node["name"] = final_name;
        named_cert_node["cert"] = named_cert_p->cert;
        named_cert_node["uuid"] = named_cert_p->uuid;
        named_cert_node["display_mode"] = named_cert_p->display_mode;
        named_cert_node["perm"] = static_cast<uint32_t>(named_cert_p->perm);
        named_cert_node["enable_legacy_ordering"] = named_cert_p->enable_legacy_ordering;
        named_cert_node["allow_client_commands"] = named_cert_p->allow_client_commands;
        named_cert_node["always_use_virtual_display"] = named_cert_p->always_use_virtual_display;

        // Add "do" commands if available.
        if (!named_cert_p->do_cmds.empty()) {
          nlohmann::json do_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert_p->do_cmds) {
            do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["do"] = do_cmds_node;
        }

        // Add "undo" commands if available.
        if (!named_cert_p->undo_cmds.empty()) {
          nlohmann::json undo_cmds_node = nlohmann::json::array();
          for (const auto &cmd : named_cert_p->undo_cmds) {
            undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
          }
          named_cert_node["undo"] = undo_cmds_node;
        }

        named_cert_nodes.push_back(named_cert_node);
      }
    }

    root["root"]["named_devices"] = named_cert_nodes;

    try {
      std::ofstream out(config::nvhttp.file_state);
      out << root.dump(4);  // Pretty-print with an indent of 4 spaces.
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't write "sv << config::nvhttp.file_state << ": "sv << e.what();
      return;
    }
  }

  void load_state() {
    if (!fs::exists(config::nvhttp.file_state)) {
      BOOST_LOG(info) << "File "sv << config::nvhttp.file_state << " doesn't exist"sv;
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    nlohmann::json tree;
    try {
      std::ifstream in(config::nvhttp.file_state);
      in >> tree;
    } catch (std::exception &e) {
      BOOST_LOG(error) << "Couldn't read "sv << config::nvhttp.file_state << ": "sv << e.what();
      http::unique_id = uuid_util::uuid_t::generate().string();
      return;
    }

    // Check that the file contains a "root.uniqueid" value.
    if (!tree.contains("root") || !tree["root"].contains("uniqueid")) {
      http::uuid = uuid_util::uuid_t::generate();
      http::unique_id = http::uuid.string();
      return;
    }

    std::string uid = tree["root"]["uniqueid"];
    http::uuid = uuid_util::uuid_t::parse(uid);
    http::unique_id = uid;

    nlohmann::json root = tree["root"];
    client_t client;  // Local client to load into

    // Import from the old format if available.
    if (root.contains("devices")) {
      for (auto &device_node : root["devices"]) {
        // For each device, if there is a "certs" array, add a named certificate.
        if (device_node.contains("certs")) {
          for (auto &el : device_node["certs"]) {
            auto named_cert_p = std::make_shared<crypto::named_cert_t>();
            named_cert_p->name = "";
            named_cert_p->cert = el.get<std::string>();
            named_cert_p->uuid = uuid_util::uuid_t::generate().string();
            named_cert_p->display_mode = "";
            named_cert_p->perm = PERM::_all;
            named_cert_p->enable_legacy_ordering = true;
            named_cert_p->allow_client_commands = true;
            named_cert_p->always_use_virtual_display = false;
            client.named_devices.emplace_back(named_cert_p);
          }
        }
      }
    }

    // Import from the new format.
    if (root.contains("named_devices")) {
      for (auto &el : root["named_devices"]) {
        auto named_cert_p = std::make_shared<crypto::named_cert_t>();
        named_cert_p->name = el.value("name", "");
        named_cert_p->cert = el.value("cert", "");
        named_cert_p->uuid = el.value("uuid", "");
        named_cert_p->display_mode = el.value("display_mode", "");
        named_cert_p->perm = (PERM)(util::get_non_string_json_value<uint32_t>(el, "perm", (uint32_t)PERM::_all)) & PERM::_all;
        named_cert_p->enable_legacy_ordering = el.value("enable_legacy_ordering", true);
        named_cert_p->allow_client_commands = el.value("allow_client_commands", true);
        named_cert_p->always_use_virtual_display = el.value("always_use_virtual_display", false);
        // Load command entries for "do" and "undo" keys.
        named_cert_p->do_cmds = extract_command_entries(el, "do");
        named_cert_p->undo_cmds = extract_command_entries(el, "undo");
        client.named_devices.emplace_back(named_cert_p);
      }
    }

    // Clear any existing certificate chain and add the imported certificates.
    cert_chain.clear();
    for (auto &named_cert : client.named_devices) {
      cert_chain.add(named_cert);
    }

    client_root = client;
  }

  void add_authorized_client(const p_named_cert_t& named_cert_p) {
    client_t &client = client_root;
    client.named_devices.push_back(named_cert_p);
    auto live_named_cert_p = named_cert_p;
    cert_chain.add(live_named_cert_p);

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
    system_tray::update_tray_paired(named_cert_p->name);
#endif

    if (!config::sunshine.flags[config::flag::FRESH_STATE]) {
      save_state();
      load_state();
    }
  }

  std::shared_ptr<rtsp_stream::launch_session_t> make_launch_session(bool host_audio, bool input_only, const args_t &args, const crypto::named_cert_t* named_cert_p) {
    auto launch_session = std::make_shared<rtsp_stream::launch_session_t>();

    launch_session->id = ++session_id_counter;

    // If launched from client
    if (named_cert_p->uuid != http::unique_id) {
      auto rikey = util::from_hex_vec(get_arg(args, "rikey"), true);
      std::copy(rikey.cbegin(), rikey.cend(), std::back_inserter(launch_session->gcm_key));

      launch_session->host_audio = host_audio;

      // Encrypted RTSP is enabled with client reported corever >= 1
      auto corever = util::from_view(get_arg(args, "corever", "0"));
      if (corever >= 1) {
        launch_session->rtsp_cipher = crypto::cipher::gcm_t {
          launch_session->gcm_key, false
        };
        launch_session->rtsp_iv_counter = 0;
      }
      launch_session->rtsp_url_scheme = launch_session->rtsp_cipher ? "rtspenc://"s : "rtsp://"s;

      // Generate the unique identifiers for this connection that we will send later during RTSP handshake
      unsigned char raw_payload[8];
      RAND_bytes(raw_payload, sizeof(raw_payload));
      launch_session->av_ping_payload = util::hex_vec(raw_payload);
      RAND_bytes((unsigned char *) &launch_session->control_connect_data, sizeof(launch_session->control_connect_data));

      launch_session->iv.resize(16);
      uint32_t prepend_iv = util::endian::big<uint32_t>(util::from_view(get_arg(args, "rikeyid")));
      auto prepend_iv_p = (uint8_t *) &prepend_iv;
      std::copy(prepend_iv_p, prepend_iv_p + sizeof(prepend_iv), std::begin(launch_session->iv));
    }

    std::stringstream mode;
    if (named_cert_p->display_mode.empty()) {
      auto mode_str = get_arg(args, "mode", config::video.fallback_mode.c_str());
      mode = std::stringstream(mode_str);
      BOOST_LOG(info) << "Display mode for client ["sv << named_cert_p->name <<"] requested to ["sv << mode_str << ']';
    } else {
      mode = std::stringstream(named_cert_p->display_mode);
      BOOST_LOG(info) << "Display mode for client ["sv << named_cert_p->name <<"] overriden to ["sv << named_cert_p->display_mode << ']';
    }

    // Split mode by the char "x", to populate width/height/fps
    int x = 0;
    std::string segment;
    while (std::getline(mode, segment, 'x')) {
      if (x == 0) {
        launch_session->width = atoi(segment.c_str());
      }
      if (x == 1) {
        launch_session->height = atoi(segment.c_str());
      }
      if (x == 2) {
        auto fps = atof(segment.c_str());
        if (fps < 1000) {
          fps *= 1000;
        };
        launch_session->fps = (int)fps;
        break;
      }
      x++;
    }

    // Parsing have failed or missing components
    if (x != 2) {
      launch_session->width = 1920;
      launch_session->height = 1080;
      launch_session->fps = 60000; // 60fps * 1000 denominator
    }

    launch_session->requested_width = launch_session->width;
    launch_session->requested_height = launch_session->height;
    launch_session->requested_fps = launch_session->fps;

    launch_session->device_name = named_cert_p->name.empty() ? "PolarisDisplay"s : named_cert_p->name;
    launch_session->unique_id = named_cert_p->uuid;
    launch_session->watch_only = watch_requested(args);
    launch_session->perm = launch_session->watch_only ? PERM::view : named_cert_p->perm;
    launch_session->enable_sops = util::from_view(get_arg(args, "sops", "0"));
    launch_session->surround_info = util::from_view(get_arg(args, "surroundAudioInfo", "196610"));
    launch_session->surround_params = (get_arg(args, "surroundParams", ""));
    launch_session->gcmap = util::from_view(get_arg(args, "gcmap", "0"));
    launch_session->enable_hdr = util::from_view(get_arg(args, "hdrMode", "0"));
    const bool client_display_mode_explicit = util::from_view(get_arg(args, "displayModeExplicit", "0"));
    const bool client_requested_virtual_display = util::from_view(get_arg(args, "virtualDisplay", "0"));
    launch_session->virtual_display = client_requested_virtual_display || named_cert_p->always_use_virtual_display;
    launch_session->user_locked_display_mode = !named_cert_p->display_mode.empty();
    launch_session->user_locked_virtual_display = client_display_mode_explicit || named_cert_p->always_use_virtual_display;
    launch_session->scale_factor = util::from_view(get_arg(args, "scaleFactor", "100"));

    if (!launch_session->watch_only) {
      launch_session->client_do_cmds = named_cert_p->do_cmds;
      launch_session->client_undo_cmds = named_cert_p->undo_cmds;
    }

    launch_session->input_only = input_only;

    return launch_session;
  }

  void remove_session(const pair_session_t &sess) {
    map_id_sess.erase(sess.client.uniqueID);
  }

  void fail_pair(pair_session_t &sess, pt::ptree &tree, const std::string status_msg) {
    tree.put("root.paired", 0);
    tree.put("root.<xmlattr>.status_code", 400);
    tree.put("root.<xmlattr>.status_message", status_msg);
    remove_session(sess);  // Security measure, delete the session when something went wrong and force a re-pair
    BOOST_LOG(warning) << "Pair attempt failed due to " << status_msg;
  }

  void getservercert(pair_session_t &sess, pt::ptree &tree, const std::string &pin) {
    if (sess.last_phase != PAIR_PHASE::NONE) {
      fail_pair(sess, tree, "Out of order call to getservercert");
      return;
    }
    sess.last_phase = PAIR_PHASE::GETSERVERCERT;

    if (sess.async_insert_pin.salt.size() < 32) {
      fail_pair(sess, tree, "Salt too short");
      return;
    }

    std::string_view salt_view {sess.async_insert_pin.salt.data(), 32};

    auto salt = util::from_hex<std::array<uint8_t, 16>>(salt_view, true);

    auto key = crypto::gen_aes_key(salt, pin);
    sess.cipher_key = std::make_unique<crypto::aes_t>(key);

    tree.put("root.paired", 1);
    tree.put("root.plaincert", util::hex_vec(conf_intern.servercert, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientchallenge(pair_session_t &sess, pt::ptree &tree, const std::string &challenge) {
    if (sess.last_phase != PAIR_PHASE::GETSERVERCERT) {
      fail_pair(sess, tree, "Out of order call to clientchallenge");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTCHALLENGE;

    if (!sess.cipher_key) {
      fail_pair(sess, tree, "Cipher key not set");
      return;
    }
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    std::vector<uint8_t> decrypted;
    cipher.decrypt(challenge, decrypted);

    auto x509 = crypto::x509(conf_intern.servercert);
    auto sign = crypto::signature(x509);
    auto serversecret = crypto::rand(16);

    decrypted.insert(std::end(decrypted), std::begin(sign), std::end(sign));
    decrypted.insert(std::end(decrypted), std::begin(serversecret), std::end(serversecret));

    auto hash = crypto::hash({(char *) decrypted.data(), decrypted.size()});
    auto serverchallenge = crypto::rand(16);

    std::string plaintext;
    plaintext.reserve(hash.size() + serverchallenge.size());

    plaintext.insert(std::end(plaintext), std::begin(hash), std::end(hash));
    plaintext.insert(std::end(plaintext), std::begin(serverchallenge), std::end(serverchallenge));

    std::vector<uint8_t> encrypted;
    cipher.encrypt(plaintext, encrypted);

    sess.serversecret = std::move(serversecret);
    sess.serverchallenge = std::move(serverchallenge);

    tree.put("root.paired", 1);
    tree.put("root.challengeresponse", util::hex_vec(encrypted, true));
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void serverchallengeresp(pair_session_t &sess, pt::ptree &tree, const std::string &encrypted_response) {
    if (sess.last_phase != PAIR_PHASE::CLIENTCHALLENGE) {
      fail_pair(sess, tree, "Out of order call to serverchallengeresp");
      return;
    }
    sess.last_phase = PAIR_PHASE::SERVERCHALLENGERESP;

    if (!sess.cipher_key || sess.serversecret.empty()) {
      fail_pair(sess, tree, "Cipher key or serversecret not set");
      return;
    }

    std::vector<uint8_t> decrypted;
    crypto::cipher::ecb_t cipher(*sess.cipher_key, false);

    cipher.decrypt(encrypted_response, decrypted);

    sess.clienthash = std::move(decrypted);

    auto serversecret = sess.serversecret;
    auto sign = crypto::sign256(crypto::pkey(conf_intern.pkey), serversecret);

    serversecret.insert(std::end(serversecret), std::begin(sign), std::end(sign));

    tree.put("root.pairingsecret", util::hex_vec(serversecret, true));
    tree.put("root.paired", 1);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  void clientpairingsecret(pair_session_t &sess, pt::ptree &tree, const std::string &client_pairing_secret) {
    if (sess.last_phase != PAIR_PHASE::SERVERCHALLENGERESP) {
      fail_pair(sess, tree, "Out of order call to clientpairingsecret");
      return;
    }
    sess.last_phase = PAIR_PHASE::CLIENTPAIRINGSECRET;

    auto &client = sess.client;

    if (client_pairing_secret.size() <= 16) {
      fail_pair(sess, tree, "Client pairing secret too short");
      return;
    }

    std::string_view secret {client_pairing_secret.data(), 16};
    std::string_view sign {client_pairing_secret.data() + secret.size(), client_pairing_secret.size() - secret.size()};

    auto x509 = crypto::x509(client.cert);
    if (!x509) {
      fail_pair(sess, tree, "Invalid client certificate");
      return;
    }
    auto x509_sign = crypto::signature(x509);

    std::string data;
    data.reserve(sess.serverchallenge.size() + x509_sign.size() + secret.size());

    data.insert(std::end(data), std::begin(sess.serverchallenge), std::end(sess.serverchallenge));
    data.insert(std::end(data), std::begin(x509_sign), std::end(x509_sign));
    data.insert(std::end(data), std::begin(secret), std::end(secret));

    auto hash = crypto::hash(data);

    // if hash not correct, probably MITM
    bool same_hash = hash.size() == sess.clienthash.size() && std::equal(hash.begin(), hash.end(), sess.clienthash.begin());
    auto verify = crypto::verify256(crypto::x509(client.cert), secret, sign);
    if (same_hash && verify) {
      tree.put("root.paired", 1);

      auto named_cert_p = std::make_shared<crypto::named_cert_t>();
      named_cert_p->name = client.name;
      for (char& c : named_cert_p->name) {
        if (c == '(') c = '[';
        else if (c == ')') c = ']';
      }
      named_cert_p->cert = std::move(client.cert);
      named_cert_p->uuid = uuid_util::uuid_t::generate().string();
      // If the device is the first one paired with the server, assign full permission.
      if (client_root.named_devices.empty()) {
        named_cert_p->perm = PERM::_all;
      } else {
        named_cert_p->perm = PERM::_default;
      }

      named_cert_p->enable_legacy_ordering = true;
      named_cert_p->allow_client_commands = true;
      named_cert_p->always_use_virtual_display = false;

      auto it = map_id_sess.find(client.uniqueID);
      map_id_sess.erase(it);

      add_authorized_client(named_cert_p);
    } else {
      tree.put("root.paired", 0);
      BOOST_LOG(warning) << "Pair attempt failed due to same_hash: " << same_hash << ", verify: " << verify;
    }

    remove_session(sess);
    tree.put("root.<xmlattr>.status_code", 200);
  }

  template<class T>
  struct tunnel;

  template<>
  struct tunnel<PolarisHTTPS> {
    static auto constexpr to_string = "HTTPS"sv;
  };

  template<>
  struct tunnel<SimpleWeb::HTTP> {
    static auto constexpr to_string = "NONE"sv;
  };

  inline crypto::named_cert_t* get_verified_cert(req_https_t request) {
    return (crypto::named_cert_t*)request->userp.get();
  }

  crypto::p_named_cert_t verify_client_cert(SSL *ssl) {
    if (!ssl) {
      return {};
    }

    crypto::x509_t x509 {
#if OPENSSL_VERSION_MAJOR >= 3
      SSL_get1_peer_certificate(ssl)
#else
      SSL_get_peer_certificate(ssl)
#endif
    };

    if (!x509) {
      return {};
    }

    p_named_cert_t named_cert_p;
    auto err_str = cert_chain.verify(x509.get(), named_cert_p);
    if (err_str) {
      BOOST_LOG(warning) << "SSL Verification error :: "sv << err_str;
      return {};
    }

    return named_cert_p;
  }

  template <class T>
  void print_req(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    BOOST_LOG(debug) << "TUNNEL :: "sv << tunnel<T>::to_string;

    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;

    for (auto &[name, val] : request->header) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;

    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << val;
    }

    BOOST_LOG(debug) << " [--] "sv;
  }

  template<class T>
  void not_found(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;
    tree.put("root.<xmlattr>.status_code", 404);

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(SimpleWeb::StatusCode::client_error_not_found, data.str());
    response->close_connection_after_response = true;
  }

  template <class T>
  void pair(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    pt::ptree tree;

    auto fg = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    if (!config::sunshine.enable_pairing) {
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Pairing is disabled for this instance");

      return;
    }

    auto args = request->parse_query_string();
    if (args.find("uniqueid"s) == std::end(args)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing uniqueid parameter");

      return;
    }

    auto uniqID {get_arg(args, "uniqueid")};

    args_t::const_iterator it;
    if (it = args.find("phrase"); it != std::end(args)) {
      if (it->second == "getservercert"sv) {
        pair_session_t sess;

        auto deviceName { get_arg(args, "devicename") };

        if (deviceName == "roth"sv) {
          deviceName = "Legacy Moonlight Client";
        }

        sess.client.uniqueID = std::move(uniqID);
        sess.client.name = std::move(deviceName);
        sess.client.cert = util::from_hex_vec(get_arg(args, "clientcert"), true);

        BOOST_LOG(debug) << sess.client.cert;
        auto ptr = map_id_sess.emplace(sess.client.uniqueID, std::move(sess)).first;

        ptr->second.async_insert_pin.salt = std::move(get_arg(args, "salt"));

        auto it = args.find("otpauth");
        if (it != std::end(args)) {
          if (one_time_pin.empty() || (std::chrono::steady_clock::now() - otp_creation_time > OTP_EXPIRE_DURATION)) {
            one_time_pin.clear();
            otp_passphrase.clear();
            otp_device_name.clear();
            tree.put("root.<xmlattr>.status_code", 503);
            tree.put("root.<xmlattr>.status_message", "OTP auth not available.");
          } else {
            auto hash = util::hex(crypto::hash(one_time_pin + ptr->second.async_insert_pin.salt + otp_passphrase), true);

            if (hash.to_string_view() == it->second) {

              if (!otp_device_name.empty()) {
                ptr->second.client.name = std::move(otp_device_name);
              }

              getservercert(ptr->second, tree, one_time_pin);

              one_time_pin.clear();
              otp_passphrase.clear();
              otp_device_name.clear();
              return;
            }
          }

          // Always return positive, attackers will fail in the next steps.
          getservercert(ptr->second, tree, crypto::rand(16));
          return;
        }

        if (config::sunshine.flags[config::flag::PIN_STDIN]) {
          std::string pin;

          std::cout << "Please insert pin: "sv;
          std::getline(std::cin, pin);

          getservercert(ptr->second, tree, pin);
          return;
        } else if (is_in_trusted_subnet(request->remote_endpoint().address())) {
          // TOFU: Auto-approve pairing from trusted subnet with well-known PIN
          BOOST_LOG(info) << "TOFU: Auto-approving pairing from trusted subnet: "
                          << net::addr_to_normalized_string(request->remote_endpoint().address());
          getservercert(ptr->second, tree, "0000");
          return;
        } else {
#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
          system_tray::update_tray_require_pin();
#endif
          ptr->second.async_insert_pin.response = std::move(response);

          fg.disable();
          return;
        }
      } else if (it->second == "pairchallenge"sv) {
        tree.put("root.paired", 1);
        tree.put("root.<xmlattr>.status_code", 200);
        return;
      }
    }

    auto sess_it = map_id_sess.find(uniqID);
    if (sess_it == std::end(map_id_sess)) {
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Invalid uniqueid");

      return;
    }

    if (it = args.find("clientchallenge"); it != std::end(args)) {
      auto challenge = util::from_hex_vec(it->second, true);
      clientchallenge(sess_it->second, tree, challenge);
    } else if (it = args.find("serverchallengeresp"); it != std::end(args)) {
      auto encrypted_response = util::from_hex_vec(it->second, true);
      serverchallengeresp(sess_it->second, tree, encrypted_response);
    } else if (it = args.find("clientpairingsecret"); it != std::end(args)) {
      auto pairingsecret = util::from_hex_vec(it->second, true);
      clientpairingsecret(sess_it->second, tree, pairingsecret);
    } else {
      tree.put("root.<xmlattr>.status_code", 404);
      tree.put("root.<xmlattr>.status_message", "Invalid pairing request");
    }
  }

  bool pin(std::string pin, std::string name) {
    pt::ptree tree;
    if (map_id_sess.empty()) {
      return false;
    }

    // ensure pin is 4 digits
    if (pin.size() != 4) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put(
        "root.<xmlattr>.status_message",
        std::format("Pin must be 4 digits, {} provided", pin.size())
      );
      return false;
    }

    // ensure all pin characters are numeric
    if (!std::all_of(pin.begin(), pin.end(), ::isdigit)) {
      tree.put("root.paired", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Pin must be numeric");
      return false;
    }

    auto &sess = std::begin(map_id_sess)->second;
    getservercert(sess, tree, pin);

    if (!name.empty()) {
      sess.client.name = name;
    }

    // response to the request for pin
    std::ostringstream data;
    pt::write_xml(data, tree);

    auto &async_response = sess.async_insert_pin.response;
    if (async_response.has_left() && async_response.left()) {
      async_response.left()->write(data.str());
    } else if (async_response.has_right() && async_response.right()) {
      async_response.right()->write(data.str());
    } else {
      return false;
    }

    // reset async_response
    async_response = std::decay_t<decltype(async_response.left())>();
    // response to the current request
    return true;
  }

  template<class T>
  void serverinfo(std::shared_ptr<typename SimpleWeb::ServerBase<T>::Response> response, std::shared_ptr<typename SimpleWeb::ServerBase<T>::Request> request) {
    print_req<T>(request);

    int pair_status = 0;
    if constexpr (std::is_same_v<PolarisHTTPS, T>) {
      auto args = request->parse_query_string();
      auto clientID = args.find("uniqueid"s);

      if (clientID != std::end(args)) {
        pair_status = 1;
      }
    }

    auto local_endpoint = request->local_endpoint();
    const crypto::named_cert_t *named_cert_p = nullptr;
    if constexpr (std::is_same_v<PolarisHTTPS, T>) {
      named_cert_p = get_verified_cert(request);
    }
    const auto advertised_codec_support = advertised_codec_support_for_http(std::is_same_v<PolarisHTTPS, T>);

    pt::ptree tree;

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put("root.hostname", config::nvhttp.sunshine_name);

    tree.put("root.appversion", VERSION);
    tree.put("root.GfeVersion", GFE_VERSION);
    tree.put("root.uniqueid", http::unique_id);
    tree.put("root.HttpsPort", net::map_port(PORT_HTTPS));
    tree.put("root.ExternalPort", net::map_port(PORT_HTTP));
    tree.put("root.MaxLumaPixelsHEVC", advertised_codec_support.hevc_mode > 1 ? "1869449984" : "0");

    // Only include the MAC address for requests sent from paired clients over HTTPS.
    // For HTTP requests, use a placeholder MAC address that Moonlight knows to ignore.
    if constexpr (std::is_same_v<PolarisHTTPS, T>) {
      tree.put("root.mac", platf::get_mac_address(net::addr_to_normalized_string(local_endpoint.address())));
      if (!!(named_cert_p->perm & PERM::server_cmd)) {
        pt::ptree& root_node = tree.get_child("root");

        if (config::sunshine.server_cmds.size() > 0) {
          // Broadcast server_cmds
          for (const auto& cmd : config::sunshine.server_cmds) {
            pt::ptree cmd_node;
            cmd_node.put_value(cmd.cmd_name);
            root_node.push_back(std::make_pair("ServerCommand", cmd_node));
          }
        }
      } else {
        BOOST_LOG(debug) << "Permission Get ServerCommand denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";
      }

      tree.put("root.Permission", std::to_string((uint32_t)named_cert_p->perm));

    #ifdef _WIN32
      tree.put("root.VirtualDisplayCapable", true);
      if (!!(named_cert_p->perm & PERM::_all_actions)) {
        tree.put("root.VirtualDisplayDriverReady", proc::vDisplayDriverStatus == VDISPLAY::DRIVER_STATUS::OK);
      } else {
        tree.put("root.VirtualDisplayDriverReady", true);
      }
    #endif
    } else {
      tree.put("root.mac", "00:00:00:00:00:00");
      tree.put("root.Permission", "0");
    }

    // Moonlight clients track LAN IPv6 addresses separately from LocalIP which is expected to
    // always be an IPv4 address. If we return that same IPv6 address here, it will clobber the
    // stored LAN IPv4 address. To avoid this, we need to return an IPv4 address in this field
    // when we get a request over IPv6.
    //
    // HACK: We should return the IPv4 address of local interface here, but we don't currently
    // have that implemented. For now, we will emulate the behavior of GFE+GS-IPv6-Forwarder,
    // which returns 127.0.0.1 as LocalIP for IPv6 connections. Moonlight clients with IPv6
    // support know to ignore this bogus address.
    if (local_endpoint.address().is_v6() && !local_endpoint.address().to_v6().is_v4_mapped()) {
      tree.put("root.LocalIP", "127.0.0.1");
    } else {
      tree.put("root.LocalIP", net::addr_to_normalized_string(local_endpoint.address()));
    }

    // Advertise TOFU support for clients on trusted subnets
    if (is_in_trusted_subnet(request->remote_endpoint().address())) {
      tree.put("root.TofuEnabled", 1);
    }

    uint32_t codec_mode_flags = SCM_H264;
    if (advertised_codec_support.yuv444_for_codec[0]) {
      codec_mode_flags |= SCM_H264_HIGH8_444;
    }
    if (advertised_codec_support.hevc_mode >= 2) {
      codec_mode_flags |= SCM_HEVC;
      if (advertised_codec_support.yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT8_444;
      }
    }
    if (advertised_codec_support.hevc_mode >= 3) {
      codec_mode_flags |= SCM_HEVC_MAIN10;
      if (advertised_codec_support.yuv444_for_codec[1]) {
        codec_mode_flags |= SCM_HEVC_REXT10_444;
      }
    }
    if (advertised_codec_support.av1_mode >= 2) {
      codec_mode_flags |= SCM_AV1_MAIN8;
      if (advertised_codec_support.yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH8_444;
      }
    }
    if (advertised_codec_support.av1_mode >= 3) {
      codec_mode_flags |= SCM_AV1_MAIN10;
      if (advertised_codec_support.yuv444_for_codec[2]) {
        codec_mode_flags |= SCM_AV1_HIGH10_444;
      }
    }
    tree.put("root.ServerCodecModeSupport", codec_mode_flags);
    tree.put("root.ServerMaxLaunchRefreshRate", advertised_max_launch_refresh_rate_for_http());

    tree.put("root.PairStatus", pair_status);

    if constexpr (std::is_same_v<PolarisHTTPS, T>) {
      int current_appid = proc::proc.running();
      // When input only mode is enabled, the only resume method should be launching the same app again.
      if (config::input.enable_input_only_mode && current_appid != proc::input_only_app_id) {
        current_appid = 0;
      }
      tree.put("root.currentgame", current_appid);
      tree.put("root.currentgameuuid", proc::proc.get_running_app_uuid());
      tree.put("root.state", current_appid > 0 ? "POLARIS_SERVER_BUSY" : "POLARIS_SERVER_FREE");
      append_current_game_session_fields(tree, named_cert_p);
    } else {
      tree.put("root.currentgame", 0);
      tree.put("root.currentgameuuid", "");
      tree.put("root.state", "POLARIS_SERVER_FREE");
      tree.put("root.currentgamesessiontoken", "");
      tree.put("root.currentgameowner", "");
      tree.put("root.currentgameviewercount", 0);
      tree.put("root.currentgameowned", 0);
    }

    std::ostringstream data;

    pt::write_xml(data, tree);
    response->write(data.str());
    response->close_connection_after_response = true;
  }

  nlohmann::json get_all_clients() {
    nlohmann::json named_cert_nodes = nlohmann::json::array();
    client_t &client = client_root;
    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    for (auto &named_cert : client.named_devices) {
      nlohmann::json named_cert_node;
      named_cert_node["name"] = named_cert->name;
      named_cert_node["uuid"] = named_cert->uuid;
      named_cert_node["display_mode"] = named_cert->display_mode;
      named_cert_node["perm"] = static_cast<uint32_t>(named_cert->perm);
      named_cert_node["enable_legacy_ordering"] = named_cert->enable_legacy_ordering;
      named_cert_node["allow_client_commands"] = named_cert->allow_client_commands;
      named_cert_node["always_use_virtual_display"] = named_cert->always_use_virtual_display;

      // Add "do" commands if available
      if (!named_cert->do_cmds.empty()) {
        nlohmann::json do_cmds_node = nlohmann::json::array();
        for (const auto &cmd : named_cert->do_cmds) {
          do_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
        }
        named_cert_node["do"] = do_cmds_node;
      }

      // Add "undo" commands if available
      if (!named_cert->undo_cmds.empty()) {
        nlohmann::json undo_cmds_node = nlohmann::json::array();
        for (const auto &cmd : named_cert->undo_cmds) {
          undo_cmds_node.push_back(crypto::command_entry_t::serialize(cmd));
        }
        named_cert_node["undo"] = undo_cmds_node;
      }

      // Determine connection status
      bool connected = false;
      if (connected_uuids.empty()) {
        connected = false;
      } else {
        for (auto it = connected_uuids.begin(); it != connected_uuids.end(); ++it) {
          if (*it == named_cert->uuid) {
            connected = true;
            connected_uuids.erase(it);
            break;
          }
        }
      }
      named_cert_node["connected"] = connected;

      named_cert_nodes.push_back(named_cert_node);
    }

    return named_cert_nodes;
  }

  void applist(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);
    const auto advertised_codec_support = advertised_codec_support_for_http(true);

    pt::ptree tree;

    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto &apps = tree.add_child("root", pt::ptree {});

    apps.put("<xmlattr>.status_code", 200);

    auto named_cert_p = get_verified_cert(request);
    if (!!(named_cert_p->perm & PERM::_all_actions)) {
      auto current_appid = proc::proc.running();
      auto should_hide_inactive_apps = config::input.enable_input_only_mode && current_appid > 0 && current_appid != proc::input_only_app_id;

      auto app_list = proc::proc.get_apps();

      bool enable_legacy_ordering = config::sunshine.legacy_ordering && named_cert_p->enable_legacy_ordering;
      size_t bits;
      if (enable_legacy_ordering) {
        bits = zwpad::pad_width_for_count(app_list.size());
      }

      for (size_t i = 0; i < app_list.size(); i++) {
        auto& app = app_list[i];
        auto appid = util::from_view(app.id);
        if (should_hide_inactive_apps) {
          if (
            appid != current_appid
            && appid != proc::input_only_app_id
            && appid != proc::terminate_app_id
          ) {
            continue;
          }
        } else {
          if (appid == proc::terminate_app_id) {
            continue;
          }
        }

        std::string app_name;
        if (enable_legacy_ordering) {
          app_name = zwpad::pad_for_ordering(app.name, bits, i);
        } else {
          app_name = app.name;
        }

        pt::ptree app_node;

        app_node.put("IsHdrSupported"s, advertised_codec_support.hevc_mode == 3 ? 1 : 0);
        app_node.put("AppTitle"s, app_name);
        app_node.put("UUID", app.uuid);
        app_node.put("IDX", app.idx);
        app_node.put("ID", app.id);

        apps.push_back(std::make_pair("App", std::move(app_node)));
      }
    } else {
      BOOST_LOG(debug) << "Permission ListApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      pt::ptree app_node;

      app_node.put("IsHdrSupported"s, 0);
      app_node.put("AppTitle"s, "Permission Denied");
      app_node.put("UUID", "");
      app_node.put("IDX", "0");
      app_node.put("ID", "114514");

      apps.push_back(std::make_pair("App", std::move(app_node)));

      return;
    }

  }

  void launch(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();

    auto appid_str = get_arg(args, "appid", "0");
    auto appuuid_str = get_arg(args, "appuuid", "");
    auto appid = util::from_view(appid_str);
    auto current_appid = proc::proc.running();
    auto current_app_uuid = proc::proc.get_running_app_uuid();
    bool is_input_only = config::input.enable_input_only_mode && (appid == proc::input_only_app_id || (appuuid_str == REMOTE_INPUT_UUID));

    auto named_cert_p = get_verified_cert(request);
    auto perm = PERM::launch;

    BOOST_LOG(verbose) << "Launching app [" << appid_str << "] with UUID [" << appuuid_str << "]";
    // BOOST_LOG(verbose) << "QS: " << request->query_string;

    // If we have already launched an app, we should allow clients with view permission to join the input only or current app's session.
    if (
      current_appid > 0
      && (appuuid_str != TERMINATE_APP_UUID || appid != proc::terminate_app_id)
      && (is_input_only || appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid))
    ) {
      perm = PERM::_allow_view;
    }

    if (!(named_cert_p->perm & perm)) {
      BOOST_LOG(debug) << "Permission LaunchApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args) ||
      args.find("localAudioPlayMode"s) == std::end(args) ||
      (args.find("appid"s) == std::end(args) && args.find("appuuid"s) == std::end(args))
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required launch parameter");

      return;
    }

    if (!is_input_only) {
      // Special handling for the "terminate" app
      if (
        (config::input.enable_input_only_mode && appid == proc::terminate_app_id)
        || appuuid_str == TERMINATE_APP_UUID
      ) {
        proc::proc.terminate();

        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 410);
        tree.put("root.<xmlattr>.status_message", "App terminated.");

        return;
      }

      if (
        current_appid > 0
        && current_appid != proc::input_only_app_id
        && (
          (appid > 0 && appid != current_appid)
          || (!appuuid_str.empty() && appuuid_str != current_app_uuid)
        )
      ) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 400);
        tree.put("root.<xmlattr>.status_message", "An app is already running on this host");

        return;
      }
    }

    host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    auto launch_session = make_launch_session(host_audio, is_input_only, args, named_cert_p);
    const bool watch_only = launch_session->watch_only;

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    bool no_active_sessions = rtsp_stream::session_count() == 0;
    if (watch_only && (current_appid == 0 || no_active_sessions)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "No active stream is available to watch");

      return;
    }

    if (const auto watch_error = pin_watch_session_to_active_profile(*launch_session)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", watch_error->first);
      tree.put("root.<xmlattr>.status_message", watch_error->second);

      return;
    }

    if (is_input_only) {
      BOOST_LOG(info) << "Launching input only session..."sv;

      launch_session->client_do_cmds.clear();
      launch_session->client_undo_cmds.clear();

      if (current_appid != 0) {
        if (!watch_only && !proc::proc.is_session_owner(named_cert_p->uuid)) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 470);
          tree.put("root.<xmlattr>.status_message", "The current session belongs to another client");

          return;
        }
        if (!watch_only && !session_token_matches_request(args)) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 470);
          tree.put("root.<xmlattr>.status_message", "The requested session token does not match the active session");

          return;
        }

        launch_session->session_token = proc::proc.get_session_token();
      }

      // Still probe encoders once, if input only session is launched first
      // But we're ignoring if it's successful or not
      if (no_active_sessions && !proc::proc.virtual_display) {
#ifdef __linux__
        if (should_defer_encoder_probe_until_cage()) {
          BOOST_LOG(info) << "nvhttp: Deferring input-only encoder probe until the cage runtime is available"sv;
        } else
#endif
        {
          video::probe_encoders();
        }
        if (current_appid == 0) {
          proc::proc.launch_input_only(launch_session);
        }
      }
    } else if (appid > 0 || !appuuid_str.empty()) {
      if (appid == current_appid || (!appuuid_str.empty() && appuuid_str == current_app_uuid)) {
        // We're basically resuming the same app

        BOOST_LOG(debug) << "Resuming app [" << proc::proc.get_last_run_app_name() << "] from launch app path...";

        if (!watch_only && !proc::proc.is_session_owner(named_cert_p->uuid)) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 470);
          tree.put("root.<xmlattr>.status_message", "The current session belongs to another client");

          return;
        }
        if (!watch_only && !session_token_matches_request(args)) {
          tree.put("root.resume", 0);
          tree.put("root.<xmlattr>.status_code", 470);
          tree.put("root.<xmlattr>.status_message", "The requested session token does not match the active session");

          return;
        }

        launch_session->session_token = proc::proc.get_session_token();

        if (watch_only || !proc::proc.allow_client_commands || !named_cert_p->allow_client_commands) {
          launch_session->client_do_cmds.clear();
          launch_session->client_undo_cmds.clear();
        }

        if (current_appid == proc::input_only_app_id) {
          launch_session->input_only = true;
        }

        if (no_active_sessions && !proc::proc.virtual_display) {
          display_device::configure_display(config::video, *launch_session);
#ifdef __linux__
          if (should_defer_encoder_probe_until_cage()) {
            BOOST_LOG(info) << "nvhttp: Deferring resume-time encoder probe until the cage runtime is available"sv;
          } else
#endif
          if (video::probe_encoders()) {
            tree.put("root.resume", 0);
            tree.put("root.<xmlattr>.status_code", 503);
            tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

            return;
          }
        }
      } else {
        const auto& apps = proc::proc.get_apps();
        auto app_iter = std::find_if(apps.begin(), apps.end(), [&appid_str, &appuuid_str](const auto _app) {
          return _app.id == appid_str || _app.uuid == appuuid_str;
        });

        if (app_iter == apps.end()) {
          BOOST_LOG(error) << "Couldn't find app with ID ["sv << appid_str << "] or UUID ["sv << appuuid_str << ']';
          tree.put("root.<xmlattr>.status_code", 404);
          tree.put("root.<xmlattr>.status_message", "Cannot find requested application");
          tree.put("root.gamesession", 0);
          return;
        }

        if (!app_iter->allow_client_commands) {
          launch_session->client_do_cmds.clear();
          launch_session->client_undo_cmds.clear();
        }

        // Update last_launched timestamp
        try {
          std::string content = file_handler::read_file(config::stream.file_apps.c_str());
          auto file_tree = nlohmann::json::parse(content);
          if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
            for (auto &app_node : file_tree["apps"]) {
              if (app_node.value("uuid", "") == app_iter->uuid) {
                app_node["last-launched"] = std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
                break;
              }
            }
            file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
          }
        } catch (...) {}

        auto err = proc::proc.execute(*app_iter, launch_session);
        if (err) {
          tree.put("root.<xmlattr>.status_code", err);
          tree.put(
            "root.<xmlattr>.status_message",
            err == 503
            ? "Failed to initialize video capture/encoding. Is a display connected and turned on?"
            : "Failed to start the specified application");
          tree.put("root.gamesession", 0);

          return;
        }
      }
    } else {
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "How did you get here?");
      tree.put("root.gamesession", 0);
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.sessionToken", launch_session->session_token);
    tree.put("root.gamesession", 1);

    rtsp_stream::launch_session_raise(launch_session);
  }

  void resume(bool &host_audio, resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto named_cert_p = get_verified_cert(request);
    if (!(named_cert_p->perm & PERM::_allow_view)) {
      BOOST_LOG(debug) << "Permission ViewApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }

    auto current_appid = proc::proc.running();
    if (current_appid == 0) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 503);
      tree.put("root.<xmlattr>.status_message", "No running app to resume");

      return;
    }

    auto args = request->parse_query_string();
    if (
      args.find("rikey"s) == std::end(args) ||
      args.find("rikeyid"s) == std::end(args)
    ) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 400);
      tree.put("root.<xmlattr>.status_message", "Missing a required resume parameter");

      return;
    }

    // Newer Moonlight clients send localAudioPlayMode on /resume too,
    // so we should use it if it's present in the args and there are
    // no active sessions we could be interfering with.
    const bool no_active_sessions {rtsp_stream::session_count() == 0};
    if (no_active_sessions && args.find("localAudioPlayMode"s) != std::end(args)) {
      host_audio = util::from_view(get_arg(args, "localAudioPlayMode"));
    }
    auto launch_session = make_launch_session(host_audio, false, args, named_cert_p);
    const bool watch_only = launch_session->watch_only;

    if (watch_only && no_active_sessions) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 409);
      tree.put("root.<xmlattr>.status_message", "No active stream is available to watch");

      return;
    }

    if (const auto watch_error = pin_watch_session_to_active_profile(*launch_session)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", watch_error->first);
      tree.put("root.<xmlattr>.status_message", watch_error->second);

      return;
    }

    if (!watch_only && !proc::proc.is_session_owner(named_cert_p->uuid)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 470);
      tree.put("root.<xmlattr>.status_message", "The current session belongs to another client");

      return;
    }
    if (!watch_only && !session_token_matches_request(args)) {
      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 470);
      tree.put("root.<xmlattr>.status_message", "The requested session token does not match the active session");

      return;
    }
    launch_session->session_token = proc::proc.get_session_token();

    if (watch_only || !proc::proc.allow_client_commands || !named_cert_p->allow_client_commands) {
      launch_session->client_do_cmds.clear();
      launch_session->client_undo_cmds.clear();
    }

    if (config::input.enable_input_only_mode && current_appid == proc::input_only_app_id) {
      launch_session->input_only = true;
    }

    if (no_active_sessions && !proc::proc.virtual_display) {
      // We want to prepare display only if there are no active sessions
      // and the current session isn't virtual display at the moment.
      // This should be done before probing encoders as it could change the active displays.
      display_device::configure_display(config::video, *launch_session);

      // Probe encoders again before streaming to ensure our chosen
      // encoder matches the active GPU (which could have changed
      // due to hotplugging, driver crash, primary monitor change,
      // or any number of other factors).
#ifdef __linux__
      if (should_defer_encoder_probe_until_cage()) {
        BOOST_LOG(info) << "nvhttp: Deferring launch-time encoder probe until the cage runtime is available"sv;
      } else
#endif
      if (video::probe_encoders()) {
        tree.put("root.resume", 0);
        tree.put("root.<xmlattr>.status_code", 503);
        tree.put("root.<xmlattr>.status_message", "Failed to initialize video capture/encoding. Is a display connected and turned on?");

        return;
      }
    }

    auto encryption_mode = net::encryption_mode_for_address(request->remote_endpoint().address());
    if (!launch_session->rtsp_cipher && encryption_mode == config::ENCRYPTION_MODE_MANDATORY) {
      BOOST_LOG(error) << "Rejecting client that cannot comply with mandatory encryption requirement"sv;

      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Encryption is mandatory for this host but unsupported by the client");
      tree.put("root.gamesession", 0);

      return;
    }

    tree.put("root.<xmlattr>.status_code", 200);
    tree.put(
      "root.sessionUrl0",
      std::format(
        "{}{}:{}",
        launch_session->rtsp_url_scheme,
        net::addr_to_url_escaped_string(request->local_endpoint().address()),
        static_cast<int>(net::map_port(rtsp_stream::RTSP_SETUP_PORT))
      )
    );
    tree.put("root.sessionToken", launch_session->session_token);
    tree.put("root.resume", 1);

    rtsp_stream::launch_session_raise(launch_session);

#if defined POLARIS_TRAY && POLARIS_TRAY >= 1
    system_tray::update_tray_client_connected(named_cert_p->name);
#endif
  }

  void cancel(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    pt::ptree tree;
    auto g = util::fail_guard([&]() {
      std::ostringstream data;

      pt::write_xml(data, tree);
      response->write(data.str());
      response->close_connection_after_response = true;
    });

    auto args = request->parse_query_string();
    auto named_cert_p = get_verified_cert(request);
    if (!(named_cert_p->perm & PERM::launch)) {
      BOOST_LOG(debug) << "Permission CancelApp denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      tree.put("root.resume", 0);
      tree.put("root.<xmlattr>.status_code", 403);
      tree.put("root.<xmlattr>.status_message", "Permission denied");

      return;
    }

    if (proc::proc.running() > 0 && !proc::proc.is_session_owner(named_cert_p->uuid)) {
      tree.put("root.cancel", 0);
      tree.put("root.<xmlattr>.status_code", 470);
      tree.put("root.<xmlattr>.status_message", "The current session belongs to another client");

      return;
    }
    if (proc::proc.running() > 0 && !session_token_matches_request(args)) {
      tree.put("root.cancel", 0);
      tree.put("root.<xmlattr>.status_code", 470);
      tree.put("root.<xmlattr>.status_message", "The requested session token does not match the active session");

      return;
    }

    tree.put("root.cancel", 1);
    tree.put("root.<xmlattr>.status_code", 200);

    proc::proc.set_session_shutdown_requested(true);
    rtsp_stream::terminate_sessions();

    if (proc::proc.running() > 0) {
      proc::proc.terminate();
    } else {
      proc::proc.set_session_shutdown_requested(false);
    }

    // The config needs to be reverted regardless of whether "proc::proc.terminate()" was called or not.
    display_device::revert_configuration();
  }

  void appasset(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    auto fg = util::fail_guard([&]() {
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
    });

    auto named_cert_p = get_verified_cert(request);

    if (!(named_cert_p->perm & PERM::_all_actions)) {
      BOOST_LOG(debug) << "Permission Get AppAsset denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      fg.disable();
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto app_image = proc::proc.get_app_image(util::from_view(get_arg(args, "appid")));

    fg.disable();

    std::ifstream in(app_image, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
    response->close_connection_after_response = true;
  }

  void getClipboard(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);

    if (
      !(named_cert_p->perm & PERM::_allow_view)
      || !(named_cert_p->perm & PERM::clipboard_read)
    ) {
      BOOST_LOG(debug) << "Permission Read Clipboard denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    if (clipboard_type != "text"sv) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), named_cert_p->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client ["<< named_cert_p->name << "] trying to get clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    std::string content = platf::get_clipboard();
    response->write(content);
    return;
  }

  void
  setClipboard(resp_https_t response, req_https_t request) {
    print_req<PolarisHTTPS>(request);

    auto named_cert_p = get_verified_cert(request);

    if (
      !(named_cert_p->perm & PERM::_allow_view)
      || !(named_cert_p->perm & PERM::clipboard_set)
    ) {
      BOOST_LOG(debug) << "Permission Write Clipboard denied for [" << named_cert_p->name << "] (" << (uint32_t)named_cert_p->perm << ")";

      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
      response->close_connection_after_response = true;
      return;
    }

    auto args = request->parse_query_string();
    auto clipboard_type = get_arg(args, "type");
    if (clipboard_type != "text"sv) {
      BOOST_LOG(debug) << "Clipboard type [" << clipboard_type << "] is not supported!";

      response->write(SimpleWeb::StatusCode::client_error_bad_request);
      response->close_connection_after_response = true;
      return;
    }

    std::list<std::string> connected_uuids = rtsp_stream::get_all_session_uuids();

    bool found = !connected_uuids.empty();

    if (found) {
      found = (std::find(connected_uuids.begin(), connected_uuids.end(), named_cert_p->uuid) != connected_uuids.end());
    }

    if (!found) {
      BOOST_LOG(debug) << "Client ["<< named_cert_p->name << "] trying to set clipboard is not connected to a stream";

      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      response->close_connection_after_response = true;
      return;
    }

    std::string content = request->content.string();

    bool success = platf::set_clipboard(content);

    if (!success) {
      BOOST_LOG(debug) << "Setting clipboard failed!";

      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      response->close_connection_after_response = true;
    }

    response->write();
    return;
  }

  void setup(const std::string &pkey, const std::string &cert) {
    conf_intern.pkey = pkey;
    conf_intern.servercert = cert;
  }

  void start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);

    auto port_http = net::map_port(PORT_HTTP);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::sunshine.address_family);

    bool clean_slate = config::sunshine.flags[config::flag::FRESH_STATE];

    if (!clean_slate) {
      load_state();
    }

    auto pkey = file_handler::read_file(config::nvhttp.pkey.c_str());
    auto cert = file_handler::read_file(config::nvhttp.cert.c_str());
    setup(pkey, cert);

    // resume doesn't always get the parameter "localAudioPlayMode"
    // launch will store it in host_audio
    bool host_audio {};

    https_server_t https_server {config::nvhttp.cert, config::nvhttp.pkey};
    http_server_t http_server;

    // Verify certificates after establishing connection
    https_server.verify = [](req_https_t req, SSL *ssl) {
      auto named_cert_p = verify_client_cert(ssl);
      if (!named_cert_p) {
        BOOST_LOG(info) << "unknown -- denied"sv;
        return false;
      }

      req->userp = named_cert_p;
      BOOST_LOG(debug) << named_cert_p->name << " -- verified"sv;

      return true;
    };

    https_server.on_verify_failed = [](resp_https_t resp, req_https_t req) {
      pt::ptree tree;
      auto g = util::fail_guard([&]() {
        std::ostringstream data;

        pt::write_xml(data, tree);
        resp->write(data.str());
        resp->close_connection_after_response = true;
      });

      tree.put("root.<xmlattr>.status_code"s, 401);
      tree.put("root.<xmlattr>.query"s, req->path);
      tree.put("root.<xmlattr>.status_message"s, "The client is not authorized. Certificate verification failed."s);
    };

    https_server.default_resource["GET"] = not_found<PolarisHTTPS>;
    https_server.resource["^/serverinfo$"]["GET"] = serverinfo<PolarisHTTPS>;
    https_server.resource["^/pair$"]["GET"] = pair<PolarisHTTPS>;
    https_server.resource["^/applist$"]["GET"] = applist;
    https_server.resource["^/appasset$"]["GET"] = appasset;
    https_server.resource["^/launch$"]["GET"] = [&host_audio](auto resp, auto req) {
      launch(host_audio, resp, req);
    };
    https_server.resource["^/resume$"]["GET"] = [&host_audio](auto resp, auto req) {
      resume(host_audio, resp, req);
    };
    https_server.resource["^/cancel$"]["GET"] = cancel;
    https_server.resource["^/actions/clipboard$"]["GET"] = getClipboard;
    https_server.resource["^/actions/clipboard$"]["POST"] = setClipboard;

    // -----------------------------------------------------------------------
    // Polaris v1 API — client cert auth (same TLS as Moonlight pairing)
    // -----------------------------------------------------------------------

    auto polarisCapabilities = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);

      auto named_cert_p = get_verified_cert(request);
      if (!named_cert_p) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      nlohmann::json output;
      output["server"] = "polaris";
      output["version"] = PROJECT_VERSION;

      // Feature flags
      auto &features = output["features"];
      features["ai_optimizer"] = ai_optimizer::is_enabled();
      features["ai_optimizer_control"] = true;
      features["adaptive_bitrate_control"] = true;
      features["game_library"] = true;
      features["session_lifecycle"] = true;
      features["device_profiles"] = true;
      features["cursor_visibility_control"] = true;
      features["lock_screen_control"] = false;
#ifdef __linux__
      features["lock_screen_control"] = true;
#endif

      // Capture info
      auto &capture = output["capture"];
#ifdef __linux__
      capture["backend"] = cage_display_router::is_running() ? "cage-screencopy" : "portal";
      capture["compositor"] = "cage";
#else
      capture["backend"] = "platform";
      capture["compositor"] = "none";
#endif
      capture["max_resolution"] = "3840x2160";
      capture["max_fps"] = 120;

      auto &codecs = capture["codecs"];
      codecs = nlohmann::json::array({"h264"});
      if (config::video.hevc_mode > 1) codecs.push_back("hevc");
      if (config::video.av1_mode > 1) codecs.push_back("av1");

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    auto polarisSessionStatus = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);

      const auto named_cert_p = get_verified_cert(request);
      if (!named_cert_p) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      nlohmann::json output;

      // Session state from the state machine
      auto stats = stream_stats::get_current();
      const auto session_state = confighttp::get_session_state();
      const auto session_token = proc::proc.get_session_token();
      const bool owned_by_client =
        !session_token.empty() && proc::proc.is_session_owner(named_cert_p->uuid);
      const bool shutdown_requested = proc::proc.session_shutdown_requested();
      output["state"] = session_state;
      output["streaming_active"] = stats.streaming;
      output["shutdown_requested"] = shutdown_requested;
#ifdef __linux__
      output["cage_pid"] = cage_display_router::get_pid();
      output["screen_locked"] = session_manager::is_screen_locked();
#endif
      output["session_token"] = session_token;
      output["owner_unique_id"] = proc::proc.get_session_owner_unique_id();
      output["owner_device_name"] = proc::proc.get_session_owner_device_name();
      output["owned_by_client"] = owned_by_client;
      output["viewer_count"] = rtsp_stream::viewer_count();

      std::string client_role = "none";
      if (const auto session = rtsp_stream::find_session(named_cert_p->uuid)) {
        client_role = stream::session::is_watch_only(*session) ? "viewer" : "owner";
      }
      output["client_role"] = client_role;
      const bool host_tuning_allowed =
        owned_by_client &&
        client_role != "viewer" &&
        !shutdown_requested &&
        stats.streaming;
      const bool quit_allowed =
        host_tuning_allowed &&
        proc::proc.allow_client_commands &&
        named_cert_p->allow_client_commands &&
        proc::proc.running() > 0;

      // Game info
      output["game_id"] = proc::proc.running();
      output["game"] = proc::proc.get_last_run_app_name();
      output["game_uuid"] = proc::proc.get_running_app_uuid();
      output["cursor_visible"] = cursor::visible();
      output["dynamic_range"] = stats.dynamic_range;
      output["adaptive_bitrate_enabled"] = adaptive_bitrate::is_enabled();
      output["adaptive_target_bitrate_kbps"] = stats.adaptive_target_bitrate_kbps;
      output["ai_optimizer_enabled"] = ai_optimizer::is_enabled();
      output["mangohud_configured"] = proc::proc.current_app_has_mangohud();

      auto &controls = output["controls"];
      controls["host_tuning_allowed"] = host_tuning_allowed;
      controls["quit_allowed"] = quit_allowed;
      controls["shutdown_in_progress"] = shutdown_requested;
      controls["client_commands_enabled"] = proc::proc.allow_client_commands;
      controls["device_commands_enabled"] = named_cert_p->allow_client_commands;

      auto &tuning = output["tuning"];
      tuning["adaptive_bitrate_enabled"] = adaptive_bitrate::is_enabled();
      tuning["adaptive_target_bitrate_kbps"] = stats.adaptive_target_bitrate_kbps;
      tuning["ai_optimizer_enabled"] = ai_optimizer::is_enabled();
      tuning["mangohud_configured"] = proc::proc.current_app_has_mangohud();

      auto &display_mode = output["display_mode"];
      const std::string display_mode_selection =
        stats.runtime_effective_headless ? "headless" :
        proc::proc.virtual_display ? "virtual_display" :
        "host_display";
      display_mode["virtual_display"] = proc::proc.virtual_display;
      display_mode["requested_headless"] = stats.runtime_requested_headless;
      display_mode["effective_headless"] = stats.runtime_effective_headless;
      display_mode["gpu_native_override_active"] = stats.runtime_gpu_native_override_active;
      display_mode["selection"] = display_mode_selection;
      display_mode["explicit_choice"] = proc::proc.session_display_mode_is_explicit();
      display_mode["requested"] =
        session_token.empty() ? "" :
        proc::proc.session_display_mode_is_explicit() ?
          (proc::proc.virtual_display ? "virtual_display" : "headless") :
          "auto";
      display_mode["label"] =
        stats.runtime_effective_headless ? "Headless" :
        proc::proc.virtual_display ? "Virtual Display" :
        "Host Display";

      // Capture info
      auto &capture_info = output["capture"];
      capture_info["backend"] = stats.runtime_backend.empty() ? "screencopy" : stats.runtime_backend;
      capture_info["resolution"] = std::to_string(stats.width) + "x" + std::to_string(stats.height);
      capture_info["transport"] = platf::from_frame_transport(stats.capture_transport);
      capture_info["residency"] = platf::from_frame_residency(stats.capture_residency);
      capture_info["format"] = platf::from_frame_format(stats.capture_format);

      // Encoder info
      auto &encoder = output["encoder"];
      encoder["codec"] = stats.codec;
      encoder["bitrate_kbps"] = stats.bitrate_kbps;
      encoder["fps"] = stats.fps;
      encoder["requested_client_fps"] = stats.requested_client_fps;
      encoder["session_target_fps"] = stats.session_target_fps;
      encoder["encode_target_fps"] = stats.encode_target_fps;
      encoder["target_device"] = stats.encode_target_device;
      encoder["target_residency"] = platf::from_frame_residency(stats.encode_target_residency);
      encoder["target_format"] = platf::from_frame_format(stats.encode_target_format);
      encoder["pacing_policy"] = stats.pacing_policy;
      encoder["optimization_source"] = stats.optimization_source;
      encoder["optimization_confidence"] = stats.optimization_confidence;
      encoder["optimization_cache_status"] = stats.optimization_cache_status;
      encoder["optimization_reasoning"] = stats.optimization_reasoning;
      encoder["optimization_normalization_reason"] = stats.optimization_normalization_reason;
      encoder["recommendation_version"] = stats.recommendation_version;

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    // Game library — returns games with metadata, covers, categories
    auto polarisGames = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }
      const auto advertised_codec_support = advertised_codec_support_for_http(true);

      // Parse query params
      auto query = request->parse_query_string();
      std::string search_query;
      std::string source_filter;
      int limit = 50, offset = 0;
      for (auto &[key, val] : query) {
        if (key == "search") search_query = val;
        else if (key == "source") source_filter = val;
        else if (key == "limit") try { limit = std::stoi(val); } catch (...) {}
        else if (key == "offset") try { offset = std::stoi(val); } catch (...) {}
      }

      auto apps = proc::proc.get_apps();
      nlohmann::json games = nlohmann::json::array();

      int idx = 0;
      for (auto &app : apps) {
        // Skip non-game entries (Desktop, Lutris launcher)
        if (app.name == "Desktop") continue;

        // Search filter
        if (!search_query.empty()) {
          std::string name_lower = app.name;
          std::string query_lower = search_query;
          std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
          std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
          if (name_lower.find(query_lower) == std::string::npos) continue;
        }

        // Source filter
        if (!source_filter.empty()) {
          bool is_steam = !app.steam_appid.empty();
          if (source_filter == "steam" && !is_steam) continue;
          if (source_filter == "other" && is_steam) continue;
        }

        // Pagination
        if (idx < offset) { idx++; continue; }
        if ((int)games.size() >= limit) break;

        nlohmann::json game;
        game["id"] = app.uuid;
        game["app_id"] = app.id;
        game["name"] = app.name;
        game["source"] = app.steam_appid.empty() ? "other" : "steam";
        game["steam_appid"] = app.steam_appid;
        game["category"] = app.game_category;
        game["source"] = app.source;
        game["installed"] = true;
        game["hdr_supported"] = advertised_codec_support.hevc_mode == 3;
        game["cover_url"] = "/polaris/v1/games/" + app.uuid + "/cover";
        game["last_launched"] = app.last_launched;
        game["launch_mode"] = launch_mode_contract_for_app(app);

        nlohmann::json genre_arr = nlohmann::json::array();
        for (const auto &g : app.genres) genre_arr.push_back(g);
        game["genres"] = genre_arr;
        game["mangohud"] = app.env_vars.count("MANGOHUD") > 0 && app.env_vars.at("MANGOHUD") == "1";

        games.push_back(game);
        idx++;
      }

      nlohmann::json output;
      output["games"] = games;
      output["total"] = idx;

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    // Game cover art
    auto polarisGameCover = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      // Extract UUID from path: /polaris/v1/games/{uuid}/cover
      auto path = request->path;
      auto start = path.find("/games/") + 7;
      auto end = path.find("/cover");
      if (start == std::string::npos || end == std::string::npos) {
        response->write(SimpleWeb::StatusCode::client_error_not_found);
        return;
      }
      std::string uuid = path.substr(start, end - start);

      // Find the app and its image
      auto apps = proc::proc.get_apps();
      for (auto &app : apps) {
        if (app.uuid == uuid) {
          std::string image_path = proc::validate_app_image_path(app.image_path);

          // Check polaris covers directory first
          if (!app.steam_appid.empty()) {
            std::string cover_path = config::sunshine.config_file.substr(
              0, config::sunshine.config_file.rfind('/')) + "/covers/steam_" + app.steam_appid + ".png";
            if (access(cover_path.c_str(), F_OK) == 0) {
              image_path = cover_path;
            }
          }

          // Try reading the image file
          try {
            auto data = file_handler::read_file(image_path.c_str());
            SimpleWeb::CaseInsensitiveMultimap headers;
            std::string content_type = "image/png";
            auto ext = fs::path(image_path).extension().string();
            boost::algorithm::to_lower(ext);
            if (ext == ".jpg" || ext == ".jpeg") {
              content_type = "image/jpeg";
            } else if (ext == ".webp") {
              content_type = "image/webp";
            }
            headers.emplace("Content-Type", content_type);
            headers.emplace("Cache-Control", "max-age=86400");
            response->write(data, headers);
            return;
          } catch (...) {}
          break;
        }
      }

      response->write(SimpleWeb::StatusCode::client_error_not_found);
    };

    // Game launch via Polaris API
    auto polarisLaunchGame = [&host_audio](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);

        std::string game_id = body.value("game_id", "");
        if (game_id.empty()) {
          nlohmann::json err;
          err["error"] = "game_id required";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        // Smart Launch: read client display info for resolution matching
        int client_width = body.value("client_width", 0);
        int client_height = body.value("client_height", 0);
        int client_fps = body.value("client_fps", 0);

        if (client_width > 0 && client_height > 0) {
          BOOST_LOG(info) << "Smart Launch: client display " << client_width << "x" << client_height
                          << " @ " << (client_fps > 0 ? client_fps : 60) << " fps";

#ifdef __linux__
          // Pre-configure labwc resolution to match client device
          if (config::video.linux_display.use_cage_compositor && cage_display_router::is_running()) {
            BOOST_LOG(info) << "Smart Launch: adjusting labwc resolution to " << client_width << "x" << client_height;
            // Note: labwc will be restarted with the correct resolution when the game launches
            // via the cage_display_router::start() call in process.cpp.
            // Store the preferred resolution for the session.
          }
#endif
        }

        // Find the app by UUID
        auto apps = proc::proc.get_apps();
        int app_id = -1;
        std::string app_name;
        for (size_t i = 0; i < apps.size(); i++) {
          if (apps[i].uuid == game_id) {
            app_id = i + 1;  // 1-indexed
            app_name = apps[i].name;
            break;
          }
        }

        if (app_id < 0) {
          nlohmann::json err;
          err["error"] = "Game not found";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_not_found, err.dump(), headers);
          return;
        }

        // Update last_launched timestamp in apps.json
        try {
          std::string content = file_handler::read_file(config::stream.file_apps.c_str());
          auto file_tree = nlohmann::json::parse(content);
          if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
            for (auto &app_node : file_tree["apps"]) {
              if (app_node.value("uuid", "") == game_id) {
                app_node["last-launched"] = std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
                break;
              }
            }
            file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
          }
        } catch (...) {
          BOOST_LOG(warning) << "Failed to update last-launched timestamp";
        }

        nlohmann::json output;
        output["status"] = "launching";
        output["game"] = app_name;
        output["game_id"] = game_id;
        if (client_width > 0 && client_height > 0) {
          output["smart_launch"] = {
            {"client_width", client_width},
            {"client_height", client_height},
            {"client_fps", client_fps > 0 ? client_fps : 60}
          };
        }

        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);

      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    // Toggle MangoHud for a game (sets/clears MANGOHUD=1 in env)
    auto polarisToggleMangoHud = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);

        std::string game_id = body.value("game_id", "");
        bool enabled = body.value("mangohud", false);

        if (game_id.empty()) {
          nlohmann::json err;
          err["error"] = "game_id required";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        // Update apps.json
        std::string content = file_handler::read_file(config::stream.file_apps.c_str());
        auto file_tree = nlohmann::json::parse(content);
        bool found = false;
        if (file_tree.contains("apps") && file_tree["apps"].is_array()) {
          for (auto &app_node : file_tree["apps"]) {
            if (app_node.value("uuid", "") == game_id) {
              if (!app_node.contains("env") || !app_node["env"].is_object()) {
                app_node["env"] = nlohmann::json::object();
              }
              if (enabled) {
                app_node["env"]["MANGOHUD"] = "1";
              } else {
                app_node["env"].erase("MANGOHUD");
                if (app_node["env"].empty()) app_node.erase("env");
              }
              found = true;
              break;
            }
          }
          if (found) {
            file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
            proc::proc.set_app_mangohud_configured(game_id, enabled);
            // Do NOT call proc::refresh() here — it would terminate the running stream.
            // The env change takes effect on the next game launch.
          }
        }

        nlohmann::json output;
        output["status"] = found;
        output["mangohud"] = enabled;
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    // Real-time bitrate adjustment from client
    auto polarisSetBitrate = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }
      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);
        int bitrate_kbps = body.value("bitrate_kbps", 0);
        if (bitrate_kbps < 1000 || bitrate_kbps > 300000) {
          nlohmann::json err;
          err["error"] = "bitrate_kbps must be between 1000 and 300000";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }
        adaptive_bitrate::set_base_bitrate(bitrate_kbps);
        BOOST_LOG(info) << "Client requested bitrate change: " << bitrate_kbps << " kbps";

        nlohmann::json output;
        output["status"] = true;
        output["bitrate_kbps"] = bitrate_kbps;
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    auto polarisSetAdaptiveBitrate = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);
        if (!body.contains("enabled") || !body["enabled"].is_boolean()) {
          nlohmann::json err;
          err["error"] = "enabled must be a boolean";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        const bool enabled = body["enabled"].get<bool>();
        adaptive_bitrate::set_enabled(enabled);
        BOOST_LOG(info) << "Adaptive bitrate toggled via Polaris API: " << (enabled ? "enabled" : "disabled");

        nlohmann::json output;
        output["status"] = true;
        output["adaptive_bitrate_enabled"] = adaptive_bitrate::is_enabled();
        output["adaptive_target_bitrate_kbps"] = stream_stats::get_current().adaptive_target_bitrate_kbps;
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    auto polarisSetAiOptimizer = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);
        if (!body.contains("enabled") || !body["enabled"].is_boolean()) {
          nlohmann::json err;
          err["error"] = "enabled must be a boolean";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        const bool enabled = body["enabled"].get<bool>();
        ai_optimizer::set_enabled(enabled);

        nlohmann::json output;
        output["status"] = true;
        output["ai_optimizer_enabled"] = ai_optimizer::is_enabled();
        output["effective"] = ai_optimizer::is_enabled();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    auto polarisSetCursorVisibility = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);
        if (!body.contains("visible") || !body["visible"].is_boolean()) {
          nlohmann::json err;
          err["error"] = "visible must be a boolean";
          SimpleWeb::CaseInsensitiveMultimap headers;
          headers.emplace("Content-Type", "application/json");
          response->write(SimpleWeb::StatusCode::client_error_bad_request, err.dump(), headers);
          return;
        }

        const bool visible = body["visible"].get<bool>();
        cursor::set_visible(visible);
        BOOST_LOG(info) << "Client requested cursor visibility change: " << (visible ? "visible" : "hidden");

        nlohmann::json output;
        output["status"] = true;
        output["visible"] = visible;
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    // Client session report — Nova sends quality summary at session end
    auto polarisSessionReport = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }
      try {
        std::string body_str(std::istreambuf_iterator<char>(request->content), {});
        auto body = nlohmann::json::parse(body_str);

        std::string device = body.value("device", "");
        std::string game = body.value("game", "");
        std::string unique_id = body.value("unique_id", "");
        double avg_fps = body.value("avg_fps", 0.0);
        double avg_latency = body.value("avg_latency_ms", 0.0);
        int avg_bitrate = body.value("avg_bitrate_kbps", 0);
        double packet_loss = body.value("packet_loss_pct", 0.0);
        std::string codec = body.value("codec", "");
        double target_fps = body.value("target_fps", 0.0);
        int duration_s = body.value("duration_s", 0);
        std::string end_reason = body.value("end_reason", "disconnect");
        std::string optimization_source = body.value("optimization_source", "");
        std::string optimization_confidence = body.value("optimization_confidence", "");
        int recommendation_version = body.value("recommendation_version", 0);

        if (!device.empty() && !game.empty() && ai_optimizer::is_enabled()) {
          const auto canonical_device = device_db::canonicalize_name(device);
          const auto owner_device_name = proc::proc.get_session_owner_device_name();
          const auto canonical_owner_device = device_db::canonicalize_name(owner_device_name);
          const bool device_matches_owner =
            !canonical_device.empty() &&
            !canonical_owner_device.empty() &&
            canonical_device == canonical_owner_device;
          const bool app_matches_active_session = proc::proc.get_last_run_app_name() == game;
          const bool matches_active_owner =
            app_matches_active_session &&
            ((!unique_id.empty() && proc::proc.is_session_owner(unique_id)) || device_matches_owner);
          if (matches_active_owner) {
            if (!owner_device_name.empty()) {
              device = owner_device_name;
            }
            proc::proc.mark_client_session_report_recorded(unique_id);
            BOOST_LOG(info) << "Client session report matched active session owner; host-side duplicate recording disabled for ["
                            << device_db::canonicalize_name(device) << ":" << game << "]";
          } else {
            device = canonical_device;
          }

          ai_optimizer::session_history_t session;
          session.avg_fps = avg_fps;
          session.avg_latency_ms = avg_latency;
          session.avg_bitrate_kbps = avg_bitrate;
          session.packet_loss_pct = packet_loss;
          session.last_fps = avg_fps;
          session.last_target_fps = target_fps > 0.0 ? target_fps : avg_fps;
          session.last_latency_ms = avg_latency;
          session.last_bitrate_kbps = avg_bitrate;
          session.last_packet_loss_pct = packet_loss;
          session.last_codec = codec;
          session.session_count = 1;

          const double effective_target_fps = session.last_target_fps > 0.0 ? session.last_target_fps : avg_fps;
          const double fps_ratio =
            (effective_target_fps > 0.0 && avg_fps > 0.0) ? std::clamp(avg_fps / effective_target_fps, 0.0, 1.5) : 0.0;
          if (avg_fps <= 0.0)
            session.last_quality_grade = "F";
          else if (fps_ratio >= 0.95 && packet_loss < 0.5 && avg_latency < 20.0)
            session.last_quality_grade = "A";
          else if (fps_ratio >= 0.85 && packet_loss < 2.0 && avg_latency < 40.0)
            session.last_quality_grade = "B";
          else if (fps_ratio >= 0.70 && packet_loss < 5.0)
            session.last_quality_grade = "C";
          else if (fps_ratio >= 0.50)
            session.last_quality_grade = "D";
          else
            session.last_quality_grade = "F";
          session.quality_grade = session.last_quality_grade;
          session.codec = session.last_codec;

          session.last_optimization_source = optimization_source;
          session.last_optimization_confidence = optimization_confidence;
          session.last_recommendation_version = recommendation_version;

          ai_optimizer::record_session(device, game, session);
          BOOST_LOG(info) << "Client session report: " << device << " + " << game
                          << " → grade " << session.quality_grade
                          << " (fps=" << avg_fps << ", lat=" << avg_latency << "ms, dur=" << duration_s << "s)";
        }

        nlohmann::json output;
        output["status"] = true;
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(output.dump(), headers);
      } catch (std::exception &e) {
        nlohmann::json err;
        err["error"] = e.what();
        SimpleWeb::CaseInsensitiveMultimap headers;
        headers.emplace("Content-Type", "application/json");
        response->write(SimpleWeb::StatusCode::server_error_internal_server_error, err.dump(), headers);
      }
    };

    // AI optimization query — Nova asks for recommended settings before launching
    auto polarisOptimize = [](resp_https_t response, req_https_t request) {
      print_req<PolarisHTTPS>(request);
      if (!get_verified_cert(request)) {
        response->write(SimpleWeb::StatusCode::client_error_unauthorized);
        return;
      }

      auto args = request->parse_query_string();
      std::string device = args.count("device") ? args.find("device")->second : "";
      std::string game = args.count("game") ? args.find("game")->second : "";

      nlohmann::json output;
      std::optional<std::string> suggested_codec;
      std::optional<int> target_bitrate_kbps;
      bool hdr_requested = false;
      const auto session_history = ai_optimizer::get_session_history(device, game);

      // Try AI cache first, then device_db
      auto ai_opt = ai_optimizer::get_cached(device, game);
      if (ai_opt) {
        append_optimization_json(output, *ai_opt);
        if (ai_opt->target_bitrate_kbps) {
          target_bitrate_kbps = *ai_opt->target_bitrate_kbps;
        }
        suggested_codec = ai_opt->preferred_codec;
        hdr_requested = ai_opt->hdr.value_or(false);
      } else {
        if (device_db::get_device(device)) {
          auto opt = device_db::get_optimization(device, game);
          if (session_history && session_history->last_invalidated_at > 0) {
            opt.cache_status = "invalidated";
          }
          append_optimization_json(output, opt);
          target_bitrate_kbps = opt.target_bitrate_kbps;
          suggested_codec = opt.preferred_codec;
          hdr_requested = opt.hdr.value_or(false);
        } else {
          auto opt = device_db::get_optimization(device, game);
          if (session_history && session_history->last_invalidated_at > 0) {
            opt.cache_status = "invalidated";
          }
          append_optimization_json(output, opt);
          target_bitrate_kbps = opt.target_bitrate_kbps;
          suggested_codec = opt.preferred_codec;
          hdr_requested = opt.hdr.value_or(false);
        }
      }

      suggested_codec = device_db::normalize_preferred_codec(
        device,
        game,
        suggested_codec,
        target_bitrate_kbps,
        hdr_requested
      );
      if (suggested_codec) {
        output["preferred_codec"] = *suggested_codec;
      }
      if (session_history) {
        output["last_quality_grade"] = session_history->quality_grade;
        output["poor_outcome_count"] = session_history->poor_outcome_count;
        output["consecutive_poor_outcomes"] = session_history->consecutive_poor_outcomes;
        output["last_invalidated_at"] = session_history->last_invalidated_at;
      }

      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(output.dump(), headers);
    };

    https_server.resource["^/polaris/v1/session/report$"]["POST"] = polarisSessionReport;
    https_server.resource["^/polaris/v1/optimize$"]["GET"] = polarisOptimize;
    https_server.resource["^/polaris/v1/capabilities$"]["GET"] = polarisCapabilities;
    https_server.resource["^/polaris/v1/session/status$"]["GET"] = polarisSessionStatus;
    https_server.resource["^/polaris/v1/session/bitrate$"]["POST"] = polarisSetBitrate;
    https_server.resource["^/polaris/v1/session/adaptive-bitrate$"]["POST"] = polarisSetAdaptiveBitrate;
    https_server.resource["^/polaris/v1/session/ai-optimizer$"]["POST"] = polarisSetAiOptimizer;
    https_server.resource["^/polaris/v1/session/cursor$"]["POST"] = polarisSetCursorVisibility;
    https_server.resource["^/polaris/v1/games$"]["GET"] = polarisGames;
    https_server.resource["^/polaris/v1/games/.+/cover$"]["GET"] = polarisGameCover;
    https_server.resource["^/polaris/v1/games/.+/mangohud$"]["POST"] = polarisToggleMangoHud;
    https_server.resource["^/polaris/v1/session/launch$"]["POST"] = polarisLaunchGame;

    https_server.config.reuse_address = true;
    https_server.config.address = net::af_to_any_address_string(address_family);
    https_server.config.port = port_https;

    http_server.default_resource["GET"] = not_found<SimpleWeb::HTTP>;
    http_server.resource["^/serverinfo$"]["GET"] = serverinfo<SimpleWeb::HTTP>;
    http_server.resource["^/pair$"]["GET"] = pair<SimpleWeb::HTTP>;

    http_server.config.reuse_address = true;
    http_server.config.address = net::af_to_any_address_string(address_family);
    http_server.config.port = port_http;

    auto accept_and_run = [&](auto *http_server) {
      try {
        http_server->start();
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling http_server->stop() from a different thread
        if (shutdown_event->peek()) {
          return;
        }

        BOOST_LOG(fatal) << "Couldn't start http server on ports ["sv << port_https << ", "sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread ssl {accept_and_run, &https_server};
    std::thread tcp {accept_and_run, &http_server};

    // Wait for any event
    shutdown_event->view();

    map_id_sess.clear();

    https_server.stop();
    http_server.stop();

    ssl.join();
    tcp.join();
  }

  std::string request_otp(const std::string& passphrase, const std::string& deviceName) {
    if (passphrase.size() < 4) {
      return "";
    }

    one_time_pin = crypto::rand_alphabet(4, "0123456789"sv);
    otp_passphrase = passphrase;
    otp_device_name = deviceName;
    otp_creation_time = std::chrono::steady_clock::now();

    return one_time_pin;
  }

  void
  erase_all_clients() {
    client_t client;
    client_root = client;
    cert_chain.clear();
    save_state();
    load_state();
  }

  void stop_session(stream::session_t& session, bool graceful) {
    if (graceful) {
      stream::session::graceful_stop(session);
    } else {
      stream::session::stop(session);
    }
  }

  bool find_and_stop_session(const std::string& uuid, bool graceful) {
    auto session = rtsp_stream::find_session(uuid);
    if (session) {
      stop_session(*session, graceful);
      return true;
    }
    return false;
  }

  void update_session_info(stream::session_t& session, const std::string& name, const crypto::PERM newPerm) {
    stream::session::update_device_info(session, name, newPerm);
  }

  bool find_and_udpate_session_info(const std::string& uuid, const std::string& name, const crypto::PERM newPerm) {
    auto session = rtsp_stream::find_session(uuid);
    if (session) {
      update_session_info(*session, name, newPerm);
      return true;
    }
    return false;
  }

  bool update_device_info(
    const std::string& uuid,
    const std::string& name,
    const std::string& display_mode,
    const cmd_list_t& do_cmds,
    const cmd_list_t& undo_cmds,
    const crypto::PERM newPerm,
    const bool enable_legacy_ordering,
    const bool allow_client_commands,
    const bool always_use_virtual_display
  ) {
    find_and_udpate_session_info(uuid, name, newPerm);

    client_t &client = client_root;
    auto it = client.named_devices.begin();
    for (; it != client.named_devices.end(); ++it) {
      auto named_cert_p = *it;
      if (named_cert_p->uuid == uuid) {
        named_cert_p->name = name;
        named_cert_p->display_mode = display_mode;
        named_cert_p->perm = newPerm;
        named_cert_p->do_cmds = do_cmds;
        named_cert_p->undo_cmds = undo_cmds;
        named_cert_p->enable_legacy_ordering = enable_legacy_ordering;
        named_cert_p->allow_client_commands = allow_client_commands;
        named_cert_p->always_use_virtual_display = always_use_virtual_display;
        save_state();
        return true;
      }
    }

    return false;
  }

  bool unpair_client(const std::string_view uuid) {
    bool removed = false;
    client_t &client = client_root;
    for (auto it = client.named_devices.begin(); it != client.named_devices.end();) {
      if ((*it)->uuid == uuid) {
        it = client.named_devices.erase(it);
        removed = true;
      } else {
        ++it;
      }
    }

    save_state();
    load_state();

    if (removed) {
      auto session = rtsp_stream::find_session(uuid);
      if (session) {
        stop_session(*session, true);
      }

      if (client.named_devices.empty()) {
        proc::proc.terminate();
      }
    }

    return removed;
  }
}  // namespace nvhttp
