/**
 * @file file_handler.h
 * @brief Declarations for file handling functions.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/**
 * @brief Responsible for file handling functions.
 */
namespace file_handler {
  /**
   * @brief A bounded byte range read from the end of a file.
   *
   * Offsets describe the half-open byte range `[start_offset, end_offset)` in
   * the file snapshot used for the read. `truncated` is true when bytes before
   * `start_offset` were omitted.
   */
  struct tail_result_t {
    std::string content;
    std::uintmax_t start_offset {0};
    std::uintmax_t end_offset {0};
    bool truncated {false};
  };

  /**
   * @brief Get the parent directory of a file or directory.
   * @param path The path of the file or directory.
   * @return The parent directory.
   * @examples
   * std::string parent_dir = get_parent_directory("path/to/file");
   * @examples_end
   */
  std::string get_parent_directory(const std::string &path);

  /**
   * @brief Make a directory.
   * @param path The path of the directory.
   * @return `true` on success, `false` on failure.
   * @examples
   * bool dir_created = make_directory("path/to/directory");
   * @examples_end
   */
  bool make_directory(const std::string &path);

  /**
   * @brief Read a file to string.
   * @param path The path of the file.
   * @return The contents of the file.
   * @examples
   * std::string contents = read_file("path/to/file");
   * @examples_end
   */
  std::string read_file(const char *path);

  /**
   * @brief Read at most `max_bytes` from the end of a file.
   * @param path The path of the file.
   * @param max_bytes The maximum number of bytes to return. Must be non-zero.
   * @return The binary-safe tail content and its byte offsets. Missing or
   * unreadable files return an empty result.
   * @throws std::invalid_argument when `max_bytes` is zero.
   */
  tail_result_t read_file_tail(const char *path, std::size_t max_bytes);

  /**
   * @brief Writes a file.
   * @param path The path of the file.
   * @param contents The contents to write.
   * @return ``0`` on success, ``-1`` on failure.
   * @examples
   * int write_status = write_file("path/to/file", "file contents");
   * @examples_end
   */
  int write_file(const char *path, const std::string_view &contents);
}  // namespace file_handler
