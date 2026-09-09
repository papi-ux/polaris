/** @file src/platform/linux/process_environment.h
 * Serialize host environment snapshots with Polaris environment mutation.
 */
#pragma once
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>

namespace process_environment {
  // Recursive ownership permits atomic read-modify-write and multi-key repairs
  // to use the same checked setters as individual platform API calls.
  inline std::recursive_mutex mutex;

  inline int set(const char *name, const char *value, int overwrite = 1) {
    std::lock_guard lock(mutex);
    return ::setenv(name, value, overwrite);
  }
  inline int unset(const char *name) {
    std::lock_guard lock(mutex);
    return ::unsetenv(name);
  }
  inline std::optional<std::string> get(const char *name) {
    std::lock_guard lock(mutex);
    const auto value = ::getenv(name);
    return value ? std::make_optional<std::string>(value) : std::nullopt;
  }
  inline std::map<std::string, std::string> snapshot() {
    std::lock_guard lock(mutex);
    std::map<std::string, std::string> result;
    for (auto entry = environ; entry && *entry; ++entry) {
      const std::string value {*entry};
      const auto separator = value.find('=');
      if (separator != std::string::npos) result[value.substr(0, separator)] = value.substr(separator + 1);
    }
    return result;
  }
}
