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
   * Decorate an already-sanitized artwork manifest with the sanitized match
   * and active override state. Invalid metadata leaves the manifest unchanged.
   */
  nlohmann::json decorate_manifest_with_artwork_override(
    nlohmann::json manifest,
    const artwork_override_t &metadata
  );
}  // namespace game_artwork
