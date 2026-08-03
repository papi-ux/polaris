#include "game_artwork_override.h"

#include "game_artwork.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace game_artwork {
  namespace {
    namespace fs = std::filesystem;

    constexpr std::string_view metadata_filename = "override.json";
    constexpr std::string_view temporary_suffix = ".tmp";
    constexpr std::string_view rollback_suffix = ".override-rollback";
    constexpr std::string_view transaction_marker_filename = "override.transaction";

    std::shared_mutex &override_gate() {
      static std::shared_mutex gate;
      return gate;
    }

    fs::path metadata_path(const fs::path &appdata, std::string_view uuid) {
      return cache_root(appdata) / std::string(uuid) / metadata_filename;
    }

    fs::path temporary_path(const fs::path &path) {
      return fs::path(path.string() + std::string(temporary_suffix));
    }

    bool is_bounded_decimal(std::string_view value, std::size_t maximum_length) {
      return !value.empty() && value.size() <= maximum_length &&
             std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return c >= '0' && c <= '9';
             });
    }

    bool is_valid_title(std::string_view title) {
      if (title.empty() || title.size() > maximum_override_title_bytes) return false;
      for (std::size_t index = 0; index < title.size(); ++index) {
        const auto c = static_cast<unsigned char>(title[index]);
        if (c <= 0x1f || c == 0x7f) return false;
        // UTF-8 encoding of the remaining C1 control range U+0080..U+009F.
        if (c == 0xc2 && index + 1 < title.size()) {
          const auto next = static_cast<unsigned char>(title[index + 1]);
          if (next >= 0x80 && next <= 0x9f) return false;
        }
      }
      try {
        // dump() uses strict UTF-8 handling by default.
        static_cast<void>(nlohmann::json(std::string(title)).dump());
      } catch (const nlohmann::json::exception &) {
        return false;
      }
      return true;
    }

    nlohmann::json serialized_metadata(const artwork_override_t &metadata) {
      nlohmann::json value {
        {"version", artwork_override_version},
        {"uuid", metadata.uuid},
        {"provider", metadata.provider},
        {"provider_game_id", metadata.provider_game_id},
        {"title", metadata.title},
        {"manual", metadata.manual},
        {"updated_at", metadata.updated_at},
      };
      if (metadata.steam_appid) value["steam_appid"] = *metadata.steam_appid;
      return value;
    }

    nlohmann::json sanitized_match(const artwork_override_t &metadata) {
      nlohmann::json match {
        {"provider", metadata.provider},
        {"provider_game_id", metadata.provider_game_id},
        {"title", metadata.title},
      };
      if (metadata.steam_appid) match["steam_appid"] = *metadata.steam_appid;
      return match;
    }

    bool has_exact_metadata_keys(const nlohmann::json &value) {
      if (!value.is_object()) return false;
      static constexpr std::array<std::string_view, 7> required {
        "version",
        "uuid",
        "provider",
        "provider_game_id",
        "title",
        "manual",
        "updated_at",
      };
      const auto expected_size = required.size() + (value.contains("steam_appid") ? 1 : 0);
      if (value.size() != expected_size) return false;
      return std::all_of(required.begin(), required.end(), [&](std::string_view key) {
        return value.contains(key);
      });
    }

    std::optional<std::int64_t> nonnegative_timestamp(const nlohmann::json &value) {
      if (value.is_number_unsigned()) {
        const auto timestamp = value.get<std::uint64_t>();
        if (timestamp > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          return std::nullopt;
        }
        return static_cast<std::int64_t>(timestamp);
      }
      if (!value.is_number_integer()) return std::nullopt;
      const auto timestamp = value.get<std::int64_t>();
      return timestamp < 0 ? std::nullopt : std::optional<std::int64_t>(timestamp);
    }

    std::optional<artwork_override_t> parse_metadata(
      const nlohmann::json &value,
      std::string_view expected_uuid
    ) {
      if (!has_exact_metadata_keys(value) ||
          !value["version"].is_number_integer() || value["version"] != artwork_override_version ||
          !value["uuid"].is_string() || !value["provider"].is_string() ||
          !value["provider_game_id"].is_string() || !value["title"].is_string() ||
          !value["manual"].is_boolean() || value["manual"] != true) {
        return std::nullopt;
      }
      if (value.contains("steam_appid") && !value["steam_appid"].is_string()) return std::nullopt;
      const auto timestamp = nonnegative_timestamp(value["updated_at"]);
      if (!timestamp) return std::nullopt;

      artwork_override_t metadata {
        .uuid = value["uuid"].get<std::string>(),
        .provider = value["provider"].get<std::string>(),
        .provider_game_id = value["provider_game_id"].get<std::string>(),
        .title = value["title"].get<std::string>(),
        .steam_appid = value.contains("steam_appid") ?
                         std::optional<std::string>(value["steam_appid"].get<std::string>()) :
                         std::nullopt,
        .manual = true,
        .updated_at = *timestamp,
      };
      if (metadata.uuid != expected_uuid || !is_valid_artwork_override(metadata)) return std::nullopt;
      return metadata;
    }

    std::optional<std::string> read_bounded_file(const fs::path &path) {
      std::error_code error;
      const auto status = fs::symlink_status(path, error);
      if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) return std::nullopt;

      std::ifstream input(path, std::ios::binary);
      if (!input.good()) return std::nullopt;
      std::string contents(maximum_override_file_bytes + 1, '\0');
      input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
      const auto count = input.gcount();
      if (input.bad() || count < 0 || static_cast<std::size_t>(count) > maximum_override_file_bytes) {
        return std::nullopt;
      }
      contents.resize(static_cast<std::size_t>(count));
      return contents;
    }

    enum class directory_tree_state_e { missing, safe, unsafe };

    directory_tree_state_e safe_directory_tree_state(
      const fs::path &appdata,
      const std::optional<std::string_view> uuid = std::nullopt
    ) {
      std::vector<fs::path> directories {appdata, appdata / "artwork", cache_root(appdata)};
      if (uuid) directories.push_back(cache_root(appdata) / std::string(*uuid));
      for (const auto &directory : directories) {
        std::error_code error;
        const auto status = fs::symlink_status(directory, error);
        if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) {
          return directory_tree_state_e::missing;
        }
        if (error || fs::is_symlink(status) || !fs::is_directory(status)) return directory_tree_state_e::unsafe;
      }
      return directory_tree_state_e::safe;
    }

    bool ensure_safe_directory_component(const fs::path &directory) {
      std::error_code error;
      auto status = fs::symlink_status(directory, error);
      if (!error && fs::exists(status)) return !fs::is_symlink(status) && fs::is_directory(status);
      if (error && error != std::errc::no_such_file_or_directory) return false;
      error.clear();
      if (!fs::create_directory(directory, error) || error) return false;
      error.clear();
      status = fs::symlink_status(directory, error);
      return !error && !fs::is_symlink(status) && fs::is_directory(status);
    }

    bool ensure_live_artwork_game_directory(const fs::path &appdata, const std::string_view uuid) {
      if (!is_valid_uuid(uuid)) return false;
      for (const auto &directory : std::array {
             appdata, appdata / "artwork", cache_root(appdata), cache_root(appdata) / std::string(uuid)}) {
        if (!ensure_safe_directory_component(directory)) return false;
      }
      return safe_directory_tree_state(appdata, uuid) == directory_tree_state_e::safe;
    }

    bool valid_staging_token(const std::string_view token) {
      return token.size() == 32 && std::all_of(token.begin(), token.end(), [](const unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      });
    }

    bool path_is_replaceable(const fs::path &path) {
      std::error_code error;
      const auto status = fs::symlink_status(path, error);
      if (!error && fs::exists(status)) return !fs::is_symlink(status) && fs::is_regular_file(status);
      return !error || error == std::errc::no_such_file_or_directory;
    }

    struct staged_replacement_t {
      std::optional<fs::path> staged;
      std::optional<fs::path> target;
      std::vector<std::pair<fs::path, fs::path>> prior_files;
    };

    std::optional<std::vector<staged_replacement_t>> staged_replacements(
      const fs::path &appdata,
      const fs::path &staging_appdata,
      std::string_view uuid
    ) {
      std::vector<staged_replacement_t> result;
      bool has_staged_asset = false;
      constexpr std::array extensions {".png", ".jpg", ".jpeg", ".webp"};
      for (const auto kind : std::array {kind_e::poster, kind_e::hero, kind_e::logo, kind_e::icon}) {
        std::optional<fs::path> staged;
        for (const auto extension : extensions) {
          const auto candidate = cache_asset_path(staging_appdata, uuid, kind, source_e::override, extension);
          if (!candidate) return std::nullopt;
          std::error_code error;
          const auto status = fs::symlink_status(*candidate, error);
          if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) continue;
          if (error || fs::is_symlink(status) || !fs::is_regular_file(status) || !image_mime_type(*candidate)) {
            return std::nullopt;
          }
          if (staged) return std::nullopt;
          staged = *candidate;
        }
        staged_replacement_t replacement;
        if (staged) {
          const auto target = cache_asset_path(appdata, uuid, kind, source_e::override, staged->extension().string());
          if (!target) return std::nullopt;
          replacement.staged = *staged;
          replacement.target = *target;
          has_staged_asset = true;
        }
        for (const auto extension : extensions) {
          const auto prior = cache_asset_path(appdata, uuid, kind, source_e::override, extension);
          if (!prior) return std::nullopt;
          std::error_code error;
          const auto status = fs::symlink_status(*prior, error);
          if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) continue;
          if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) return std::nullopt;
          replacement.prior_files.emplace_back(*prior, fs::path(prior->string() + std::string(rollback_suffix)));
        }
        if (replacement.staged || !replacement.prior_files.empty()) result.push_back(std::move(replacement));
      }
      return has_staged_asset ? std::optional(std::move(result)) : std::nullopt;
    }

    bool remove_metadata_path(const fs::path &path) {
      std::error_code error;
      const auto status = fs::symlink_status(path, error);
      if ((!error && !fs::exists(status)) || error == std::errc::no_such_file_or_directory) return true;
      if (error || fs::is_directory(status)) return false;
      static_cast<void>(fs::remove(path, error));
      return !error;
    }

    std::vector<fs::path> override_asset_paths(const fs::path &appdata, const std::string_view uuid) {
      std::vector<fs::path> paths;
      for (const auto kind : std::array {kind_e::poster, kind_e::hero, kind_e::logo, kind_e::icon}) {
        for (const auto extension : std::array<std::string_view, 4> {".png", ".jpg", ".jpeg", ".webp"}) {
          if (const auto path = cache_asset_path(appdata, uuid, kind, source_e::override, extension)) paths.push_back(*path);
        }
      }
      return paths;
    }

    bool write_transaction_marker(const fs::path &appdata, const std::string_view uuid) {
      if (safe_directory_tree_state(appdata, uuid) != directory_tree_state_e::safe) return false;
      const auto marker = metadata_path(appdata, uuid).parent_path() / transaction_marker_filename;
      std::error_code error;
      const auto status = fs::symlink_status(marker, error);
      if ((!error && fs::exists(status)) || (error && error != std::errc::no_such_file_or_directory)) return false;
      const auto temporary = temporary_path(marker);
      if (!path_is_replaceable(temporary)) return false;
      error.clear();
      static_cast<void>(fs::remove(temporary, error));
      if (error) return false;
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output.good()) return false;
      output << "1\n";
      output.flush();
      if (!output.good()) return false;
      output.close();
      fs::rename(temporary, marker, error);
      return !error;
    }

    std::optional<std::vector<std::pair<fs::path, fs::path>>> existing_rollback_files(
      const fs::path &appdata,
      const std::string_view uuid
    ) {
      auto originals = override_asset_paths(appdata, uuid);
      originals.push_back(metadata_path(appdata, uuid));
      std::vector<std::pair<fs::path, fs::path>> result;
      for (const auto &original : originals) {
        const auto backup = fs::path(original.string() + std::string(rollback_suffix));
        std::error_code error;
        const auto status = fs::symlink_status(backup, error);
        if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) continue;
        if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) return std::nullopt;
        result.emplace_back(original, backup);
      }
      return result;
    }

    bool restore_rollback_files(const std::vector<std::pair<fs::path, fs::path>> &backups) {
      for (const auto &[original, backup] : backups) {
        std::error_code error;
        const auto status = fs::symlink_status(original, error);
        if (!error && fs::exists(status)) return false;
        if (error && error != std::errc::no_such_file_or_directory) return false;
        error.clear();
        fs::rename(backup, original, error);
        if (error) return false;
      }
      return true;
    }

    std::optional<bool> valid_transaction_marker_exists(const fs::path &marker) {
      std::error_code error;
      const auto status = fs::symlink_status(marker, error);
      if (error == std::errc::no_such_file_or_directory || (!error && !fs::exists(status))) return false;
      if (error || fs::is_symlink(status) || !fs::is_regular_file(status)) return std::nullopt;
      const auto contents = read_bounded_file(marker);
      return contents && *contents == "1\n" ? std::optional<bool>(true) : std::nullopt;
    }

    bool remove_live_override_generation(const fs::path &appdata, const std::string_view uuid) {
      bool success = true;
      for (const auto &path : override_asset_paths(appdata, uuid)) {
        if (!remove_metadata_path(path)) success = false;
      }
      const auto metadata = metadata_path(appdata, uuid);
      if (!remove_metadata_path(metadata)) success = false;
      if (!remove_metadata_path(temporary_path(metadata))) success = false;
      return success;
    }

    bool remove_rollback_files(const std::vector<std::pair<fs::path, fs::path>> &backups) {
      bool success = true;
      for (const auto &[original, backup] : backups) {
        static_cast<void>(original);
        if (!remove_metadata_path(backup)) success = false;
      }
      return success;
    }

    std::string revised_manifest_revision(
      std::string_view base_revision,
      const artwork_override_t &metadata
    ) {
      const auto canonical = serialized_metadata(metadata).dump();
      std::uint64_t hash = 1469598103934665603ULL;
      const auto update = [&](std::string_view input) {
        for (const auto c : input) {
          hash ^= static_cast<unsigned char>(c);
          hash *= 1099511628211ULL;
        }
      };
      update("polaris-artwork-override-v1\n");
      update(base_revision);
      update("\n");
      update(canonical);

      std::ostringstream output;
      output << std::hex << std::setfill('0') << std::setw(16) << hash;
      return output.str();
    }
  }  // namespace

  bool is_valid_artwork_override(const artwork_override_t &metadata) {
    return is_valid_uuid(metadata.uuid) &&
           (metadata.provider == "steam" || metadata.provider == "steamgriddb") &&
           is_bounded_decimal(metadata.provider_game_id, maximum_provider_game_id_length) &&
           is_valid_title(metadata.title) &&
           (!metadata.steam_appid || is_valid_steam_appid(*metadata.steam_appid)) &&
           metadata.manual && metadata.updated_at >= 0;
  }

  std::optional<artwork_override_t> load_artwork_override(
    const fs::path &appdata,
    std::string_view uuid
  ) {
    if (!is_valid_uuid(uuid) || safe_directory_tree_state(appdata, uuid) != directory_tree_state_e::safe) {
      return std::nullopt;
    }
    const auto contents = read_bounded_file(metadata_path(appdata, uuid));
    if (!contents) return std::nullopt;
    const auto value = nlohmann::json::parse(*contents, nullptr, false);
    if (value.is_discarded()) return std::nullopt;
    try {
      return parse_metadata(value, uuid);
    } catch (const nlohmann::json::exception &) {
      return std::nullopt;
    }
  }

  bool save_artwork_override(
    const fs::path &appdata,
    const artwork_override_t &metadata
  ) {
    if (!is_valid_artwork_override(metadata)) return false;
    const auto path = metadata_path(appdata, metadata.uuid);
    const auto temporary = temporary_path(path);
    if (!ensure_live_artwork_game_directory(appdata, metadata.uuid) || !path_is_replaceable(path)) return false;

    std::error_code error;
    static_cast<void>(fs::remove(temporary, error));
    if (error) return false;

    try {
      std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
      if (!output.good()) return false;
      output << serialized_metadata(metadata).dump(2) << '\n';
      output.flush();
      if (!output.good()) {
        output.close();
        fs::remove(temporary, error);
        return false;
      }
      output.close();
      if (output.fail()) {
        fs::remove(temporary, error);
        return false;
      }
    } catch (const nlohmann::json::exception &) {
      fs::remove(temporary, error);
      return false;
    }

    fs::rename(temporary, path, error);
    if (!error) return true;
    error.clear();
    fs::remove(temporary, error);
    return false;
  }

  std::optional<fs::path> create_artwork_staging_directory(
    const fs::path &appdata,
    const std::string_view token
  ) {
    if (appdata.empty() || !valid_staging_token(token)) return std::nullopt;
    const auto root = cache_root(appdata);
    const auto transactions = root / ".transactions";
    for (const auto &directory : std::array {appdata, appdata / "artwork", root, transactions}) {
      if (!ensure_safe_directory_component(directory)) return std::nullopt;
    }
    const auto candidate = transactions / std::string(token);
    std::error_code error;
    if (!fs::create_directory(candidate, error) || error) return std::nullopt;
    const auto status = fs::symlink_status(candidate, error);
    if (error || fs::is_symlink(status) || !fs::is_directory(status)) {
      error.clear();
      static_cast<void>(fs::remove(candidate, error));
      return std::nullopt;
    }
    return candidate;
  }

  namespace {
    bool recover_interrupted_artwork_override_unlocked(
      const fs::path &appdata,
      const std::string_view uuid
    ) {
      if (!is_valid_uuid(uuid)) return false;
      const auto tree_state = safe_directory_tree_state(appdata, uuid);
      if (tree_state == directory_tree_state_e::missing) return true;
      if (tree_state == directory_tree_state_e::unsafe) return false;
      const auto marker = metadata_path(appdata, uuid).parent_path() / transaction_marker_filename;
      const auto marker_exists = valid_transaction_marker_exists(marker);
      const auto backups = existing_rollback_files(appdata, uuid);
      if (!marker_exists || !backups) return false;
      if (!*marker_exists) {
        if (!restore_rollback_files(*backups)) return false;
        return remove_metadata_path(temporary_path(marker));
      }

      if (load_artwork_override(appdata, uuid)) {
        if (!remove_rollback_files(*backups)) return false;
      } else {
        if (!remove_live_override_generation(appdata, uuid)) return false;
        if (!restore_rollback_files(*backups)) return false;
      }
      if (!remove_metadata_path(temporary_path(marker))) return false;
      return remove_metadata_path(marker);
    }
  }

  bool recover_interrupted_artwork_override(
    const fs::path &appdata,
    const std::string_view uuid
  ) {
    std::unique_lock transaction_lock(override_gate());
    return recover_interrupted_artwork_override_unlocked(appdata, uuid);
  }

  std::shared_lock<std::shared_mutex> acquire_artwork_override_read_lock() {
    return std::shared_lock(override_gate());
  }

  bool commit_staged_artwork_override(
    const fs::path &appdata,
    const fs::path &staging_appdata,
    const artwork_override_t &metadata,
    const staged_override_commit_options_t &options
  ) {
    if (!is_valid_artwork_override(metadata)) return false;
    std::unique_lock transaction_lock(override_gate());
    if (!ensure_live_artwork_game_directory(appdata, metadata.uuid)) return false;
    if (!recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid)) return false;
    auto replacements = staged_replacements(appdata, staging_appdata, metadata.uuid);
    if (!replacements) return false;
    const auto live_metadata = metadata_path(appdata, metadata.uuid);
    const auto metadata_backup = fs::path(live_metadata.string() + std::string(rollback_suffix));
    std::error_code error;

    try {
      for (const auto &replacement : *replacements) {
        for (const auto &[prior, backup] : replacement.prior_files) {
          error.clear();
          if (fs::exists(backup, error) || error) {
            static_cast<void>(recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid));
            return false;
          }
          fs::rename(prior, backup, error);
          if (error) {
            static_cast<void>(recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid));
            return false;
          }
        }
      }

      error.clear();
      const auto metadata_status = fs::symlink_status(live_metadata, error);
      if (!error && fs::exists(metadata_status)) {
        if (fs::is_symlink(metadata_status) || !fs::is_regular_file(metadata_status)) {
          static_cast<void>(recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid));
          return false;
        }
        fs::rename(live_metadata, metadata_backup, error);
        if (error) {
          static_cast<void>(recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid));
          return false;
        }
      } else if (error != std::errc::no_such_file_or_directory) {
        static_cast<void>(recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid));
        return false;
      }
      if (!write_transaction_marker(appdata, metadata.uuid)) {
        static_cast<void>(recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid));
        return false;
      }

      bool notified = false;
      for (auto &replacement : *replacements) {
        if (!replacement.staged || !replacement.target) continue;
        error.clear();
        fs::rename(*replacement.staged, *replacement.target, error);
        if (error) {
          static_cast<void>(recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid));
          return false;
        }
        if (!notified && options.after_first_asset_published) {
          notified = true;
          options.after_first_asset_published();
        }
      }
      const auto save = options.save_metadata ? options.save_metadata : save_artwork_override;
      if (!save(appdata, metadata)) {
        static_cast<void>(recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid));
        return false;
      }
    } catch (...) {
      static_cast<void>(recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid));
      return false;
    }

    return recover_interrupted_artwork_override_unlocked(appdata, metadata.uuid);
  }

  bool clear_artwork_override(
    const fs::path &appdata,
    std::string_view uuid
  ) {
    if (!is_valid_uuid(uuid)) return false;
    std::unique_lock transaction_lock(override_gate());
    if (!recover_interrupted_artwork_override_unlocked(appdata, uuid)) return false;
    const auto path = metadata_path(appdata, uuid);
    const auto temporary = temporary_path(path);
    bool success = remove_metadata_path(path);
    if (!remove_metadata_path(temporary)) success = false;
    for (const auto kind : std::array {kind_e::poster, kind_e::hero, kind_e::logo, kind_e::icon}) {
      for (const auto extension : std::array<std::string_view, 4> {".png", ".jpg", ".jpeg", ".webp"}) {
        const auto candidate = cache_asset_path(appdata, uuid, kind, source_e::override, extension);
        if (candidate && !remove_metadata_path(*candidate)) success = false;
      }
    }
    return success;
  }

  nlohmann::json decorate_manifest_with_artwork_override(
    nlohmann::json manifest,
    const artwork_override_t &metadata
  ) {
    if (!is_valid_artwork_override(metadata) || !manifest.is_object() ||
        !manifest.contains("revision") || !manifest["revision"].is_string()) {
      return manifest;
    }

    const auto match = sanitized_match(metadata);
    nlohmann::json kinds = nlohmann::json::array();
    if (manifest.contains("assets") && manifest["assets"].is_object()) {
      for (const auto kind : std::array {kind_e::poster, kind_e::hero, kind_e::logo, kind_e::icon}) {
        const auto name = std::string(kind_name(kind));
        if (manifest["assets"].contains(name) && manifest["assets"][name].is_object() &&
            manifest["assets"][name].value("source", std::string {}) == "override") {
          kinds.push_back(name);
        }
      }
    }
    const nlohmann::json override_state {
      {"active", true},
      {"kinds", std::move(kinds)},
      {"manual", true},
      {"updated_at", metadata.updated_at},
    };

    if (manifest.contains("match") && manifest["match"] == match &&
        manifest.contains("override") && manifest["override"] == override_state) {
      return manifest;
    }

    const auto base_revision = manifest["revision"].get<std::string>();
    manifest["match"] = match;
    manifest["override"] = override_state;
    manifest["revision"] = revised_manifest_revision(base_revision, metadata);
    return manifest;
  }
}  // namespace game_artwork
