/** @file src/platform/linux/encoder_probe_identity.h
 * Live Linux GPU and loaded-driver evidence. Unknown identities never hit.
 */
#pragma once

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <link.h>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

namespace platf::encoder_probe_identity {
  namespace fs = std::filesystem;

  inline std::optional<std::string> bounded_file(const fs::path &path, std::size_t limit = 4096) {
    // A provider manifest can be in a user-controlled XDG directory. Opening
    // nonblocking and checking the actual descriptor avoids a FIFO hang even
    // when a regular pathname is replaced between discovery and open.
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    struct owner_t { int fd; ~owner_t() { close(fd); } } owner {fd};
    struct stat status {};
    if (fstat(fd, &status) || !S_ISREG(status.st_mode)) return std::nullopt;
    std::string bytes(limit + 1, '\0');
    std::size_t count = 0;
    while (count < bytes.size()) {
      const auto result = read(fd, bytes.data() + count, bytes.size() - count);
      if (result < 0) {
        if (errno == EINTR) continue;
        return std::nullopt;
      }
      if (result == 0) break;
      count += static_cast<std::size_t>(result);
    }
    if (count == 0 || count > limit) return std::nullopt;
    bytes.resize(count);
    return bytes;
  }

  inline std::optional<std::string> file_identity(const fs::path &path, mode_t type) {
    struct stat status {};
    if (stat(path.c_str(), &status) || (status.st_mode & S_IFMT) != type) return std::nullopt;
    std::ostringstream out;
    out << status.st_dev << ':' << status.st_ino << ':' << status.st_rdev << ':'
        << status.st_size << ':' << status.st_mtim.tv_sec << ':' << status.st_mtim.tv_nsec
        << ':' << status.st_ctim.tv_sec << ':' << status.st_ctim.tv_nsec;
    return out.str();
  }

  struct gpu_driver_t {
    std::string gpu;
    std::string driver;
  };

  inline std::optional<gpu_driver_t> observe(std::string_view selected_render_node) {
    try {
      if (selected_render_node.empty()) return std::nullopt;
      std::vector<fs::path> nodes;
      for (const auto &entry : fs::directory_iterator("/dev/dri")) {
        if (entry.path().filename().string().starts_with("renderD")) nodes.push_back(entry.path());
      }
      if (nodes.empty() || nodes.size() > 64) return std::nullopt;
      std::sort(nodes.begin(), nodes.end());
      bool selected_present = false;
      std::ostringstream gpu, driver;
      struct utsname kernel {};
      if (uname(&kernel)) return std::nullopt;
      driver << std::quoted(kernel.release) << std::quoted(kernel.version);
      for (const auto &node : nodes) {
        const auto identity = file_identity(node, S_IFCHR);
        const auto device = fs::canonical(fs::path("/sys/class/drm") / node.filename() / "device");
        const auto vendor = bounded_file(device / "vendor");
        const auto device_id = bounded_file(device / "device");
        const auto revision = bounded_file(device / "revision");
        const auto module = fs::canonical(device / "driver/module");
        const auto source_version = bounded_file(module / "srcversion");
        if (!identity || !vendor || !device_id || !revision || !source_version) return std::nullopt;
        const auto version = bounded_file(module / "version");
        gpu << std::quoted(node.string()) << std::quoted(device.string()) << std::quoted(*identity)
            << std::quoted(*vendor) << std::quoted(*device_id) << std::quoted(*revision);
        driver << std::quoted(module.string()) << std::quoted(*source_version)
               << std::quoted(version.value_or("built-with-kernel"));
        selected_present |= fs::equivalent(node, fs::path(selected_render_node));
      }
      if (!selected_present) return std::nullopt;
      gpu << std::quoted(fs::canonical(fs::path(selected_render_node)).string());

      return gpu_driver_t {gpu.str(), driver.str()};
    } catch (...) {
      return std::nullopt;
    }
  }
}  // namespace platf::encoder_probe_identity
