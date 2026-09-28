/**
 * @file src/private_state_file.h
 * @brief Secure bounded persistence for authorization-adjacent private state.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace private_state_file {
  enum class read_status_e {
    ok,
    missing,
    rejected,
    io_error,
  };

  /**
   * @brief Which check turned a state file away.
   *
   * read_status_e says whether a read worked. This says why it did not, so a
   * caller can name the problem and its fix instead of failing quietly. Only
   * the walk to the file's directory explains itself in the log (it names the
   * directory and sets refusal_t::logged), and only when the caller leaves it
   * to the walk (refusal_log_e); every other refusal is left for the caller to
   * explain.
   */
  enum class refusal_e {
    none,  ///< Nothing was refused.
    missing,  ///< The file does not exist.
    directory_missing,  ///< A directory on the way to the file does not exist.
    directory_foreign_owner,  ///< A directory on the way is owned by another user.
    directory_writable,  ///< A directory on the way is writable by group or other.
    directory_unusable,  ///< A directory on the way could not be opened or inspected.
    directory_symlink,  ///< A directory on the way is a symbolic link, which the walk does not follow.
    lock_unavailable,  ///< The <name>.lock sidecar could not be opened, created or locked.
    lock_unsafe,  ///< The sidecar is not a regular file this user owns with a single link.
    lock_busy,  ///< Another process holds the sidecar lock.
    permission_denied,  ///< This user may not open the file.
    symlink,  ///< The file is a symbolic link.
    not_regular,  ///< The file is not a regular file.
    foreign_owner,  ///< The file is owned by another user.
    hard_linked,  ///< The file has more than one hard link.
    group_writable,  ///< The file's group can write it.
    other_writable,  ///< Every user can write the file.
    not_private,  ///< Group or other can read a file only its owner may.
    oversize,  ///< The file is larger than the caller accepts.
    size_changed,  ///< The file grew or shrank while it was being read.
    read_failed,  ///< Opening or reading the file failed for another reason.
  };

  /// The enumerator's own name, such as "group_writable": a stable id that says
  /// why without naming the file.
  std::string_view refusal_name(refusal_e kind);

  struct refusal_t {
    refusal_e kind = refusal_e::none;
    int error_number = 0;  ///< errno behind the refusal, when a call failed.
    std::filesystem::path directory;  ///< The directory a directory_* refusal is about.
    bool holds_file = false;  ///< That directory is the one that holds the file, not one above it.
    /// mode and owner were read from the file, or for a directory_* refusal from
    /// that directory; links and size only ever come from the file.
    bool inspected = false;
    unsigned mode = 0;  ///< Permission bits (st_mode & 07777).
    unsigned owner = 0;  ///< Owning uid.
    std::uintmax_t links = 0;  ///< Hard link count.
    std::uintmax_t size = 0;  ///< Size in bytes when the read began.
    bool logged = false;  ///< The refusal was already explained in the log where it was found.

    explicit operator bool() const {
      return kind != refusal_e::none;
    }
  };

  struct read_result_t {
    read_status_e status = read_status_e::io_error;
    std::string payload;
    refusal_t refusal;

    explicit operator bool() const {
      return status == read_status_e::ok;
    }
  };

  enum class write_status_e {
    committed,
    not_committed,
    durability_uncertain,
  };

  struct write_result_t {
    write_status_e status = write_status_e::not_committed;
    /// Set when the file or its surroundings turned the write away before it began.
    refusal_t refusal;

    explicit operator bool() const {
      return status == write_status_e::committed;
    }
  };

  /**
   * @brief Who explains a directory the walk refused.
   *
   * By default the walk logs each refusal where it finds it, every time. A
   * caller that describes refusals itself, and logs them once per change, asks
   * the walk to stay quiet so the log carries one voice instead of two.
   */
  enum class refusal_log_e {
    walk,  ///< The walk logs the refusal and sets refusal_t::logged.
    caller,  ///< The caller explains it; the walk logs nothing.
  };

  read_result_t read_secure(const std::filesystem::path &target, std::size_t max_bytes,
                            bool permit_public_read = false, bool wait_for_lock = true,
                            refusal_log_e refusal_log = refusal_log_e::walk);

  struct leased_read_result_t {
    read_result_t read;
    std::shared_ptr<void> lease;
  };

  // Nonblocking exclusive read. Keep the lease while the snapshot authorizes
  // live resources. Cooperating writers cannot change it until the last owner
  // releases the lease. Failed reads never retain the lock.
  leased_read_result_t read_with_lease(const std::filesystem::path &target,
                                     std::size_t max_bytes);
  // One cross-process transaction. Contention fails immediately so callers can
  // retain session authority without waiting on an unrelated file-lock holder.
  // The callback must not call another persistence operation on this path.
  write_result_t update_atomic(const std::filesystem::path &target, std::size_t max_bytes,
    const std::function<std::optional<std::string>(const read_result_t &)> &update,
    bool permit_public_read = false, refusal_log_e refusal_log = refusal_log_e::walk);

  write_result_t write_atomic(const std::filesystem::path &target, std::string_view payload);

#ifdef POLARIS_TESTS
  enum class write_fault_e {
    none,
    open,
    short_write,
    flush,
    sync,
    rename,
    parent_sync,
    parent_close,
    post_rename_durability,
    directory_close,
  };

  void set_write_fault_for_tests(write_fault_e fault);
  void set_parent_component_fault_index_for_tests(std::size_t index);
  void set_parent_eexist_race_for_tests(bool enabled);
  void set_trusted_home_symlink_for_tests(const std::filesystem::path &path);
  void set_trusted_home_owner_mismatch_for_tests(bool enabled);
#endif
}  // namespace private_state_file
