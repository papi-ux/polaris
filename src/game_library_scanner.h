/**
 * @file src/game_library_scanner.h
 * @brief Helpers for discovering third-party launcher libraries.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
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
    int64_t playtime_seconds = 0;  // as Lutris reports it; 0 when it does not
  };

  /**
   * @brief How long a launcher says one of its games has been played.
   *
   * Minutes rather than seconds because that is the coarser of the two units the
   * launchers use, and rounding up to it loses nothing anyone reads.
   */
  struct playtime_t {
    int64_t minutes = 0;      // total, as the owning launcher counts it
    int64_t last_played = 0;  // unix seconds; 0 when the launcher does not say
  };

  bool is_lutris_slug_safe(const std::string &slug);
  std::vector<std::filesystem::path> library_home_roots(std::string_view runtime_home, std::string_view account_home);
  std::vector<std::filesystem::path> library_home_roots();
  std::string find_lutris_image_path(const std::string &slug, const std::vector<std::filesystem::path> &lutris_roots);
  std::string lutris_launch_command(const std::string &slug);
  std::vector<lutris_game_t> parse_lutris_list_games_json(std::string_view json_payload);
  std::optional<lutris_game_t> parse_lutris_game_config(const std::filesystem::path &path);
  std::vector<lutris_game_t> scan_lutris_games(const std::filesystem::path &games_dir);
  std::vector<lutris_game_t> scan_lutris_games(const std::vector<std::filesystem::path> &games_dirs);
  std::vector<lutris_game_t> scan_lutris_library(const std::vector<std::filesystem::path> &games_dirs);

  /**
   * @brief Steam app id to playtime, read from one localconfig.vdf payload.
   *
   * Keyed by app id as a string, because that is how Steam writes it and how the app
   * catalogue already carries it.
   */
  std::map<std::string, playtime_t> parse_steam_playtime_vdf(std::string_view vdf_payload);

  /** @brief Every localconfig.vdf belonging to a Steam user under the given roots. */
  std::vector<std::filesystem::path> steam_localconfig_paths(const std::vector<std::filesystem::path> &roots);

  /** @brief Where Steam keeps its data, for each home directory we know about. */
  std::vector<std::filesystem::path> steam_data_roots(const std::vector<std::filesystem::path> &home_roots);

  /** @brief What the launcher files said, and when we last looked. */
  struct playtime_snapshot_t {
    std::map<std::string, playtime_t> by_app_id;
    int64_t read_at = 0;  // unix seconds; localconfig.vdf lags Steam cloud, so this is worth saying
  };

  /**
   * @brief Steam playtime for every locally known game, cached briefly.
   *
   * A library listing serialises every game in one pass, so this is read once for the
   * request rather than once per game. Playtime that is half a minute stale is still true.
   */
  playtime_snapshot_t steam_playtime_snapshot();

}  // namespace game_library
