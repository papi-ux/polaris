#include "game_artwork.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace game_artwork {
  namespace {
    namespace fs = std::filesystem;

    constexpr std::array<std::string_view, 4> image_extensions {".png", ".jpg", ".jpeg", ".webp"};

    bool is_hex(char value) {
      const auto c = static_cast<unsigned char>(value);
      return std::isxdigit(c) != 0;
    }

    std::string lowercase(std::string_view value) {
      std::string result(value);
      std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return result;
    }

    bool starts_with(std::string_view value, std::string_view prefix) {
      return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    std::optional<source_e> parse_source(std::string_view value) {
      if (value == "override") return source_e::override;
      if (value == "local") return source_e::local;
      if (value == "steam") return source_e::steam;
      if (value == "steamgriddb") return source_e::steamgriddb;
      if (value == "host") return source_e::host;
      return std::nullopt;
    }

    int source_priority(source_e source) {
      switch (source) {
        case source_e::override:
          return 0;
        case source_e::local:
          return 1;
        case source_e::host:
          return 2;
        case source_e::steam:
          return 3;
        case source_e::steamgriddb:
          return 4;
      }
      return 5;
    }

    int kind_priority(kind_e kind) {
      switch (kind) {
        case kind_e::poster:
          return 0;
        case kind_e::hero:
          return 1;
        case kind_e::logo:
          return 2;
        case kind_e::icon:
          return 3;
      }
      return 4;
    }

    std::string extension_for_mime(std::string_view mime) {
      if (mime == "image/png") return ".png";
      if (mime == "image/jpeg") return ".jpg";
      if (mime == "image/webp") return ".webp";
      return {};
    }

    bool is_safe_extension(std::string_view extension) {
      const auto normalized = lowercase(extension);
      return std::find(image_extensions.begin(), image_extensions.end(), normalized) != image_extensions.end();
    }

    void hash_bytes(std::uint64_t &hash, const unsigned char *data, std::size_t size) {
      constexpr std::uint64_t fnv_prime = 1099511628211ULL;
      for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= fnv_prime;
      }
    }

    void hash_text(std::uint64_t &hash, std::string_view value) {
      hash_bytes(hash, reinterpret_cast<const unsigned char *>(value.data()), value.size());
    }

    std::string revision_for(std::string_view uuid, const std::vector<asset_t> &assets) {
      std::uint64_t hash = 14695981039346656037ULL;
      hash_text(hash, uuid);
      for (const auto &asset : assets) {
        hash_text(hash, kind_name(asset.kind));
        hash_text(hash, source_name(asset.source));
        hash_text(hash, asset.mime_type);
        hash_text(hash, asset.path.filename().string());

        std::error_code error;
        const auto size = fs::file_size(asset.path, error);
        if (error) continue;
        hash_text(hash, std::to_string(size));
        const auto modified = fs::last_write_time(asset.path, error);
        if (!error) hash_text(hash, std::to_string(modified.time_since_epoch().count()));

        // Revision tokens are cache invalidators, not integrity digests. Hashing
        // small edge samples catches same-size rewrites without rereading every
        // multi-megabyte artwork file on each library request.
        std::ifstream input(asset.path, std::ios::binary);
        std::array<unsigned char, 64> buffer {};
        input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto first_count = input.gcount();
        if (first_count > 0) hash_bytes(hash, buffer.data(), static_cast<std::size_t>(first_count));
        if (size > buffer.size()) {
          input.clear();
          input.seekg(static_cast<std::streamoff>(size - buffer.size()), std::ios::beg);
          input.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
          const auto last_count = input.gcount();
          if (last_count > 0) hash_bytes(hash, buffer.data(), static_cast<std::size_t>(last_count));
        }
      }
      std::ostringstream output;
      output << std::hex << std::setw(16) << std::setfill('0') << hash;
      return output.str();
    }

    std::optional<std::string> exact_https_host(std::string_view url) {
      constexpr std::string_view scheme = "https://";
      if (!starts_with(url, scheme)) return std::nullopt;
      auto remainder = url.substr(scheme.size());
      const auto authority_end = remainder.find_first_of("/?#");
      const auto authority = remainder.substr(0, authority_end);
      if (authority.empty() || authority.find('@') != std::string_view::npos ||
          authority.find(':') != std::string_view::npos || authority.front() == '[') {
        return std::nullopt;
      }
      if (authority_end != std::string_view::npos && remainder[authority_end] != '/') return std::nullopt;
      const auto lowered_url = lowercase(url);
      if (lowered_url.find("api_key=") != std::string::npos ||
          lowered_url.find("authorization=") != std::string::npos ||
          lowered_url.find("token=") != std::string::npos) {
        return std::nullopt;
      }
      return lowercase(authority);
    }

    std::optional<asset_t> validated_asset(kind_e kind, source_e source, const fs::path &path) {
      const auto mime = image_mime_type(path);
      if (!mime) return std::nullopt;
      return asset_t {kind, source, path, *mime};
    }
  }  // namespace

  bool is_valid_uuid(std::string_view uuid) {
    if (uuid.size() != 36) return false;
    constexpr std::array<std::size_t, 4> hyphens {8, 13, 18, 23};
    for (std::size_t i = 0; i < uuid.size(); ++i) {
      const bool expects_hyphen = std::find(hyphens.begin(), hyphens.end(), i) != hyphens.end();
      if (expects_hyphen ? uuid[i] != '-' : !is_hex(uuid[i])) return false;
    }
    return true;
  }

  bool is_valid_steam_appid(std::string_view appid) {
    return !appid.empty() && appid.size() <= 20 &&
           std::all_of(appid.begin(), appid.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
  }

  std::optional<kind_e> parse_kind(std::string_view value) {
    if (value == "poster") return kind_e::poster;
    if (value == "hero") return kind_e::hero;
    if (value == "logo") return kind_e::logo;
    if (value == "icon") return kind_e::icon;
    return std::nullopt;
  }

  std::string_view kind_name(kind_e kind) {
    switch (kind) {
      case kind_e::poster:
        return "poster";
      case kind_e::hero:
        return "hero";
      case kind_e::logo:
        return "logo";
      case kind_e::icon:
        return "icon";
    }
    return "unknown";
  }

  std::string_view source_name(source_e source) {
    switch (source) {
      case source_e::override:
        return "override";
      case source_e::local:
        return "local";
      case source_e::steam:
        return "steam";
      case source_e::steamgriddb:
        return "steamgriddb";
      case source_e::host:
        return "host";
    }
    return "host";
  }

  std::optional<asset_request_t> parse_asset_request_target(std::string_view path) {
    constexpr std::string_view prefix = "/polaris/v1/games/";
    constexpr std::string_view marker = "/artwork/";
    if (!starts_with(path, prefix)) return std::nullopt;
    const auto remainder = path.substr(prefix.size());
    const auto marker_at = remainder.find(marker);
    if (marker_at == std::string_view::npos || remainder.find(marker, marker_at + 1) != std::string_view::npos) {
      return std::nullopt;
    }
    const auto uuid = remainder.substr(0, marker_at);
    const auto raw_kind = remainder.substr(marker_at + marker.size());
    const auto kind = parse_kind(raw_kind);
    if (!is_valid_uuid(uuid) || !kind) return std::nullopt;
    return asset_request_t {std::string(uuid), *kind};
  }

  std::optional<std::string> parse_resolve_request_target(std::string_view path) {
    constexpr std::string_view suffix = "/artwork/resolve";
    constexpr std::string_view prefix = "/polaris/v1/games/";
    if (!starts_with(path, prefix) || path.size() <= prefix.size() + suffix.size() ||
        path.substr(path.size() - suffix.size()) != suffix) {
      return std::nullopt;
    }
    const auto uuid = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    if (!is_valid_uuid(uuid)) return std::nullopt;
    return std::string(uuid);
  }

  fs::path cache_root(const fs::path &appdata) {
    return appdata / "artwork" / "v1";
  }

  std::optional<fs::path> cache_asset_path(
    const fs::path &appdata,
    std::string_view uuid,
    kind_e kind,
    source_e source,
    std::string_view extension
  ) {
    if (!is_valid_uuid(uuid) || !is_safe_extension(extension)) return std::nullopt;
    return cache_root(appdata) / std::string(uuid) /
           (std::string(kind_name(kind)) + "." + std::string(source_name(source)) + lowercase(extension));
  }

  std::optional<std::string> image_mime_type(const fs::path &path) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error || fs::is_symlink(path, error) || error) return std::nullopt;
    const auto size = fs::file_size(path, error);
    if (error || size == 0 || size > maximum_asset_bytes) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 12> bytes {};
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const auto count = input.gcount();
    if (count >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G' &&
        bytes[4] == '\r' && bytes[5] == '\n' && bytes[6] == 0x1a && bytes[7] == '\n') {
      return "image/png";
    }
    if (count >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) return "image/jpeg";
    if (count >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
        bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P') {
      return "image/webp";
    }
    return std::nullopt;
  }

  std::optional<local_candidate_t> select_legacy_poster(
    const fs::path &covers_directory,
    std::string_view uuid,
    std::string_view steam_appid,
    const fs::path &configured_image,
    source_e configured_source
  ) {
    if (!is_valid_uuid(uuid)) return std::nullopt;
    if (image_mime_type(configured_image)) return local_candidate_t {configured_image, configured_source};
    const auto find_stem = [&](const std::string &stem, source_e source) -> std::optional<local_candidate_t> {
      for (const auto extension : image_extensions) {
        auto path = covers_directory / (stem + std::string(extension));
        if (image_mime_type(path)) return local_candidate_t {std::move(path), source};
      }
      return std::nullopt;
    };
    if (auto result = find_stem(std::string(uuid), source_e::host)) return result;
    if (is_valid_steam_appid(steam_appid)) {
      return find_stem("steam_" + std::string(steam_appid), source_e::steam);
    }
    return std::nullopt;
  }

  std::optional<asset_t> cache_local_poster(
    const fs::path &appdata,
    std::string_view uuid,
    const local_candidate_t &candidate
  ) {
    const auto mime = image_mime_type(candidate.path);
    if (!mime || !is_valid_uuid(uuid)) return std::nullopt;
    const auto extension = extension_for_mime(*mime);
    const auto destination = cache_asset_path(appdata, uuid, kind_e::poster, candidate.source, extension);
    if (!destination) return std::nullopt;
    std::error_code error;
    fs::create_directories(destination->parent_path(), error);
    if (error) return std::nullopt;
    if (fs::equivalent(candidate.path, *destination, error) && !error) {
      return asset_t {kind_e::poster, candidate.source, *destination, *mime};
    }
    error.clear();
    const auto temporary = fs::path(destination->string() + ".tmp");
    fs::remove(temporary, error);
    error.clear();
    fs::copy_file(candidate.path, temporary, fs::copy_options::overwrite_existing, error);
    if (error || !image_mime_type(temporary)) {
      fs::remove(temporary, error);
      return std::nullopt;
    }
    fs::rename(temporary, *destination, error);
    if (error) {
      std::error_code cleanup;
      fs::remove(temporary, cleanup);
      return std::nullopt;
    }
    return asset_t {kind_e::poster, candidate.source, *destination, *mime};
  }

  std::vector<asset_t> scan_cached_assets(const fs::path &appdata, std::string_view uuid) {
    std::vector<asset_t> assets;
    if (!is_valid_uuid(uuid)) return assets;
    const auto directory = cache_root(appdata) / std::string(uuid);
    std::error_code error;
    if (!fs::is_directory(directory, error) || error) return assets;
    for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
      const auto &entry = *it;
      if (entry.is_symlink(error) || error || !entry.is_regular_file(error) || error) {
        error.clear();
        continue;
      }
      const auto filename = entry.path().filename().string();
      const auto first_dot = filename.find('.');
      const auto second_dot = first_dot == std::string::npos ? std::string::npos : filename.find('.', first_dot + 1);
      if (first_dot == std::string::npos || second_dot == std::string::npos) continue;
      const auto kind = parse_kind(filename.substr(0, first_dot));
      const auto source = parse_source(filename.substr(first_dot + 1, second_dot - first_dot - 1));
      if (!kind || !source || !is_safe_extension(filename.substr(second_dot))) continue;
      if (auto asset = validated_asset(*kind, *source, entry.path())) assets.push_back(std::move(*asset));
    }
    std::sort(assets.begin(), assets.end(), [](const asset_t &left, const asset_t &right) {
      const auto left_key = std::pair {kind_priority(left.kind), source_priority(left.source)};
      const auto right_key = std::pair {kind_priority(right.kind), source_priority(right.source)};
      if (left_key != right_key) return left_key < right_key;
      return left.path.filename().string() < right.path.filename().string();
    });
    return assets;
  }

  std::optional<asset_t> find_cached_asset(const fs::path &appdata, std::string_view uuid, kind_e kind) {
    const auto assets = scan_cached_assets(appdata, uuid);
    const auto match = std::find_if(assets.begin(), assets.end(), [kind](const asset_t &asset) {
      return asset.kind == kind;
    });
    return match == assets.end() ? std::nullopt : std::optional<asset_t>(*match);
  }

  bool needs_source_upgrade(
    const fs::path &appdata,
    std::string_view uuid,
    kind_e kind,
    source_e candidate_source
  ) {
    if (!is_valid_uuid(uuid)) return false;
    const auto current = find_cached_asset(appdata, uuid, kind);
    return !current || source_priority(candidate_source) < source_priority(current->source);
  }

  nlohmann::json make_manifest(std::string_view uuid, const std::vector<asset_t> &input_assets) {
    std::vector<asset_t> assets;
    std::set<kind_e> seen;
    auto sorted = input_assets;
    std::sort(sorted.begin(), sorted.end(), [](const asset_t &left, const asset_t &right) {
      return std::pair {kind_priority(left.kind), source_priority(left.source)} <
             std::pair {kind_priority(right.kind), source_priority(right.source)};
    });
    for (const auto &asset : sorted) {
      if (!is_valid_uuid(uuid) || seen.count(asset.kind) != 0 || !image_mime_type(asset.path)) continue;
      seen.insert(asset.kind);
      assets.push_back(asset);
    }
    std::int64_t cached_at = 0;
    for (const auto &asset : assets) {
      std::error_code error;
      const auto file_time = fs::last_write_time(asset.path, error);
      if (error) continue;
      const auto system_time = decltype(file_time)::clock::to_sys(file_time);
      const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(system_time.time_since_epoch()).count();
      cached_at = std::max<std::int64_t>(cached_at, milliseconds);
    }
    const auto state = assets.empty() ? "fallback" : (assets.size() == 4 ? "cached" : "partial");
    nlohmann::json manifest {
      {"version", manifest_version},
      {"revision", revision_for(uuid, assets)},
      {"state", state},
      {"cached_at", cached_at},
      {"override", {{"active", false}, {"kinds", nlohmann::json::array()}}},
      {"assets", nlohmann::json::object()},
    };
    for (const auto &asset : assets) {
      manifest["assets"][kind_name(asset.kind)] = {
        {"url", "/polaris/v1/games/" + std::string(uuid) + "/artwork/" + std::string(kind_name(asset.kind))},
        {"source", source_name(asset.source)},
        {"mime_type", asset.mime_type},
        {"cached", true},
      };
    }
    return manifest;
  }

  nlohmann::json current_manifest(const fs::path &appdata, std::string_view uuid) {
    return make_manifest(uuid, scan_cached_assets(appdata, uuid));
  }

  bool is_allowed_provider_url(provider_e provider, std::string_view url) {
    const auto host = exact_https_host(url);
    if (!host) return false;
    static const std::set<std::string> steam_hosts {
      "cdn.akamai.steamstatic.com",
      "cdn.cloudflare.steamstatic.com",
      "shared.akamai.steamstatic.com",
      "steamcdn-a.akamaihd.net",
      "store.steampowered.com",
    };
    static const std::set<std::string> steamgriddb_hosts {
      "cdn.steamgriddb.com",
      "cdn2.steamgriddb.com",
      "www.steamgriddb.com",
    };
    static const std::set<std::string> epic_hosts {
      "cdn1.epicgames.com",
    };
    switch (provider) {
      case provider_e::steam:
        return steam_hosts.count(*host) != 0;
      case provider_e::steamgriddb:
        return steamgriddb_hosts.count(*host) != 0;
      case provider_e::epic:
        return epic_hosts.count(*host) != 0;
    }
    return false;
  }

  std::vector<asset_t> resolve_missing_assets(const resolve_request_t &request) {
    auto assets = scan_cached_assets(request.appdata, request.uuid);
    for (const auto &candidate : request.local_posters) {
      if (!needs_source_upgrade(request.appdata, request.uuid, kind_e::poster, candidate.source)) continue;
      if (auto cached = cache_local_poster(request.appdata, request.uuid, candidate)) {
        assets.push_back(std::move(*cached));
        break;
      }
    }
    return scan_cached_assets(request.appdata, request.uuid);
  }
}  // namespace game_artwork
