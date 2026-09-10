/**
 * @file src/private_state_file.h
 * @brief Secure bounded persistence for authorization-adjacent private state.
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
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

  struct read_result_t {
    read_status_e status = read_status_e::io_error;
    std::string payload;

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

    explicit operator bool() const {
      return status == write_status_e::committed;
    }
  };

  read_result_t read_secure(const std::filesystem::path &target, std::size_t max_bytes,
                            bool permit_public_read = false, bool wait_for_lock = true);
  // One cross-process transaction. Contention fails immediately so callers can
  // retain session authority without waiting on an unrelated file-lock holder.
  // The callback must not call another persistence operation on this path.
  write_result_t update_atomic(const std::filesystem::path &target, std::size_t max_bytes,
    const std::function<std::optional<std::string>(const read_result_t &)> &update,
    bool permit_public_read = false);

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
