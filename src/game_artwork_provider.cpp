#include "game_artwork_provider.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
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
    constexpr std::size_t maximum_match_candidate_count = 10;
    constexpr std::size_t maximum_match_title_bytes = 160;

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

    std::optional<std::uint64_t> positive_json_integer(const json &value) {
      if (value.is_number_unsigned()) {
        const auto parsed = value.get<std::uint64_t>();
        if (parsed != 0) return parsed;
      } else if (value.is_number_integer()) {
        const auto parsed = value.get<std::int64_t>();
        if (parsed > 0) return static_cast<std::uint64_t>(parsed);
      }
      return std::nullopt;
    }

    std::optional<std::string> sanitized_match_title(const json &result) {
      if (!result.contains("name") || !result["name"].is_string()) return std::nullopt;

      const auto raw = result["name"].get<std::string>();
      for (std::size_t index = 0; index < raw.size(); ++index) {
        const auto byte = static_cast<unsigned char>(raw[index]);
        if (byte < 0x20 || byte == 0x7F) return std::nullopt;
        if (byte == 0xC2 && index + 1 < raw.size()) {
          const auto next = static_cast<unsigned char>(raw[index + 1]);
          if (next >= 0x80 && next <= 0x9F) return std::nullopt;
        }
      }
      try {
        static_cast<void>(json(raw).dump());
      } catch (const json::exception &) {
        return std::nullopt;
      }

      const auto trimmed = trim_ascii_whitespace(raw);
      if (trimmed.empty() || trimmed.size() > maximum_match_title_bytes) return std::nullopt;
      return std::string {trimmed};
    }

    std::string normalized_match_title(const std::string_view value) {
      std::string normalized;
      normalized.reserve(std::min(value.size(), maximum_match_title_bytes));
      bool pending_separator = false;
      std::size_t inspected = 0;
      for (const unsigned char byte : value) {
        if (inspected++ == maximum_match_title_bytes) break;

        const bool ascii_digit = byte >= '0' && byte <= '9';
        const bool ascii_letter = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
        if (ascii_digit || ascii_letter || byte >= 0x80) {
          if (pending_separator && !normalized.empty()) normalized.push_back(' ');
          normalized.push_back(
            byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : static_cast<char>(byte)
          );
          pending_separator = false;
        } else {
          pending_separator = true;
        }
      }
      return normalized;
    }

    double normalized_title_similarity(const std::string_view query, const std::string_view title) {
      const auto left = normalized_match_title(query);
      const auto right = normalized_match_title(title);
      if (left.empty() || right.empty()) return 0.0;

      std::vector<std::size_t> previous(right.size() + 1);
      std::vector<std::size_t> current(right.size() + 1);
      for (std::size_t column = 0; column <= right.size(); ++column) previous[column] = column;

      for (std::size_t row = 1; row <= left.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1; column <= right.size(); ++column) {
          const auto substitution_cost = left[row - 1] == right[column - 1] ? 0U : 1U;
          current[column] = std::min({
            previous[column] + 1,
            current[column - 1] + 1,
            previous[column - 1] + substitution_cost,
          });
        }
        previous.swap(current);
      }

      const auto longest = std::max(left.size(), right.size());
      return 1.0 - static_cast<double>(previous[right.size()]) / static_cast<double>(longest);
    }

    std::optional<std::string> sanitized_steam_appid(const json &result) {
      if (!result.contains("steam_appid")) return std::nullopt;
      const auto &value = result["steam_appid"];

      std::string appid;
      if (value.is_string()) {
        appid = value.get<std::string>();
      } else if (const auto parsed = positive_json_integer(value)) {
        appid = std::to_string(*parsed);
      } else {
        return std::nullopt;
      }

      if (!is_valid_steam_appid(appid) ||
          std::all_of(appid.begin(), appid.end(), [](const char digit) { return digit == '0'; })) {
        return std::nullopt;
      }
      return appid;
    }

    std::optional<unsigned int> sanitized_release_year(const json &result) {
      if (!result.contains("release_year")) return std::nullopt;
      const auto year = positive_json_integer(result["release_year"]);
      if (!year || *year < 1970 || *year > 2100) return std::nullopt;
      return static_cast<unsigned int>(*year);
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
      const std::vector<unsigned char> &body,
      const execution_options_t &options
    ) {
      namespace fs = std::filesystem;
      if (!request.kind || body.empty() || body.size() > maximum_asset_bytes) return std::nullopt;
      const auto destination_source = options.destination_source.value_or(source_for_provider(request.provider));

      const auto directory = cache_root(appdata) / std::string(uuid);
      std::error_code error;
      fs::create_directories(directory, error);
      if (error) return std::nullopt;

      static std::atomic_uint64_t sequence {0};
      temporary_file_t temporary {
        directory / ("." + std::string(kind_name(*request.kind)) + "." +
                     std::string(source_name(destination_source)) + "." +
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
        destination_source,
        extension
      );
      if (!destination) return std::nullopt;

      // A valid cache entry may have appeared while the transport was active.
      // Publish only when this provider still outranks the selected source.
      if (!options.force_replace && !needs_source_upgrade(appdata, uuid, *request.kind, destination_source)) {
        return std::nullopt;
      }

      fs::rename(temporary.path, *destination, error);
      if (error) return std::nullopt;
      temporary.path.clear();
      for (const auto suffix : std::array<std::string_view, 4> {".png", ".jpg", ".jpeg", ".webp"}) {
        const auto stale = cache_asset_path(appdata, uuid, *request.kind, destination_source, suffix);
        if (!stale || *stale == *destination) continue;
        error.clear();
        fs::remove(*stale, error);
      }
      return asset_t {*request.kind, destination_source, *destination, *mime_type};
    }
  }

  std::vector<asset_t> execute_download_plan(
    const std::filesystem::path &appdata,
    const std::string_view uuid,
    const std::vector<request_t> &requests,
    const transport_t &transport,
    const execution_options_t &options
  ) {
    if (!is_valid_uuid(uuid)) return {};
    if (!transport) return scan_cached_assets(appdata, uuid);

    std::set<kind_e> published_kinds;
    for (const auto &request : requests) {
      const auto output_source = options.destination_source.value_or(source_for_provider(request.provider));
      if (request.operation != operation_e::download || !request.kind ||
          published_kinds.contains(*request.kind) ||
          !is_allowed_provider_url(request.provider, request.url) ||
          (!options.force_replace && !needs_source_upgrade(appdata, uuid, *request.kind, output_source))) {
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
        if (const auto published = commit_download(appdata, uuid, request, response->body, options)) {
          published_kinds.emplace(published->kind);
          if (options.on_published) options.on_published(*published);
        }
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

  std::vector<match_candidate_t> parse_steamgriddb_match_candidates(
    const std::string_view query,
    const std::string_view response_body,
    const std::size_t maximum_candidates
  ) {
    const auto bounded_maximum = std::min(maximum_candidates, maximum_match_candidate_count);
    if (bounded_maximum == 0) return {};

    const auto response = parse_response(response_body);
    if (!is_success_response(response) || !response.contains("data") || !response["data"].is_array()) {
      return {};
    }

    std::vector<match_candidate_t> candidates;
    candidates.reserve(bounded_maximum);
    std::unordered_set<std::uint64_t> seen_ids;
    for (const auto &result : response["data"]) {
      if (!result.is_object() || !result.contains("id")) continue;

      const auto id = positive_json_integer(result["id"]);
      const auto title = sanitized_match_title(result);
      if (!id || !title || !seen_ids.emplace(*id).second) continue;

      candidates.push_back({
        "steamgriddb",
        std::to_string(*id),
        *title,
        sanitized_steam_appid(result),
        sanitized_release_year(result),
        normalized_title_similarity(query, *title),
      });
      if (candidates.size() == bounded_maximum) break;
    }
    return candidates;
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
      if (!asset.is_object()) continue;

      std::optional<std::string> selected_url;
      if (kind == kind_e::icon && asset.contains("thumb") && asset["thumb"].is_string()) {
        const auto thumb = asset["thumb"].get<std::string>();
        if (is_allowed_provider_url(provider_e::steamgriddb, thumb)) selected_url = thumb;
      }
      if (!selected_url && asset.contains("url") && asset["url"].is_string()) {
        const auto url = asset["url"].get<std::string>();
        if (is_allowed_provider_url(provider_e::steamgriddb, url)) selected_url = url;
      }
      if (!selected_url || !seen.emplace(*selected_url).second) continue;
      candidates.push_back({kind, source_e::steamgriddb, *selected_url});
    }
    return candidates;
  }
}
