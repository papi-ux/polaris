#pragma once

#include <filesystem>
#include <format>
#include <string>

namespace test_paths {

  inline std::filesystem::path root() {
    const auto path = std::filesystem::temp_directory_path() / "polaris-tests";
    std::filesystem::create_directories(path);
    return path;
  }

  inline std::filesystem::path log_file() {
    return root() / "test_polaris.log";
  }

  inline std::filesystem::path write_file(int file_num) {
    return root() / std::format("write_file_test_{}.txt", file_num);
  }

}  // namespace test_paths
