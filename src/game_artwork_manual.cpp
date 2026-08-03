#include "game_artwork_manual.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <random>
#include <set>
#include <sstream>

namespace game_artwork::manual {
  namespace {
    using json = nlohmann::json;

    bool valid_token(const std::string_view value) {
      return value.size() == 32 && std::all_of(value.begin(), value.end(), [](const unsigned char c) {
        return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
      });
    }

    std::string random_token() {
      std::random_device random;
      std::ostringstream output;
      output << std::hex << std::setfill('0');
      for (int index = 0; index < 4; ++index) {
        output << std::setw(8) << static_cast<std::uint32_t>(random());
      }
      return output.str();
    }

    std::optional<std::string> sanitized_title(const json &value) {
      if (!value.is_string()) return std::nullopt;
      auto title = value.get<std::string>();
      const auto first = title.find_first_not_of(" \t\r\n");
      if (first == std::string::npos) return std::nullopt;
      const auto last = title.find_last_not_of(" \t\r\n");
      title = title.substr(first, last - first + 1);
      if (title.empty() || title.size() > maximum_search_query_bytes) return std::nullopt;
      for (std::size_t index = 0; index < title.size(); ++index) {
        const auto byte = static_cast<unsigned char>(title[index]);
        if (byte < 0x20 || byte == 0x7f) return std::nullopt;
        if (byte == 0xc2 && index + 1 < title.size()) {
          const auto next = static_cast<unsigned char>(title[index + 1]);
          if (next >= 0x80 && next <= 0x9f) return std::nullopt;
        }
      }
      try {
        static_cast<void>(json(title).dump());
      } catch (const json::exception &) {
        return std::nullopt;
      }
      return title;
    }

    bool positive_identifier(const std::string_view value) {
      return !value.empty() && value.size() <= 20 &&
             std::all_of(value.begin(), value.end(), [](const unsigned char c) { return std::isdigit(c) != 0; }) &&
             std::any_of(value.begin(), value.end(), [](const char c) { return c != '0'; });
    }

    std::optional<std::string> mime_from_signature(const std::vector<unsigned char> &body) {
      constexpr std::array<unsigned char, 8> png_signature {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
      if (body.size() >= png_signature.size() &&
          std::equal(png_signature.begin(), png_signature.end(), body.begin())) return "image/png";
      if (body.size() >= 3 && body[0] == 0xff && body[1] == 0xd8 && body[2] == 0xff) return "image/jpeg";
      if (body.size() >= 12 && body[0] == 'R' && body[1] == 'I' && body[2] == 'F' && body[3] == 'F' &&
          body[8] == 'W' && body[9] == 'E' && body[10] == 'B' && body[11] == 'P') return "image/webp";
      return std::nullopt;
    }
  }

  std::string request_log_value(const std::string_view name, const std::string_view value) {
    std::string normalized(name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](const unsigned char character) {
      if (character == '-') return '_';
      return static_cast<char>(std::tolower(character));
    });
    const bool sensitive = normalized == "authorization" || normalized == "proxy_authorization" ||
      normalized == "cookie" || normalized == "set_cookie" || normalized == "token" ||
      normalized == "apikey" || normalized.contains("password") || normalized.contains("passwd") ||
      normalized.contains("api_key") || normalized.contains("access_token") ||
      normalized.contains("refresh_token") || normalized.contains("client_secret") ||
      normalized.ends_with("_token");
    return sensitive ? "[REDACTED]" : std::string(value);
  }

  std::string request_log_path(const std::string_view path) {
    constexpr std::string_view marker = "/artwork/candidate/";
    const auto start = path.find(marker);
    if (start == std::string_view::npos) return std::string(path);
    const auto token_start = start + marker.size();
    const auto token_end = path.find('/', token_start);
    if (token_end == std::string_view::npos) {
      const auto token = path.substr(token_start);
      return valid_token(token)
        ? std::string(path.substr(0, token_start)) + "[REDACTED]"
        : std::string(path);
    }
    if (!valid_token(path.substr(token_start, token_end - token_start))) return std::string(path);
    return std::string(path.substr(0, token_start)) + "[REDACTED]" + std::string(path.substr(token_end));
  }

  std::string request_log_target(
    const std::string_view path,
    const std::vector<std::pair<std::string, std::string>> &query
  ) {
    std::ostringstream output;
    output << request_log_path(path);
    bool first = true;
    for (const auto &[name, value] : query) {
      output << (first ? '?' : '&') << name << '=' << request_log_value(name, value);
      first = false;
    }
    return output.str();
  }

  std::optional<route_request_t> parse_route_target(const std::string_view path) {
    constexpr std::string_view prefix = "/polaris/v1/games/";
    constexpr std::string_view separator = "/artwork/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const auto remainder = path.substr(prefix.size());
    const auto split = remainder.find(separator);
    if (split == std::string_view::npos) return std::nullopt;
    const auto uuid = remainder.substr(0, split);
    if (!is_valid_uuid(uuid)) return std::nullopt;
    const auto suffix = remainder.substr(split + separator.size());
    if (suffix == "candidates") return route_request_t {route_e::search, std::string(uuid), {}, {}};
    if (suffix == "match") return route_request_t {route_e::apply, std::string(uuid), {}, {}};
    if (suffix == "override") return route_request_t {route_e::clear, std::string(uuid), {}, {}};

    constexpr std::string_view candidate_prefix = "candidate/";
    if (!suffix.starts_with(candidate_prefix)) return std::nullopt;
    const auto candidate = suffix.substr(candidate_prefix.size());
    const auto slash = candidate.find('/');
    if (slash == std::string_view::npos || candidate.find('/', slash + 1) != std::string_view::npos) {
      return std::nullopt;
    }
    const auto token = candidate.substr(0, slash);
    const auto kind = parse_kind(candidate.substr(slash + 1));
    if (!valid_token(token) || !kind) return std::nullopt;
    return route_request_t {route_e::preview, std::string(uuid), std::string(token), *kind};
  }

  std::optional<std::string> sanitize_search_query(const std::string_view query) {
    return sanitized_title(json(std::string(query)));
  }

  std::optional<match_selection_t> parse_match_selection(const std::string_view body) {
    if (body.empty() || body.size() > maximum_match_body_bytes) return std::nullopt;
    json document;
    try {
      document = json::parse(body);
    } catch (const json::exception &) {
      return std::nullopt;
    }
    if (!document.is_object()) return std::nullopt;
    const std::set<std::string> allowed {"provider", "provider_game_id", "title", "steam_appid", "kinds"};
    for (const auto &[key, value] : document.items()) {
      static_cast<void>(value);
      if (!allowed.contains(key)) return std::nullopt;
    }
    if (!document.contains("provider") || !document["provider"].is_string() ||
        document["provider"].get<std::string>() != "steamgriddb" ||
        !document.contains("provider_game_id") || !document["provider_game_id"].is_string() ||
        !document.contains("title") || !document.contains("kinds") || !document["kinds"].is_array()) return std::nullopt;
    const auto provider_game_id = document["provider_game_id"].get<std::string>();
    const auto title = sanitized_title(document["title"]);
    if (!positive_identifier(provider_game_id) || !title) return std::nullopt;
    std::optional<std::string> steam_appid;
    if (document.contains("steam_appid")) {
      if (!document["steam_appid"].is_string()) return std::nullopt;
      const auto value = document["steam_appid"].get<std::string>();
      if (!positive_identifier(value) || !is_valid_steam_appid(value)) return std::nullopt;
      steam_appid = value;
    }
    std::vector<kind_e> kinds;
    std::set<kind_e> seen_kinds;
    if (document["kinds"].empty() || document["kinds"].size() > 4) return std::nullopt;
    for (const auto &value : document["kinds"]) {
      if (!value.is_string()) return std::nullopt;
      const auto kind = parse_kind(value.get<std::string>());
      if (!kind || !seen_kinds.emplace(*kind).second) return std::nullopt;
      kinds.push_back(*kind);
    }
    return match_selection_t {"steamgriddb", provider_game_id, *title, steam_appid, std::move(kinds)};
  }

  preview_cache_t::preview_cache_t(
    const std::size_t maximum_entries,
    const std::uintmax_t maximum_total_bytes,
    const std::int64_t ttl_milliseconds,
    token_factory_t token_factory
  ):
      maximum_entries_(maximum_entries),
      maximum_total_bytes_(maximum_total_bytes),
      ttl_milliseconds_(ttl_milliseconds),
      token_factory_(token_factory ? std::move(token_factory) : token_factory_t(random_token)) {
  }

  void preview_cache_t::prune_locked(const std::int64_t now_milliseconds) {
    for (auto entry = entries_.begin(); entry != entries_.end();) {
      if (entry->second.expires_at > now_milliseconds) {
        ++entry;
        continue;
      }
      total_bytes_ -= entry->second.body.size();
      entry = entries_.erase(entry);
    }
  }

  std::optional<preview_t> preview_cache_t::publish(
    const std::string_view uuid,
    const kind_e kind,
    std::vector<unsigned char> body,
    const std::int64_t now_milliseconds
  ) {
    const auto mime_type = mime_from_signature(body);
    if (!is_valid_uuid(uuid) || !mime_type || body.empty() || body.size() > maximum_preview_bytes ||
        maximum_entries_ == 0 || body.size() > maximum_total_bytes_ || ttl_milliseconds_ <= 0 ||
        now_milliseconds < 0 || now_milliseconds > std::numeric_limits<std::int64_t>::max() - ttl_milliseconds_) {
      return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    prune_locked(now_milliseconds);
    while (!entries_.empty() &&
           (entries_.size() >= maximum_entries_ || total_bytes_ + body.size() > maximum_total_bytes_)) {
      const auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto &left, const auto &right) {
        if (left.second.expires_at != right.second.expires_at) return left.second.expires_at < right.second.expires_at;
        return left.first < right.first;
      });
      total_bytes_ -= oldest->second.body.size();
      entries_.erase(oldest);
    }
    std::string token;
    for (int attempt = 0; attempt < 8; ++attempt) {
      token = token_factory_();
      if (valid_token(token) && !entries_.contains(token)) break;
      token.clear();
    }
    if (token.empty()) return std::nullopt;
    preview_t preview {token, std::string(uuid), kind, *mime_type, std::move(body), now_milliseconds + ttl_milliseconds_};
    total_bytes_ += preview.body.size();
    entries_.emplace(token, preview);
    return preview;
  }

  std::optional<preview_t> preview_cache_t::lookup(
    const std::string_view uuid,
    const std::string_view token,
    const kind_e kind,
    const std::int64_t now_milliseconds
  ) {
    if (!is_valid_uuid(uuid) || !valid_token(token) || now_milliseconds < 0) return std::nullopt;
    std::lock_guard lock(mutex_);
    prune_locked(now_milliseconds);
    const auto found = entries_.find(std::string(token));
    if (found == entries_.end() || found->second.uuid != uuid || found->second.kind != kind) return std::nullopt;
    return found->second;
  }

  void preview_cache_t::clear_game(const std::string_view uuid) {
    std::lock_guard lock(mutex_);
    for (auto entry = entries_.begin(); entry != entries_.end();) {
      if (entry->second.uuid != uuid) {
        ++entry;
        continue;
      }
      total_bytes_ -= entry->second.body.size();
      entry = entries_.erase(entry);
    }
  }

  std::size_t preview_cache_t::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
  }
}  // namespace game_artwork::manual
