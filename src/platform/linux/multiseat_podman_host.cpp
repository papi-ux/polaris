/**
 * @file src/platform/linux/multiseat_podman_host.cpp
 * @brief Live Linux host adapter for the rootless Podman worker backend.
 */
#include "multiseat_podman_host.h"

#ifdef __linux__

#include "misc.h"

#include <sys/stat.h>
#include <unistd.h>

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

  bool local_host_t::read_write_character_device(const std::filesystem::path &path) const {
    return accessible_as(path, S_IFCHR, R_OK | W_OK);
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
