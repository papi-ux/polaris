#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace configuration_store {
  // Lock order: Doctor/session authority, configuration, adaptive controller.
  std::recursive_mutex &mutex();
  struct snapshot_t { std::string contents; std::string revision; };
  std::optional<snapshot_t> read(const std::string &path);
  std::string revision(const std::string &path, bool refresh = false);
  enum class result { committed, conflict, failed };
  result replace(const std::string &path, const std::string &contents,
                 const std::optional<std::string> &expected = std::nullopt);
  result patch(const std::string &path,
               const std::unordered_map<std::string, std::string> &updates,
               const std::optional<std::string> &expected = std::nullopt);
}

