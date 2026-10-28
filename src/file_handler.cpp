/**
 * @file file_handler.cpp
 * @brief Definitions for file handling functions.
 */

// standard includes
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>

// local includes
#include "file_handler.h"
#include "logging.h"

namespace file_handler {
  std::string get_parent_directory(const std::string &path) {
    // remove any trailing path separators
    std::string trimmed_path = path;
    while (!trimmed_path.empty() && trimmed_path.back() == '/') {
      trimmed_path.pop_back();
    }

    std::filesystem::path p(trimmed_path);
    return p.parent_path().string();
  }

  bool make_directory(const std::string &path) {
    // first, check if the directory already exists
    if (std::filesystem::exists(path)) {
      return true;
    }

    return std::filesystem::create_directories(path);
  }

  bool make_private_directory(const std::filesystem::path &path, std::error_code &error) {
    error.clear();
    if (std::filesystem::is_directory(path, error)) {
      return true;
    }
    if (error && error != std::errc::no_such_file_or_directory) {
      return false;
    }
    error.clear();
    const auto parent = path.parent_path();
    if (!parent.empty() && parent != path && !make_private_directory(parent, error)) {
      return false;
    }
    // Set the mode at creation, without changing the process-wide umask or
    // chmod'ing a directory that another process may have created meanwhile.
    if (::mkdir(path.c_str(), S_IRWXU) == 0) {
      return true;
    }
    const auto failure = errno;
    if (failure == EEXIST && std::filesystem::is_directory(path, error)) {
      return true;
    }
    if (!error) {
      error = std::error_code(failure, std::generic_category());
    }
    return false;
  }

  void make_private_directory(const std::filesystem::path &path) {
    std::error_code error;
    if (!make_private_directory(path, error)) {
      throw std::filesystem::filesystem_error("Creating private state directory", path, error);
    }
  }

  bool seed_user_file(const std::filesystem::path &source, const std::filesystem::path &destination) {
    if (!std::filesystem::copy_file(source, destination, std::filesystem::copy_options::skip_existing)) {
      return false;
    }
    // Immutable packages (such as the Nix store) carry read-only templates.
    // Only the new user copy needs write access; never chmod a substituted link.
    std::filesystem::permissions(destination,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::add | std::filesystem::perm_options::nofollow);
    return true;
  }

  std::string read_file(const char *path) {
    if (!std::filesystem::exists(path)) {
      BOOST_LOG(debug) << "Missing file: " << path;
      return {};
    }

    std::ifstream in(path);
    return std::string {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
  }

  tail_result_t read_file_tail(const char *path, std::size_t max_bytes) {
    if (max_bytes == 0) {
      throw std::invalid_argument("max_bytes must be greater than zero");
    }

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
      BOOST_LOG(debug) << "Unable to open file tail: " << path;
      return {};
    }

    const std::streampos end_position = in.tellg();
    if (end_position == std::streampos {-1}) {
      BOOST_LOG(debug) << "Unable to determine file tail offset: " << path;
      return {};
    }

    const auto end_offset = static_cast<std::uintmax_t>(end_position);
    const auto maximum_stream_read = static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max());
    const auto requested_bytes = std::min({
      end_offset,
      static_cast<std::uintmax_t>(max_bytes),
      maximum_stream_read,
    });
    const auto start_offset = end_offset - requested_bytes;

    tail_result_t result;
    result.start_offset = start_offset;
    result.end_offset = end_offset;
    result.truncated = start_offset != 0;

    if (requested_bytes == 0) {
      return result;
    }

    in.seekg(static_cast<std::streamoff>(start_offset), std::ios::beg);
    if (!in.good()) {
      BOOST_LOG(debug) << "Unable to seek to file tail: " << path;
      return {};
    }

    result.content.resize(static_cast<std::size_t>(requested_bytes));
    in.read(result.content.data(), static_cast<std::streamsize>(requested_bytes));
    if (in.gcount() != static_cast<std::streamsize>(requested_bytes)) {
      BOOST_LOG(debug) << "Unable to read complete file tail: " << path;
      return {};
    }

    return result;
  }

  int write_file(const char *path, const std::string_view &contents) {
    std::ofstream out(path);

    if (!out.is_open()) {
      return -1;
    }

    out << contents;
    out.flush();
    if (!out) return -1;
    out.close();
    return out.fail() ? -1 : 0;
  }
}  // namespace file_handler
