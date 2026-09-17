#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace game_artwork {
  inline constexpr int artwork_override_version = 1;
  inline constexpr std::size_t maximum_provider_game_id_length = 20;
  inline constexpr std::size_t maximum_override_title_bytes = 256;
  inline constexpr std::size_t maximum_override_file_bytes = 16 * 1024;

  struct artwork_override_t {
    std::string uuid;
    std::string provider;
    std::string provider_game_id;
    std::string title;
    std::optional<std::string> steam_appid;
    bool manual = true;
    std::int64_t updated_at = 0;

    bool operator==(const artwork_override_t &) const = default;
  };

  /**
   * Validate the complete persisted override contract without performing I/O.
   */
  bool is_valid_artwork_override(const artwork_override_t &metadata);

  /**
   * Load one game's override metadata. Missing, malformed, oversized, or
   * tampered files fail closed and return std::nullopt.
   */
  std::optional<artwork_override_t> load_artwork_override(
    const std::filesystem::path &appdata,
    std::string_view uuid
  );

  /**
   * Persist one game's override as artwork/v1/{uuid}/override.json using a
   * same-directory temporary file followed by an atomic rename.
   */
  bool save_artwork_override(
    const std::filesystem::path &appdata,
    const artwork_override_t &metadata
  );

  struct staged_override_commit_options_t {
    std::function<bool(const std::filesystem::path &, const artwork_override_t &)> save_metadata;
    std::function<void()> after_first_asset_published;
  };

  /** Create a random-token staging directory without following symlinked cache ancestors. */
  std::optional<std::filesystem::path> create_artwork_staging_directory(
    const std::filesystem::path &appdata,
    std::string_view token
  );

  /** Recover or finalize a transaction interrupted by process termination. */
  bool recover_interrupted_artwork_override(
    const std::filesystem::path &appdata,
    std::string_view uuid
  );

  /** Shared gate for manifest/asset readers while an override transaction commits. */
  std::shared_lock<std::shared_mutex> acquire_artwork_override_read_lock();

  /**
   * Atomically expose validated override files from a separate staging appdata
   * root and their metadata. On any failure, restore the complete prior state.
   */
  bool commit_staged_artwork_override(
    const std::filesystem::path &appdata,
    const std::filesystem::path &staging_appdata,
    const artwork_override_t &metadata,
    const staged_override_commit_options_t &options = {}
  );

  /**
   * Remove only override metadata (including a stale metadata temp file).
   * Cached artwork assets and the per-game directory are never removed.
   */
  bool clear_artwork_override(
    const std::filesystem::path &appdata,
    std::string_view uuid
  );

  /**
   * A cover chosen in the console after artwork was picked for the game in Nova takes the poster
   * back. The saved image has to be the file Find Cover writes for the entry, `<uuid>.<ext>` in
   * the covers directory, and either new to the entry or rewritten after the Nova pick. Every
   * other save keeps the pick, because other images change under an entry with nobody choosing
   * one: a library rescan refreshes an imported game's `steam_<appid>` cover in the same
   * directory, Steam refreshes its library cache, and the console shows a Lutris entry the
   * cover art path that entry stores none of. Only the picked poster goes; the picked hero, logo
   * and icon stay, and a pick left without an image is cleared with its metadata. Images are
   * resolved files: an empty saved image, or one that is not a readable image, keeps the pick.
   * @return true when the picked poster was removed.
   */
  bool yield_picked_poster_to_console_cover(
    const std::filesystem::path &appdata,
    std::string_view uuid,
    const std::filesystem::path &previous_image,
    const std::filesystem::path &saved_image,
    const std::filesystem::path &covers_directory
  );

  /**
   * Whether automatic artwork lookup may fetch artwork for a game. Remove artwork turns it off
   * with a marker beside the game's cached artwork, and Find artwork again turns it back on.
   * A game without the marker is looked up; an unsafe artwork directory is not.
   */
  bool automatic_artwork_lookup_enabled(
    const std::filesystem::path &appdata,
    std::string_view uuid
  );

  /**
   * Remove artwork: turn automatic lookup off for a game, then delete every image that was
   * downloaded for it, the automatic Steam and SteamGridDB copies and a picked override with its
   * metadata. The copy of the entry's own image stays. The marker is written first, so a removal
   * that fails partway still never fetches again; false means something could not be removed.
   */
  bool remove_downloaded_artwork(
    const std::filesystem::path &appdata,
    std::string_view uuid
  );

  /**
   * Find artwork again: turn automatic lookup back on for a game. A game that never had it off
   * succeeds without change.
   */
  bool enable_automatic_artwork_lookup(
    const std::filesystem::path &appdata,
    std::string_view uuid
  );

  /**
   * Decorate an already-sanitized artwork manifest with the sanitized match
   * and active override state. Invalid metadata leaves the manifest unchanged.
   */
  nlohmann::json decorate_manifest_with_artwork_override(
    nlohmann::json manifest,
    const artwork_override_t &metadata
  );
}  // namespace game_artwork
