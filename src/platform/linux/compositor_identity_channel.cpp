/**
 * @file src/platform/linux/compositor_identity_channel.cpp
 * @brief Supervisor → host compositor pidfd handoff and the pinned lease built from it.
 */
#include "compositor_identity_channel.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "vrr_clock_probe.h"

namespace compositor_identity {

  unique_fd_t &unique_fd_t::operator=(unique_fd_t &&other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) close(fd_);
      fd_ = other.release();
    }
    return *this;
  }

  unique_fd_t::~unique_fd_t() {
    if (fd_ >= 0) close(fd_);
  }

  namespace {
    template<class T>
    std::optional<T> parse_number(std::string_view text) {
      T value {};
      const auto *first = text.data();
      const auto *last = text.data() + text.size();
      const auto [end, error] = std::from_chars(first, last, value);
      if (error != std::errc {} || end != last || first == last) return std::nullopt;
      return value;
    }

    std::optional<std::string> read_small_file(const std::string &path) {
      // Owned, so an allocation failure while appending cannot leak the /proc descriptor.
      const unique_fd_t fd {open(path.c_str(), O_RDONLY | O_CLOEXEC)};
      if (!fd) return std::nullopt;
      std::array<char, 4096> buffer {};
      std::string text;
      while (text.size() < 65536) {
        const auto count = read(fd.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        text.append(buffer.data(), static_cast<std::size_t>(count));
      }
      if (text.empty()) return std::nullopt;
      return text;
    }

    int open_pidfd(pid_t pid) {
      return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    }
  }  // namespace

  std::optional<std::int64_t> parse_fdinfo_pid(std::string_view fdinfo) {
    std::optional<std::int64_t> found;
    std::size_t start = 0;
    while (start < fdinfo.size()) {
      auto end = fdinfo.find('\n', start);
      if (end == std::string_view::npos) end = fdinfo.size();
      auto line = fdinfo.substr(start, end - start);
      start = end + 1;
      if (!line.starts_with("Pid:")) continue;
      if (found) return std::nullopt;  // a second Pid line is not a pidfd we understand
      line.remove_prefix(4);
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
      found = parse_number<std::int64_t>(line);
      if (!found) return std::nullopt;
    }
    return found;
  }

  std::optional<std::int64_t> parse_ppid(std::string_view stat) {
    const auto close_paren = stat.rfind(')');
    if (close_paren == std::string_view::npos) return std::nullopt;
    std::size_t position = close_paren + 1;
    for (int field = 3; field <= 4; ++field) {
      while (position < stat.size() && stat[position] == ' ') ++position;
      const auto start = position;
      while (position < stat.size() && stat[position] != ' ' && stat[position] != '\n') ++position;
      if (start == position) return std::nullopt;
      if (field == 4) return parse_number<std::int64_t>(stat.substr(start, position - start));
    }
    return std::nullopt;
  }

  std::optional<std::int64_t> read_ppid(std::int64_t pid) {
    if (pid <= 0) return std::nullopt;
    const auto stat = read_small_file("/proc/" + std::to_string(pid) + "/stat");
    return stat ? parse_ppid(*stat) : std::nullopt;
  }

  std::optional<std::uint64_t> read_starttime(std::int64_t pid) {
    if (pid <= 0) return std::nullopt;
    const auto stat = read_small_file("/proc/" + std::to_string(pid) + "/stat");
    return stat ? vrr_clock::linux_probe::parse_starttime(*stat) : std::nullopt;
  }

  std::optional<std::int64_t> read_pidfd_pid(int pidfd) {
    if (pidfd < 0) return std::nullopt;
    const auto info = read_small_file("/proc/self/fdinfo/" + std::to_string(pidfd));
    return info ? parse_fdinfo_pid(*info) : std::nullopt;
  }

  bool pidfd_alive(int pidfd) {
    if (pidfd < 0) return false;
    pollfd process {pidfd, POLLIN, 0};
    int ready = 0;
    do {
      ready = poll(&process, 1, 0);
    } while (ready < 0 && errno == EINTR);
    return ready == 0;
  }

  // ---- Supervisor side ------------------------------------------------------------------------

  void report_exec_failure(int exec_status_write_fd, int error) noexcept {
    if (exec_status_write_fd < 0) return;
    const auto *bytes = reinterpret_cast<const char *>(&error);
    std::size_t written = 0;
    while (written < sizeof(error)) {
      const auto count = write(exec_status_write_fd, bytes + written, sizeof(error) - written);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return;
      written += static_cast<std::size_t>(count);
    }
  }

  exec_e await_exec(pid_t child, int exec_status_read_fd, const volatile sig_atomic_t *stop_requested,
                    std::chrono::milliseconds timeout) noexcept {
    if (child <= 0 || exec_status_read_fd < 0) return exec_e::failed;
    const auto stopping = [stop_requested] {
      return stop_requested && *stop_requested;
    };
    if (stopping()) return exec_e::interrupted;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      if (stopping()) return exec_e::interrupted;
      const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0) return exec_e::timed_out;
      // Short slices: a stop set without interrupting poll() is still seen promptly.
      pollfd status {exec_status_read_fd, POLLIN, 0};
      const int ready = poll(&status, 1, static_cast<int>(std::min<std::int64_t>(remaining, 50)));
      if (ready < 0) {
        if (errno == EINTR) continue;
        return exec_e::failed;
      }
      if (ready == 0) continue;
      int error = 0;
      const auto count = read(exec_status_read_fd, &error, sizeof(error));
      if (count > 0) return exec_e::failed;  // the child reported a failed exec
      if (count == 0) break;  // closed: by exec, or by the child's death
      if (errno == EINTR || errno == EAGAIN) continue;
      return exec_e::failed;
    }
    if (stopping()) return exec_e::interrupted;
    // A child that died before exec also closes the pipe. Look without reaping it.
    siginfo_t info {};
    if (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT) != 0) return exec_e::failed;
    return info.si_pid == 0 ? exec_e::succeeded : exec_e::failed;
  }

  payload_bytes_t encode_payload(const payload_t &payload) noexcept {
    payload_bytes_t bytes {};
    std::memcpy(bytes.data(), &payload.version, sizeof(payload.version));
    std::memcpy(bytes.data() + 4, &payload.reserved, sizeof(payload.reserved));
    std::memcpy(bytes.data() + 8, &payload.pid, sizeof(payload.pid));
    std::memcpy(bytes.data() + 16, &payload.starttime, sizeof(payload.starttime));
    return bytes;
  }

  payload_t decode_payload(const payload_bytes_t &bytes) noexcept {
    payload_t payload;
    std::memcpy(&payload.version, bytes.data(), sizeof(payload.version));
    std::memcpy(&payload.reserved, bytes.data() + 4, sizeof(payload.reserved));
    std::memcpy(&payload.pid, bytes.data() + 8, sizeof(payload.pid));
    std::memcpy(&payload.starttime, bytes.data() + 16, sizeof(payload.starttime));
    return payload;
  }

  bool send_after_exec(int channel_fd, pid_t child, int exec_status_read_fd,
                       const volatile sig_atomic_t *stop_requested,
                       std::chrono::milliseconds exec_timeout) noexcept {
    if (channel_fd < 0) return false;
    // No handoff on failure, stop or timeout.
    if (await_exec(child, exec_status_read_fd, stop_requested, exec_timeout) != exec_e::succeeded) return false;
    // The child is ours and unreaped, so its PID cannot have been recycled.
    const int pidfd = open_pidfd(child);
    if (pidfd < 0) return false;
    std::optional<std::uint64_t> starttime;
    try {
      starttime = read_starttime(child);
    } catch (...) {
      // Optional identity: an allocation failure only withholds it, never ends supervision.
    }
    if (!starttime || *starttime == 0) {
      close(pidfd);
      return false;
    }
    payload_t payload;
    payload.pid = child;
    payload.starttime = *starttime;
    auto bytes = encode_payload(payload);

    iovec body {bytes.data(), bytes.size()};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control {};
    msghdr message {};
    message.msg_iov = &body;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto *rights = CMSG_FIRSTHDR(&message);
    rights->cmsg_level = SOL_SOCKET;
    rights->cmsg_type = SCM_RIGHTS;
    rights->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(rights), &pidfd, sizeof(int));

    ssize_t sent = 0;
    do {
      sent = sendmsg(channel_fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (sent < 0 && errno == EINTR);
    close(pidfd);
    return sent == static_cast<ssize_t>(bytes.size());
  }

  // ---- Host side ------------------------------------------------------------------------------

  bool prepare_receiver(int receiver_fd) {
    const int enabled = 1;
    return receiver_fd >= 0 && setsockopt(receiver_fd, SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0;
  }

  namespace {
  #ifndef SCM_PIDFD
    constexpr int SCM_PIDFD = 0x04;  // Linux 6.5; installs a descriptor when SO_PASSPIDFD is set
  #endif

    /// Whether this control message carries descriptors the kernel installed in this process.
    bool carries_descriptors(const cmsghdr &header) {
      return header.cmsg_level == SOL_SOCKET && (header.cmsg_type == SCM_RIGHTS || header.cmsg_type == SCM_PIDFD) &&
             header.cmsg_len >= CMSG_LEN(0);
    }

    template<class F>
    void for_each_descriptor(const cmsghdr &header, F &&each) {
      const auto length = header.cmsg_len - CMSG_LEN(0);
      for (std::size_t offset = 0; offset + sizeof(int) <= length; offset += sizeof(int)) {
        int fd = -1;
        std::memcpy(&fd, CMSG_DATA(&header) + offset, sizeof(int));
        each(fd);
      }
    }
  }  // namespace

  ancillary_t take_ancillary(msghdr &message) {
    ancillary_t result;
    // Count first and reserve, so taking ownership below cannot throw while any descriptor is unowned.
    std::size_t rights = 0;
    std::size_t foreign = 0;
    for (auto *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
      if (!carries_descriptors(*header)) continue;
      (header->cmsg_type == SCM_RIGHTS ? rights : foreign) += (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    }
    try {
      result.descriptors.reserve(rights);
      result.foreign_descriptors.reserve(foreign);
    } catch (...) {
      // Cannot own them: close every received descriptor directly and refuse.
      for (auto *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (!carries_descriptors(*header)) continue;
        for_each_descriptor(*header, [](int fd) {
          if (fd >= 0) close(fd);
        });
      }
      result.unexpected = true;
      return result;
    }
    for (auto *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_len < CMSG_LEN(0)) {
        result.unexpected = true;
        continue;
      }
      const auto length = header->cmsg_len - CMSG_LEN(0);
      if (carries_descriptors(*header)) {
        auto &owned = header->cmsg_type == SCM_RIGHTS ? result.descriptors : result.foreign_descriptors;
        for_each_descriptor(*header, [&owned](int fd) {
          owned.emplace_back(fd);
        });
        if (header->cmsg_type != SCM_RIGHTS || length % sizeof(int) != 0) result.unexpected = true;
      } else if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_CREDENTIALS) {
        ++result.credential_messages;
        if (length != sizeof(ucred)) {
          result.unexpected = true;
          continue;
        }
        ucred value {};
        std::memcpy(&value, CMSG_DATA(header), sizeof(value));
        result.credentials = value;
      } else {
        result.unexpected = true;  // e.g. SCM_SECURITY or anything unknown
      }
    }
    return result;
  }

  receive_result_t receive(int receiver_fd, std::int64_t supervisor_pid, std::uint32_t expected_uid,
                           std::chrono::milliseconds timeout) {
    if (receiver_fd < 0 || supervisor_pid <= 1) return {std::nullopt, "identity_channel_unavailable"};
    pollfd channel {receiver_fd, POLLIN, 0};
    int ready = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      ready = poll(&channel, 1, static_cast<int>(std::max<std::int64_t>(0, remaining.count())));
      if (ready < 0 && errno == EINTR) continue;
      break;
    }
    if (ready == 0) return {std::nullopt, "identity_channel_timeout"};
    if (ready < 0) return {std::nullopt, "identity_channel_unavailable"};

    // One byte more than the payload, and room for extra descriptors and credentials, so excess is
    // seen (and every descriptor closed) rather than silently truncated away.
    std::array<unsigned char, k_payload_size + 8> buffer {};
    iovec body {buffer.data(), buffer.size()};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 8) + CMSG_SPACE(sizeof(ucred)) * 2> control {};
    msghdr message {};
    message.msg_iov = &body;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    std::vector<unique_fd_t> descriptors;  // every received descriptor, closed on any refusal
    std::optional<ucred> credentials;
    unsigned credential_messages = 0;
    bool unexpected_ancillary = false;
    ssize_t count = 0;
    do {
      count = recvmsg(receiver_fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    } while (count < 0 && errno == EINTR);

    if (count >= 0) {
      auto taken = take_ancillary(message);
      descriptors = std::move(taken.descriptors);
      credentials = taken.credentials;
      credential_messages = taken.credential_messages;
      unexpected_ancillary = taken.unexpected;
    }
    if (count < 0) return {std::nullopt, "identity_channel_unavailable"};
    if (count == 0) return {std::nullopt, "identity_channel_closed"};
    if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return {std::nullopt, "identity_message_truncated"};
    if (unexpected_ancillary) return {std::nullopt, "identity_ancillary_unexpected"};
    if (count != static_cast<ssize_t>(k_payload_size)) return {std::nullopt, "identity_payload_size"};
    if (credential_messages != 1 || !credentials || credentials->pid != supervisor_pid ||
        credentials->uid != expected_uid) {
      return {std::nullopt, "identity_sender_mismatch"};
    }
    if (descriptors.size() != 1) return {std::nullopt, "identity_descriptor_count"};

    payload_bytes_t bytes {};
    std::copy_n(buffer.begin(), bytes.size(), bytes.begin());
    const auto payload = decode_payload(bytes);
    if (payload.version != k_payload_version || payload.reserved != 0) return {std::nullopt, "identity_payload_version"};
    if (payload.pid <= 1 || payload.starttime == 0) return {std::nullopt, "identity_payload_invalid"};

    auto &pidfd = descriptors.front();
    const auto kernel_pid = read_pidfd_pid(pidfd.get());
    if (!kernel_pid) return {std::nullopt, "identity_descriptor_not_pidfd"};
    if (*kernel_pid != payload.pid) return {std::nullopt, "identity_pidfd_mismatch"};
    if (!pidfd_alive(pidfd.get())) return {std::nullopt, "identity_compositor_exited"};
    if (read_ppid(payload.pid) != supervisor_pid) return {std::nullopt, "identity_not_supervisor_child"};
    const auto first = read_starttime(payload.pid);
    const auto second = read_starttime(payload.pid);
    if (!first || !second || *first != payload.starttime || *second != payload.starttime) {
      return {std::nullopt, "identity_starttime_mismatch"};
    }
    // Recheck the descriptor still names that live process after the /proc reads.
    if (read_pidfd_pid(pidfd.get()) != payload.pid || !pidfd_alive(pidfd.get())) {
      return {std::nullopt, "identity_compositor_exited"};
    }
    received_t accepted;
    accepted.pidfd = std::move(pidfd);
    accepted.process = {payload.pid, expected_uid, payload.starttime};
    return {std::move(accepted), "identity_accepted"};
  }

  pinned_lease_t::pinned_lease_t(received_t compositor, unique_fd_t supervisor_pidfd, std::string socket_path,
                                 unique_fd_t socket_fd, std::uint64_t socket_dev, std::uint64_t socket_ino,
                                 published_t published):
      compositor_lease_t(compositor.process),
      compositor_pidfd_ {std::move(compositor.pidfd)},
      supervisor_pidfd_ {std::move(supervisor_pidfd)},
      socket_path_ {std::move(socket_path)},
      socket_fd_ {std::move(socket_fd)},
      socket_dev_ {socket_dev},
      socket_ino_ {socket_ino},
      published_ {std::move(published)} {
  }

  bool pinned_lease_t::current() const {
    if (revoked_.load()) return false;
    // Not published (yet, or any more), or a writer is active: false, but nothing was observed wrong
    // with this lease's own kernel objects, so it is not latched.
    if (!published_ || !published_(*this)) return false;
    const auto revoke = [this] {
      revoked_.store(true);
      return false;
    };
    if (!pidfd_alive(compositor_pidfd_.get()) || !pidfd_alive(supervisor_pidfd_.get())) return revoke();
    struct stat pinned {}, named {};
    if (fstat(socket_fd_.get(), &pinned) != 0 || lstat(socket_path_.c_str(), &named) != 0) return revoke();
    if ((named.st_mode & S_IFMT) != S_IFSOCK || static_cast<std::uint64_t>(pinned.st_dev) != socket_dev_ ||
        static_cast<std::uint64_t>(pinned.st_ino) != socket_ino_ ||
        static_cast<std::uint64_t>(named.st_dev) != socket_dev_ ||
        static_cast<std::uint64_t>(named.st_ino) != socket_ino_) {
      // A replaced or unavailable endpoint revokes for good: putting the original name back later
      // cannot prove nobody else answered on it meanwhile.
      return revoke();
    }
    // Recheck publication after the reads, so a retirement racing them is never missed.
    return published_(*this) && !revoked_.load();
  }

  std::shared_ptr<const pinned_lease_t> make_pinned_lease(received_t compositor, int supervisor_pidfd,
                                                          const std::string &socket_path,
                                                          pinned_lease_t::published_t published) {
    if (!compositor.pidfd || supervisor_pidfd < 0 || socket_path.empty() || !published) return nullptr;
    unique_fd_t supervisor {fcntl(supervisor_pidfd, F_DUPFD_CLOEXEC, 0)};
    unique_fd_t socket {open(socket_path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC)};
    if (!supervisor || !socket) return nullptr;
    struct stat pinned {}, named {};
    if (fstat(socket.get(), &pinned) != 0 || lstat(socket_path.c_str(), &named) != 0 ||
        (pinned.st_mode & S_IFMT) != S_IFSOCK || pinned.st_dev != named.st_dev || pinned.st_ino != named.st_ino) {
      return nullptr;
    }
    return std::make_shared<const pinned_lease_t>(
      std::move(compositor), std::move(supervisor), socket_path, std::move(socket),
      static_cast<std::uint64_t>(pinned.st_dev), static_cast<std::uint64_t>(pinned.st_ino), std::move(published)
    );
  }

}  // namespace compositor_identity
