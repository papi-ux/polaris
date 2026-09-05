/**
 * @file src/platform/linux/multiseat_worker_authority.h
 * @brief Private per-generation filesystem authority for multiseat workers.
 */
#pragma once

#ifdef __linux__

#include "src/multiseat_worker_protocol.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace multiseat::worker_ipc {

  inline constexpr std::string_view authority_ipc_directory_name = "ipc";
  inline constexpr std::string_view authority_auth_directory_name = "auth";
  inline constexpr std::string_view authority_capability_file_name = "auth-token";
  inline constexpr std::string_view authority_record_file_name = "authority-record";
  inline constexpr std::string_view authority_control_socket_name = "control.sock";
  inline constexpr std::string_view authority_media_socket_name = "media.sock";

  enum class authority_status_e {
    applied,
    invalid_argument,
    unsafe_root,
    already_exists,
    random_failed,
    io_error,
    integrity_violation,
    inactive,
  };

  struct authority_paths_t {
    std::filesystem::path root;
    std::filesystem::path generation;
    std::filesystem::path ipc;
    std::filesystem::path auth;
    std::filesystem::path capability;
    std::filesystem::path record;
    std::filesystem::path control_socket;
    std::filesystem::path media_socket;

    bool operator==(const authority_paths_t &) const = default;
  };

  /**
   * Move-only authority for one exact worker generation.
   *
   * Open directory descriptors and inode identities fence cleanup from a
   * replacement directory that happens to reuse the same opaque name. The
   * capability is cleansed when the handle is destroyed or removed.
   */
  class authority_handle_t {
  public:
    authority_handle_t() = default;
    ~authority_handle_t();

    authority_handle_t(const authority_handle_t &) = delete;
    authority_handle_t &operator=(const authority_handle_t &) = delete;
    authority_handle_t(authority_handle_t &&other) noexcept;
    authority_handle_t &operator=(authority_handle_t &&other) noexcept;

    [[nodiscard]] bool active() const;
    [[nodiscard]] const endpoint_identity_t &identity() const;
    [[nodiscard]] const authority_paths_t &paths() const;
    [[nodiscard]] std::span<const std::uint8_t, capability_size> capability() const;
    [[nodiscard]] std::uint32_t owner_uid() const;

  private:
    friend class authority_store_t;

    authority_handle_t(
      endpoint_identity_t identity,
      authority_paths_t paths,
      const capability_t &capability,
      std::uint32_t owner_uid,
      std::uint64_t root_device,
      std::uint64_t root_inode,
      std::uint64_t generation_device,
      std::uint64_t generation_inode,
      std::uint64_t ipc_device,
      std::uint64_t ipc_inode,
      std::uint64_t auth_device,
      std::uint64_t auth_inode,
      std::uint64_t capability_device,
      std::uint64_t capability_inode,
      std::uint64_t record_device,
      std::uint64_t record_inode,
      int generation_fd,
      int ipc_fd,
      int auth_fd
    );

    void release_resources() noexcept;

    endpoint_identity_t identity_;
    authority_paths_t paths_;
    capability_t capability_ {};
    std::uint32_t owner_uid_ = 0;
    std::uint64_t root_device_ = 0;
    std::uint64_t root_inode_ = 0;
    std::uint64_t generation_device_ = 0;
    std::uint64_t generation_inode_ = 0;
    std::uint64_t ipc_device_ = 0;
    std::uint64_t ipc_inode_ = 0;
    std::uint64_t auth_device_ = 0;
    std::uint64_t auth_inode_ = 0;
    std::uint64_t capability_device_ = 0;
    std::uint64_t capability_inode_ = 0;
    std::uint64_t record_device_ = 0;
    std::uint64_t record_inode_ = 0;
    int generation_fd_ = -1;
    int ipc_fd_ = -1;
    int auth_fd_ = -1;
    bool active_ = false;
  };

  struct authority_create_result_t {
    authority_status_e status = authority_status_e::invalid_argument;
    std::optional<authority_handle_t> authority;

    [[nodiscard]] bool created() const {
      return status == authority_status_e::applied && authority.has_value();
    }
  };

  struct authority_recovery_result_t {
    authority_status_e status = authority_status_e::invalid_argument;
    std::size_t observed = 0;
    std::size_t active = 0;
    std::vector<endpoint_identity_t> active_identities;
    std::vector<authority_handle_t> inactive;

    [[nodiscard]] bool inspected() const {
      return status == authority_status_e::applied;
    }

    [[nodiscard]] bool all_inactive() const {
      return inspected() && active == 0;
    }
  };

  /** Returns true after filling all 32 capability bytes. */
  using capability_factory_t = std::function<bool(capability_t &)>;

  /**
   * Owns generation directories below one pre-created private runtime root.
   *
   * The root must already exist, be owned by the effective user, have exact
   * mode 0700, and contain no symlink component. This class never recursively
   * deletes. Cleanup removes only the exact inode-fenced generation and its
   * allowlisted token, signed record, and socket nodes.
   */
  class authority_store_t {
  public:
    explicit authority_store_t(
      std::filesystem::path root,
      capability_factory_t capability_factory = {}
    );
    ~authority_store_t();

    authority_store_t(const authority_store_t &) = delete;
    authority_store_t &operator=(const authority_store_t &) = delete;
    authority_store_t(authority_store_t &&) = delete;
    authority_store_t &operator=(authority_store_t &&) = delete;

    [[nodiscard]] authority_status_e status() const;
    [[nodiscard]] authority_create_result_t create(
      const endpoint_identity_t &identity,
      std::string runtime_namespace
    );
    /**
     * Reacquires only signed authority records absent from an authoritative
     * backend inventory. Active identities remain untouched and are counted.
     * Any malformed, duplicate, replaced, or unexpected entry rejects the
     * entire scan without returning cleanup authority.
     */
    [[nodiscard]] authority_recovery_result_t recover_inactive(
      std::span<const endpoint_identity_t> active_identities
    );
    [[nodiscard]] authority_status_e validate(const authority_handle_t &authority) const;
    [[nodiscard]] authority_status_e remove(authority_handle_t &authority);

  private:
    std::filesystem::path root_;
    capability_factory_t capability_factory_;
    std::uint32_t owner_uid_ = 0;
    std::uint64_t root_device_ = 0;
    std::uint64_t root_inode_ = 0;
    int root_fd_ = -1;
    authority_status_e status_ = authority_status_e::unsafe_root;
  };

}  // namespace multiseat::worker_ipc

#endif
