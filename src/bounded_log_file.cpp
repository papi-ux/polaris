/**
 * @file src/bounded_log_file.cpp
 * @brief Record-atomic bounded active and backup log storage.
 */

#include "bounded_log_file.h"

#include "file_handler.h"

#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace logging {
  bounded_log_file_t::bounded_log_file_t(
    std::filesystem::path active_path,
    std::filesystem::path backup_path,
    const std::uintmax_t max_bytes,
    rotation_callback_t on_rotation
  ):
      active_path_(std::move(active_path)),
      backup_path_(std::move(backup_path)),
      max_bytes_(max_bytes),
      on_rotation_(std::move(on_rotation)) {
    if (active_path_.empty() || backup_path_.empty() || active_path_ == backup_path_ || max_bytes_ == 0 ||
        max_bytes_ > std::numeric_limits<std::streamsize>::max()) {
      throw std::invalid_argument("bounded log paths must be non-empty and the byte bound must be representable");
    }
    open_active(std::ios::out | std::ios::trunc);
  }

  bool bounded_log_file_t::good() const {
    return stream_.is_open() && stream_.good();
  }

  std::uintmax_t bounded_log_file_t::size() const {
    return size_;
  }

  bool bounded_log_file_t::open_active(const std::ios_base::openmode mode) {
    stream_.clear();
    stream_.open(active_path_, mode | std::ios::binary);
    if (!stream_.is_open() || !stream_.good()) {
      return false;
    }

    std::error_code error;
    size_ = std::filesystem::file_size(active_path_, error);
    if (error) {
      stream_.close();
      return false;
    }
    return true;
  }

  bool bounded_log_file_t::reopen_active_for_append() {
    return open_active(std::ios::out | std::ios::app);
  }

  bool bounded_log_file_t::rotate() {
    stream_.flush();
    stream_.close();

    std::error_code error;
    std::filesystem::remove(backup_path_, error);
    if (error) {
      reopen_active_for_append();
      return false;
    }

    std::filesystem::rename(active_path_, backup_path_, error);
    if (error) {
      reopen_active_for_append();
      return false;
    }

    if (open_active(std::ios::out | std::ios::trunc)) {
      return true;
    }

    // A failed open may still have created the replacement path. Remove it
    // before restoring the prior active log, which matters on Windows where a
    // rename cannot replace an existing destination.
    stream_.close();
    std::filesystem::remove(active_path_, error);
    std::filesystem::rename(backup_path_, active_path_, error);
    reopen_active_for_append();
    return false;
  }

  bounded_log_write_result_e bounded_log_file_t::write_record(const std::string_view record) {
    if (!good()) {
      return bounded_log_write_result_e::rejected;
    }

    const auto append_newline = record.empty() || record.back() != '\n';
    if (record.size() > max_bytes_ ||
        (append_newline && record.size() == max_bytes_)) {
      return bounded_log_write_result_e::rejected;
    }
    const auto record_bytes = static_cast<std::uintmax_t>(record.size()) + (append_newline ? 1U : 0U);

    auto result = bounded_log_write_result_e::written;
    if (size_ > 0 && record_bytes > max_bytes_ - size_) {
      if (!rotate()) {
        return bounded_log_write_result_e::rejected;
      }
      // Publish the new generation after the replacement active file exists,
      // but before it can expose any bytes from that generation.
      if (on_rotation_) {
        on_rotation_();
      }
      result = bounded_log_write_result_e::rotated;
    }

    if (!record.empty()) {
      stream_.write(record.data(), static_cast<std::streamsize>(record.size()));
    }
    if (append_newline) {
      stream_.put('\n');
    }
    stream_.flush();
    if (!stream_.good()) {
      return bounded_log_write_result_e::rejected;
    }
    size_ += record_bytes;
    return result;
  }

  bool bounded_log_file_t::clear() {
    if (!stream_.is_open()) {
      return false;
    }

    stream_.flush();
    stream_.close();

    std::error_code error;
    std::filesystem::remove(backup_path_, error);
    if (error) {
      reopen_active_for_append();
      return false;
    }
    return open_active(std::ios::out | std::ios::trunc);
  }

  void bounded_log_file_t::flush() {
    if (stream_.is_open()) {
      stream_.flush();
    }
  }

  bool bounded_log_file_t::preserve_existing(
    const std::filesystem::path &active_path,
    const std::filesystem::path &backup_path,
    const std::uintmax_t max_bytes
  ) {
    if (active_path.empty() || backup_path.empty() || active_path == backup_path ||
        max_bytes == 0 || max_bytes > std::numeric_limits<std::size_t>::max() ||
        max_bytes > std::numeric_limits<std::streamsize>::max()) {
      return false;
    }

    std::error_code error;
    if (!std::filesystem::exists(active_path, error)) {
      if (error || !std::filesystem::exists(backup_path, error)) {
        return !error;
      }
      if (error) {
        return false;
      }

      const auto backup_size = std::filesystem::file_size(backup_path, error);
      if (error || backup_size <= max_bytes) {
        return !error;
      }

      const auto backup_path_string = backup_path.string();
      const auto tail = file_handler::read_file_tail(
        backup_path_string.c_str(),
        static_cast<std::size_t>(max_bytes)
      );
      if (tail.content.size() != max_bytes || tail.end_offset != backup_size || !tail.truncated) {
        return false;
      }

      std::ofstream bounded_backup {backup_path, std::ios::out | std::ios::trunc | std::ios::binary};
      bounded_backup.write(tail.content.data(), static_cast<std::streamsize>(tail.content.size()));
      bounded_backup.flush();
      return bounded_backup.good();
    }
    if (error) {
      return false;
    }

    const auto prior_size = std::filesystem::file_size(active_path, error);
    if (error) {
      return false;
    }

    std::filesystem::remove(backup_path, error);
    if (error) {
      return false;
    }

    if (prior_size <= max_bytes) {
      std::filesystem::rename(active_path, backup_path, error);
      return !error;
    }

    const auto active_path_string = active_path.string();
    const auto tail = file_handler::read_file_tail(active_path_string.c_str(), static_cast<std::size_t>(max_bytes));
    if (tail.content.size() != max_bytes || tail.end_offset != prior_size || !tail.truncated) {
      return false;
    }

    std::ofstream backup {backup_path, std::ios::out | std::ios::trunc | std::ios::binary};
    backup.write(tail.content.data(), static_cast<std::streamsize>(tail.content.size()));
    backup.flush();
    if (!backup.good()) {
      return false;
    }
    backup.close();

    std::filesystem::remove(active_path, error);
    return !error;
  }
}  // namespace logging
