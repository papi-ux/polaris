#include "../tests_common.h"
#include <src/process.h>
#include <src/steam_lifetime.h>
#include <boost/process/v1/env.hpp>
#include <src/confighttp_validation.h>
#include <src/file_handler.h>
#include "../tests_paths.h"
#ifdef __linux__
#include <src/platform/linux/game_mode_host.h>
#endif

TEST(DesktopSteamLifetime, OptInAndApplicationOverride) {
  EXPECT_FALSE(proc::keep_desktop_steam("inherit", false, "host_virtual_display", false));
  EXPECT_TRUE(proc::keep_desktop_steam("inherit", true, "host_virtual_display", false));
  EXPECT_TRUE(proc::keep_desktop_steam("keep", false, "desktop_display", false));
  EXPECT_FALSE(proc::keep_desktop_steam("close", true, "desktop_display", false));
  EXPECT_THROW(proc::normalize_desktop_steam_lifetime("unknown"), std::invalid_argument);
}

TEST(DesktopSteamLifetime, StandaloneSteamLaunchCanOutliveTheStream) {
  for (const auto command : {"steam -gamepadui", "setsid steam steam://open/bigpicture",
                            "setsid -f steam steam://rungameid/620", "steam -applaunch 620"})
    EXPECT_TRUE(proc::standalone_desktop_steam_launch(command)) << command;
}

TEST(DesktopSteamLifetime, ShellFollowupsAndUnrelatedCommandsRemainStreamOwned) {
  for (const auto command : {"setsid bash -lc \"sleep 6; steam steam://rungameid/620\"",
                            "setsid steam -gamepadui && sleep 600",
                            "echo steam://open/bigpicture", "steam steam://rungameid/620; sleep 600",
                            "steam -shutdown", "sleep 600"})
    EXPECT_FALSE(proc::standalone_desktop_steam_launch(command)) << command;
}

TEST(DesktopSteamLifetime, PrivateAndGameModeNeverBorrowDesktopPolicy) {
  for (const auto mode : {"headless_stream", "windowed_stream", "gamescope_stream", "desktop_takeover", "headless_dongle"})
    EXPECT_FALSE(proc::keep_desktop_steam("keep", true, mode, false));
  EXPECT_FALSE(proc::keep_desktop_steam("keep", true, "host_virtual_display", true));
}

TEST(DesktopSteamControl, WatchPermissionCannotReadOrMutateSteam) {
  proc::proc_t host {boost::process::v1::environment {}, {}};
  for (const auto action : {"", "open", "big_picture", "quit"}) {
    const auto [status, body] = host.desktop_steam_control("viewer", false, action, "revision", true);
    EXPECT_EQ(status, 403);
    EXPECT_EQ(body.at("state"), "unavailable");
    EXPECT_EQ(body.at("code"), "permission_denied");
  }
}

TEST(DesktopSteamLifetime, AppEditorRejectsInvalidPolicyTypesAndValues) {
  for (const auto policy : {"inherit", "keep", "close"}) {
    std::string error;
    EXPECT_TRUE(confighttp::validation::validate_app_payload({{"name", "Steam"}, {"desktop-steam-lifetime", policy}}, error)) << error;
  }
  for (const nlohmann::json &value : {nlohmann::json(true), nlohmann::json("restart"), nlohmann::json(3)}) {
    std::string error;
    EXPECT_FALSE(confighttp::validation::validate_app_payload({{"name", "Steam"}, {"desktop-steam-lifetime", value}}, error));
  }
}

TEST(DesktopSteamLifetime, LoadedAppsDistinguishManagedCleanupFromCustomUndo) {
  const auto path = test_paths::root() / "desktop_steam_lifetime.json";
  const nlohmann::json data {{"version", 15}, {"apps", {
    {{"name", "Steam Big Picture"}, {"uuid", "11111111-1111-4111-8111-111111111111"}, {"detached", {"setsid steam steam://open/bigpicture"}}, {"desktop-steam-lifetime", "keep"}},
    {{"name", "Custom Steam"}, {"uuid", "22222222-2222-4222-8222-222222222222"}, {"steam-appid", "620"}, {"desktop-steam-lifetime", "close"},
     {"prep-cmd", {{{"undo", "notify-send done; steam -shutdown"}}}}}
  }}};
  ASSERT_EQ(file_handler::write_file(path.string().c_str(), data.dump()), 0);
  auto parsed = proc::parse(path.string());
  ASSERT_TRUE(parsed);
  const auto apps = parsed->get_apps();
  const auto managed = std::find_if(apps.begin(), apps.end(), [](const auto &a) { return a.name == "Steam Big Picture"; });
  const auto custom = std::find_if(apps.begin(), apps.end(), [](const auto &a) { return a.name == "Custom Steam"; });
  ASSERT_NE(managed, apps.end()); ASSERT_NE(custom, apps.end());
  EXPECT_EQ(managed->desktop_steam_lifetime, "keep");
  ASSERT_EQ(managed->prep_cmds.size(), 1);
  EXPECT_TRUE(managed->prep_cmds.front().managed_steam_cleanup);
  EXPECT_EQ(custom->desktop_steam_lifetime, "close");
  ASSERT_EQ(custom->prep_cmds.size(), 1);
  EXPECT_FALSE(custom->prep_cmds.front().managed_steam_cleanup);
  EXPECT_EQ(custom->prep_cmds.front().undo_cmd, "notify-send done; steam -shutdown");
  std::filesystem::remove(path);
}

#ifdef __linux__
TEST(DesktopSteamControl, RefusesOtherOwnersPrivateSessionsAndTransitionsBeforeObservingSteam) {
  platf::game_mode_host::set_session_live_for_tests(false);
  proc::proc_t host {boost::process::v1::environment {}, {}};
  proc::ctx_t app;
  auto launch = std::make_shared<rtsp_stream::launch_session_t>();
  launch->unique_id = "owner"; launch->expected_stream_mode = "host_virtual_display";
  host.set_active_launch_for_tests(app, launch);
  EXPECT_EQ(host.desktop_steam_control("other", true).second.at("code"), "session_not_owned");
  launch->expected_stream_mode = "headless_stream";
  EXPECT_EQ(host.desktop_steam_control("owner", true, "quit", "revision", true).second.at("code"), "private_runtime");
  platf::game_mode_host::set_session_live_for_tests(true);
  EXPECT_EQ(host.desktop_steam_control("owner", true).second.at("code"), "game_mode");
  platf::game_mode_host::set_session_live_for_tests(false);
  ASSERT_TRUE(host.begin_session_stop_for_tests());
  EXPECT_EQ(host.desktop_steam_control("owner", true).second.at("code"), "session_busy");
  host.finish_session_stop_for_tests(false);
  host.clear_running_app_for_tests();
  platf::game_mode_host::set_session_live_for_tests(std::nullopt);
}
#endif

#ifdef __linux__
TEST(DesktopSteamLifetime, ExactCleanupLeavesDesktopSteamAndItsRoutingAlive) {
  const auto token = "desktop-steam-test-" + std::to_string(getpid());
  auto session_env = boost::this_process::environment();
  session_env["POLARIS_SESSION_INSTANCE_ID"] = token;
  session_env["PULSE_SINK"] = "fixture-stream-sink";
  auto desktop_env = proc::desktop_steam_environment(session_env);
  EXPECT_EQ(desktop_env.find("POLARIS_SESSION_INSTANCE_ID"), desktop_env.end());
  EXPECT_EQ(desktop_env.find("POLARIS_PRIVATE_SESSION"), desktop_env.end());
  ASSERT_NE(desktop_env.find("PULSE_SINK"), desktop_env.end());
  EXPECT_EQ(desktop_env["PULSE_SINK"].to_string(), "fixture-stream-sink");
  boost::process::v1::child desktop("/bin/sleep", "30", desktop_env);
  boost::process::v1::child transient("/bin/sleep", "30", session_env);
  // Real kernel processes stand in for desktop Steam and an ordinary detached
  // session app. The existing generation sweep must only stop the latter.
  EXPECT_TRUE(proc::non_cage_detached_generation_cleanup_for_tests(token, transient.id()));
  transient.detach();
  EXPECT_TRUE(desktop.running());
  desktop.terminate(); desktop.wait();
}
#endif

#ifdef __linux__
TEST(DesktopSteamControl, IdleProcessDoesNotInventAnActiveStreamOwner) {
  platf::game_mode_host::set_session_live_for_tests(false);
  proc::proc_t host {boost::process::v1::environment {}, {}};
  const auto [status, body] = host.desktop_steam_control("paired-client", true);
  // This is a read only probe. A host without visible Steam may report unknown
  // ownership, but an idle proc_t must never invent someone else's session.
  EXPECT_NE(body.value("code", ""), "session_not_owned");
  EXPECT_NE(body.value("code", ""), "private_runtime");
  EXPECT_NE(status, 403);
  platf::game_mode_host::set_session_live_for_tests(std::nullopt);
}
#endif
