#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace test_paths {

  inline std::filesystem::path root() {
    const auto path = std::filesystem::temp_directory_path() / "polaris-tests";
    std::filesystem::create_directories(path);
    return path;
  }

  namespace detail {
    inline std::string &log_owner_storage() {
      // Only reached by a binary that never called set_log_owner().
      static std::string owner = "test_polaris";
      return owner;
    }
  }  // namespace detail

  /// Name of the binary the log file belongs to.
  inline const std::string &log_owner() {
    return detail::log_owner_storage();
  }

  /**
   * @brief Scope the log file to this binary. Call once from main() with argv[0].
   *
   * Every test binary logs into the same directory, and one of them truncates the
   * log through logging::clear_log_file(). While the file name was a shared
   * constant, `ctest -j` had binaries destroying each other's log and the tests
   * that assert on log contents failed at random, naming the code under test
   * rather than the collision (issue #414).
   */
  inline void set_log_owner(std::string_view argv0) {
    auto name = std::filesystem::path(argv0).filename().string();
    if (!name.empty()) {
      detail::log_owner_storage() = std::move(name);
    }
  }

  inline std::filesystem::path log_file() {
    return root() / (log_owner() + ".log");
  }

  inline std::filesystem::path write_file(int file_num) {
    return root() / std::format("write_file_test_{}.txt", file_num);
  }

}  // namespace test_paths
