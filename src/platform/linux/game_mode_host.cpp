/**
 * @file src/platform/linux/game_mode_host.cpp
 * @brief Recognise hosts that boot into a gamescope Steam session (Game Mode) and phrase the guidance they need.
 */
#ifdef __linux__

#include "game_mode_host.h"

#include "executable_path.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf::game_mode_host {

  namespace {
    constexpr std::size_t max_probe_file_bytes = 64U * 1024U;
    constexpr auto headless_boot_command = "sudo -H polaris --setup-host --enable-headless-boot"sv;

    /// Read up to max_probe_file_bytes; procfs files report no size, so read until EOF.
    std::optional<std::string> read_small_file(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        return std::nullopt;
      }
      std::string content;
      char chunk[4096];
      while (content.size() < max_probe_file_bytes) {
        in.read(chunk, sizeof(chunk));
        const auto got = in.gcount();
        if (got <= 0) {
          break;
        }
        content.append(chunk, static_cast<std::size_t>(got));
      }
      if (content.size() > max_probe_file_bytes) {
        content.resize(max_probe_file_bytes);
      }
      return content;
    }

    std::string trim(std::string_view text) {
      const auto is_space = [](unsigned char c) {
        return std::isspace(c) != 0;
      };
      while (!text.empty() && is_space(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
      }
      while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
      }
      return std::string(text);
    }

    std::string lower(std::string text) {
      std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return text;
    }

    std::string strip_quotes(std::string value) {
      if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
      }
      return value;
    }

    std::string basename_of(std::string_view token) {
      return fs::path(token).filename().string();
    }

    /**
     * `gamescope-session` (SteamOS), `gamescope-session-plus` (ChimeraOS,
     * Bazzite, CachyOS handheld), `start-gamescope-session` (SteamOS 3.7+).
     * Polaris' own `polaris-gamescope-session` launcher is not a Game Mode
     * session and must never match.
     */
    bool is_session_tool_name(std::string_view name) {
      return name.find("gamescope-session"sv) != std::string_view::npos && !name.starts_with("polaris-"sv);
    }

    bool is_shell_name(std::string_view name) {
      return name == "bash"sv || name == "sh"sv || name == "dash"sv || name == "zsh"sv;
    }

    std::vector<std::string> split_nul(const std::string &raw) {
      std::vector<std::string> tokens;
      std::size_t start = 0;
      while (start < raw.size()) {
        const auto end = raw.find('\0', start);
        const auto stop = end == std::string::npos ? raw.size() : end;
        if (stop > start) {
          tokens.emplace_back(raw.substr(start, stop - start));
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      return tokens;
    }

    std::vector<std::string> split_lines(const std::string &raw) {
      std::vector<std::string> lines;
      std::istringstream stream(raw);
      std::string line;
      while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        lines.emplace_back(std::move(line));
      }
      return lines;
    }

    /**
     * Whether any word of an Exec line names a session tool. Entries wrap the
     * tool in `env` prefixes with options, assignments and quotes, and the
     * Desktop Entry spec allows all of it; the tool name is the constant.
     */
    bool exec_runs_session_tool(std::string_view exec) {
      std::istringstream stream {std::string(exec)};
      std::string word;
      while (stream >> word) {
        word = strip_quotes(word);
        if (word.starts_with('-') || word.find('=') != std::string::npos) {
          continue;
        }
        if (is_session_tool_name(basename_of(word))) {
          return true;
        }
      }
      return false;
    }

    std::optional<std::string> desktop_entry_gamescope_reason(const std::string &content) {
      for (const auto &raw_line : split_lines(content)) {
        const auto line = trim(raw_line);
        if (line.starts_with("Exec="sv) && exec_runs_session_tool(line.substr(5))) {
          return "Exec=" + trim(line.substr(5));
        }
        if (line.starts_with("DesktopNames="sv) && lower(line).find("gamescope"sv) != std::string::npos) {
          return trim(line);
        }
      }
      return std::nullopt;
    }

    template<typename visit_t>
    void for_each_entry(const fs::path &dir, visit_t &&visit) {
      std::error_code ec;
      fs::directory_iterator it(dir, ec);
      const fs::directory_iterator end;
      for (; !ec && it != end; it.increment(ec)) {
        visit(*it);
      }
    }

    void scan_session_dirs(const probe_t &probe, detection_t &detection) {
      std::vector<fs::path> entries;
      for (const auto &dir : probe.session_dirs) {
        for_each_entry(dir, [&entries](const fs::directory_entry &entry) {
          if (entry.path().extension() == ".desktop"sv) {
            entries.push_back(entry.path());
          }
        });
      }
      std::sort(entries.begin(), entries.end());
      for (const auto &entry : entries) {
        const auto content = read_small_file(entry);
        if (!content) {
          continue;
        }
        if (const auto reason = desktop_entry_gamescope_reason(*content)) {
          detection.installed = true;
          detection.evidence.push_back("session entry " + entry.string() + " (" + *reason + ")");
        }
      }
    }

    void scan_path_dirs(const probe_t &probe, detection_t &detection) {
      std::string search_path;
      for (const auto &dir : probe.path_dirs) {
        if (!search_path.empty()) {
          search_path += ':';
        }
        search_path += dir.string();
      }
      if (search_path.empty()) {
        return;
      }
      for (const char *tool : {"steamos-session-select", "gamescope-session-plus", "gamescope-session"}) {
        const auto found = platf::linux_util::find_executable_in_path(tool, search_path.c_str());
        if (!found.empty()) {
          detection.installed = true;
          detection.evidence.push_back(std::string(tool) + " on PATH (" + found + ")");
        }
      }
    }

    void scan_os_release(const probe_t &probe, detection_t &detection) {
      if (probe.os_release.empty()) {
        return;
      }
      const auto content = read_small_file(probe.os_release);
      if (!content) {
        return;
      }
      std::string id;
      std::string variant_id;
      for (const auto &raw_line : split_lines(*content)) {
        const auto line = trim(raw_line);
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
          continue;
        }
        const auto key = trim(line.substr(0, eq));
        const auto value = strip_quotes(trim(line.substr(eq + 1)));
        if (key == "ID"sv) {
          id = lower(value);
        } else if (key == "VARIANT_ID"sv) {
          variant_id = lower(value);
        }
      }
      if (id == "steamos"sv) {
        detection.installed = true;
        detection.evidence.push_back("os-release ID=steamos");
      }
      if (variant_id.find("deck"sv) != std::string::npos) {
        detection.installed = true;
        detection.evidence.push_back("os-release VARIANT_ID=" + variant_id);
      }
    }

    /// The session script itself, or a shell running it. `grep gamescope-session` does not count.
    std::optional<std::string> session_tool_in_cmdline(const std::vector<std::string> &argv) {
      if (argv.empty()) {
        return std::nullopt;
      }
      const auto first = basename_of(argv[0]);
      if (is_session_tool_name(first)) {
        return first;
      }
      if (argv.size() >= 2 && is_shell_name(first)) {
        const auto second = basename_of(argv[1]);
        if (is_session_tool_name(second)) {
          return second;
        }
      }
      return std::nullopt;
    }

    /**
     * A gamescope-session process is a shell script run by the account, so
     * its /proc directory is owned by that account (the effective uid equals
     * the real one); stat() answers the ownership question without opening
     * anything for every other process on the host.
     */
    void scan_processes(const probe_t &probe, detection_t &detection) {
      if (probe.proc_root.empty()) {
        return;
      }
      std::vector<unsigned long> pids;
      for_each_entry(probe.proc_root, [&](const fs::directory_entry &entry) {
        const auto name = entry.path().filename().string();
        unsigned long pid = 0;
        const auto [end, err] = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (err != std::errc() || end != name.data() + name.size()) {
          return;
        }
        struct stat info {};
        if (::stat(entry.path().c_str(), &info) != 0 || info.st_uid != probe.uid) {
          return;
        }
        pids.push_back(pid);
      });
      std::sort(pids.begin(), pids.end());
      for (const auto pid : pids) {
        const auto cmdline = read_small_file(probe.proc_root / std::to_string(pid) / "cmdline");
        if (!cmdline) {
          continue;
        }
        if (const auto tool = session_tool_in_cmdline(split_nul(*cmdline))) {
          detection.installed = true;
          detection.session_active = true;
          detection.evidence.push_back(*tool + " running as this account (pid " + std::to_string(pid) + ")");
          return;
        }
      }
    }

    std::vector<fs::path> path_dirs_from_environment() {
      std::vector<fs::path> dirs;
      const auto push_unique = [&dirs](fs::path dir) {
        if (dir.empty() || std::find(dirs.begin(), dirs.end(), dir) != dirs.end()) {
          return;
        }
        dirs.push_back(std::move(dir));
      };
      if (const char *path = std::getenv("PATH"); path && *path) {
        std::string_view remaining {path};
        while (!remaining.empty()) {
          const auto colon = remaining.find(':');
          push_unique(fs::path(remaining.substr(0, colon)));
          if (colon == std::string_view::npos) {
            break;
          }
          remaining.remove_prefix(colon + 1);
        }
      }
      // sudo sanitises PATH and the user service may carry a minimal one; the
      // session tools live in the system directories either way.
      push_unique("/usr/bin");
      push_unique("/usr/local/bin");
      return dirs;
    }

    detection_t detect_installed(const probe_t &probe) {
      detection_t detection;
      scan_session_dirs(probe, detection);
      scan_path_dirs(probe, detection);
      scan_os_release(probe, detection);
      return detection;
    }
  }  // namespace

  probe_t default_probe(uid_t uid) {
    probe_t probe;
    probe.session_dirs = {"/usr/share/wayland-sessions", "/usr/local/share/wayland-sessions"};
    probe.path_dirs = path_dirs_from_environment();
    probe.os_release = "/etc/os-release";
    probe.proc_root = "/proc";
    probe.uid = uid;
    return probe;
  }

  detection_t detect(const probe_t &probe) {
    auto detection = detect_installed(probe);
    scan_processes(probe, detection);
    return detection;
  }

  detection_t detect_cached() {
    static std::once_flag once;
    static detection_t installed;
    std::call_once(once, [] {
      installed = detect_installed(default_probe(::getuid()));
    });
    auto detection = installed;
    probe_t live;
    live.proc_root = "/proc";
    live.uid = ::getuid();
    scan_processes(live, detection);
    return detection;
  }

  std::string headline_evidence(const detection_t &detection) {
    if (detection.evidence.empty()) {
      return "no evidence recorded";
    }
    std::string headline = detection.evidence.front();
    if (detection.evidence.size() > 1) {
      headline += " and " + std::to_string(detection.evidence.size() - 1) + " more signal" + (detection.evidence.size() > 2 ? "s" : "");
    }
    return headline;
  }

  boot_paths_t default_boot_paths(std::string user, const fs::path &home) {
    return boot_paths_t {
      "/var/lib/systemd/linger",
      std::move(user),
      home.empty() ? fs::path() : home / ".config",
      "/etc/systemd/user/default.target.wants",
    };
  }

  boot_readiness_t boot_readiness(const boot_paths_t &paths) {
    boot_readiness_t readiness;
    std::error_code ec;
    if (!paths.user.empty() && !paths.linger_dir.empty()) {
      readiness.linger_enabled = fs::exists(paths.linger_dir / paths.user, ec);
    }
    const bool user_want = !paths.config_home.empty() &&
                           fs::exists(paths.config_home / "systemd/user/default.target.wants/polaris.service", ec);
    const bool system_want = !paths.system_wants_dir.empty() &&
                             fs::exists(paths.system_wants_dir / "polaris.service", ec);
    readiness.boot_start_linked = user_want || system_want;
    return readiness;
  }

  guidance_t boot_readiness_guidance(const detection_t &detection, bool boot_independent) {
    if (boot_independent) {
      return guidance_t {
        "boot_independent",
        "Polaris starts at boot with no monitor or desktop login.",
        "No action needed.",
      };
    }
    if (detection.installed) {
      return guidance_t {
        "session_bound",
        "This host has a Steam Game Mode session, which never starts Polaris. Polaris is only reachable after a Desktop Mode login and goes offline when the host returns to Game Mode.",
        std::string("Run: ") + std::string(headless_boot_command),
      };
    }
    return guidance_t {
      "session_bound",
      "Polaris starts with the desktop session, so after a reboot it is unavailable until someone logs in.",
      std::string("For a monitor-less or Game Mode host, run: ") + std::string(headless_boot_command),
    };
  }

  guidance_t display_session_guidance(
    const detection_t &detection,
    bool boot_independent,
    bool has_wayland_display,
    bool has_x11_display
  ) {
    if (detection.session_active) {
      return guidance_t {
        "game_mode_session",
        "Steam Game Mode is running. Streaming from inside Game Mode is not supported yet.",
        "Switch to Desktop Mode to stream. The handhelds guide lists what works today.",
      };
    }
    if (has_wayland_display || has_x11_display) {
      return guidance_t {
        "healthy",
        has_wayland_display ? "Wayland desktop environment is available to Polaris." : "X11 desktop environment is available to Polaris.",
        "No action needed.",
      };
    }
    if (boot_independent) {
      return guidance_t {
        "missing_display_environment",
        "No desktop environment is attached, which is expected on a headless-boot host. Private Stream is unaffected.",
        "To stream the visible desktop instead, log into the desktop and restart Polaris so it inherits the graphical environment.",
      };
    }
    if (detection.installed) {
      return guidance_t {
        "missing_display_environment",
        "Polaris could not find WAYLAND_DISPLAY or DISPLAY. On a Game Mode host this usually means Polaris started outside the desktop session.",
        std::string("Restart Polaris from Desktop Mode, then run ") + std::string(headless_boot_command) + " so it stops depending on the session.",
      };
    }
    return guidance_t {
      "missing_display_environment",
      "Polaris could not find WAYLAND_DISPLAY or DISPLAY for desktop previews.",
      "Restart Polaris from the desktop session or run the user service so it inherits the graphical environment.",
    };
  }

  std::string setup_host_advice(const detection_t &detection, setup_host_state_t state, std::string_view exe_path) {
    if (!detection.installed) {
      return {};
    }
    const std::string enable_command = "sudo -H " + std::string(exe_path) + " --setup-host --enable-headless-boot";
    const std::string not_yet = "Streaming from inside Game Mode is not supported yet; Desktop Mode streams work. See docs/handhelds.md.\n";
    std::string advice = "Steam Game Mode session detected: " + headline_evidence(detection) + ".\n";
    switch (state) {
      case setup_host_state_t::needs_headless_boot:
        advice += "Game Mode never runs desktop autostart, so Polaris is only reachable after a Desktop Mode login and goes offline when the host returns to Game Mode.\n";
        advice += "Make it start at boot instead:\n  " + enable_command + "\n";
        return advice;
      case setup_host_state_t::already_independent:
        advice += "Polaris already starts at boot, so switching between Desktop Mode and Game Mode does not take it offline.\n";
        return advice + not_yet;
      case setup_host_state_t::headless_boot_enabled_now:
        advice += "With headless boot on, switching between Desktop Mode and Game Mode no longer takes Polaris offline.\n";
        return advice + not_yet;
      case setup_host_state_t::headless_boot_disabled_now:
        advice += "With headless boot off, Polaris goes offline when the host returns to Game Mode and comes back after a Desktop Mode login.\n";
        advice += "Turn it back on with:\n  " + enable_command + "\n";
        return advice;
    }
    return advice;
  }

}  // namespace platf::game_mode_host

#endif
