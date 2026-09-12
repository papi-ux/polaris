/**
 * @file src/platform/linux/multiseat_profile_catalog.h
 * @brief Private profile storage and administrative Docker provisioning.
 */
#pragma once
#ifdef __linux__

#include "multiseat_container_backend.h"
#include "src/private_state_file.h"

namespace multiseat::profiles {
  inline constexpr std::size_t maximum_catalog_bytes = 4 * 1024 * 1024;

  struct entry_t {
    container::profile_t storage;
    std::string name;
    workload_plan_t workload;
    std::vector<std::string> client_keys;
  };

  struct catalog_t {
    std::uint32_t owner_uid = 0;
    std::uint32_t owner_gid = 0;
    std::vector<entry_t> profiles;
  };

  struct loaded_catalog_t {
    catalog_t catalog;
    std::shared_ptr<void> lease;
  };

  [[nodiscard]] std::optional<catalog_t> decode(std::string_view payload);
  // Throws on invalid state rather than serializing a partially valid catalog.
  [[nodiscard]] std::string encode(const catalog_t &catalog);
  [[nodiscard]] std::optional<loaded_catalog_t> load(const std::filesystem::path &path);

  struct change_result_t {
    private_state_file::write_status_e status = private_state_file::write_status_e::not_committed;
    std::string error;
    std::string profile_key;
    // Retain these on any failure after provisioning starts. Never silently
    // adopt, reinitialize, or delete a volume after an uncertain transaction.
    std::string volume_name;
    std::string initializer_name;
    std::string network_name;
    explicit operator bool() const { return status == private_state_file::write_status_e::committed; }
  };

  [[nodiscard]] change_result_t initialize(const std::filesystem::path &path,
    std::uint32_t uid, std::uint32_t gid);
  [[nodiscard]] change_result_t assign(const std::filesystem::path &path,
    std::string_view profile_key, std::string_view client_key);
  [[nodiscard]] change_result_t unassign(const std::filesystem::path &path,
    std::string_view client_key);
  // Supported Gamescope or Steam workloads only. Immutable local images, fresh
  // private storage, and an owned bridge for Steam. No pulls or host binds.
  [[nodiscard]] change_result_t create(const std::filesystem::path &path,
    std::string_view name, std::string_view image, container::host_t &host,
    const workload_plan_t &workload = {workload_kind_e::gamescope, "input-pong-v1"});
  int command(int argc, char **argv);
}  // namespace multiseat::profiles
#endif
