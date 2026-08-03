#pragma once

#include "game_artwork.h"

#include <cstddef>
#include <cstdint>
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
  inline constexpr std::uintmax_t maximum_preview_bytes = 2U * 1024U * 1024U;

  enum class route_e { search, apply, clear, preview };

  struct route_request_t {
    route_e route;
    std::string uuid;
    std::optional<std::string> token;
    std::optional<kind_e> kind;
  };

  struct match_selection_t {
    std::string provider;
    std::string provider_game_id;
    std::string title;
    std::optional<std::string> steam_appid;
    std::vector<kind_e> kinds;
  };

  struct preview_t {
    std::string token;
    std::string uuid;
    kind_e kind;
    std::string mime_type;
    std::vector<unsigned char> body;
    std::int64_t expires_at;
  };

  [[nodiscard]] std::optional<route_request_t> parse_route_target(std::string_view path);
  [[nodiscard]] std::string request_log_value(std::string_view name, std::string_view value);
  [[nodiscard]] std::string request_log_path(std::string_view path);
  [[nodiscard]] std::string request_log_target(
    std::string_view path,
    const std::vector<std::pair<std::string, std::string>> &query
  );
  [[nodiscard]] std::optional<std::string> sanitize_search_query(std::string_view query);
  [[nodiscard]] std::optional<match_selection_t> parse_match_selection(std::string_view body);

  class preview_cache_t {
   public:
    using token_factory_t = std::function<std::string()>;

    explicit preview_cache_t(
      std::size_t maximum_entries = 32,
      std::uintmax_t maximum_total_bytes = 32U * 1024U * 1024U,
      std::int64_t ttl_milliseconds = 5 * 60 * 1000,
      token_factory_t token_factory = {}
    );

    [[nodiscard]] std::optional<preview_t> publish(
      std::string_view uuid,
      kind_e kind,
      std::vector<unsigned char> body,
      std::int64_t now_milliseconds
    );
    [[nodiscard]] std::optional<preview_t> lookup(
      std::string_view uuid,
      std::string_view token,
      kind_e kind,
      std::int64_t now_milliseconds
    );
    void clear_game(std::string_view uuid);
    [[nodiscard]] std::size_t size() const;

   private:
    void prune_locked(std::int64_t now_milliseconds);

    std::size_t maximum_entries_;
    std::uintmax_t maximum_total_bytes_;
    std::int64_t ttl_milliseconds_;
    token_factory_t token_factory_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, preview_t> entries_;
    std::uintmax_t total_bytes_ = 0;
  };
}  // namespace game_artwork::manual
