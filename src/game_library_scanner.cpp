/**
 * @file src/game_library_scanner.cpp
 * @brief Helpers for discovering third-party launcher libraries.
 */
#include "game_library_scanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace game_library {
  namespace {
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
