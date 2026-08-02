/**
 * @file src/game_artwork.h
 * @brief Sanitized game artwork manifests and host-side artwork resolution.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace game_artwork {

  inline constexpr int manifest_version = 1;
  inline constexpr std::uintmax_t maximum_asset_bytes = 20U * 1024U * 1024U;

  enum class kind_e {
    poster,
    hero,
    logo,
    icon,
  };

  enum class source_e {
    override,
    local,
    steam,
    steamgriddb,
    host,
  };

  enum class provider_e {
    steam,
    steamgriddb,
  };

  struct asset_t {
    kind_e kind;
    source_e source;
    std::filesystem::path path;
    std::string mime_type;
  };

  struct local_candidate_t {
    std::filesystem::path path;
    source_e source = source_e::local;
  };

  struct asset_request_t {
    std::string uuid;
    kind_e kind;
  };

  struct resolve_request_t {
    std::filesystem::path appdata;
    std::string uuid;
    std::string game_name;
    std::string steam_appid;
    std::vector<local_candidate_t> local_posters;
    std::string steamgriddb_api_key;
  };

  [[nodiscard]] bool is_valid_uuid(std::string_view uuid);
  [[nodiscard]] bool is_valid_steam_appid(std::string_view appid);
  [[nodiscard]] std::optional<kind_e> parse_kind(std::string_view value);
  [[nodiscard]] std::string_view kind_name(kind_e kind);
  [[nodiscard]] std::string_view source_name(source_e source);
  [[nodiscard]] std::optional<asset_request_t> parse_asset_request_target(std::string_view path);
  [[nodiscard]] std::optional<std::string> parse_resolve_request_target(std::string_view path);

  [[nodiscard]] std::filesystem::path cache_root(const std::filesystem::path &appdata);
  [[nodiscard]] std::optional<std::filesystem::path> cache_asset_path(
    const std::filesystem::path &appdata,
    std::string_view uuid,
    kind_e kind,
    source_e source,
    std::string_view extension
  );

  [[nodiscard]] std::optional<std::string> image_mime_type(const std::filesystem::path &path);
  [[nodiscard]] std::optional<local_candidate_t> select_legacy_poster(
    const std::filesystem::path &covers_directory,
    std::string_view uuid,
    std::string_view steam_appid,
    const std::filesystem::path &configured_image,
    source_e configured_source = source_e::local
  );
  [[nodiscard]] std::optional<asset_t> cache_local_poster(
    const std::filesystem::path &appdata,
    std::string_view uuid,
    const local_candidate_t &candidate
  );

  [[nodiscard]] std::vector<asset_t> scan_cached_assets(
    const std::filesystem::path &appdata,
    std::string_view uuid
  );
  [[nodiscard]] std::optional<asset_t> find_cached_asset(
    const std::filesystem::path &appdata,
    std::string_view uuid,
    kind_e kind
  );
  [[nodiscard]] bool needs_source_upgrade(
    const std::filesystem::path &appdata,
    std::string_view uuid,
    kind_e kind,
    source_e candidate_source
  );
  [[nodiscard]] nlohmann::json make_manifest(
    std::string_view uuid,
    const std::vector<asset_t> &assets
  );
  [[nodiscard]] nlohmann::json current_manifest(
    const std::filesystem::path &appdata,
    std::string_view uuid
  );

  [[nodiscard]] bool is_allowed_provider_url(provider_e provider, std::string_view url);

  /**
   * Resolve missing artwork into the deterministic host cache. Existing valid
   * bytes are retained; higher-priority sources may be added alongside lower-
   * priority fallbacks. Local poster candidates win over Steam official assets,
   * which win over SteamGridDB. Upstream failures are isolated per kind.
   */
  [[nodiscard]] std::vector<asset_t> resolve_missing_assets(const resolve_request_t &request);

}  // namespace game_artwork
