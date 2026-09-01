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
    std::string store;  // Polaris-facing store name: gog or epic
    launcher_install_t install = launcher_install_t::native;
  };

  /** @brief One validated, launchable Heroic library entry. */
  struct heroic_game_t {
    std::string name;
    std::string app_name;
    std::string store;
    std::string runner;
    launcher_install_t install = launcher_install_t::native;
    std::string command;
    std::string poster_url;
    std::string hero_url;
  };

  /** @brief The stable API/storage name for a launcher installation. */
  std::string heroic_install_name(launcher_install_t install);

  /** @brief Parse a stable API/storage installation name. */
  std::optional<launcher_install_t> heroic_install_from_name(std::string_view install);

  /** @brief Map a Polaris-facing Heroic store to Heroic's protocol runner. */
  std::string heroic_runner_for_store(std::string_view store);

  /** @brief Validate scanner/import metadata and derive its command without trusting the browser. */
  std::optional<heroic_game_t> heroic_game_from_metadata(
    const std::string &app_name,
    const std::string &store,
    const std::string &runner,
    std::string_view install
  );

  /** @brief Heroic's installed-games manifests, native install first. */
  std::vector<heroic_library_file_t> heroic_installed_files(const std::vector<std::filesystem::path> &home_roots);

  /** @brief Heroic's cached store libraries, used to recover installed-game metadata. */
  std::vector<heroic_library_file_t> heroic_cache_files(const std::vector<std::filesystem::path> &home_roots);

  /** @brief True when a Heroic app name is safe to place in a launch command. */
  bool is_heroic_app_name_safe(const std::string &app_name);

  /** @brief The `heroic://launch` command for one title, for the install that has it. */
  std::string heroic_launch_command(const std::string &store, const std::string &app_name, launcher_install_t install);

  /** @brief Every command form that could launch one Heroic title, for import dedup. */
  std::vector<std::string> heroic_launch_commands(const std::string &store, const std::string &app_name);

  /** @brief Parse only the exact Heroic command form emitted by older Polaris releases. */
  std::optional<heroic_game_t> parse_legacy_heroic_launch_command(std::string_view command);

  /** @brief Parse Legendary's installed-games manifest into validated launchable entries. */
  std::vector<heroic_game_t> parse_heroic_installed_json(
    std::string_view json_payload,
    const std::string &store,
    launcher_install_t install
  );

  /** @brief Join GOG's installed identities to its cached game metadata. */
  std::vector<heroic_game_t> parse_heroic_gog_library_json(
    std::string_view installed_json_payload,
    std::string_view library_json_payload,
    launcher_install_t install
  );

  /** @brief Parse Legendary's installed store-cache entries into validated launchable entries. */
  std::vector<heroic_game_t> parse_heroic_cache_json(
    std::string_view json_payload,
    const std::string &store,
    launcher_install_t install
  );

  /** @brief Resolve one exact installed Epic title's official artwork from Heroic's local cache. */
  std::optional<heroic_game_t> find_heroic_cached_game(
    const std::vector<std::filesystem::path> &home_roots,
    const std::string &app_name,
    const std::string &store,
    launcher_install_t install
  );

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

  /**
   * @brief Steam Input settings relevant to Polaris' emulated Xbox controller.
   *
   * App ids and profile paths deliberately do not leave the parser. Doctor only
   * needs the aggregate conflict state, and diagnostics exports must not reveal
   * which local account owns a profile or which games are installed.
   */
  struct steam_input_config_t {
    bool parsed = false;
    bool xbox_support_enabled = false;
    int forced_app_count = 0;
  };

  /** @brief Read the relevant settings from one localconfig.vdf payload. */
  steam_input_config_t parse_steam_input_vdf(std::string_view vdf_payload);

  /**
   * @brief PII-free aggregate of the Steam profiles Polaris could inspect.
   */
  struct steam_input_snapshot_t {
    std::string status = "unknown";
    int profiles_checked = 0;
    int profiles_with_xbox_support = 0;
    int forced_app_count = 0;
    std::string detail;

    bool conflict() const {
      return profiles_with_xbox_support > 0 || forced_app_count > 0;
    }
  };

  /** @brief Inspect an explicit set of profile files without caching. */
  steam_input_snapshot_t inspect_steam_input_configs(const std::vector<std::filesystem::path> &paths);

  /** @brief Inspect locally discovered Steam profiles, cached briefly. */
  steam_input_snapshot_t steam_input_snapshot();

  /**
   * @brief Rewrite one profile's host-wide Xbox Steam Input opt-in.
   *
   * Returns the whole updated payload, or nullopt when the key is absent or
   * already holds the requested value, so a caller can skip the write entirely.
   * Everything outside that one value is preserved byte for byte: this edits a
   * live Steam profile, and a reformatting writer would be a far worse bug than
   * the setting it came to change.
   */
  std::optional<std::string> set_steam_input_xbox_support(std::string_view vdf_payload, bool enabled);

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
