#pragma once

#include "game_artwork.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace game_artwork::providers {
  /**
   * The network operation a caller must perform. Provider planning is pure:
   * this module never reads configuration, injects credentials, or performs I/O.
   */
  enum class operation_e {
    download,
    search,
    list,
  };

  struct request_t {
    provider_e provider;
    operation_e operation;
    std::optional<kind_e> kind;
    std::string url;
    bool requires_authorization;
  };

  struct candidate_t {
    kind_e kind;
    source_e source;
    std::string url;
  };

  struct transport_response_t {
    unsigned int status_code;
    std::vector<unsigned char> body;
    // Empty means the request URL was the effective URL. A transport that
    // follows a redirect must report the final URL for a second allowlist check.
    std::string final_url;
  };

  using transport_t = std::function<std::optional<transport_response_t>(
    const request_t &request,
    std::uintmax_t maximum_bytes
  )>;

  /**
   * Execute allowlisted download requests with caller-injected I/O.
   *
   * Each kind is isolated: successful images are atomically moved into the
   * deterministic cache, while failed/invalid/oversized responses leave no
   * partial file and do not remove valid cached artwork. The returned vector is
   * the complete valid cache state, including assets that existed beforehand.
   */
  [[nodiscard]] std::vector<asset_t> execute_download_plan(
    const std::filesystem::path &appdata,
    std::string_view uuid,
    const std::vector<request_t> &requests,
    const transport_t &transport
  );

  /** Plan deterministic downloads from Steam's public app-art CDN. */
  std::vector<request_t> plan_steam_assets(std::string_view appid);

  /** Plan the first SteamGridDB lookup without embedding the API key. */
  std::optional<request_t> plan_steamgriddb_search(std::string_view title);

  /** Parse a ranked SteamGridDB search response, returning the first valid ID. */
  std::optional<std::uint64_t> parse_steamgriddb_game_id(std::string_view response_body);

  /** Plan one SteamGridDB metadata request per supported artwork kind. */
  std::vector<request_t> plan_steamgriddb_assets(std::uint64_t game_id);

  /** Parse and allowlist artwork URLs from a SteamGridDB metadata response. */
  std::vector<candidate_t> parse_steamgriddb_assets(kind_e kind, std::string_view response_body);
}
