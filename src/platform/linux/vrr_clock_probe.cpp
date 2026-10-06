/**
 * @file src/platform/linux/vrr_clock_probe.cpp
 * @brief Read-only kernel facts for the capture clock proof.
 */
#include "vrr_clock_probe.h"

#include <chrono>
#include <charconv>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace vrr_clock::linux_probe {

  std::optional<std::uint64_t> parse_starttime(std::string_view stat) {
    const auto close = stat.rfind(')');
    if (close == std::string_view::npos) return std::nullopt;
    // Fields after comm start at 3 (state). starttime is field 22, so it is the 20th after ')'.
    std::size_t position = close + 1;
    for (int field = 3; field <= 22; ++field) {
      while (position < stat.size() && stat[position] == ' ') ++position;
      const auto start = position;
      while (position < stat.size() && stat[position] != ' ' && stat[position] != '\n') ++position;
      if (start == position) return std::nullopt;
      if (field == 22) {
        std::uint64_t value = 0;
        const auto *first = stat.data() + start;
        const auto *last = stat.data() + position;
        const auto [end, error] = std::from_chars(first, last, value);
        if (error != std::errc {} || end != last) return std::nullopt;
        return value;
      }
    }
    return std::nullopt;
  }

  namespace {
    /** k_failed_read rather than a successful-looking zero when the read fails or cannot be represented. */
    std::int64_t clock_ns(clockid_t clock) {
      timespec value {};
      if (clock_gettime(clock, &value) != 0 || value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1'000'000'000L ||
          value.tv_sec > (k_int64_max - value.tv_nsec) / 1'000'000'000LL) {
        return k_failed_read;
      }
      return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000LL + value.tv_nsec;
    }

    std::string proc_path(std::int64_t pid, std::string_view leaf) {
      return (pid == 0 ? std::string {"/proc/self"} : "/proc/" + std::to_string(pid)) + "/" + std::string {leaf};
    }

    class kernel_system_t final: public system_t {
    public:
      std::optional<peer_cred_t> peer_credentials(int fd) override {
        ucred peer {};
        socklen_t size = sizeof(peer);
        if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || size != sizeof(peer)) {
          return std::nullopt;
        }
        return peer_cred_t {.pid = peer.pid, .uid = peer.uid};
      }

      std::optional<ns_id_t> time_namespace(std::int64_t pid) override {
        // Open, then fstat the descriptor, so dev and inode describe exactly what was opened.
        const int fd = open(proc_path(pid, "ns/time").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return std::nullopt;
        struct stat info {};
        const bool ok = fstat(fd, &info) == 0;
        close(fd);
        if (!ok) return std::nullopt;
        return ns_id_t {.dev = static_cast<std::uint64_t>(info.st_dev), .ino = static_cast<std::uint64_t>(info.st_ino)};
      }

      std::optional<std::uint64_t> process_starttime(std::int64_t pid) override {
        if (pid <= 0) return std::nullopt;
        const int fd = open(proc_path(pid, "stat").c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return std::nullopt;
        char buffer[1024];
        const auto count = read(fd, buffer, sizeof(buffer));
        close(fd);
        if (count <= 0) return std::nullopt;
        return parse_starttime(std::string_view {buffer, static_cast<std::size_t>(count)});
      }

      std::uint32_t current_uid() override {
        return static_cast<std::uint32_t>(getuid());
      }

      probe_sample_t sample_clocks() override {
        probe_sample_t sample;
        sample.mono_before_ns = clock_ns(CLOCK_MONOTONIC);
        const auto steady = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()
        ).count();
        sample.steady_ns = steady >= 0 ? static_cast<std::int64_t>(steady) : k_failed_read;
        sample.mono_after_ns = clock_ns(CLOCK_MONOTONIC);
        return sample;
      }

      std::int64_t monotonic_now_ns() override {
        return clock_ns(CLOCK_MONOTONIC);
      }
    };
  }  // namespace

  system_t &real_system() {
    static kernel_system_t system;
    return system;
  }

}  // namespace vrr_clock::linux_probe
