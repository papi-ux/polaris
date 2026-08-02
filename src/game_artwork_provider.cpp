#include "game_artwork_provider.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <cctype>
#include <unordered_set>

namespace game_artwork::providers {
  namespace {
    using json = nlohmann::json;

    constexpr std::string_view steam_cdn_root =
      "https://cdn.cloudflare.steamstatic.com/steam/apps/";
    constexpr std::string_view steamgriddb_api_root =
      "https://www.steamgriddb.com/api/v2/";

    std::string_view trim_ascii_whitespace(std::string_view value) {
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
      }
      while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
      }
      return value;
    }

    std::string percent_encode_path_segment(std::string_view value) {
      constexpr char hex[] = "0123456789ABCDEF";
      std::string encoded;
      encoded.reserve(value.size());
      for (const unsigned char byte : value) {
        if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
          encoded.push_back(static_cast<char>(byte));
        } else {
          encoded.push_back('%');
          encoded.push_back(hex[byte >> 4]);
          encoded.push_back(hex[byte & 0x0F]);
        }
      }
      return encoded;
    }

    json parse_response(std::string_view response_body) {
      if (response_body.empty()) {
        return json::value_t::discarded;
      }
      return json::parse(response_body.begin(), response_body.end(), nullptr, false);
    }

    bool is_success_response(const json &response) {
      return response.is_object() &&
             response.contains("success") &&
             response["success"].is_boolean() &&
             response["success"].get<bool>();
    }

    request_t steamgriddb_request(operation_e operation, std::optional<kind_e> kind, std::string url) {
      return {
        provider_e::steamgriddb,
        operation,
        kind,
        std::move(url),
        true,
      };
    }

    source_e source_for_provider(provider_e provider) {
      return provider == provider_e::steam ? source_e::steam : source_e::steamgriddb;
    }

    std::string extension_for_mime(std::string_view mime_type) {
      if (mime_type == "image/png") return ".png";
      if (mime_type == "image/jpeg") return ".jpg";
      if (mime_type == "image/webp") return ".webp";
      return {};
    }

    struct temporary_file_t {
      std::filesystem::path path;

      ~temporary_file_t() {
        if (path.empty()) return;
        std::error_code error;
        std::filesystem::remove(path, error);
      }
    };

    std::optional<asset_t> commit_download(
      const std::filesystem::path &appdata,
      std::string_view uuid,
      const request_t &request,
      const std::vector<unsigned char> &body
    ) {
      namespace fs = std::filesystem;
      if (!request.kind || body.empty() || body.size() > maximum_asset_bytes) return std::nullopt;

      const auto directory = cache_root(appdata) / std::string(uuid);
      std::error_code error;
      fs::create_directories(directory, error);
      if (error) return std::nullopt;

      static std::atomic_uint64_t sequence {0};
      temporary_file_t temporary {
        directory / ("." + std::string(kind_name(*request.kind)) + "." +
                     std::string(source_name(source_for_provider(request.provider))) + "." +
                     std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp")
      };
      {
        std::ofstream output(temporary.path, std::ios::binary | std::ios::trunc);
        if (!output) return std::nullopt;
        output.write(
          reinterpret_cast<const char *>(body.data()),
          static_cast<std::streamsize>(body.size())
        );
        output.close();
        if (!output) return std::nullopt;
      }

      const auto mime_type = image_mime_type(temporary.path);
      if (!mime_type) return std::nullopt;
      const auto extension = extension_for_mime(*mime_type);
      const auto destination = cache_asset_path(
        appdata,
        uuid,
        *request.kind,
        source_for_provider(request.provider),
        extension
      );
      if (!destination) return std::nullopt;

      // A valid cache entry may have appeared while the transport was active.
      // Publish only when this provider still outranks the selected source.
      if (!needs_source_upgrade(appdata, uuid, *request.kind, source_for_provider(request.provider))) return std::nullopt;

      fs::rename(temporary.path, *destination, error);
      if (error) return std::nullopt;
      temporary.path.clear();
      return asset_t {*request.kind, source_for_provider(request.provider), *destination, *mime_type};
    }
  }

  std::vector<asset_t> execute_download_plan(
    const std::filesystem::path &appdata,
    const std::string_view uuid,
    const std::vector<request_t> &requests,
    const transport_t &transport
  ) {
    if (!is_valid_uuid(uuid)) return {};
    if (!transport) return scan_cached_assets(appdata, uuid);

    for (const auto &request : requests) {
      if (request.operation != operation_e::download || !request.kind ||
          !is_allowed_provider_url(request.provider, request.url) ||
          !needs_source_upgrade(appdata, uuid, *request.kind, source_for_provider(request.provider))) {
        continue;
      }

      // Transport and malformed upstream failures are isolated to this request.
      // A later candidate for the same kind can still succeed.
      try {
        const auto response = transport(request, maximum_asset_bytes);
        if (!response || response->status_code < 200 || response->status_code >= 300 ||
            response->body.empty() || response->body.size() > maximum_asset_bytes) {
          continue;
        }
        const auto &effective_url = response->final_url.empty() ? request.url : response->final_url;
        if (!is_allowed_provider_url(request.provider, effective_url)) continue;
        (void) commit_download(appdata, uuid, request, response->body);
      } catch (...) {
        // The next kind/candidate must still be attempted. Any temporary file
        // created by commit_download is removed by its scope guard.
      }
    }
    return scan_cached_assets(appdata, uuid);
  }

  std::vector<request_t> plan_steam_assets(const std::string_view appid) {
    if (!is_valid_steam_appid(appid)) {
      return {};
    }

    const std::string root = std::string {steam_cdn_root} + std::string {appid} + '/';
    return {
      {provider_e::steam, operation_e::download, kind_e::poster, root + "library_600x900.jpg", false},
      {provider_e::steam, operation_e::download, kind_e::hero, root + "library_hero.jpg", false},
      {provider_e::steam, operation_e::download, kind_e::logo, root + "logo.png", false},
    };
  }

  std::optional<request_t> plan_steamgriddb_search(const std::string_view title) {
    const auto trimmed = trim_ascii_whitespace(title);
    if (trimmed.empty()) {
      return std::nullopt;
    }

    auto request = steamgriddb_request(
      operation_e::search,
      std::nullopt,
      std::string {steamgriddb_api_root} + "search/autocomplete/" + percent_encode_path_segment(trimmed)
    );
    if (!is_allowed_provider_url(provider_e::steamgriddb, request.url)) {
      return std::nullopt;
    }
    return request;
  }

  std::optional<std::uint64_t> parse_steamgriddb_game_id(const std::string_view response_body) {
    const auto response = parse_response(response_body);
    if (!is_success_response(response) || !response.contains("data") || !response["data"].is_array()) {
      return std::nullopt;
    }

    for (const auto &result : response["data"]) {
      if (!result.is_object() || !result.contains("id")) {
        continue;
      }
      const auto &id = result["id"];
      if (id.is_number_unsigned()) {
        const auto value = id.get<std::uint64_t>();
        if (value != 0) {
          return value;
        }
      } else if (id.is_number_integer()) {
        const auto value = id.get<std::int64_t>();
        if (value > 0) {
          return static_cast<std::uint64_t>(value);
        }
      }
    }
    return std::nullopt;
  }

  std::vector<request_t> plan_steamgriddb_assets(const std::uint64_t game_id) {
    if (game_id == 0) {
      return {};
    }

    const std::string root = std::string {steamgriddb_api_root};
    const std::string id = std::to_string(game_id);
    std::vector<request_t> requests {
      steamgriddb_request(operation_e::list, kind_e::poster,
                          root + "grids/game/" + id + "?dimensions=600x900&types=static&limit=5"),
      steamgriddb_request(operation_e::list, kind_e::hero,
                          root + "heroes/game/" + id + "?types=static&limit=5"),
      steamgriddb_request(operation_e::list, kind_e::logo,
                          root + "logos/game/" + id + "?types=static&limit=5"),
      steamgriddb_request(operation_e::list, kind_e::icon,
                          root + "icons/game/" + id + "?types=static&limit=5"),
    };

    for (const auto &request : requests) {
      if (!is_allowed_provider_url(provider_e::steamgriddb, request.url)) {
        return {};
      }
    }
    return requests;
  }

  std::vector<candidate_t> parse_steamgriddb_assets(
    const kind_e kind,
    const std::string_view response_body
  ) {
    const auto response = parse_response(response_body);
    if (!is_success_response(response) || !response.contains("data") || !response["data"].is_array()) {
      return {};
    }

    std::vector<candidate_t> candidates;
    std::unordered_set<std::string> seen;
    for (const auto &asset : response["data"]) {
      if (!asset.is_object() || !asset.contains("url") || !asset["url"].is_string()) {
        continue;
      }

      const auto url = asset["url"].get<std::string>();
      if (!is_allowed_provider_url(provider_e::steamgriddb, url) || !seen.emplace(url).second) {
        continue;
      }
      candidates.push_back({kind, source_e::steamgriddb, url});
    }
    return candidates;
  }
}
