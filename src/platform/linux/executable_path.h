/**
 * @file src/platform/linux/executable_path.h
 * @brief Locale-independent Linux executable lookup.
 */
#pragma once

#include <cstdlib>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace platf::linux_util {
  inline bool is_executable_file(const std::string &path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && access(path.c_str(), X_OK) == 0;
  }

  inline std::string find_executable_in_path(std::string_view name, const char *path_override = nullptr) {
    if (name.empty()) {
      return {};
    }
    const std::string requested(name);
    if (requested.find('/') != std::string::npos) {
      return is_executable_file(requested) ? requested : std::string {};
    }

    const char *configured_path = path_override ? path_override : std::getenv("PATH");
    const std::string path = configured_path && *configured_path ? configured_path : "/usr/local/bin:/usr/bin:/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
      const auto end = path.find(':', start);
      auto dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (dir.empty()) {
        dir = ".";
      }
      const auto candidate = dir + "/" + requested;
      if (is_executable_file(candidate)) {
        return candidate;
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return {};
  }
}  // namespace platf::linux_util
