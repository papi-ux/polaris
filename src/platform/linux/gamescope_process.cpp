/**
 * @file src/platform/linux/gamescope_process.cpp
 * @brief Exact-generation gamescope ownership and XWayland discovery helpers.
 */
#include "gamescope_process.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace stream_runtime::gamescope_process {
  namespace {
    namespace fs = std::filesystem;

    struct process_t {
      int pid = 0;
      int ppid = 0;
      int pgid = 0;
      int session = 0;
      std::uint64_t start_time = 0;
      fs::path executable;
      std::vector<std::string> argv;
    };

    class locked_fd_t {
    public:
      explicit locked_fd_t(int fd):
          fd_(fd) {}

      locked_fd_t(const locked_fd_t &) = delete;
      locked_fd_t &operator=(const locked_fd_t &) = delete;
      locked_fd_t(locked_fd_t &&other) noexcept:
          fd_(std::exchange(other.fd_, -1)) {}

      ~locked_fd_t() {
        if (fd_ >= 0) {
          close(fd_);
        }
      }

      bool still_names(const fs::path &path) const {
        struct stat fd_stat {};
        struct stat path_stat {};
        return fd_ >= 0 && fstat(fd_, &fd_stat) == 0 && lstat(path.c_str(), &path_stat) == 0 &&
               S_ISREG(fd_stat.st_mode) && S_ISREG(path_stat.st_mode) &&
               fd_stat.st_dev == path_stat.st_dev && fd_stat.st_ino == path_stat.st_ino;
      }

      bool still_names_node(const fs::path &path) const {
        struct stat fd_stat {};
        struct stat path_stat {};
        return fd_ >= 0 && fstat(fd_, &fd_stat) == 0 && lstat(path.c_str(), &path_stat) == 0 &&
               !S_ISLNK(path_stat.st_mode) &&
               fd_stat.st_dev == path_stat.st_dev && fd_stat.st_ino == path_stat.st_ino &&
               fd_stat.st_mode == path_stat.st_mode;
      }

    private:
      int fd_;
    };

    bool process_holds_deleted_path(const fs::path &proc_root, const fs::path &path, uid_t owner) {
      const auto expected = path.string() + " (deleted)";
      std::error_code ec;
      for (const auto &process : fs::directory_iterator(proc_root, ec)) {
        if (ec) {
          return true;
        }
        const auto name = process.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
          continue;
        }
        struct stat process_stat {};
        if (lstat(process.path().c_str(), &process_stat) != 0) {
          return true;
        }
        if (process_stat.st_uid != owner) {
          continue;
        }
        std::error_code fd_ec;
        for (const auto &entry : fs::directory_iterator(process.path() / "fd", fd_ec)) {
          if (fd_ec) {
            return true;
          }
          std::error_code link_ec;
          const auto target = fs::read_symlink(entry.path(), link_ec);
          if (link_ec) {
            if (link_ec == std::errc::no_such_file_or_directory) {
              continue;
            }
            return true;
          }
          if (target == expected) {
            return true;
          }
        }
        if (fd_ec) {
          return true;
        }
      }
      if (ec) {
        return true;
      }
      return false;
    }

    bool deleted_lock_state_unsafe(const fs::path &proc_root, const fs::path &lock_path) {
      struct stat lock_stat {};
      if (lstat(lock_path.c_str(), &lock_stat) != 0 || !S_ISREG(lock_stat.st_mode)) {
        return true;
      }
      return process_holds_deleted_path(proc_root, lock_path, lock_stat.st_uid);
    }

    std::optional<locked_fd_t> lock_wayland_socket_path(
      const fs::path &socket_path,
      const lookup_paths_t &paths
    ) {
      const fs::path lock_path {socket_path.string() + ".lock"};
      if (deleted_lock_state_unsafe(paths.proc_root, lock_path)) {
        return std::nullopt;
      }
      const int fd = open(lock_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0) {
        return std::nullopt;
      }
      locked_fd_t locked {fd};
      if (!locked.still_names(lock_path) || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        return std::nullopt;
      }
      if (!locked.still_names(lock_path) || deleted_lock_state_unsafe(paths.proc_root, lock_path)) {
        return std::nullopt;
      }
      return locked;
    }

    template<typename T>
    std::optional<T> parse_integer(std::string_view value) {
      T parsed {};
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (ec != std::errc {} || end != value.data() + value.size()) {
        return std::nullopt;
      }
      return parsed;
    }

    std::vector<std::string> split_words(std::string_view value) {
      std::istringstream input(std::string {value});
      std::vector<std::string> words;
      for (std::string word; input >> word;) {
        words.push_back(std::move(word));
      }
      return words;
    }

    bool gamescope_executable_name(const fs::path &path) {
      const auto name = path.filename();
      return name == "gamescope" || name == ".gamescope-wrapped";
    }

    std::optional<fs::path> read_process_executable(
      const lookup_paths_t &paths,
      int pid,
      const std::vector<std::string> &argv
    ) {
      std::error_code executable_ec;
      std::optional<fs::path> executable;
      if (paths.read_executable_for_tests) {
        executable = paths.read_executable_for_tests(pid, executable_ec);
      }
      else {
        auto path = fs::read_symlink(
          paths.proc_root / std::to_string(pid) / "exe",
          executable_ec
        );
        if (!executable_ec) {
          executable = std::move(path);
        }
      }
      if (executable && !executable->empty()) {
        return executable;
      }
      if (executable_ec != std::errc::permission_denied || argv.empty()) {
        return std::nullopt;
      }

      // Executing a file-capability gamescope can make it non-dumpable. Linux
      // then denies a same-user parent readlink(/proc/<pid>/exe), even though
      // stat and cmdline remain readable. The launcher supplies an absolute
      // argv[0]; pin its canonical executable only for this procfs denial.
      const fs::path requested {argv.front()};
      if (!requested.is_absolute()) {
        return std::nullopt;
      }
      std::error_code canonical_ec;
      const auto canonical = fs::canonical(requested, canonical_ec);
      if (canonical_ec || !gamescope_executable_name(canonical)) {
        return std::nullopt;
      }
      struct stat executable_stat {};
      if (stat(canonical.c_str(), &executable_stat) != 0 || !S_ISREG(executable_stat.st_mode) ||
          access(canonical.c_str(), X_OK) != 0) {
        return std::nullopt;
      }
      struct stat process_stat {};
      if (stat((paths.proc_root / std::to_string(pid)).c_str(), &process_stat) != 0 ||
          process_stat.st_uid != getuid()) {
        return std::nullopt;
      }
      return canonical;
    }

    std::optional<process_t> read_process(const lookup_paths_t &paths, int pid) {
      if (pid <= 0) {
        return std::nullopt;
      }

      std::ifstream stat_file(paths.proc_root / std::to_string(pid) / "stat");
      std::string stat;
      if (!std::getline(stat_file, stat)) {
        return std::nullopt;
      }
      // comm is parenthesized and may contain spaces or ')'; fields after its last
      // ") " begin with state (field 3).
      const auto comm_end = stat.rfind(") ");
      if (comm_end == std::string::npos) {
        return std::nullopt;
      }
      const auto fields = split_words(std::string_view {stat}.substr(comm_end + 2));
      if (fields.size() < 20) {
        return std::nullopt;
      }
      const auto ppid = parse_integer<int>(fields[1]);
      const auto pgid = parse_integer<int>(fields[2]);
      const auto session = parse_integer<int>(fields[3]);
      const auto start_time = parse_integer<std::uint64_t>(fields[19]);
      if (!ppid || !pgid || !session || !start_time || *start_time == 0 ||
          *pgid <= 1 || *session <= 1) {
        return std::nullopt;
      }

      std::ifstream cmdline_file(paths.proc_root / std::to_string(pid) / "cmdline", std::ios::binary);
      if (!cmdline_file) {
        return std::nullopt;
      }
      const std::string bytes {
        std::istreambuf_iterator<char> {cmdline_file},
        std::istreambuf_iterator<char> {},
      };
      std::vector<std::string> argv;
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto end = bytes.find('\0', offset);
        const auto length = (end == std::string::npos ? bytes.size() : end) - offset;
        if (length > 0) {
          argv.emplace_back(bytes.substr(offset, length));
        }
        if (end == std::string::npos) {
          break;
        }
        offset = end + 1;
      }
      if (argv.empty()) {
        return std::nullopt;
      }

      const auto executable = read_process_executable(paths, pid, argv);
      if (!executable) {
        return std::nullopt;
      }

      return process_t {
        .pid = pid,
        .ppid = *ppid,
        .pgid = *pgid,
        .session = *session,
        .start_time = *start_time,
        .executable = *executable,
        .argv = std::move(argv),
      };
    }

    bool executable_named(const process_t &process, std::string_view expected) {
      if (process.argv.empty()) {
        return false;
      }
      if (fs::path(process.argv.front()).filename() != expected) {
        return false;
      }
      return expected == "gamescope" ? gamescope_executable_name(process.executable) :
                                        process.executable.filename() == expected;
    }

    bool is_headless_gamescope(const process_t &process) {
      if (!executable_named(process, "gamescope")) {
        return false;
      }
      for (std::size_t i = 1; i < process.argv.size(); ++i) {
        if (process.argv[i] == "--backend=headless") {
          return true;
        }
        if (process.argv[i] == "--backend" && i + 1 < process.argv.size() &&
            process.argv[i + 1] == "headless") {
          return true;
        }
      }
      return false;
    }

    bool valid_role(std::string_view role) {
      return !role.empty() && std::all_of(role.begin(), role.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || c == '-';
      });
    }

    std::optional<process_t> validate_process(
      const marker_t &marker,
      const lookup_paths_t &paths
    ) {
      if (!marker.executable.is_absolute() || !gamescope_executable_name(marker.executable)) {
        return std::nullopt;
      }
      const auto process = read_process(paths, marker.pid);
      if (!process || process->start_time != marker.start_time ||
          process->executable != marker.executable || !is_headless_gamescope(*process)) {
        return std::nullopt;
      }
      return process;
    }

    std::map<int, process_t> read_processes(const lookup_paths_t &paths) {
      std::map<int, process_t> processes;
      std::error_code ec;
      fs::directory_iterator iterator(paths.proc_root, fs::directory_options::skip_permission_denied, ec);
      for (const auto &entry : iterator) {
        if (ec) {
          break;
        }
        const auto pid = parse_integer<int>(entry.path().filename().string());
        if (!pid) {
          continue;
        }
        if (auto process = read_process(paths, *pid)) {
          processes.emplace(*pid, std::move(*process));
        }
      }
      return processes;
    }

    bool is_descendant_of(
      int candidate,
      int root,
      const std::map<int, process_t> &processes
    ) {
      std::set<int> visited;
      while (candidate > 0 && visited.insert(candidate).second) {
        if (candidate == root) {
          return true;
        }
        const auto process = processes.find(candidate);
        if (process == processes.end()) {
          return false;
        }
        candidate = process->second.ppid;
      }
      return false;
    }

    using socket_inode_map_t = std::unordered_map<std::string, std::optional<std::uint64_t>>;

    std::optional<socket_inode_map_t> read_unix_socket_inodes(
      const fs::path &proc_net_unix
    ) {
      std::ifstream input(proc_net_unix);
      if (!input) {
        return std::nullopt;
      }
      socket_inode_map_t inodes;
      std::string line;
      std::getline(input, line);  // header
      while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string num;
        std::string ref_count;
        std::string protocol;
        std::string flags;
        std::string type;
        std::string state;
        std::string inode_text;
        if (!(row >> num >> ref_count >> protocol >> flags >> type >> state >> inode_text)) {
          continue;
        }
        std::string path;
        std::getline(row >> std::ws, path);
        const auto inode = parse_integer<std::uint64_t>(inode_text);
        if (inode && !path.empty()) {
          const auto [entry, inserted] = inodes.emplace(path, *inode);
          if (!inserted) {
            // An unlinked old listener and a rebound successor can coexist in
            // /proc/net/unix with the same pathname. Path identity is
            // ambiguous in that state, so fail closed instead of selecting a
            // row based on kernel iteration order.
            entry->second.reset();
          }
        }
      }
      return inodes;
    }

    bool process_holds_inode(const lookup_paths_t &paths, int pid, std::uint64_t inode) {
      const auto expected = "socket:[" + std::to_string(inode) + "]";
      std::error_code ec;
      fs::directory_iterator iterator(
        paths.proc_root / std::to_string(pid) / "fd",
        fs::directory_options::skip_permission_denied,
        ec
      );
      for (const auto &entry : iterator) {
        if (ec) {
          break;
        }
        std::error_code link_ec;
        const auto target = fs::read_symlink(entry.path(), link_ec);
        if (!link_ec && target == expected) {
          return true;
        }
      }
      return false;
    }

    std::optional<int> socket_peer_pid(const fs::path &socket_path) {
      const auto path = socket_path.string();
      sockaddr_un address {};
      if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        return std::nullopt;
      }
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
      if (fd < 0) {
        return std::nullopt;
      }
      locked_fd_t connection {fd};
      int connected;
      do {
        connected = connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
      } while (connected != 0 && errno == EINTR);
      if (connected != 0 && errno == EINPROGRESS) {
        pollfd descriptor {.fd = fd, .events = POLLOUT, .revents = 0};
        if (poll(&descriptor, 1, 100) == 1 && (descriptor.revents & POLLOUT) != 0) {
          int socket_error = 0;
          socklen_t socket_error_size = sizeof(socket_error);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 &&
              socket_error == 0) {
            connected = 0;
          }
        }
      }
      if (connected != 0) {
        return std::nullopt;
      }
      struct ucred peer {};
      socklen_t peer_size = sizeof(peer);
      if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) != 0 ||
          peer_size != sizeof(peer) || peer.pid <= 1 || peer.uid != getuid()) {
        return std::nullopt;
      }
      return peer.pid;
    }

    bool related_to_root(
      int pid,
      int root,
      const std::map<int, process_t> &processes
    ) {
      if (pid == root) {
        return false;
      }
      if (is_descendant_of(pid, root, processes)) {
        return true;
      }
      // gamescope calls setsid() then starts Xwayland. On current builds
      // Xwayland may double-fork and reparent to PID 1 while staying in
      // gamescope's session and process group — PPID ancestry alone fails.
      const auto root_it = processes.find(root);
      const auto pid_it = processes.find(pid);
      if (root_it == processes.end() || pid_it == processes.end()) {
        return false;
      }
      return root_it->second.session == root &&
             root_it->second.pgid == root &&
             pid_it->second.session == root &&
             pid_it->second.pgid == root;
    }

    std::optional<std::string> process_env_value(const fs::path &process_dir, std::string_view key) {
      std::ifstream environ_file(process_dir / "environ", std::ios::binary);
      if (!environ_file) {
        return std::nullopt;
      }
      const std::string bytes {
        std::istreambuf_iterator<char> {environ_file},
        std::istreambuf_iterator<char> {},
      };

      const std::string prefix = std::string {key} + "=";
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto end = bytes.find('\0', offset);
        const auto length = (end == std::string::npos ? bytes.size() : end) - offset;
        const std::string_view entry {bytes.data() + offset, length};
        if (entry.starts_with(prefix)) {
          return std::string {entry.substr(prefix.size())};
        }
        if (end == std::string::npos) {
          break;
        }
        offset = end + 1;
      }
      return std::nullopt;
    }

    std::optional<std::uint64_t> inode_for_path(
      const socket_inode_map_t &inodes,
      const fs::path &path
    ) {
      const auto found = inodes.find(path.string());
      if (found == inodes.end() || !found->second) {
        return std::nullopt;
      }
      return *found->second;
    }

  }  // namespace

  std::optional<marker_t> read_marker(const fs::path &path) {
    std::ifstream input(path);
    std::string pid_text;
    std::string start_time_text;
    std::string role;
    std::string executable_text;
    std::string extra;
    if (!(input >> pid_text >> start_time_text >> role >> executable_text) || (input >> extra)) {
      return std::nullopt;
    }
    const auto pid = parse_integer<int>(pid_text);
    const auto start_time = parse_integer<std::uint64_t>(start_time_text);
    const fs::path executable {executable_text};
    if (!pid || *pid <= 0 || !start_time || *start_time == 0 || !valid_role(role) ||
        !executable.is_absolute() || !gamescope_executable_name(executable)) {
      return std::nullopt;
    }
    return marker_t {
      .pid = *pid,
      .start_time = *start_time,
      .role = std::move(role),
      .executable = executable,
    };
  }

  bool write_marker(const fs::path &path, const marker_t &marker) {
    if (marker.pid <= 0 || marker.start_time == 0 || !valid_role(marker.role) ||
        !marker.executable.is_absolute() || !gamescope_executable_name(marker.executable) ||
        marker.executable.string().find_first_of(" \t\r\n") != std::string::npos) {
      return false;
    }
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = path;
    temporary += ".tmp." + std::to_string(nonce);
    {
      std::ofstream output(temporary, std::ios::trunc);
      if (!output) {
        return false;
      }
      output << marker.pid << ' ' << marker.start_time << ' ' << marker.role << ' '
             << marker.executable.string() << '\n';
      output.flush();
      if (!output) {
        std::error_code remove_ec;
        fs::remove(temporary, remove_ec);
        return false;
      }
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
      std::error_code remove_ec;
      fs::remove(temporary, remove_ec);
      return false;
    }
    return true;
  }

  std::optional<marker_t> marker_for_pid(
    int pid,
    std::string_view role,
    const lookup_paths_t &paths
  ) {
    if (!valid_role(role)) {
      return std::nullopt;
    }
    const auto process = read_process(paths, pid);
    if (!process || !is_headless_gamescope(*process)) {
      return std::nullopt;
    }
    return marker_t {
      .pid = pid,
      .start_time = process->start_time,
      .role = std::string {role},
      .executable = process->executable,
    };
  }

  bool is_valid_headless_gamescope(
    const marker_t &marker,
    const lookup_paths_t &paths
  ) {
    return validate_process(marker, paths).has_value();
  }

  std::optional<marker_t> validated_marker(
    const fs::path &path,
    std::string_view expected_role,
    const lookup_paths_t &paths
  ) {
    const auto marker = read_marker(path);
    if (!marker || (!expected_role.empty() && marker->role != expected_role) ||
        !validate_process(*marker, paths)) {
      return std::nullopt;
    }
    return marker;
  }

  bool process_has_argument(
    const marker_t &marker,
    std::string_view argument,
    const lookup_paths_t &paths
  ) {
    const auto process = validate_process(marker, paths);
    return process && std::find(process->argv.begin(), process->argv.end(), argument) != process->argv.end();
  }

  bool process_tree_owns_socket(
    const marker_t &marker,
    const fs::path &socket_path,
    const lookup_paths_t &paths
  ) {
    if (!validate_process(marker, paths)) {
      return false;
    }
    const auto inodes = read_unix_socket_inodes(paths.proc_net_unix);
    if (!inodes) {
      return false;
    }
    const auto inode = inode_for_path(*inodes, socket_path);
    if (!inode) {
      return false;
    }
    const auto processes = read_processes(paths);
    std::optional<pid_t> holder_pid;
    bool peer_credential_proof = false;
    for (const auto &[pid, process] : processes) {
      (void) process;
      if (is_descendant_of(pid, marker.pid, processes) && process_holds_inode(paths, pid, *inode)) {
        holder_pid = pid;
        break;
      }
    }
    if (!holder_pid) {
      // File capabilities can make gamescope non-dumpable, which hides every
      // /proc/<pid>/fd symlink from the same-user launcher. A connection to the
      // live Unix listener still exposes its kernel-authenticated peer PID.
      holder_pid = socket_peer_pid(socket_path);
      if (!holder_pid || !is_descendant_of(*holder_pid, marker.pid, processes)) {
        return false;
      }
      peer_credential_proof = true;
    }
    if (paths.before_socket_ownership_return_for_tests) {
      paths.before_socket_ownership_return_for_tests();
    }
    if (!validate_process(marker, paths)) {
      return false;
    }
    const auto final_inodes = read_unix_socket_inodes(paths.proc_net_unix);
    const auto final_inode = final_inodes ? inode_for_path(*final_inodes, socket_path) : std::nullopt;
    if (!final_inode || *final_inode != *inode) {
      return false;
    }
    const auto final_processes = read_processes(paths);
    if (!is_descendant_of(*holder_pid, marker.pid, final_processes)) {
      return false;
    }
    if (peer_credential_proof) {
      const auto final_peer = socket_peer_pid(socket_path);
      return final_peer && *final_peer == *holder_pid;
    }
    return process_holds_inode(paths, *holder_pid, *inode);
  }

  bool socket_has_live_holder(
    const fs::path &socket_path,
    const lookup_paths_t &paths
  ) {
    std::error_code ec;
    const bool socket_exists = fs::exists(socket_path, ec);
    if (ec) {
      return true;
    }
    if (!socket_exists) {
      return false;
    }
    const auto inodes = read_unix_socket_inodes(paths.proc_net_unix);
    if (!inodes) {
      return true;
    }
    const auto found = inodes->find(socket_path.string());
    // No /proc/net/unix row → filesystem residue only.
    if (found == inodes->end()) {
      return false;
    }
    // Any kernel socket-table row means the socket is still referenced. The
    // owning fd may be hidden by procfs permissions, so never use a failed fd
    // scan as permission to unlink the pathname.
    return true;
  }

  bool remove_orphan_socket(
    const fs::path &socket_path,
    const lookup_paths_t &paths
  ) {
    std::error_code ec;
    if (!fs::exists(socket_path, ec) || ec) {
      return !ec;
    }
    // Libwayland holds this advisory lock from bind through display teardown.
    // Taking it non-blocking closes the check/unlink race with a compositor
    // that has locked the name but has not created its socket yet.
    const fs::path lock_path {socket_path.string() + ".lock"};
    auto socket_lock = lock_wayland_socket_path(socket_path, paths);
    if (!socket_lock) {
      return false;
    }
    if (!fs::exists(socket_path, ec) || ec) {
      return !ec;
    }
    const int socket_pin_fd = open(socket_path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
    if (socket_pin_fd < 0) {
      return false;
    }
    locked_fd_t socket_pin {socket_pin_fd};
    if (!socket_pin.still_names_node(socket_path)) {
      return false;
    }
    if (!socket_lock->still_names(lock_path) || deleted_lock_state_unsafe(paths.proc_root, lock_path)) {
      return false;
    }
    if (socket_has_live_holder(socket_path, paths)) {
      return false;
    }
    if (paths.before_socket_unlink_for_tests) {
      paths.before_socket_unlink_for_tests();
    }
    if (!socket_lock->still_names(lock_path) || deleted_lock_state_unsafe(paths.proc_root, lock_path)) {
      return false;
    }
    // Keep an O_PATH descriptor for the original node until unlink completes.
    // A replacement path therefore cannot reuse the original inode allocation.
    if (!socket_pin.still_names_node(socket_path)) {
      return false;
    }
    if (!fs::remove(socket_path, ec) || ec) {
      return false;
    }
    // Leave the unlocked lock file in place. Unlinking a held lock creates a
    // second inode that another binder can acquire concurrently.
    return true;
  }

  std::optional<std::string> discover_owned_x11_display(
    const marker_t &marker,
    const lookup_paths_t &paths
  ) {
    if (!validate_process(marker, paths)) {
      return std::nullopt;
    }
    const auto processes = read_processes(paths);
    const auto x11_inodes = read_unix_socket_inodes(paths.proc_net_unix);
    if (!x11_inodes) {
      return std::nullopt;
    }
    struct candidate_t {
      int display;
      int pid;
      std::uint64_t start_time;
      fs::path executable;
      std::uint64_t inode;
    };
    std::optional<candidate_t> best;

    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(paths.x11_socket_dir, ec)) {
      if (ec) {
        break;
      }
      const auto name = entry.path().filename().string();
      if (name.size() < 2 || name.front() != 'X') {
        continue;
      }
      const auto display = parse_integer<int>(std::string_view {name}.substr(1));
      if (!display || *display < 0 || !entry.exists(ec)) {
        continue;
      }
      // Routing by pathname is authoritative only when the kernel exposes one
      // unambiguous inode for that X socket. Duplicate rows can represent an
      // unlinked predecessor plus a successor rebind.
      const auto inode = inode_for_path(*x11_inodes, entry.path());
      if (!inode) {
        continue;
      }
      for (const auto &[pid, process] : processes) {
        if (pid == marker.pid || !executable_named(process, "Xwayland") ||
            !related_to_root(pid, marker.pid, processes) ||
            !process_holds_inode(paths, pid, *inode)) {
          continue;
        }
        if (!best || *display < best->display) {
          best = candidate_t {
            .display = *display,
            .pid = pid,
            .start_time = process.start_time,
            .executable = process.executable,
            .inode = *inode,
          };
        }
      }
    }
    if (!best) {
      return std::nullopt;
    }
    if (paths.before_x11_return_for_tests) {
      paths.before_x11_return_for_tests();
    }
    // Rebuild the process snapshot and prove both generations and fd ownership
    // again immediately before returning a routing decision.
    if (!validate_process(marker, paths)) {
      return std::nullopt;
    }
    const auto final_processes = read_processes(paths);
    const auto root = final_processes.find(marker.pid);
    if (root == final_processes.end() ||
        root->second.start_time != marker.start_time ||
        root->second.executable != marker.executable) {
      return std::nullopt;
    }
    const auto found = final_processes.find(best->pid);
    if (found == final_processes.end() ||
        found->second.start_time != best->start_time ||
        found->second.executable != best->executable ||
        !executable_named(found->second, "Xwayland") ||
        !related_to_root(best->pid, marker.pid, final_processes) ||
        !process_holds_inode(paths, best->pid, best->inode)) {
      return std::nullopt;
    }
    const auto final_x11_inodes = read_unix_socket_inodes(paths.proc_net_unix);
    const auto socket_path = paths.x11_socket_dir / ("X" + std::to_string(best->display));
    const auto final_inode = final_x11_inodes ? inode_for_path(*final_x11_inodes, socket_path) : std::nullopt;
    if (!final_inode || *final_inode != best->inode) {
      return std::nullopt;
    }
    return ":" + std::to_string(best->display);
  }

  std::optional<int> nested_gamescope_client(
    std::string_view wayland_socket,
    const lookup_paths_t &paths
  ) {
    if (wayland_socket.empty()) {
      return std::nullopt;
    }

    // Runs on a session watch thread every couple of seconds, so the scan reads
    // one symlink per process and only opens environ for a gamescope.
    std::error_code ec;
    fs::directory_iterator iterator(paths.proc_root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
      return std::nullopt;
    }

    for (const auto &entry : iterator) {
      const auto pid = parse_integer<int>(entry.path().filename().string());
      if (!pid) {
        continue;
      }

      std::error_code link_ec;
      const auto executable = fs::read_symlink(entry.path() / "exe", link_ec);
      if (link_ec || !gamescope_executable_name(executable)) {
        continue;
      }

      if (process_env_value(entry.path(), "WAYLAND_DISPLAY") == wayland_socket) {
        return *pid;
      }
    }

    return std::nullopt;
  }

}  // namespace stream_runtime::gamescope_process
