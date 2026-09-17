/**
 * @file tests/unit/test_emulator_library.cpp
 * @brief ROM folder import: presets, names, commands, the folder scan and the source list.
 */
#include "../tests_common.h"
#include "../tests_paths.h"

#include <src/emulator_install.h>
#include <src/emulator_library.h>

#include <boost/program_options/parsers.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <set>

namespace {
  namespace fs = std::filesystem;

  fs::path fresh_root(const std::string &name) {
    const auto root = test_paths::root() / "emulator_library" / name;
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
  }

  void touch(const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path);
    file << "x";
  }

  std::vector<std::string> names_of(const std::vector<emulator_library::rom_t> &roms) {
    std::vector<std::string> names;
    for (const auto &rom : roms) {
      names.push_back(rom.name);
    }
    return names;
  }
}  // namespace

TEST(EmulatorLibraryPresets, EveryPresetIsCompleteAndUnique) {
  std::set<std::string_view> ids;
  for (const auto &preset : emulator_library::presets()) {
    EXPECT_TRUE(ids.insert(preset.id).second) << preset.id;
    EXPECT_FALSE(preset.label.empty()) << preset.id;
    EXPECT_FALSE(preset.platform.empty()) << preset.id;
    EXPECT_FALSE(preset.platform_id.empty()) << preset.id;
    EXPECT_FALSE(preset.es_systems.empty()) << preset.id;
    EXPECT_EQ(preset.platform_id.find(' '), std::string_view::npos) << preset.id;
    EXPECT_FALSE(preset.binaries.empty()) << preset.id;
    EXPECT_FALSE(preset.flatpak_id.empty()) << preset.id;
    EXPECT_FALSE(preset.extensions.empty()) << preset.id;
    // The path goes exactly where the emulator expects it, once.
    const auto arguments = preset.arguments.empty() ? emulator_library::rom_placeholder : preset.arguments;
    EXPECT_TRUE(emulator_library::custom_template_valid(arguments) || arguments == emulator_library::rom_placeholder) << preset.id;
    for (const auto extension : preset.extensions) {
      EXPECT_EQ(std::string(extension), emulator_library::lower_copy(extension)) << preset.id;
      EXPECT_EQ(extension.find('.'), std::string_view::npos) << preset.id;
    }
  }
  ASSERT_NE(emulator_library::find_preset("eden"), nullptr);
  EXPECT_EQ(emulator_library::find_preset("eden")->gamepad, "switch");
  EXPECT_EQ(emulator_library::find_preset("Eden"), nullptr);
  EXPECT_EQ(emulator_library::find_preset(""), nullptr);
}

TEST(EmulatorLibraryCommands, QuotedPathsSplitBackIntoOneArgument) {
  const auto quoted = emulator_library::shell_quote("/roms/it's here/Game (USA).nsp");
  EXPECT_EQ(quoted, "'/roms/it'\\''s here/Game (USA).nsp'");

  const auto *eden = emulator_library::find_preset("eden");
  ASSERT_NE(eden, nullptr);
  const emulator_library::install_t native {emulator_library::install_e::native, "/usr/bin/eden"};
  const auto command = emulator_library::launch_command(*eden, native, "/roms/it's here/Game (USA).nsp");
  EXPECT_EQ(command, "eden -f -g '/roms/it'\\''s here/Game (USA).nsp'");

  // process.cpp splits the command line the way a POSIX shell would.
  const auto parts = boost::program_options::split_unix(command);
  ASSERT_EQ(parts.size(), 4u);
  EXPECT_EQ(parts[0], "eden");
  EXPECT_EQ(parts[1], "-f");
  EXPECT_EQ(parts[2], "-g");
  EXPECT_EQ(parts[3], "/roms/it's here/Game (USA).nsp");
}

TEST(EmulatorLibraryCommands, TheCommandFollowsTheInstall) {
  const auto *eden = emulator_library::find_preset("eden");
  const auto *cemu = emulator_library::find_preset("cemu");
  const auto *ppsspp = emulator_library::find_preset("ppsspp");
  ASSERT_NE(eden, nullptr);
  ASSERT_NE(cemu, nullptr);
  ASSERT_NE(ppsspp, nullptr);
  using install_e = emulator_library::install_e;
  using install_t = emulator_library::install_t;

  EXPECT_EQ(emulator_library::launch_command(*eden, install_t {install_e::flatpak, "dev.eden_emu.eden"}, "/roms/Game.nsp"),
            "flatpak run dev.eden_emu.eden -f -g '/roms/Game.nsp'");
  EXPECT_EQ(emulator_library::launch_command(*eden, install_t {install_e::launcher, "/opt/Eden.AppImage"}, "/roms/Game.nsp"),
            "'/opt/Eden.AppImage' -f -g '/roms/Game.nsp'");
  // Not installed yet: written so it works once it is.
  EXPECT_EQ(emulator_library::launch_command(*eden, install_t {}, "/roms/Game.nsp"), "eden -f -g '/roms/Game.nsp'");
  // The binary that was actually found names the command, not the first candidate.
  EXPECT_EQ(emulator_library::launch_command(*cemu, install_t {install_e::native, "/usr/bin/cemu"}, "/roms/Game.wua"),
            "cemu -f -g '/roms/Game.wua'");
  // A path-only emulator gets the path alone.
  EXPECT_EQ(emulator_library::launch_command(*ppsspp, install_t {install_e::native, "/usr/bin/PPSSPPSDL"}, "/roms/Game.iso"),
            "PPSSPPSDL '/roms/Game.iso'");

  EXPECT_EQ(emulator_library::launcher_command(*eden, install_t {install_e::flatpak, "dev.eden_emu.eden"}), "flatpak run dev.eden_emu.eden");
  EXPECT_EQ(emulator_library::launcher_command(*eden, install_t {install_e::launcher, "/opt/Eden.AppImage"}), "'/opt/Eden.AppImage'");
  EXPECT_EQ(emulator_library::launcher_command(*eden, install_t {}), "eden");
}

TEST(EmulatorLibraryCommands, CustomTemplateCarriesThePlaceholderOnce) {
  EXPECT_TRUE(emulator_library::custom_template_valid("retroarch -f -L /cores/snes9x_libretro.so {rom}"));
  EXPECT_TRUE(emulator_library::custom_template_valid("  my-emu {rom} --fullscreen  "));
  EXPECT_FALSE(emulator_library::custom_template_valid(""));
  EXPECT_FALSE(emulator_library::custom_template_valid("{rom}"));
  EXPECT_FALSE(emulator_library::custom_template_valid("retroarch -f"));
  EXPECT_FALSE(emulator_library::custom_template_valid("emu {rom} {rom}"));

  EXPECT_EQ(emulator_library::custom_launch_command(" retroarch -f -L /cores/snes9x_libretro.so {rom} ", "/roms/Game (USA).sfc"),
            "retroarch -f -L /cores/snes9x_libretro.so '/roms/Game (USA).sfc'");
}

TEST(EmulatorLibraryCommands, HomeExpandsAtTokenStartsSinceNoShellRuns) {
  EXPECT_EQ(emulator_library::expand_home_tokens("retroarch -L ~/cores/x.so --config=~/ra.cfg '~/my cores/y.so' ~ a~b", "/accounts/x"),
            "retroarch -L /accounts/x/cores/x.so --config=/accounts/x/ra.cfg '/accounts/x/my cores/y.so' /accounts/x a~b");
  EXPECT_EQ(emulator_library::expand_home_tokens("~/emu {rom}", ""), "~/emu {rom}");
  EXPECT_EQ(emulator_library::expand_home_tokens("emu ~other/x {rom}", "/accounts/x"), "emu ~other/x {rom}");
  EXPECT_EQ(emulator_library::custom_launch_command(" ~/emu -f {rom} ", "/roms/Game.sfc", "/accounts/x"), "/accounts/x/emu -f '/roms/Game.sfc'");
}

TEST(EmulatorLibraryNames, FilenamesBecomeGridNames) {
  using emulator_library::display_name;
  EXPECT_EQ(display_name("/roms/Legend of Zelda, The - Breath of the Wild [0100F2C0115B6000][v0].nsp"), "The Legend of Zelda - Breath of the Wild");
  EXPECT_EQ(display_name("/roms/Super_Mario_Odyssey (USA) (Rev 1).xci"), "Super Mario Odyssey");
  EXPECT_EQ(display_name("/roms/Metroid Prime (USA) (v1.02).rvz"), "Metroid Prime");
  EXPECT_EQ(display_name("/roms/Hades.nsp"), "Hades");
  EXPECT_EQ(display_name("/roms/Game, A.iso"), "A Game");
  EXPECT_EQ(display_name("/roms/Legend of Zelda, The: Link's Awakening.nsp"), "The Legend of Zelda: Link's Awakening");
  EXPECT_EQ(display_name("/roms/Game, Then.iso"), "Game, Then");
  EXPECT_EQ(display_name("/roms/Title -  Subtitle -.chd"), "Title - Subtitle");
  // Nothing left once the tags go: keep the stem rather than an empty name.
  EXPECT_EQ(display_name("/roms/[0100000000010000].nsp"), "[0100000000010000]");
  EXPECT_EQ(display_name("/roms/Game.v1.2.nsp"), "Game.v1.2");
}

TEST(EmulatorLibraryNames, UpdateAndDlcDumpsAreRecognised) {
  using emulator_library::looks_like_update_or_dlc;
  const fs::path folder = "/roms";
  EXPECT_TRUE(looks_like_update_or_dlc("/roms/Game [0100AAAA][v65536].nsp", folder));
  EXPECT_TRUE(looks_like_update_or_dlc("/roms/Game [0100AAAA][v131072].nsp", folder));
  EXPECT_TRUE(looks_like_update_or_dlc("/roms/Game [UPD].nsp", folder));
  EXPECT_TRUE(looks_like_update_or_dlc("/roms/Game (Update 1.2).nsp", folder));
  EXPECT_TRUE(looks_like_update_or_dlc("/roms/Game (DLC).nsp", folder));
  EXPECT_TRUE(looks_like_update_or_dlc("/roms/Game [Patch].nsp", folder));
  EXPECT_TRUE(looks_like_update_or_dlc("/roms/updates/Game.nsp", folder));
  EXPECT_TRUE(looks_like_update_or_dlc("/roms/Zelda/DLC/Extra.nsp", folder));

  EXPECT_FALSE(looks_like_update_or_dlc("/roms/Game [0100AAAA][v0].nsp", folder));
  EXPECT_FALSE(looks_like_update_or_dlc("/roms/Game (USA) (v1.1).gba", folder));
  EXPECT_FALSE(looks_like_update_or_dlc("/roms/Game (Rev 1).gb", folder));
  EXPECT_FALSE(looks_like_update_or_dlc("/roms/Updated Edition/Game.nsp", folder));
  EXPECT_FALSE(looks_like_update_or_dlc("/roms/Game.nsp", folder));
}

TEST(EmulatorLibraryScan, FindsTheGamesAndNothingElse) {
  const auto root = fresh_root("scan");
  touch(root / "Game One (USA).nsp");
  touch(root / "Game One (EUR).nsp");  // same game, second region: one entry
  touch(root / "zelda" / "Legend of Zelda, The [0100F2C0115B6000][v0].xci");
  touch(root / "zelda" / "Legend of Zelda, The [0100F2C0115B6000][v65536].nsp");  // update
  touch(root / "zelda" / "dlc" / "Expansion.nsp");
  touch(root / "notes.txt");
  touch(root / ".hidden" / "Secret.nsp");
  touch(root / ".Trash.nsp");
  touch(root / "a" / "b" / "c" / "d" / "Too Deep.nsp");
  touch(root / "a" / "b" / "c" / "Just Deep Enough.NSP");

  const auto roms = emulator_library::scan_folder(root, {"nsp", "xci"});
  EXPECT_EQ(names_of(roms), (std::vector<std::string> {"Game One", "Just Deep Enough", "The Legend of Zelda"}));
  ASSERT_EQ(roms.size(), 3u);
  EXPECT_EQ(roms[0].path, root / "Game One (EUR).nsp");  // first in path order keeps the name
  EXPECT_EQ(roms[2].path, root / "zelda" / "Legend of Zelda, The [0100F2C0115B6000][v0].xci");

  EXPECT_TRUE(emulator_library::scan_folder(root, {}).empty());
  EXPECT_TRUE(emulator_library::scan_folder(root / "missing", {"nsp"}).empty());
  EXPECT_EQ(emulator_library::scan_folder(root, {"nsp", "xci"}, emulator_library::max_scan_depth, 1).size(), 1u);
}

TEST(EmulatorLibraryCovers, NextToTheGameThenEsDeThenRetroArchByRawStem) {
  const auto root = fresh_root("covers");
  const auto home = root / "home";
  const auto rom = root / "roms" / "switch" / "Game One (USA).nsp";
  touch(rom);
  const auto *eden = emulator_library::find_preset("eden");
  ASSERT_NE(eden, nullptr);
  const auto exists = [](const fs::path &) { return true; };

  EXPECT_FALSE(emulator_library::find_local_cover(rom, eden, {home}, exists).has_value());

  const auto esde = home / "ES-DE" / "downloaded_media" / "switch" / "covers" / "Game One (USA).jpg";
  touch(esde);
  EXPECT_EQ(emulator_library::find_local_cover(rom, eden, {home}, exists), esde);

  const auto beside = root / "roms" / "switch" / "covers" / "Game One (USA).png";
  touch(beside);
  EXPECT_EQ(emulator_library::find_local_cover(rom, eden, {home}, exists), beside);

  const auto adjacent = root / "roms" / "switch" / "Game One (USA).webp";
  touch(adjacent);
  EXPECT_EQ(emulator_library::find_local_cover(rom, eden, {home}, exists), adjacent);

  // The caller's image test is the last word.
  const auto never = [](const fs::path &) { return false; };
  EXPECT_FALSE(emulator_library::find_local_cover(rom, eden, {home}, never).has_value());

  // RetroArch names its boxarts after the game with the characters libretro forbids replaced.
  const auto *dolphin = emulator_library::find_preset("dolphin");
  ASSERT_NE(dolphin, nullptr);
  const auto ratchet = root / "roms" / "gc" / "Ratchet & Clank (USA).rvz";
  touch(ratchet);
  const auto boxart = home / ".config" / "retroarch" / "thumbnails" / "Nintendo - GameCube" / "Named_Boxarts" / "Ratchet _ Clank (USA).png";
  touch(boxart);
  EXPECT_EQ(emulator_library::find_local_cover(ratchet, dolphin, {home}, exists), boxart);
  EXPECT_EQ(emulator_library::libretro_thumbnail_stem("A: B/C*D?E"), "A_ B_C_D_E");

  // A custom folder has no system to look up; only the game's own folder counts, so with
  // the local copies gone the ES-DE cover is not consulted.
  fs::remove(adjacent);
  fs::remove(beside);
  EXPECT_FALSE(emulator_library::find_local_cover(rom, nullptr, {home}, exists).has_value());
  touch(beside);
  EXPECT_EQ(emulator_library::find_local_cover(rom, nullptr, {home}, exists), beside);
}

TEST(EmulatorLibraryCovers, CopiedCoversGetSafeUniqueNames) {
  const auto one = emulator_library::cover_stem("/roms/Game: One (USA).nsp", "eden");
  EXPECT_EQ(one.rfind("emulator_eden_Game__One__USA__", 0), 0u) << one;
  EXPECT_EQ(one.find(' '), std::string::npos);
  EXPECT_EQ(one.find(':'), std::string::npos);
  EXPECT_NE(one, emulator_library::cover_stem("/other/Game: One (USA).nsp", "eden"));
  EXPECT_EQ(one, emulator_library::cover_stem("/roms/./Game: One (USA).nsp", "eden"));
  EXPECT_LE(emulator_library::cover_stem(std::string(300, 'a') + ".nsp", "eden").size(), std::string_view("emulator_eden_").size() + 96 + 9);
}

TEST(EmulatorLibraryPrerequisites, FlatpakGrantsAreReadInOrderWithNegationsAndXdgNames) {
  const auto grants = emulator_library::flatpak_filesystem_grants(
    "[Application]\nname=dev.eden_emu.eden\n\n[Context]\nshared=network;\nfilesystems=home;!~/Private;/mnt/roms:ro;xdg-download/roms;\n\n[Session Bus Policy]\nfilesystems=ignored\n");
  EXPECT_EQ(grants, (std::vector<std::string> {"home", "!~/Private", "/mnt/roms", "xdg-download/roms"}));

  const std::vector<fs::path> homes {"/accounts/x"};
  using emulator_library::flatpak_can_read;
  EXPECT_TRUE(flatpak_can_read(grants, "/accounts/x/Games/switch", homes));
  EXPECT_FALSE(flatpak_can_read(grants, "/accounts/x/Private/roms", homes));
  EXPECT_TRUE(flatpak_can_read(grants, "/mnt/roms/switch", homes));
  EXPECT_FALSE(flatpak_can_read(grants, "/mnt/romsx", homes));
  EXPECT_FALSE(flatpak_can_read(grants, "/srv/roms", homes));
  EXPECT_TRUE(flatpak_can_read(grants, "/accounts/x/Downloads/roms/gba", homes));
  EXPECT_TRUE(flatpak_can_read({"host"}, "/srv/roms", homes));
  EXPECT_FALSE(flatpak_can_read({}, "/accounts/x/Games", homes));
  // The later grant wins, which is how an override re-allows a folder the metadata took away.
  EXPECT_TRUE(flatpak_can_read({"home", "!~/Games", "~/Games/switch"}, "/accounts/x/Games/switch/a", homes));
}

TEST(EmulatorLibraryPrerequisites, EdenNeedsKeysWhereverItIsInstalled) {
  const auto root = fresh_root("prereq-eden");
  const auto home = root / "home";
  fs::create_directories(home);
  const auto *eden = emulator_library::find_preset("eden");
  ASSERT_NE(eden, nullptr);
  using install_e = emulator_library::install_e;
  using install_t = emulator_library::install_t;

  auto checks = emulator_library::prerequisites(*eden, install_t {install_e::native, "/usr/bin/eden"}, root / "roms", {home}, root / "flatpak");
  ASSERT_EQ(checks.size(), 1u);
  EXPECT_EQ(checks[0].id, "eden_keys_missing");
  EXPECT_EQ(checks[0].severity, "warning");
  EXPECT_NE(checks[0].action.find("prod.keys"), std::string::npos);

  // Not installed at all: the folder says so itself, nothing is checked here.
  EXPECT_TRUE(emulator_library::prerequisites(*eden, install_t {}, root / "roms", {home}, root / "flatpak").empty());

  touch(home / ".var" / "app" / "dev.eden_emu.eden" / "data" / "eden" / "keys" / "prod.keys");
  EXPECT_TRUE(emulator_library::prerequisites(*eden, install_t {install_e::native, "/usr/bin/eden"}, root / "roms", {home}, root / "flatpak").empty());

  // A portable launcher keeps its keys next to itself.
  const auto portable_home = root / "home2";
  fs::create_directories(portable_home);
  touch(root / "apps" / "user" / "keys" / "prod.keys");
  EXPECT_TRUE(emulator_library::prerequisites(*eden, install_t {install_e::launcher, (root / "apps" / "Eden.AppImage").string()}, root / "roms", {portable_home}, root / "flatpak").empty());

  const auto *dolphin = emulator_library::find_preset("dolphin");
  ASSERT_NE(dolphin, nullptr);
  EXPECT_TRUE(emulator_library::prerequisites(*dolphin, install_t {install_e::native, "/usr/bin/dolphin-emu"}, root / "roms", {portable_home}, root / "flatpak").empty());

  const auto *duckstation = emulator_library::find_preset("duckstation");
  ASSERT_NE(duckstation, nullptr);
  auto bios = emulator_library::prerequisites(*duckstation, install_t {install_e::native, "/usr/bin/duckstation-qt"}, root / "roms", {portable_home}, root / "flatpak");
  ASSERT_EQ(bios.size(), 1u);
  EXPECT_EQ(bios[0].id, "duckstation_bios_missing");
  touch(portable_home / ".local" / "share" / "duckstation" / "bios" / "scph1001.BIN");
  EXPECT_TRUE(emulator_library::prerequisites(*duckstation, install_t {install_e::native, "/usr/bin/duckstation-qt"}, root / "roms", {portable_home}, root / "flatpak").empty());
}

TEST(EmulatorLibraryPrerequisites, AFlatpakThatCannotSeeTheFolderIsNamedWithTheOverride) {
  const auto root = fresh_root("prereq-flatpak");
  const auto home = root / "home";
  const auto system_root = root / "var-lib-flatpak";
  touch(home / ".var" / "app" / "dev.eden_emu.eden" / "data" / "eden" / "keys" / "prod.keys");
  const auto *eden = emulator_library::find_preset("eden");
  ASSERT_NE(eden, nullptr);
  const emulator_library::install_t flatpak {emulator_library::install_e::flatpak, "dev.eden_emu.eden"};

  // No metadata readable: nothing can be said, so nothing is claimed.
  EXPECT_TRUE(emulator_library::prerequisites(*eden, flatpak, "/mnt/roms/switch", {home}, system_root).empty());

  const auto metadata = system_root / "app" / "dev.eden_emu.eden" / "current" / "active" / "metadata";
  fs::create_directories(metadata.parent_path());
  {
    std::ofstream out(metadata);
    out << "[Application]\nname=dev.eden_emu.eden\n\n[Context]\nfilesystems=home;\n";
  }
  auto checks = emulator_library::prerequisites(*eden, flatpak, "/mnt/roms/switch", {home}, system_root);
  ASSERT_EQ(checks.size(), 1u);
  EXPECT_EQ(checks[0].id, "flatpak_folder_not_visible");
  EXPECT_EQ(checks[0].action, "flatpak override --user --filesystem='/mnt/roms/switch' dev.eden_emu.eden");
  EXPECT_TRUE(emulator_library::prerequisites(*eden, flatpak, (home / "Games" / "switch").string(), {home}, system_root).empty());

  // The user's override grants it, and the override is read after the metadata.
  const auto overrides = home / ".local" / "share" / "flatpak" / "overrides" / "dev.eden_emu.eden";
  fs::create_directories(overrides.parent_path());
  {
    std::ofstream out(overrides);
    out << "[Context]\nfilesystems=/mnt/roms:ro;\n";
  }
  EXPECT_TRUE(emulator_library::prerequisites(*eden, flatpak, "/mnt/roms/switch", {home}, system_root).empty());
  const auto grants = emulator_library::flatpak_effective_grants("dev.eden_emu.eden", {home}, system_root);
  ASSERT_TRUE(grants.has_value());
  EXPECT_EQ(*grants, (std::vector<std::string> {"home", "/mnt/roms"}));
}

TEST(EmulatorLibrarySources, RoundTripThroughJsonAndSurviveGarbage) {
  std::vector<emulator_library::source_t> sources {
    {"one", "/roms/switch", "eden", "", "", {}},
    {"two", "/roms/snes", "custom", "", "retroarch -f -L /cores/snes9x_libretro.so {rom}", {"SFC", ".smc", "sfc", "zip"}},
    {"three", "/roms/wiiu", "cemu", "/opt/Cemu.AppImage", "", {}},
  };
  const auto document = emulator_library::serialize_sources(sources);
  EXPECT_EQ(document["version"].get<int>(), emulator_library::sources_file_version);
  const auto parsed = emulator_library::parse_sources(document.dump());
  ASSERT_EQ(parsed.size(), 3u);
  EXPECT_EQ(parsed[0].id, "one");
  EXPECT_EQ(parsed[0].emulator, "eden");
  EXPECT_TRUE(parsed[0].extensions.empty());
  EXPECT_EQ(parsed[1].command, "retroarch -f -L /cores/snes9x_libretro.so {rom}");
  EXPECT_EQ(parsed[1].extensions, (std::vector<std::string> {"sfc", "smc", "zip"}));
  EXPECT_EQ(parsed[2].launcher, "/opt/Cemu.AppImage");

  EXPECT_TRUE(emulator_library::parse_sources("").empty());
  EXPECT_TRUE(emulator_library::parse_sources("not json").empty());
  EXPECT_TRUE(emulator_library::parse_sources("[]").empty());
  EXPECT_TRUE(emulator_library::parse_sources(R"({"sources": [{"id": "x"}, 4, {"path": "/p", "emulator": "eden"}]})").empty());
  const auto partial = emulator_library::parse_sources(R"({"sources": [{"id": "x", "path": "/p", "emulator": "eden", "extensions": "nsp"}]})");
  ASSERT_EQ(partial.size(), 1u);
  EXPECT_TRUE(partial[0].extensions.empty());

  EXPECT_EQ(emulator_library::parse_extension_list("nsp, xci .nca;NRO  nsp"), (std::vector<std::string> {"nsp", "xci", "nca", "nro"}));
  EXPECT_TRUE(emulator_library::parse_extension_list(" , ").empty());

  const auto *eden = emulator_library::find_preset("eden");
  ASSERT_NE(eden, nullptr);
  EXPECT_EQ(emulator_library::effective_extensions(parsed[0], eden), (std::vector<std::string> {"nsp", "xci", "nca", "nro", "nso"}));
  EXPECT_EQ(emulator_library::effective_extensions(parsed[1], nullptr), (std::vector<std::string> {"sfc", "smc", "zip"}));
  emulator_library::source_t overridden = parsed[0];
  overridden.extensions = {"nsp"};
  EXPECT_EQ(emulator_library::effective_extensions(overridden, eden), (std::vector<std::string> {"nsp"}));
}

TEST(EmulatorLibrarySources, ValidationNamesTheProblem) {
  const auto root = fresh_root("validate");
  touch(root / "Eden.AppImage");
  emulator_library::source_t source {"id", root.string(), "eden", "", "", {}};
  EXPECT_FALSE(emulator_library::validate_source(source).has_value());

  source.path = "relative/roms";
  EXPECT_NE(emulator_library::validate_source(source).value_or("").find("absolute"), std::string::npos);
  source.path = (root / "missing").string();
  EXPECT_NE(emulator_library::validate_source(source).value_or("").find("Folder not found"), std::string::npos);
  source.path = root.string();

  source.emulator = "ryujinx";
  EXPECT_EQ(emulator_library::validate_source(source).value_or(""), "Unknown emulator: ryujinx");
  source.emulator = "eden";

  source.launcher = (root / "Nope.AppImage").string();
  EXPECT_NE(emulator_library::validate_source(source).value_or("").find("Emulator not found at"), std::string::npos);
  source.launcher = (root / "Eden.AppImage").string();
  EXPECT_FALSE(emulator_library::validate_source(source).has_value());

  source.emulator = "custom";
  source.command = "retroarch -f";
  EXPECT_NE(emulator_library::validate_source(source).value_or("").find("{rom} exactly once"), std::string::npos);
  source.command = "retroarch -f -L /cores/snes9x_libretro.so {rom}";
  EXPECT_NE(emulator_library::validate_source(source).value_or("").find("file extensions"), std::string::npos);
  source.extensions = {"sfc"};
  EXPECT_FALSE(emulator_library::validate_source(source).has_value());

  source.id.clear();
  EXPECT_TRUE(emulator_library::validate_source(source).has_value());
}

TEST(EmulatorLibrarySources, ARomMustSitInsideItsFolder) {
  const auto root = fresh_root("belongs");
  touch(root / "switch" / "Game.nsp");
  touch(root / "other" / "Game.nsp");
  const auto folder = root / "switch";
  EXPECT_TRUE(emulator_library::rom_belongs_to_folder(folder / "Game.nsp", folder));
  EXPECT_TRUE(emulator_library::rom_belongs_to_folder(folder / "sub" / ".." / "Game.nsp", folder));
  EXPECT_FALSE(emulator_library::rom_belongs_to_folder(root / "other" / "Game.nsp", folder));
  EXPECT_FALSE(emulator_library::rom_belongs_to_folder(folder / ".." / "other" / "Game.nsp", folder));
  EXPECT_FALSE(emulator_library::rom_belongs_to_folder(folder, folder));
  EXPECT_FALSE(emulator_library::rom_belongs_to_folder(folder / "Game.nsp", root / "missing"));
}

TEST(EmulatorLibraryInstall, LauncherThenPathThenFlatpak) {
  const auto root = fresh_root("install");
  const auto bin = root / "bin";
  touch(bin / "eden");
  fs::permissions(bin / "eden", fs::perms::owner_all, fs::perm_options::replace);
  touch(bin / "not-executable");
  fs::permissions(bin / "not-executable", fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
  const auto home = root / "home";
  fs::create_directories(home / ".var" / "app" / "org.DolphinEmu.dolphin-emu");
  const auto system_flatpak = root / "var-lib-flatpak";
  fs::create_directories(system_flatpak / "app" / "info.cemu.Cemu");
  touch(root / "Eden.AppImage");

  const auto path_env = (root / "empty").string() + ":" + bin.string();
  const auto *eden = emulator_library::find_preset("eden");
  const auto *dolphin = emulator_library::find_preset("dolphin");
  const auto *cemu = emulator_library::find_preset("cemu");
  const auto *mgba = emulator_library::find_preset("mgba");
  ASSERT_NE(eden, nullptr);
  ASSERT_NE(dolphin, nullptr);
  ASSERT_NE(cemu, nullptr);
  ASSERT_NE(mgba, nullptr);
  using install_e = emulator_library::install_e;

  const auto native = emulator_library::detect_install(*eden, "", {home}, path_env, system_flatpak);
  EXPECT_EQ(native.kind, install_e::native);
  EXPECT_EQ(native.location, (bin / "eden").string());

  const auto launcher = emulator_library::detect_install(*eden, (root / "Eden.AppImage").string(), {home}, path_env, system_flatpak);
  EXPECT_EQ(launcher.kind, install_e::launcher);
  EXPECT_EQ(launcher.location, (root / "Eden.AppImage").string());

  // A launcher that went away is reported, not replaced by whatever PATH has.
  const auto gone = emulator_library::detect_install(*eden, (root / "Gone.AppImage").string(), {home}, path_env, system_flatpak);
  EXPECT_EQ(gone.kind, install_e::missing);
  EXPECT_EQ(gone.location, (root / "Gone.AppImage").string());

  const auto user_flatpak = emulator_library::detect_install(*dolphin, "", {home}, path_env, system_flatpak);
  EXPECT_EQ(user_flatpak.kind, install_e::flatpak);
  EXPECT_EQ(user_flatpak.location, "org.DolphinEmu.dolphin-emu");

  const auto system_wide = emulator_library::detect_install(*cemu, "", {home}, path_env, system_flatpak);
  EXPECT_EQ(system_wide.kind, install_e::flatpak);

  const auto missing = emulator_library::detect_install(*mgba, "", {home}, path_env, system_flatpak);
  EXPECT_EQ(missing.kind, install_e::missing);
  EXPECT_TRUE(missing.location.empty());

  EXPECT_FALSE(emulator_library::find_on_path("not-executable", path_env).has_value());
  EXPECT_FALSE(emulator_library::find_on_path("eden", "").has_value());
  EXPECT_FALSE(emulator_library::find_on_path("bin/eden", path_env).has_value());
  EXPECT_EQ(emulator_library::install_name(install_e::flatpak), "flatpak");
  EXPECT_EQ(emulator_library::install_name(install_e::missing), "missing");
}

TEST(EmulatorLibraryInstall, HomeExpansion) {
  EXPECT_EQ(emulator_library::expand_home("~/Games/switch", "/accounts/x"), "/accounts/x/Games/switch");
  EXPECT_EQ(emulator_library::expand_home("  ~  ", "/accounts/x"), "/accounts/x");
  EXPECT_EQ(emulator_library::expand_home("~other/roms", "/accounts/x"), "~other/roms");
  EXPECT_EQ(emulator_library::expand_home("/roms", "/accounts/x"), "/roms");
  EXPECT_EQ(emulator_library::expand_home("~/roms", ""), "~/roms");
}

TEST(EmulatorLibraryResolve, AnEntryRunsWhatIsInstalledNotWhatWasSavedAtImport) {
  const auto root = fresh_root("resolve");
  const auto home = root / "home";
  const auto system_flatpak = root / "var-lib-flatpak";
  const auto empty_path = (root / "empty").string();
  using install_e = emulator_library::install_e;

  // Imported while Eden was missing: the saved command names the bare binary.
  const auto missing = emulator_library::resolve_entry_launch("eden", "/roms/Game.xci", "", {home}, empty_path, system_flatpak);
  ASSERT_TRUE(missing.has_value());
  EXPECT_EQ(missing->install.kind, install_e::missing);
  EXPECT_TRUE(missing->command.empty());
  ASSERT_NE(missing->preset, nullptr);
  EXPECT_EQ(missing->preset->id, "eden");

  // Installed from Flathub afterwards: the game and the emulator's own entry both follow it.
  fs::create_directories(home / ".local" / "share" / "flatpak" / "app" / "dev.eden_emu.eden");
  const auto game = emulator_library::resolve_entry_launch("eden", "/roms/Game.xci", "", {home}, empty_path, system_flatpak);
  ASSERT_TRUE(game.has_value());
  EXPECT_EQ(game->install.kind, install_e::flatpak);
  EXPECT_EQ(game->command, "flatpak run dev.eden_emu.eden -f -g '/roms/Game.xci'");
  const auto launcher_entry = emulator_library::resolve_entry_launch("eden", "", "", {home}, empty_path, system_flatpak);
  ASSERT_TRUE(launcher_entry.has_value());
  EXPECT_EQ(launcher_entry->command, "flatpak run dev.eden_emu.eden");

  // The folder's own emulator file wins, and one that went away is missing with its path.
  touch(root / "Eden.AppImage");
  const auto appimage = emulator_library::resolve_entry_launch("eden", "/roms/Game.xci", (root / "Eden.AppImage").string(), {home}, empty_path, system_flatpak);
  ASSERT_TRUE(appimage.has_value());
  EXPECT_EQ(appimage->command, "'" + (root / "Eden.AppImage").string() + "' -f -g '/roms/Game.xci'");
  const auto gone = emulator_library::resolve_entry_launch("eden", "/roms/Game.xci", (root / "Gone.AppImage").string(), {home}, empty_path, system_flatpak);
  ASSERT_TRUE(gone.has_value());
  EXPECT_EQ(gone->install.kind, install_e::missing);
  EXPECT_EQ(gone->install.location, (root / "Gone.AppImage").string());

  // A custom template, an unknown emulator and an ordinary app have nothing to resolve.
  EXPECT_FALSE(emulator_library::resolve_entry_launch("custom", "/roms/Game.sfc", "", {home}, empty_path, system_flatpak).has_value());
  EXPECT_FALSE(emulator_library::resolve_entry_launch("retroarch", "/roms/Game.sfc", "", {home}, empty_path, system_flatpak).has_value());
  EXPECT_FALSE(emulator_library::resolve_entry_launch("", "", "", {home}, empty_path, system_flatpak).has_value());
}

TEST(EmulatorLibraryResolve, TheRefusalNamesTheEmulatorTheGameAndTheFix) {
  const auto *eden = emulator_library::find_preset("eden");
  ASSERT_NE(eden, nullptr);

  const auto not_installed = emulator_library::missing_install_reason(*eden, {}, "The Legend of Zelda - Breath of the Wild");
  EXPECT_EQ(not_installed.message, "Eden is not installed on this host, so The Legend of Zelda - Breath of the Wild cannot start.");
  EXPECT_EQ(not_installed.action, "Install Eden from ROM folders under Import Games in the Polaris web UI, then launch again.");

  // The emulator's own entry does not repeat its name.
  const auto own_entry = emulator_library::missing_install_reason(*eden, {}, "Eden");
  EXPECT_EQ(own_entry.message, "Eden is not installed on this host.");

  const auto gone = emulator_library::missing_install_reason(
    *eden, {emulator_library::install_e::missing, "/opt/Eden.AppImage"}, "Game");
  EXPECT_EQ(gone.message, "Eden was not found at /opt/Eden.AppImage, so Game cannot start.");
  EXPECT_EQ(gone.action, "Put Eden back at that path, or add the ROM folder again with the file where it is now, then launch again.");
}

TEST(EmulatorLibraryResolve, TheFolderListSitsNextToTheAppsFileAndNamesItsLauncher) {
  EXPECT_EQ(emulator_library::sources_path_for_apps_file("/accounts/x/.config/polaris/apps.json", "/fallback"),
            fs::path("/accounts/x/.config/polaris/library_sources.json"));
  EXPECT_EQ(emulator_library::sources_path_for_apps_file("apps.json", "/fallback"), fs::path("/fallback/library_sources.json"));

  std::vector<emulator_library::source_t> sources(2);
  sources[0].id = "folder-a";
  sources[0].launcher = "/opt/Eden.AppImage";
  sources[1].id = "folder-b";
  EXPECT_EQ(emulator_library::configured_launcher_for(sources, "folder-a"), "/opt/Eden.AppImage");
  EXPECT_EQ(emulator_library::configured_launcher_for(sources, " folder-a "), "/opt/Eden.AppImage");
  EXPECT_EQ(emulator_library::configured_launcher_for(sources, "folder-b"), "");
  EXPECT_EQ(emulator_library::configured_launcher_for(sources, "unknown"), "");
  EXPECT_EQ(emulator_library::configured_launcher_for(sources, ""), "");
}

TEST(EmulatorLibraryResolve, OnlyACommandPolarisWroteIsReplaced) {
  const auto *eden = emulator_library::find_preset("eden");
  const auto *cemu = emulator_library::find_preset("cemu");
  ASSERT_NE(eden, nullptr);
  ASSERT_NE(cemu, nullptr);
  const std::string rom = "/roms/it's here/Game.nsp";
  const auto arguments = " -f -g " + emulator_library::shell_quote(rom);

  // Every install import can have written for, and the emulator's own entry without a game.
  EXPECT_TRUE(emulator_library::generated_entry_command(*eden, rom, "eden" + arguments));
  EXPECT_TRUE(emulator_library::generated_entry_command(*eden, rom, "flatpak run dev.eden_emu.eden" + arguments));
  EXPECT_TRUE(emulator_library::generated_entry_command(*eden, rom, emulator_library::shell_quote("/opt/It's Eden.AppImage") + arguments));
  EXPECT_TRUE(emulator_library::generated_entry_command(*cemu, "", "cemu"));
  EXPECT_TRUE(emulator_library::generated_entry_command(*cemu, "", "Cemu"));
  EXPECT_TRUE(emulator_library::generated_entry_command(*eden, "", "flatpak run dev.eden_emu.eden"));

  // The player's edits, and commands for another game or emulator.
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, rom, "gamemoderun eden" + arguments));
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, rom, "eden -g " + emulator_library::shell_quote(rom)));
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, rom, "eden -f -g '/roms/Other.nsp'"));
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, rom, "flatpak run --command=eden-cli dev.eden_emu.eden" + arguments));
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, rom, "dolphin-emu" + arguments));
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, rom, "'/opt/Eden.AppImage' --portable" + arguments));
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, "", "eden --help"));
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, rom, arguments));

  // An emulator file that is still there, and not the one the folder names, is the player's.
  const auto directory = std::filesystem::temp_directory_path() / "polaris-emulator-command";
  std::error_code cleanup;
  std::filesystem::remove_all(directory, cleanup);
  std::filesystem::create_directories(directory);
  const auto moved = directory / "Eden.AppImage";
  { std::ofstream(moved, std::ios::binary) << "x"; }
  const auto moved_command = emulator_library::shell_quote(moved.string()) + arguments;
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, rom, moved_command, "/old/Eden.AppImage"));
  EXPECT_FALSE(emulator_library::generated_entry_command(*eden, rom, moved_command));
  // The folder's own launcher stays Polaris's to update, and so does a file that is gone.
  EXPECT_TRUE(emulator_library::generated_entry_command(*eden, rom, moved_command, moved.string()));
  EXPECT_TRUE(emulator_library::generated_entry_command(
    *eden, rom, emulator_library::shell_quote((directory / "Gone.AppImage").string()) + arguments, moved.string()));
  std::filesystem::remove_all(directory, cleanup);

  EXPECT_TRUE(emulator_library::single_quoted_token("'/opt/Eden.AppImage'"));
  EXPECT_TRUE(emulator_library::single_quoted_token(emulator_library::shell_quote("/opt/it's/Eden")));
  EXPECT_FALSE(emulator_library::single_quoted_token("'/opt/a' '/opt/b'"));
  EXPECT_FALSE(emulator_library::single_quoted_token("''\\''"));
  EXPECT_FALSE(emulator_library::single_quoted_token("'"));
  EXPECT_FALSE(emulator_library::single_quoted_token("/opt/Eden.AppImage"));
}

TEST(EmulatorInstall, FlatpakRunsForTheAccountFromFlathubWithoutAShell) {
  EXPECT_EQ(emulator_install::remotes_argv("/usr/bin/flatpak"), (std::vector<std::string> {"/usr/bin/flatpak", "remotes", "--user", "--columns=name"}));
  EXPECT_EQ(emulator_install::remote_add_argv("/usr/bin/flatpak"),
            (std::vector<std::string> {"/usr/bin/flatpak", "remote-add", "--user", "--if-not-exists", "flathub", "https://dl.flathub.org/repo/flathub.flatpakrepo"}));
  EXPECT_EQ(emulator_install::install_argv("/usr/bin/flatpak", "dev.eden_emu.eden"),
            (std::vector<std::string> {"/usr/bin/flatpak", "install", "--user", "--noninteractive", "-y", "flathub", "dev.eden_emu.eden"}));

  EXPECT_TRUE(emulator_install::remote_listed("fedora\nflathub\n", "flathub"));
  EXPECT_TRUE(emulator_install::remote_listed("  flathub  ", "flathub"));
  EXPECT_FALSE(emulator_install::remote_listed("flathub-beta\nfedora\n", "flathub"));
  EXPECT_FALSE(emulator_install::remote_listed("", "flathub"));
}

TEST(EmulatorInstall, AFailureSaysWhatWasTriedAndWhatFlatpakSaid) {
  // Progress redraws one line with carriage returns; the reason is the last line with text.
  EXPECT_EQ(emulator_install::last_output_line("Looking for matches\n\rDownloading 10%\rDownloading 90%\n\n"), "Downloading 90%");
  EXPECT_EQ(emulator_install::last_output_line(std::string(1000, 'x')).size(), emulator_install::maximum_message_bytes);

  emulator_install::run_result_t nothing_matches {1, false, "Looking for matches…\nerror: Nothing matches org.duckstation.DuckStation in remote flathub\n"};
  EXPECT_EQ(emulator_install::failure_message("Installing DuckStation from Flathub", nothing_matches),
            "Installing DuckStation from Flathub failed: Nothing matches org.duckstation.DuckStation in remote flathub");
  EXPECT_EQ(emulator_install::failure_message("Installing Eden from Flathub", {127, false, ""}),
            "Installing Eden from Flathub failed (exit status 127).");
  EXPECT_EQ(emulator_install::failure_message("Installing Eden from Flathub", {-1, true, "Downloading 40%"}),
            "Installing Eden from Flathub took too long and was stopped.");
  EXPECT_EQ(emulator_install::failure_message("Installing Eden from Flathub", {1, false, "error:\n"}),
            "Installing Eden from Flathub failed (exit status 1).");
}

TEST(EmulatorInstall, AJobAddsFlathubOnlyWhenMissingAndOneRunsPerEmulator) {
  const auto *eden = emulator_library::find_preset("eden");
  const auto *dolphin = emulator_library::find_preset("dolphin");
  ASSERT_NE(eden, nullptr);
  ASSERT_NE(dolphin, nullptr);

  std::mutex calls_mutex;
  std::vector<std::string> calls;
  std::string remotes_output = "fedora\n";
  std::promise<void> release_install;
  auto released = release_install.get_future().share();
  std::atomic<bool> hold_install {false};
  emulator_install::runner_t runner = [&](const std::vector<std::string> &argv, std::chrono::milliseconds) {
    {
      std::lock_guard lock(calls_mutex);
      calls.push_back(argv.at(1) + (argv.size() > 1 ? " " + argv.back() : ""));
    }
    if (argv.at(1) == "remotes") {
      return emulator_install::run_result_t {0, false, remotes_output};
    }
    if (argv.at(1) == "install" && hold_install) {
      released.wait();
    }
    if (argv.at(1) == "install" && argv.back() == "org.DolphinEmu.dolphin-emu") {
      return emulator_install::run_result_t {1, false, "error: Unable to load summary from remote flathub\n"};
    }
    return emulator_install::run_result_t {0, false, ""};
  };
  std::optional<std::string> flatpak = "/usr/bin/flatpak";
  emulator_install::installer_t installer {runner, [&]() {
                                             return flatpak;
                                           }};

  // Nothing to install without Flatpak, or for a preset with no Flatpak id.
  flatpak.reset();
  EXPECT_FALSE(installer.installable(*eden));
  EXPECT_EQ(installer.start(*eden, {}), emulator_install::start_e::not_installable);
  flatpak = "/usr/bin/flatpak";
  auto no_flatpak_id = *eden;
  no_flatpak_id.flatpak_id = {};
  EXPECT_FALSE(installer.installable(no_flatpak_id));
  EXPECT_TRUE(installer.installable(*eden));
  EXPECT_FALSE(installer.job("eden").has_value());

  // Flathub is added for the account first, then the emulator installs and its entries follow.
  std::vector<std::string> installed;
  const auto on_installed = [&](const emulator_library::preset_t &preset) {
    std::lock_guard lock(calls_mutex);
    installed.emplace_back(preset.id);
  };
  hold_install = true;
  ASSERT_EQ(installer.start(*eden, on_installed), emulator_install::start_e::started);
  ASSERT_TRUE(installer.job("eden").has_value());
  EXPECT_EQ(installer.job("eden")->state, emulator_install::state_e::installing);
  EXPECT_GT(installer.job("eden")->started_at, 0);
  EXPECT_EQ(installer.job("eden")->finished_at, 0);
  EXPECT_EQ(installer.start(*eden, on_installed), emulator_install::start_e::already_running);
  release_install.set_value();
  ASSERT_TRUE(installer.wait_idle_for_tests(std::chrono::seconds {10}));
  hold_install = false;
  EXPECT_EQ(calls, (std::vector<std::string> {"remotes --columns=name", "remote-add https://dl.flathub.org/repo/flathub.flatpakrepo", "install dev.eden_emu.eden"}));
  EXPECT_EQ(installed, (std::vector<std::string> {"eden"}));
  auto job = installer.job("eden");
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state, emulator_install::state_e::installed);
  EXPECT_EQ(job->message, "Eden is installed.");
  EXPECT_GE(job->finished_at, job->started_at);

  // With Flathub listed nothing is added, and a failure keeps Flatpak's reason and skips the entries.
  calls.clear();
  remotes_output = "fedora\nflathub\n";
  ASSERT_EQ(installer.start(*dolphin, on_installed), emulator_install::start_e::started);
  ASSERT_TRUE(installer.wait_idle_for_tests(std::chrono::seconds {10}));
  EXPECT_EQ(calls, (std::vector<std::string> {"remotes --columns=name", "install org.DolphinEmu.dolphin-emu"}));
  EXPECT_EQ(installed, (std::vector<std::string> {"eden"}));
  job = installer.job("dolphin");
  ASSERT_TRUE(job.has_value());
  EXPECT_EQ(job->state, emulator_install::state_e::failed);
  EXPECT_EQ(job->message, "Installing Dolphin from Flathub failed: Unable to load summary from remote flathub");

  // A finished job does not block trying again.
  EXPECT_EQ(installer.start(*dolphin, {}), emulator_install::start_e::started);
  ASSERT_TRUE(installer.wait_idle_for_tests(std::chrono::seconds {10}));
}
