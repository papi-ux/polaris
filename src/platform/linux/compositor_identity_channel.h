/**
 * @file src/platform/linux/compositor_identity_channel.h
 * @brief Hand the private compositor's own pidfd from its supervisor to the host, and pin it as a lease.
 *
 * The host spawns a supervisor, which forks the child that execs labwc. Only the supervisor knows that
 * exact child before any reaping, so it opens a pidfd for it once the child has provably exec'd and
 * passes it over a private inherited channel (fixed FD 5). The host accepts exactly one live pidfd,
 * sent with SCM_CREDENTIALS of the tracked supervisor and this user, whose kernel Pid, parent and
 * start time all match. Anything else is refused and every received descriptor is closed.
 *
 * Diagnostic authority only: an unavailable identity leaves ordinary fixed-pacing startup unchanged,
 * and a lease never signals or otherwise touches any process. No PID-by-name scans are made.
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <vector>

#include "src/vrr_compositor_identity.h"

namespace compositor_identity {

  /// The fixed child descriptor of the identity channel; 3 and 4 stay the startup diagnostics.
  inline constexpr int k_channel_fd = 5;
  /// The supervisor argument that enables the channel; without it FD 5 is never trusted.
  inline constexpr std::string_view k_supervisor_flag = "--identity-fd=5";
  inline constexpr std::uint32_t k_payload_version = 1;

  /** The decoded message body; reserved must be zero. */
  struct payload_t {
    std::uint32_t version = k_payload_version;
    std::uint32_t reserved = 0;
    std::int64_t pid = 0;
    std::uint64_t starttime = 0;
  };

  /// The wire size: version, reserved, pid, starttime as fixed native-endian fields, no padding.
  inline constexpr std::size_t k_payload_size = 24;
  using payload_bytes_t = std::array<unsigned char, k_payload_size>;

  /** Serializes field by field, so no byte on the wire comes from struct padding. */
  payload_bytes_t encode_payload(const payload_t &payload) noexcept;
  payload_t decode_payload(const payload_bytes_t &bytes) noexcept;

  /** An owned descriptor, closed on destruction. */
  class unique_fd_t {
  public:
    unique_fd_t() = default;
    explicit unique_fd_t(int fd): fd_ {fd} {}
    unique_fd_t(unique_fd_t &&other) noexcept: fd_ {other.release()} {}
    unique_fd_t &operator=(unique_fd_t &&other) noexcept;
    unique_fd_t(const unique_fd_t &) = delete;
    unique_fd_t &operator=(const unique_fd_t &) = delete;
    ~unique_fd_t();

    int get() const { return fd_; }
    int release() {
      const int fd = fd_;
      fd_ = -1;
      return fd;
    }
    explicit operator bool() const { return fd_ >= 0; }

  private:
    int fd_ = -1;
  };

  /// The `Pid:` line of a /proc/self/fdinfo entry; nullopt when absent (not a pidfd) or malformed.
  std::optional<std::int64_t> parse_fdinfo_pid(std::string_view fdinfo);
  /// Field 4 (ppid) of /proc/<pid>/stat text, parsed after the last ')'.
  std::optional<std::int64_t> parse_ppid(std::string_view stat);
  std::optional<std::int64_t> read_ppid(std::int64_t pid);
  std::optional<std::uint64_t> read_starttime(std::int64_t pid);
  std::optional<std::int64_t> read_pidfd_pid(int pidfd);
  /// A pidfd polls readable once its process exits; a live one does not.
  bool pidfd_alive(int pidfd);

  // ---- Supervisor side ------------------------------------------------------------------------

  /** In the forked child, after a failed exec: report errno through the CLOEXEC status pipe. */
  void report_exec_failure(int exec_status_write_fd, int error) noexcept;

  enum class exec_e {
    succeeded,  ///< the pipe closed by exec and the child has not exited
    failed,  ///< the child reported an exec failure, exited, or the pipe broke
    interrupted,  ///< a supervisor stop was requested before or while waiting
    timed_out,  ///< the pipe stayed open past the deadline
  };

  /// How long the supervisor waits for its child's exec before giving up on the optional handoff.
  inline constexpr std::chrono::milliseconds k_exec_wait {2000};

  /**
   * Wait, at most `timeout`, for the child's exec through a CLOEXEC status pipe. Stop is checked
   * before waiting and at least every 50 ms while waiting. EOF alone is not success: a child that died
   * before exec also closes the pipe, so its unreaped state is checked without reaping it.
   */
  exec_e await_exec(pid_t child, int exec_status_read_fd, const volatile sig_atomic_t *stop_requested,
                    std::chrono::milliseconds timeout = k_exec_wait) noexcept;

  /**
   * After proven exec: open the child's pidfd, read its start time and send one payload with the pidfd
   * as the only SCM_RIGHTS descriptor. Never blocks; any failure sends nothing.
   * @return Whether a message was sent. The caller closes channel_fd either way.
   */
  bool send_after_exec(int channel_fd, pid_t child, int exec_status_read_fd,
                       const volatile sig_atomic_t *stop_requested,
                       std::chrono::milliseconds exec_timeout = k_exec_wait) noexcept;

  // ---- Host side ------------------------------------------------------------------------------

  /** Enable SO_PASSCRED on the receiving end before the supervisor can send. */
  bool prepare_receiver(int receiver_fd);

  /**
   * Every control message of one received message. Each SCM_RIGHTS and SCM_PIDFD descriptor is owned
   * here, so all are closed with it. `unexpected` is set for any level/type other than SOL_SOCKET SCM_RIGHTS and
   * SCM_CREDENTIALS, a credential of the wrong length, or a rights payload not a whole number of
   * descriptors. Takes ownership even when unexpected.
   */
  struct ancillary_t {
    std::vector<unique_fd_t> descriptors;
    /// Descriptors the kernel installed outside SCM_RIGHTS (SCM_PIDFD); always unexpected, always closed.
    std::vector<unique_fd_t> foreign_descriptors;
    std::optional<ucred> credentials;
    unsigned credential_messages = 0;
    bool unexpected = false;
  };
  ancillary_t take_ancillary(msghdr &message);

  struct received_t {
    unique_fd_t pidfd;
    vrr_clock::compositor_process_t process;  ///< uid is host-derived, never from the payload
  };

  struct receive_result_t {
    std::optional<received_t> identity;
    std::string_view reason;  ///< fixed refusal name; never a runtime string
  };

  /**
   * Receive one handoff within timeout. Requires exactly the payload size, a known version and zero
   * reserved bits, one SCM_CREDENTIALS of supervisor_pid and expected_uid, and exactly one descriptor
   * that is a live pidfd whose kernel Pid is the payload pid, whose process is supervisor_pid's direct
   * child, and whose start time matches the payload on two reads. Every received descriptor is closed
   * on refusal; the accepted one is CLOEXEC.
   */
  receive_result_t receive(int receiver_fd, std::int64_t supervisor_pid, std::uint32_t expected_uid,
                           std::chrono::milliseconds timeout);

  /**
   * A compositor lease pinned to kernel objects. current() requires the caller's publication check,
   * a live compositor and supervisor, and an unchanged socket name → object (dev and inode), and repeats
   * the publication check after those reads, so a retired or replaced generation never qualifies.
   *
   * Observed kernel/socket invalidity (an exited process, an unreadable or replaced socket) revokes
   * the lease irrevocably: restoring the original name later cannot reauthorize it. A publication
   * check that is merely false (writer active, not yet or no longer published) is never latched.
   */
  class pinned_lease_t final: public vrr_clock::compositor_lease_t {
  public:
    using published_t = std::function<bool(const vrr_clock::compositor_lease_t &)>;

    pinned_lease_t(received_t compositor, unique_fd_t supervisor_pidfd, std::string socket_path,
                   unique_fd_t socket_fd, std::uint64_t socket_dev, std::uint64_t socket_ino, published_t published);

    bool current() const override;

  private:
    unique_fd_t compositor_pidfd_;
    unique_fd_t supervisor_pidfd_;
    std::string socket_path_;
    unique_fd_t socket_fd_;
    std::uint64_t socket_dev_;
    std::uint64_t socket_ino_;
    published_t published_;
    mutable std::atomic<bool> revoked_ {false};
  };

  /**
   * Pin a lease: duplicates supervisor_pidfd, opens the socket path O_PATH and requires it to be the
   * same socket object the name names now. nullptr when anything is unavailable.
   */
  std::shared_ptr<const pinned_lease_t> make_pinned_lease(received_t compositor, int supervisor_pidfd,
                                                          const std::string &socket_path,
                                                          pinned_lease_t::published_t published);

}  // namespace compositor_identity
