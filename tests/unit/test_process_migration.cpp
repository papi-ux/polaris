#include "../tests_common.h"
#include "../tests_paths.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <src/file_handler.h>
#include <src/process.h>

#ifdef __linux__
namespace {
  struct linux_cage_compositor_guard_t {
    linux_cage_compositor_guard_t():
        use_cage_compositor(config::video.linux_display.use_cage_compositor) {}

    ~linux_cage_compositor_guard_t() {
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
    }

    bool use_cage_compositor;
  };
}  // namespace
#endif

TEST(ProcessMigrationTests, ParseRepairsMalformedLegacyAppsJson) {
  const auto file_path = test_paths::root() / "legacy_apps_migration.json";

  const nlohmann::json legacy_apps = {
    {"version", 1},
    {"apps", {
      {
        {"name", "Legacy App"},
        {"uuid", 12345},
        {"allow-client-commands", "yes"},
        {"exclude-global-state-cmd", nlohmann::json::array({"off"})},
        {"auto-detach", nlohmann::json::array({"ON"})},
        {"wait-all", "YES"},
        {"terminate-on-pause", "true"},
        {"virtual-display", nlohmann::json::array({"on"})},
        {"last-launched", "1710000000"},
        {"exit-timeout", "bad"},
        {"scale-factor", "125"},
        {"prep-cmd", {{
          {"do", "echo hello"},
          {"elevated", nlohmann::json::array({"TRUE"})}
        }}},
        {"detached", {"echo ready"}}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), legacy_apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto migrated_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  ASSERT_TRUE(migrated_tree.contains("version"));
  EXPECT_EQ(migrated_tree["version"], 8);
  ASSERT_TRUE(migrated_tree.contains("apps"));
  ASSERT_TRUE(migrated_tree["apps"].is_array());
  ASSERT_EQ(migrated_tree["apps"].size(), 1);

  const auto &migrated_app = migrated_tree["apps"][0];
  EXPECT_TRUE(migrated_app["uuid"].is_string());
  EXPECT_TRUE(migrated_app["allow-client-commands"].is_boolean());
  EXPECT_TRUE(migrated_app["exclude-global-state-cmd"].is_boolean());
  EXPECT_TRUE(migrated_app["auto-detach"].is_boolean());
  EXPECT_TRUE(migrated_app["wait-all"].is_boolean());
  EXPECT_TRUE(migrated_app["terminate-on-pause"].is_boolean());
  EXPECT_TRUE(migrated_app["virtual-display"].is_boolean());
  EXPECT_TRUE(migrated_app["last-launched"].is_number_integer());
  EXPECT_TRUE(migrated_app["exit-timeout"].is_number_integer());
  EXPECT_TRUE(migrated_app["scale-factor"].is_number_integer());
  EXPECT_TRUE(migrated_app["prep-cmd"][0]["elevated"].is_boolean());
  EXPECT_EQ(migrated_app["exit-timeout"], 5);
  EXPECT_EQ(migrated_app["scale-factor"], 125);

  const auto &apps = parsed_proc->get_apps();
  const auto migrated_ctx = std::find_if(apps.begin(), apps.end(), [](const auto &app) {
    return app.name == "Legacy App";
  });

  ASSERT_NE(migrated_ctx, apps.end());
  EXPECT_FALSE(migrated_ctx->uuid.empty());
  EXPECT_TRUE(migrated_ctx->allow_client_commands);
  EXPECT_TRUE(migrated_ctx->auto_detach);
  EXPECT_TRUE(migrated_ctx->wait_all);
  EXPECT_TRUE(migrated_ctx->virtual_display);
  EXPECT_TRUE(migrated_ctx->terminate_on_pause);
  EXPECT_EQ(migrated_ctx->scale_factor, 125);
  EXPECT_EQ(migrated_ctx->exit_timeout, std::chrono::seconds(5));
  EXPECT_EQ(migrated_ctx->last_launched, 1710000000);

  std::filesystem::remove(file_path);
}

TEST(ProcessMigrationTests, ParseNormalizesSteamBigPictureLaunchAndAddsCleanupUndo) {
#ifdef __linux__
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = false;
#endif

  const auto file_path = test_paths::root() / "steam_big_picture_normalization.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Steam Big Picture"},
        {"uuid", "steam-big-picture-test"},
        {"cmd", ""},
        {"detached", {"setsid steam -gamepadui"}},
        {"prep-cmd", nlohmann::json::array()},
        {"env", {
          {"MANGOHUD", "1"},
          {"MANGOHUD_CONFIG", "fps_limit=60"}
        }},
        {"image-path", "./assets/steam.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Steam Big Picture";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->detached.size(), 1);
  EXPECT_EQ(steam_ctx->detached.front(), "setsid steam -gamepadui");
  EXPECT_TRUE(steam_ctx->cmd.empty());
  ASSERT_FALSE(steam_ctx->prep_cmds.empty());
  EXPECT_EQ(steam_ctx->prep_cmds.back().undo_cmd, "setsid steam -shutdown");
  EXPECT_TRUE(steam_ctx->env_vars.empty());

  std::filesystem::remove(file_path);
}

#ifdef __linux__
TEST(ProcessMigrationTests, ParseDoesNotAddSteamBigPicturePrelaunchShutdownForLinuxCage) {
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = true;

  const auto file_path = test_paths::root() / "steam_big_picture_cage_prelaunch_shutdown.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Steam Big Picture"},
        {"uuid", "steam-big-picture-cage-prelaunch"},
        {"cmd", ""},
        {"detached", {"setsid steam -gamepadui"}},
        {"prep-cmd", nlohmann::json::array()},
        {"image-path", "./assets/steam.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Steam Big Picture";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->prep_cmds.size(), 1);
  EXPECT_TRUE(steam_ctx->prep_cmds.front().do_cmd.empty());
  EXPECT_EQ(steam_ctx->prep_cmds.front().undo_cmd, "setsid steam -shutdown");

  std::filesystem::remove(file_path);
}
#endif

TEST(ProcessMigrationTests, ParseStripsSteamBigPictureMangoHudEvenWithExistingCleanupUndo) {
#ifdef __linux__
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = false;
#endif

  const auto file_path = test_paths::root() / "steam_big_picture_existing_cleanup.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Steam Big Picture"},
        {"uuid", "steam-big-picture-existing-cleanup"},
        {"cmd", ""},
        {"detached", {"setsid steam steam://open/bigpicture"}},
        {"prep-cmd", {{
          {"undo", "setsid steam steam://close/bigpicture"}
        }}},
        {"env", {
          {"MANGOHUD", "1"},
          {"MANGOHUD_DLSYM", "1"},
          {"MANGOHUD_CONFIG", "fps_limit=60"}
        }},
        {"image-path", "./assets/steam.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Steam Big Picture";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->prep_cmds.size(), 1);
  EXPECT_EQ(steam_ctx->prep_cmds.front().undo_cmd, "setsid steam -shutdown");
  EXPECT_TRUE(steam_ctx->env_vars.empty());

  std::filesystem::remove(file_path);
}

TEST(ProcessMigrationTests, ParseNormalizesSteamLibraryLaunchAndAddsShutdownUndo) {
#ifdef __linux__
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = false;
#endif

  const auto file_path = test_paths::root() / "steam_library_launch_normalization.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Indiana Jones and the Great Circle"},
        {"uuid", "steam-library-normalization-test"},
        {"cmd", ""},
        {"detached", {"setsid steam steam://rungameid/2677660"}},
        {"prep-cmd", nlohmann::json::array()},
        {"source", "steam"},
        {"steam-appid", "2677660"},
        {"image-path", "./assets/indiana.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Indiana Jones and the Great Circle";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
#ifdef __linux__
  ASSERT_EQ(steam_ctx->detached.size(), 2);
  EXPECT_EQ(steam_ctx->detached.front(), "setsid steam -gamepadui");
  EXPECT_EQ(steam_ctx->detached[1], "setsid bash -lc \"sleep 6; steam steam://rungameid/2677660 >/dev/null 2>&1 || true; sleep 4; exec steam -applaunch 2677660 >/dev/null 2>&1 || true\"");
#else
  ASSERT_EQ(steam_ctx->detached.size(), 1);
  EXPECT_EQ(steam_ctx->detached.front(), "setsid steam steam://rungameid/2677660");
#endif
  ASSERT_FALSE(steam_ctx->prep_cmds.empty());
  EXPECT_EQ(steam_ctx->prep_cmds.back().undo_cmd, "setsid steam -shutdown");
  EXPECT_EQ(steam_ctx->steam_appid, "2677660");
  EXPECT_EQ(steam_ctx->source, "steam");

  const auto migrated_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  EXPECT_EQ(migrated_tree["version"], 8);

  std::filesystem::remove(file_path);
}

#ifdef __linux__
TEST(ProcessMigrationTests, ParseDoesNotAddSteamLibraryPrelaunchShutdownForLinuxCage) {
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = true;

  const auto file_path = test_paths::root() / "steam_library_cage_prelaunch_shutdown.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Indiana Jones and the Great Circle"},
        {"uuid", "steam-library-cage-prelaunch"},
        {"cmd", ""},
        {"detached", {"setsid steam steam://rungameid/2677660"}},
        {"prep-cmd", nlohmann::json::array()},
        {"source", "steam"},
        {"steam-appid", "2677660"},
        {"image-path", "./assets/indiana.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Indiana Jones and the Great Circle";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->prep_cmds.size(), 1);
  EXPECT_TRUE(steam_ctx->prep_cmds.front().do_cmd.empty());
  EXPECT_EQ(steam_ctx->prep_cmds.front().undo_cmd, "setsid steam -shutdown");

  std::filesystem::remove(file_path);
}

TEST(ProcessMigrationTests, ParsePreservesLutrisImportMetadataAndSource) {
  const auto file_path = test_paths::root() / "lutris_import_metadata.json";

  const nlohmann::json apps = {
    {"version", 8},
    {"apps", {{
      {"name", "Lutris Game"},
      {"uuid", "a4a9a18a-3898-4b34-9e3d-21a7a9647712"},
      {"source", "lutris"},
      {"lutris-slug", "lutris-game"},
      {"lutris-runner", "wine"},
      {"detached", {"setsid lutris lutris:rungame/lutris-game"}},
      {"gamepad", "ds5"},
      {"game-category", "cinematic"},
      {"auto-detach", true},
      {"wait-all", true},
      {"exit-timeout", 5}
    }}}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto migrated_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  ASSERT_TRUE(migrated_tree.contains("apps"));
  ASSERT_EQ(migrated_tree["apps"].size(), 1);
  const auto &migrated_app = migrated_tree["apps"][0];
  EXPECT_EQ(migrated_app["source"], "lutris");
  EXPECT_EQ(migrated_app["lutris-slug"], "lutris-game");
  EXPECT_EQ(migrated_app["lutris-runner"], "wine");

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto lutris_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Lutris Game";
  });

  ASSERT_NE(lutris_ctx, parsed_apps.end());
  EXPECT_EQ(lutris_ctx->source, "lutris");
  EXPECT_EQ(lutris_ctx->gamepad, "ds5");
  EXPECT_EQ(lutris_ctx->game_category, "cinematic");
  ASSERT_EQ(lutris_ctx->detached.size(), 1);
  EXPECT_EQ(lutris_ctx->detached[0], "setsid lutris lutris:rungame/lutris-game");
}

TEST(ProcessMigrationTests, ParseAddsLutrisLauncherWhenLutrisGamesExist) {
  const auto file_path = test_paths::root() / "lutris_launcher_migration.json";

  const nlohmann::json apps = {
    {"version", 7},
    {"apps", {{
      {"name", "Black Myth: Wukong"},
      {"uuid", "45cb1d9d-90d3-6023-0800-457901181759"},
      {"source", "lutris"},
      {"lutris-slug", "black-myth-wukong"},
      {"detached", {"setsid lutris lutris:rungame/black-myth-wukong"}},
      {"auto-detach", true},
      {"wait-all", true},
      {"exit-timeout", 5}
    }}}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto migrated_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  EXPECT_EQ(migrated_tree["version"], 8);
  ASSERT_TRUE(migrated_tree.contains("apps"));

  const auto &migrated_apps = migrated_tree["apps"];
  const auto lutris_app = std::find_if(migrated_apps.begin(), migrated_apps.end(), [](const auto &app) {
    return app.value("name", "") == "Lutris";
  });

  ASSERT_NE(lutris_app, migrated_apps.end());
  EXPECT_EQ((*lutris_app)["source"], "lutris");
  EXPECT_EQ((*lutris_app)["image-path"], "lutris.png");
  ASSERT_TRUE((*lutris_app).contains("detached"));
  ASSERT_EQ((*lutris_app)["detached"].size(), 1);
  EXPECT_EQ((*lutris_app)["detached"][0], "setsid lutris");

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto parsed_lutris = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Lutris";
  });

  ASSERT_NE(parsed_lutris, parsed_apps.end());
  ASSERT_EQ(parsed_lutris->detached.size(), 1);
  EXPECT_EQ(parsed_lutris->detached[0], "setsid lutris");

  std::filesystem::remove(file_path);
}
#endif
