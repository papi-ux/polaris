/**
 * @file src/platform/linux/multiseat_container_host.h
 * @brief Live Linux host adapter for Docker and the retained Podman backend.
 */
#pragma once

#ifdef __linux__

#include "multiseat_container_backend.h"
#include <stop_token>

namespace multiseat::container {

  class local_host_t final : public host_t {
  public:
    explicit local_host_t(std::stop_token stop = {}) : stop_(stop) {}
    [[nodiscard]] std::uint64_t effective_uid() const override;
    [[nodiscard]] std::uint64_t effective_gid() const override;
    [[nodiscard]] bool executable_file(const std::filesystem::path &path) const override;
    [[nodiscard]] bool trusted_runtime_file(const std::filesystem::path &path) const override;
    [[nodiscard]] bool trusted_data_file(
      const std::filesystem::path &path, std::string_view expected
    ) const override;
    [[nodiscard]] std::optional<std::vector<std::uint64_t>> supplementary_groups() const override;
    [[nodiscard]] std::optional<group_membership_t> group_membership(std::string_view group) const override;
    [[nodiscard]] bool readable_directory(const std::filesystem::path &path) const override;
    [[nodiscard]] bool private_read_write_directory(
      const std::filesystem::path &path
    ) const override;
    [[nodiscard]] bool private_readable_file(
      const std::filesystem::path &path
    ) const override;
    [[nodiscard]] std::optional<character_device_identity_t>
    read_write_character_device(
      const std::filesystem::path &path
    ) const override;
    [[nodiscard]] std::optional<std::string> read_owned_regular_file(
      const std::filesystem::path &path,
      std::size_t max_bytes
    ) const override;
    command_result_t run(
      const std::vector<std::string> &argv,
      std::chrono::milliseconds timeout,
      std::size_t max_output_bytes
    ) override;
  private:
    std::stop_token stop_;
  };

}  // namespace multiseat::container

#endif
