/**
 * @file src/bounded_log_file.h
 * @brief Record-atomic bounded active and backup log storage.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>

namespace logging {
  inline constexpr std::uintmax_t runtime_log_max_bytes = 8U * 1024U * 1024U;

  enum class bounded_log_write_result_e {
    written,
    rotated,
    rejected,
  };

  /**
   * @brief Process-lifetime exclusive owner claim on the bounded runtime log.
   *
   * Advisory lock on a sidecar path next to the active log. The kernel drops
   * the lock when the owning process exits, so a crashed owner cannot leave
   * the log orphaned-locked. A process that fails to acquire it must not
   * inherit, rotate, truncate, or write the owner's files.
   */
  class bounded_log_owner_lock_t {
  public:
    explicit bounded_log_owner_lock_t(const std::filesystem::path &lock_path);
    ~bounded_log_owner_lock_t();

    bounded_log_owner_lock_t(const bounded_log_owner_lock_t &) = delete;
    bounded_log_owner_lock_t &operator=(const bounded_log_owner_lock_t &) = delete;

    [[nodiscard]] bool owned() const;

  private:
#ifdef _WIN32
    void *handle_ = nullptr;
#else
    int fd_ = -1;
#endif
  };

  class bounded_log_file_t {
  public:
    using rotation_callback_t = std::function<void()>;

    bounded_log_file_t(
      std::filesystem::path active_path,
      std::filesystem::path backup_path,
      std::uintmax_t max_bytes,
      rotation_callback_t on_rotation = {}
    );

    bounded_log_file_t(const bounded_log_file_t &) = delete;
    bounded_log_file_t &operator=(const bounded_log_file_t &) = delete;

    [[nodiscard]] bool good() const;
    [[nodiscard]] std::uintmax_t size() const;

    /**
     * @brief Write one complete formatted record, rotating before it if needed.
     *
     * A record larger than the configured active-file bound is rejected rather
     * than allowing the file to exceed the bound.
     */
    bounded_log_write_result_e write_record(std::string_view record);

    /**
     * @brief Remove the retained backup and truncate/reopen the active file.
     */
    bool clear();
    void flush();

    /**
     * @brief Preserve at most the newest max_bytes from a prior active log.
     *
     * The caller must hold the bounded log owner lock: this renames and
     * truncates without further ownership checks.
     */
    static bool preserve_existing(
      const std::filesystem::path &active_path,
      const std::filesystem::path &backup_path,
      std::uintmax_t max_bytes
    );

  private:
    bool open_active(std::ios_base::openmode mode);
    bool reopen_active_for_append();
    bool rotate();

    std::filesystem::path active_path_;
    std::filesystem::path backup_path_;
    std::uintmax_t max_bytes_;
    std::uintmax_t size_ = 0;
    rotation_callback_t on_rotation_;
    std::ofstream stream_;
  };
}  // namespace logging
