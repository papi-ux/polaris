#pragma once
#include "private_state_file.h"
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace configuration_store {
  // Lock order: Doctor/session authority, configuration, adaptive controller.
  std::recursive_mutex &mutex();
  struct snapshot_t { std::string contents; std::string revision; };

  /// Why the settings store turned a file away, in words an operator can act on.
  struct refusal_t {
    private_state_file::refusal_e kind = private_state_file::refusal_e::none;
    std::string path;  ///< The settings file.
    std::string reason;  ///< What is wrong, in plain sentences.
    std::string fix;  ///< What to run or do about it.
    explicit operator bool() const { return kind != private_state_file::refusal_e::none; }
  };

  /// The one description the read and the write side share, so both name the same fix.
  refusal_t describe_refusal(const std::string &path, const private_state_file::refusal_t &refusal);

  /// A refused read returns nullopt and, when @p refusal is given, says why there.
  /// The log carries one warning each time the refusal changes, not one per read.
  std::optional<snapshot_t> read(const std::string &path, refusal_t *refusal = nullptr);

  /// The refusal the last read or save of @p path met, or nullopt when it went
  /// through or nothing has touched the file yet. It never reads the file and
  /// never takes mutex(), so the Doctor can ask while it builds a report.
  std::optional<refusal_t> last_refusal(const std::string &path);
  std::string revision(const std::string &path, bool refresh = false);
  enum class result { committed, conflict, failed };
  result replace(const std::string &path, const std::string &contents,
                 const std::optional<std::string> &expected = std::nullopt);
  result patch(const std::string &path,
               const std::unordered_map<std::string, std::string> &updates,
               const std::optional<std::string> &expected = std::nullopt);
}

