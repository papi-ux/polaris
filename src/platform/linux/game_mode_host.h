/**
 * @file src/platform/linux/game_mode_host.h
 * @brief Recognise hosts that boot into a gamescope Steam session (Game Mode) and phrase the guidance they need.
 */
#pragma once

#ifdef __linux__

#include <filesystem>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace platf::game_mode_host {

  /**
   * @brief Where detection looks.
   *
   * Production uses default_probe(); tests point every path at scratch
   * directories so no test ever reads the real host.
   */
  struct probe_t {
    std::vector<std::filesystem::path> session_dirs;  ///< wayland-sessions directories the display manager offers
    std::vector<std::filesystem::path> path_dirs;  ///< directories searched for the session tools
    std::filesystem::path os_release;  ///< os-release file
    std::filesystem::path proc_root;  ///< procfs root for the live-session scan; empty skips it
    uid_t uid {0};  ///< account whose processes count as a live session
  };

  struct detection_t {
    bool installed {false};  ///< the host offers a gamescope Steam session at login, or is running one now
    bool session_active {false};  ///< a gamescope session is running for the account right now
    std::vector<std::string> evidence;  ///< what was found, in a stable order
  };

  /// Real host paths, PATH, os-release, /proc, and the given account.
  probe_t default_probe(uid_t uid);

  /**
   * @brief Look for a gamescope Steam session on the host.
   *
   * Three install-time signals, any of which counts: a wayland-sessions entry
   * whose Exec runs a gamescope-session tool or that names the gamescope
   * desktop (ChimeraOS-style `gamescope-session-plus steam` on Bazzite,
   * CachyOS handheld edition and Nobara; `start-gamescope-session` with
   * `DesktopNames=gamescope` on SteamOS), one of those tools or
   * `steamos-session-select` on PATH, or an os-release that identifies SteamOS
   * or a deck variant. One live signal, which also counts as installed: a
   * gamescope-session process owned by the account.
   */
  detection_t detect(const probe_t &probe);

  /**
   * @brief detect() for the calling account, for the long-lived host process.
   *
   * The install-time signals cannot change while Polaris runs, so they are
   * scanned once per process; only the live-session scan repeats.
   */
  detection_t detect_cached();

  /// The first signal plus a count of the rest, for one line of terminal output.
  std::string headline_evidence(const detection_t &detection);

  struct boot_paths_t {
    std::filesystem::path linger_dir;  ///< /var/lib/systemd/linger
    std::string user;  ///< account name, the linger file name
    std::filesystem::path config_home;  ///< the account's config home, as the headless-boot writer sees it
    std::filesystem::path system_wants_dir;  ///< /etc/systemd/user/default.target.wants
  };

  /**
   * @brief Real paths for an account.
   *
   * @param home the account's home directory from passwd, which is what
   * `--enable-headless-boot` writes under; the reader must look where the
   * writer wrote, not where XDG_CONFIG_HOME in this process happens to point.
   */
  boot_paths_t default_boot_paths(std::string user, const std::filesystem::path &home);

  /**
   * @brief Whether this account's Polaris survives a reboot with no desktop login.
   *
   * Lingering plus a default.target want is what `--setup-host
   * --enable-headless-boot` arranges; either alone is not enough, so both are
   * reported.
   */
  struct boot_readiness_t {
    bool linger_enabled {false};
    bool boot_start_linked {false};

    bool independent() const {
      return linger_enabled && boot_start_linked;
    }
  };

  boot_readiness_t boot_readiness(const boot_paths_t &paths);

  struct guidance_t {
    std::string status;
    std::string summary;
    std::string action;
  };

  /// Boot-readiness copy for the stats API: `boot_independent` or `session_bound`.
  guidance_t boot_readiness_guidance(const detection_t &detection, bool boot_independent);

  /**
   * @brief Display-session copy for the stats API.
   *
   * `game_mode_session` while a gamescope session runs, whatever the
   * environment still says (a long-lived Polaris keeps the desktop's
   * WAYLAND_DISPLAY after the desktop is gone); `healthy` with a display
   * environment; otherwise `missing_display_environment` with advice that
   * fits the host: a headless-boot host is told nothing is wrong, a Game Mode
   * host is pointed at headless boot, and a plain desktop host is told to
   * restart from the desktop.
   */
  guidance_t display_session_guidance(
    const detection_t &detection,
    bool boot_independent,
    bool has_wayland_display,
    bool has_x11_display
  );

  /// What `--setup-host` did or found about headless boot on this run.
  enum class setup_host_state_t {
    needs_headless_boot,  ///< session-bound and this run did not change that
    already_independent,  ///< boot start was already in place before this run
    headless_boot_enabled_now,  ///< this run enabled headless boot
    headless_boot_disabled_now,  ///< this run disabled headless boot
  };

  /**
   * @brief What `--setup-host` prints about a Game Mode host. Empty when the host has none.
   *
   * @param exe_path the running binary, for the command to print.
   */
  std::string setup_host_advice(const detection_t &detection, setup_host_state_t state, std::string_view exe_path);

}  // namespace platf::game_mode_host

#endif
