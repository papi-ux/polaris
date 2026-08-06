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

    std::string read_lutris_list_games_json() {
    #ifdef __linux__
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
    return "setsid lutris lutris:rungame/" + slug;
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

    return lutris_game_t {
      .name = name,
      .slug = slug,
      .runner = runner,
      .command = lutris_launch_command(slug),
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
