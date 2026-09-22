/**
 * @file src/platform/linux/steam_title_process.h
 * @brief Find the processes of one Steam title and ask that title to close, leaving Steam alone.
 */
#pragma once

#ifdef __linux__

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace platf::steam_title {

  /**
   * @brief One row of the process table, as /proc gives it.
   */
  struct process_t {
    pid_t pid {0};
    pid_t parent {0};
    uid_t uid {0};
    std::uint64_t start_ticks {0};  ///< start time from stat, which tells a process from a later one with its pid
    std::string comm;  ///< the kernel's name for it, 15 characters at most
    std::string cmdline;  ///< NUL separated
  };

  /**
   * @brief Whether a command line is Steam starting this title.
   *
   * Steam starts every title as `reaper SteamLaunch AppId=<appid> -- ...`. The two words must be
   * whole arguments, next to each other, and the appid must be decimal, so a title whose own
   * arguments mention another AppId does not match.
   */
  bool launch_cmdline_matches_appid(std::string_view cmdline, std::string_view appid);

  /**
   * @brief Whether Steam is running this title for this account.
   *
   * Only Steam's own launch processes count, the reaper and the launch wrapper, so a shell or a
   * process search whose command line happens to hold the same words is not taken for the title.
   */
  bool running(const std::vector<process_t> &table, std::string_view appid, uid_t uid);

  /**
   * @brief The processes to ask so the title closes and Steam stays.
   *
   * Everything under the title's SteamLaunch root except Steam's own wrappers: the reaper that Steam
   * is waiting on, and the bwrap that holds the container's process namespace. A bwrap that gets
   * the signal exits at once and the kernel kills everything inside it, which takes away the very
   * chance to close that the signal was meant to give. Asked from the inside, the container's own
   * supervisor passes the request on, waits, and the wrappers unwind after it.
   *
   * Empty when the title is not running, and empty when more than one root claims the appid, because
   * then nothing here can say which of them this host started.
   */
  std::vector<process_t> processes_to_ask(const std::vector<process_t> &table, std::string_view appid, uid_t uid);

  /**
   * @brief The process table of the host. Rows that vanish while being read are left out.
   */
  std::vector<process_t> read_process_table(const std::filesystem::path &proc_root = "/proc");

  struct close_result_t {
    bool was_running {false};
    int asked {0};  ///< processes that took the request
    int gone {0};  ///< processes that had already exited or changed identity by the time they were reached
  };

  /**
   * @brief Ask a running Steam title of this account to close. Sends SIGTERM, never anything harder.
   *
   * Each process is reached through a pidfd and checked against the row it was chosen from, so a
   * pid that was reused in between is never signalled.
   */
  close_result_t ask_to_close(std::string_view appid);

}  // namespace platf::steam_title

#endif
