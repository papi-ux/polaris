/**
 * @file src/platform/linux/kwin_permission_entries.h
 * @brief The user-local desktop entries through which KWin grants Polaris screencast access:
 * one stable name per binary, and which old entries to delete.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kwingrab::permission {
  inline constexpr std::string_view desktop_prefix = "dev.polaris-stream.app.Polaris.kwin";
  /// The Name Polaris writes into its own entries; only entries carrying it are ever deleted.
  inline constexpr std::string_view entry_name = "Polaris KWin screencast permission";

  /**
   * @brief The entry name for one Polaris binary. It stays the same for a path, so a restart reuses
   * the entry and an update adds one entry instead of one per start.
   */
  inline std::string desktop_file_name(std::string_view exe) {
    std::uint64_t hash = 14695981039346656037ULL;  // FNV-1a
    for (const unsigned char ch : exe) {
      hash ^= ch;
      hash *= 1099511628211ULL;
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string suffix(16, '0');
    for (auto i = suffix.size(); i-- > 0;) {
      suffix[i] = digits[hash & 0xf];
      hash >>= 4;
    }
    return std::string {desktop_prefix} + "." + suffix + ".desktop";
  }

  /**
   * @brief One Polaris permission entry in the user's applications directory.
   */
  struct entry_t {
    std::filesystem::path path;
    std::string exec;  ///< The binary the entry grants; empty when the entry has no Exec line.
    bool written_by_polaris = false;  ///< Its Name is entry_name, so Polaris wrote it.
  };

  /**
   * @brief The entry kept for the running binary, if one exists, and the entries to delete.
   */
  struct plan_t {
    std::optional<std::filesystem::path> keep;
    std::vector<std::filesystem::path> remove;
  };

  /**
   * @brief Decide which entries stay. An entry for a binary that no longer exists is what an update
   * or a deleted build leaves behind, and a second entry for the running binary repeats its grant,
   * so both go. Entries for other binaries that still exist stay, since two builds can run on one
   * host, and an entry Polaris did not write is never deleted.
   */
  inline plan_t plan_entries(const std::vector<entry_t> &entries, std::string_view current_exe,
                             const std::function<bool(const std::string &)> &binary_exists) {
    plan_t plan;
    const auto preferred = desktop_file_name(current_exe);
    for (const auto &entry : entries) {
      if (entry.exec == current_exe && entry.path.filename() == preferred) {
        plan.keep = entry.path;
      }
    }
    for (const auto &entry : entries) {
      if (entry.exec == current_exe) {
        if (!plan.keep) {
          plan.keep = entry.path;
        } else if (entry.path != *plan.keep && entry.written_by_polaris) {
          plan.remove.push_back(entry.path);
        }
        continue;
      }
      if (entry.written_by_polaris && (entry.exec.empty() || !binary_exists(entry.exec))) {
        plan.remove.push_back(entry.path);
      }
    }
    return plan;
  }
}  // namespace kwingrab::permission
