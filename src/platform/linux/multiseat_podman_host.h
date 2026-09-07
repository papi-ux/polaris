/**
 * @file src/platform/linux/multiseat_podman_host.h
 * @brief Live Linux host adapter for the rootless Podman worker backend.
 */
#pragma once

#ifdef __linux__

#include "multiseat_podman_backend.h"

namespace multiseat::podman {

  class local_host_t final : public host_t {
  public:
    [[nodiscard]] std::uint64_t effective_uid() const override;
    [[nodiscard]] bool executable_file(const std::filesystem::path &path) const override;
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
  };

}  // namespace multiseat::podman

#endif
