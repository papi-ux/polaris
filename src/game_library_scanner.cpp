/**
 * @file src/game_library_scanner.cpp
 * @brief Helpers for discovering third-party launcher libraries.
 */
#include "game_library_scanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#if !defined(_WIN32)
  #include <pwd.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

namespace game_library {
  namespace {
    void append_unique_path(std::vector<std::filesystem::path> &paths, std::filesystem::path path) {
      if (path.empty()) {
        return;
      }

      path = path.lexically_normal();
      if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
        paths.push_back(std::move(path));
      }
    }

    std::string account_home_dir() {
    #if !defined(_WIN32)
      const auto *entry = getpwuid(getuid());
      if (entry && entry->pw_dir && *entry->pw_dir) {
        return entry->pw_dir;
      }
    #endif
      return {};
    }

    std::string trim_copy(std::string_view text) {
      auto begin = text.begin();
      auto end = text.end();

      while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
      }
      while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
        --end;
      }

      return std::string(begin, end);
    }

    std::string strip_inline_comment(std::string_view value) {
      char quote = '\0';
      bool seen_non_space = false;

      for (std::size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (!seen_non_space && std::isspace(static_cast<unsigned char>(ch))) {
          continue;
        }

        if (quote != '\0') {
          if (ch == quote) {
            quote = '\0';
          }
          continue;
        }

        if (!seen_non_space && (ch == '\'' || ch == '"')) {
          quote = ch;
          seen_non_space = true;
          continue;
        }

        seen_non_space = true;
        if (ch == '#' &&
            (i == 0 || std::isspace(static_cast<unsigned char>(value[i - 1])))) {
          return trim_copy(value.substr(0, i));
        }
      }

      return trim_copy(value);
    }

    std::string decode_yaml_scalar(std::string_view raw_value) {
      std::string value = strip_inline_comment(raw_value);
      if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '\'' && last == '\'') || (first == '"' && last == '"')) {
          value = value.substr(1, value.size() - 2);
        }
      }

      return trim_copy(value);
    }

    std::optional<std::pair<std::string, std::string>> parse_top_level_yaml_scalar(std::string_view raw_line) {
      if (raw_line.empty() || raw_line.front() == '#' ||
          std::isspace(static_cast<unsigned char>(raw_line.front()))) {
        return std::nullopt;
      }

      const auto separator = raw_line.find(':');
      if (separator == std::string_view::npos) {
        return std::nullopt;
      }

      auto key = trim_copy(raw_line.substr(0, separator));
      auto value = decode_yaml_scalar(raw_line.substr(separator + 1));
      if (key.empty() || value.empty()) {
        return std::nullopt;
      }

      return std::make_pair(std::move(key), std::move(value));
    }

    std::string slug_to_name(std::string slug) {
      std::replace(slug.begin(), slug.end(), '-', ' ');
      std::replace(slug.begin(), slug.end(), '_', ' ');
      return slug;
    }

    bool has_supported_image_extension(const std::filesystem::path &path) {
      auto extension = path.extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });

      return extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".webp";
    }

    std::string supported_image_path_or_empty(std::string path) {
      path = trim_copy(path);
      if (path.empty() || !has_supported_image_extension(path)) {
        return {};
      }

      return path;
    }

    void append_lutris_image_candidates(
      std::vector<std::filesystem::path> &candidates,
      const std::filesystem::path &root,
      const std::string &slug
    ) {
      const std::array<std::filesystem::path, 8> directories {
        root / "coverart",
        root / "banners",
        root / "icons",
        root / "steam/covers",
        root / "steam/banners",
        root / "steam/header",
        root / "gog/banners",
        root / "ubisoft/covers",
      };
      const std::array<std::string_view, 4> extensions {".jpg", ".jpeg", ".png", ".webp"};

      for (const auto &directory : directories) {
        for (const auto extension : extensions) {
          candidates.emplace_back(directory / (slug + std::string(extension)));
        }
      }
    }

    void append_unique_lutris_games(
      std::vector<lutris_game_t> &games,
      std::set<std::string> &seen_slugs,
      std::vector<lutris_game_t> discovered
    ) {
      for (auto &game : discovered) {
        if (!seen_slugs.insert(game.slug).second) {
          continue;
        }

        games.push_back(std::move(game));
      }
    }

    constexpr std::string_view heroic_flatpak_app_id = "com.heroicgameslauncher.hgl";
    constexpr std::string_view lutris_flatpak_app_id = "net.lutris.Lutris";

    /**
     * @brief Whether PATH holds an executable by this name.
     *
     * A Flatpak-only host has the launcher installed and no such binary, and asking before
     * spawning keeps the scan from paying for a shell that can only fail.
     */
    bool binary_on_path(std::string_view name) {
    #if defined(_WIN32)
      (void) name;
      return false;
    #else
      const char *path_env = std::getenv("PATH");
      if (!path_env || !*path_env) {
        return false;
      }

      std::string_view remaining(path_env);
      while (true) {
        const auto separator = remaining.find(':');
        const auto entry = remaining.substr(0, separator);
        if (!entry.empty()) {
          const auto candidate = (std::filesystem::path(entry) / name).string();
          if (::access(candidate.c_str(), X_OK) == 0) {
            return true;
          }
        }

        if (separator == std::string_view::npos) {
          return false;
        }
        remaining.remove_prefix(separator + 1);
      }
    #endif
    }

    struct heroic_config_root_t {
      std::filesystem::path path;
      launcher_install_t install = launcher_install_t::native;
    };

    /** @brief Heroic's config directory for every install we can see, native first. */
    std::vector<heroic_config_root_t> heroic_config_roots(const std::vector<std::filesystem::path> &home_roots) {
      std::vector<heroic_config_root_t> roots;
      std::set<std::filesystem::path> seen;
      const auto append = [&roots, &seen](std::filesystem::path path, launcher_install_t install) {
        if (path.empty()) {
          return;
        }

        path = path.lexically_normal();
        if (seen.insert(path).second) {
          roots.push_back(heroic_config_root_t {std::move(path), install});
        }
      };

      for (const auto &home : home_roots) {
        if (home.empty()) {
          continue;
        }

        append(home / ".config" / "heroic", launcher_install_t::native);
        append(
          home / ".var" / "app" / std::filesystem::path(heroic_flatpak_app_id) / "config" / "heroic",
          launcher_install_t::flatpak
        );
      }

      return roots;
    }

    std::string read_lutris_list_games_json() {
    #ifdef __linux__
      // Flatpak-only hosts have no lutris binary. Their games come from the directory scan,
      // which reads the same YAML out of the Flatpak home instead of shelling out.
      if (!binary_on_path("lutris")) {
        return {};
      }

      std::string output;
      std::array<char, 4096> buffer {};
      FILE *pipe = popen("lutris --list-games --json 2>/dev/null", "r");
      if (!pipe) {
        return output;
      }

      while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
      }
      pclose(pipe);
      return output;
    #else
      return {};
    #endif
    }
  }  // namespace

  bool is_lutris_slug_safe(const std::string &slug) {
    if (slug.empty()) {
      return false;
    }

    return std::all_of(slug.begin(), slug.end(), [](unsigned char ch) {
      return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
    });
  }

  std::vector<std::filesystem::path> library_home_roots(std::string_view runtime_home, std::string_view account_home) {
    std::vector<std::filesystem::path> roots;
    append_unique_path(roots, std::filesystem::path(runtime_home));
    append_unique_path(roots, std::filesystem::path(account_home));
    return roots;
  }

  std::vector<std::filesystem::path> library_home_roots() {
    const char *home = std::getenv("HOME");
    return library_home_roots(home ? std::string_view(home) : std::string_view(), account_home_dir());
  }

  std::string lutris_launch_command(const std::string &slug) {
    return lutris_launch_command(slug, launcher_install_t::native);
  }

  std::string lutris_launch_command(const std::string &slug, launcher_install_t install) {
    const auto uri = "lutris:rungame/" + slug;
    if (install == launcher_install_t::flatpak) {
      return "setsid flatpak run " + std::string(lutris_flatpak_app_id) + " " + uri;
    }

    return "setsid lutris " + uri;
  }

  std::vector<std::filesystem::path> lutris_game_config_dirs(const std::vector<std::filesystem::path> &home_roots) {
    std::vector<std::filesystem::path> dirs;
    const char *xdg_config_home = std::getenv("XDG_CONFIG_HOME");
    const char *xdg_data_home = std::getenv("XDG_DATA_HOME");
    if (xdg_config_home && *xdg_config_home) {
      append_unique_path(dirs, std::filesystem::path(xdg_config_home) / "lutris" / "games");
    }
    if (xdg_data_home && *xdg_data_home) {
      append_unique_path(dirs, std::filesystem::path(xdg_data_home) / "lutris" / "games");
    }

    for (const auto &home : home_roots) {
      if (home.empty()) {
        continue;
      }

      append_unique_path(dirs, home / ".config" / "lutris" / "games");
      append_unique_path(dirs, home / ".local" / "share" / "lutris" / "games");

      const auto flatpak_home = home / ".var" / "app" / std::filesystem::path(lutris_flatpak_app_id);
      append_unique_path(dirs, flatpak_home / "config" / "lutris" / "games");
      append_unique_path(dirs, flatpak_home / "data" / "lutris" / "games");
    }

    return dirs;
  }

  std::vector<std::filesystem::path> lutris_art_roots(const std::vector<std::filesystem::path> &home_roots) {
    std::vector<std::filesystem::path> roots;
    const char *xdg_data_home = std::getenv("XDG_DATA_HOME");
    const char *xdg_cache_home = std::getenv("XDG_CACHE_HOME");
    if (xdg_data_home && *xdg_data_home) {
      append_unique_path(roots, std::filesystem::path(xdg_data_home) / "lutris");
    }
    if (xdg_cache_home && *xdg_cache_home) {
      append_unique_path(roots, std::filesystem::path(xdg_cache_home) / "lutris");
    }

    for (const auto &home : home_roots) {
      if (home.empty()) {
        continue;
      }

      append_unique_path(roots, home / ".local" / "share" / "lutris");
      append_unique_path(roots, home / ".cache" / "lutris");

      const auto flatpak_home = home / ".var" / "app" / std::filesystem::path(lutris_flatpak_app_id);
      append_unique_path(roots, flatpak_home / "data" / "lutris");
      append_unique_path(roots, flatpak_home / "cache" / "lutris");
    }

    return roots;
  }

  bool path_is_under_flatpak_app(const std::filesystem::path &path, std::string_view app_id) {
    if (app_id.empty()) {
      return false;
    }

    const auto normalized = path.lexically_normal();
    for (auto part = normalized.begin(); part != normalized.end(); ++part) {
      if (part->string() != ".var") {
        continue;
      }

      const auto app_dir = std::next(part);
      if (app_dir == normalized.end() || app_dir->string() != "app") {
        continue;
      }

      const auto id = std::next(app_dir);
      if (id != normalized.end() && id->string() == app_id) {
        return true;
      }
    }

    return false;
  }

  std::vector<heroic_library_file_t> heroic_installed_files(const std::vector<std::filesystem::path> &home_roots) {
    std::vector<heroic_library_file_t> files;
    for (const auto &root : heroic_config_roots(home_roots)) {
      files.push_back(heroic_library_file_t {root.path / "gog_store" / "installed.json", "gog", root.install});
      files.push_back(heroic_library_file_t {
        root.path / "legendaryConfig" / "legendary" / "installed.json",
        "epic",
        root.install
      });
    }

    return files;
  }

  std::vector<heroic_library_file_t> heroic_cache_files(const std::vector<std::filesystem::path> &home_roots) {
    std::vector<heroic_library_file_t> files;
    for (const auto &root : heroic_config_roots(home_roots)) {
      files.push_back(heroic_library_file_t {root.path / "store_cache" / "gog_library.json", "gog", root.install});
      files.push_back(heroic_library_file_t {root.path / "store_cache" / "legendary_library.json", "epic", root.install});
    }

    return files;
  }

  std::string heroic_install_name(launcher_install_t install) {
    return install == launcher_install_t::flatpak ? "flatpak" : "native";
  }

  std::optional<launcher_install_t> heroic_install_from_name(std::string_view install) {
    if (install == "native") {
      return launcher_install_t::native;
    }
    if (install == "flatpak") {
      return launcher_install_t::flatpak;
    }
    return std::nullopt;
  }

  std::string heroic_runner_for_store(std::string_view store) {
    if (store == "gog") {
      return "gog";
    }
    if (store == "epic") {
      return "legendary";
    }
    return {};
  }

  std::optional<heroic_game_t> heroic_game_from_metadata(
    const std::string &app_name,
    const std::string &store,
    const std::string &runner,
    std::string_view install_name
  ) {
    const auto install = heroic_install_from_name(install_name);
    const auto expected_runner = heroic_runner_for_store(store);
    if (!install || expected_runner.empty() || runner != expected_runner ||
        !is_heroic_app_name_safe(app_name)) {
      return std::nullopt;
    }

    const auto command = heroic_launch_command(store, app_name, *install);
    if (command.empty()) {
      return std::nullopt;
    }

    return heroic_game_t {
      .app_name = app_name,
      .store = store,
      .runner = runner,
      .install = *install,
      .command = command,
    };
  }

  bool is_heroic_app_name_safe(const std::string &app_name) {
    if (app_name.empty()) {
      return false;
    }

    return std::all_of(app_name.begin(), app_name.end(), [](unsigned char ch) {
      return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.';
    });
  }

  std::string heroic_launch_command(const std::string &store, const std::string &app_name, launcher_install_t install) {
    // appName reaches a shell through the app's launch command. The store is never
    // interpolated: only the explicit Polaris store-to-Heroic-runner mapping is accepted.
    const auto runner = heroic_runner_for_store(store);
    if (runner.empty() || !is_heroic_app_name_safe(app_name)) {
      return {};
    }

    // Heroic's current shortcut contract is a query URI. Quote it because '&' is a shell
    // operator; the strict app-name and runner alphabets make the single quotes final.
    const auto uri = "'heroic://launch?appName=" + app_name + "&runner=" + runner + "'";
    if (install == launcher_install_t::flatpak) {
      return "setsid flatpak run " + std::string(heroic_flatpak_app_id) +
             " --no-gui --no-sandbox " + uri;
    }

    return "setsid heroic --no-gui --no-sandbox " + uri;
  }

  std::vector<std::string> heroic_launch_commands(const std::string &store, const std::string &app_name) {
    std::vector<std::string> commands;
    for (const auto install : {launcher_install_t::native, launcher_install_t::flatpak}) {
      auto command = heroic_launch_command(store, app_name, install);
      if (!command.empty()) {
        commands.push_back(std::move(command));
      }
    }

    // Older Polaris releases emitted Heroic's path-style URI. Keep these exact strings
    // solely for deduplication/migration; all new launches use the canonical query URI.
    if (!heroic_runner_for_store(store).empty() && is_heroic_app_name_safe(app_name)) {
      const auto legacy_uri = "heroic://launch/" + store + "/" + app_name;
      commands.push_back("setsid heroic " + legacy_uri);
      commands.push_back("setsid flatpak run " + std::string(heroic_flatpak_app_id) + " " + legacy_uri);
    }

    return commands;
  }

  std::optional<heroic_game_t> parse_legacy_heroic_launch_command(std::string_view command) {
    constexpr std::string_view native_prefix = "setsid heroic heroic://launch/";
    constexpr std::string_view flatpak_prefix =
      "setsid flatpak run com.heroicgameslauncher.hgl heroic://launch/";

    launcher_install_t install;
    if (command.starts_with(native_prefix)) {
      install = launcher_install_t::native;
      command.remove_prefix(native_prefix.size());
    } else if (command.starts_with(flatpak_prefix)) {
      install = launcher_install_t::flatpak;
      command.remove_prefix(flatpak_prefix.size());
    } else {
      return std::nullopt;
    }

    const auto separator = command.find('/');
    if (separator == std::string_view::npos || command.find('/', separator + 1) != std::string_view::npos) {
      return std::nullopt;
    }

    const std::string store {command.substr(0, separator)};
    const std::string app_name {command.substr(separator + 1)};
    const auto runner = heroic_runner_for_store(store);
    if (runner.empty() || !is_heroic_app_name_safe(app_name)) {
      return std::nullopt;
    }

    return heroic_game_t {
      .app_name = app_name,
      .store = store,
      .runner = runner,
      .install = install,
      .command = heroic_launch_command(store, app_name, install),
    };
  }

  namespace {
    std::optional<heroic_game_t> make_heroic_game(
      const std::string &app_name,
      const std::string &title,
      const std::string &store,
      launcher_install_t install,
      std::string poster_url = {},
      std::string hero_url = {}
    ) {
      const auto runner = heroic_runner_for_store(store);
      const auto command = heroic_launch_command(store, app_name, install);
      if (title.empty() || runner.empty() || command.empty()) {
        return std::nullopt;
      }

      return heroic_game_t {
        .name = title,
        .app_name = app_name,
        .store = store,
        .runner = runner,
        .install = install,
        .command = command,
        .poster_url = std::move(poster_url),
        .hero_url = std::move(hero_url),
      };
    }

    std::optional<heroic_game_t> parse_legendary_entry(
      const nlohmann::json &entry,
      const std::string &fallback_app_name,
      launcher_install_t install,
      bool from_cache
    ) {
      if (!entry.is_object()) {
        return std::nullopt;
      }

      if (from_cache && (!entry.contains("is_installed") ||
                         !entry["is_installed"].is_boolean() ||
                         !entry["is_installed"].get<bool>())) {
        return std::nullopt;
      }
      if (!entry.contains("app_name") || !entry["app_name"].is_string() ||
          !entry.contains("title") || !entry["title"].is_string() ||
          (entry.contains("is_dlc") && !entry["is_dlc"].is_boolean())) {
        return std::nullopt;
      }
      if (entry.contains("is_dlc") && entry["is_dlc"].get<bool>()) {
        return std::nullopt;
      }
      if (!from_cache && !entry.contains("is_dlc")) {
        return std::nullopt;
      }
      if (from_cache) {
        if (!entry.contains("install") || !entry["install"].is_object() ||
            !entry["install"].contains("is_dlc") || !entry["install"]["is_dlc"].is_boolean() ||
            entry["install"]["is_dlc"].get<bool>()) {
          return std::nullopt;
        }
      }
      const auto app_name = entry["app_name"].get<std::string>();
      if (!fallback_app_name.empty() && app_name != fallback_app_name) {
        return std::nullopt;
      }

      return make_heroic_game(
        app_name,
        entry["title"].get<std::string>(),
        "epic",
        install,
        entry.value("art_square", ""),
        entry.value("art_cover", "")
      );
    }
  }  // namespace

  std::vector<heroic_game_t> parse_heroic_installed_json(
    std::string_view json_payload,
    const std::string &store,
    launcher_install_t install
  ) {
    std::vector<heroic_game_t> games;
    if (store != "epic") {
      return games;
    }

    try {
      const auto data = nlohmann::json::parse(json_payload);
      if (data.is_object()) {
        // Legendary keys its installed object by app name and repeats that identity in
        // each current InstalledJsonMetadata value. Require both to match.
        for (const auto &[app_name, entry] : data.items()) {
          if (auto game = parse_legendary_entry(entry, app_name, install, false)) {
            games.push_back(std::move(*game));
          }
        }
      }
    } catch (...) {
      games.clear();
    }
    return games;
  }

  std::vector<heroic_game_t> parse_heroic_gog_library_json(
    std::string_view installed_json_payload,
    std::string_view library_json_payload,
    launcher_install_t install
  ) {
    std::vector<heroic_game_t> games;
    try {
      const auto installed_data = nlohmann::json::parse(installed_json_payload);
      if (!installed_data.is_object() || !installed_data.contains("installed") ||
          !installed_data["installed"].is_array()) {
        return games;
      }

      std::set<std::string> installed_app_names;
      for (const auto &entry : installed_data["installed"]) {
        if (!entry.is_object() || !entry.contains("appName") || !entry["appName"].is_string() ||
            !entry.contains("is_dlc") || !entry["is_dlc"].is_boolean() || entry["is_dlc"].get<bool>()) {
          continue;
        }

        auto app_name = entry["appName"].get<std::string>();
        if (is_heroic_app_name_safe(app_name)) {
          installed_app_names.insert(std::move(app_name));
        }
      }

      const auto library_data = nlohmann::json::parse(library_json_payload);
      if (!library_data.is_object() || !library_data.contains("games") || !library_data["games"].is_array()) {
        return games;
      }

      std::set<std::string> emitted_app_names;
      for (const auto &entry : library_data["games"]) {
        if (!entry.is_object() || !entry.contains("app_name") || !entry["app_name"].is_string() ||
            !entry.contains("title") || !entry["title"].is_string() ||
            !entry.contains("install") || !entry["install"].is_object() ||
            !entry["install"].contains("is_dlc") || !entry["install"]["is_dlc"].is_boolean() ||
            entry["install"]["is_dlc"].get<bool>()) {
          continue;
        }

        const auto app_name = entry["app_name"].get<std::string>();
        if (installed_app_names.count(app_name) == 0 || !emitted_app_names.insert(app_name).second) {
          continue;
        }

        if (auto game = make_heroic_game(
              app_name,
              entry["title"].get<std::string>(),
              "gog",
              install,
              entry.value("art_square", ""),
              entry.value("art_cover", "")
            )) {
          games.push_back(std::move(*game));
        }
      }
    } catch (...) {
      games.clear();
    }
    return games;
  }

  std::vector<heroic_game_t> parse_heroic_cache_json(
    std::string_view json_payload,
    const std::string &store,
    launcher_install_t install
  ) {
    std::vector<heroic_game_t> games;
    if (store != "epic") {
      return games;
    }

    try {
      const auto data = nlohmann::json::parse(json_payload);
      if (!data.is_object() || !data.contains("library") || !data["library"].is_array()) {
        return games;
      }

      for (const auto &entry : data["library"]) {
        if (auto game = parse_legendary_entry(entry, "", install, true)) {
          games.push_back(std::move(*game));
        }
      }
    } catch (...) {
      games.clear();
    }
    return games;
  }

  std::optional<heroic_game_t> find_heroic_cached_game(
    const std::vector<std::filesystem::path> &home_roots,
    const std::string &app_name,
    const std::string &store,
    launcher_install_t install
  ) {
    if (store != "epic" || !is_heroic_app_name_safe(app_name)) {
      return std::nullopt;
    }

    for (const auto &library : heroic_cache_files(home_roots)) {
      if (library.store != store || library.install != install) {
        continue;
      }

      std::ifstream file(library.path);
      if (!file) {
        continue;
      }
      std::stringstream payload;
      payload << file.rdbuf();
      for (auto &entry : parse_heroic_cache_json(payload.str(), store, install)) {
        if (entry.app_name == app_name) {
          return entry;
        }
      }
    }

    return std::nullopt;
  }

  std::string find_lutris_image_path(const std::string &slug, const std::vector<std::filesystem::path> &lutris_roots) {
    if (!is_lutris_slug_safe(slug)) {
      return {};
    }

    std::vector<std::filesystem::path> candidates;
    std::set<std::filesystem::path> seen_roots;
    for (const auto &root : lutris_roots) {
      if (root.empty() || !seen_roots.insert(root).second) {
        continue;
      }

      append_lutris_image_candidates(candidates, root, slug);
    }

    for (const auto &candidate : candidates) {
      std::error_code ec;
      if (has_supported_image_extension(candidate) &&
          std::filesystem::is_regular_file(candidate, ec)) {
        return candidate.string();
      }
    }

    return {};
  }

  std::vector<std::filesystem::path> steam_data_roots(const std::vector<std::filesystem::path> &home_roots) {
    std::vector<std::filesystem::path> roots;
    const auto add = [&roots](std::filesystem::path candidate) {
      if (std::find(roots.begin(), roots.end(), candidate) == roots.end()) {
        roots.push_back(std::move(candidate));
      }
    };
    for (const auto &home : home_roots) {
      if (home.empty()) {
        continue;
      }
      add(home / ".steam" / "steam");
      add(home / ".local" / "share" / "Steam");
      add(home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam");
    }
    return roots;
  }

  steam_input_config_t parse_steam_input_vdf(std::string_view vdf_payload) {
    steam_input_config_t result;
    if (vdf_payload.empty()) {
      return result;
    }

    std::vector<std::string> section;
    std::string pending_section;
    std::set<std::string> forced_apps;
    std::istringstream stream {std::string(vdf_payload)};
    std::string line;
    bool saw_vdf_entry = false;

    const auto quoted = [](const std::string &text, std::size_t from, std::string &out) -> std::size_t {
      const auto open = text.find('"', from);
      if (open == std::string::npos) {
        return std::string::npos;
      }
      const auto close = text.find('"', open + 1);
      if (close == std::string::npos) {
        return std::string::npos;
      }
      out = text.substr(open + 1, close - open - 1);
      return close + 1;
    };

    while (std::getline(stream, line)) {
      const auto begin = line.find_first_not_of(" \t\r");
      if (begin == std::string::npos) {
        continue;
      }
      line = line.substr(begin);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
      }

      if (line == "{") {
        section.push_back(pending_section);
        pending_section.clear();
        continue;
      }
      if (line == "}") {
        if (!section.empty()) {
          section.pop_back();
        }
        continue;
      }
      if (line.empty() || line.front() != '"') {
        continue;
      }

      std::string key;
      const auto after_key = quoted(line, 0, key);
      if (after_key == std::string::npos) {
        continue;
      }

      std::string value;
      if (quoted(line, after_key, value) == std::string::npos) {
        pending_section = key;
        saw_vdf_entry = true;
        continue;
      }
      saw_vdf_entry = true;

      if (key == "SteamController_XBoxSupport") {
        result.xbox_support_enabled = value == "1";
        continue;
      }

      if (key != "UseSteamControllerConfig" ||
          section.size() < 2 ||
          section[section.size() - 2] != "apps") {
        continue;
      }

      const auto &app_id = section.back();
      if (app_id.empty() || app_id.find_first_not_of("0123456789") != std::string::npos) {
        continue;
      }
      if (value == "2") {
        forced_apps.insert(app_id);
      } else {
        forced_apps.erase(app_id);
      }
    }

    result.parsed = saw_vdf_entry;
    result.forced_app_count = static_cast<int>(forced_apps.size());
    return result;
  }

  steam_input_snapshot_t inspect_steam_input_configs(const std::vector<std::filesystem::path> &paths) {
    steam_input_snapshot_t result;
    for (const auto &path : paths) {
      std::ifstream file(path);
      if (!file.is_open()) {
        continue;
      }

      const std::string payload {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      const auto profile = parse_steam_input_vdf(payload);
      if (!profile.parsed) {
        continue;
      }
      ++result.profiles_checked;
      if (profile.xbox_support_enabled) {
        ++result.profiles_with_xbox_support;
      }
      result.forced_app_count += profile.forced_app_count;
    }

    if (result.profiles_checked == 0) {
      result.detail = "No readable Steam local profile was found.";
    } else if (result.profiles_with_xbox_support > 0 && result.forced_app_count > 0) {
      result.status = "xbox_opt_in_and_per_app_forced";
      result.detail =
        "Steam Input is opted in for Xbox controllers in " +
        std::to_string(result.profiles_with_xbox_support) +
        " local profile(s), and " +
        std::to_string(result.forced_app_count) +
        " app override(s) force Steam Input on.";
    } else if (result.profiles_with_xbox_support > 0) {
      result.status = "xbox_opt_in";
      result.detail =
        "Steam Input is opted in for Xbox controllers in " +
        std::to_string(result.profiles_with_xbox_support) +
        " local profile(s).";
    } else if (result.forced_app_count > 0) {
      result.status = "per_app_forced";
      result.detail =
        std::to_string(result.forced_app_count) +
        " app override(s) force Steam Input on.";
    } else {
      result.status = "clear";
      result.detail = "Steam Input is not opted in for Xbox controllers, and no app override forces it on.";
    }

    return result;
  }

  steam_input_snapshot_t steam_input_snapshot() {
    static std::mutex guard;
    static steam_input_snapshot_t cached;
    static std::chrono::steady_clock::time_point read_at {};

    const std::lock_guard<std::mutex> lock {guard};
    const auto now = std::chrono::steady_clock::now();
    if (read_at != std::chrono::steady_clock::time_point {} && now - read_at < std::chrono::seconds(30)) {
      return cached;
    }

    cached = inspect_steam_input_configs(
      steam_localconfig_paths(steam_data_roots(library_home_roots()))
    );
    read_at = now;
    return cached;
  }

  std::optional<std::string> set_steam_input_xbox_support(std::string_view vdf_payload, bool enabled) {
    constexpr std::string_view key = "\"SteamController_XBoxSupport\"";
    const auto key_at = vdf_payload.find(key);
    if (key_at == std::string_view::npos) {
      return std::nullopt;
    }

    // The value is the next quoted token, and it has to be on the same line: a
    // key whose value Steam has not written yet must not consume the next
    // setting's value and silently rewrite the wrong field.
    const auto line_end = vdf_payload.find('\n', key_at);
    const auto limit = line_end == std::string_view::npos ? vdf_payload.size() : line_end;
    const auto value_open = vdf_payload.find('"', key_at + key.size());
    if (value_open == std::string_view::npos || value_open >= limit) {
      return std::nullopt;
    }
    const auto value_close = vdf_payload.find('"', value_open + 1);
    if (value_close == std::string_view::npos || value_close >= limit) {
      return std::nullopt;
    }

    const auto current = vdf_payload.substr(value_open + 1, value_close - value_open - 1);
    const std::string_view desired = enabled ? "1" : "0";
    if (current == desired) {
      return std::nullopt;
    }

    std::string updated;
    updated.reserve(vdf_payload.size());
    updated.append(vdf_payload.substr(0, value_open + 1));
    updated.append(desired);
    updated.append(vdf_payload.substr(value_close));
    return updated;
  }

  playtime_snapshot_t steam_playtime_snapshot() {
    static std::mutex guard;
    static playtime_snapshot_t cached;
    static std::chrono::steady_clock::time_point read_at {};

    const std::lock_guard<std::mutex> lock {guard};
    const auto now = std::chrono::steady_clock::now();
    if (read_at != std::chrono::steady_clock::time_point {} && now - read_at < std::chrono::seconds(30)) {
      return cached;
    }

    std::map<std::string, playtime_t> fresh;
    for (const auto &path : steam_localconfig_paths(steam_data_roots(library_home_roots()))) {
      std::ifstream file(path);
      if (!file.is_open()) {
        continue;
      }
      const std::string payload {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      for (auto &[app_id, played] : parse_steam_playtime_vdf(payload)) {
        // One machine can hold several Steam accounts; the one that played most is the
        // one whose number means anything to whoever is looking at the library.
        auto &slot = fresh[app_id];
        if (played.minutes > slot.minutes) {
          slot = played;
        }
      }
    }

    cached.by_app_id = std::move(fresh);
    cached.read_at = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch()
    )
                       .count();
    read_at = now;
    return cached;
  }

  std::map<std::string, playtime_t> parse_steam_playtime_vdf(std::string_view vdf_payload) {
    std::map<std::string, playtime_t> playtime;
    if (vdf_payload.empty()) {
      return playtime;
    }

    // The block a key sits in is what makes it meaningful here: localconfig.vdf holds a
    // whole user profile, and "apps" is only one branch of it.
    std::vector<std::string> section;
    std::string pending_section;
    std::istringstream stream {std::string(vdf_payload)};
    std::string line;

    const auto quoted = [](const std::string &text, std::size_t from, std::string &out) -> std::size_t {
      const auto open = text.find('"', from);
      if (open == std::string::npos) {
        return std::string::npos;
      }
      const auto close = text.find('"', open + 1);
      if (close == std::string::npos) {
        return std::string::npos;
      }
      out = text.substr(open + 1, close - open - 1);
      return close + 1;
    };

    while (std::getline(stream, line)) {
      const auto begin = line.find_first_not_of(" \t\r");
      if (begin == std::string::npos) {
        continue;
      }
      line = line.substr(begin);
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
      }

      if (line == "{") {
        section.push_back(pending_section);
        pending_section.clear();
        continue;
      }
      if (line == "}") {
        if (!section.empty()) {
          section.pop_back();
        }
        continue;
      }
      if (line.empty() || line[0] != '"') {
        continue;
      }

      std::string key;
      const auto after_key = quoted(line, 0, key);
      if (after_key == std::string::npos) {
        continue;
      }

      std::string value;
      if (quoted(line, after_key, value) == std::string::npos) {
        // A bare quoted token opens the block named on the next line.
        pending_section = key;
        continue;
      }

      // Inside "apps" the enclosing section is the app id itself.
      if (section.size() < 2 || section.back().empty()) {
        continue;
      }
      const auto &owner = section[section.size() - 2];
      if (owner != "apps") {
        continue;
      }
      const auto &app_id = section.back();
      if (app_id.find_first_not_of("0123456789") != std::string::npos) {
        continue;
      }

      try {
        if (key == "Playtime") {
          playtime[app_id].minutes = std::stoll(value);
        } else if (key == "LastPlayed") {
          playtime[app_id].last_played = std::stoll(value);
        }
      } catch (...) {
        // A field Steam wrote in a shape we do not understand is not a reason to lose
        // every other game's playtime.
        continue;
      }
    }

    // An entry that only ever recorded a launch, never a minute, says nothing worth
    // showing, so it does not travel.
    for (auto it = playtime.begin(); it != playtime.end();) {
      it = it->second.minutes > 0 ? std::next(it) : playtime.erase(it);
    }
    return playtime;
  }

  std::vector<std::filesystem::path> steam_localconfig_paths(const std::vector<std::filesystem::path> &roots) {
    std::vector<std::filesystem::path> found;
    // A stock Linux install symlinks ~/.steam/steam at ~/.local/share/Steam, and both
    // are probed as roots, so the same profile arrives twice. Callers that merge by
    // app id never noticed; callers that count -- Doctor reports "N app override(s)"
    // -- reported double. Identity is the resolved file, not the path used to reach it.
    std::set<std::filesystem::path> seen;
    for (const auto &root : roots) {
      // A fresh code per query: one shared across the walk stays set after the first
      // user directory that has no localconfig, and silently swallows every later one.
      std::error_code root_ec;
      const auto userdata = root / "userdata";
      if (!std::filesystem::is_directory(userdata, root_ec) || root_ec) {
        continue;
      }

      std::error_code walk_ec;
      std::filesystem::directory_iterator users(userdata, walk_ec);
      if (walk_ec) {
        continue;
      }

      for (const auto &user : users) {
        std::error_code entry_ec;
        if (!user.is_directory(entry_ec) || entry_ec) {
          continue;
        }
        auto candidate = user.path() / "config" / "localconfig.vdf";
        std::error_code file_ec;
        if (std::filesystem::is_regular_file(candidate, file_ec) && !file_ec) {
          // Deduplicate on the resolved target, but report the path we walked: the
          // raw path is what a caller can act on, and a file we cannot resolve is
          // still worth reading once.
          std::error_code canonical_ec;
          auto identity = std::filesystem::weakly_canonical(candidate, canonical_ec);
          if (canonical_ec) {
            identity = candidate;
          }
          if (!seen.insert(std::move(identity)).second) {
            continue;
          }
          found.push_back(std::move(candidate));
        }
      }
    }
    return found;
  }

  std::vector<lutris_game_t> parse_lutris_list_games_json(std::string_view json_payload) {
    std::vector<lutris_game_t> games;
    if (json_payload.empty()) {
      return games;
    }

    try {
      const auto payload = nlohmann::json::parse(json_payload);
      if (!payload.is_array()) {
        return games;
      }

      for (const auto &entry : payload) {
        if (!entry.is_object()) {
          continue;
        }

        const auto name = entry.value("name", "");
        const auto slug = trim_copy(entry.value("slug", ""));
        if (name.empty() || !is_lutris_slug_safe(slug)) {
          continue;
        }

        games.push_back(lutris_game_t {
          .name = name,
          .slug = slug,
          .runner = entry.value("runner", ""),
          .command = lutris_launch_command(slug),
          .image_path = entry.contains("coverPath") && entry["coverPath"].is_string() ?
            supported_image_path_or_empty(entry["coverPath"].get<std::string>()) :
            "",
          .playtime_seconds = entry.contains("playtimeSeconds") && entry["playtimeSeconds"].is_number() ?
            static_cast<int64_t>(entry["playtimeSeconds"].get<double>()) :
            0,
        });
      }
    } catch (...) {
      games.clear();
    }

    return games;
  }

  std::optional<lutris_game_t> parse_lutris_game_config(const std::filesystem::path &path) {
    std::ifstream file(path);
    if (!file) {
      return std::nullopt;
    }

    std::string name;
    std::string slug;
    std::string game_slug;
    std::string runner;

    std::string line;
    while (std::getline(file, line)) {
      const auto scalar = parse_top_level_yaml_scalar(line);
      if (!scalar) {
        continue;
      }

      const auto &[key, value] = *scalar;
      if (key == "name") {
        name = value;
      } else if (key == "slug") {
        slug = value;
      } else if (key == "game_slug") {
        game_slug = value;
      } else if (key == "runner") {
        runner = value;
      }
    }

    if (slug.empty()) {
      slug = game_slug;
    }
    if (slug.empty()) {
      slug = path.stem().string();
    }
    slug = trim_copy(slug);

    if (!is_lutris_slug_safe(slug)) {
      return std::nullopt;
    }

    if (name.empty()) {
      name = slug_to_name(slug);
    }

    // Where the config was found is what says how to launch it: a config under
    // ~/.var/app/net.lutris.Lutris belongs to a Flatpak install that has no lutris on PATH.
    const auto install = path_is_under_flatpak_app(path, lutris_flatpak_app_id) ?
      launcher_install_t::flatpak :
      launcher_install_t::native;

    return lutris_game_t {
      .name = name,
      .slug = slug,
      .runner = runner,
      .command = lutris_launch_command(slug, install),
    };
  }

  std::vector<lutris_game_t> scan_lutris_games(const std::filesystem::path &games_dir) {
    return scan_lutris_games(std::vector<std::filesystem::path> {games_dir});
  }

  std::vector<lutris_game_t> scan_lutris_games(const std::vector<std::filesystem::path> &games_dirs) {
    std::vector<lutris_game_t> games;
    std::vector<std::filesystem::path> config_paths;
    std::set<std::filesystem::path> lutris_roots;
    for (const auto &games_dir : games_dirs) {
      std::error_code ec;
      if (!std::filesystem::is_directory(games_dir, ec)) {
        continue;
      }

      lutris_roots.insert(games_dir.parent_path());

      for (const auto &entry : std::filesystem::directory_iterator(games_dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
          break;
        }

        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec)) {
          continue;
        }

        const auto extension = entry.path().extension().string();
        if (extension == ".yml" || extension == ".yaml") {
          config_paths.push_back(entry.path());
        }
      }
    }

    std::sort(config_paths.begin(), config_paths.end());

    std::set<std::string> seen_slugs;
    for (const auto &path : config_paths) {
      auto game = parse_lutris_game_config(path);
      if (!game) {
        continue;
      }

      if (!seen_slugs.insert(game->slug).second) {
        continue;
      }

      if (game->image_path.empty()) {
        std::vector<std::filesystem::path> roots(lutris_roots.begin(), lutris_roots.end());
        game->image_path = find_lutris_image_path(
          game->slug,
          roots
        );
      }

      games.push_back(std::move(*game));
    }

    return games;
  }

  std::vector<lutris_game_t> scan_lutris_library(const std::vector<std::filesystem::path> &games_dirs) {
    std::vector<lutris_game_t> games;
    std::set<std::string> seen_slugs;

    append_unique_lutris_games(games, seen_slugs, parse_lutris_list_games_json(read_lutris_list_games_json()));
    if (!games.empty()) {
      return games;
    }

    append_unique_lutris_games(games, seen_slugs, scan_lutris_games(games_dirs));

    return games;
  }

}  // namespace game_library
