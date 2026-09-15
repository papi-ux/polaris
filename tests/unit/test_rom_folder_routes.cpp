/**
 * @file tests/unit/test_rom_folder_routes.cpp
 * @brief The ROM folder routes end to end: register a folder, scan it, import from it.
 */
#include "../tests_common.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/private_state_file.h"
#include "src/process.h"

#include <Simple-Web-Server/client_https.hpp>
#include <Simple-Web-Server/server_https.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>

namespace confighttp {
  using resp_https_t = std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<SimpleWeb::Server<SimpleWeb::HTTPS>::Request>;
  void getLibrarySources(resp_https_t, req_https_t);
  void addLibrarySource(resp_https_t, req_https_t);
  void deleteLibrarySource(resp_https_t, req_https_t);
  void importGames(resp_https_t, req_https_t);
  nlohmann::json rom_folder_scan_for_tests(const std::set<std::string> &, const std::set<std::string> &);
  void with_web_session_for_tests(const std::filesystem::path &, const std::string &,
                                 const std::function<void(const std::string &)> &);
}  // namespace confighttp

namespace {
  namespace fs = std::filesystem;

  void touch(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << "x";
  }

  std::string read_text(const fs::path &path) {
    std::ifstream in {path, std::ios::binary};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }
}  // namespace

TEST(RomFolderRoutes, RegisterScanAndImportARomFolder) {
  const auto old_config = config::sunshine;
  const auto old_file_apps = config::stream.file_apps;
  const auto directory = fs::temp_directory_path() /
    ("polaris-rom-folders-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(directory);
  // The keys checks look under the home roots; point the runtime home at the temp directory
  // so this host's own emulator files cannot change the answer.
  const char *old_home_env = std::getenv("HOME");
  const std::string old_home = old_home_env ? old_home_env : "";
  setenv("HOME", directory.string().c_str(), 1);
  auto restore = util::fail_guard([&] {
    config::sunshine = old_config;
    config::stream.file_apps = old_file_apps;
    if (old_home_env) {
      setenv("HOME", old_home.c_str(), 1);
    } else {
      unsetenv("HOME");
    }
    fs::remove_all(directory);
  });

  const auto roms = directory / "roms" / "switch";
  touch(roms / "Game One (USA).nsp");
  touch(roms / "Game One [0100AAAA][v65536].nsp");  // an update dump, never an entry
  touch(roms / "notes.txt");
  touch(directory / "elsewhere" / "Other.nsp");
  touch(directory / "Eden.AppImage");
  const auto launcher = (directory / "Eden.AppImage").string();

  config::sunshine.username = "test-admin";
  config::sunshine.api_key = "isolated-test-api-key";
  config::sunshine.config_file = (directory / "polaris.conf").string();
  config::stream.file_apps = (directory / "apps.json").string();
  ASSERT_TRUE(private_state_file::write_atomic(config::stream.file_apps, R"({"env": {}, "apps": []})"));
  const auto credentials = crypto::gen_creds("localhost", 2048);
  ASSERT_TRUE(private_state_file::write_atomic(directory / "cert.pem", credentials.x509));
  ASSERT_TRUE(private_state_file::write_atomic(directory / "key.pem", credentials.pkey));

  confighttp::with_web_session_for_tests(directory / "sessions.json", "test-csrf", [&](const std::string &cookie) {
    SimpleWeb::Server<SimpleWeb::HTTPS> server((directory / "cert.pem").string(), (directory / "key.pem").string());
    server.config.address = "127.0.0.1";
    server.config.port = 0;
    server.config.timeout_request = 5;
    server.config.timeout_content = 5;
    server.resource["^/api/library/sources$"]["GET"] = confighttp::getLibrarySources;
    server.resource["^/api/library/sources$"]["POST"] = confighttp::addLibrarySource;
    server.resource["^/api/library/sources/([^/]+)$"]["DELETE"] = confighttp::deleteLibrarySource;
    server.resource["^/api/games/import$"]["POST"] = confighttp::importGames;
    std::atomic<unsigned short> port {0};
    std::jthread worker([&] { server.start([&](unsigned short assigned) { port = assigned; }); });
    auto stop = util::fail_guard([&] { server.stop(); worker.join(); });
    for (int attempt = 0; attempt < 100 && port == 0; ++attempt) std::this_thread::sleep_for(10ms);
    ASSERT_NE(port, 0);
    SimpleWeb::Client<SimpleWeb::HTTPS> client("127.0.0.1:" + std::to_string(port.load()), false);
    client.config.timeout = 5;
    auto request = [&](const std::string &method, const std::string &path, const std::string &content, bool authenticated = true) {
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      if (authenticated) {
        headers.emplace("Cookie", "auth=" + cookie);
      }
      return client.request(method, path, content, headers);
    };
    auto code = [](const auto &response) { return std::stoi(response->status_code); };
    auto body = [](const auto &response) { return nlohmann::json::parse(response->content.string()); };

    // Nothing without a session.
    EXPECT_EQ(code(request("GET", "/api/library/sources", "", false)), 401);

    // The presets come with where each emulator is on this host; nothing registered yet.
    auto listed = request("GET", "/api/library/sources", "");
    ASSERT_EQ(code(listed), 200);
    auto listing = body(listed);
    EXPECT_TRUE(listing["status"].get<bool>());
    EXPECT_TRUE(listing["sources"].empty());
    ASSERT_FALSE(listing["presets"].empty());
    EXPECT_EQ(listing["presets"][0]["id"], "eden");
    EXPECT_TRUE(listing["presets"][0].contains("install"));

    // A folder that is not there is refused with the reason.
    auto missing = request("POST", "/api/library/sources", nlohmann::json {{"path", (directory / "nope").string()}, {"emulator", "eden"}}.dump());
    EXPECT_EQ(code(missing), 400);
    EXPECT_NE(body(missing)["error"].get<std::string>().find("Folder not found"), std::string::npos);

    // Registered, with the launcher the user pointed at.
    auto added = request("POST", "/api/library/sources", nlohmann::json {{"path", roms.string() + "/"}, {"emulator", "eden"}, {"launcher", launcher}}.dump());
    ASSERT_EQ(code(added), 200);
    auto added_body = body(added);
    ASSERT_TRUE(added_body["status"].get<bool>());
    ASSERT_EQ(added_body["sources"].size(), 1u);
    const auto source_id = added_body["source"]["id"].get<std::string>();
    EXPECT_FALSE(source_id.empty());
    EXPECT_EQ(added_body["source"]["path"], roms.string());
    EXPECT_EQ(added_body["source"]["label"], "Eden");
    EXPECT_EQ(added_body["source"]["install"]["kind"], "launcher");
    EXPECT_EQ(added_body["source"]["install"]["location"], launcher);
    EXPECT_TRUE(added_body["source"]["warning"].get<std::string>().empty());
    EXPECT_TRUE(fs::exists(directory / "library_sources.json"));
    // Eden is installed (the launcher) but has no keys yet: the folder says so, with the fix.
    ASSERT_EQ(added_body["source"]["prerequisites"].size(), 1u) << added_body.dump();
    EXPECT_EQ(added_body["source"]["prerequisites"][0]["id"], "eden_keys_missing");
    EXPECT_EQ(added_body["source"]["prerequisites"][0]["severity"], "warning");
    touch(directory / ".local" / "share" / "eden" / "keys" / "prod.keys");
    auto relisted = body(request("GET", "/api/library/sources", ""));
    ASSERT_EQ(relisted["sources"].size(), 1u);
    EXPECT_TRUE(relisted["sources"][0]["prerequisites"].empty()) << relisted.dump();
    EXPECT_TRUE(relisted["presets"][0].contains("prerequisites"));

    // The same folder for the same emulator updates in place rather than duplicating.
    auto again = request("POST", "/api/library/sources", nlohmann::json {{"path", roms.string()}, {"emulator", "eden"}}.dump());
    ASSERT_EQ(code(again), 200);
    EXPECT_EQ(body(again)["sources"].size(), 1u);
    EXPECT_EQ(body(again)["source"]["id"], source_id);
    EXPECT_EQ(body(again)["source"]["install"]["kind"], "missing");
    EXPECT_NE(body(again)["source"]["warning"].get<std::string>().find("not installed"), std::string::npos);
    // Back to the launcher form for the rest of the test.
    ASSERT_EQ(code(request("POST", "/api/library/sources", nlohmann::json {{"path", roms.string()}, {"emulator", "eden"}, {"launcher", launcher}}.dump())), 200);

    // The scan: one candidate, the update dump and the text file skipped, the command rebuilt.
    const auto rom_path = (roms / "Game One (USA).nsp").string();
    const auto expected_command = "'" + launcher + "' -f -g '" + rom_path + "'";
    auto scan = confighttp::rom_folder_scan_for_tests({}, {});
    ASSERT_EQ(scan["emulator_games"].size(), 1u);
    const auto &candidate = scan["emulator_games"][0];
    EXPECT_EQ(candidate["name"], "Game One");
    EXPECT_EQ(candidate["source"], "emulator");
    EXPECT_EQ(candidate["emulator"], "eden");
    EXPECT_EQ(candidate["emulator_label"], "Eden");
    EXPECT_EQ(candidate["platform"], "Nintendo Switch");
    EXPECT_EQ(candidate["source_id"], source_id);
    EXPECT_EQ(candidate["rom_path"], rom_path);
    EXPECT_EQ(candidate["cmd"], expected_command);
    EXPECT_FALSE(candidate["already_imported"].get<bool>());
    ASSERT_EQ(scan["library_sources"].size(), 1u);
    EXPECT_EQ(scan["library_sources"][0]["rom_count"], 1);
    EXPECT_TRUE(confighttp::rom_folder_scan_for_tests({expected_command}, {})["emulator_games"][0]["already_imported"].get<bool>());

    // Import refuses a file outside the folder or one the emulator does not load.
    const auto import_body = [&](const std::string &rom, const std::string &id = "") {
      return nlohmann::json {{"games", nlohmann::json::array({{
        {"name", "Game One"}, {"source", "emulator"}, {"source_id", id.empty() ? source_id : id}, {"rom_path", rom}, {"cmd", "ignored"}, {"game_category", "unknown"}
      }})}}.dump();
    };
    EXPECT_EQ(code(request("POST", "/api/games/import", import_body((directory / "elsewhere" / "Other.nsp").string()))), 400);
    EXPECT_EQ(code(request("POST", "/api/games/import", import_body((roms / "notes.txt").string()))), 400);
    EXPECT_EQ(code(request("POST", "/api/games/import", import_body(rom_path, "not-a-folder"))), 400);

    // A cover beside the game is copied in at import, so the entry has art without any key.
    {
      std::ofstream png(roms / "Game One (USA).png", std::ios::binary);
      png << "\x89PNG\r\n\x1a\n" << std::string(16, '\0') << "IHDR" << std::string(32, '\0');
    }

    // The import writes the entry from the folder, not from the browser, and publishes Eden once.
    auto imported = request("POST", "/api/games/import", import_body(rom_path));
    ASSERT_EQ(code(imported), 200);
    EXPECT_EQ(body(imported)["imported"], 1);
    auto apps = nlohmann::json::parse(read_text(config::stream.file_apps))["apps"];
    ASSERT_EQ(apps.size(), 2u);
    const auto &game = apps[0];
    EXPECT_EQ(game["name"], "Game One");
    EXPECT_EQ(game["cmd"], expected_command);
    EXPECT_EQ(game["source"], "emulator");
    EXPECT_EQ(game["emulator"], "eden");
    EXPECT_EQ(game["rom-folder"], source_id);
    EXPECT_EQ(game["rom-path"], rom_path);
    EXPECT_EQ(game["gamepad"], "switch");
    ASSERT_TRUE(game.contains("image-path")) << game.dump();
    const auto image_path = game["image-path"].get<std::string>();
    EXPECT_EQ(image_path.rfind((directory / "covers").string(), 0), 0u) << image_path;
    EXPECT_NE(image_path.find("emulator_eden_Game_One__USA__"), std::string::npos) << image_path;
    EXPECT_TRUE(fs::is_regular_file(image_path)) << image_path;
    EXPECT_FALSE(game["uuid"].get<std::string>().empty());
    const auto &eden = apps[1];
    EXPECT_EQ(eden["name"], "Eden");
    EXPECT_EQ(eden["cmd"], "'" + launcher + "'");
    EXPECT_EQ(eden["source"], "emulator");
    EXPECT_FALSE(eden.contains("rom-path"));
    // The emulator is the game: a fast death ends the session, and a save gets its time.
    EXPECT_FALSE(game["auto-detach"].get<bool>());
    EXPECT_EQ(game["exit-timeout"], 10);
    EXPECT_TRUE(eden["auto-detach"].get<bool>());

    // The import refreshed the running app list, so the entry knows what it is.
    const auto loaded = proc::proc.get_apps();
    const auto entry = std::find_if(loaded.begin(), loaded.end(), [](const proc::ctx_t &candidate) {
      return candidate.name == "Game One";
    });
    ASSERT_NE(entry, loaded.end());
    EXPECT_EQ(entry->source, "emulator");
    EXPECT_EQ(entry->emulator, "eden");
    EXPECT_EQ(entry->rom_path, rom_path);
    EXPECT_EQ(entry->gamepad, "switch");

    // Importing it again is a no-op, and the scan now reports it as imported.
    auto repeat = request("POST", "/api/games/import", import_body(rom_path));
    ASSERT_EQ(code(repeat), 200);
    EXPECT_EQ(body(repeat)["imported"], 0);
    EXPECT_EQ(nlohmann::json::parse(read_text(config::stream.file_apps))["apps"].size(), 2u);
    EXPECT_TRUE(confighttp::rom_folder_scan_for_tests({}, {rom_path})["emulator_games"][0]["already_imported"].get<bool>());

    // A custom template runs without a shell, so ~/ is expanded when the command is built.
    touch(directory / "roms" / "snes" / "Game Two.sfc");
    auto custom = request("POST", "/api/library/sources", nlohmann::json {
      {"path", (directory / "roms" / "snes").string()},
      {"emulator", "custom"},
      {"command", "'~/fake emu' -L ~/cores/snes9x_libretro.so {rom}"},
      {"extensions", "sfc, smc"}
    }.dump());
    ASSERT_EQ(code(custom), 200);
    EXPECT_EQ(body(custom)["sources"].size(), 2u);
    auto custom_scan = confighttp::rom_folder_scan_for_tests({}, {});
    ASSERT_EQ(custom_scan["emulator_games"].size(), 2u);
    const auto game_two = std::find_if(custom_scan["emulator_games"].begin(), custom_scan["emulator_games"].end(), [](const nlohmann::json &entry) {
      return entry["name"] == "Game Two";
    });
    ASSERT_NE(game_two, custom_scan["emulator_games"].end());
    const auto custom_command = (*game_two)["cmd"].get<std::string>();
    EXPECT_EQ(custom_command.find("~/"), std::string::npos) << custom_command;
    EXPECT_EQ(custom_command.rfind("'/", 0), 0u) << custom_command;
    EXPECT_NE(custom_command.find("/cores/snes9x_libretro.so '" + (directory / "roms" / "snes" / "Game Two.sfc").string() + "'"), std::string::npos) << custom_command;
    EXPECT_EQ((*game_two)["emulator_label"], "Custom command");
    const auto custom_id = body(custom)["source"]["id"].get<std::string>();
    ASSERT_EQ(code(request("DELETE", "/api/library/sources/" + custom_id, "")), 200);

    // Removal leaves the imported entries alone.
    auto unknown = request("DELETE", "/api/library/sources/nope", "");
    ASSERT_EQ(code(unknown), 200);
    EXPECT_FALSE(body(unknown)["status"].get<bool>());
    auto removed = request("DELETE", "/api/library/sources/" + source_id, "");
    ASSERT_EQ(code(removed), 200);
    EXPECT_TRUE(body(removed)["status"].get<bool>());
    EXPECT_TRUE(body(removed)["sources"].empty());
    EXPECT_TRUE(body(request("GET", "/api/library/sources", ""))["sources"].empty());
    EXPECT_EQ(nlohmann::json::parse(read_text(config::stream.file_apps))["apps"].size(), 2u);
  });
}
