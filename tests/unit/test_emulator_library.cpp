/**
 * @file tests/unit/test_emulator_library.cpp
 * @brief ROM folder import: presets, names, commands, the folder scan and the source list.
 */
#include "../tests_common.h"
#include "../tests_paths.h"

#include <src/emulator_library.h>

#include <boost/program_options/parsers.hpp>

#include <filesystem>
#include <fstream>
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
