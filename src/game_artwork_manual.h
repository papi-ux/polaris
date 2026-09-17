#pragma once

#include "game_artwork.h"
#include "game_artwork_provider.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace game_artwork::manual {
  inline constexpr std::size_t maximum_match_body_bytes = 4096;
  inline constexpr std::size_t maximum_search_query_bytes = 160;
  inline constexpr std::size_t maximum_candidate_count = 5;
  inline constexpr std::size_t maximum_choice_count = 5;
  inline constexpr std::size_t maximum_choice_url_bytes = 2048;
  inline constexpr std::uintmax_t maximum_listing_bytes = 1024U * 1024U;
  inline constexpr std::uintmax_t maximum_preview_bytes = 2U * 1024U * 1024U;
  // A player browses four kinds and picks before applying, so previews outlive a
  // quick look and leave room for four full choice lists plus a search.
  inline constexpr std::size_t preview_cache_entries = 64;
  inline constexpr std::uintmax_t preview_cache_bytes = 64U * 1024U * 1024U;
  inline constexpr std::int64_t preview_ttl_milliseconds = 15 * 60 * 1000;

  enum class route_e { search, apply, clear, preview, choices };

  struct route_request_t {
    route_e route;
    std::string uuid;
    std::optional<std::string> token;
    std::optional<kind_e> kind;
  };

  /** One pick from the alternatives: the opaque token a choice list handed out for this kind. */
  struct selected_choice_t {
    kind_e kind;
    std::string token;

    bool operator==(const selected_choice_t &) const = default;
  };

  struct match_selection_t {
    std::string provider;
    std::string provider_game_id;
    std::string title;
    std::optional<std::string> steam_appid;
    std::vector<kind_e> kinds;  ///< kinds a match replaces, or the picked kinds in poster, hero, logo, icon order
    std::vector<selected_choice_t> selections;  ///< empty for a match by kinds, one pick per kind otherwise
  };

  /** Where a pickable preview came from. Only the host sees these values. */
  struct choice_source_t {
    std::string provider_game_id;  ///< the SteamGridDB game the alternatives were listed for
    std::string asset_url;  ///< the allowlisted image an apply downloads for this choice
  };

  struct preview_t {
    std::string token;
    std::string uuid;
    kind_e kind;
    std::string mime_type;
    std::vector<unsigned char> body;
    std::int64_t expires_at;
    std::optional<choice_source_t> choice;  ///< set only when the token is a pickable alternative
  };

  [[nodiscard]] std::optional<route_request_t> parse_route_target(std::string_view path);
  [[nodiscard]] std::string request_log_value(std::string_view name, std::string_view value);
  [[nodiscard]] std::string request_log_path(std::string_view path);
  [[nodiscard]] std::string request_log_target(
    std::string_view path,
    const std::vector<std::pair<std::string, std::string>> &query
  );
  [[nodiscard]] std::optional<std::string> sanitize_search_query(std::string_view query);

  /** Why a SteamGridDB search could not answer, in a shape clients can act on. */
  struct search_failure_t {
    std::string code;  ///< stable machine word: steamgriddb_key_missing, steamgriddb_unauthorized, steamgriddb_rate_limited, steamgriddb_unreachable, steamgriddb_unavailable
    std::string message;  ///< player-facing sentence naming the fix
    int http_status;  ///< status Polaris answers with (503 without a key, 502 for upstream trouble)
  };

  /**
   * Classify a failed search from the host's key state and the upstream status.
   * Callers pass std::nullopt when SteamGridDB could not be reached at all.
   */
  [[nodiscard]] search_failure_t classify_search_failure(bool key_present, std::optional<long> upstream_status);

  /**
   * Parse a match body. It names the kinds to replace with SteamGridDB's first image
   * (`kinds`), or the exact images picked from choice lists (`selections`, kind to
   * opaque token). Exactly one of the two is present.
   */
  [[nodiscard]] std::optional<match_selection_t> parse_match_selection(std::string_view body);

  /** Parse a choice list body: the match identity alone, with no kinds and no selections. */
  [[nodiscard]] std::optional<match_selection_t> parse_choice_request(std::string_view body);

  class preview_cache_t {
   public:
    using token_factory_t = std::function<std::string()>;

    explicit preview_cache_t(
      std::size_t maximum_entries = preview_cache_entries,
      std::uintmax_t maximum_total_bytes = preview_cache_bytes,
      std::int64_t ttl_milliseconds = preview_ttl_milliseconds,
      token_factory_t token_factory = {}
    );

    [[nodiscard]] std::optional<preview_t> publish(
      std::string_view uuid,
      kind_e kind,
      std::vector<unsigned char> body,
      std::int64_t now_milliseconds,
      std::optional<choice_source_t> choice = std::nullopt
    );
    [[nodiscard]] std::optional<preview_t> lookup(
      std::string_view uuid,
      std::string_view token,
      kind_e kind,
      std::int64_t now_milliseconds
    );
    /** The source behind a pickable token, or nothing when it is unknown, expired, another game's or kind's, or only a preview. */
    [[nodiscard]] std::optional<choice_source_t> lookup_choice(
      std::string_view uuid,
      std::string_view token,
      kind_e kind,
      std::int64_t now_milliseconds
    );
    void clear_game(std::string_view uuid);
    [[nodiscard]] std::size_t size() const;

   private:
    void prune_locked(std::int64_t now_milliseconds);
    const preview_t *find_locked(std::string_view uuid, std::string_view token, kind_e kind, std::int64_t now_milliseconds);

    std::size_t maximum_entries_;
    std::uintmax_t maximum_total_bytes_;
    std::int64_t ttl_milliseconds_;
    token_factory_t token_factory_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, preview_t> entries_;
    std::uintmax_t total_bytes_ = 0;
  };

  /** One alternative as a client sees it: an opaque token, its kind and when it expires. */
  struct choice_t {
    std::string token;
    kind_e kind;
    std::int64_t expires_at;  ///< epoch milliseconds, like preview_expires_at on search candidates
  };

  struct choice_listing_t {
    std::vector<choice_t> choices;
    std::optional<search_failure_t> failure;  ///< set when nothing could be listed for a reason the player can act on
  };

  /**
   * List up to maximum_choice_count SteamGridDB images of one kind for a match, with
   * the match path's filters, and publish a bounded preview of each into the cache.
   * The transport carries the API key; nothing here sees it.
   */
  [[nodiscard]] choice_listing_t list_artwork_choices(
    preview_cache_t &cache,
    std::string_view uuid,
    kind_e kind,
    const match_selection_t &identity,
    const providers::transport_t &transport,
    std::int64_t now_milliseconds
  );

  /** One SteamGridDB game a search found, with the opaque token of its first poster when one could be fetched. */
  struct match_candidate_preview_t {
    providers::match_candidate_t candidate;
    std::optional<std::string> poster_token;  ///< a poster preview in the cache, scoped to the searched uuid
    std::int64_t preview_expires_at = 0;
  };

  struct match_candidate_search_t {
    bool invalid_query = false;  ///< the query cannot become a SteamGridDB search at all
    std::optional<search_failure_t> failure;  ///< SteamGridDB did not answer the search
    std::vector<match_candidate_preview_t> candidates;
  };

  /**
   * Search SteamGridDB for up to maximum_candidate_count games matching a sanitized query and
   * publish each one's first poster into the preview cache under uuid. This is the search behind
   * Nova's Artwork Studio and the console's Find Cover, so both list the same games for a title.
   * A candidate whose poster cannot be fetched is still listed, without a preview. An exception
   * from the search request itself propagates, so a route can answer it as an upstream failure.
   */
  [[nodiscard]] match_candidate_search_t search_match_candidates(
    preview_cache_t &cache,
    std::string_view uuid,
    std::string_view query,
    const providers::transport_t &transport,
    std::int64_t now_milliseconds
  );

  [[nodiscard]] nlohmann::json artwork_choice_json(std::string_view uuid, const choice_t &choice);
  [[nodiscard]] nlohmann::json artwork_choices_json(std::string_view uuid, kind_e kind, const std::vector<choice_t> &choices);

  struct selected_download_plan_t {
    std::vector<providers::request_t> downloads;  ///< one download per pick, in the selection's order
    std::optional<search_failure_t> refusal;  ///< artwork_choice_expired or artwork_choice_mismatch, with a 4xx status
  };

  /** Resolve every pick to the image it names before anything touches the network or the disk. */
  [[nodiscard]] selected_download_plan_t plan_selected_downloads(
    preview_cache_t &cache,
    std::string_view uuid,
    const match_selection_t &selection,
    std::int64_t now_milliseconds
  );

  enum class apply_stage_e { published, asset_download, staging, commit };

  /**
   * Copy the live override image of every kind outside selected_kinds into the
   * staging root, so the commit, which replaces the whole override generation, keeps
   * custom images the player did not touch.
   */
  [[nodiscard]] bool stage_unselected_override_assets(
    const std::filesystem::path &appdata,
    const std::filesystem::path &staging_appdata,
    std::string_view uuid,
    const std::vector<kind_e> &selected_kinds
  );

  /**
   * Download the planned images into staging_appdata and commit them as the game's
   * override with the selection's identity. A match by kinds succeeds when any kind
   * lands and replaces the override generation. Picks must all land, and they keep
   * the override images of kinds the player did not pick.
   */
  [[nodiscard]] apply_stage_e publish_artwork_override(
    const std::filesystem::path &appdata,
    const std::filesystem::path &staging_appdata,
    std::string_view uuid,
    const match_selection_t &selection,
    const std::vector<providers::request_t> &downloads,
    const providers::transport_t &transport,
    std::int64_t now_milliseconds
  );
}  // namespace game_artwork::manual
