/**
 * @file src/emulator_library.h
 * @brief ROM folder import: emulator presets, launch commands and the persisted folder list.
 *
 * A ROM folder is a directory plus the emulator that loads its files. Everything an
 * import needs is derived here from the folder and the file, never from the browser:
 * the launch command comes from the preset (or the user's own template), the entry
 * name from the filename, and the pad from what the platform expects.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace emulator_library {
  /// The apps.json `source` value for entries this importer creates.
  inline constexpr std::string_view source_name = "emulator";
  /// The emulator id of a folder that carries its own command template.
  inline constexpr std::string_view custom_emulator_id = "custom";
  /// The placeholder a command template carries once; it becomes the quoted ROM path.
  inline constexpr std::string_view rom_placeholder = "{rom}";
  inline constexpr int sources_file_version = 1;
  /// How many directory levels below the folder are entered. One folder per game plus
  /// a couple of grouping levels is the common layout; deeper trees are archives.
  inline constexpr std::size_t max_scan_depth = 3;
  inline constexpr std::size_t max_roms_per_folder = 2000;

  struct preset_t {
    std::string_view id;
    std::string_view label;
    std::string_view platform;
    std::string_view platform_id;  ///< the console as a client-facing id: switch, psx, gba
    std::vector<std::string_view> binaries;  ///< PATH candidates, first match wins
    std::string_view flatpak_id;
    std::string_view arguments;  ///< carries rom_placeholder; empty means the path alone
    std::vector<std::string_view> extensions;  ///< lower case, no dot
    std::string_view gamepad;  ///< per-app Emulated Gamepad Type; empty keeps the host default
    std::vector<std::string_view> es_systems;  ///< ES-DE system folders whose downloaded media may hold covers
    std::vector<std::string_view> retroarch_systems;  ///< RetroArch thumbnail system names for the same games
  };

  /**
   * @brief The emulators Polaris knows how to launch straight into a game.
   *
   * Flags come from each emulator's own command-line handling: Eden's user handbook
   * (`-f`, `-g`), Dolphin's CommandLineParse (`-b`, `-e`), Cemu's LaunchSettings
   * (`-f`, `-g`), DuckStation's and PCSX2's Qt hosts (`-batch`, `-fullscreen`, then
   * the path), PPSSPP's positional boot filename, and mgba-qt(6) (`-f`).
   */
  inline const std::vector<preset_t> &presets() {
    static const std::vector<preset_t> list {
      {"eden", "Eden", "Nintendo Switch", "switch", {"eden"}, "dev.eden_emu.eden", "-f -g {rom}", {"nsp", "xci", "nca", "nro", "nso"}, "switch", {"switch"}, {}},
      {"dolphin", "Dolphin", "GameCube and Wii", "gamecube-wii", {"dolphin-emu"}, "org.DolphinEmu.dolphin-emu", "-b -e {rom}", {"iso", "gcm", "wbfs", "rvz", "ciso", "gcz", "wia", "wad", "dol", "elf"}, "", {"gc", "wii"}, {"Nintendo - GameCube", "Nintendo - Wii"}},
      {"cemu", "Cemu", "Wii U", "wiiu", {"Cemu", "cemu"}, "info.cemu.Cemu", "-f -g {rom}", {"wua", "wud", "wux", "rpx"}, "", {"wiiu"}, {"Nintendo - Wii U"}},
      {"duckstation", "DuckStation", "PlayStation", "psx", {"duckstation-qt"}, "org.duckstation.DuckStation", "-batch -fullscreen {rom}", {"cue", "chd", "iso", "pbp", "m3u", "img", "ecm", "mds"}, "ds5", {"psx"}, {"Sony - PlayStation"}},
      {"pcsx2", "PCSX2", "PlayStation 2", "ps2", {"pcsx2-qt"}, "net.pcsx2.PCSX2", "-batch -fullscreen {rom}", {"iso", "chd", "cso", "zso", "gz", "bin", "elf"}, "ds5", {"ps2"}, {"Sony - PlayStation 2"}},
      {"ppsspp", "PPSSPP", "PlayStation Portable", "psp", {"PPSSPPSDL", "PPSSPPQt"}, "org.ppsspp.PPSSPP", "{rom}", {"iso", "cso", "chd", "pbp"}, "ds5", {"psp"}, {"Sony - PlayStation Portable"}},
      {"mgba", "mGBA", "Game Boy Advance", "gba", {"mgba-qt"}, "io.mgba.mGBA", "-f {rom}", {"gba", "gb", "gbc", "sgb"}, "", {"gba", "gb", "gbc"}, {"Nintendo - Game Boy Advance", "Nintendo - Game Boy", "Nintendo - Game Boy Color"}},
    };
    return list;
  }

  inline const preset_t *find_preset(std::string_view id) {
    const auto &list = presets();
    const auto it = std::find_if(list.begin(), list.end(), [&](const preset_t &preset) {
      return preset.id == id;
    });
    return it == list.end() ? nullptr : &*it;
  }

  enum class install_e {
    native,  ///< a binary on the service's PATH
    flatpak,  ///< the Flatpak, run through `flatpak run`
    launcher,  ///< the file the user pointed the folder at, an AppImage typically
    missing,
  };

  struct install_t {
    install_e kind = install_e::missing;
    std::string location;  ///< binary path, Flatpak id, or the launcher the user named
  };

  inline std::string_view install_name(install_e kind) {
    switch (kind) {
      case install_e::native:
        return "native";
      case install_e::flatpak:
        return "flatpak";
      case install_e::launcher:
        return "launcher";
      case install_e::missing:
        break;
    }
    return "missing";
  }

  inline std::string lower_copy(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return result;
  }

  inline std::string_view trim_view(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
      text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
      text.remove_suffix(1);
    }
    return text;
  }

  /// `~` and `~/...` against the account's home; anything else unchanged.
  inline std::string expand_home(std::string_view path, std::string_view home) {
    path = trim_view(path);
    if (home.empty() || path.empty() || path.front() != '~') {
      return std::string(path);
    }
    if (path.size() == 1) {
      return std::string(home);
    }
    if (path[1] == '/') {
      return std::string(home) + std::string(path.substr(1));
    }
    return std::string(path);
  }

  inline std::optional<std::filesystem::path> find_on_path(std::string_view binary, std::string_view path_env) {
    if (binary.empty() || binary.find('/') != std::string_view::npos) {
      return std::nullopt;
    }
    std::size_t start = 0;
    while (start <= path_env.size()) {
      const auto end = path_env.find(':', start);
      const auto dir = path_env.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      if (!dir.empty()) {
        std::error_code error;
        const auto candidate = std::filesystem::path(dir) / std::filesystem::path(binary);
        const auto status = std::filesystem::status(candidate, error);
        constexpr auto any_exec = std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec;
        if (!error && std::filesystem::is_regular_file(status) && (status.permissions() & any_exec) != std::filesystem::perms::none) {
          return candidate;
        }
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return std::nullopt;
  }

  /**
   * @brief True when the Flatpak is installed for the account or the system.
   *
   * `~/.var/app/<id>` only appears after the first run, so the installation
   * directories are checked as well: a Flatpak installed a minute ago should count.
   */
  inline bool flatpak_app_installed(
    std::string_view flatpak_id,
    const std::vector<std::filesystem::path> &home_roots,
    const std::filesystem::path &system_flatpak_root = "/var/lib/flatpak"
  ) {
    if (flatpak_id.empty()) {
      return false;
    }
    std::error_code error;
    const std::filesystem::path id {flatpak_id};
    for (const auto &home : home_roots) {
      if (std::filesystem::is_directory(home / ".var" / "app" / id, error) ||
          std::filesystem::is_directory(home / ".local" / "share" / "flatpak" / "app" / id, error)) {
        return true;
      }
    }
    return std::filesystem::is_directory(system_flatpak_root / "app" / id, error);
  }

  /**
   * @brief Where the preset's emulator is on this host.
   *
   * The launcher the user named wins, and one that is gone is reported missing rather
   * than silently replaced: they chose it for a reason. Then a binary on PATH, then
   * the Flatpak.
   */
  inline install_t detect_install(
    const preset_t &preset,
    std::string_view configured_launcher,
    const std::vector<std::filesystem::path> &home_roots,
    std::string_view path_env,
    const std::filesystem::path &system_flatpak_root = "/var/lib/flatpak"
  ) {
    configured_launcher = trim_view(configured_launcher);
    if (!configured_launcher.empty()) {
      std::error_code error;
      const std::filesystem::path launcher {configured_launcher};
      if (std::filesystem::is_regular_file(launcher, error)) {
        return {install_e::launcher, launcher.string()};
      }
      return {install_e::missing, launcher.string()};
    }
    for (const auto binary : preset.binaries) {
      if (const auto found = find_on_path(binary, path_env); found) {
        return {install_e::native, found->string()};
      }
    }
    if (flatpak_app_installed(preset.flatpak_id, home_roots, system_flatpak_root)) {
      return {install_e::flatpak, std::string(preset.flatpak_id)};
    }
    return {};
  }

  /// Single quotes for the launch line; platf::run_command splits a quoted command like a POSIX shell.
  inline std::string shell_quote(std::string_view value) {
    std::string quoted = "'";
    for (const char ch : value) {
      if (ch == '\'') {
        quoted += "'\\''";
      } else {
        quoted += ch;
      }
    }
    quoted += '\'';
    return quoted;
  }

  /// The command that opens the emulator on its own, the entry published next to the games.
  inline std::string launcher_command(const preset_t &preset, const install_t &install) {
    switch (install.kind) {
      case install_e::flatpak:
        return "flatpak run " + std::string(preset.flatpak_id);
      case install_e::launcher:
        return shell_quote(install.location);
      case install_e::native: {
        // The bare name rather than the resolved path, so the entry survives a package
        // update that moves the binary and reads like what the user would type.
        const auto name = std::filesystem::path(install.location).filename().string();
        return name.empty() ? std::string(preset.binaries.front()) : name;
      }
      case install_e::missing:
        break;
    }
    // Not installed yet: the entry is written so it launches once it is.
    return install.location.empty() ? std::string(preset.binaries.front()) : shell_quote(install.location);
  }

  inline std::string substitute_rom(std::string_view arguments, const std::filesystem::path &rom) {
    const auto quoted = shell_quote(rom.string());
    std::string result;
    std::size_t start = 0;
    while (true) {
      const auto pos = arguments.find(rom_placeholder, start);
      if (pos == std::string_view::npos) {
        result.append(arguments.substr(start));
        break;
      }
      result.append(arguments.substr(start, pos - start));
      result += quoted;
      start = pos + rom_placeholder.size();
    }
    return result;
  }

  inline std::string launch_command(const preset_t &preset, const install_t &install, const std::filesystem::path &rom) {
    auto command = launcher_command(preset, install);
    command += ' ';
    command += substitute_rom(preset.arguments.empty() ? rom_placeholder : preset.arguments, rom);
    return command;
  }

  /// A custom template names the emulator and carries the placeholder exactly once.
  inline bool custom_template_valid(std::string_view command) {
    command = trim_view(command);
    const auto first = command.find(rom_placeholder);
    if (first == std::string_view::npos) {
      return false;
    }
    if (command.find(rom_placeholder, first + rom_placeholder.size()) != std::string_view::npos) {
      return false;
    }
    return command != rom_placeholder;
  }

  /**
   * @brief `~` and `~/...` at the start of a token expand against the account home.
   *
   * The command runs without a shell, so nothing else would expand them; a token starts
   * the line, follows whitespace, a quote, or an `=` (for `--option=~/path`).
   */
  inline std::string expand_home_tokens(std::string_view command, std::string_view home) {
    if (home.empty()) {
      return std::string(command);
    }
    std::string result;
    result.reserve(command.size() + home.size());
    for (std::size_t i = 0; i < command.size(); ++i) {
      const char ch = command[i];
      const bool token_start = i == 0 || std::isspace(static_cast<unsigned char>(command[i - 1])) ||
                               command[i - 1] == '\'' || command[i - 1] == '"' || command[i - 1] == '=';
      const bool token_end = i + 1 == command.size() || command[i + 1] == '/' || command[i + 1] == '\'' ||
                             command[i + 1] == '"' || std::isspace(static_cast<unsigned char>(command[i + 1]));
      if (ch == '~' && token_start && token_end) {
        result.append(home);
        continue;
      }
      result += ch;
    }
    return result;
  }

  inline std::string custom_launch_command(std::string_view command, const std::filesystem::path &rom, std::string_view home = {}) {
    return substitute_rom(expand_home_tokens(trim_view(command), home), rom);
  }

  /// The contents of every `(...)`, `[...]` and `{...}` group, in order.
  inline std::vector<std::string> bracket_groups(std::string_view text) {
    std::vector<std::string> groups;
    std::string current;
    int depth = 0;
    for (const char ch : text) {
      if (ch == '(' || ch == '[' || ch == '{') {
        if (depth == 0) {
          current.clear();
        }
        ++depth;
        continue;
      }
      if (ch == ')' || ch == ']' || ch == '}') {
        if (depth > 0 && --depth == 0) {
          groups.push_back(current);
        }
        continue;
      }
      if (depth > 0) {
        current += ch;
      }
    }
    return groups;
  }

  inline std::string strip_bracket_groups(std::string_view text) {
    std::string result;
    int depth = 0;
    for (const char ch : text) {
      if (ch == '(' || ch == '[' || ch == '{') {
        ++depth;
        continue;
      }
      if (ch == ')' || ch == ']' || ch == '}') {
        if (depth > 0) {
          --depth;
        }
        continue;
      }
      if (depth == 0) {
        result += ch;
      }
    }
    return result;
  }

  /// Underscores to spaces, runs of whitespace collapsed, stray separators trimmed.
  inline std::string tidy_name(std::string_view text) {
    std::string result;
    bool pending_space = false;
    for (const char ch : text) {
      const bool space = ch == '_' || std::isspace(static_cast<unsigned char>(ch));
      if (space) {
        pending_space = !result.empty();
        continue;
      }
      if (pending_space) {
        result += ' ';
        pending_space = false;
      }
      result += ch;
    }
    const auto is_separator = [](char ch) {
      return ch == '-' || ch == ':' || ch == ' ' || ch == ',';
    };
    while (!result.empty() && is_separator(result.back())) {
      result.pop_back();
    }
    std::size_t front = 0;
    while (front < result.size() && (result[front] == '-' || result[front] == ' ')) {
      ++front;
    }
    return result.substr(front);
  }

  /**
   * @brief A numbered set's catalogue index, dropped: "0001 - Super Mario 64 DS" is that game.
   *
   * Some ROM sets prefix every file with its number in the set. The number says nothing about the
   * game and it sorts a library by an order nobody chose.
   *
   * Two real titles decide how careful this has to be. "1943 - The Battle of Midway" is a year, and
   * "007 - GoldenEye" is a name, and stripping either leaves a different game's title behind. So an
   * index has to be padded with a leading zero, which a year never is, and four or five digits long,
   * which the Bond number is not. Sets pad to four.
   *
   * The leading zero is what costs something: a set with more than 999 entries keeps the index on
   * everything from 1000 on, so those names are searched for with a catalogue number in them. Fixing
   * that means telling 1000 the index from 1943 the year, which nothing in a filename can do.
   */
  inline std::string strip_set_number(std::string_view name) {
    if (name.empty() || name.front() != '0') {
      return std::string(name);
    }
    std::size_t digits = 0;
    while (digits < name.size() && std::isdigit(static_cast<unsigned char>(name[digits]))) {
      ++digits;
    }
    if (digits < 4 || digits > 5) {
      return std::string(name);
    }
    auto rest = name.substr(digits);
    if (rest.rfind(" - ", 0) == 0) {
      rest.remove_prefix(3);
    }
    else if (rest.rfind("- ", 0) == 0 || rest.rfind(". ", 0) == 0) {
      rest.remove_prefix(2);
    }
    else if (rest.rfind("-", 0) == 0) {
      rest.remove_prefix(1);
    }
    else {
      // A number that runs straight into the title is part of it.
      return std::string(name);
    }
    while (!rest.empty() && rest.front() == ' ') {
      rest.remove_prefix(1);
    }
    return rest.empty() ? std::string(name) : std::string(rest);
  }

  /**
   * @brief "Legend of Zelda, The - Breath of the Wild" reads as "The Legend of Zelda - Breath of the Wild".
   *
   * Sorted ROM sets move the article behind a comma; a launcher grid wants it in front.
   */
  inline std::string restore_leading_article(std::string name) {
    static constexpr std::string_view articles[] = {"The", "A", "An"};
    const auto lowered = lower_copy(name);
    for (const auto article : articles) {
      const auto marker = ", " + lower_copy(article);
      const auto pos = lowered.find(marker);
      if (pos == std::string::npos || pos == 0) {
        continue;
      }
      const auto after = pos + marker.size();
      const bool at_end = after == lowered.size();
      const bool before_subtitle = !at_end && (lowered.compare(after, 3, " - ") == 0 || lowered.compare(after, 2, ": ") == 0);
      if (!at_end && !before_subtitle) {
        continue;
      }
      return std::string(article) + " " + name.substr(0, pos) + name.substr(after);
    }
    return name;
  }

  /// The entry name a ROM file gets: tags gone, underscores gone, the article back in front.
  inline std::string display_name(const std::filesystem::path &rom) {
    const auto stem = rom.stem().string();
    auto name = strip_set_number(tidy_name(strip_bracket_groups(stem)));
    if (name.empty()) {
      name = tidy_name(stem);
    }
    if (name.empty()) {
      name = rom.filename().string();
    }
    return restore_leading_article(name);
  }

  /**
   * @brief Update and DLC dumps sit next to the game and must not become entries.
   *
   * Switch sets tag them `[v65536]` (updates are multiples of 65536; `[v0]` is the
   * game), other sets spell it out, and some keep them in a folder of their own.
   * A `(v1.1)` or `(Rev 1)` is a revision of the game itself and stays.
   */
  inline bool looks_like_update_or_dlc(const std::filesystem::path &rom, const std::filesystem::path &folder) {
    for (const auto &group : bracket_groups(rom.stem().string())) {
      const auto tag = lower_copy(trim_view(group));
      if (tag.find("update") != std::string::npos || tag.find("dlc") != std::string::npos ||
          tag.find("patch") != std::string::npos || tag == "upd") {
        return true;
      }
      if (tag.size() > 1 && tag.front() == 'v' && std::all_of(tag.begin() + 1, tag.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
          })) {
        try {
          if (std::stoull(tag.substr(1)) >= 65536) {
            return true;
          }
        } catch (...) {
        }
      }
    }
    static constexpr std::string_view update_folders[] = {"update", "updates", "dlc", "dlcs", "patch", "patches", "mods"};
    const auto relative = rom.lexically_relative(folder);
    const auto parent = relative.empty() ? rom.parent_path() : relative.parent_path();
    for (const auto &component : parent) {
      const auto lowered = lower_copy(component.string());
      if (std::find(std::begin(update_folders), std::end(update_folders), lowered) != std::end(update_folders)) {
        return true;
      }
    }
    return false;
  }

  /// Lower case, no leading dot, letters and digits only, duplicates dropped.
  inline std::vector<std::string> normalize_extensions(const std::vector<std::string> &raw) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto &entry : raw) {
      std::string cleaned;
      for (const char ch : lower_copy(trim_view(entry))) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
          cleaned += ch;
        }
      }
      if (!cleaned.empty() && seen.insert(cleaned).second) {
        result.push_back(cleaned);
      }
    }
    return result;
  }

  /// "nsp, xci .nca" and friends into a clean list.
  inline std::vector<std::string> parse_extension_list(std::string_view text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : text) {
      if (ch == ',' || ch == ';' || std::isspace(static_cast<unsigned char>(ch))) {
        if (!current.empty()) {
          parts.push_back(current);
          current.clear();
        }
        continue;
      }
      current += ch;
    }
    if (!current.empty()) {
      parts.push_back(current);
    }
    return normalize_extensions(parts);
  }

  inline bool has_extension(const std::filesystem::path &file, const std::vector<std::string> &extensions) {
    auto extension = lower_copy(file.extension().string());
    if (!extension.empty() && extension.front() == '.') {
      extension.erase(0, 1);
    }
    return !extension.empty() && std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
  }

  struct rom_t {
    std::filesystem::path path;
    std::string name;
  };

  /**
   * @brief The games in a folder: matching files, hidden entries and archives skipped,
   * update and DLC dumps skipped, one entry per name so a second region dump of the
   * same game does not become a duplicate.
   */
  inline std::vector<rom_t> scan_folder(
    const std::filesystem::path &folder,
    const std::vector<std::string> &extensions,
    std::size_t max_depth = max_scan_depth,
    std::size_t limit = max_roms_per_folder
  ) {
    std::vector<rom_t> roms;
    std::error_code error;
    if (extensions.empty() || !std::filesystem::is_directory(folder, error)) {
      return roms;
    }

    std::vector<std::filesystem::path> files;
    std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end) {
      const auto &entry = *it;
      const auto filename = entry.path().filename().string();
      const bool hidden = !filename.empty() && filename.front() == '.';
      std::error_code type_error;
      if (entry.is_directory(type_error)) {
        if (hidden || it.depth() + 1 > static_cast<int>(max_depth)) {
          it.disable_recursion_pending();
        }
      } else if (!hidden && entry.is_regular_file(type_error) && has_extension(entry.path(), extensions) &&
                 !looks_like_update_or_dlc(entry.path(), folder)) {
        files.push_back(entry.path());
        if (files.size() >= limit) {
          break;
        }
      }
      it.increment(error);
    }

    std::sort(files.begin(), files.end());
    std::set<std::string> seen_names;
    for (const auto &file : files) {
      auto name = display_name(file);
      if (!seen_names.insert(lower_copy(name)).second) {
        continue;
      }
      roms.push_back({file, std::move(name)});
    }
    std::sort(roms.begin(), roms.end(), [](const rom_t &a, const rom_t &b) {
      const auto left = lower_copy(a.name);
      const auto right = lower_copy(b.name);
      return left != right ? left < right : a.path < b.path;
    });
    return roms;
  }


  /// The image types a cover next to a game may use, in the order they are tried.
  inline constexpr std::string_view cover_extensions[] = {".png", ".jpg", ".jpeg", ".webp"};

  /// libretro names its thumbnails after the game with `&*/:`<>?\|` replaced by `_`.
  inline std::string libretro_thumbnail_stem(std::string_view stem) {
    std::string result(stem);
    for (auto &ch : result) {
      if (std::string_view("&*/:`<>?\\|").find(ch) != std::string_view::npos) {
        ch = '_';
      }
    }
    return result;
  }

  /**
   * @brief Where a cover for this game may already sit, in priority order, by the raw file stem.
   *
   * Next to the game first (`<stem>.png`, then `covers`, `boxart` and `media` beside it), then
   * the ES-DE media the folder's systems download, then RetroArch's boxarts for those systems
   * under both the native and the Flatpak config roots. Nothing is checked for existence here.
   */
  inline std::vector<std::filesystem::path> cover_candidates(
    const std::filesystem::path &rom,
    const preset_t *preset,
    const std::vector<std::filesystem::path> &home_roots
  ) {
    std::vector<std::filesystem::path> candidates;
    const auto stem = rom.stem().string();
    const auto directory = rom.parent_path();
    const auto with_extensions = [&](const std::filesystem::path &base) {
      for (const auto extension : cover_extensions) {
        candidates.emplace_back(base.string() + std::string(extension));
      }
    };
    with_extensions(directory / stem);
    for (const auto sub : {"covers", "boxart", "media"}) {
      with_extensions(directory / sub / stem);
    }
    if (preset == nullptr) {
      return candidates;
    }
    const auto libretro_stem = libretro_thumbnail_stem(stem);
    for (const auto &home : home_roots) {
      for (const auto system : preset->es_systems) {
        with_extensions(home / "ES-DE" / "downloaded_media" / std::filesystem::path(system) / "covers" / stem);
        with_extensions(home / ".var" / "app" / "org.es_de.frontend" / "ES-DE" / "downloaded_media" / std::filesystem::path(system) / "covers" / stem);
      }
      std::vector<std::string> boxart_names {stem};
      if (libretro_stem != stem) {
        boxart_names.push_back(libretro_stem);
      }
      for (const auto system : preset->retroarch_systems) {
        for (const auto &name : boxart_names) {
          candidates.push_back(home / ".config" / "retroarch" / "thumbnails" / std::filesystem::path(system) / "Named_Boxarts" / (name + ".png"));
          candidates.push_back(home / ".var" / "app" / "org.libretro.RetroArch" / "config" / "retroarch" / "thumbnails" / std::filesystem::path(system) / "Named_Boxarts" / (name + ".png"));
        }
      }
    }
    return candidates;
  }

  /// The first candidate that exists and passes the caller's image test.
  template<typename Predicate>
  inline std::optional<std::filesystem::path> find_local_cover(
    const std::filesystem::path &rom,
    const preset_t *preset,
    const std::vector<std::filesystem::path> &home_roots,
    Predicate is_image
  ) {
    for (const auto &candidate : cover_candidates(rom, preset, home_roots)) {
      std::error_code error;
      if (std::filesystem::is_regular_file(candidate, error) && is_image(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  /// A filesystem-safe name for the copied cover: the emulator, the stem, and a hash of the path.
  inline std::string cover_stem(const std::filesystem::path &rom, std::string_view emulator) {
    std::string safe;
    for (const char ch : rom.stem().string()) {
      const auto byte = static_cast<unsigned char>(ch);
      safe += std::isalnum(byte) || ch == '.' || ch == '_' || ch == '-' ? ch : '_';
    }
    if (safe.size() > 96) {
      safe.resize(96);
    }
    if (safe.empty()) {
      safe = "rom";
    }
    char hash[17];
    std::snprintf(hash, sizeof(hash), "%08zx", static_cast<std::size_t>(std::hash<std::string> {}(rom.lexically_normal().string())) & 0xffffffffu);
    return "emulator_" + std::string(emulator) + "_" + safe + "_" + hash;
  }


  /// One thing an emulator still needs before a game boots, in the words the console shows.
  struct prerequisite_t {
    std::string id;
    std::string severity;  ///< "warning" when no game will boot, "info" when only some will
    std::string message;
    std::string action;
  };

  inline std::optional<std::string> read_text_file(const std::filesystem::path &path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
      return std::nullopt;
    }
    std::ifstream in {path, std::ios::binary};
    if (!in) {
      return std::nullopt;
    }
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }

  /// The `filesystems=` grants of a Flatpak metadata or overrides file, in order, suffixes dropped.
  inline std::vector<std::string> flatpak_filesystem_grants(std::string_view text) {
    std::vector<std::string> grants;
    bool in_context = false;
    std::size_t start = 0;
    while (start <= text.size()) {
      const auto end = text.find('\n', start);
      const auto line = trim_view(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
      if (!line.empty() && line.front() == '[') {
        in_context = line == "[Context]";
      } else if (in_context && line.rfind("filesystems=", 0) == 0) {
        const auto values = line.substr(std::string_view("filesystems=").size());
        std::size_t value_start = 0;
        while (value_start <= values.size()) {
          const auto value_end = values.find(';', value_start);
          auto value = trim_view(values.substr(value_start, value_end == std::string_view::npos ? std::string_view::npos : value_end - value_start));
          for (const std::string_view suffix : {":ro", ":rw", ":create"}) {
            if (value.size() > suffix.size() && value.substr(value.size() - suffix.size()) == suffix) {
              value.remove_suffix(suffix.size());
              break;
            }
          }
          if (!value.empty()) {
            grants.emplace_back(value);
          }
          if (value_end == std::string_view::npos) {
            break;
          }
          value_start = value_end + 1;
        }
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return grants;
  }

  /// Lexical containment, no filesystem access: `/a/b/c` is under `/a/b`, not under `/a/bc`.
  inline bool path_is_under(const std::filesystem::path &path, const std::filesystem::path &root) {
    const auto normal_path = path.lexically_normal();
    auto normal_root = root.lexically_normal();
    if (!normal_root.empty() && !normal_root.has_filename()) {
      normal_root = normal_root.parent_path();
    }
    if (normal_root.empty()) {
      return false;
    }
    const auto root_count = std::distance(normal_root.begin(), normal_root.end());
    const auto path_count = std::distance(normal_path.begin(), normal_path.end());
    if (path_count < root_count) {
      return false;
    }
    return std::equal(normal_root.begin(), normal_root.end(), normal_path.begin());
  }

  /**
   * @brief Whether a Flatpak with these grants may read the folder.
   *
   * Grants apply in order, so an override read after the app's metadata wins, and a `!path`
   * takes a folder away again. `host` is everything; `home` and `~/...` follow each home
   * root; `xdg-*` names map to their usual folders under each home.
   */
  inline bool flatpak_can_read(const std::vector<std::string> &grants, const std::filesystem::path &folder, const std::vector<std::filesystem::path> &home_roots) {
    static constexpr std::pair<std::string_view, std::string_view> xdg_names[] = {
      {"xdg-download", "Downloads"}, {"xdg-documents", "Documents"}, {"xdg-music", "Music"},
      {"xdg-pictures", "Pictures"}, {"xdg-videos", "Videos"}, {"xdg-desktop", "Desktop"},
      {"xdg-public-share", "Public"}, {"xdg-templates", "Templates"},
      {"xdg-data", ".local/share"}, {"xdg-config", ".config"}, {"xdg-cache", ".cache"},
    };
    bool allowed = false;
    for (const auto &grant : grants) {
      const bool negate = !grant.empty() && grant.front() == '!';
      const std::string_view value = negate ? std::string_view(grant).substr(1) : std::string_view(grant);
      bool matches = false;
      if (value == "host") {
        matches = true;
      } else if (value == "home" || value == "~") {
        for (const auto &home : home_roots) {
          matches = matches || path_is_under(folder, home);
        }
      } else if (value.rfind("~/", 0) == 0) {
        for (const auto &home : home_roots) {
          matches = matches || path_is_under(folder, home / std::filesystem::path(value.substr(2)));
        }
      } else if (!value.empty() && value.front() == '/') {
        matches = path_is_under(folder, std::filesystem::path(value));
      } else if (value.rfind("xdg-", 0) == 0) {
        const auto slash = value.find('/');
        const auto name = value.substr(0, slash);
        const auto rest = slash == std::string_view::npos ? std::string_view {} : value.substr(slash + 1);
        for (const auto &[xdg, directory] : xdg_names) {
          if (name != xdg) {
            continue;
          }
          for (const auto &home : home_roots) {
            auto base = home / std::filesystem::path(directory);
            if (!rest.empty()) {
              base /= std::filesystem::path(rest);
            }
            matches = matches || path_is_under(folder, base);
          }
        }
      }
      if (matches) {
        allowed = !negate;
      }
    }
    return allowed;
  }

  /**
   * @brief The grants a Flatpak actually runs with: its metadata, then the global and per-app
   * overrides of its installation, then the user's overrides. Nothing when no metadata is readable.
   */
  inline std::optional<std::vector<std::string>> flatpak_effective_grants(
    std::string_view flatpak_id,
    const std::vector<std::filesystem::path> &home_roots,
    const std::filesystem::path &system_flatpak_root = "/var/lib/flatpak"
  ) {
    const std::filesystem::path id {flatpak_id};
    std::vector<std::filesystem::path> roots;
    for (const auto &home : home_roots) {
      roots.push_back(home / ".local" / "share" / "flatpak");
    }
    roots.push_back(system_flatpak_root);
    for (const auto &root : roots) {
      const auto metadata = read_text_file(root / "app" / id / "current" / "active" / "metadata");
      if (!metadata) {
        continue;
      }
      auto grants = flatpak_filesystem_grants(*metadata);
      const auto append_overrides = [&](const std::filesystem::path &overrides) {
        for (const auto &name : {std::filesystem::path("global"), id}) {
          if (const auto text = read_text_file(overrides / name); text) {
            for (auto &grant : flatpak_filesystem_grants(*text)) {
              grants.push_back(std::move(grant));
            }
          }
        }
      };
      append_overrides(root / "overrides");
      for (const auto &home : home_roots) {
        const auto user_overrides = home / ".local" / "share" / "flatpak" / "overrides";
        if (user_overrides != root / "overrides") {
          append_overrides(user_overrides);
        }
      }
      return grants;
    }
    return std::nullopt;
  }

  /**
   * @brief What the emulator still lacks before a game in this folder boots.
   *
   * Keys and BIOS images are looked for where each emulator keeps them, native, Flatpak and
   * next to a portable launcher; a Flatpak is also checked for permission to read the folder.
   * An emulator that is not installed at all is reported by the folder itself, not here.
   */
  inline std::vector<prerequisite_t> prerequisites(
    const preset_t &preset,
    const install_t &install,
    const std::filesystem::path &folder,
    const std::vector<std::filesystem::path> &home_roots,
    const std::filesystem::path &system_flatpak_root = "/var/lib/flatpak"
  ) {
    std::vector<prerequisite_t> found;
    if (install.kind == install_e::missing) {
      return found;
    }
    const auto launcher_dir = install.kind == install_e::launcher ? std::filesystem::path(install.location).parent_path() : std::filesystem::path();
    const auto any_file = [](const std::vector<std::filesystem::path> &paths) {
      std::error_code error;
      return std::any_of(paths.begin(), paths.end(), [&](const std::filesystem::path &path) {
        return std::filesystem::is_regular_file(path, error);
      });
    };
    const auto any_file_in = [](const std::vector<std::filesystem::path> &directories, std::string_view extension) {
      std::error_code error;
      for (const auto &directory : directories) {
        if (!std::filesystem::is_directory(directory, error)) {
          continue;
        }
        for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
          if (!entry.is_regular_file(error)) {
            continue;
          }
          if (extension.empty() || lower_copy(entry.path().extension().string()) == extension) {
            return true;
          }
        }
      }
      return false;
    };
    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> directories;
    if (preset.id == "eden") {
      for (const auto &home : home_roots) {
        files.push_back(home / ".local/share/eden/keys/prod.keys");
        files.push_back(home / ".var/app/dev.eden_emu.eden/data/eden/keys/prod.keys");
      }
      if (!launcher_dir.empty()) {
        files.push_back(launcher_dir / "user/keys/prod.keys");
      }
      if (!any_file(files)) {
        found.push_back({"eden_keys_missing", "warning", "Eden has no prod.keys, so no game will boot.",
                         "Copy your prod.keys to ~/.local/share/eden/keys/ (Flatpak: ~/.var/app/dev.eden_emu.eden/data/eden/keys/)."});
      }
    } else if (preset.id == "cemu") {
      for (const auto &home : home_roots) {
        files.push_back(home / ".local/share/Cemu/keys.txt");
        files.push_back(home / ".config/Cemu/keys.txt");
        files.push_back(home / ".var/app/info.cemu.Cemu/data/Cemu/keys.txt");
      }
      if (!launcher_dir.empty()) {
        files.push_back(launcher_dir / "keys.txt");
      }
      if (!any_file(files)) {
        found.push_back({"cemu_keys_missing", "info", "Cemu has no keys.txt, so encrypted dumps (wud, wux) will not load; wua and rpx will.",
                         "Put keys.txt in ~/.local/share/Cemu/ (Flatpak: ~/.var/app/info.cemu.Cemu/data/Cemu/)."});
      }
    } else if (preset.id == "duckstation") {
      for (const auto &home : home_roots) {
        directories.push_back(home / ".local/share/duckstation/bios");
        directories.push_back(home / ".var/app/org.duckstation.DuckStation/data/duckstation/bios");
      }
      if (!launcher_dir.empty()) {
        directories.push_back(launcher_dir / "bios");
      }
      if (!any_file_in(directories, ".bin")) {
        found.push_back({"duckstation_bios_missing", "warning", "DuckStation has no BIOS image, so no game will boot.",
                         "Copy a PlayStation BIOS (.bin) to ~/.local/share/duckstation/bios/ (Flatpak: ~/.var/app/org.duckstation.DuckStation/data/duckstation/bios/)."});
      }
    } else if (preset.id == "pcsx2") {
      for (const auto &home : home_roots) {
        directories.push_back(home / ".config/PCSX2/bios");
        directories.push_back(home / ".var/app/net.pcsx2.PCSX2/config/PCSX2/bios");
      }
      if (!launcher_dir.empty()) {
        directories.push_back(launcher_dir / "bios");
      }
      if (!any_file_in(directories, "")) {
        found.push_back({"pcsx2_bios_missing", "warning", "PCSX2 has no BIOS image, so no game will boot.",
                         "Copy a PlayStation 2 BIOS to ~/.config/PCSX2/bios/ (Flatpak: ~/.var/app/net.pcsx2.PCSX2/config/PCSX2/bios/)."});
      }
    }
    if (install.kind == install_e::flatpak && !folder.empty()) {
      if (const auto grants = flatpak_effective_grants(preset.flatpak_id, home_roots, system_flatpak_root);
          grants && !flatpak_can_read(*grants, folder, home_roots)) {
        found.push_back({"flatpak_folder_not_visible", "warning",
                         std::string(preset.label) + "'s Flatpak cannot see " + folder.string() + ", so it cannot open the games.",
                         "flatpak override --user --filesystem=" + shell_quote(folder.string()) + " " + std::string(preset.flatpak_id)});
      }
    }
    return found;
  }

  /// One registered folder, as stored in library_sources.json.
  struct source_t {
    std::string id;
    std::string path;
    std::string emulator;  ///< a preset id, or custom_emulator_id
    std::string launcher;  ///< optional: the emulator file to run instead of PATH or Flatpak
    std::string command;  ///< custom only: the template with rom_placeholder
    std::vector<std::string> extensions;  ///< custom: required; presets: optional override
  };

  inline std::vector<source_t> parse_sources(std::string_view json_text) {
    std::vector<source_t> sources;
    const auto document = nlohmann::json::parse(json_text, nullptr, false);
    if (document.is_discarded() || !document.is_object() || !document.contains("sources") || !document["sources"].is_array()) {
      return sources;
    }
    const auto string_member = [](const nlohmann::json &node, const char *key) {
      const auto it = node.find(key);
      return it != node.end() && it->is_string() ? it->get<std::string>() : std::string {};
    };
    for (const auto &node : document["sources"]) {
      if (!node.is_object()) {
        continue;
      }
      source_t source;
      source.id = string_member(node, "id");
      source.path = string_member(node, "path");
      source.emulator = string_member(node, "emulator");
      source.launcher = string_member(node, "launcher");
      source.command = string_member(node, "command");
      if (const auto it = node.find("extensions"); it != node.end() && it->is_array()) {
        std::vector<std::string> raw;
        for (const auto &extension : *it) {
          if (extension.is_string()) {
            raw.push_back(extension.get<std::string>());
          }
        }
        source.extensions = normalize_extensions(raw);
      }
      if (source.id.empty() || source.path.empty() || source.emulator.empty()) {
        continue;
      }
      sources.push_back(std::move(source));
    }
    return sources;
  }

  inline nlohmann::json serialize_sources(const std::vector<source_t> &sources) {
    nlohmann::json document;
    document["version"] = sources_file_version;
    document["sources"] = nlohmann::json::array();
    for (const auto &source : sources) {
      document["sources"].push_back({
        {"id", source.id},
        {"path", source.path},
        {"emulator", source.emulator},
        {"launcher", source.launcher},
        {"command", source.command},
        {"extensions", source.extensions},
      });
    }
    return document;
  }

  /// The extensions a folder is scanned for: the user's list when given, else the preset's.
  inline std::vector<std::string> effective_extensions(const source_t &source, const preset_t *preset) {
    if (!source.extensions.empty() || preset == nullptr) {
      return source.extensions;
    }
    std::vector<std::string> extensions;
    for (const auto extension : preset->extensions) {
      extensions.emplace_back(extension);
    }
    return extensions;
  }

  /// The reason a folder cannot be registered, in the words the console shows.
  inline std::optional<std::string> validate_source(const source_t &source) {
    if (source.id.empty()) {
      return "The folder has no id";
    }
    const std::filesystem::path folder {source.path};
    if (source.path.empty() || !folder.is_absolute()) {
      return "Give the folder as an absolute path, or start it with ~/";
    }
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
      return "Folder not found on this host: " + source.path;
    }
    if (!source.launcher.empty()) {
      const std::filesystem::path launcher {source.launcher};
      if (!launcher.is_absolute() || !std::filesystem::is_regular_file(launcher, error)) {
        return "Emulator not found at " + source.launcher;
      }
    }
    if (source.emulator == custom_emulator_id) {
      if (!custom_template_valid(source.command)) {
        return "A custom command names the emulator and carries {rom} exactly once, for example: retroarch -f -L ~/.config/retroarch/cores/snes9x_libretro.so {rom}";
      }
      if (source.extensions.empty()) {
        return "List the file extensions to look for, for example: sfc, smc, zip";
      }
      return std::nullopt;
    }
    if (find_preset(source.emulator) == nullptr) {
      return "Unknown emulator: " + source.emulator;
    }
    return std::nullopt;
  }

  /// True when the file sits under the folder; symlinks are resolved on both sides first.
  inline bool rom_belongs_to_folder(const std::filesystem::path &rom, const std::filesystem::path &folder) {
    std::error_code error;
    const auto canonical_folder = std::filesystem::weakly_canonical(folder, error);
    if (error || canonical_folder.empty()) {
      return false;
    }
    const auto canonical_rom = std::filesystem::weakly_canonical(rom, error);
    if (error || canonical_rom.empty()) {
      return false;
    }
    const auto folder_end = std::distance(canonical_folder.begin(), canonical_folder.end());
    const auto rom_end = std::distance(canonical_rom.begin(), canonical_rom.end());
    if (rom_end <= folder_end) {
      return false;
    }
    return std::equal(canonical_folder.begin(), canonical_folder.end(), canonical_rom.begin());
  }

  inline std::string source_label(const source_t &source, const preset_t *preset) {
    if (preset != nullptr) {
      return std::string(preset->label);
    }
    return source.emulator == custom_emulator_id ? "Custom command" : source.emulator;
  }

  /// The folder list lives next to apps.json, wherever that is: the folders belong with the apps they feed.
  inline std::filesystem::path sources_path_for_apps_file(const std::filesystem::path &apps_file, const std::filesystem::path &fallback_directory) {
    const auto directory = apps_file.has_parent_path() ? apps_file.parent_path() : fallback_directory;
    return directory / "library_sources.json";
  }

  /// The emulator file a registered folder names, or empty when it names none or is not registered.
  inline std::string configured_launcher_for(const std::vector<source_t> &sources, std::string_view source_id) {
    source_id = trim_view(source_id);
    if (source_id.empty()) {
      return {};
    }
    const auto it = std::find_if(sources.begin(), sources.end(), [&](const source_t &source) {
      return source.id == source_id;
    });
    return it == sources.end() ? std::string {} : it->launcher;
  }

  /// The path a token holds when it is exactly one path as shell_quote writes it.
  inline std::optional<std::string> single_quoted_path(std::string_view token) {
    if (token.size() < 2 || token.front() != '\'' || token.back() != '\'') {
      return std::nullopt;
    }
    std::string inner;
    std::size_t start = 1;
    while (true) {
      const auto close = token.find('\'', start);
      if (close == std::string_view::npos) {
        return std::nullopt;
      }
      inner.append(token.substr(start, close - start));
      if (close == token.size() - 1) {
        return shell_quote(inner) == token ? std::optional<std::string>(inner) : std::nullopt;
      }
      if (token.substr(close, 4) != "'\\''") {
        return std::nullopt;
      }
      inner.push_back('\'');
      start = close + 4;
    }
  }

  /// Whether a token is exactly one path as shell_quote writes it.
  inline bool single_quoted_token(std::string_view token) {
    return single_quoted_path(token).has_value();
  }

  /**
   * @brief Whether a saved entry command is one Polaris wrote at import, under any install.
   *
   * Import writes the launch for the install the host had then: an emulator file, a
   * binary on PATH, the Flatpak, or the bare binary name while the emulator was missing,
   * followed by the preset's arguments for the game. A quoted emulator file counts when the
   * folder names that file, or when the file is gone, since the folder may have been added again
   * with another one. A command the player edited (other flags, a wrapper in front, an emulator
   * file of their own that is there) matches none of them and is theirs: launch neither replaces
   * nor refuses it.
   */
  inline bool generated_entry_command(
    const preset_t &preset,
    std::string_view rom_path,
    std::string_view saved_command,
    std::string_view configured_launcher = {}
  ) {
    const auto saved = trim_view(saved_command);
    const auto rom = trim_view(rom_path);
    std::string_view emulator = saved;
    if (!rom.empty()) {
      const auto arguments = " " + substitute_rom(preset.arguments.empty() ? rom_placeholder : preset.arguments, std::filesystem::path(rom));
      if (saved.size() <= arguments.size() || saved.substr(saved.size() - arguments.size()) != arguments) {
        return false;
      }
      emulator = saved.substr(0, saved.size() - arguments.size());
    }
    if (emulator == "flatpak run " + std::string(preset.flatpak_id)) {
      return !preset.flatpak_id.empty();
    }
    if (std::find(preset.binaries.begin(), preset.binaries.end(), emulator) != preset.binaries.end()) {
      return true;
    }
    // An emulator file, quoted the way import writes it. The folder's own launcher is Polaris's
    // to update. So is a file that is no longer there, because the folder may have been added
    // again with another emulator and no command can be running from a path that is gone. A
    // path that does exist and is not the folder's is the player's, edited in deliberately.
    const auto quoted = single_quoted_path(emulator);
    if (!quoted) {
      return false;
    }
    const auto launcher = trim_view(configured_launcher);
    if (!launcher.empty() && *quoted == launcher) {
      return true;
    }
    std::error_code error;
    return !std::filesystem::is_regular_file(std::filesystem::path(*quoted), error);
  }

  /// What an imported entry runs with its emulator as this host has it right now.
  struct entry_launch_t {
    const preset_t *preset = nullptr;
    install_t install;
    std::string command;  ///< empty while the emulator is missing
  };

  /**
   * @brief Resolve an imported entry's command against the emulator as it is installed now.
   *
   * The command saved at import names whatever was there then, and a missing emulator is
   * saved under its bare binary name, which a later Flatpak install never provides. The
   * game entries carry their file; the emulator's own entry carries none and gets the
   * launcher command. A custom template or an unknown emulator has nothing to resolve.
   */
  inline std::optional<entry_launch_t> resolve_entry_launch(
    std::string_view emulator_id,
    std::string_view rom_path,
    std::string_view configured_launcher,
    const std::vector<std::filesystem::path> &home_roots,
    std::string_view path_env,
    const std::filesystem::path &system_flatpak_root = "/var/lib/flatpak"
  ) {
    const auto *preset = find_preset(trim_view(emulator_id));
    if (preset == nullptr) {
      return std::nullopt;
    }
    entry_launch_t result;
    result.preset = preset;
    result.install = detect_install(*preset, configured_launcher, home_roots, path_env, system_flatpak_root);
    if (result.install.kind == install_e::missing) {
      return result;
    }
    const auto rom = trim_view(rom_path);
    result.command = rom.empty() ? launcher_command(*preset, result.install) : launch_command(*preset, result.install, std::filesystem::path(rom));
    return result;
  }

  /// Why an imported entry cannot start, as the one sentence and the one fix a client shows.
  struct missing_install_reason_t {
    std::string message;
    std::string action;
  };

  inline missing_install_reason_t missing_install_reason(const preset_t &preset, const install_t &install, std::string_view app_name) {
    const std::string label {preset.label};
    const auto name = trim_view(app_name);
    const std::string subject = name.empty() || name == preset.label ? std::string {} : ", so " + std::string(name) + " cannot start";
    if (!install.location.empty()) {
      return {
        label + " was not found at " + install.location + subject + ".",
        "Put " + label + " back at that path, or add the ROM folder again with the file where it is now, then launch again.",
      };
    }
    return {
      label + " is not installed on this host" + subject + ".",
      "Install " + label + " from ROM folders under Import Games in the Polaris web UI, then launch again.",
    };
  }
}  // namespace emulator_library
