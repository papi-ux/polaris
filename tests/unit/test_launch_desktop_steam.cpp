/**
 * @file tests/unit/test_launch_desktop_steam.cpp
 * @brief What a GameStream /launch does to desktop Steam on the host.
 *
 * Desktop Steam here is a stand-in that counts the requests to close it, so nothing on this
 * machine is closed. The route tests send each launch to the production /launch route over TLS
 * with a client certificate, as Moonlight does. A launch the route admits goes on to start the
 * app, so those launches run through the route's desktop Steam step instead.
 */
#include "../tests_common.h"

#include <src/config.h>
#include <src/crypto.h>
#include <src/nvhttp.h>
#include <src/process.h>
#include <src/utility.h>
#include <src/uuid.h>

#include <Simple-Web-Server/client_https.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(__linux__)
  #include <src/platform/linux/game_mode_host.h>

namespace {
  using namespace std::literals;

  constexpr auto steam_game_id = "41";
  constexpr auto desktop_id = "42";

  /// A Steam game, as a Steam library import saves it.
  proc::ctx_t steam_game() {
    proc::ctx_t app;
    app.id = steam_game_id;
    app.uuid = "A6C1E4D2-3B5F-4E8A-9C7D-1F2E3D4C5B6A";
    app.name = "Portal 2";
    app.steam_appid = "620";
    return app;
  }

  /// An app that never touches Steam.
  proc::ctx_t desktop() {
    proc::ctx_t app;
    app.id = desktop_id;
    app.uuid = "0D9C8B7A-6F5E-4D3C-8B2A-19F8E7D6C5B4";
    app.name = "Desktop";
    return app;
  }

  /// A launch of `appid` with what a stock Moonlight sends, and `extra` after it.
  std::string moonlight_launch(std::string_view appid, std::string_view extra = {}) {
    return "/launch?appid="s + std::string {appid} + "&mode=1920x1080x60&rikey=" + std::string(32, 'a') +
           "&rikeyid=1&localAudioPlayMode=0&corever=1" + std::string {extra};
  }

  struct launch_reply_t {
    int status = 0;
    std::string error_code;
    /// What the desktop Steam step decided, or empty when the launch never reached it.
    std::string recommended_action;
  };

  launch_reply_t read_reply(const boost::property_tree::ptree &tree) {
    return {
      tree.get<int>("root.<xmlattr>.status_code", 0),
      tree.get<std::string>("root.error_code", ""),
      tree.get<std::string>("root.launchPolicy.recommendedAction", ""),
    };
  }

  void write_file(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream output {path, std::ios::binary | std::ios::trunc};
    output << contents;
  }

  struct credentials_t {
    std::filesystem::path cert;
    std::filesystem::path key;
    std::string cert_pem;
  };

  class LaunchDesktopSteamTest: public testing::Test {
  protected:
    static void SetUpTestSuite() {
      directory = std::filesystem::temp_directory_path() /
                  ("polaris-launch-desktop-steam-" + uuid_util::uuid_t::generate().string());
      std::filesystem::create_directories(directory);
      host = write_credentials("host", crypto::gen_creds("localhost", 2048));
      moonlight = write_credentials("moonlight", crypto::gen_creds("Moonlight", 2048));
      stranger = write_credentials("stranger", crypto::gen_creds("Stranger", 2048));
    }

    static void TearDownTestSuite() {
      std::error_code ec;
      std::filesystem::remove_all(directory, ec);
    }

    void SetUp() override {
      previous_fresh_state = config::sunshine.flags.test(config::flag::FRESH_STATE);
      previous_state_path = config::nvhttp.file_state;
      state_path = directory / ("state-" + uuid_util::uuid_t::generate().string() + ".json");
      config::nvhttp.file_state = state_path.string();
      config::sunshine.flags.set(config::flag::FRESH_STATE);
      nvhttp::reset_pairing_state_for_tests();

      // Gamescope Stream is a private stream, which cannot start beside desktop Steam, and it starts
      // nothing until a launch is admitted.
      previous_linux_display = config::video.linux_display;
      config::video.linux_display.headless_mode = false;
      config::video.linux_display.use_cage_compositor = false;
      config::video.linux_display.stream_mode = "gamescope_stream";
      config::video.linux_display.private_runtime.clear();
      previous_lan_encryption = config::stream.lan_encryption_mode;
      previous_wan_encryption = config::stream.wan_encryption_mode;
      config::stream.lan_encryption_mode = config::ENCRYPTION_MODE_OPPORTUNISTIC;
      config::stream.wan_encryption_mode = config::ENCRYPTION_MODE_OPPORTUNISTIC;
      previous_input_only = config::input.enable_input_only_mode;
      config::input.enable_input_only_mode = false;

      previous_apps = proc::proc.get_apps();
      proc::proc.reload_configuration(proc::proc_t {boost::process::v1::environment {}, {steam_game(), desktop()}});

      platf::game_mode_host::set_session_live_for_tests(false);
      proc::set_desktop_steam_double_for_tests(&steam);

      // Without the device's switch, a private launch of the Steam game is refused while desktop
      // Steam runs. Every test below starts from that.
      boost::property_tree::ptree tree;
      ASSERT_FALSE(nvhttp::admit_desktop_launch_policy_for_tests(tree, {}, steam_game(), false, false));
      ASSERT_EQ(read_reply(tree).error_code, "desktop_active_private_stream_refused");
      ASSERT_EQ(steam.close_requests.load(), 0);
    }

    void TearDown() override {
      route.reset();
      proc::set_desktop_steam_double_for_tests(nullptr);
      platf::game_mode_host::set_session_live_for_tests(std::nullopt);
      proc::proc.reload_configuration(proc::proc_t {boost::process::v1::environment {}, std::move(previous_apps)});
      config::input.enable_input_only_mode = previous_input_only;
      config::stream.lan_encryption_mode = previous_lan_encryption;
      config::stream.wan_encryption_mode = previous_wan_encryption;
      config::video.linux_display = previous_linux_display;

      nvhttp::reset_pairing_state_for_tests();
      config::sunshine.flags.set(config::flag::FRESH_STATE, previous_fresh_state);
      config::nvhttp.file_state = previous_state_path;
      std::error_code ec;
      std::filesystem::remove(state_path, ec);
      std::filesystem::remove(state_path.string() + ".lock", ec);
    }

    /// Pairs Moonlight's certificate with `perm`, and its switch as given or left at the default.
    crypto::p_named_cert_t pair_moonlight(
      std::optional<bool> close_desktop_steam,
      crypto::PERM perm = crypto::PERM::_game_control
    ) {
      auto device = std::make_shared<crypto::named_cert_t>();
      device->name = "Moonlight";
      device->uuid = "5F1D2C3B-4A59-4687-B6C5-D4E3F2A1B0C9";
      device->cert = moonlight.cert_pem;
      device->enable_legacy_ordering = true;
      device->allow_client_commands = false;
      device->always_use_virtual_display = false;
      if (close_desktop_steam) {
        device->close_desktop_steam = *close_desktop_steam;
      }
      EXPECT_TRUE(nvhttp::add_authorized_client_for_tests(device, perm));
      return device;
    }

    /// Sends `path` to the /launch route from the client holding `client`.
    launch_reply_t launch(const credentials_t &client, const std::string &path) {
      if (!route) {
        route = std::make_unique<nvhttp::launch_route_for_tests_t>(host.cert.string(), host.key.string());
      }
      EXPECT_NE(route->port(), 0);
      SimpleWeb::Client<SimpleWeb::HTTPS> https {
        "127.0.0.1:" + std::to_string(route->port()),
        false,
        client.cert.string(),
        client.key.string()
      };
      https.config.timeout = 10;
      const auto response = https.request("GET", path);
      boost::property_tree::ptree tree;
      std::istringstream body {response->content.string()};
      boost::property_tree::read_xml(body, tree);
      return read_reply(tree);
    }

    /// Runs the route's desktop Steam step for a launch of `app` with `args`.
    launch_reply_t admit(const nvhttp::args_t &args, const proc::ctx_t &app, bool device_closes_desktop_steam, bool &admitted) {
      boost::property_tree::ptree tree;
      admitted = nvhttp::admit_desktop_launch_policy_for_tests(tree, args, app, false, device_closes_desktop_steam);
      return read_reply(tree);
    }

    static inline std::filesystem::path directory;
    static inline credentials_t host;
    static inline credentials_t moonlight;
    static inline credentials_t stranger;

    proc::desktop_steam_double_t steam;
    std::unique_ptr<nvhttp::launch_route_for_tests_t> route;

  private:
    static credentials_t write_credentials(const std::string &name, const crypto::creds_t &creds) {
      credentials_t written {directory / (name + ".crt"), directory / (name + ".key"), creds.x509};
      write_file(written.cert, creds.x509);
      write_file(written.key, creds.pkey);
      return written;
    }

    bool previous_fresh_state = false;
    std::string previous_state_path;
    std::filesystem::path state_path;
    config::video_t::linux_display_t previous_linux_display;
    int previous_lan_encryption = 0;
    int previous_wan_encryption = 0;
    bool previous_input_only = false;
    std::vector<proc::ctx_t> previous_apps;
  };
}  // namespace

TEST_F(LaunchDesktopSteamTest, DeviceLeftAtTheDefaultIsRefusedAndSteamIsLeftAlone) {
  pair_moonlight(std::nullopt);

  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));

  EXPECT_EQ(reply.status, 409);
  EXPECT_EQ(reply.error_code, "desktop_active_private_stream_refused");
  EXPECT_EQ(steam.close_requests.load(), 0);
  EXPECT_TRUE(steam.running.load());
}

TEST_F(LaunchDesktopSteamTest, DeviceWithTheSwitchOnAsksDesktopSteamToClose) {
  pair_moonlight(true);

  // The stand-in stays open, so the launch stops at the close instead of starting the game.
  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));

  EXPECT_EQ(reply.status, 409);
  EXPECT_EQ(reply.error_code, "desktop_steam_shutdown_failed");
  EXPECT_EQ(steam.close_requests.load(), 1);
}

TEST_F(LaunchDesktopSteamTest, UnpairedCertificateIsRefusedWithoutTouchingSteam) {
  pair_moonlight(true);

  const auto reply = launch(stranger, moonlight_launch(steam_game_id));

  EXPECT_EQ(reply.status, 401);
  EXPECT_TRUE(reply.recommended_action.empty());
  EXPECT_EQ(steam.close_requests.load(), 0);
}

TEST_F(LaunchDesktopSteamTest, ViewOnlyDeviceIsRefusedWithoutTouchingSteam) {
  pair_moonlight(true, crypto::PERM::_default);

  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));

  EXPECT_EQ(reply.status, 403);
  EXPECT_TRUE(reply.recommended_action.empty());
  EXPECT_EQ(steam.close_requests.load(), 0);
}

TEST_F(LaunchDesktopSteamTest, ViewOnlyDeviceJoiningTheRunningGameNeverReachesTheSteamStep) {
  pair_moonlight(true, crypto::PERM::_default);
  proc::proc.set_running_app_for_tests(steam_game());
  auto stop_game = util::fail_guard([]() {
    proc::proc.clear_running_app_for_tests();
  });

  // Joining the running game takes the resume path, which has no desktop Steam step. The stand-in
  // game has no launch session, so that path stops where it checks the launch against it.
  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));

  EXPECT_EQ(reply.status, 503);
  EXPECT_TRUE(reply.recommended_action.empty());
  EXPECT_EQ(steam.close_requests.load(), 0);
}

TEST_F(LaunchDesktopSteamTest, LaunchWhileAnotherAppRunsIsRefusedWithoutTouchingSteam) {
  pair_moonlight(true);
  proc::proc.set_running_app_for_tests(desktop());
  auto stop_desktop = util::fail_guard([]() {
    proc::proc.clear_running_app_for_tests();
  });

  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));

  EXPECT_EQ(reply.status, 400);
  EXPECT_TRUE(reply.recommended_action.empty());
  EXPECT_EQ(steam.close_requests.load(), 0);
}

TEST_F(LaunchDesktopSteamTest, LaunchesRefusedBeforeTheSteamStepLeaveSteamAlone) {
  pair_moonlight(true);

  const struct {
    std::string path;
    int status;
  } refused[] {
    {"/launch?appid="s + steam_game_id + "&rikeyid=1&localAudioPlayMode=0&corever=1", 400},
    {"/launch?appid="s + steam_game_id + "&rikey=00112233&rikeyid=1&localAudioPlayMode=0&corever=1", 400},
    {moonlight_launch("999"), 404},
    {moonlight_launch(steam_game_id, "&watch=1"), 409},
  };
  for (const auto &[path, status] : refused) {
    const auto reply = launch(moonlight, path);
    EXPECT_EQ(reply.status, status) << path;
    EXPECT_TRUE(reply.recommended_action.empty()) << path;
  }
  EXPECT_EQ(steam.close_requests.load(), 0);
}

TEST_F(LaunchDesktopSteamTest, ClientThatCannotEncryptIsRefusedWithoutTouchingSteam) {
  pair_moonlight(true);
  config::stream.lan_encryption_mode = config::ENCRYPTION_MODE_MANDATORY;

  const auto reply = launch(
    moonlight,
    "/launch?appid="s + steam_game_id + "&rikey=" + std::string(32, 'a') + "&rikeyid=1&localAudioPlayMode=0"
  );

  EXPECT_EQ(reply.status, 403);
  EXPECT_TRUE(reply.recommended_action.empty());
  EXPECT_EQ(steam.close_requests.load(), 0);
}

TEST_F(LaunchDesktopSteamTest, LaunchWhileTheSessionStopsIsRefusedWithoutTouchingSteam) {
  pair_moonlight(true);
  ASSERT_TRUE(proc::proc.begin_session_stop_for_tests());
  auto finish_stop = util::fail_guard([]() {
    proc::proc.finish_session_stop_for_tests(false);
  });

  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));

  EXPECT_EQ(reply.status, 409);
  EXPECT_TRUE(reply.recommended_action.empty());
  EXPECT_EQ(steam.close_requests.load(), 0);
}

TEST_F(LaunchDesktopSteamTest, SwitchOnClosesDesktopSteamAndAdmitsThePrivateLaunch) {
  steam.closes_when_asked = true;

  bool admitted = false;
  const auto reply = admit({}, steam_game(), true, admitted);

  EXPECT_TRUE(admitted);
  EXPECT_EQ(reply.recommended_action, "launch_private_stream");
  EXPECT_EQ(steam.close_requests.load(), 1);
  EXPECT_FALSE(steam.running.load());
}

TEST_F(LaunchDesktopSteamTest, ExplicitMirrorLeavesSteamAlone) {
  steam.closes_when_asked = true;

  for (const auto &[key, value] : std::vector<std::pair<std::string, std::string>> {
         {"mirrorDesktop", "1"},
         {"launchMode", "mirror_desktop"},
       }) {
    nvhttp::args_t args;
    args.emplace(key, value);
    bool admitted = false;
    const auto reply = admit(args, steam_game(), true, admitted);
    EXPECT_TRUE(admitted) << key;
    EXPECT_EQ(reply.recommended_action, "mirror_desktop") << key;
  }
  EXPECT_EQ(steam.close_requests.load(), 0);
  EXPECT_TRUE(steam.running.load());
}

TEST_F(LaunchDesktopSteamTest, GameModeLeavesSteamAlone) {
  steam.closes_when_asked = true;
  platf::game_mode_host::set_session_live_for_tests(true);

  bool admitted = false;
  const auto reply = admit({}, steam_game(), true, admitted);

  EXPECT_TRUE(admitted);
  EXPECT_EQ(reply.recommended_action, "mirror_desktop");
  EXPECT_EQ(steam.close_requests.load(), 0);
  EXPECT_TRUE(steam.running.load());

  // Under Game Mode the running Steam is the session itself, and a direct request to close it is
  // refused too.
  EXPECT_FALSE(proc::request_desktop_steam_shutdown_for_private_stream());
  EXPECT_TRUE(steam.running.load());
}

TEST_F(LaunchDesktopSteamTest, LaunchOfAnAppThatIsNotSteamLeavesSteamAlone) {
  steam.closes_when_asked = true;

  bool admitted = false;
  const auto reply = admit({}, desktop(), true, admitted);

  EXPECT_TRUE(admitted);
  EXPECT_EQ(reply.recommended_action, "launch_private_stream");
  EXPECT_EQ(steam.close_requests.load(), 0);
  EXPECT_TRUE(steam.running.load());
}

TEST_F(LaunchDesktopSteamTest, NothingIsClosedWhenDesktopSteamIsNotRunning) {
  steam.running = false;

  bool admitted = false;
  const auto reply = admit({}, steam_game(), true, admitted);

  EXPECT_TRUE(admitted);
  EXPECT_EQ(reply.recommended_action, "launch_private_stream");
  EXPECT_EQ(steam.close_requests.load(), 0);
}
#endif
