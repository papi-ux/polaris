/**
 * @file src/game_library_scanner.h
 * @brief Helpers for discovering third-party launcher libraries.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace game_library {

  struct lutris_game_t {
    std::string name;
    std::string slug;
    std::string runner;
    std::string command;
    std::string image_path;
  };

  bool is_lutris_slug_safe(const std::string &slug);
  std::string find_lutris_image_path(const std::string &slug, const std::vector<std::filesystem::path> &lutris_roots);
  std::string lutris_launch_command(const std::string &slug);
  std::vector<lutris_game_t> parse_lutris_list_games_json(std::string_view json_payload);
  std::optional<lutris_game_t> parse_lutris_game_config(const std::filesystem::path &path);
  std::vector<lutris_game_t> scan_lutris_games(const std::filesystem::path &games_dir);
  std::vector<lutris_game_t> scan_lutris_games(const std::vector<std::filesystem::path> &games_dirs);
  std::vector<lutris_game_t> scan_lutris_library(const std::vector<std::filesystem::path> &games_dirs);

}  // namespace game_library
