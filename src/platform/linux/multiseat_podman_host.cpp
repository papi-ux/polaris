/**
 * @file src/platform/linux/multiseat_podman_host.cpp
 * @brief Live Linux host adapter for the rootless Podman worker backend.
 */
#include "multiseat_podman_host.h"

#ifdef __linux__

#include "misc.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <limits>

namespace multiseat::podman {
  namespace {
    bool accessible_as(const std::filesystem::path &path, mode_t type, int mode) {
      struct stat metadata {};
      if (stat(path.c_str(), &metadata) != 0 ||
          (metadata.st_mode & S_IFMT) != type) {
        return false;
      }
      return access(path.c_str(), mode) == 0;
    }

    bool private_accessible_as(
      const std::filesystem::path &path,
      mode_t type,
      int mode
    ) {
      struct stat metadata {};
      return lstat(path.c_str(), &metadata) == 0 &&
             (metadata.st_mode & S_IFMT) == type &&
             metadata.st_uid == geteuid() &&
             (metadata.st_mode & (S_IRWXG | S_IRWXO)) == 0 &&
             (metadata.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) == 0 &&
             access(path.c_str(), mode) == 0;
    }
  }  // namespace

  std::uint64_t local_host_t::effective_uid() const {
    return static_cast<std::uint64_t>(geteuid());
  }

  bool local_host_t::executable_file(const std::filesystem::path &path) const {
    return accessible_as(path, S_IFREG, X_OK);
  }

  bool local_host_t::readable_directory(const std::filesystem::path &path) const {
    return accessible_as(path, S_IFDIR, R_OK | X_OK);
  }

  bool local_host_t::private_read_write_directory(
    const std::filesystem::path &path
  ) const {
    return private_accessible_as(path, S_IFDIR, R_OK | W_OK | X_OK);
  }

  bool local_host_t::private_readable_file(const std::filesystem::path &path) const {
    return private_accessible_as(path, S_IFREG, R_OK);
  }

  std::optional<character_device_identity_t>
  local_host_t::read_write_character_device(const std::filesystem::path &path) const {
    const auto descriptor = open(
      path.c_str(),
      O_PATH | O_CLOEXEC | O_NOFOLLOW
    );
    if (descriptor < 0) {
      return std::nullopt;
    }
    struct stat metadata {};
    const auto metadata_ready = fstat(descriptor, &metadata) == 0;
    const auto accessible = metadata_ready &&
                            faccessat(
                              descriptor,
                              "",
                              R_OK | W_OK,
                              AT_EMPTY_PATH | AT_EACCESS
                            ) == 0;
    close(descriptor);
    const auto character_major = metadata_ready ? major(metadata.st_rdev) : 0;
    const auto character_minor = metadata_ready ? minor(metadata.st_rdev) : 0;
    if (!metadata_ready || !accessible || !S_ISCHR(metadata.st_mode) ||
        character_major > std::numeric_limits<std::uint32_t>::max() ||
        character_minor > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    return character_device_identity_t {
      .filesystem_device = static_cast<std::uint64_t>(metadata.st_dev),
      .inode = static_cast<std::uint64_t>(metadata.st_ino),
      .character_major = static_cast<std::uint32_t>(character_major),
      .character_minor = static_cast<std::uint32_t>(character_minor),
    };
  }

  command_result_t local_host_t::run(
    const std::vector<std::string> &argv,
    std::chrono::milliseconds timeout,
    std::size_t max_output_bytes
  ) {
    auto result = platf::run_process_argv_capture(argv, timeout, max_output_bytes);
    return {
      .exit_status = result.exit_status,
      .timed_out = result.timed_out,
      .output_truncated = result.truncated,
      .output = std::move(result.output),
    };
  }

}  // namespace multiseat::podman

#endif
