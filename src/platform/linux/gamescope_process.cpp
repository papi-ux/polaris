/**
 * @file src/platform/linux/gamescope_process.cpp
 * @brief Exact-generation gamescope ownership and XWayland discovery helpers.
 */
#include "gamescope_process.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stream_runtime::gamescope_process {
  namespace {
    namespace fs = std::filesystem;

    struct process_t {
      int pid = 0;
      int ppid = 0;
      std::uint64_t start_time = 0;
      fs::path executable;
      std::vector<std::string> argv;
    };

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

    std::optional<process_t> read_process(const fs::path &proc_root, int pid) {
      if (pid <= 0) {
        return std::nullopt;
      }

      std::ifstream stat_file(proc_root / std::to_string(pid) / "stat");
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
      const auto start_time = parse_integer<std::uint64_t>(fields[19]);
      if (!ppid || !start_time || *start_time == 0) {
        return std::nullopt;
      }

      std::ifstream cmdline_file(proc_root / std::to_string(pid) / "cmdline", std::ios::binary);
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

      std::error_code executable_ec;
      const auto executable = fs::read_symlink(
        proc_root / std::to_string(pid) / "exe",
        executable_ec
      );
      if (executable_ec || executable.empty()) {
        return std::nullopt;
      }

      return process_t {
        .pid = pid,
        .ppid = *ppid,
        .start_time = *start_time,
        .executable = executable,
        .argv = std::move(argv),
      };
    }

    bool gamescope_executable_name(const fs::path &path) {
      const auto name = path.filename();
      return name == "gamescope" || name == ".gamescope-wrapped";
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
      const auto process = read_process(paths.proc_root, marker.pid);
      if (!process || process->start_time != marker.start_time ||
          process->executable != marker.executable || !is_headless_gamescope(*process)) {
        return std::nullopt;
      }
      return process;
    }

    std::map<int, process_t> read_processes(const fs::path &proc_root) {
      std::map<int, process_t> processes;
      std::error_code ec;
      fs::directory_iterator iterator(proc_root, fs::directory_options::skip_permission_denied, ec);
      for (const auto &entry : iterator) {
        if (ec) {
          break;
        }
        const auto pid = parse_integer<int>(entry.path().filename().string());
        if (!pid) {
          continue;
        }
        if (auto process = read_process(proc_root, *pid)) {
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

    socket_inode_map_t read_unix_socket_inodes(
      const fs::path &proc_net_unix
    ) {
      socket_inode_map_t inodes;
      std::ifstream input(proc_net_unix);
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

    std::optional<std::string> read_cgroup(const lookup_paths_t &paths, int pid) {
      std::ifstream input(paths.proc_root / std::to_string(pid) / "cgroup");
      std::string line;
      std::string last;
      while (std::getline(input, line)) {
        if (!line.empty()) {
          last = line;
        }
      }
      if (last.empty()) {
        return std::nullopt;
      }
      return last;
    }

    bool same_cgroup(const lookup_paths_t &paths, int a, int b) {
      const auto ca = read_cgroup(paths, a);
      const auto cb = read_cgroup(paths, b);
      return ca && cb && *ca == *cb;
    }

    bool related_to_root(
      int pid,
      int root,
      const std::map<int, process_t> &processes,
      const lookup_paths_t &paths
    ) {
      if (pid == root) {
        return true;
      }
      if (is_descendant_of(pid, root, processes)) {
        return true;
      }
      return same_cgroup(paths, pid, root);
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

    // All inode numbers listed for pathname (including ambiguous multi-row sets).
    std::vector<std::uint64_t> all_inodes_for_path(
      const fs::path &proc_net_unix,
      const fs::path &path
    ) {
      std::vector<std::uint64_t> out;
      std::ifstream input(proc_net_unix);
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
        std::string sock_path;
        std::getline(row >> std::ws, sock_path);
        if (sock_path != path.string()) {
          continue;
        }
        if (const auto inode = parse_integer<std::uint64_t>(inode_text)) {
          out.push_back(*inode);
        }
      }
      return out;
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
    const auto process = read_process(paths.proc_root, pid);
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
    const auto inode = inode_for_path(inodes, socket_path);
    if (!inode) {
      return false;
    }
    const auto processes = read_processes(paths.proc_root);
    for (const auto &[pid, process] : processes) {
      (void) process;
      if (is_descendant_of(pid, marker.pid, processes) && process_holds_inode(paths, pid, *inode)) {
        return true;
      }
    }
    return false;
  }

  bool socket_has_live_holder(
    const fs::path &socket_path,
    const lookup_paths_t &paths
  ) {
    std::error_code ec;
    if (!fs::exists(socket_path, ec) || ec) {
      return false;
    }
    const auto inodes = read_unix_socket_inodes(paths.proc_net_unix);
    const auto found = inodes.find(socket_path.string());
    // No /proc/net/unix row → filesystem residue only.
    if (found == inodes.end()) {
      return false;
    }
    // Ambiguous duplicate pathname rows: treat as live so callers fail closed.
    if (!found->second) {
      return true;
    }
    const auto processes = read_processes(paths.proc_root);
    for (const auto &[pid, process] : processes) {
      (void) process;
      if (process_holds_inode(paths, pid, *found->second)) {
        return true;
      }
    }
    return false;
  }

  bool remove_orphan_socket(
    const fs::path &socket_path,
    const lookup_paths_t &paths
  ) {
    std::error_code ec;
    if (!fs::exists(socket_path, ec) || ec) {
      fs::remove(fs::path(socket_path.string() + ".lock"), ec);
      return true;
    }
    if (socket_has_live_holder(socket_path, paths)) {
      return false;
    }
    fs::remove(socket_path, ec);
    fs::remove(fs::path(socket_path.string() + ".lock"), ec);
    return !fs::exists(socket_path, ec);
  }

  std::optional<std::string> discover_owned_x11_display(
    const marker_t &marker,
    const lookup_paths_t &paths
  ) {
    if (!validate_process(marker, paths)) {
      return std::nullopt;
    }
    const auto processes = read_processes(paths.proc_root);
    std::optional<int> best;

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
      // Include every inode row for this path so unlink/rebind residue does not
      // hide a live Xwayland still related to the marker generation.
      for (const auto inode : all_inodes_for_path(paths.proc_net_unix, entry.path())) {
        for (const auto &[pid, process] : processes) {
          if (pid == marker.pid || !executable_named(process, "Xwayland") ||
              !related_to_root(pid, marker.pid, processes, paths) ||
              !process_holds_inode(paths, pid, inode)) {
            continue;
          }
          if (!best || *display < *best) {
            best = *display;
          }
        }
      }
    }
    if (!best) {
      return std::nullopt;
    }
    return ":" + std::to_string(*best);
  }

}  // namespace stream_runtime::gamescope_process
