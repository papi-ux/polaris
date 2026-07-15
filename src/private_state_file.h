/**
 * @file src/private_state_file.h
 * @brief Secure bounded persistence for authorization-adjacent private state.
 */
#pragma once

#include <cstddef>
#include <filesystem>
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

  read_result_t read_secure(const std::filesystem::path &target, std::size_t max_bytes);
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
#endif
}  // namespace private_state_file
