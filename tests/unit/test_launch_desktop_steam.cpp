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

#include <src/browser_stream.h>
#include <src/config.h>
#include <src/confighttp.h>
#include <src/confighttp_validation.h>
#include <src/crypto.h>
#include <src/file_handler.h>
#include <src/nvhttp.h>
#include <src/prelaunch_transition.h>
#include <src/process.h>
#include <src/rtsp.h>
#include <src/tailscale_connection.h>
#include <src/utility.h>
#include <src/uuid.h>

#include <Simple-Web-Server/client_https.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
  #include <src/platform/linux/game_mode_host.h>
  #include <src/platform/linux/stream_display_policy.h>

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
    /// A launch refusal's own code and message (put_launch_refusal), when there is one.
    std::string refusal_code;
    std::string message;
  };

  launch_reply_t read_reply(const boost::property_tree::ptree &tree) {
    return {
      tree.get<int>("root.<xmlattr>.status_code", 0),
      tree.get<std::string>("root.error_code", ""),
      tree.get<std::string>("root.launchPolicy.recommendedAction", ""),
      tree.get<std::string>("root.<xmlattr>.error_code", ""),
      tree.get<std::string>("root.<xmlattr>.status_message", ""),
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
      previous_capture = config::video.capture;
      previous_output_name = config::video.output_name;
      // A Game Mode hold is process state that an earlier test (or suite) may have taken while it
      // pinned Game Mode live: this fixture starts from none, as a freshly loaded config does.
      stream_display_policy::forget_game_mode_hold();
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
      ASSERT_TRUE(stream_display_policy::game_mode_held_selection().empty()) << "the fixture starts with no Game Mode hold";
      ASSERT_EQ(config::video.linux_display.stream_mode, "gamescope_stream");

      // Without the device's switch, a private launch of the Steam game is refused while desktop
      // Steam runs. Every test below starts from that.
      boost::property_tree::ptree tree;
      ASSERT_FALSE(nvhttp::admit_desktop_launch_policy_for_tests(tree, {}, steam_game(), false, false));
      ASSERT_EQ(read_reply(tree).error_code, "desktop_active_private_stream_refused");
      ASSERT_EQ(steam.close_requests.load(), 0);
    }

    void TearDown() override {
      route.reset();
      tailscale_connection::set_default_status_reader_for_tests({});
      proc::set_desktop_steam_double_for_tests(nullptr);
      platf::game_mode_host::set_session_live_for_tests(std::nullopt);
      proc::proc.reload_configuration(proc::proc_t {boost::process::v1::environment {}, std::move(previous_apps)});
      config::input.enable_input_only_mode = previous_input_only;
      config::stream.lan_encryption_mode = previous_lan_encryption;
      config::stream.wan_encryption_mode = previous_wan_encryption;
      // A launch that reconciled while this test pinned Game Mode live took a hold and rewrote the
      // mode and capture in memory. Drop that hold first, the way a config reload does, then put the
      // configured values back, so no hold outlives the test and none restores stale values later.
      stream_display_policy::forget_game_mode_hold();
      config::video.linux_display = previous_linux_display;
      config::video.capture = previous_capture;
      config::video.output_name = previous_output_name;

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

    /// network_reply on its own thread, for a request that waits on a stalled daemon.
    std::future<std::pair<std::string, std::string>> network_reply_async(const credentials_t &client) {
      if (!route) {
        route = std::make_unique<nvhttp::launch_route_for_tests_t>(host.cert.string(), host.key.string());
      }
      return std::async(std::launch::async, [this, &client] { return network_reply(client); });
    }

    std::pair<std::string, std::string> network_reply(const credentials_t &client) {
      if (!route) {
        route = std::make_unique<nvhttp::launch_route_for_tests_t>(host.cert.string(), host.key.string());
      }
      SimpleWeb::Client<SimpleWeb::HTTPS> https {
        "127.0.0.1:" + std::to_string(route->port()), false,
        client.cert.string(), client.key.string()
      };
      https.config.timeout = 5;
      const auto response = https.request("GET",
        "/polaris/v1/network/connection?peer=100.64.0.2&hostname=private.example.ts.net");
      return {response->status_code, response->content.string()};
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
    std::string previous_capture;
    std::string previous_output_name;
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
// --- #204: leaving Game Mode before an opted-in private launch, through the production entries ---
//
// The production session-switch adapter is not implemented in this build, so with no stand-ins an
// opted-in app on a host in Game Mode must be refused before anything else in its launch runs. The
// ordering cases inject a host, an adapter and a clock that only move this process's own Game Mode
// answer (set_session_live_for_tests); nothing on the machine changes.

namespace {
  constexpr auto pinned_id = "43";

  proc::ctx_t opted_in(proc::ctx_t app) {
    app.leave_game_mode_before_launch = true;
    return app;
  }

  /// A Gamescope Stream pin: refused while Game Mode runs, judged on its own merits after it.
  proc::ctx_t pinned_private() {
    proc::ctx_t app;
    app.id = pinned_id;
    app.uuid = "7B3E2F1A-0C9D-4E8B-A7F6-5D4C3B2A1908";
    app.name = "Pinned private";
    app.launch_as = "gamescope_stream";
    app.leave_game_mode_before_launch = true;
    return app;
  }

  prelaunch::session_identity_t stand_in_session(std::string id, prelaunch::mode_e mode, char generation) {
    prelaunch::session_identity_t session;
    session.boot_id = "6c1b5f3e-2a4d-4c8e-9f10-0123456789ab";
    session.logind_id = std::move(id);
    session.logind_started = 5000;
    session.uid = 1000;
    session.leader_pid = 100;
    session.leader_start_ticks = 5000;
    session.target_invocation = std::string(32, generation);
    session.mode = mode;
    return session;
  }

  /// The Game Mode answer this process reads, moved by the stand-in adapter, and session records
  /// that follow it: one Game Mode session, then (unless held back) one new desktop session.
  class session_answer_host_t final: public prelaunch::host_t {
  public:
    bool desktop_appears = true;
    // The #427 cases keep the typed unit independent of the process scan, as production does.
    std::optional<bool> typed_game_mode;
    std::function<void()> on_scan;
    std::atomic<int> scans {0};

    prelaunch::observation_t observe(const prelaunch::session_identity_t *since, const prelaunch::wait_t &, bool) override {
      ++scans;
      if (on_scan) on_scan();
      prelaunch::observation_t out;
      const bool live = typed_game_mode ? *typed_game_mode : platf::game_mode_host::session_live_fresh();
      out.game_mode = live ? prelaunch::steamos::game_mode_e::active : prelaunch::steamos::game_mode_e::inactive;
      if (live) {
        out.session = stand_in_session("1", prelaunch::mode_e::game, 'a');
      } else if (desktop_appears) {
        out.session = stand_in_session("2", prelaunch::mode_e::desktop, 'b');
      } else {
        out.bind = prelaunch::verdict_t::fail("prelaunch_session_identity_unavailable", "no desktop yet");
      }
      out.retired = since && (!out.session || (out.session->logind_id != since->logind_id &&
                                               out.session->target_invocation != since->target_invocation));
      return out;
    }

    bool local_title_running() override {
      return false;
    }

    bool service_independent(std::string &) override {
      return true;
    }
  };

  class session_answer_adapter_t final: public prelaunch::adapter_t {
  public:
    std::vector<prelaunch::direction_e> starts;
    prelaunch::direction_e direction = prelaunch::direction_e::to_desktop;
    std::function<void(prelaunch::direction_e)> on_switch;

    bool implemented() const override {
      return true;
    }

    bool command_present() const override {
      return true;
    }

    prelaunch::verdict_t support(const prelaunch::wait_t &) override {
      return prelaunch::verdict_t::pass();
    }

    bool start(prelaunch::direction_e to) override {
      starts.push_back(to);
      direction = to;
      return true;
    }

    prelaunch::step_e poll() override {
      if (on_switch) on_switch(direction);
      else platf::game_mode_host::set_session_live_for_tests(direction == prelaunch::direction_e::to_game_mode);
      return prelaunch::step_e::succeeded;
    }

    void abort() override {}
  };

  class instant_clock_t final: public prelaunch::clock_t {
  public:
    std::chrono::steady_clock::time_point at {};

    std::chrono::steady_clock::time_point now() override {
      return at;
    }

    void sleep_for(std::chrono::milliseconds duration) override {
      at += duration;
    }
  };

  /// A build without a session switch, whatever this test binary was built with: the unavailable
  /// adapter, over a stand-in host, until the test's stage log resets the seams.
  struct unavailable_build_t {
    session_answer_host_t host;
    prelaunch::unavailable_adapter_t adapter {"/nonexistent/polaris-test/steamosctl"};
    instant_clock_t clock;

    unavailable_build_t() {
      prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
    }

    ~unavailable_build_t() {
      prelaunch::set_dependencies_for_tests(nullptr, nullptr, nullptr);
    }
  };

  /// Records the launch stages for one test and restores every #204 test seam after it.
  struct stage_log_t {
    std::shared_ptr<std::vector<std::string>> stages = std::make_shared<std::vector<std::string>>();

    stage_log_t() {
      auto sink = stages;
      prelaunch::set_stage_sink_for_tests([sink](std::string_view stage) {
        sink->emplace_back(stage);
      });
    }

    ~stage_log_t() {
      prelaunch::set_stage_sink_for_tests({});
      prelaunch::set_dependencies_for_tests(nullptr, nullptr, nullptr);
      proc::set_prelaunch_capture_ready_for_tests({});
    }

    bool has(std::string_view stage) const {
      return std::find(stages->begin(), stages->end(), stage) != stages->end();
    }

    std::ptrdiff_t at(std::string_view stage) const {
      return std::find(stages->begin(), stages->end(), stage) - stages->begin();
    }
  };

  void use_apps(std::vector<proc::ctx_t> apps) {
    proc::proc.reload_configuration(proc::proc_t {boost::process::v1::environment {}, std::move(apps)});
  }

  // Exercise the real critical prep boundary without starting Steam, Gamescope or a title.
  // The sentinel always fails start, and records whether admission let it run at all.
  class nested_prep_sentinel_t {
  public:
    explicit nested_prep_sentinel_t(const std::filesystem::path &root):
        calls {root / "calls"},
        previous_capture {config::video.capture},
        previous_encoder {config::video.encoder},
        previous_ignore_probe {config::video.ignore_encoder_probe_failure} {
      std::filesystem::create_directories(root);
      const auto helper = root / "polaris-gamescope-session";
      write_file(helper, "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"${0%/*}/calls\"\n"
        "if [ \"$1\" = start ]; then exit 73; fi\nexit 0\n");
      const auto gamescope = root / "gamescope";
      write_file(gamescope, "#!/bin/sh\nexit 73\n");
      std::filesystem::permissions(helper, std::filesystem::perms::owner_all);
      std::filesystem::permissions(gamescope, std::filesystem::perms::owner_all);
      if (const auto *value = std::getenv("PATH")) previous_path = value;
      if (const auto *value = std::getenv("POLARIS_GAMESCOPE_SESSION")) previous_helper = value;
      const auto path = root.string() + ":" + previous_path.value_or("");
      setenv("PATH", path.c_str(), 1);
      setenv("POLARIS_GAMESCOPE_SESSION", helper.c_str(), 1);
      // Existing capture test seam; no physical display or encoder is needed to reach prep.
      platf::set_capture_request_satisfiable_for_tests(true);
      config::video.capture = "late-steam-test-unavailable";
      config::video.encoder.clear();
      config::video.ignore_encoder_probe_failure = true;
    }

    ~nested_prep_sentinel_t() {
      if (previous_path) setenv("PATH", previous_path->c_str(), 1);
      else unsetenv("PATH");
      if (previous_helper) setenv("POLARIS_GAMESCOPE_SESSION", previous_helper->c_str(), 1);
      else unsetenv("POLARIS_GAMESCOPE_SESSION");
      platf::set_capture_request_satisfiable_for_tests(std::nullopt);
      config::video.capture = previous_capture;
      config::video.encoder = previous_encoder;
      config::video.ignore_encoder_probe_failure = previous_ignore_probe;
    }

    bool started() const {
      std::ifstream input {calls};
      std::string line;
      while (std::getline(input, line)) if (line == "start" || line.starts_with("start ")) return true;
      return false;
    }

  private:
    std::filesystem::path calls;
    std::optional<std::string> previous_path;
    std::optional<std::string> previous_helper;
    std::string previous_capture;
    std::string previous_encoder;
    bool previous_ignore_probe;
  };

  class LateDesktopSteamTest: public LaunchDesktopSteamTest {
  protected:
    void late_launch(bool device_opt_in, bool app_opt_in, std::string_view request, bool closes_when_asked) {
      stage_log_t log;
      session_answer_host_t host;
      session_answer_adapter_t adapter;
      instant_clock_t clock;
      prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
      proc::set_prelaunch_capture_ready_for_tests([](std::string_view, std::string &) { return true; });
      nested_prep_sentinel_t sentinel {directory / uuid_util::uuid_t::generate().string()};
      const auto client = pair_moonlight(device_opt_in);
      auto app = opted_in(steam_game());
      app.close_desktop_steam_for_private = app_opt_in;
      // Failure cleanup reloads file_apps. Give that real loader the same fixture-owned
      // catalogue as admission, instead of letting a missing host file erase the test apps.
      const auto apps_path = directory / ("late-apps-" + uuid_util::uuid_t::generate().string() + ".json");
      const auto previous_apps_file = std::exchange(config::stream.file_apps, apps_path.string());
      auto restore_apps_file = util::fail_guard([previous_apps_file]() {
        config::stream.file_apps = previous_apps_file;
      });
      ASSERT_EQ(file_handler::write_file(apps_path.string().c_str(), nlohmann::json {{"version", 15}, {"apps", {
        {{"name", app.name}, {"uuid", app.uuid}, {"steam-appid", app.steam_appid},
          {std::string {prelaunch::k_app_key}, true}, {"close-desktop-steam-for-private", app_opt_in},
          {"exclude-global-prep-cmd", true}, {"exclude-global-state-cmd", true}},
        {{"name", desktop().name}, {"uuid", desktop().uuid},
          {"exclude-global-prep-cmd", true}, {"exclude-global-state-cmd", true}},
      }}}.dump()), 0);
      ASSERT_TRUE(proc::proc.reload_configuration_from_file(apps_path.string()));
      const auto initial_apps = proc::proc.get_apps();
      const auto initial_game = std::find_if(initial_apps.begin(), initial_apps.end(), [&](const auto &entry) { return entry.uuid == app.uuid; });
      ASSERT_NE(initial_game, initial_apps.end());
      ASSERT_EQ(initial_game->close_desktop_steam_for_private, app_opt_in);
      ASSERT_TRUE(initial_game->leave_game_mode_before_launch);
      const auto launch_app_id = initial_game->id; // the loader derives IDs; UUID is the persisted identity
      auto persisted_before = nlohmann::json::parse(file_handler::read_file(apps_path.string().c_str()));
      steam.running = false;
      steam.closes_when_asked = closes_when_asked;
      platf::game_mode_host::set_session_live_for_tests(true);
      auto stages = log.stages;
      prelaunch::set_stage_sink_for_tests([this, stages](std::string_view stage) {
        stages->emplace_back(stage);
        if (stage == "prep") steam.running = true; // after the existing post-transition admission
      });

      const auto reply = launch(moonlight, moonlight_launch(launch_app_id, request));
      ASSERT_TRUE(log.has("transition_ready"));
      ASSERT_TRUE(log.has("prep")) << reply.refusal_code << ": " << reply.message;
      EXPECT_LT(log.at("transition_ready"), log.at("prep"));
      const bool effective_opt_in = device_opt_in || app_opt_in || !request.empty();
      if (!effective_opt_in) {
        EXPECT_EQ(reply.status, 409);
        EXPECT_EQ(reply.refusal_code, "desktop_active_private_stream_refused");
        EXPECT_EQ(steam.close_requests.load(), 0);
        EXPECT_TRUE(steam.running.load());
        EXPECT_FALSE(sentinel.started());
      } else if (!closes_when_asked) {
        EXPECT_EQ(reply.status, 409);
        EXPECT_EQ(reply.refusal_code, "desktop_steam_shutdown_failed");
        EXPECT_EQ(steam.close_requests.load(), 1);
        EXPECT_TRUE(steam.running.load());
        EXPECT_FALSE(sentinel.started());
      } else {
        // Only the harmless sentinel ran. Its fixed failure keeps the route from publishing media.
        EXPECT_EQ(reply.status, 503);
        EXPECT_EQ(reply.refusal_code, "gamescope_session_failed");
        EXPECT_EQ(steam.close_requests.load(), 1);
        EXPECT_FALSE(steam.running.load());
        EXPECT_TRUE(sentinel.started());
      }
      EXPECT_EQ(client->close_desktop_steam, device_opt_in);
      EXPECT_EQ(prelaunch::coordinator().receipt().restore, prelaunch::restore_e::rolled_back);
      auto persisted_after = nlohmann::json::parse(file_handler::read_file(apps_path.string().c_str()));
      // The route owns only this timestamp write; all persisted permission/settings bytes
      // represented by JSON values must survive admission, failure cleanup and loader refresh.
      for (auto *catalogue : {&persisted_before, &persisted_after}) {
        for (auto &entry : catalogue->at("apps")) {
          if (entry.value("uuid", std::string {}) == app.uuid) entry.erase("last-launched");
        }
      }
      EXPECT_EQ(persisted_after, persisted_before);
      const auto &stored = proc::proc.get_apps();
      EXPECT_EQ(stored.size(), initial_apps.size());
      const auto game = std::find_if(stored.begin(), stored.end(), [&](const auto &entry) { return entry.uuid == app.uuid; });
      ASSERT_NE(game, stored.end());
      EXPECT_EQ(game->id, launch_app_id);
      EXPECT_EQ(game->close_desktop_steam_for_private, app_opt_in) << "effective consent never changes the stored app";
    }
  };

  /// The production console app list, save and launch handlers on a loopback TLS listener, with an
  /// API key, and the apps file pointed at a scratch copy for the length of the test.
  class console_routes_t {
  public:
    console_routes_t(const std::string &cert, const std::string &key, const std::filesystem::path &apps_file):
        old_username_ {std::exchange(config::sunshine.username, "prelaunch-fixture-user")},
        old_key_ {std::exchange(config::sunshine.api_key, "prelaunch-fixture-key")},
        old_apps_file_ {std::exchange(config::stream.file_apps, apps_file.string())} {
      server_ = std::async(std::launch::async, [this, cert, key]() {
        confighttp::run_console_app_routes_for_tests(cert, key, [this](unsigned short port, std::function<void()> stop) {
          stop_ = std::move(stop);
          listening_.set_value(port);
        });
      });
      port_ = listening_.get_future().get();
    }

    ~console_routes_t() {
      if (stop_) stop_();
      if (server_.valid()) server_.get();
      config::sunshine.username = old_username_;
      config::sunshine.api_key = old_key_;
      config::stream.file_apps = old_apps_file_;
    }

    console_routes_t(const console_routes_t &) = delete;
    console_routes_t &operator=(const console_routes_t &) = delete;

    std::pair<int, nlohmann::json> request(const std::string &method, const std::string &path, const std::string &body = {}) {
      SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(port_), false};
      client.config.timeout = 10;
      SimpleWeb::CaseInsensitiveMultimap headers {{"Authorization", "Bearer prelaunch-fixture-key"}, {"Content-Type", "application/json"}};
      const auto response = client.request(method, path, body, headers);
      const auto content = response->content.string();
      return {std::stoi(response->status_code), nlohmann::json::parse(content, nullptr, false)};
    }

  private:
    std::string old_username_;
    std::string old_key_;
    std::string old_apps_file_;
    std::promise<unsigned short> listening_;
    std::function<void()> stop_;
    std::future<void> server_;
    unsigned short port_ = 0;
  };

  // Model an admitted, committed opted-in launch without opening a compositor or encoder.
  // Only the host/adapter and the media threads are doubles: Close App below is the real TLS
  // Console handler, process stop admission, teardown, binding and restore worker.
  struct console_return_launch_t {
    session_answer_host_t host;
    session_answer_adapter_t adapter;
    instant_clock_t clock;
    proc::ctx_t app = opted_in(desktop());
    std::shared_ptr<rtsp_stream::launch_session_t> launch = std::make_shared<rtsp_stream::launch_session_t>();
    std::uint64_t transition_generation = 0;
    std::atomic<int> retired_slots {0};
    std::atomic<bool> retirement_admitted {false};

    console_return_launch_t() {
      prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
      proc::set_prelaunch_capture_ready_for_tests([](std::string_view, std::string &) { return true; });
      rtsp_stream::set_cleanup_session_probe_for_tests([this]() {
        ++retired_slots;
        retirement_admitted = proc::proc.session_shutdown_requested() &&
          !proc::proc.capture_session_launch_generation();
      });
    }

    ~console_return_launch_t() {
      // Also cleans up the deliberately failing old-code comparator, without joining fake media.
      rtsp_stream::terminate_sessions();
      proc::proc.end_session(false, false);
      prelaunch::coordinator().wait_for_restore();
      rtsp_stream::set_cleanup_session_probe_for_tests({});
      proc::set_prelaunch_capture_ready_for_tests({});
      prelaunch::set_dependencies_for_tests(nullptr, nullptr, nullptr);
    }

    void prepare() {
      ASSERT_EQ(rtsp_stream::session_snapshot({}).active_sessions, 0);
      ASSERT_EQ(rtsp_stream::session_snapshot({}).pending_sessions, 0);
      platf::game_mode_host::set_session_live_for_tests(true);
      const auto admitted = proc::proc.capture_session_launch_generation();
      ASSERT_TRUE(admitted && proc::proc.try_begin_session_launch(*admitted));
      auto finish_launch = util::fail_guard([]() { proc::proc.finish_session_launch(); });
      launch->id = 42901;
      launch->unique_id = "console-return-owner";
      launch->session_token = "console-return-token";
      launch->lifecycle_generation = *admitted;
      const auto ready = proc::prelaunch_for_admitted_launch(
        prelaunch::entry_e::rtsp, app, launch->unique_id, *admitted, "gamescope_stream",
        []() { return false; });
      ASSERT_TRUE(ready.proceed()) << ready.code;
      ASSERT_EQ(ready.state, prelaunch::state_e::ready);
      prelaunch::launch_scope_t scope {ready};
      proc::proc.set_active_launch_for_tests(app, launch);
      proc::proc.set_running_app_for_tests(app); // detached command, no native child
      transition_generation = scope.commit();
      ASSERT_NE(transition_generation, 0u);
      proc::proc.bind_prelaunch_transition(transition_generation);
      ASSERT_TRUE(prelaunch::coordinator().receipt().committed);
    }

    void retained_resumes() {
      for (int cycle = 0; cycle < 2; ++cycle) {
        proc::proc.pause();
        ASSERT_GT(proc::proc.running(), 0);
        proc::proc.resume();
        const auto receipt = prelaunch::coordinator().receipt();
        ASSERT_EQ(receipt.transition_generation, transition_generation);
        ASSERT_TRUE(receipt.committed);
        ASSERT_EQ(receipt.restore, prelaunch::restore_e::owned);
        ASSERT_EQ(proc::proc.get_session_token(), launch->session_token);
      }
    }

    void adopt_exited_child() {
      // Keep a real owned handle with a cached exit status on running()'s cleanup path.
      boost::process::v1::child child {"/bin/true"};
      ASSERT_TRUE(child.wait_for(2s)) << "the fixture child must exit within its bound";
      ASSERT_EQ(child.exit_code(), 0);
      ASSERT_TRUE(child) << "running() must see a real owned child handle";
      proc::proc.set_exited_app_child_for_tests(std::move(child));
    }
  };
}  // namespace

TEST_F(LaunchDesktopSteamTest, ConsoleCloseReturnsCommittedGameModeLaunchAfterRetainedResumes) {
  const auto apps_file = directory / "console-return-apps.json";
  write_file(apps_file, R"({"version":15,"apps":[]})");
  console_routes_t console {host.cert.string(), host.key.string(), apps_file};
  console_return_launch_t owned;
  ASSERT_NO_FATAL_FAILURE(owned.prepare());
  ASSERT_NO_FATAL_FAILURE(owned.retained_resumes());
  rtsp_stream::add_session_for_tests(*owned.launch, false);
  ASSERT_EQ(rtsp_stream::session_snapshot(owned.launch->unique_id).active_sessions, 1);

  const auto [status, reply] = console.request("POST", "/api/apps/close", "{}");
  prelaunch::coordinator().wait_for_restore();

  EXPECT_EQ(status, 200);
  EXPECT_EQ(reply, (nlohmann::json {{"status", true}}));
  EXPECT_EQ(owned.retired_slots.load(), 1) << "the owned slot must retire before the one-shot restore";
  EXPECT_TRUE(owned.retirement_admitted.load()) << "media retirement must already own stop admission";
  EXPECT_EQ(proc::proc.running(), 0);
  EXPECT_EQ(rtsp_stream::session_snapshot({}).active_sessions, 0);
  const auto receipt = prelaunch::coordinator().receipt();
  EXPECT_EQ(receipt.transition_generation, owned.transition_generation);
  EXPECT_EQ(receipt.restore, prelaunch::restore_e::restored) << receipt.restore_reason;
  EXPECT_FALSE(receipt.committed);
  EXPECT_EQ(owned.adapter.starts, (std::vector<prelaunch::direction_e> {
    prelaunch::direction_e::to_desktop, prelaunch::direction_e::to_game_mode}));

  EXPECT_EQ(console.request("POST", "/api/apps/close", "{}").first, 200);
  prelaunch::coordinator().wait_for_restore();
  EXPECT_EQ(owned.retired_slots.load(), 1);
  EXPECT_EQ(owned.adapter.starts.size(), 2u) << "a repeated End cannot return twice";
}

TEST_F(LaunchDesktopSteamTest, ConsoleCloseLeavesOtherClientActiveOrPendingToRefuseGameModeReturn) {
  const auto apps_file = directory / "console-return-other-apps.json";
  write_file(apps_file, R"({"version":15,"apps":[]})");
  console_routes_t console {host.cert.string(), host.key.string(), apps_file};
  for (const bool pending : {false, true}) {
    SCOPED_TRACE(pending ? "other pending viewer" : "other active viewer");
    console_return_launch_t owned;
    ASSERT_NO_FATAL_FAILURE(owned.prepare());
    ASSERT_NO_FATAL_FAILURE(owned.retained_resumes());
    rtsp_stream::add_session_for_tests(*owned.launch, false);
    auto other = std::make_shared<rtsp_stream::launch_session_t>();
    other->id = 42902;
    other->unique_id = "console-return-other-client";
    // A viewer shares the app's token; the token alone is not retirement authority.
    other->session_token = owned.launch->session_token;
    other->watch_only = true;
    if (pending) {
      const auto admitted = proc::proc.capture_session_launch_generation();
      ASSERT_TRUE(admitted && proc::proc.try_begin_session_launch(*admitted));
      auto finish_launch = util::fail_guard([]() { proc::proc.finish_session_launch(); });
      other->lifecycle_generation = *admitted;
      // A real Watch /resume binds its prepared object before publishing RTSP. Teardown must
      // preserve that object too, not only the RTSP event installed immediately afterwards.
      ASSERT_EQ(proc::proc.prepare_capture_for_admitted_launch(other), 0);
      ASSERT_TRUE(rtsp_stream::launch_session_raise(other));
    } else {
      rtsp_stream::add_session_for_tests(*other, false);
    }

    const auto [status, reply] = console.request("POST", "/api/apps/close", "{}");
    prelaunch::coordinator().wait_for_restore();

    EXPECT_EQ(status, 200);
    EXPECT_EQ(reply, (nlohmann::json {{"status", true}}));
    EXPECT_EQ(owned.retired_slots.load(), 1) << "only the owned media may be retired";
    EXPECT_TRUE(owned.retirement_admitted.load());
    const auto remaining = rtsp_stream::session_snapshot(other->unique_id);
    EXPECT_EQ(remaining.active_sessions, pending ? 0 : 1);
    EXPECT_EQ(remaining.pending_sessions, pending ? 1 : 0);
    EXPECT_FALSE(other->is_cancelled());
    const auto receipt = prelaunch::coordinator().receipt();
    EXPECT_EQ(receipt.transition_generation, owned.transition_generation);
    EXPECT_EQ(receipt.restore, prelaunch::restore_e::refused);
    EXPECT_EQ(receipt.restore_reason, "a stream is active or pending on the host");
    EXPECT_EQ(owned.adapter.starts, (std::vector<prelaunch::direction_e> {prelaunch::direction_e::to_desktop}));
  }
}

TEST_F(LaunchDesktopSteamTest, ConsoleCloseCannotRetireMediaWhenAnotherStopOwnsAdmission) {
  const auto apps_file = directory / "console-return-admission-apps.json";
  write_file(apps_file, R"({"version":15,"apps":[]})");
  console_routes_t console {host.cert.string(), host.key.string(), apps_file};
  console_return_launch_t owned;
  ASSERT_NO_FATAL_FAILURE(owned.prepare());
  rtsp_stream::add_session_for_tests(*owned.launch, false);
  ASSERT_TRUE(proc::proc.begin_session_stop_for_tests());
  {
    auto finish_stop = util::fail_guard([]() { proc::proc.finish_session_stop_for_tests(false); });
    const auto [status, reply] = console.request("POST", "/api/apps/close", "{}");
    EXPECT_EQ(status, 200); // preserve the existing admin Close App response
    EXPECT_EQ(reply, (nlohmann::json {{"status", true}}));
    EXPECT_EQ(owned.retired_slots.load(), 0);
    EXPECT_EQ(rtsp_stream::session_snapshot({}).active_sessions, 1);
    EXPECT_TRUE(prelaunch::coordinator().receipt().committed);
  }
}

TEST_F(LaunchDesktopSteamTest, ConsoleCloseRetiresOnlyTheExactOwnedPendingResume) {
  const auto apps_file = directory / "console-return-pending-apps.json";
  write_file(apps_file, R"({"version":15,"apps":[]})");
  console_routes_t console {host.cert.string(), host.key.string(), apps_file};
  console_return_launch_t owned;
  ASSERT_NO_FATAL_FAILURE(owned.prepare());
  ASSERT_NO_FATAL_FAILURE(owned.retained_resumes());
  rtsp_stream::add_session_for_tests(*owned.launch, false);
  auto resume = std::make_shared<rtsp_stream::launch_session_t>();
  resume->id = 42903;
  resume->unique_id = owned.launch->unique_id;
  resume->session_token = owned.launch->session_token;
  const auto admitted = proc::proc.capture_session_launch_generation();
  ASSERT_TRUE(admitted && proc::proc.try_begin_session_launch(*admitted));
  {
    auto finish_launch = util::fail_guard([]() { proc::proc.finish_session_launch(); });
    resume->lifecycle_generation = *admitted;
    ASSERT_EQ(proc::proc.prepare_capture_for_admitted_launch(resume), 0);
    ASSERT_TRUE(rtsp_stream::launch_session_raise(resume));
  }

  EXPECT_EQ(console.request("POST", "/api/apps/close", "{}").first, 200);
  prelaunch::coordinator().wait_for_restore();

  EXPECT_EQ(owned.retired_slots.load(), 1);
  EXPECT_TRUE(owned.retirement_admitted.load());
  EXPECT_TRUE(resume->is_cancelled());
  EXPECT_EQ(rtsp_stream::session_snapshot({}).pending_sessions, 0);
  const auto receipt = prelaunch::coordinator().receipt();
  EXPECT_EQ(receipt.restore, prelaunch::restore_e::restored) << receipt.restore_reason;
}

TEST_F(LaunchDesktopSteamTest, ConsoleCloseCannotRetireUnboundStaleOrIndependentMedia) {
  const auto apps_file = directory / "console-return-binding-apps.json";
  write_file(apps_file, R"({"version":15,"apps":[]})");
  console_routes_t console {host.cert.string(), host.key.string(), apps_file};
  for (const std::string kind : {"missing token", "other owner", "stale token", "stale generation", "worker"}) {
    SCOPED_TRACE(kind);
    console_return_launch_t owned;
    ASSERT_NO_FATAL_FAILURE(owned.prepare());
    rtsp_stream::launch_session_t media {};
    media.id = 42904;
    media.unique_id = owned.launch->unique_id;
    media.session_token = owned.launch->session_token;
    media.lifecycle_generation = owned.launch->lifecycle_generation;
    if (kind == "missing token") owned.launch->session_token.clear();
    if (kind == "other owner") media.unique_id = "another-launch-owner";
    if (kind == "stale token") media.session_token = "previous-launch-token";
    if (kind == "stale generation") media.lifecycle_generation = *media.lifecycle_generation + 1;
    if (kind == "worker") media.require_worker_connection();
    rtsp_stream::add_session_for_tests(media, false);

    EXPECT_EQ(console.request("POST", "/api/apps/close", "{}").first, 200);
    prelaunch::coordinator().wait_for_restore();

    EXPECT_EQ(owned.retired_slots.load(), 0);
    EXPECT_EQ(rtsp_stream::session_snapshot({}).active_sessions, 1);
    const auto receipt = prelaunch::coordinator().receipt();
    EXPECT_EQ(receipt.restore, prelaunch::restore_e::refused);
    EXPECT_EQ(receipt.restore_reason, "a stream is active or pending on the host");
    EXPECT_EQ(owned.adapter.starts, (std::vector<prelaunch::direction_e> {prelaunch::direction_e::to_desktop}));
  }
}

TEST_F(LaunchDesktopSteamTest, ConsoleClosePreservesPreparedPendingWithoutExactOrdinaryHostAuthority) {
  const auto apps_file = directory / "console-return-prepared-binding-apps.json";
  write_file(apps_file, R"({"version":15,"apps":[]})");
  console_routes_t console {host.cert.string(), host.key.string(), apps_file};
  for (const std::string kind : {"missing token", "missing generation", "other owner", "stale token", "stale generation", "worker"}) {
    SCOPED_TRACE(kind);
    console_return_launch_t owned;
    ASSERT_NO_FATAL_FAILURE(owned.prepare());
    rtsp_stream::add_session_for_tests(*owned.launch, false);
    auto pending = std::make_shared<rtsp_stream::launch_session_t>();
    pending->id = 42905;
    pending->unique_id = owned.launch->unique_id;
    pending->session_token = owned.launch->session_token;
    pending->watch_only = true; // preparation binds metadata without starting capture
    const auto admitted = proc::proc.capture_session_launch_generation();
    ASSERT_TRUE(admitted && proc::proc.try_begin_session_launch(*admitted));
    {
      auto finish_launch = util::fail_guard([]() { proc::proc.finish_session_launch(); });
      pending->lifecycle_generation = *admitted;
      if (kind == "missing token") owned.launch->session_token.clear();
      if (kind == "missing generation") owned.launch->lifecycle_generation.reset();
      if (kind == "other owner") pending->unique_id = "other-prepared-owner";
      if (kind == "stale token") pending->session_token = "previous-prepared-token";
      if (kind == "stale generation") pending->lifecycle_generation = *admitted + 1;
      if (kind == "worker") pending->require_worker_connection();
      ASSERT_EQ(proc::proc.prepare_capture_for_admitted_launch(pending), 0);
      ASSERT_TRUE(rtsp_stream::launch_session_raise(pending));
    }

    EXPECT_EQ(console.request("POST", "/api/apps/close", "{}").first, 200);
    prelaunch::coordinator().wait_for_restore();

    EXPECT_FALSE(pending->is_cancelled());
    EXPECT_EQ(rtsp_stream::session_snapshot({}).pending_sessions, 1);
    const auto receipt = prelaunch::coordinator().receipt();
    EXPECT_EQ(receipt.restore, prelaunch::restore_e::refused);
    EXPECT_EQ(receipt.restore_reason, "a stream is active or pending on the host");
    EXPECT_EQ(owned.adapter.starts, (std::vector<prelaunch::direction_e> {prelaunch::direction_e::to_desktop}));
  }
}

TEST_F(LaunchDesktopSteamTest, ConsoleCloseWithoutReturnTransitionCancelsPreparedPendingAndAdmitsNextLaunch) {
  const auto apps_file = directory / "console-close-ordinary-pending-apps.json";
  write_file(apps_file, R"({"version":15,"apps":[]})");
  console_routes_t console {host.cert.string(), host.key.string(), apps_file};
  console_return_launch_t owned;
  owned.app = desktop();
  platf::game_mode_host::set_session_live_for_tests(false);
  const auto admitted = proc::proc.capture_session_launch_generation();
  ASSERT_TRUE(admitted && proc::proc.try_begin_session_launch(*admitted));
  auto pending = std::make_shared<rtsp_stream::launch_session_t>();
  {
    auto finish_launch = util::fail_guard([]() { proc::proc.finish_session_launch(); });
    owned.launch->id = 42906;
    owned.launch->unique_id = "ordinary-console-owner";
    owned.launch->session_token = "ordinary-console-token";
    owned.launch->lifecycle_generation = *admitted;
    proc::proc.set_active_launch_for_tests(owned.app, owned.launch);
    proc::proc.set_running_app_for_tests(owned.app);
    pending->id = 42907;
    pending->unique_id = "ordinary-console-pending-viewer";
    pending->session_token = owned.launch->session_token;
    pending->lifecycle_generation = *admitted;
    pending->watch_only = true;
    ASSERT_EQ(proc::proc.prepare_capture_for_admitted_launch(pending), 0);
    ASSERT_TRUE(rtsp_stream::launch_session_raise(pending));
  }
  ASSERT_EQ(rtsp_stream::session_snapshot({}).pending_sessions, 1);
  ASSERT_FALSE(prelaunch::coordinator().receipt().committed);

  const auto [status, reply] = console.request("POST", "/api/apps/close", "{}");
  prelaunch::coordinator().wait_for_restore();

  EXPECT_EQ(status, 200);
  EXPECT_EQ(reply, (nlohmann::json {{"status", true}}));
  EXPECT_TRUE(pending->is_cancelled());
  EXPECT_EQ(rtsp_stream::session_snapshot({}).pending_sessions, 0);
  EXPECT_EQ(proc::proc.running(), 0);
  EXPECT_TRUE(owned.adapter.starts.empty());
  const auto next = proc::proc.capture_session_launch_generation();
  ASSERT_TRUE(next.has_value());
  const bool next_admitted = proc::proc.try_begin_session_launch(*next);
  EXPECT_TRUE(next_admitted) << "ordinary Close must not leave stale RTSP publication blocking launch";
  if (next_admitted) proc::proc.finish_session_launch();
}

TEST_F(LaunchDesktopSteamTest, ConsoleCloseDefersExitedChildCleanupBeforeAndAfterMediaClaim) {
  const auto apps_file = directory / "console-return-exited-child-apps.json";
  write_file(apps_file, R"({"version":15,"apps":[]})");
  console_routes_t console {host.cert.string(), host.key.string(), apps_file};
  for (const bool after_claim : {false, true}) {
    SCOPED_TRACE(after_claim ? "after owned slot claim" : "before owned slot claim");
    console_return_launch_t owned;
    ASSERT_NO_FATAL_FAILURE(owned.prepare());
    rtsp_stream::add_session_for_tests(*owned.launch, false);
    ASSERT_NO_FATAL_FAILURE(owned.adopt_exited_child());
    bool observed = false;
    const auto poll_exited_child = [&]() {
      if (observed) return;
      observed = true;
      EXPECT_TRUE(proc::proc.session_shutdown_requested());
      EXPECT_EQ(rtsp_stream::session_snapshot({}).active_sessions, after_claim ? 0 : 1);
      // Actual running(), with a real exited child and auto-detach disabled, used to enter
      // terminate_impl here. Wait out any incorrectly queued worker before inspecting its effects.
      EXPECT_EQ(proc::proc.running(), 0);
      prelaunch::coordinator().wait_for_restore();
      const auto receipt = prelaunch::coordinator().receipt();
      EXPECT_EQ(receipt.transition_generation, owned.transition_generation);
      EXPECT_TRUE(receipt.committed) << "the admitted Console drain still owns app cleanup";
      EXPECT_EQ(receipt.restore, prelaunch::restore_e::owned);
      EXPECT_EQ(proc::proc.get_session_token(), owned.launch->session_token);
      EXPECT_EQ(owned.adapter.starts, (std::vector<prelaunch::direction_e> {prelaunch::direction_e::to_desktop}));
    };
    auto reset_observers = util::fail_guard([]() {
      proc::proc.set_stop_before_media_observer_for_tests({});
      rtsp_stream::set_cleanup_unlocked_probe_for_tests({});
    });
    if (after_claim) rtsp_stream::set_cleanup_unlocked_probe_for_tests(poll_exited_child);
    else proc::proc.set_stop_before_media_observer_for_tests(poll_exited_child);

    EXPECT_EQ(console.request("POST", "/api/apps/close", "{}").first, 200);
    prelaunch::coordinator().wait_for_restore();

    EXPECT_TRUE(observed) << "the real Console stop must reach the selected media boundary";
    EXPECT_EQ(owned.retired_slots.load(), 1);
    EXPECT_TRUE(owned.retirement_admitted.load());
    EXPECT_EQ(prelaunch::coordinator().receipt().restore, prelaunch::restore_e::restored);
    EXPECT_EQ(owned.adapter.starts, (std::vector<prelaunch::direction_e> {
      prelaunch::direction_e::to_desktop, prelaunch::direction_e::to_game_mode}));
    proc::proc.set_stop_before_media_observer_for_tests({});
    rtsp_stream::set_cleanup_unlocked_probe_for_tests({});

    // The deferral belongs to that stop alone; an exited child in a later launch is cleaned by
    // running() normally, and its unrequested end refuses return through the production path.
    ASSERT_NO_FATAL_FAILURE(owned.prepare());
    ASSERT_NO_FATAL_FAILURE(owned.adopt_exited_child());
    EXPECT_EQ(proc::proc.running(), 0);
    prelaunch::coordinator().wait_for_restore();
    EXPECT_FALSE(prelaunch::coordinator().receipt().committed);
    EXPECT_EQ(prelaunch::coordinator().receipt().restore, prelaunch::restore_e::refused);
    EXPECT_EQ(prelaunch::coordinator().receipt().restore_reason, "the session did not end on request");
    EXPECT_TRUE(proc::proc.get_session_token().empty());
  }
}

TEST_F(LaunchDesktopSteamTest, OptedInLaunchOnAGameModeHostIsRefusedByTheUnavailableAdapterBeforeAnythingElse) {
  stage_log_t log;
  unavailable_build_t build;
  pair_moonlight(std::nullopt);
  use_apps({opted_in(steam_game()), desktop(), pinned_private()});
  platf::game_mode_host::set_session_live_for_tests(true);
  const auto before = prelaunch::coordinator().receipt().transition_generation;

  for (const auto *id : {steam_game_id, pinned_id}) {
    const auto reply = launch(moonlight, moonlight_launch(id));
    EXPECT_EQ(reply.status, 503) << id;
    EXPECT_EQ(reply.refusal_code, "prelaunch_transition_unavailable") << id << ": the pin is not judged first";
    EXPECT_TRUE(reply.recommended_action.empty()) << id;
  }
  EXPECT_FALSE(log.has("launch_as")) << "nothing after the refusal ran";
  EXPECT_FALSE(log.has("semantics"));
  EXPECT_FALSE(log.has("transition_start")) << "the production adapter is never started";
  EXPECT_EQ(steam.close_requests.load(), 0);
  const auto receipt = prelaunch::coordinator().receipt();
  EXPECT_GT(receipt.transition_generation, before);
  EXPECT_EQ(receipt.code, "prelaunch_transition_unavailable");
  EXPECT_FALSE(receipt.changed_session);
}

TEST_F(LaunchDesktopSteamTest, WithTheSettingOffTheLaunchIsTheOneItAlwaysWas) {
  stage_log_t log;
  pair_moonlight(std::nullopt);
  auto pin = pinned_private();
  pin.leave_game_mode_before_launch = false;
  use_apps({steam_game(), desktop(), pin});
  const auto before = prelaunch::coordinator().receipt().transition_generation;

  // Out of Game Mode: the desktop Steam step refuses, as before.
  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));
  EXPECT_EQ(reply.error_code, "desktop_active_private_stream_refused");
  // In Game Mode: the pin is refused for Game Mode, as before.
  platf::game_mode_host::set_session_live_for_tests(true);
  const auto pinned = launch(moonlight, moonlight_launch(pinned_id));
  EXPECT_EQ(pinned.refusal_code, "app_launch_mode_unavailable");

  EXPECT_FALSE(log.has("transition_start"));
  EXPECT_LT(log.at("launch_as"), log.at("semantics"));
  EXPECT_EQ(prelaunch::coordinator().receipt().transition_generation, before) << "no receipt for an app that is off";
}

TEST_F(LaunchDesktopSteamTest, UnverifiedAndViewOnlyClientsNeverReachTheTransition) {
  stage_log_t log;
  session_answer_host_t host;
  session_answer_adapter_t adapter;
  instant_clock_t clock;
  prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
  proc::set_prelaunch_capture_ready_for_tests([](std::string_view, std::string &) {
    return true;
  });
  use_apps({opted_in(steam_game()), desktop()});
  platf::game_mode_host::set_session_live_for_tests(true);

  pair_moonlight(std::nullopt, crypto::PERM::_default);
  EXPECT_EQ(launch(stranger, moonlight_launch(steam_game_id)).status, 401);
  EXPECT_EQ(launch(moonlight, moonlight_launch(steam_game_id)).status, 403);
  EXPECT_TRUE(adapter.starts.empty());
  EXPECT_EQ(host.scans.load(), 0);
  EXPECT_TRUE(log.stages->empty());
}

TEST_F(LaunchDesktopSteamTest, AnAdmittedTransitionPrecedesLaunchAsAndTheModeDecisionAndIsRolledBackWhenTheLaunchFails) {
  stage_log_t log;
  session_answer_host_t host;
  session_answer_adapter_t adapter;
  instant_clock_t clock;
  prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
  proc::set_prelaunch_capture_ready_for_tests([](std::string_view, std::string &) {
    return true;
  });
  pair_moonlight(std::nullopt);
  use_apps({opted_in(steam_game()), desktop()});
  platf::game_mode_host::set_session_live_for_tests(true);

  // After the switch the host is out of Game Mode, so the private launch meets the desktop Steam
  // step, which refuses it (the device's switch is off). The launch therefore never starts the app.
  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));
  // In Game Mode the same launch would be admitted as a mirror of the Game Mode screen instead.
  EXPECT_EQ(reply.error_code, "desktop_active_private_stream_refused") << "the mode decision saw the desktop";

  ASSERT_TRUE(log.has("transition_ready"));
  EXPECT_LT(log.at("transition_start"), log.at("transition_ready"));
  EXPECT_LT(log.at("transition_ready"), log.at("launch_as"));
  EXPECT_LT(log.at("launch_as"), log.at("semantics"));
  EXPECT_FALSE(log.has("prep")) << "refused before the app's own prep commands";

  // The failed launch rolls back only the desktop it opened.
  ASSERT_EQ(adapter.starts.size(), 2u);
  EXPECT_EQ(adapter.starts.back(), prelaunch::direction_e::to_game_mode);
  EXPECT_EQ(prelaunch::coordinator().receipt().restore, prelaunch::restore_e::rolled_back);
  EXPECT_TRUE(platf::game_mode_host::session_live_fresh());
}

TEST_F(LaunchDesktopSteamTest, PrelaunchWaitsForFreshProcessAbsenceBeforeHandOff) {
  for (const bool lingering_process : {false, true}) {
    SCOPED_TRACE(lingering_process);
    stage_log_t log;
    session_answer_host_t host_double;
    session_answer_adapter_t adapter;
    instant_clock_t clock;
    host_double.typed_game_mode = true;
    bool returning = false;
    adapter.on_switch = [&](prelaunch::direction_e direction) {
      returning = direction == prelaunch::direction_e::to_game_mode;
      host_double.typed_game_mode = returning;
    };
    const auto clears_at = lingering_process ? 3 * prelaunch::k_poll_period : 0ms;
    std::atomic<int> process_scans {0};
    bool switched = false;
    host_double.on_scan = [&]() {
      if (host_double.typed_game_mode == false) switched = true;
    };
    platf::game_mode_host::set_session_live_for_tests(std::nullopt);
    platf::game_mode_host::set_session_scan_for_tests([&]() {
      ++process_scans;
      return !switched || returning || clock.at.time_since_epoch() < clears_at;
    });
    auto reset_scan = util::fail_guard([]() {
      platf::game_mode_host::set_session_scan_for_tests({});
      platf::game_mode_host::set_session_live_for_tests(false);
    });
    ASSERT_TRUE(platf::game_mode_host::session_live()) << "prime the real cache with Game Mode";
    prelaunch::set_dependencies_for_tests(&host_double, &adapter, &clock);
    int capture_checks = 0;
    int typed_before_capture = 0;
    proc::set_prelaunch_capture_ready_for_tests([&](std::string_view, std::string &) {
      ++capture_checks;
      typed_before_capture = host_double.scans.load();
      EXPECT_EQ(host_double.typed_game_mode, false);
      return true;
    });
    const auto generation = proc::proc.capture_session_launch_generation();
    ASSERT_TRUE(generation && proc::proc.try_begin_session_launch(*generation));
    auto release = util::fail_guard([]() { proc::proc.finish_session_launch(); });
    const auto outcome = proc::prelaunch_for_admitted_launch(
      prelaunch::entry_e::rtsp, opted_in(steam_game()), "client-427", *generation, {}, {});
    prelaunch::launch_scope_t rollback {outcome};
    rollback.set_others([](std::string &) { return false; });
    ASSERT_TRUE(outcome.proceed()) << outcome.code;
    EXPECT_EQ(outcome.state, prelaunch::state_e::ready);
    EXPECT_FALSE(platf::game_mode_host::session_live()) << "handoff must not retain the old true answer";
    EXPECT_GE(process_scans.load(), lingering_process ? 4 : 2);
    EXPECT_GE(clock.at.time_since_epoch(), clears_at);
    EXPECT_LT(clock.at.time_since_epoch(), prelaunch::budgets_t {}.forward);
    EXPECT_EQ(capture_checks, 1);
    EXPECT_EQ(host_double.scans.load(), typed_before_capture + 1) << "fresh typed identity after readiness";
    const auto receipt = prelaunch::coordinator().receipt();
    EXPECT_EQ(receipt.pre, stand_in_session("1", prelaunch::mode_e::game, 'a'));
    EXPECT_EQ(receipt.post, stand_in_session("2", prelaunch::mode_e::desktop, 'b'));
    EXPECT_FALSE(log.has("prep"));
    EXPECT_EQ(proc::proc.running(), 0) << "this admitted-entry check never executes an app";
  }
}

TEST_F(LaunchDesktopSteamTest, PersistentOrUnreadableProcessModeRefusesBeforeHandOff) {
  for (const bool unreadable : {false, true}) {
    SCOPED_TRACE(unreadable);
    stage_log_t log;
    session_answer_host_t host_double;
    session_answer_adapter_t adapter;
    instant_clock_t clock;
    host_double.typed_game_mode = true;
    bool switched = false;
    adapter.on_switch = [&](prelaunch::direction_e direction) {
      switched = direction == prelaunch::direction_e::to_desktop;
      host_double.typed_game_mode = !switched;
    };
    platf::game_mode_host::set_session_live_for_tests(std::nullopt);
    platf::game_mode_host::set_session_scan_for_tests([&]() {
      if (switched && unreadable) throw std::runtime_error {"unavailable process observation"};
      return true;
    });
    auto reset_scan = util::fail_guard([]() {
      platf::game_mode_host::set_session_scan_for_tests({});
      platf::game_mode_host::set_session_live_for_tests(false);
    });
    ASSERT_TRUE(platf::game_mode_host::session_live());
    prelaunch::budgets_t budget;
    budget.forward = 4 * prelaunch::k_poll_period;
    prelaunch::set_dependencies_for_tests(&host_double, &adapter, &clock, budget);
    proc::set_prelaunch_capture_ready_for_tests([](std::string_view, std::string &) { return true; });
    const auto generation = proc::proc.capture_session_launch_generation();
    ASSERT_TRUE(generation && proc::proc.try_begin_session_launch(*generation));
    auto release = util::fail_guard([]() { proc::proc.finish_session_launch(); });
    const auto outcome = proc::prelaunch_for_admitted_launch(
      prelaunch::entry_e::rtsp, opted_in(steam_game()), "client-427", *generation, {}, {});
    prelaunch::launch_scope_t rollback {outcome};
    rollback.set_others([](std::string &) { return false; });
    EXPECT_FALSE(outcome.proceed());
    EXPECT_EQ(outcome.code, "prelaunch_capture_timeout");
    EXPECT_EQ(clock.at.time_since_epoch(), budget.forward) << "one original forward deadline, no reset";
    EXPECT_FALSE(log.has("transition_ready"));
    EXPECT_FALSE(log.has("prep"));
    EXPECT_EQ(proc::proc.running(), 0);
    EXPECT_EQ(prelaunch::coordinator().receipt().restore, prelaunch::restore_e::rolled_back);
    ASSERT_EQ(adapter.starts.size(), 2u);
    EXPECT_EQ(adapter.starts.back(), prelaunch::direction_e::to_game_mode);
  }
}

TEST_F(LaunchDesktopSteamTest, DesktopSteamAppearingDuringTransitionIsRefusedWithoutConsent) {
  stage_log_t log;
  session_answer_host_t host;
  session_answer_adapter_t adapter;
  instant_clock_t clock;
  prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
  proc::set_prelaunch_capture_ready_for_tests([](std::string_view, std::string &) { return true; });
  pair_moonlight(false);
  use_apps({opted_in(steam_game()), desktop()});
  steam.running = false;
  platf::game_mode_host::set_session_live_for_tests(true);
  host.on_scan = [&] {
    if (!platf::game_mode_host::session_live_fresh()) steam.running = true;
  };

  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));
  EXPECT_EQ(reply.error_code, "desktop_active_private_stream_refused");
  EXPECT_TRUE(log.has("transition_ready"));
  EXPECT_FALSE(log.has("prep"));
  EXPECT_EQ(steam.close_requests.load(), 0);
  EXPECT_TRUE(steam.running.load());
  EXPECT_EQ(prelaunch::coordinator().receipt().restore, prelaunch::restore_e::rolled_back);
}

TEST_F(LaunchDesktopSteamTest, DesktopSteamAppearingDuringTransitionRequiresSuccessfulOptInShutdown) {
  stage_log_t log;
  session_answer_host_t host;
  session_answer_adapter_t adapter;
  instant_clock_t clock;
  prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
  proc::set_prelaunch_capture_ready_for_tests([](std::string_view, std::string &) { return true; });
  pair_moonlight(true);
  use_apps({opted_in(steam_game()), desktop()});
  steam.running = false;
  steam.closes_when_asked = false;
  platf::game_mode_host::set_session_live_for_tests(true);
  host.on_scan = [&] {
    if (!platf::game_mode_host::session_live_fresh()) steam.running = true;
  };

  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));
  EXPECT_EQ(reply.error_code, "desktop_steam_shutdown_failed");
  EXPECT_TRUE(log.has("transition_ready"));
  EXPECT_FALSE(log.has("prep"));
  EXPECT_EQ(steam.close_requests.load(), 1);
  EXPECT_TRUE(steam.running.load());
  EXPECT_EQ(prelaunch::coordinator().receipt().restore, prelaunch::restore_e::rolled_back);
}

TEST_F(LateDesktopSteamTest, AppearingAfterAdmissionIsRefusedBeforeNestedPrepWithoutConsent) {
  late_launch(false, false, {}, false);
}

TEST_F(LateDesktopSteamTest, AppearingAfterAdmissionUsesTheAdmittedDeviceConsent) {
  late_launch(true, false, {}, true);
}

TEST_F(LateDesktopSteamTest, AppearingAfterAdmissionUsesTheAdmittedAppConsent) {
  late_launch(false, true, {}, true);
}

TEST_F(LateDesktopSteamTest, AppearingAfterAdmissionUsesTheAdmittedRequestConsent) {
  late_launch(false, false, "&closeDesktopSteamForPrivate=true", true);
}

TEST_F(LateDesktopSteamTest, AppearingAfterAdmissionMustFinishOptInShutdownBeforeNestedPrep) {
  late_launch(true, false, {}, false);
}

TEST_F(LaunchDesktopSteamTest, APinnedPrivateLaunchAsIsJudgedAfterTheTransitionNotRefusedForGameMode) {
  stage_log_t log;
  session_answer_host_t host;
  session_answer_adapter_t adapter;
  instant_clock_t clock;
  prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
  proc::set_prelaunch_capture_ready_for_tests([](std::string_view, std::string &) {
    return true;
  });
  pair_moonlight(std::nullopt);
  use_apps({steam_game(), desktop(), pinned_private()});
  platf::game_mode_host::set_session_live_for_tests(true);

  const auto reply = launch(moonlight, moonlight_launch(pinned_id));
  ASSERT_TRUE(log.has("transition_ready")) << reply.refusal_code << ": " << reply.message;
  EXPECT_LT(log.at("transition_ready"), log.at("launch_as"));
  // Whatever this machine's Gamescope Stream readiness, the pin was not refused for Game Mode.
  EXPECT_EQ(reply.message.find("Game Mode"), std::string::npos) << reply.message;
  EXPECT_NE(reply.status, 200) << "the stand-in host never admits a real private launch";
}

TEST_F(LaunchDesktopSteamTest, OnlyTheRequestingClientCancelsItsTransitionAndNoLifecycleLockIsHeldWhileItWaits) {
  stage_log_t log;
  session_answer_host_t host;
  host.desktop_appears = false;  // the run stays waiting for the desktop
  session_answer_adapter_t adapter;
  instant_clock_t clock;
  prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
  const auto device = pair_moonlight(std::nullopt);
  use_apps({opted_in(steam_game()), desktop()});
  platf::game_mode_host::set_session_live_for_tests(true);

  bool other_refused = false;
  bool owner_cancelled = false;
  host.on_scan = [&]() {
    if (adapter.starts.empty() || owner_cancelled) return;
    // The production /cancel step, from this request's own wait: it takes the lifecycle lock.
    other_refused = !proc::proc.cancel_capture_preparation_for_shutdown("0A1B2C3D-0000-4000-8000-000000000000", "", true, false);
    owner_cancelled = proc::proc.cancel_capture_preparation_for_shutdown(device->uuid, "", true, false) != nullptr;
  };
  const auto reply = launch(moonlight, moonlight_launch(steam_game_id));
  EXPECT_TRUE(other_refused);
  EXPECT_TRUE(owner_cancelled);
  EXPECT_EQ(reply.refusal_code, "prelaunch_cancelled");
  EXPECT_FALSE(log.has("launch_as"));
  EXPECT_EQ(prelaunch::coordinator().receipt().restore, prelaunch::restore_e::refused) << "no identity to undo";
  EXPECT_EQ(adapter.starts.size(), 1u);
}

TEST_F(LaunchDesktopSteamTest, JoiningTheRunningOptedInAppNeverTransitions) {
  stage_log_t log;
  session_answer_host_t host;
  session_answer_adapter_t adapter;
  instant_clock_t clock;
  prelaunch::set_dependencies_for_tests(&host, &adapter, &clock);
  pair_moonlight(std::nullopt);
  use_apps({opted_in(steam_game()), desktop()});
  platf::game_mode_host::set_session_live_for_tests(true);
  proc::proc.set_running_app_for_tests(opted_in(steam_game()));
  auto stop_game = util::fail_guard([]() {
    proc::proc.clear_running_app_for_tests();
  });

  (void) launch(moonlight, moonlight_launch(steam_game_id));
  (void) launch(moonlight, moonlight_launch(steam_game_id, "&watch=1"));
  EXPECT_TRUE(adapter.starts.empty());
  EXPECT_EQ(host.scans.load(), 0);
}

TEST_F(LaunchDesktopSteamTest, BrowserStreamRefusesAnOptedInPinBeforeJudgingItForGameMode) {
  stage_log_t log;
  unavailable_build_t build;
  use_apps({steam_game(), desktop(), pinned_private()});
  platf::game_mode_host::set_session_live_for_tests(true);

  const auto output = browser_stream::create_session("127.0.0.1", "localhost", pinned_private().uuid, "balanced");
  EXPECT_FALSE(output.value("status", true));
  EXPECT_EQ(output.value("error_code", std::string {}), "prelaunch_transition_unavailable");
  EXPECT_FALSE(log.has("launch_as"));

  auto off = pinned_private();
  off.leave_game_mode_before_launch = false;
  use_apps({steam_game(), desktop(), off});
  const auto before = browser_stream::create_session("127.0.0.1", "localhost", off.uuid, "balanced");
  EXPECT_EQ(before.value("error_code", std::string {}), "app_launch_mode_unavailable") << "off: refused for Game Mode as before";
}

TEST_F(LaunchDesktopSteamTest, ConsoleLaunchRefusesAnOptedInPinBeforeJudgingItForGameMode) {
  stage_log_t log;
  unavailable_build_t build;
  console_routes_t console {host.cert.string(), host.key.string(), directory / "console-launch-apps.json"};
  const auto post = [&](const std::string &uuid) {
    return console.request("POST", "/api/apps/launch", nlohmann::json {{"uuid", uuid}}.dump()).second;
  };

  use_apps({steam_game(), desktop(), pinned_private()});
  platf::game_mode_host::set_session_live_for_tests(true);
  const auto refused = post(pinned_private().uuid);
  EXPECT_FALSE(refused.value("status", true));
  EXPECT_EQ(refused.value("error_code", std::string {}), "prelaunch_transition_unavailable");
  EXPECT_FALSE(log.has("launch_as"));

  auto off = pinned_private();
  off.leave_game_mode_before_launch = false;
  use_apps({steam_game(), desktop(), off});
  const auto before = post(off.uuid);
  EXPECT_EQ(before.value("error_code", std::string {}), "app_launch_mode_unavailable") << "off: refused for Game Mode as before";
}
TEST_F(LaunchDesktopSteamTest, RefusedLaunchesAndResumesLeaveTheHostsModeAndGameModeAnswerAlone) {
  // The real Game Mode reconciliation, reading a scan this test counts (no pinned answer).
  std::atomic<int> scans {0};
  platf::game_mode_host::set_session_live_for_tests(std::nullopt);
  platf::game_mode_host::set_session_scan_for_tests([&scans]() {
    ++scans;
    return true;
  });
  auto release_hold = util::fail_guard([]() {
    // Leave Game Mode in this process's answer so a hold taken by the control below is released.
    platf::game_mode_host::set_session_scan_for_tests([]() { return false; });
    nvhttp::reconcile_game_mode_host();
    platf::game_mode_host::set_session_scan_for_tests({});
    platf::game_mode_host::set_session_live_for_tests(false);
  });
  pair_moonlight(true);
  const auto mode_unchanged = [&](const std::string &what) {
    EXPECT_EQ(scans.load(), 0) << what << ": the Game Mode answer was read or kept";
    EXPECT_TRUE(stream_display_policy::game_mode_held_selection().empty()) << what << ": the mode was held";
    EXPECT_EQ(config::video.linux_display.stream_mode, "gamescope_stream") << what << ": the live mode changed";
  };

  EXPECT_EQ(launch(stranger, moonlight_launch(steam_game_id)).status, 401);
  mode_unchanged("unverified /launch");
  EXPECT_EQ(launch(moonlight, "/launch?appid="s + steam_game_id + "&rikeyid=1&localAudioPlayMode=0&corever=1").status, 400);
  mode_unchanged("malformed /launch");
  {
    ASSERT_TRUE(proc::proc.begin_session_stop_for_tests());
    auto finish_stop = util::fail_guard([]() {
      proc::proc.finish_session_stop_for_tests(false);
    });
    EXPECT_EQ(launch(moonlight, moonlight_launch(steam_game_id)).status, 409);
    mode_unchanged("/launch without the reservation");
  }
  EXPECT_EQ(launch(stranger, "/resume?rikey=" + std::string(32, 'a') + "&rikeyid=1").status, 401);
  mode_unchanged("unverified /resume");
  EXPECT_EQ(launch(moonlight, "/resume?rikey=" + std::string(32, 'a') + "&rikeyid=1").status, 503);
  mode_unchanged("/resume with nothing to resume");

  // Control: a permitted, reserved, well-formed request does bring the mode in line, so the
  // assertions above observe the real reconciliation. App 999 then fails its lookup, before execute.
  EXPECT_EQ(launch(moonlight, moonlight_launch("999")).status, 404);
  EXPECT_GE(scans.load(), 1);
  EXPECT_EQ(stream_display_policy::game_mode_held_selection(), "gamescope_stream");
  EXPECT_EQ(config::video.linux_display.stream_mode, "desktop_display");
}

TEST_F(LaunchDesktopSteamTest, ConsoleAndBrowserAdmissionRefusedWhileAnotherRequestHoldsTheLaunchReleaseNothing) {
  stage_log_t log;
  session_answer_host_t host_double;
  session_answer_adapter_t adapter;
  instant_clock_t clock;
  prelaunch::set_dependencies_for_tests(&host_double, &adapter, &clock);
  proc::set_prelaunch_capture_ready_for_tests([](std::string_view, std::string &) {
    return true;
  });
  use_apps({steam_game(), desktop(), pinned_private()});
  platf::game_mode_host::set_session_live_for_tests(true);
  console_routes_t console {host.cert.string(), host.key.string(), directory / "console-admission-apps.json"};

  // Another request holds the launch reservation.
  const auto held = proc::proc.capture_session_launch_generation();
  ASSERT_TRUE(held && proc::proc.try_begin_session_launch(*held));
  bool still_held = false;
  {
    auto release = util::fail_guard([]() {
      proc::proc.finish_session_launch();
    });
    const auto [status, refused] = console.request("POST", "/api/apps/launch", nlohmann::json {{"uuid", pinned_private().uuid}}.dump());
    EXPECT_EQ(status, 409);
    EXPECT_EQ(refused.value("error_code", std::string {}), "prelaunch_launch_busy");
    const auto browser = browser_stream::create_session("127.0.0.1", "localhost", pinned_private().uuid, "balanced");
    EXPECT_EQ(browser.value("error_code", std::string {}), "prelaunch_launch_busy");
    // Neither refused request finished the reservation it never owned.
    const auto again = proc::proc.capture_session_launch_generation();
    if (again && proc::proc.try_begin_session_launch(*again)) {
      proc::proc.finish_session_launch();
    } else {
      still_held = true;
    }
  }
  EXPECT_TRUE(still_held) << "a refused admission released another request's reservation";

  // A stop in progress refuses them the same way.
  ASSERT_TRUE(proc::proc.begin_session_stop_for_tests());
  {
    auto finish_stop = util::fail_guard([]() {
      proc::proc.finish_session_stop_for_tests(false);
    });
    EXPECT_EQ(console.request("POST", "/api/apps/launch", nlohmann::json {{"uuid", pinned_private().uuid}}.dump()).second.value("error_code", std::string {}),
      "prelaunch_launch_busy");
  }
  EXPECT_TRUE(adapter.starts.empty()) << "no transition without this request's own admission";
  EXPECT_EQ(host_double.scans.load(), 0);
  EXPECT_FALSE(log.has("launch_as"));
}

TEST_F(LaunchDesktopSteamTest, TheAppKeyIsStrictInTheRealLoaderAndValidator) {
  const auto path = directory / "prelaunch-loader-apps.json";
  const std::vector<std::pair<std::string, nlohmann::json>> values {
    {"absent", nullptr}, {"false", false}, {"true", true}, {"string", "true"}, {"number", 1}, {"null", nullptr},
  };
  nlohmann::json apps = nlohmann::json::array();
  for (std::size_t index = 0; index < values.size(); ++index) {
    nlohmann::json app {{"name", values[index].first}, {"uuid", "6E7F0A1B-2C3D-4E5F-8A9B-" + std::string(11, '0') + std::to_string(index)}};
    if (values[index].first != "absent") app[std::string {prelaunch::k_app_key}] = values[index].second;
    apps.push_back(app);
  }
  ASSERT_EQ(file_handler::write_file(path.string().c_str(), nlohmann::json {{"version", 15}, {"apps", apps}}.dump()), 0);
  const auto parsed = proc::parse(path.string());
  ASSERT_TRUE(parsed);
  const auto parsed_apps = parsed->get_apps();
  for (const auto &[label, value] : values) {
    const auto fixture = std::find_if(parsed_apps.begin(), parsed_apps.end(), [&](const auto &app) { return app.name == label; });
    ASSERT_NE(fixture, parsed_apps.end()) << label << ": every fixture must reach the actual loader assertions";
  }
  for (const auto &app : parsed_apps) {
    EXPECT_EQ(app.leave_game_mode_before_launch, app.name == "true") << app.name << ": only JSON true turns it on";
  }

  for (const auto &[label, value] : values) {
    if (label == "absent") continue;
    std::string error;
    const bool accepted = confighttp::validation::validate_app_payload(
      {{"name", "Fixture"}, {std::string {prelaunch::k_app_key}, value}}, error);
    EXPECT_EQ(accepted, value.is_boolean()) << label;
    if (!accepted) EXPECT_NE(error.find(std::string {prelaunch::k_app_key}), std::string::npos) << label << ": " << error;
  }
}

TEST_F(LaunchDesktopSteamTest, ConsoleReadinessIsResponseOnlyAndAnEditorRoundTripStoresOnlyTheSetting) {
  // This metadata/round-trip case owns an unavailable build, independent of
  // whether the compiled production adapter supports SteamOS on this platform.
  unavailable_build_t unavailable;
  const auto path = directory / "prelaunch-roundtrip-apps.json";
  const std::string opted_uuid = "5A6B7C8D-9E0F-4A1B-8C2D-3E4F5A6B7C8D";
  const std::string off_uuid = "1B2C3D4E-5F6A-4B7C-8D9E-0F1A2B3C4D5E";
  ASSERT_EQ(file_handler::write_file(path.string().c_str(), nlohmann::json {{"version", 15}, {"apps", {
    {{"name", "Opted"}, {"uuid", opted_uuid}, {std::string {prelaunch::k_app_key}, true}},
    {{"name", "Off"}, {"uuid", off_uuid}},
  }}}.dump()), 0);
  console_routes_t console {host.cert.string(), host.key.string(), path};

  const auto [listed_status, listed] = console.request("GET", "/api/apps");
  ASSERT_EQ(listed_status, 200);
  nlohmann::json opted;
  for (const auto &app : listed.at("apps")) {
    if (app.value("uuid", "") == off_uuid) EXPECT_FALSE(app.contains("leave-game-mode-before-launch-readiness"));
    if (app.value("uuid", "") == opted_uuid) opted = app;
  }
  ASSERT_TRUE(opted.contains("leave-game-mode-before-launch-readiness"));
  const auto readiness = opted.at("leave-game-mode-before-launch-readiness");
  EXPECT_TRUE(readiness.at("enabled").get<bool>());
  EXPECT_FALSE(readiness.at("adapter_implemented").get<bool>()) << "never presented as working";
  EXPECT_FALSE(readiness.at("ready").get<bool>());

  // An editor sends the listed app back, readiness included.
  const auto [saved_status, saved] = console.request("POST", "/api/apps", opted.dump());
  EXPECT_EQ(saved_status, 200) << saved.dump();
  const auto stored = nlohmann::json::parse(file_handler::read_file(path.string().c_str()));
  bool found = false;
  for (const auto &app : stored.at("apps")) {
    if (app.value("uuid", "") != opted_uuid) continue;
    found = true;
    EXPECT_FALSE(app.contains("leave-game-mode-before-launch-readiness")) << "response-only data was stored";
    EXPECT_EQ(app.value(std::string {prelaunch::k_app_key}, false), true);
  }
  EXPECT_TRUE(found);
  // The save reloads the apps through the real loader.
  const auto apps = proc::proc.get_apps();
  const auto loaded = std::find_if(apps.begin(), apps.end(), [&](const auto &app) { return app.uuid == opted_uuid; });
  ASSERT_NE(loaded, apps.end());
  EXPECT_TRUE(loaded->leave_game_mode_before_launch);

  auto bad = opted;
  bad[std::string {prelaunch::k_app_key}] = "true";
  EXPECT_EQ(console.request("POST", "/api/apps", bad.dump()).first, 400) << "a string is refused at the API";
}

TEST_F(LaunchDesktopSteamTest, AStalledOwnedRestoreFencesEveryAdmissionOfTheRealLifecycleGate) {
  stage_log_t log;
  session_answer_host_t host_double;
  session_answer_adapter_t adapter;
  instant_clock_t clock;
  prelaunch::coordinator_t coordinator {host_double, adapter, clock};
  platf::game_mode_host::set_session_live_for_tests(true);
  auto request = prelaunch::request_t {};
  request.policy_enabled = true;
  request.client_uuid = "client-a";
  request.capture_ready = [](std::string &) {
    return true;
  };
  request.others_now = [](std::string &) {
    return false;
  };
  const auto ready = coordinator.run(request);
  ASSERT_TRUE(ready.proceed()) << ready.code;
  coordinator.commit(ready.transition_generation);

  proc::session_lifecycle_gate_t gate;
  const prelaunch::admission_fence_t fence {
    [&gate]() {
      return gate.try_fence_restore();
    },
    [&gate]() {
      gate.release_restore_fence();
    },
  };
  std::promise<void> unstall;
  auto unstalled = unstall.get_future().share();
  const prelaunch::coordinator_t::others_t stalled = [unstalled](std::string &) {
    unstalled.wait();
    return false;
  };
  const auto generation = gate.capture_launch_generation();
  ASSERT_TRUE(generation);
  ASSERT_EQ(coordinator.on_session_end(ready.transition_generation, true, false, stalled, fence), prelaunch::restore_e::owned);

  // While the owned restore stalls, no admission of any app gets through the gate.
  EXPECT_TRUE(gate.restore_fenced());
  EXPECT_FALSE(gate.capture_launch_generation()) << "the generation a launch would reserve is withheld";
  EXPECT_FALSE(gate.try_begin_rtsp_launch()) << "a policy-off launch is refused";
  EXPECT_FALSE(gate.try_begin_rtsp_launch(*generation));
  EXPECT_FALSE(gate.try_begin_rtsp_setup(*generation));
  std::atomic<bool> blocking_launch_admitted {false};
  std::thread blocking([&]() {
    gate.begin_launch();
    blocking_launch_admitted = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds {100});
  EXPECT_FALSE(blocking_launch_admitted) << "a blocking launch waits for the restore";

  unstall.set_value();
  coordinator.wait_for_restore();
  blocking.join();
  EXPECT_TRUE(blocking_launch_admitted);
  EXPECT_FALSE(gate.restore_fenced());
  EXPECT_EQ(coordinator.receipt().restore, prelaunch::restore_e::restored);
  gate.finish_launch();

  // The other order: a launch admitted first refuses the claim, and nothing is fenced or run.
  platf::game_mode_host::set_session_live_for_tests(true);
  const auto again = coordinator.run(request);
  ASSERT_TRUE(again.proceed()) << again.code;
  coordinator.commit(again.transition_generation);
  ASSERT_TRUE(gate.try_begin_rtsp_launch());
  EXPECT_EQ(coordinator.on_session_end(again.transition_generation, true, false, stalled, fence), prelaunch::restore_e::refused);
  EXPECT_FALSE(gate.restore_fenced());
  gate.finish_launch();
  EXPECT_EQ(adapter.starts.size(), 3u) << "leave, return, leave: the refused claim ran nothing";
  platf::game_mode_host::set_session_live_for_tests(false);
}
#endif


// Real TLS verification and the production read-only handler: query parameters
// cannot substitute for the actual authenticated connection's endpoints.
TEST_F(LaunchDesktopSteamTest, PairedNetworkEvidenceDescribesOnlyItsOwnControlConnection) {
  pair_moonlight(false, crypto::PERM::_default);
  const auto [status, body] = network_reply(moonlight);
  ASSERT_TRUE(status.starts_with("200"));
  const auto evidence = nlohmann::json::parse(body);
  EXPECT_EQ(evidence.at("schema_version"), 1);
  EXPECT_EQ(evidence.at("status"), "unknown");
  EXPECT_EQ(evidence.at("tailscale").at("scope"), "control_connection");
  EXPECT_EQ(evidence.at("tailscale").at("confirmed"), false);
  EXPECT_EQ(body.find("100.64.0.2"), std::string::npos);
  EXPECT_EQ(body.find("private.example.ts.net"), std::string::npos);
}

TEST_F(LaunchDesktopSteamTest, PairedNetworkEvidenceRequiresViewPermission) {
  pair_moonlight(false, crypto::PERM::_reserved);
  const auto [status, body] = network_reply(moonlight);
  EXPECT_TRUE(status.starts_with("403"));
  EXPECT_EQ(body.find("tailscale"), std::string::npos);
}

TEST_F(LaunchDesktopSteamTest, UnpairedCertificateCannotReadNetworkEvidence) {
  pair_moonlight(false);
  const auto [status, body] = network_reply(stranger);
  EXPECT_TRUE(status.starts_with("401"));
  EXPECT_EQ(body.find("tailscale"), std::string::npos);
}

namespace {
  /// The daemon's status as the test route's TLS connection sees it: both ends are 127.0.0.1.
  std::string loopback_tailnet_status(std::string_view self_ip = "127.0.0.1", std::string_view peer_ip = "127.0.0.1") {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc {};
    gmtime_r(&now, &utc);
    char written[32] {};
    std::strftime(written, sizeof(written), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return nlohmann::json {
      {"BackendState", "Running"}, {"TailscaleIPs", {self_ip}},
      {"Peer", {{"nodekey:fixture", {{"TailscaleIPs", {peer_ip}}, {"Online", true}, {"Active", true},
        {"LastWrite", written}, {"CurAddr", "198.51.100.7:41641"}, {"Relay", "fixture-region"}, {"PeerRelay", ""},
        {"HostName", "fixture-host"}}}}},
    }.dump();
  }

  /// A daemon read that blocks until released and says when it has started.
  struct stalled_daemon_t {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, open = false;
    std::string body = loopback_tailnet_status();

    std::optional<std::string> read() {
      std::unique_lock lock {mutex};
      entered = true;
      changed.notify_all();
      if (!changed.wait_for(lock, std::chrono::seconds(10), [this] { return open; })) return std::nullopt;
      return body;
    }

    bool wait_entered() {
      std::unique_lock lock {mutex};
      return changed.wait_for(lock, std::chrono::seconds(5), [this] { return entered; });
    }

    void release() {
      std::lock_guard lock {mutex};
      open = true;
      changed.notify_all();
    }
  };
}  // namespace

// The production handler on a real TLS connection: confirmation comes from that connection's own
// endpoints (both 127.0.0.1 here), never from the request's parameters.
TEST_F(LaunchDesktopSteamTest, PairedNetworkEvidenceConfirmsFromTheActualTlsEndpoints) {
  pair_moonlight(false, crypto::PERM::_default);
  tailscale_connection::set_default_status_reader_for_tests([] { return std::optional<std::string> {loopback_tailnet_status()}; });
  auto [status, body] = network_reply(moonlight);
  ASSERT_TRUE(status.starts_with("200")) << status;
  auto evidence = nlohmann::json::parse(body);
  EXPECT_EQ(evidence.at("schema_version"), 1);
  EXPECT_EQ(evidence.at("status"), "tailscale");
  EXPECT_EQ(evidence.at("tailscale").at("confirmed"), true);
  EXPECT_EQ(evidence.at("tailscale").at("scope"), "control_connection");
  EXPECT_EQ(evidence.at("tailscale").at("source"), "tailscaled_localapi");
  EXPECT_EQ(evidence.at("tailscale").at("transport"), "direct");
  EXPECT_EQ(evidence.at("tailscale").at("reason"), "observed");
  EXPECT_GT(evidence.at("tailscale").at("observed_at_ms").get<std::int64_t>(), 0);
  EXPECT_EQ(evidence.size(), 4u);
  for (const auto *secret : {"127.0.0.1", "198.51.100.7", "nodekey", "fixture-host", "fixture-region", "100.64.0.2", "private.example.ts.net"})
    EXPECT_EQ(body.find(secret), std::string::npos) << secret;

  // The host's own tailnet address is not the address this connection reached: unknown.
  tailscale_connection::set_default_status_reader_for_tests([] { return std::optional<std::string> {loopback_tailnet_status("100.64.0.1")}; });
  std::tie(status, body) = network_reply(moonlight);
  EXPECT_EQ(nlohmann::json::parse(body).at("status"), "unknown");
  // The peer the query names (100.64.0.2) is not the peer that connected: unknown.
  tailscale_connection::set_default_status_reader_for_tests([] { return std::optional<std::string> {loopback_tailnet_status("127.0.0.1", "100.64.0.2")}; });
  std::tie(status, body) = network_reply(moonlight);
  EXPECT_EQ(nlohmann::json::parse(body).at("status"), "unknown");
  // A stopped daemon says so.
  tailscale_connection::set_default_status_reader_for_tests([] {
    auto stopped = nlohmann::json::parse(loopback_tailnet_status());
    stopped["BackendState"] = "Stopped";
    return std::optional<std::string> {stopped.dump()};
  });
  std::tie(status, body) = network_reply(moonlight);
  EXPECT_EQ(nlohmann::json::parse(body).at("tailscale").at("reason"), "localapi_unavailable");
}

// F1: a daemon that does not answer must not hold the paired server. While one client's evidence
// request waits on the daemon, another request on the same server is answered.
TEST_F(LaunchDesktopSteamTest, StalledDaemonDoesNotHoldThePairedServer) {
  pair_moonlight(true, crypto::PERM::_default);
  stalled_daemon_t daemon;
  tailscale_connection::set_default_status_reader_for_tests([&daemon] { return daemon.read(); });
  auto release = util::fail_guard([&] { daemon.release(); });

  auto evidence = network_reply_async(moonlight);
  ASSERT_TRUE(daemon.wait_entered());
  const auto started = std::chrono::steady_clock::now();
  const auto refused = launch(moonlight, moonlight_launch(steam_game_id));
  EXPECT_EQ(refused.status, 403);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
  EXPECT_EQ(evidence.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout)
    << "the evidence request should still be waiting on the daemon";

  daemon.release();
  release.disable();
  ASSERT_EQ(evidence.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  const auto [status, body] = evidence.get();
  ASSERT_TRUE(status.starts_with("200")) << status;
  EXPECT_EQ(nlohmann::json::parse(body).at("status"), "tailscale");
}

// A device whose view permission is withdrawn while the daemon is read gets no evidence either.
TEST_F(LaunchDesktopSteamTest, ViewPermissionWithdrawnDuringTheReadGetsNoEvidence) {
  pair_moonlight(false, crypto::PERM::_default);
  stalled_daemon_t daemon;
  tailscale_connection::set_default_status_reader_for_tests([&daemon] { return daemon.read(); });
  auto release = util::fail_guard([&] { daemon.release(); });

  auto evidence = network_reply_async(moonlight);
  ASSERT_TRUE(daemon.wait_entered());
  // The same device, paired again without view permission.
  nvhttp::reset_pairing_state_for_tests();
  pair_moonlight(false, crypto::PERM::_reserved);
  daemon.release();
  release.disable();
  ASSERT_EQ(evidence.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  const auto [status, body] = evidence.get();
  EXPECT_TRUE(status.starts_with("403")) << status;
  EXPECT_EQ(body.find("tailscale"), std::string::npos);
}

// Authorization is checked again before the answer is written: a device unpaired while the daemon
// was being read gets no evidence.
TEST_F(LaunchDesktopSteamTest, DeviceUnpairedDuringTheReadGetsNoEvidence) {
  pair_moonlight(false, crypto::PERM::_default);
  stalled_daemon_t daemon;
  tailscale_connection::set_default_status_reader_for_tests([&daemon] { return daemon.read(); });
  auto release = util::fail_guard([&] { daemon.release(); });

  auto evidence = network_reply_async(moonlight);
  ASSERT_TRUE(daemon.wait_entered());
  nvhttp::reset_pairing_state_for_tests();
  daemon.release();
  release.disable();
  ASSERT_EQ(evidence.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  const auto [status, body] = evidence.get();
  EXPECT_TRUE(status.starts_with("401")) << status;
  EXPECT_EQ(body.find("tailscale"), std::string::npos);
}
