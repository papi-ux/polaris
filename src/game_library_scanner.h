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

  /**
   * @brief Which packaging of a launcher a library entry was discovered through.
   *
   * The launch command has to follow the install that produced the entry: a Flatpak-only
   * host has no `heroic` or `lutris` on PATH, and a native host should keep the command it
   * already has.
   */
  enum class launcher_install_t {
    native,
    flatpak,
  };

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
  std::string lutris_launch_command(const std::string &slug, launcher_install_t install);
  std::vector<lutris_game_t> parse_lutris_list_games_json(std::string_view json_payload);
  std::optional<lutris_game_t> parse_lutris_game_config(const std::filesystem::path &path);
  std::vector<lutris_game_t> scan_lutris_games(const std::filesystem::path &games_dir);
  std::vector<lutris_game_t> scan_lutris_games(const std::vector<std::filesystem::path> &games_dirs);
  std::vector<lutris_game_t> scan_lutris_library(const std::vector<std::filesystem::path> &games_dirs);

  /** @brief Where Lutris keeps its per-game YAML, for each install we can see. */
  std::vector<std::filesystem::path> lutris_game_config_dirs(const std::vector<std::filesystem::path> &home_roots);

  /** @brief Where Lutris keeps its artwork, for each install we can see. */
  std::vector<std::filesystem::path> lutris_art_roots(const std::vector<std::filesystem::path> &home_roots);

  /**
   * @brief True when the path lives inside a Flatpak application's per-app home.
   *
   * Flatpak maps `XDG_CONFIG_HOME` and friends into `~/.var/app/<app-id>/`, so where a
   * library file was found is what tells us how to launch it.
   */
  bool path_is_under_flatpak_app(const std::filesystem::path &path, std::string_view app_id);

  /** @brief One Heroic library file, and the install it belongs to. */
  struct heroic_library_file_t {
    std::filesystem::path path;
    std::string store;  // the segment Heroic expects in heroic://launch/<store>/<app_name>
    launcher_install_t install = launcher_install_t::native;
  };

  /** @brief Heroic's installed-games manifests, native install first. */
  std::vector<heroic_library_file_t> heroic_installed_files(const std::vector<std::filesystem::path> &home_roots);

  /** @brief Heroic's cached store libraries, used when a manifest is missing. */
  std::vector<heroic_library_file_t> heroic_cache_files(const std::vector<std::filesystem::path> &home_roots);

  /**
   * @brief True when a Heroic app name or store is safe to place in a launch command.
   *
   * Both come out of a file on disk and end up in a shell command, the same exposure the
   * Steam app id and the Lutris slug are already checked for.
   */
  bool is_heroic_app_name_safe(const std::string &app_name);

  /** @brief The `heroic://launch` command for one title, for the install that has it. */
  std::string heroic_launch_command(const std::string &store, const std::string &app_name, launcher_install_t install);

  /** @brief Every command form that could launch one Heroic title, for import dedup. */
  std::vector<std::string> heroic_launch_commands(const std::string &store, const std::string &app_name);

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
