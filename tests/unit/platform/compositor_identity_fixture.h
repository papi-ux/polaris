/**
 * @file tests/unit/platform/compositor_identity_fixture.h
 * @brief A controlled stand-in compositor for the identity handoff: the test process plays the
 *        supervisor, forks a child that optionally listens on a real UNIX socket, and execs it.
 *
 * Isolated: nothing here touches the live private runtime, its globals or any real compositor.
 */
#pragma once

#ifdef __linux__

  #include <src/platform/linux/compositor_identity_channel.h>

  #include <cerrno>
  #include <chrono>
  #include <csignal>
  #include <cstring>
  #include <dirent.h>
  #include <fcntl.h>
  #include <filesystem>
  #include <poll.h>
  #include <string>
  #include <sys/socket.h>
  #include <sys/syscall.h>
  #include <sys/un.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>

namespace compositor_identity_fixture {

  /** Open descriptors of this process, for leak checks. */
  inline std::size_t open_descriptor_count() {
    std::size_t count = 0;
    if (auto *directory = opendir("/proc/self/fd")) {
      while (const auto *entry = readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
      }
      closedir(directory);
    }
    return count > 0 ? count - 1 : 0;  // minus the directory's own descriptor
  }

  inline int open_pidfd(pid_t pid) {
    return static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
  }

  /** Wait until a pidfd reports its process exited. */
  inline bool wait_for_exit(int pidfd, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    pollfd process {pidfd, POLLIN, 0};
    return poll(&process, 1, static_cast<int>(timeout.count())) == 1;
  }

  /// A unique runtime directory under the system temp directory, removed on destruction.
  struct scratch_dir_t {
    std::filesystem::path path;

    scratch_dir_t() {
      static int sequence = 0;
      path = std::filesystem::temp_directory_path() /
             ("polaris-identity-" + std::to_string(getpid()) + "-" + std::to_string(++sequence));
      std::filesystem::create_directories(path);
    }

    ~scratch_dir_t() {
      std::error_code ignored;
      std::filesystem::remove_all(path, ignored);
    }
  };

  /** Bind and listen on path, inheritable across exec (only this owned listener). */
  inline int listen_inheritable(const std::string &path) {
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return -1;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 4) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  /** A connected client of path, CLOEXEC; -1 on failure. */
  inline int connect_to(const std::string &path) {
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) return -1;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
      close(fd);
      return -1;
    }
    return fd;
  }

  /**
   * The test process as supervisor: a SEQPACKET channel with SO_PASSCRED on the receiving end, and a
   * direct child launched through the production exec-status protocol.
   */
  class controlled_child_t {
  public:
    enum class mode_e {
      exec,  ///< exec program
      die_before_exec,  ///< exit without exec and without reporting
    };

    struct options_t {
      const char *program = "/bin/sleep";
      std::string listen_path;  ///< nonempty: the child listens there before exec, as labwc does
      mode_e mode = mode_e::exec;
      bool send = true;  ///< run the production send_after_exec once the child is launched
      bool close_sender = true;  ///< close the supervisor end afterwards, as the supervisor does
    };

    controlled_child_t(): controlled_child_t(options_t {}) {}

    explicit controlled_child_t(options_t options) {
      int channel[2] {-1, -1};
      if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) != 0) return;
      receiver = compositor_identity::unique_fd_t {channel[0]};
      sender = compositor_identity::unique_fd_t {channel[1]};
      if (!compositor_identity::prepare_receiver(receiver.get())) return;

      int exec_status[2] {-1, -1};
      if (pipe2(exec_status, O_CLOEXEC) != 0) return;
      const char *program = options.program;
      const char *argument = "60";
      const bool listening = !options.listen_path.empty();
      sockaddr_un address {};
      address.sun_family = AF_UNIX;
      if (listening) {
        if (options.listen_path.size() >= sizeof(address.sun_path)) return;
        std::memcpy(address.sun_path, options.listen_path.c_str(), options.listen_path.size() + 1);
      }

      pid = fork();
      if (pid == 0) {
        // Only async-signal-safe calls between fork and exec.
        if (listening) {
          const int listener = socket(AF_UNIX, SOCK_STREAM, 0);  // deliberately inheritable
          if (listener < 0 || bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
              listen(listener, 4) != 0) {
            _exit(125);
          }
        }
        if (options.mode == mode_e::die_before_exec) _exit(0);
        execl(program, program, argument, static_cast<char *>(nullptr));
        const int error = errno;
        compositor_identity::report_exec_failure(exec_status[1], error);
        _exit(127);
      }
      close(exec_status[1]);
      if (pid < 0) {
        close(exec_status[0]);
        return;
      }
      pidfd = compositor_identity::unique_fd_t {open_pidfd(pid)};
      // A child dying before exec closes the pipe before it becomes waitable; let it finish dying so
      // the check is deterministic. (Production refuses that window later: the pidfd is not alive.)
      if (options.mode == mode_e::die_before_exec) (void) wait_for_exit(pidfd.get());
      if (options.send) {
        sent = compositor_identity::send_after_exec(sender.get(), pid, exec_status[0], nullptr);
      } else {
        exec_result = compositor_identity::await_exec(pid, exec_status[0], nullptr);
      }
      close(exec_status[0]);
      if (options.close_sender) sender = compositor_identity::unique_fd_t {};
    }

    ~controlled_child_t() {
      if (pid > 0) {
        (void) kill(pid, SIGKILL);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      }
    }

    controlled_child_t(const controlled_child_t &) = delete;
    controlled_child_t &operator=(const controlled_child_t &) = delete;

    compositor_identity::receive_result_t receive(std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
      return compositor_identity::receive(receiver.get(), getpid(), static_cast<std::uint32_t>(getuid()), timeout);
    }

    pid_t pid = -1;
    compositor_identity::unique_fd_t pidfd;
    compositor_identity::unique_fd_t receiver;
    compositor_identity::unique_fd_t sender;
    bool sent = false;
    compositor_identity::exec_e exec_result = compositor_identity::exec_e::failed;
  };

}  // namespace compositor_identity_fixture

#endif  // __linux__
