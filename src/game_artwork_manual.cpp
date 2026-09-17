#include "game_artwork_manual.h"

#include "game_artwork_override.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <iomanip>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <system_error>

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

    std::optional<json> parse_bounded_object(const std::string_view body) {
      if (body.empty() || body.size() > maximum_match_body_bytes) return std::nullopt;
      try {
        auto document = json::parse(body);
        if (!document.is_object()) return std::nullopt;
        return document;
      } catch (const json::exception &) {
        return std::nullopt;
      }
    }

    bool has_only_keys(const json &document, const std::set<std::string> &allowed) {
      for (const auto &item : document.items()) {
        if (!allowed.contains(item.key())) return false;
      }
      return true;
    }

    // The match identity every artwork body carries. Callers refuse unknown keys first.
    std::optional<match_selection_t> parse_match_identity(const json &document) {
      if (!document.contains("provider") || !document.at("provider").is_string() ||
          document.at("provider").get<std::string>() != "steamgriddb" ||
          !document.contains("provider_game_id") || !document.at("provider_game_id").is_string() ||
          !document.contains("title")) return std::nullopt;
      const auto provider_game_id = document.at("provider_game_id").get<std::string>();
      const auto title = sanitized_title(document.at("title"));
      if (!positive_identifier(provider_game_id) || !title) return std::nullopt;
      std::optional<std::string> steam_appid;
      if (document.contains("steam_appid")) {
        if (!document.at("steam_appid").is_string()) return std::nullopt;
        const auto value = document.at("steam_appid").get<std::string>();
        if (!positive_identifier(value) || !is_valid_steam_appid(value)) return std::nullopt;
        steam_appid = value;
      }
      return match_selection_t {"steamgriddb", provider_game_id, *title, steam_appid, {}, {}};
    }

    std::optional<std::uint64_t> provider_game_number(const std::string_view value) {
      std::uint64_t number = 0;
      const auto *const end = value.data() + value.size();
      const auto [parsed_end, error] = std::from_chars(value.data(), end, number);
      if (error != std::errc {} || parsed_end != end || number == 0) return std::nullopt;
      return number;
    }

    bool successful_status(const unsigned int status) {
      return status >= 200 && status < 300;
    }

    search_failure_t invalid_match_failure() {
      return {"artwork_match_invalid", "That artwork match is not valid. Search for the game again.", 400};
    }

    search_failure_t choice_expired_failure() {
      return {"artwork_choice_expired", "That artwork choice expired. Load the alternatives again.", 409};
    }

    search_failure_t choice_mismatch_failure() {
      return {"artwork_choice_mismatch", "That artwork choice belongs to a different match. Load the alternatives again.", 409};
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

    constexpr std::string_view choices_prefix = "choices/";
    if (suffix.starts_with(choices_prefix)) {
      const auto kind = parse_kind(suffix.substr(choices_prefix.size()));
      if (!kind) return std::nullopt;
      return route_request_t {route_e::choices, std::string(uuid), {}, *kind};
    }

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
    const auto document = parse_bounded_object(body);
    if (!document ||
        !has_only_keys(*document, {"provider", "provider_game_id", "title", "steam_appid", "kinds", "selections"})) {
      return std::nullopt;
    }
    auto selection = parse_match_identity(*document);
    if (!selection || document->contains("kinds") == document->contains("selections")) return std::nullopt;

    if (document->contains("kinds")) {
      const auto &kinds = document->at("kinds");
      if (!kinds.is_array() || kinds.empty() || kinds.size() > 4) return std::nullopt;
      std::set<kind_e> seen_kinds;
      for (const auto &value : kinds) {
        if (!value.is_string()) return std::nullopt;
        const auto kind = parse_kind(value.get<std::string>());
        if (!kind || !seen_kinds.emplace(*kind).second) return std::nullopt;
        selection->kinds.push_back(*kind);
      }
      return selection;
    }

    // Picks name images only by the opaque tokens a choice list issued. Whatever a
    // token stands for stays on the host until plan_selected_downloads resolves it.
    const auto &selections = document->at("selections");
    if (!selections.is_object() || selections.empty() || selections.size() > 4) return std::nullopt;
    std::map<kind_e, std::string> picks;
    std::set<std::string> seen_tokens;
    for (const auto &item : selections.items()) {
      const auto kind = parse_kind(item.key());
      if (!kind || !item.value().is_string()) return std::nullopt;
      auto token = item.value().get<std::string>();
      if (!valid_token(token) || !seen_tokens.emplace(token).second) return std::nullopt;
      picks.emplace(*kind, std::move(token));
    }
    for (auto &[kind, token] : picks) {
      selection->kinds.push_back(kind);
      selection->selections.push_back({kind, std::move(token)});
    }
    return selection;
  }

  std::optional<match_selection_t> parse_choice_request(const std::string_view body) {
    const auto document = parse_bounded_object(body);
    if (!document || !has_only_keys(*document, {"provider", "provider_game_id", "title", "steam_appid"})) {
      return std::nullopt;
    }
    return parse_match_identity(*document);
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
    const std::int64_t now_milliseconds,
    std::optional<choice_source_t> choice
  ) {
    const auto mime_type = mime_from_signature(body);
    if (!is_valid_uuid(uuid) || !mime_type || body.empty() || body.size() > maximum_preview_bytes ||
        maximum_entries_ == 0 || body.size() > maximum_total_bytes_ || ttl_milliseconds_ <= 0 ||
        now_milliseconds < 0 || now_milliseconds > std::numeric_limits<std::int64_t>::max() - ttl_milliseconds_) {
      return std::nullopt;
    }
    if (choice && (!positive_identifier(choice->provider_game_id) ||
                   choice->asset_url.size() > maximum_choice_url_bytes ||
                   !is_allowed_provider_url(provider_e::steamgriddb, choice->asset_url))) {
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
    preview_t preview {
      token,
      std::string(uuid),
      kind,
      *mime_type,
      std::move(body),
      now_milliseconds + ttl_milliseconds_,
      std::move(choice),
    };
    total_bytes_ += preview.body.size();
    entries_.emplace(token, preview);
    return preview;
  }

  const preview_t *preview_cache_t::find_locked(
    const std::string_view uuid,
    const std::string_view token,
    const kind_e kind,
    const std::int64_t now_milliseconds
  ) {
    prune_locked(now_milliseconds);
    const auto found = entries_.find(std::string(token));
    if (found == entries_.end() || found->second.uuid != uuid || found->second.kind != kind) return nullptr;
    return &found->second;
  }

  std::optional<preview_t> preview_cache_t::lookup(
    const std::string_view uuid,
    const std::string_view token,
    const kind_e kind,
    const std::int64_t now_milliseconds
  ) {
    if (!is_valid_uuid(uuid) || !valid_token(token) || now_milliseconds < 0) return std::nullopt;
    std::lock_guard lock(mutex_);
    const auto *const entry = find_locked(uuid, token, kind, now_milliseconds);
    if (entry == nullptr) return std::nullopt;
    return *entry;
  }

  std::optional<choice_source_t> preview_cache_t::lookup_choice(
    const std::string_view uuid,
    const std::string_view token,
    const kind_e kind,
    const std::int64_t now_milliseconds
  ) {
    if (!is_valid_uuid(uuid) || !valid_token(token) || now_milliseconds < 0) return std::nullopt;
    std::lock_guard lock(mutex_);
    const auto *const entry = find_locked(uuid, token, kind, now_milliseconds);
    if (entry == nullptr) return std::nullopt;
    return entry->choice;
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
  search_failure_t classify_search_failure(bool key_present, std::optional<long> upstream_status) {
    if (!key_present) {
      return {"steamgriddb_key_missing", "SteamGridDB is not configured on the host. Add a SteamGridDB API key in Polaris settings.", 503};
    }
    if (!upstream_status) {
      return {"steamgriddb_unreachable", "Polaris could not reach SteamGridDB.", 502};
    }
    switch (*upstream_status) {
      case 401:
      case 403:
        return {"steamgriddb_unauthorized", "SteamGridDB rejected the host's API key. Update it in Polaris settings.", 502};
      case 429:
        return {"steamgriddb_rate_limited", "SteamGridDB is rate limiting this host. Try again in a minute.", 502};
      default:
        return {"steamgriddb_unavailable", "SteamGridDB did not answer (HTTP " + std::to_string(*upstream_status) + ").", 502};
    }
  }

  choice_listing_t list_artwork_choices(
    preview_cache_t &cache,
    const std::string_view uuid,
    const kind_e kind,
    const match_selection_t &identity,
    const providers::transport_t &transport,
    const std::int64_t now_milliseconds
  ) {
    choice_listing_t listing;
    const auto game_id = provider_game_number(identity.provider_game_id);
    const auto plans = game_id ? providers::plan_steamgriddb_assets(*game_id) : std::vector<providers::request_t> {};
    // The match path's own lookup for this kind, so choices carry its style and size filters.
    const auto plan = std::find_if(plans.begin(), plans.end(), [kind](const auto &request) {
      return request.kind == kind;
    });
    if (!is_valid_uuid(uuid) || plan == plans.end()) {
      listing.failure = invalid_match_failure();
      return listing;
    }
    if (!transport) {
      listing.failure = classify_search_failure(true, std::nullopt);
      return listing;
    }

    std::vector<providers::choice_candidate_t> candidates;
    try {
      const auto response = transport(*plan, maximum_listing_bytes);
      if (!response ||
          !is_allowed_provider_url(plan->provider, response->final_url.empty() ? plan->url : response->final_url)) {
        listing.failure = classify_search_failure(true, std::nullopt);
        return listing;
      }
      if (!successful_status(response->status_code)) {
        listing.failure = classify_search_failure(true, static_cast<long>(response->status_code));
        return listing;
      }
      const std::string body(response->body.begin(), response->body.end());
      candidates = providers::parse_steamgriddb_choices(kind, body, maximum_choice_count);
    } catch (...) {
      listing.failure = classify_search_failure(true, std::nullopt);
      return listing;
    }

    // A preview that cannot be fetched drops only its own choice.
    std::optional<search_failure_t> preview_failure;
    for (const auto &candidate : candidates) {
      const providers::request_t download {
        provider_e::steamgriddb,
        providers::operation_e::download,
        kind,
        candidate.preview_url,
        false,
      };
      try {
        const auto image = transport(download, maximum_preview_bytes);
        if (!image ||
            !is_allowed_provider_url(download.provider, image->final_url.empty() ? download.url : image->final_url)) {
          if (!preview_failure) preview_failure = classify_search_failure(true, std::nullopt);
          continue;
        }
        if (!successful_status(image->status_code)) {
          if (!preview_failure) preview_failure = classify_search_failure(true, static_cast<long>(image->status_code));
          continue;
        }
        const auto published = cache.publish(
          uuid,
          kind,
          image->body,
          now_milliseconds,
          choice_source_t {identity.provider_game_id, candidate.asset_url}
        );
        if (published) listing.choices.push_back({published->token, published->kind, published->expires_at});
      } catch (...) {
        if (!preview_failure) preview_failure = classify_search_failure(true, std::nullopt);
      }
    }
    // Images SteamGridDB listed but the host could not fetch are an upstream failure, not an empty list.
    if (listing.choices.empty() && preview_failure) listing.failure = preview_failure;
    return listing;
  }

  match_candidate_search_t search_match_candidates(
    preview_cache_t &cache,
    const std::string_view uuid,
    const std::string_view query,
    const providers::transport_t &transport,
    const std::int64_t now_milliseconds,
    const candidate_listing_e listing
  ) {
    match_candidate_search_t result;
    const auto search_request = providers::plan_steamgriddb_search(query);
    if (!search_request) {
      result.invalid_query = true;
      return result;
    }
    const auto search_response = transport(*search_request, maximum_listing_bytes);
    if (!search_response || !successful_status(search_response->status_code) ||
        !is_allowed_provider_url(
          provider_e::steamgriddb,
          search_response->final_url.empty() ? search_request->url : search_response->final_url)) {
      result.failure = classify_search_failure(
        true,
        search_response ? std::optional<long>(static_cast<long>(search_response->status_code)) : std::optional<long> {}
      );
      return result;
    }
    const std::string search_body(search_response->body.begin(), search_response->body.end());
    const bool posters_only = listing == candidate_listing_e::matches_with_posters;
    const auto searched = posters_only ? maximum_searched_match_count : maximum_candidate_count;
    for (auto &candidate : providers::parse_steamgriddb_match_candidates(query, search_body, searched)) {
      match_candidate_preview_t item {std::move(candidate), std::nullopt, 0};
      try {
        const auto game_id = provider_game_number(item.candidate.provider_game_id);
        const auto plans = game_id ? providers::plan_steamgriddb_assets(*game_id) : std::vector<providers::request_t> {};
        const auto poster = std::find_if(plans.begin(), plans.end(), [](const auto &plan) {
          return plan.kind == kind_e::poster;
        });
        if (poster != plans.end()) {
          const auto list_response = transport(*poster, maximum_listing_bytes);
          if (list_response && successful_status(list_response->status_code)) {
            const std::string list_body(list_response->body.begin(), list_response->body.end());
            const auto images = providers::parse_steamgriddb_assets(kind_e::poster, list_body);
            if (!images.empty()) {
              const providers::request_t download {
                provider_e::steamgriddb,
                providers::operation_e::download,
                kind_e::poster,
                images.front().url,
                false,
              };
              const auto image = transport(download, maximum_preview_bytes);
              const auto effective = image && !image->final_url.empty() ? image->final_url : download.url;
              if (image && successful_status(image->status_code) && is_allowed_provider_url(download.provider, effective)) {
                if (const auto preview = cache.publish(uuid, kind_e::poster, image->body, now_milliseconds)) {
                  item.poster_token = preview->token;
                  item.preview_expires_at = preview->expires_at;
                }
              }
            }
          }
        }
      } catch (...) {
        // A preview failure never removes an otherwise valid sanitized candidate from Nova's list.
      }
      if (posters_only && !item.poster_token) continue;
      result.candidates.push_back(std::move(item));
      if (result.candidates.size() == maximum_candidate_count) break;
    }
    return result;
  }

  cover_pick_t cover_image_for_pick(const preview_t &pick, const providers::transport_t &transport) {
    cover_pick_t result;
    if (!pick.choice) {
      result.image = cover_image_t {pick.mime_type, pick.body};
      return result;
    }
    if (!is_allowed_provider_url(provider_e::steamgriddb, pick.choice->asset_url)) {
      result.failure = choice_expired_failure();
      return result;
    }
    const providers::request_t download {
      provider_e::steamgriddb,
      providers::operation_e::download,
      pick.kind,
      pick.choice->asset_url,
      false,
    };
    try {
      const auto image = transport(download, maximum_asset_bytes);
      if (!image ||
          !is_allowed_provider_url(download.provider, image->final_url.empty() ? download.url : image->final_url)) {
        result.failure = classify_search_failure(true, std::nullopt);
        return result;
      }
      if (!successful_status(image->status_code)) {
        result.failure = classify_search_failure(true, static_cast<long>(image->status_code));
        return result;
      }
      const auto mime_type = mime_from_signature(image->body);
      if (!mime_type || image->body.size() > maximum_asset_bytes) {
        result.failure = classify_search_failure(true, std::nullopt);
        return result;
      }
      result.image = cover_image_t {*mime_type, image->body};
    } catch (...) {
      result.failure = classify_search_failure(true, std::nullopt);
    }
    return result;
  }

  nlohmann::json artwork_choice_json(const std::string_view uuid, const choice_t &choice) {
    nlohmann::json body {
      {"selection_token", choice.token},
      {"preview", "/polaris/v1/games/" + std::string(uuid) + "/artwork/candidate/" + choice.token + "/" + std::string(kind_name(choice.kind))},
      {"expires_at", choice.expires_at},
    };
    return body;
  }

  nlohmann::json artwork_choices_json(const std::string_view uuid, const kind_e kind, const std::vector<choice_t> &choices) {
    auto values = nlohmann::json::array();
    for (const auto &choice : choices) values.push_back(artwork_choice_json(uuid, choice));
    nlohmann::json body {
      {"status", true},
      {"kind", std::string(kind_name(kind))},
      {"choices", std::move(values)},
    };
    return body;
  }

  selected_download_plan_t plan_selected_downloads(
    preview_cache_t &cache,
    const std::string_view uuid,
    const match_selection_t &selection,
    const std::int64_t now_milliseconds
  ) {
    selected_download_plan_t plan;
    if (selection.selections.empty()) {
      plan.refusal = invalid_match_failure();
      return plan;
    }
    for (const auto &pick : selection.selections) {
      const auto source = cache.lookup_choice(uuid, pick.token, pick.kind, now_milliseconds);
      if (!source || !is_allowed_provider_url(provider_e::steamgriddb, source->asset_url)) {
        plan.downloads.clear();
        plan.refusal = choice_expired_failure();
        return plan;
      }
      if (source->provider_game_id != selection.provider_game_id) {
        plan.downloads.clear();
        plan.refusal = choice_mismatch_failure();
        return plan;
      }
      plan.downloads.push_back({
        provider_e::steamgriddb,
        providers::operation_e::download,
        pick.kind,
        source->asset_url,
        false,
      });
    }
    return plan;
  }

  bool stage_unselected_override_assets(
    const std::filesystem::path &appdata,
    const std::filesystem::path &staging_appdata,
    const std::string_view uuid,
    const std::vector<kind_e> &selected_kinds
  ) {
    namespace fs = std::filesystem;
    if (!is_valid_uuid(uuid) || !recover_interrupted_artwork_override(appdata, uuid)) return false;
    // Commits wait while this copies, so no live file changes halfway through.
    const auto lock = acquire_artwork_override_read_lock();
    for (const auto kind : std::array {kind_e::poster, kind_e::hero, kind_e::logo, kind_e::icon}) {
      if (std::find(selected_kinds.begin(), selected_kinds.end(), kind) != selected_kinds.end()) continue;
      for (const auto extension : std::array<std::string_view, 4> {".png", ".jpg", ".jpeg", ".webp"}) {
        const auto live = cache_asset_path(appdata, uuid, kind, source_e::override, extension);
        const auto staged = cache_asset_path(staging_appdata, uuid, kind, source_e::override, extension);
        if (!live || !staged) return false;
        std::error_code error;
        const auto status = fs::symlink_status(*live, error);
        if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) continue;
        // The commit refuses these shapes as well. Refusing here fails the apply
        // instead of quietly dropping the player's image.
        if (error || fs::is_symlink(status) || !fs::is_regular_file(status) || !image_mime_type(*live)) return false;
        const auto size = fs::file_size(*live, error);
        if (error || size == 0 || size > maximum_asset_bytes) return false;
        fs::create_directories(staged->parent_path(), error);
        if (error) return false;
        if (!fs::copy_file(*live, *staged, fs::copy_options::none, error) || error) return false;
        break;
      }
    }
    return true;
  }

  apply_stage_e publish_artwork_override(
    const std::filesystem::path &appdata,
    const std::filesystem::path &staging_appdata,
    const std::string_view uuid,
    const match_selection_t &selection,
    const std::vector<providers::request_t> &downloads,
    const providers::transport_t &transport,
    const std::int64_t now_milliseconds
  ) {
    std::set<kind_e> published_kinds;
    const providers::execution_options_t options {
      .destination_source = source_e::override,
      .force_replace = true,
      .on_published = [&](const asset_t &asset) {
        published_kinds.insert(asset.kind);
      },
    };
    static_cast<void>(providers::execute_download_plan(staging_appdata, uuid, downloads, transport, options));

    const bool picked = !selection.selections.empty();
    const bool every_pick_landed = std::all_of(selection.selections.begin(), selection.selections.end(), [&](const auto &pick) {
      return published_kinds.contains(pick.kind);
    });
    // A match by kinds keeps whatever SteamGridDB could supply. A pick names the exact
    // image the player chose, so a missing one fails the apply rather than half of it.
    if (published_kinds.empty() || (picked && !every_pick_landed)) return apply_stage_e::asset_download;
    if (picked && !stage_unselected_override_assets(appdata, staging_appdata, uuid, selection.kinds)) {
      return apply_stage_e::staging;
    }

    const artwork_override_t metadata {
      std::string(uuid),
      selection.provider,
      selection.provider_game_id,
      selection.title,
      selection.steam_appid,
      true,
      now_milliseconds,
    };
    if (!commit_staged_artwork_override(appdata, staging_appdata, metadata)) return apply_stage_e::commit;
    return apply_stage_e::published;
  }

}  // namespace game_artwork::manual
