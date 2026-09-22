/**
 * @file src/platform/linux/steam_title_process.cpp
 * @brief Find the processes of one Steam title and ask that title to close, leaving Steam alone.
 */

#include "steam_title_process.h"

#ifdef __linux__

  #include <algorithm>
  #include <array>
  #include <cctype>
  #include <charconv>
  #include <csignal>
  #include <cstdlib>
  #include <fstream>
  #include <iterator>
  #include <map>
  #include <optional>
  #include <sstream>
  #include <sys/stat.h>
  #include <sys/syscall.h>
  #include <unistd.h>

using namespace std::literals;

namespace platf::steam_title {
  namespace {

    /// Steam's own processes around a title. The reaper is what Steam waits on, and a bwrap is the
    /// first process of a container's namespace: signalled, it exits at once and the kernel kills
    /// everything inside. The launch wrapper's name is cut to the kernel's fifteen characters.
    constexpr std::array k_wrappers {"reaper"sv, "steam-launch-wr"sv, "bwrap"sv, "srt-bwrap"sv, "pv-bwrap"sv};

    /// starttime is the 22nd field of stat, the 20th after the command name.
    constexpr int k_start_time_field = 19;

    /// The processes Steam starts a title through, and so the only ones whose command line counts.
    constexpr std::array k_launchers {"reaper"sv, "steam-launch-wr"sv};

    bool is_wrapper(std::string_view comm) {
      return std::find(k_wrappers.begin(), k_wrappers.end(), comm) != k_wrappers.end();
    }

    /// Steam starting this title: one of its launch processes, this account's, with the words in
    /// its command line. A shell or a search whose command line only mentions them is none of that.
    bool launches(const process_t &process, std::string_view appid, uid_t uid) {
      return process.uid == uid &&
             std::find(k_launchers.begin(), k_launchers.end(), std::string_view {process.comm}) != k_launchers.end() &&
             launch_cmdline_matches_appid(process.cmdline, appid);
    }

    bool numeric(std::string_view text) {
      return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
      });
    }

    std::string read_file(const std::filesystem::path &path) {
      std::ifstream file {path, std::ios::binary};
      return std::string {std::istreambuf_iterator<char> {file}, std::istreambuf_iterator<char> {}};
    }

    /// The name sits in brackets and may itself hold brackets and spaces, so it ends at the LAST one.
    std::optional<process_t> from_stat(std::string_view stat) {
      const auto name_begin = stat.find('(');
      const auto name_end = stat.rfind(')');
      if (name_begin == std::string_view::npos || name_end == std::string_view::npos || name_end < name_begin) {
        return std::nullopt;
      }

      process_t process;
      process.comm = std::string {stat.substr(name_begin + 1, name_end - name_begin - 1)};

      std::istringstream fields {std::string {stat.substr(name_end + 1)}};
      std::string field;
      for (int index = 0; fields >> field; ++index) {
        if (index == 1) {
          if (!numeric(field)) {
            return std::nullopt;
          }
          process.parent = static_cast<pid_t>(std::strtol(field.c_str(), nullptr, 10));
        } else if (index == k_start_time_field) {
          if (!numeric(field)) {
            return std::nullopt;
          }
          process.start_ticks = std::strtoull(field.c_str(), nullptr, 10);
          return process;
        }
      }
      return std::nullopt;  // cut short, so not a row to act on
    }

    std::optional<process_t> read_process(const std::filesystem::path &proc_root, pid_t pid) {
      const auto dir = proc_root / std::to_string(pid);
      auto process = from_stat(read_file(dir / "stat"));
      if (!process) {
        return std::nullopt;
      }
      struct stat owner {};
      if (::stat(dir.c_str(), &owner) != 0) {
        return std::nullopt;
      }
      process->pid = pid;
      process->uid = owner.st_uid;
      process->cmdline = read_file(dir / "cmdline");
      return process;
    }

    /// The title's one root: a matching process whose parent is not a matching process too.
    std::optional<process_t> title_root(const std::vector<process_t> &table, std::string_view appid, uid_t uid) {
      std::map<pid_t, const process_t *> matching;
      for (const auto &process : table) {
        if (launches(process, appid, uid)) {
          matching.emplace(process.pid, &process);
        }
      }

      const process_t *root = nullptr;
      for (const auto &[pid, process] : matching) {
        if (matching.contains(process->parent)) {
          continue;
        }
        if (root != nullptr) {
          return std::nullopt;  // two launches claim the appid
        }
        root = process;
      }
      return root ? std::optional<process_t> {*root} : std::nullopt;
    }

  }  // namespace

  bool launch_cmdline_matches_appid(std::string_view cmdline, std::string_view appid) {
    if (!numeric(appid)) {
      return false;
    }

    const auto expected_appid = "AppId="s + std::string {appid};
    const auto is_separator = [](unsigned char value) {
      return value == '\0' || std::isspace(value) != 0;
    };
    std::string_view previous;
    std::size_t cursor = 0;
    while (cursor < cmdline.size()) {
      while (cursor < cmdline.size() && is_separator(static_cast<unsigned char>(cmdline[cursor]))) {
        ++cursor;
      }
      const auto begin = cursor;
      while (cursor < cmdline.size() && !is_separator(static_cast<unsigned char>(cmdline[cursor]))) {
        ++cursor;
      }
      if (begin == cursor) {
        break;
      }
      const auto token = cmdline.substr(begin, cursor - begin);
      if (previous == "SteamLaunch"sv && token == expected_appid) {
        return true;
      }
      previous = token;
    }
    return false;
  }

  std::string launch_appid(std::string_view steam_game_id) {
    if (!numeric(steam_game_id) || steam_game_id.size() > 20) {
      return std::string {steam_game_id};
    }
    std::uint64_t id = 0;
    const auto [end, error] = std::from_chars(steam_game_id.data(), steam_game_id.data() + steam_game_id.size(), id);
    if (error != std::errc {} || end != steam_game_id.data() + steam_game_id.size()) {
      return std::string {steam_game_id};
    }
    if (id > 0xFFFFFFFFull) {
      return std::to_string(id >> 32);
    }
    return std::string {steam_game_id};
  }

  bool running(const std::vector<process_t> &table, std::string_view appid, uid_t uid) {
    return std::any_of(table.begin(), table.end(), [&](const process_t &process) {
      return launches(process, appid, uid);
    });
  }

  std::vector<process_t> processes_to_ask(const std::vector<process_t> &table, std::string_view appid, uid_t uid) {
    const auto root = title_root(table, appid, uid);
    if (!root) {
      return {};
    }

    std::multimap<pid_t, const process_t *> children;
    for (const auto &process : table) {
      children.emplace(process.parent, &process);
    }

    // Outermost first, so the container's supervisor hears before the title it supervises.
    std::vector<process_t> to_ask;
    std::vector<pid_t> frontier {root->pid};
    std::vector<pid_t> seen {root->pid};
    while (!frontier.empty()) {
      std::vector<pid_t> next;
      for (const auto parent : frontier) {
        const auto [first, last] = children.equal_range(parent);
        for (auto it = first; it != last; ++it) {
          const auto &child = *it->second;
          if (child.uid != uid || std::find(seen.begin(), seen.end(), child.pid) != seen.end()) {
            continue;
          }
          seen.push_back(child.pid);
          next.push_back(child.pid);
          if (!is_wrapper(child.comm)) {
            to_ask.push_back(child);
          }
        }
      }
      std::sort(next.begin(), next.end());
      frontier = std::move(next);
    }
    return to_ask;
  }

  std::vector<process_t> read_process_table(const std::filesystem::path &proc_root) {
    std::vector<process_t> table;
    std::error_code ec;
    for (std::filesystem::directory_iterator it {proc_root, ec}, last; !ec && it != last; it.increment(ec)) {
      const auto name = it->path().filename().string();
      if (!numeric(name)) {
        continue;
      }
      const auto pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
      if (pid <= 1) {
        continue;
      }
      if (auto process = read_process(proc_root, pid)) {
        table.push_back(std::move(*process));
      }
    }
    std::sort(table.begin(), table.end(), [](const process_t &left, const process_t &right) {
      return left.pid < right.pid;
    });
    return table;
  }

  close_result_t ask_to_close(std::string_view appid) {
    close_result_t result;
    const auto uid = getuid();
    const auto table = read_process_table();
    result.was_running = running(table, appid, uid);

    for (const auto &chosen : processes_to_ask(table, appid, uid)) {
      const auto fd = static_cast<int>(syscall(SYS_pidfd_open, chosen.pid, 0));
      if (fd < 0) {
        ++result.gone;
        continue;
      }

      // The pidfd now names whatever holds this pid. It is only signalled if that is still the
      // process the row was read from.
      const auto now = read_process("/proc", chosen.pid);
      const bool same = now && now->start_ticks == chosen.start_ticks && now->comm == chosen.comm && now->uid == uid;
      if (same && syscall(SYS_pidfd_send_signal, fd, SIGTERM, nullptr, 0) == 0) {
        ++result.asked;
      } else {
        ++result.gone;
      }
      close(fd);
    }
    return result;
  }

}  // namespace platf::steam_title

#endif
