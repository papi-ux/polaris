/**
 * @file src/platform/linux/epic_process.cpp
 * @brief Non-secret procfs observations and exact retained-pidfd Epic close grants.
 */
#include "epic_process.h"

#ifdef __linux__

#include "src/vrr_compositor_identity.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <fcntl.h>
#include <mutex>
#include <openssl/sha.h>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <utility>

namespace platf::epic {
  namespace {
    namespace fs = std::filesystem;
    using testing::backend_t;
    using testing::exit_state_t;
    using testing::file_t;
    constexpr std::size_t small_limit = 64 * 1024;
    constexpr std::size_t maps_limit = 4 * 1024 * 1024;
    constexpr std::size_t mount_limit = 2 * 1024 * 1024;
    constexpr std::size_t process_limit = 8192;
    constexpr std::size_t snapshot_limit = 64 * 1024 * 1024;

    struct refusal_t: std::runtime_error {
      using std::runtime_error::runtime_error;
    };

    void require(bool condition, const char *reason) {
      if (!condition) {
        throw refusal_t(reason);
      }
    }

    template<class T>
    T needed(std::optional<T> value, const char *reason) {
      require(value.has_value(), reason);
      return std::move(*value);
    }

    std::uint64_t number(std::string_view text, int base = 10) {
      std::uint64_t result = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, base);
      require(!text.empty() && parsed.ec == std::errc {} && parsed.ptr == text.data() + text.size(), "malformed_proc_number");
      return result;
    }

    std::vector<std::string> words(const std::string &text) {
      std::istringstream stream(text);
      std::vector<std::string> result;
      std::string value;
      while (stream >> value) {
        result.push_back(std::move(value));
      }
      return result;
    }

    bool safe_absolute_path(const fs::path &path) {
      const auto text = path.string();
      return path.is_absolute() && text.size() <= 4096 && text.find('\0') == std::string::npos &&
             std::none_of(path.begin(), path.end(), [](const auto &part) { return part == ".." || part == "."; });
    }

    bool inside(const fs::path &path, const fs::path &parent) {
      auto p = path.begin();
      for (auto q = parent.begin(); q != parent.end(); ++q, ++p) {
        if (p == path.end() || *p != *q) {
          return false;
        }
      }
      return true;
    }

    bool same_file(const file_t &a, const file_t &b) {
      return a.device == b.device && a.inode == b.inode && a.uid == b.uid && a.mode == b.mode;
    }

    bool same_image(const file_t &a, const file_t &b) {
      return same_file(a, b) && a.size == b.size && a.modified_ns == b.modified_ns && a.changed_ns == b.changed_ns;
    }

    bool safe_file(const file_t &file, std::uint64_t owner, bool executable) {
      return S_ISREG(file.mode) && file.inode != 0 && (file.uid == owner || file.uid == 0) &&
             (file.mode & (S_IWGRP | S_IWOTH | S_ISUID | S_ISGID)) == 0 && (!executable || (file.mode & 0111) != 0);
    }

    // Only kernel path escapes are decoded. A literal backslash is encoded as \134.
    std::string unescape(std::string_view text) {
      std::string result;
      for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\') {
          result += text[i];
          continue;
        }
        require(i + 3 < text.size(), "malformed_proc_path");
        const auto code = text.substr(i + 1, 3);
        require(code == "040" || code == "011" || code == "012" || code == "134", "malformed_proc_path");
        result += static_cast<char>(number(code, 8));
        i += 3;
      }
      require(result.find('\0') == std::string::npos, "malformed_proc_path");
      return result;
    }

    struct stat_t {
      std::uint64_t pid, parent, start;
      char state;
    };

    stat_t parse_stat(const std::string &text) {
      const auto left = text.find('(');
      const auto right = text.rfind(')');
      require(left != std::string::npos && right != std::string::npos && left > 1 && right > left && text[left - 1] == ' ', "malformed_process_stat");
      const auto fields = words(text.substr(right + 1));
      require(fields.size() >= 20 && fields[0].size() == 1, "incomplete_process_stat");
      stat_t result {number(std::string_view(text).substr(0, left - 1)), number(fields[1]), number(fields[19]), fields[0][0]};
      require(result.pid > 1 && result.pid <= INT_MAX && result.start > 0, "invalid_process_identity");
      return result;
    }

    std::string cgroup(const std::string &text) {
      std::optional<std::string> result;
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        if (line.compare(0, 3, "0::") == 0) {
          require(!result, "ambiguous_process_cgroup");
          result = line.substr(3);
        }
      }
      require(result && safe_absolute_path(*result), "missing_unified_cgroup");
      return *result;
    }

    bool service_cgroup(const std::string &path, const std::string &self) {
      for (const auto &part : fs::path(path)) {
        const auto name = part.string();
        if (name == "polaris.service" || (name.compare(0, 8, "polaris@") == 0 && name.size() >= 8 && name.find(".service") != std::string::npos)) {
          return true;
        }
      }
      return inside(path, self);
    }

    bool check_uid(const std::string &text, std::uint64_t owner) {
      bool found = false;
      bool owned = false;
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        if (line.compare(0, 4, "Uid:") == 0) {
          require(!found, "ambiguous_process_uid");
          found = true;
          const auto ids = words(line.substr(4));
          require(ids.size() == 4, "incomplete_process_uid");
          const auto matching = std::count_if(ids.begin(), ids.end(), [owner](const auto &id) { return number(id) == owner; });
          // A setuid-root helper may retain the owner's real UID, while its effective,
          // saved and filesystem credentials are all root. Like a wholly-root process,
          // it is outside owner game-control authority, never a candidate to adopt.
          if (number(ids[0]) == owner && number(ids[1]) == 0 && number(ids[2]) == 0 && number(ids[3]) == 0) {
            continue;
          }
          require(matching == 0 || matching == 4, "foreign_process_uid");
          owned = matching == 4;
        }
      }
      require(found, "missing_process_uid");
      return owned;
    }

    bool token(const std::string &value) {
      return !value.empty() && value.size() <= 256 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
      });
    }

    enum class credentials_t { owner, root, other };

    /** All four status UIDs, classified without refusing a mix: lineage needs root as well as the owner. */
    credentials_t credentials(const std::string &text, std::uint64_t owner) {
      std::optional<credentials_t> result;
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        if (line.compare(0, 4, "Uid:") == 0) {
          require(!result, "ambiguous_process_uid");
          const auto ids = words(line.substr(4));
          require(ids.size() == 4, "incomplete_process_uid");
          const auto all = [&ids](std::uint64_t uid) {
            return std::all_of(ids.begin(), ids.end(), [uid](const auto &id) { return number(id) == uid; });
          };
          result = all(owner) ? credentials_t::owner : all(0) ? credentials_t::root : credentials_t::other;
        }
      }
      require(result.has_value(), "missing_process_uid");
      return *result;
    }

    // The kernel task name in stat, which may itself contain spaces and parentheses.
    std::string stat_name(const std::string &text) {
      const auto left = text.find('(');
      const auto right = text.rfind(')');
      require(left != std::string::npos && right != std::string::npos && right > left, "malformed_process_stat");
      return text.substr(left + 1, right - left - 1);
    }

    std::optional<std::string> status_name(const std::string &text) {
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        if (line.compare(0, 5, "Name:") == 0) {
          const auto first = line.find_first_not_of(" \t", 5);
          return first == std::string::npos ? std::string {} : line.substr(first);
        }
      }
      return std::nullopt;
    }

    /**
     * Only ever refuses. A task whose own name could be Epic, an AppImage, Wine, Proton or a
     * Windows game is never placed by session_role(), whatever its placement says. A name is
     * chosen by the task itself, so it never admits on its own.
     */
    bool unrelated_name(std::string name) {
      std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
      });
      for (const char *marker : {"epic", "appimage", "wine", "proton"}) {
        if (name.find(marker) != std::string::npos) {
          return false;
        }
      }
      return !name.empty() && !(name.size() >= 4 && name.compare(name.size() - 4, 4, ".exe") == 0);
    }

    enum class placement_t { none, user_manager, user_service, login_session };

    /**
     * Where a unified cgroup puts a task among the roles admission may set aside, all inside this
     * owner's user slice. systemd and logind assign these units; the task does not name them.
     */
    placement_t placement(const std::string &group, std::uint64_t owner) {
      const auto uid = std::to_string(owner);
      std::vector<std::string> parts;
      for (const auto &part : fs::path(group).relative_path()) {
        parts.push_back(part.string());
      }
      if (parts.size() < 3 || parts[0] != "user.slice" || parts[1] != "user-" + uid + ".slice") {
        return placement_t::none;
      }
      const auto ends = [](const std::string &text, std::string_view suffix) {
        return text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
      };
      if (parts.size() == 3) {
        // A logind login session: session-<id>.scope directly under the user slice.
        const auto &scope = parts[2];
        return scope.compare(0, 8, "session-") == 0 && ends(scope, ".scope") && token(scope.substr(8, scope.size() - 14)) ?
                 placement_t::login_session :
                 placement_t::none;
      }
      if (parts[2] != "user@" + uid + ".service") {
        return placement_t::none;
      }
      if (parts.size() == 4) {
        return parts[3] == "init.scope" ? placement_t::user_manager : placement_t::none;
      }
      // A service unit of the user manager. Application launches (app-*), transient runs (run-*)
      // and D-Bus activations (dbus-*) are not session services, and nothing nests below a unit.
      const auto &slice = parts[3];
      const auto &unit = parts[4];
      if (parts.size() != 5 || (slice != "session.slice" && slice != "background.slice" && slice != "app.slice") || !ends(unit, ".service")) {
        return placement_t::none;
      }
      for (const char *prefix : {"app-", "run-", "dbus-"}) {
        if (unit.compare(0, std::string_view(prefix).size(), prefix) == 0) {
          return placement_t::none;
        }
      }
      return placement_t::user_service;
    }

    struct map_t {
      std::string range;
      std::uint64_t major, minor, inode;
    };

    std::vector<map_t> image_maps(const std::string &text, const fs::path &image) {
      std::vector<map_t> result;
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        std::istringstream row(line);
        std::string range, perms, offset, device, inode, path;
        require(bool(row >> range >> perms >> offset >> device >> inode), "malformed_process_maps");
        const auto dash = range.find('-');
        const auto colon = device.find(':');
        require(dash != std::string::npos && colon != std::string::npos && perms.size() == 4, "malformed_process_maps");
        require((perms[0] == 'r' || perms[0] == '-') && (perms[1] == 'w' || perms[1] == '-') &&
                  (perms[2] == 'x' || perms[2] == '-') && (perms[3] == 'p' || perms[3] == 's'), "malformed_map_permissions");
        require(number(std::string_view(range).substr(0, dash), 16) < number(std::string_view(range).substr(dash + 1), 16), "malformed_map_range");
        (void) number(offset, 16);
        const auto major = number(std::string_view(device).substr(0, colon), 16);
        const auto minor = number(std::string_view(device).substr(colon + 1), 16);
        const auto ino = number(inode);
        std::getline(row, path);
        const auto first = path.find_first_not_of(' ');
        path = first == std::string::npos ? "" : unescape(std::string_view(path).substr(first));
        require(path != image.string() + " (deleted)", "deleted_game_image");
        if (path == image) {
          require(ino > 0, "missing_mapped_image_inode");
          result.push_back({range, major, minor, ino});
        }
      }
      return result;
    }

    struct mount_t {
      std::string identity, type, source;
      fs::path point;
      std::uint64_t major, minor;
    };

    mount_t image_mount(const std::string &text, const fs::path &image) {
      std::optional<mount_t> result;
      std::istringstream stream(text);
      std::string line;
      while (std::getline(stream, line)) {
        const auto fields = words(line);
        const auto sep = std::find(fields.begin(), fields.end(), "-");
        require(fields.size() >= 10 && sep != fields.end() && sep - fields.begin() >= 6 && fields.end() - sep >= 4, "malformed_mountinfo");
        (void) number(fields[0]);
        (void) number(fields[1]);
        const auto colon = fields[2].find(':');
        require(colon != std::string::npos, "malformed_mount_device");
        const auto major = number(std::string_view(fields[2]).substr(0, colon));
        const auto minor = number(std::string_view(fields[2]).substr(colon + 1));
        const fs::path point = unescape(fields[4]);
        require(safe_absolute_path(point) && safe_absolute_path(unescape(fields[3])), "malformed_mount_path");
        if (!inside(image, point)) {
          continue;
        }
        require(!result || point != result->point, "ambiguous_image_mount");
        if (!result || point.string().size() > result->point.string().size()) {
          // Include mount ID, root, source, type and mount flags, not just the device pair.
          result = mount_t {line, *(sep + 1), unescape(*(sep + 2)), point, major, minor};
        }
      }
      require(result.has_value(), "missing_image_mount");
      return *result;
    }

    struct process_t {
      stat_t stat;
      fs::path exe, cwd;
      file_t image;
      std::string group;
      std::vector<map_t> maps;
    };

    struct reader_t {
      backend_t &backend;
      std::size_t bytes {0};
      fs::path group_namespace;
      struct uninspected_t {
        stat_t identity;
        file_t directory;
      };
      std::vector<uninspected_t> uninspected;
      // Set only by a private snapshot, whose every protected task must be proved outside.
      bool private_census {false};

      std::string counted(std::optional<std::string> text) {
        auto value = needed(std::move(text), "incomplete_process_observation");
        bytes += value.size();
        require(bytes <= snapshot_limit, "process_snapshot_too_large");
        return value;
      }

      std::string read(const fs::path &path, std::size_t limit = small_limit) {
        return counted(backend.read(path, limit));
      }

      fs::path link(const fs::path &path) {
        auto value = needed(backend.link(path), "unreadable_process_link");
        require(safe_absolute_path(value), "unresolved_process_link");
        return value;
      }

      file_t stat(const fs::path &path) {
        return needed(backend.stat(path), "unreadable_file_identity");
      }

      fs::path canonical(const fs::path &path) {
        auto value = needed(backend.canonical(path), "unresolved_title_path");
        require(safe_absolute_path(value), "unresolved_title_path");
        return value;
      }

      std::string boot() {
        auto value = read("/proc/sys/kernel/random/boot_id", 64);
        if (!value.empty() && value.back() == '\n') {
          value.pop_back();
        }
        require(value.size() == 36, "missing_boot_identity");
        for (std::size_t i = 0; i < value.size(); ++i) {
          require((i == 8 || i == 13 || i == 18 || i == 23) ? value[i] == '-' :
                    ((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')), "invalid_boot_identity");
        }
        return value;
      }

      bool unchanged(const process_t &process) {
        const auto path = fs::path("/proc") / std::to_string(process.stat.pid);
        const auto current = parse_stat(read(path / "stat"));
        return current.pid == process.stat.pid && current.start == process.stat.start &&
               current.state != 'Z' && current.state != 'X' && current.state != 'x' &&
               check_uid(read(path / "status"), backend.owner_uid()) && stat(path).uid == backend.owner_uid() &&
               link(path / "exe") == process.exe && link(path / "cwd") == process.cwd &&
               cgroup(read(path / "cgroup")) == process.group && backend.link(path / "ns/cgroup") == group_namespace &&
               same_image(process.image, stat(path / "exe")) && same_image(process.image, stat(process.exe)) &&
               same_image(process.image, stat(path / "root" / process.exe.relative_path()));
      }

      bool still_uninspected(const uninspected_t &process) {
        const auto path = fs::path("/proc") / std::to_string(process.identity.pid);
        const auto current = parse_stat(read(path / "stat"));
        const auto exe = backend.inspect_link(path / "exe");
        return current.pid == process.identity.pid && current.start == process.identity.start &&
               current.state != 'Z' && current.state != 'X' && current.state != 'x' &&
               check_uid(read(path / "status"), backend.owner_uid()) &&
               process.directory.uid == backend.owner_uid() &&
               same_file(process.directory, stat(path)) && !exe.target && exe.permission_denied;
      }

      /**
       * The same private protected task: its stat, owner status and pinned proc directory, which
       * may be owner- or root-owned. What it executes now is not part of its identity.
       */
      bool still_same_task(const uninspected_t &process) {
        const auto path = fs::path("/proc") / std::to_string(process.identity.pid);
        const auto current = parse_stat(read(path / "stat"));
        return current.pid == process.identity.pid && current.start == process.identity.start &&
               current.state != 'Z' && current.state != 'X' && current.state != 'x' &&
               check_uid(read(path / "status"), backend.owner_uid()) && same_file(process.directory, stat(path));
      }

      struct lineage_t {
        stat_t identity;
        std::string group;
        credentials_t credentials;
      };

      // World-readable facts only: stat, status credentials and the unified cgroup, never PID 1.
      lineage_t lineage(std::uint64_t pid) {
        const auto path = fs::path("/proc") / std::to_string(pid);
        const auto identity = parse_stat(read(path / "stat"));
        require(identity.pid == pid && identity.state != 'Z' && identity.state != 'X' && identity.state != 'x', "protected_lineage_unproven");
        return {identity, cgroup(read(path / "cgroup")), credentials(read(path / "status"), backend.owner_uid())};
      }

      static bool same_lineage(const lineage_t &a, const lineage_t &b) {
        return a.identity.pid == b.identity.pid && a.identity.start == b.identity.start && a.identity.parent == b.identity.parent &&
               a.group == b.group && a.credentials == b.credentials;
      }

      /**
       * D-A (B), papi's 2026-10-29 decision on #420. At private admission a protected owner task,
       * whose executable, maps and links the owner cannot read, counts as "not Epic" only when its
       * world-readable kernel facts mark it as an unrelated session or system process. Those facts
       * are its stat (name, parent, start time), its status (name, credentials) and its unified
       * cgroup, and the same for each ancestor; never maps, exe, cwd, argv or environ. With U the
       * owner's slice /user.slice/user-<uid>.slice and M its manager U/user@<uid>.service, exactly
       * three roles qualify:
       *
       *  1. The user manager itself, M/init.scope: a child of PID 1, or a child of that scope's
       *     owner-credentialed child of PID 1 ("systemd --user" and its "(sd-pam)").
       *  2. A user service, M/{session,background,app}.slice/<unit>.service, other than an
       *     application launch (app-*), a transient run (run-*) or a D-Bus activation (dbus-*).
       *     Every ancestor up to the user manager is owner-credentialed in that same unit, so the
       *     manager started it rather than an application ("kwin_wayland", "ssh-agent").
       *  3. A login session, U/session-<id>.scope, whose parent is root in that same scope and whose
       *     ancestors are all root up to PID 1 (an SSH login's unprivileged "sshd-session").
       *
       * Polaris's own service, or anything inside its cgroup, never qualifies. Each ancestor is no
       * younger than its child, and every edge is read again before the role counts; a vanished,
       * reused, mixed-credential or changed link leaves it unproven. The task's stat and status
       * names must agree and must not look like Epic, an AppImage, Wine, Proton or a Windows game;
       * that screen can only refuse. Anything else, including any application placement, refuses.
       *
       * This is the owner-visible residual papi accepted, not a proof of absence. If it is ever
       * wrong, a hidden desktop launcher receives the request and the game opens on the desktop:
       * the in-session ownership check stays the backstop, so Private shows "Game unverified", End
       * closes only the private session, and nothing outside that session is signalled.
       */
      bool session_role(const uninspected_t &task, const std::string &own_group) {
        try {
          const auto owner = backend.owner_uid();
          const auto path = fs::path("/proc") / std::to_string(task.identity.pid);
          const auto name = stat_name(read(path / "stat"));
          if (status_name(read(path / "status")) != name || !unrelated_name(name)) {
            return false;
          }
          std::vector<lineage_t> chain {lineage(task.identity.pid)};
          if (chain[0].identity.start != task.identity.start || chain[0].credentials != credentials_t::owner) {
            return false;
          }
          std::set<std::uint64_t> visited {task.identity.pid};
          while (chain.back().identity.parent != 1) {
            const auto parent = chain.back().identity.parent;
            if (parent <= 1 || chain.size() >= 64 || !visited.insert(parent).second) {
              return false;
            }
            auto next = lineage(parent);
            if (next.identity.start > chain.back().identity.start) {
              return false;
            }
            chain.push_back(std::move(next));
          }
          const auto &group = chain[0].group;
          const auto manager_scope = "/user.slice/user-" + std::to_string(owner) + ".slice/user@" + std::to_string(owner) + ".service/init.scope";
          const auto owned_in = [&chain](std::size_t i, const std::string &unit) {
            return chain[i].credentials == credentials_t::owner && chain[i].group == unit;
          };
          bool placed = false;
          switch (service_cgroup(group, own_group) ? placement_t::none : placement(group, owner)) {
            case placement_t::user_manager:
              placed = chain.size() == 1 || (chain.size() == 2 && owned_in(1, group));
              break;
            case placement_t::user_service:
              placed = chain.size() >= 2 && owned_in(chain.size() - 1, manager_scope);
              for (std::size_t i = 1; placed && i + 1 < chain.size(); ++i) {
                placed = owned_in(i, group);
              }
              break;
            case placement_t::login_session:
              placed = chain.size() >= 2 && chain[1].group == group;
              for (std::size_t i = 1; placed && i < chain.size(); ++i) {
                placed = chain[i].credentials == credentials_t::root;
              }
              break;
            case placement_t::none:
              break;
          }
          if (!placed) {
            return false;
          }
          for (const auto &edge : chain) {
            if (!same_lineage(edge, lineage(edge.identity.pid))) {
              return false;
            }
          }
          return true;
        }
        catch (const refusal_t &) {
          return false;
        }
      }

      /** Admission sets aside the protected tasks the census recorded, each by session_role(), or refuses. */
      void place_protected(const char *reason) {
        if (uninspected.empty()) {
          return;
        }
        const auto own_group = cgroup(read("/proc/self/cgroup"));
        for (const auto &task : uninspected) {
          require(session_role(task, own_group), reason);
        }
      }

      // A set-aside task must still be the same protected task when the census ends: one that
      // exits, is reused or becomes readable (by running something else) was never checked.
      void recheck_placed(const char *reason) {
        for (const auto &task : uninspected) {
          require(still_uninspected(task), reason);
        }
      }

      std::vector<process_t> launchers() {
        // This is a presence-only prerequisite, never game selection or signal authority. An
        // unrelated protected desktop process cannot supply a launcher or an AppImage proof.
        // Once an executable is a candidate, every identity/namespace/bookend remains required.
        const auto owner = backend.owner_uid();
        require(owner != 0, "host_owner_uid_required");
        group_namespace = needed(backend.link("/proc/self/ns/cgroup"), "unknown_cgroup_namespace");
        const auto ns = group_namespace.string();
        require(ns.size() > 9 && ns.compare(0, 8, "cgroup:[") == 0 && ns.back() == ']' &&
                  number(std::string_view(ns).substr(8, ns.size() - 9)) > 0, "unknown_cgroup_namespace");
        const auto names = needed(backend.list("/proc", process_limit), "incomplete_process_table");
        std::set<std::uint64_t> seen;
        std::vector<process_t> result;
        for (const auto &name : names) {
          if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
          }
          const auto pid = number(name);
          require(pid > 0 && pid <= INT_MAX && seen.insert(pid).second, "ambiguous_process_table");
          if (pid == 1) {
            continue;
          }
          const auto path = fs::path("/proc") / name;
          const auto exe = backend.link(path / "exe");
          if (!exe) {
            continue;
          }
          auto extension = exe->extension().string();
          std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
          });
          if (exe->filename() != "EpicGamesLauncher" && extension != ".appimage") {
            continue;
          }
          require(safe_absolute_path(*exe), "unresolved_process_link");
          if (!check_uid(read(path / "status"), owner)) {
            continue;
          }
          require(stat(path).uid == owner, "unreadable_owner_process");
          const auto first = parse_stat(read(path / "stat"));
          require(first.pid == pid, "process_pid_changed");
          if (first.state == 'Z' || first.state == 'X' || first.state == 'x') {
            continue;
          }
          process_t row {first, *exe, link(path / "cwd"), stat(path / "exe"), cgroup(read(path / "cgroup")), {}};
          require(backend.link(path / "ns/cgroup") == group_namespace, "foreign_cgroup_namespace");
          require(same_image(row.image, stat(row.exe)) && same_image(row.image, stat(path / "root" / row.exe.relative_path())), "process_executable_view_changed");
          require(unchanged(row), "process_changed_during_read");
          result.push_back(std::move(row));
        }
        return result;
      }

      std::vector<process_t> table(const fs::path &game_image = {}) {
        const auto owner = backend.owner_uid();
        require(owner != 0, "host_owner_uid_required");
        group_namespace = needed(backend.link("/proc/self/ns/cgroup"), "unknown_cgroup_namespace");
        const auto ns = group_namespace.string();
        require(ns.size() > 9 && ns.compare(0, 8, "cgroup:[") == 0 && ns.back() == ']' &&
                  number(std::string_view(ns).substr(8, ns.size() - 9)) > 0, "unknown_cgroup_namespace");
        const auto names = needed(backend.list("/proc", process_limit), "incomplete_process_table");
        std::set<std::uint64_t> seen;
        std::vector<process_t> result;
        for (const auto &name : names) {
          if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
          }
          const auto pid = number(name);
          require(pid > 0 && pid <= INT_MAX && seen.insert(pid).second, "ambiguous_process_table");
          if (pid == 1) {
            continue;
          }
          const auto path = fs::path("/proc") / name;
          // A same-UID non-dumpable process can have a root-owned proc directory.
          // Read non-secret status first, so it cannot silently disappear from an owned scan.
          auto status = backend.initial_status(path, small_limit);
          if (!status.text && status.absence == testing::process_absence_t::vanished) {
            continue;
          }
          if (!check_uid(counted(std::move(status.text)), owner)) {
            continue;
          }
          const auto directory = stat(path);
          if (directory.uid != owner) {
            // Nothing past status and stat is readable here. Only the private census may keep
            // such an owner task, as protected, for its stat-ancestry outside proof; it never
            // becomes a row. Every other scan refuses, as before.
            require(private_census, "unreadable_owner_process");
            const auto first = parse_stat(read(path / "stat"));
            require(first.pid == pid, "process_pid_changed");
            if (first.state == 'Z' || first.state == 'X' || first.state == 'x') {
              continue;
            }
            const auto last = parse_stat(read(path / "stat"));
            require(last.pid == pid && last.start == first.start && last.state != 'Z' && last.state != 'X' && last.state != 'x' &&
                      check_uid(read(path / "status"), owner) && same_file(directory, stat(path)), "protected_process_changed");
            uninspected.push_back({first, directory});
            continue;
          }
          const auto first = parse_stat(read(path / "stat"));
          require(first.pid == pid, "process_pid_changed");
          if (first.state == 'Z' || first.state == 'X' || first.state == 'x') {
            continue;
          }
          const auto exe = backend.inspect_link(path / "exe");
          if (!exe.target && exe.permission_denied) {
            // Explicit consent selects one verified instance; it does not claim a complete
            // desktop census. Only an actual access denial may be disclosed as uninspected.
            // A missing/raced link, readable candidate or incomplete maps still fails closed.
            uninspected_t denied {first, stat(path)};
            require(still_uninspected(denied), "protected_process_changed");
            uninspected.push_back(denied);
            continue;
          }
          const auto executable = needed(exe.target, "unreadable_process_link");
          require(safe_absolute_path(executable), "unresolved_process_link");
          auto maps = game_image.empty() ? std::vector<map_t> {} : image_maps(read(path / "maps", maps_limit), game_image);
          if (!game_image.empty() && maps.empty() && executable != game_image && executable.filename() != "wineserver") {
            // This readable process does not map the game and cannot supply its prefix
            // server. It needs a stable negative observation, not close-target authority.
            // In particular, an unrelated app's stale /proc/root view must not block a game.
            const auto last = parse_stat(read(path / "stat"));
            require(last.pid == first.pid && last.start == first.start &&
                      last.state != 'Z' && last.state != 'X' && last.state != 'x' &&
                      check_uid(read(path / "status"), owner) && stat(path).uid == owner &&
                      link(path / "exe") == executable, "unmapped_process_changed");
            continue;
          }
          process_t row {first, executable, link(path / "cwd"), stat(path / "exe"), cgroup(read(path / "cgroup")), std::move(maps)};
          require(backend.link(path / "ns/cgroup") == group_namespace, "foreign_cgroup_namespace");
          require(same_image(row.image, stat(row.exe)) && same_image(row.image, stat(path / "root" / row.exe.relative_path())), "process_executable_view_changed");
          require(unchanged(row), "process_changed_during_read");
          result.push_back(std::move(row));
        }
        return result;
      }

      struct owner_task_t {
        stat_t identity;
        fs::path exe;
        file_t image;
      };

      /**
       * Every readable owner task's stat and executable link, with no identity required yet; a
       * protected task is recorded as uninspected, exactly as table() records it. The caller gives
       * full identity() only to its candidates and a stable_negative() to the rest.
       */
      std::vector<owner_task_t> owner_tasks() {
        const auto owner = backend.owner_uid();
        require(owner != 0, "host_owner_uid_required");
        group_namespace = needed(backend.link("/proc/self/ns/cgroup"), "unknown_cgroup_namespace");
        const auto ns = group_namespace.string();
        require(ns.size() > 9 && ns.compare(0, 8, "cgroup:[") == 0 && ns.back() == ']' &&
                  number(std::string_view(ns).substr(8, ns.size() - 9)) > 0, "unknown_cgroup_namespace");
        const auto names = needed(backend.list("/proc", process_limit), "incomplete_process_table");
        std::set<std::uint64_t> seen;
        std::vector<owner_task_t> result;
        for (const auto &name : names) {
          if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
          }
          const auto pid = number(name);
          require(pid > 0 && pid <= INT_MAX && seen.insert(pid).second, "ambiguous_process_table");
          if (pid == 1) {
            continue;
          }
          const auto path = fs::path("/proc") / name;
          auto status = backend.initial_status(path, small_limit);
          if (!status.text && status.absence == testing::process_absence_t::vanished) {
            continue;
          }
          if (!check_uid(counted(std::move(status.text)), owner)) {
            continue;
          }
          require(stat(path).uid == owner, "unreadable_owner_process");
          const auto first = parse_stat(read(path / "stat"));
          require(first.pid == pid, "process_pid_changed");
          if (first.state == 'Z' || first.state == 'X' || first.state == 'x') {
            continue;
          }
          const auto exe = backend.inspect_link(path / "exe");
          if (!exe.target && exe.permission_denied) {
            uninspected_t denied {first, stat(path)};
            require(still_uninspected(denied), "protected_process_changed");
            uninspected.push_back(denied);
            continue;
          }
          const auto executable = needed(exe.target, "unreadable_process_link");
          require(safe_absolute_path(executable), "unresolved_process_link");
          // The task's own executable handle stays statable for a Flatpak or deleted path.
          result.push_back({first, executable, stat(path / "exe")});
        }
        return result;
      }

      /** The same task, still the owner's, still running the same executable file and link. */
      bool stable_negative(const owner_task_t &task) {
        const auto path = fs::path("/proc") / std::to_string(task.identity.pid);
        const auto last = parse_stat(read(path / "stat"));
        return last.pid == task.identity.pid && last.start == task.identity.start &&
               last.state != 'Z' && last.state != 'X' && last.state != 'x' &&
               check_uid(read(path / "status"), backend.owner_uid()) && stat(path).uid == backend.owner_uid() &&
               link(path / "exe") == task.exe && same_file(task.image, stat(path / "exe"));
      }

      /** A candidate's full identity, as table() requires of every row it returns. */
      process_t identity(const owner_task_t &task) {
        const auto path = fs::path("/proc") / std::to_string(task.identity.pid);
        process_t row {task.identity, task.exe, link(path / "cwd"), task.image, cgroup(read(path / "cgroup")), {}};
        require(backend.link(path / "ns/cgroup") == group_namespace, "foreign_cgroup_namespace");
        require(same_image(row.image, stat(row.exe)) && same_image(row.image, stat(path / "root" / row.exe.relative_path())), "process_executable_view_changed");
        require(unchanged(row), "process_changed_during_read");
        return row;
      }
    };

    struct digest_t {
      std::string bytes;

      void add(const std::string &value) {
        bytes += std::to_string(value.size()) + ":" + value;
      }

      void add(std::uint64_t value) {
        add(std::to_string(value));
      }

      void file(const file_t &value, bool directory = false) {
        add(value.device);
        add(value.inode);
        add(value.uid);
        add(value.mode);
        if (!directory) {
          add(value.size);
          add(static_cast<std::uint64_t>(value.modified_ns));
          add(static_cast<std::uint64_t>(value.changed_ns));
        }
      }

      void process(const process_t &value) {
        add(value.stat.pid);
        add(value.stat.start);
        add(value.exe.string());
        add(value.cwd.string());
        add(value.group);
        file(value.image);
      }

      std::string finish() const {
        std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
        SHA256(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), digest.data());
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        for (const auto byte : digest) {
          result += hex[byte >> 4];
          result += hex[byte & 15];
        }
        return result;
      }
    };

    std::optional<fs::path> runtime(const fs::path &exe, const fs::path &base, const char *binary) {
      if (!inside(exe, base)) {
        return std::nullopt;
      }
      const auto relative = exe.lexically_relative(base);
      std::vector<fs::path> parts(relative.begin(), relative.end());
      if (parts.size() != 4 || parts[1] != "files" || parts[2] != "bin" || parts[3] != binary || !token(parts[0].string())) {
        return std::nullopt;
      }
      return base / parts[0];
    }

    struct private_context_t {
      std::shared_ptr<const vrr_clock::compositor_lease_t> lease;
      std::shared_ptr<backend_t> backend;
      std::string generation, boot;
      fs::path group_namespace;
      process_t host, supervisor, compositor;
    };

    bool session_current(const private_context_t &session) {
      try {
        return session.lease && session.lease->current();
      }
      catch (const std::exception &) {
        return false;
      }
    }

    process_t session_process(reader_t &read, std::uint64_t pid) {
      require(pid > 1 && pid <= INT_MAX, "invalid_private_process");
      if (read.group_namespace.empty()) {
        read.group_namespace = needed(read.backend.link("/proc/self/ns/cgroup"), "unknown_cgroup_namespace");
        const auto ns = read.group_namespace.string();
        require(ns.size() > 9 && ns.compare(0, 8, "cgroup:[") == 0 && ns.back() == ']' &&
                  number(std::string_view(ns).substr(8, ns.size() - 9)) > 0, "unknown_cgroup_namespace");
      }
      const auto path = fs::path("/proc") / std::to_string(pid);
      const auto identity = parse_stat(read.read(path / "stat"));
      require(identity.pid == pid && identity.state != 'Z' && identity.state != 'X' && identity.state != 'x' &&
                check_uid(read.read(path / "status"), read.backend.owner_uid()) && read.stat(path).uid == read.backend.owner_uid(), "private_process_identity_unproven");
      process_t process {identity, read.link(path / "exe"), read.link(path / "cwd"), read.stat(path / "exe"), cgroup(read.read(path / "cgroup")), {}};
      require(safe_file(process.image, read.backend.owner_uid(), true) && read.unchanged(process) &&
                parse_stat(read.read(path / "stat")).parent == identity.parent, "private_process_changed");
      return process;
    }

    bool unchanged_session_process(reader_t &read, const process_t &process) {
      const auto path = fs::path("/proc") / std::to_string(process.stat.pid);
      return read.unchanged(process) && parse_stat(read.read(path / "stat")).parent == process.stat.parent;
    }

    void validate_session(reader_t &read, const private_context_t &session) {
      require(session_current(session), "private_session_not_current");
      require(session.backend.get() == &read.backend && read.backend.owner_uid() != 0 &&
                session.lease->process.uid == read.backend.owner_uid() && session.boot == read.boot() &&
                read.backend.link("/proc/self/ns/cgroup") == session.group_namespace, "private_session_identity_changed");
      read.group_namespace = session.group_namespace;
      require(unchanged_session_process(read, session.host) && unchanged_session_process(read, session.supervisor) &&
                unchanged_session_process(read, session.compositor) && session_current(session), "private_supervisor_changed");
    }

    private_context_t private_context(std::shared_ptr<const vrr_clock::compositor_lease_t> lease,
                                      const std::string &generation, std::shared_ptr<backend_t> backend) {
      require(bool(backend), "missing_process_reader");
      require(lease && lease->current() && token(generation), "private_session_not_current");
      require(backend->owner_uid() != 0 && lease->process.uid == backend->owner_uid() &&
                lease->process.pid > 1 && lease->process.pid <= INT_MAX, "private_compositor_identity_unproven");
      reader_t read {*backend};
      const auto boot = read.boot();
      const auto self = parse_stat(read.read("/proc/self/stat"));
      const auto host = session_process(read, self.pid);
      require(host.stat.start == self.start && host.stat.parent == self.parent, "private_host_identity_changed");
      const auto compositor = session_process(read, static_cast<std::uint64_t>(lease->process.pid));
      require(compositor.stat.start == lease->process.starttime, "private_compositor_identity_changed");
      const auto supervisor = session_process(read, compositor.stat.parent);
      const auto own_group = cgroup(read.read("/proc/self/cgroup"));
      require(own_group != "/" && host.group == own_group && supervisor.group == own_group && compositor.group == own_group &&
                supervisor.stat.parent == host.stat.pid && host.stat.start <= supervisor.stat.start &&
                supervisor.stat.start <= compositor.stat.start && supervisor.exe == host.exe &&
                same_image(supervisor.image, host.image), "private_supervisor_unproven");
      private_context_t result {std::move(lease), std::move(backend), generation, boot, read.group_namespace, host, supervisor, compositor};
      validate_session(read, result);
      return result;
    }

    /**
     * Whether a task descends from this generation's supervisor, read from /proc stat alone, which
     * stays readable for a protected task. A task, or an ancestor, born before the supervisor
     * cannot descend from it, and a chain that reaches the root without it is outside. A parent
     * younger than its child (a reused PID), an unreadable or vanished ancestor and an edge that
     * changes before the bookend are unproven: they refuse, never count as outside.
     */
    bool generation_member(reader_t &read, const stat_t &task, const private_context_t &session) {
      const auto &supervisor = session.supervisor.stat;
      auto current = task;
      std::set<std::uint64_t> visited;
      std::vector<stat_t> edges;
      bool member = false;
      for (std::size_t depth = 0;; ++depth) {
        require(depth < 4096 && visited.insert(current.pid).second, "ambiguous_private_ancestry");
        if (current.pid == supervisor.pid) {
          require(current.start == supervisor.start, "private_supervisor_changed");
          member = true;
          break;
        }
        if (current.start < supervisor.start || current.parent <= 1) {
          break;
        }
        const auto parent = parse_stat(read.read(fs::path("/proc") / std::to_string(current.parent) / "stat"));
        require(parent.pid == current.parent && parent.start <= current.start &&
                  parent.state != 'Z' && parent.state != 'X' && parent.state != 'x', "private_ancestry_unproven");
        edges.push_back(current);
        current = parent;
      }
      // Reparenting or exit during the walk leaves the answer unproven.
      for (const auto &edge : edges) {
        const auto again = parse_stat(read.read(fs::path("/proc") / std::to_string(edge.pid) / "stat"));
        require(again.pid == edge.pid && again.start == edge.start && again.parent == edge.parent, "private_ancestry_changed");
      }
      return member;
    }

    bool in_private_session(reader_t &read, const process_t &process, const private_context_t &session,
                            std::vector<process_t> &ancestors) {
      // Stat ancestry settles membership first, so a copy outside, such as the same title on the
      // desktop, is skipped without reading its protected desktop ancestors. A member keeps the
      // exact lineage validation below.
      if (!generation_member(read, process.stat, session)) {
        return false;
      }
      auto current = process.stat;
      std::set<std::uint64_t> visited;
      for (std::size_t depth = 0; depth < 4096; ++depth) {
        require(visited.insert(current.pid).second, "ambiguous_private_ancestry");
        if (current.parent <= 1) {
          return false;
        }
        if (current.parent == session.supervisor.stat.pid) {
          require(session.supervisor.stat.start <= current.start, "private_parent_younger_than_child");
          return true;
        }
        auto parent = session_process(read, current.parent);
        require(parent.stat.start <= current.start, "private_parent_younger_than_child");
        current = parent.stat;
        ancestors.push_back(std::move(parent));
      }
      throw refusal_t("private_ancestry_too_deep");
    }

    observation_t snapshot(const title_t &title, backend_t &backend, const private_context_t *session = nullptr,
                           std::uint64_t not_before_ticks = 0) {
      reader_t read {backend};
      if (session) {
        validate_session(read, *session);
        require(not_before_ticks > 0, "invalid_private_dispatch_bound");
      }
      require(token(title.namespace_id) && token(title.item_id) && token(title.artifact_id), "invalid_title_identity");
      require(title.platform == "Windows" || title.platform == "Linux", "unknown_title_runtime");
      require(safe_absolute_path(title.install_dir) && safe_absolute_path(title.data_root), "invalid_title_path");
      const fs::path relative = title.launch_executable;
      require(!relative.empty() && !relative.is_absolute() && safe_absolute_path(fs::path("/") / relative) &&
                title.launch_executable.find('\\') == std::string::npos, "invalid_title_executable");
      const auto install = read.canonical(title.install_dir);
      const auto data = read.canonical(title.data_root);
      const auto image = read.canonical(install / relative);
      require(inside(image, install) && image != install, "title_image_outside_installation");
      const auto image_file = read.stat(image);
      const auto install_file = read.stat(install);
      const auto data_file = read.stat(data);
      require(safe_file(image_file, backend.owner_uid(), title.platform == "Linux"), "unsafe_title_image");
      const auto magic = needed(backend.prefix(image, 4), "unreadable_title_image");
      const bool native = title.platform == "Linux";
      require(native ? magic == std::string("\177ELF", 4) : magic.size() == 4 && magic.substr(0, 2) == "MZ", "unsupported_title_image");
      const auto boot = read.boot();
      const auto own_group = cgroup(read.read("/proc/self/cgroup"));
      require(own_group != "/", "unknown_polaris_cgroup");
      read.private_census = session != nullptr;
      const auto processes = read.table(image);
      std::vector<process_t> ancestors;
      if (session) {
        // A protected owner task, including one whose proc directory is not the owner's, is
        // irrelevant only when stat ancestry proves it outside this generation. A member, or a
        // task whose ancestry cannot be proved, still refuses. This proves only the protected
        // tasks the census recorded, never that it recorded every one.
        for (const auto &task : read.uninspected) {
          require(!generation_member(read, task.identity, *session), "private_process_inventory_incomplete");
        }
      }
      const process_t *game = nullptr;
      for (const auto &process : processes) {
        if (!process.maps.empty() || process.exe == image) {
          if (session) {
            if (!in_private_session(read, process, *session, ancestors)) {
              continue;
            }
            require(process.stat.start >= not_before_ticks, "game_predates_private_dispatch");
            require(inside(process.group, session->supervisor.group), "private_process_cgroup_mismatch");
            require(process.stat.pid != session->host.stat.pid && process.stat.pid != session->supervisor.stat.pid &&
                      process.stat.pid != session->compositor.stat.pid && process.exe.filename() != "EpicGamesLauncher" &&
                      process.exe.filename() != "wineserver", "private_game_identity_unproven");
          }
          require(!game, "ambiguous_game_process");
          game = &process;
        }
      }
      require(game != nullptr, "game_not_observed");
      require(!game->maps.empty() && game->cwd == install && (session || !service_cgroup(game->group, own_group)), "game_identity_incomplete");
      require(safe_file(game->image, backend.owner_uid(), true), "unsafe_game_executable");
      const auto process_path = fs::path("/proc") / std::to_string(game->stat.pid);
      require(same_image(image_file, read.stat(process_path / "root" / image.relative_path())), "game_process_root_mismatch");
      const auto mount = image_mount(read.read(process_path / "mountinfo", mount_limit), image);
      digest_t digest;
      digest.add("epic-owner-consent-v1");
      digest.add(title.namespace_id);
      digest.add(title.item_id);
      digest.add(title.artifact_id);
      digest.add(install.string());
      digest.add(data.string());
      digest.add(image.string());
      digest.add(title.platform);
      digest.add(backend.owner_uid());
      digest.add(boot);
      if (session) {
        digest.add("epic-private-candidate-v1");
        digest.add(session->generation);
        digest.process(session->host);
        digest.process(session->supervisor);
        digest.process(session->compositor);
        digest.add(not_before_ticks);
      }
      // The confirmation describes this exact observation, including its inspection limits.
      // A changed protected set invalidates consent; none of these rows can supply a target.
      // A private observation has only tasks proved outside its generation here, so they are
      // no part of its revision: an SSH login or a polkit prompt cannot revoke its grant.
      if (!session) {
        std::sort(read.uninspected.begin(), read.uninspected.end(), [](const auto &a, const auto &b) {
          return a.identity.pid < b.identity.pid;
        });
        digest.add(read.uninspected.size());
        for (const auto &process : read.uninspected) {
          digest.add(process.identity.pid);
          digest.add(process.identity.start);
          digest.file(process.directory, true);
        }
      }
      digest.process(*game);
      digest.file(image_file);
      digest.file(install_file, true);
      digest.file(data_file, true);
      digest.add(mount.identity);
      for (const auto &map : game->maps) {
        require(map.inode == image_file.inode && map.major == mount.major && map.minor == mount.minor, "mapped_game_identity_mismatch");
        // Btrfs subvolumes have a different stat st_dev from their VMA superblock device.
        require(mount.type == "btrfs" || (map.major == major(image_file.device) && map.minor == minor(image_file.device)), "mapped_game_device_mismatch");
        require(read.link(process_path / "map_files" / map.range) == image, "mapped_game_link_mismatch");
        digest.add(map.range);
        digest.add(map.major);
        digest.add(map.minor);
        digest.add(map.inode);
      }
      const process_t *server = nullptr;
      std::vector<std::pair<fs::path, file_t>> directories;
      if (native) {
        require(game->exe == image && same_image(game->image, image_file), "native_executable_mismatch");
        digest.add("native");
      }
      else {
        const auto base = read.canonical(data / "compat/runtime");
        require(base == data / "compat/runtime", "redirected_runtime_root");
        const auto game_runtime = runtime(game->exe, base, "wine64-preloader");
        require(game_runtime.has_value(), "unknown_game_runtime");
        require(read.canonical(game->exe) == game->exe, "redirected_game_runtime");
        const auto prefix_path = data / "compat/compatdata" / title.artifact_id / "pfx";
        require(read.canonical(prefix_path) == prefix_path, "redirected_title_prefix");
        const auto prefix = read.stat(prefix_path);
        require(S_ISDIR(prefix.mode) && prefix.uid == backend.owner_uid() && prefix.inode > 0, "unsafe_title_prefix");
        std::ostringstream server_name;
        server_name << "server-" << std::hex << prefix.device << '-' << prefix.inode;
        const auto server_dir = fs::path("/tmp") / (".wine-" + std::to_string(backend.owner_uid())) / server_name.str();
        for (const auto &process : processes) {
          if (process.cwd == server_dir) {
            if (session) {
              if (!in_private_session(read, process, *session, ancestors)) {
                continue;
              }
              require(process.stat.start >= not_before_ticks, "prefix_server_predates_private_dispatch");
              require(inside(process.group, session->supervisor.group), "private_process_cgroup_mismatch");
            }
            require(!server, "ambiguous_prefix_server");
            require(runtime(process.exe, base, "wineserver") == game_runtime && (session || !service_cgroup(process.group, own_group)), "prefix_runtime_mismatch");
            require(safe_file(process.image, backend.owner_uid(), true) && read.canonical(process.exe) == process.exe, "unsafe_prefix_runtime");
            server = &process;
          }
        }
        require(server != nullptr, "prefix_server_not_observed");
        digest.add("proton");
        digest.add(game_runtime->string());
        const auto runtime_file = read.stat(*game_runtime);
        require(S_ISDIR(runtime_file.mode) && (runtime_file.uid == backend.owner_uid() || runtime_file.uid == 0), "unsafe_runtime_directory");
        digest.file(runtime_file, true);
        digest.add(prefix_path.string());
        digest.file(prefix, true);
        digest.process(*server);
        directories.emplace_back(*game_runtime, runtime_file);
        directories.emplace_back(prefix_path, prefix);
      }
      // Bookend process and file identity after maps/root/runtime reads as well as inside table().
      for (const auto &[path, identity] : directories) {
        require(read.canonical(path) == path && same_file(identity, read.stat(path)), "runtime_or_prefix_changed");
      }
      require(!server || read.unchanged(*server), "prefix_server_changed");
      const auto last_maps = image_maps(read.read(process_path / "maps", maps_limit), image);
      require(last_maps.size() == game->maps.size(), "game_mappings_changed");
      for (std::size_t i = 0; i < last_maps.size(); ++i) {
        const auto &a = last_maps[i];
        const auto &b = game->maps[i];
        require(a.range == b.range && a.major == b.major && a.minor == b.minor && a.inode == b.inode &&
                  read.link(process_path / "map_files" / a.range) == image, "game_mappings_changed");
      }
      require(read.unchanged(*game) && same_image(image_file, read.stat(image)) &&
                same_image(image_file, read.stat(process_path / "root" / image.relative_path())) &&
                image_mount(read.read(process_path / "mountinfo", mount_limit), image).identity == mount.identity &&
                read.canonical(title.install_dir) == install && read.canonical(title.data_root) == data &&
                same_file(install_file, read.stat(install)) && same_file(data_file, read.stat(data)) && boot == read.boot(), "game_changed_during_observation");
      // The owner-consent set stays exact. A task proved outside a generation cannot join it,
      // whatever it runs next, but its PID could now name a new task inside, so it must still be
      // the same task.
      for (const auto &process : read.uninspected) {
        require(session ? read.still_same_task(process) : read.still_uninspected(process), "protected_process_changed");
      }
      if (session) {
        // Parent edges are identity too: reparenting cannot retain private authority.
        require(unchanged_session_process(read, *game) && (!server || unchanged_session_process(read, *server)), "private_ancestry_changed");
        digest.add(game->stat.parent);
        if (server) {
          digest.add(server->stat.parent);
        }
        std::sort(ancestors.begin(), ancestors.end(), [](const auto &a, const auto &b) {
          return a.stat.pid < b.stat.pid;
        });
        for (const auto &ancestor : ancestors) {
          require(unchanged_session_process(read, ancestor), "private_ancestry_changed");
          digest.process(ancestor);
          digest.add(ancestor.stat.parent);
        }
        validate_session(read, *session);
      }
      return {true, session ? "private_candidate_requires_dispatch_authority" : "owner_confirmation_required", digest.finish(), native ? "native" : "proton", game->stat.pid, game->stat.start, read.uninspected.size()};
    }

    observation_t safe_snapshot(const title_t &title, backend_t &backend, const private_context_t *session = nullptr,
                                std::uint64_t not_before_ticks = 0) {
      try {
        return snapshot(title, backend, session, not_before_ticks);
      }
      catch (const refusal_t &error) {
        return {false, error.what(), "", "unknown", 0, 0};
      }
      catch (const std::exception &) {
        return {false, "process_observation_failed", "", "unknown", 0, 0};
      }
    }

    std::optional<bool> runtime_absent(const title_t &title, std::string &reason, backend_t &backend) {
      reason.clear();
      try {
        reader_t read {backend};
        require(token(title.namespace_id) && token(title.item_id) && token(title.artifact_id), "invalid_title_identity");
        require(title.platform == "Windows" || title.platform == "Linux", "unknown_title_runtime");
        require(safe_absolute_path(title.install_dir) && safe_absolute_path(title.data_root), "invalid_title_path");
        const fs::path relative = title.launch_executable;
        require(!relative.empty() && !relative.is_absolute() && safe_absolute_path(fs::path("/") / relative) &&
                  title.launch_executable.find('\\') == std::string::npos, "invalid_title_executable");
        const auto install = read.canonical(title.install_dir);
        const auto data = read.canonical(title.data_root);
        const auto image = read.canonical(install / relative);
        require(inside(image, install) && image != install, "title_image_outside_installation");
        const auto image_file = read.stat(image);
        const auto install_file = read.stat(install);
        const auto data_file = read.stat(data);
        const auto owner = backend.owner_uid();
        const bool native = title.platform == "Linux";
        require(safe_file(image_file, owner, native), "unsafe_title_image");
        require(S_ISDIR(install_file.mode) && S_ISDIR(data_file.mode) && install_file.inode > 0 && data_file.inode > 0 &&
                  (install_file.uid == owner || install_file.uid == 0) && data_file.uid == owner &&
                  ((install_file.mode | data_file.mode) & (S_IWGRP | S_IWOTH)) == 0, "unsafe_title_directory");
        const auto magic = needed(backend.prefix(image, 4), "unreadable_title_image");
        require(native ? magic == std::string("\177ELF", 4) : magic.size() == 4 && magic.substr(0, 2) == "MZ", "unsupported_title_image");
        fs::path prefix_path, server_dir;
        file_t prefix {};
        if (!native) {
          prefix_path = data / "compat/compatdata" / title.artifact_id / "pfx";
          // A title that has never run has no prefix yet, so its first play is in Mirror Desktop
          // (N3). This backend cannot tell a missing prefix from an IO failure; both are unknown.
          const auto resolved = backend.canonical(prefix_path);
          require(resolved.has_value(), "title_prefix_unavailable");
          require(*resolved == prefix_path, "redirected_title_prefix");
          prefix = read.stat(prefix_path);
          require(S_ISDIR(prefix.mode) && prefix.uid == owner && prefix.inode > 0 &&
                    (prefix.mode & (S_IWGRP | S_IWOTH)) == 0, "unsafe_title_prefix");
          std::ostringstream server_name;
          server_name << "server-" << std::hex << prefix.device << '-' << prefix.inode;
          server_dir = fs::path("/tmp") / (".wine-" + std::to_string(owner)) / server_name.str();
        }
        const auto boot = read.boot();
        auto names = needed(backend.list("/proc", process_limit), "incomplete_process_table");
        std::sort(names.begin(), names.end());
        // Rows are only the tasks that map or run the title image, and Wine servers. Every other
        // readable task gets the stable negative the game observation already relies on, so a
        // Flatpak, a deleted host path or a foreign namespace elsewhere cannot make this unknown.
        const auto processes = read.table(image);
        // A protected task can be neither row only by its session or system role (D-A (B)).
        read.place_protected("title_runtime_inventory_incomplete");
        bool busy = false;
        for (const auto &process : processes) {
          busy = busy || process.exe == image || !process.maps.empty() || (!native && process.cwd == server_dir);
        }
        // Bookend each row's image mapping by presence: its whole maps text changes with any allocation.
        for (const auto &process : processes) {
          const auto path = fs::path("/proc") / std::to_string(process.stat.pid);
          require(read.unchanged(process) && image_maps(read.read(path / "maps", maps_limit), image).empty() == process.maps.empty(),
                  "title_runtime_inventory_changed");
        }
        read.recheck_placed("title_runtime_inventory_changed");
        auto last_names = needed(backend.list("/proc", process_limit), "incomplete_process_table");
        std::sort(last_names.begin(), last_names.end());
        require(names == last_names && owner == backend.owner_uid() && boot == read.boot() &&
                  backend.link("/proc/self/ns/cgroup") == read.group_namespace, "title_runtime_inventory_changed");
        require(read.canonical(title.install_dir) == install && read.canonical(title.data_root) == data &&
                  read.canonical(install / relative) == image && same_image(image_file, read.stat(image)) &&
                  same_file(install_file, read.stat(install)) && same_file(data_file, read.stat(data)), "title_runtime_input_changed");
        require(native || (read.canonical(prefix_path) == prefix_path && same_image(prefix, read.stat(prefix_path))), "title_prefix_changed");
        reason = busy ? "title_runtime_busy" : "title_runtime_absent";
        return !busy;
      }
      catch (const refusal_t &error) {
        reason = error.what();
      }
      catch (const std::exception &) {
        reason = "title_runtime_observation_failed";
      }
      return std::nullopt;
    }

    bool same_observation(const observation_t &a, const observation_t &b) {
      return a.available && b.available && !a.revision.empty() && a.revision == b.revision &&
             a.pid == b.pid && a.start_ticks == b.start_ticks && a.runtime == b.runtime;
    }

    /** A task's own /proc/<pid>/exe handle, which opens the executable that task runs. */
    bool task_executable_handle(const fs::path &path) {
      const auto task = path.parent_path();
      const auto pid = task.filename().string();
      return path.filename() == "exe" && task.parent_path() == "/proc" && !pid.empty() &&
             std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    std::optional<std::string> read_file(const fs::path &path, std::size_t limit, bool prefix, int *open_error = nullptr) {
      if (limit == 0 || limit > maps_limit) {
        return std::nullopt;
      }
      // Only a task's executable handle is followed: the kernel resolves it to that exact file,
      // under the same ptrace read check as its link. Every other path keeps O_NOFOLLOW.
      const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | (task_executable_handle(path) ? 0 : O_NOFOLLOW));
      if (fd < 0) {
        if (open_error) {
          *open_error = errno;
        }
        return std::nullopt;
      }
      struct fd_guard_t {
        int fd;
        ~fd_guard_t() { ::close(fd); }
      } guard {fd};
      struct stat info {};
      if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        return std::nullopt;
      }
      // Procfs reports st_size == 0 for maps/status/stat. Always read to EOF or the cap.
      std::string result;
      std::array<char, 4096> buffer;
      const auto cap = prefix ? limit : limit + 1;
      while (result.size() < cap) {
        const auto count = ::read(fd, buffer.data(), std::min(buffer.size(), cap - result.size()));
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count < 0) {
          return std::nullopt;
        }
        if (count == 0) {
          break;
        }
        result.append(buffer.data(), static_cast<std::size_t>(count));
      }
      return result.size() > limit || (prefix && result.size() != limit) ? std::nullopt : std::make_optional(std::move(result));
    }

    struct linux_backend_t final: backend_t {
      std::uint64_t owner_uid() const override { return ::getuid(); }

      std::optional<std::string> read(const fs::path &path, std::size_t limit) override {
        return read_file(path, limit, false);
      }

      testing::status_result_t initial_status(const fs::path &process, std::size_t limit) override {
        int open_error = 0;
        auto text = read_file(process / "status", limit, false, &open_error);
        if (text) {
          return {std::move(text)};
        }
        struct stat info {};
        if (open_error != ENOENT || ::lstat(process.c_str(), &info) == 0 || errno != ENOENT) {
          return {};
        }
#ifdef SYS_pidfd_open
        // Procfs may hide a live process as ENOENT. Require kernel absence too.
        // A reused/live PID supplies no census identity or close authority: close
        // the probe immediately and refuse. No retry follows either result.
        const int probe = open_pidfd(number(process.filename().string()));
        if (probe >= 0) {
          ::close(probe);
          return {};
        }
        if (errno == ESRCH) {
          return {std::nullopt, testing::process_absence_t::vanished};
        }
#endif
        return {};
      }

      std::optional<std::string> prefix(const fs::path &path, std::size_t length) override {
        return read_file(path, length, true);
      }

      std::optional<fs::path> link(const fs::path &path) override {
        return inspect_link(path).target;
      }

      testing::link_result_t inspect_link(const fs::path &path) override {
        std::array<char, 4097> buffer;
        const auto count = ::readlink(path.c_str(), buffer.data(), buffer.size());
        if (count < 0) {
          return {std::nullopt, errno == EACCES || errno == EPERM};
        }
        if (count <= 0 || static_cast<std::size_t>(count) == buffer.size()) {
          return {std::nullopt, false};
        }
        return {fs::path(std::string(buffer.data(), static_cast<std::size_t>(count))), false};
      }

      std::optional<fs::path> canonical(const fs::path &path) override {
        std::error_code error;
        auto result = fs::canonical(path, error);
        return error ? std::nullopt : std::make_optional(result);
      }

      std::optional<file_t> stat(const fs::path &path) override {
        struct stat value {};
        if (::stat(path.c_str(), &value) != 0) {
          return std::nullopt;
        }
        return file_t {value.st_dev, value.st_ino, value.st_uid, value.st_mode, static_cast<std::uint64_t>(value.st_size),
                       value.st_mtim.tv_sec * 1000000000LL + value.st_mtim.tv_nsec, value.st_ctim.tv_sec * 1000000000LL + value.st_ctim.tv_nsec};
      }

      std::optional<std::vector<std::string>> list(const fs::path &path, std::size_t limit) override {
        std::vector<std::string> result;
        std::error_code error;
        fs::directory_iterator it(path, error), end;
        while (!error && it != end) {
          if (result.size() == limit) {
            return std::nullopt;
          }
          result.push_back(it->path().filename().string());
          it.increment(error);
        }
        return error ? std::nullopt : std::make_optional(result);
      }

      int open_pidfd(std::uint64_t pid) override {
#ifdef SYS_pidfd_open
        return static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
#else
        return -1;
#endif
      }

      std::optional<std::uint64_t> pidfd_pid(int fd) override {
        const auto info = read(fs::path("/proc/self/fdinfo") / std::to_string(fd), small_limit);
        if (!info) {
          return std::nullopt;
        }
        std::istringstream stream(*info);
        std::string line;
        std::optional<std::uint64_t> pid;
        try {
          while (std::getline(stream, line)) {
            if (line.compare(0, 4, "Pid:") == 0) {
              const auto fields = words(line.substr(4));
              if (pid || fields.size() != 1) {
                return std::nullopt;
              }
              pid = number(fields[0]);
            }
          }
        }
        catch (const refusal_t &) {
          return std::nullopt;
        }
        return pid;
      }

      exit_state_t wait(int fd, std::chrono::milliseconds timeout) override {
        const auto bounded = std::clamp(timeout, std::chrono::milliseconds::zero(), std::chrono::milliseconds(10000));
        const auto deadline = std::chrono::steady_clock::now() + bounded;
        for (;;) {
          pollfd process {fd, POLLIN, 0};
          const auto remaining = std::max(std::chrono::milliseconds::zero(), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
          const int result = ::poll(&process, 1, static_cast<int>(remaining.count()));
          if (result < 0 && errno == EINTR && std::chrono::steady_clock::now() < deadline) {
            continue;
          }
          if (result < 0 || (process.revents & (POLLERR | POLLNVAL | POLLHUP))) {
            return exit_state_t::unavailable;
          }
          return (process.revents & POLLIN) ? exit_state_t::exited : exit_state_t::alive;
        }
      }

      bool term(int fd) override {
#ifdef SYS_pidfd_send_signal
        return ::syscall(SYS_pidfd_send_signal, fd, SIGTERM, nullptr, 0) == 0;
#else
        return false;
#endif
      }

      void close(int fd) override { ::close(fd); }
    };

    std::shared_ptr<backend_t> production_backend() {
      return std::make_shared<linux_backend_t>();
    }

    struct appimage_keeper_t {
      const process_t *process {nullptr};
      fs::path mount_namespace, image_fd, fuse_fd;
      file_t fuse_file;
      std::string fuse_info;
    };

    fs::path mount_namespace(backend_t &backend, const fs::path &process) {
      const auto value = needed(backend.link(process / "ns/mnt"), "unknown_mount_namespace");
      const auto text = value.string();
      require(text.size() > 6 && text.compare(0, 5, "mnt:[") == 0 && text.back() == ']' &&
                number(std::string_view(text).substr(5, text.size() - 6)) > 0, "unknown_mount_namespace");
      return value;
    }

    std::optional<std::uint64_t> fuse_connection(const std::string &info) {
      std::istringstream stream(info);
      std::string line;
      std::optional<std::uint64_t> result;
      while (std::getline(stream, line)) {
        if (line.compare(0, 16, "fuse_connection:") == 0) {
          const auto fields = words(line.substr(16));
          require(!result && fields.size() == 1, "malformed_fuse_connection");
          result = number(fields[0]);
          require(*result > 0, "malformed_fuse_connection");
        }
      }
      return result;
    }

    appimage_keeper_t appimage_keeper(reader_t &read, const std::vector<process_t> &table,
                                     const process_t &launcher, const mount_t &mount,
                                     const std::string &own_group) {
      // New kernels expose the FUSE connection device ID in fdinfo. It binds the daemon to
      // the mount's minor device, unlike an AppImage basename or an open image file alone.
      // Older kernels without this metadata remain unavailable; never guess a backing path.
      const fs::path source = mount.source;
      require(mount.major == 0 && mount.type.compare(0, 5, "fuse.") == 0 &&
                source == source.filename() && safe_absolute_path(fs::path("/") / source), "unproven_fuse_mount");
      const auto launcher_proc = fs::path("/proc") / std::to_string(launcher.stat.pid);
      const auto ns = mount_namespace(read.backend, "/proc/self");
      require(mount_namespace(read.backend, launcher_proc) == ns, "foreign_launcher_mount_namespace");
      const auto fuse_file = read.stat("/dev/fuse");
      require(S_ISCHR(fuse_file.mode), "unknown_fuse_device");
      std::optional<appimage_keeper_t> result;
      for (const auto &process : table) {
        if (process.exe.filename() != source) {
          continue;
        }
        const auto proc = fs::path("/proc") / std::to_string(process.stat.pid);
        require(!service_cgroup(process.group, own_group) && process.image.uid == read.backend.owner_uid() &&
                  safe_file(process.image, read.backend.owner_uid(), true) && read.canonical(process.exe) == process.exe,
                "unsafe_appimage_keeper");
        require(mount_namespace(read.backend, proc) == ns &&
                  image_mount(read.read(proc / "mountinfo", mount_limit), launcher.exe).identity == mount.identity,
                "foreign_appimage_mount");
        const auto header = needed(read.backend.prefix(process.exe, 11), "unreadable_launcher_appimage");
        require(header.substr(0, 4) == std::string("\177ELF", 4) && header.substr(8, 3) == std::string("AI\002", 3),
                "unknown_launcher_appimage");
        const auto fds = needed(read.backend.list(proc / "fd", 256), "incomplete_appimage_descriptors");
        std::set<std::uint64_t> seen;
        appimage_keeper_t candidate {&process, ns, {}, {}, fuse_file, {}};
        for (const auto &name : fds) {
          const auto descriptor = number(name);
          require(descriptor <= INT_MAX && seen.insert(descriptor).second, "ambiguous_appimage_descriptors");
          const auto path = proc / "fd" / name;
          const auto target = needed(read.backend.link(path), "unreadable_appimage_descriptor");
          require(target.string().size() <= 4096, "unreadable_appimage_descriptor");
          if (target == process.exe) {
            require(same_image(process.image, read.stat(path)), "changed_appimage_descriptor");
            // Multiple descriptors for the same exact file add no different backing candidate.
            candidate.image_fd = path;
          }
          else if (target == "/dev/fuse") {
            require(same_file(fuse_file, read.stat(path)), "changed_fuse_descriptor");
            const auto info = read.read(proc / "fdinfo" / name, 4096);
            if (fuse_connection(info) == mount.minor) {
              require(candidate.fuse_fd.empty(), "ambiguous_fuse_descriptors");
              candidate.fuse_fd = path;
              candidate.fuse_info = info;
            }
          }
        }
        if (!candidate.image_fd.empty() && !candidate.fuse_fd.empty()) {
          require(!result, "ambiguous_appimage_keeper");
          result = std::move(candidate);
        }
      }
      require(result.has_value(), "appimage_keeper_not_proven");
      return *result;
    }

    bool unchanged_keeper(reader_t &read, const appimage_keeper_t &keeper, const process_t &launcher,
                          const mount_t &mount) {
      const auto proc = fs::path("/proc") / std::to_string(keeper.process->stat.pid);
      const auto launcher_proc = fs::path("/proc") / std::to_string(launcher.stat.pid);
      return read.unchanged(*keeper.process) &&
             mount_namespace(read.backend, "/proc/self") == keeper.mount_namespace &&
             mount_namespace(read.backend, proc) == keeper.mount_namespace &&
             mount_namespace(read.backend, launcher_proc) == keeper.mount_namespace &&
             image_mount(read.read(proc / "mountinfo", mount_limit), launcher.exe).identity == mount.identity &&
             read.canonical(keeper.process->exe) == keeper.process->exe &&
             read.backend.link(keeper.image_fd) == keeper.process->exe &&
             same_image(keeper.process->image, read.stat(keeper.image_fd)) &&
             read.backend.link(keeper.fuse_fd) == fs::path("/dev/fuse") &&
             same_file(keeper.fuse_file, read.stat(keeper.fuse_fd)) &&
             read.read(proc / "fdinfo" / keeper.fuse_fd.filename(), 4096) == keeper.fuse_info;
    }

    struct launcher_image_t {
      fs::path path;
      file_t file;
      bool appimage;
    };

    launcher_image_t launcher_path(backend_t &backend) {
      reader_t read {backend};
      const auto boot = read.boot();
      const auto own_group = cgroup(read.read("/proc/self/cgroup"));
      require(own_group != "/", "unknown_polaris_cgroup");
      const auto table = read.launchers();
      const process_t *launcher = nullptr;
      for (const auto &process : table) {
        if (process.exe.filename() == "EpicGamesLauncher") {
          require(!launcher, "ambiguous_desktop_launcher");
          launcher = &process;
        }
      }
      require(launcher != nullptr, "open_epic_on_host_desktop");
      require(!service_cgroup(launcher->group, own_group), "launcher_in_polaris_cgroup");
      require(safe_file(launcher->image, backend.owner_uid(), true) && read.canonical(launcher->exe) == launcher->exe, "unsafe_desktop_launcher");
      require(needed(backend.prefix(launcher->exe, 4), "unreadable_desktop_launcher") == std::string("\177ELF", 4), "unknown_launcher_executable");
      const auto proc = fs::path("/proc") / std::to_string(launcher->stat.pid);
      const auto mount = image_mount(read.read(proc / "mountinfo", mount_limit), launcher->exe);
      auto executable = launcher->exe;
      auto executable_file = launcher->image;
      std::optional<fs::path> backing_source;
      std::optional<appimage_keeper_t> keeper;
      bool appimage = false;
      if (mount.point.filename().string().compare(0, 7, ".mount_") == 0 || mount.type.compare(0, 4, "fuse") == 0 || mount.type == "squashfs") {
        appimage = true;
        // Do not guess Desktop/Downloads paths or use an unpacked AppImage binary without its runtime.
        const fs::path source = mount.source;
        if (safe_absolute_path(source)) {
          executable = read.canonical(source);
          backing_source = source;
        }
        else {
          try {
            keeper = appimage_keeper(read, table, *launcher, mount, own_group);
          }
          catch (const refusal_t &) {
            throw refusal_t("launcher_appimage_path_unproven");
          }
          executable = keeper->process->exe;
        }
        executable_file = read.stat(executable);
        require(executable_file.uid == backend.owner_uid() && safe_file(executable_file, backend.owner_uid(), true), "unsafe_launcher_appimage");
        const auto header = needed(backend.prefix(executable, 11), "unreadable_launcher_appimage");
        require(header.substr(0, 4) == std::string("\177ELF", 4) && header.substr(8, 3) == std::string("AI\002", 3), "unknown_launcher_appimage");
      }
      require(read.unchanged(*launcher) && boot == read.boot() &&
                (!backing_source || read.canonical(*backing_source) == executable) &&
                (!keeper || unchanged_keeper(read, *keeper, *launcher, mount)) &&
                same_image(executable_file, read.stat(executable)) &&
                image_mount(read.read(proc / "mountinfo", mount_limit), launcher->exe).identity == mount.identity, "desktop_launcher_changed");
      return {executable, executable_file, appimage};
    }
  }  // namespace

  struct launcher_proof_t::state_t {
    launcher_image_t image;
    std::uint64_t owner;
    std::shared_ptr<backend_t> backend;
  };

  struct private_session_t::state_t {
    private_context_t context;
  };

  struct adopted_game_t::state_t {
    title_t title;
    observation_t observation;
    std::shared_ptr<backend_t> backend;
    std::optional<private_context_t> session;
    std::uint64_t not_before_ticks {0};
    int fd {-1};
    mutable std::mutex mutex;
    mutable bool revoked {false};
    bool signalled {false};

    ~state_t() {
      if (fd >= 0) {
        backend->close(fd);
      }
    }

    observation_t inspect() const {
      if (revoked) {
        return {false, "adoption_revoked", "", "unknown", 0, 0};
      }
      const auto status = backend->wait(fd, std::chrono::milliseconds::zero());
      if (status != exit_state_t::alive || backend->pidfd_pid(fd) != observation.pid) {
        revoked = true;
        return {false, status == exit_state_t::exited ? "adopted_game_exited" : "retained_pidfd_unavailable", "", "unknown", 0, 0};
      }
      auto current = safe_snapshot(title, *backend, session ? &*session : nullptr, not_before_ticks);
      if (!same_observation(observation, current) || backend->wait(fd, std::chrono::milliseconds::zero()) != exit_state_t::alive || backend->pidfd_pid(fd) != observation.pid ||
          (session && !session_current(*session))) {
        revoked = true;
        return {false, current.available ? "adopted_game_identity_changed" : current.reason, "", "unknown", 0, 0};
      }
      current.reason = "managed";
      return current;
    }
  };

  namespace detail {
    struct engine_t {
      static std::optional<std::size_t> launcher_instances(
        std::string &reason, std::shared_ptr<backend_t> backend, const launcher_proof_t *known_launcher) {
        reason.clear();
        try {
          require(bool(backend), "missing_process_reader");
          if (known_launcher && !verify_launcher(*known_launcher, reason)) {
            return std::nullopt;
          }
          reader_t read {*backend};
          const auto boot = read.boot();
          const auto tasks = read.owner_tasks();
          // A protected task is not a launcher only by its session or system role (D-A (B)).
          read.place_protected("launcher_inventory_incomplete");
          std::size_t count = 0;
          for (const auto &task : tasks) {
            const auto path = fs::path("/proc") / std::to_string(task.identity.pid);
            // Read through the task's own executable handle, which also opens a Flatpak's or a
            // replaced (deleted) executable that the host cannot open by its path.
            const auto header = needed(backend->prefix(path / "exe", 11), "launcher_census_unresolved");
            bool known_image = false;
            if (known_launcher) {
              const auto &image = known_launcher->state_->image;
              known_image = task.exe == image.path && same_image(task.image, image.file);
            }
            // AppImages may be renamed and their runtime may precede the inner launcher.
            // Never turn such a readable but unassociated runtime into a proven zero.
            const bool appimage_marker = header.substr(0, 4) == std::string("\177ELF", 4) && header.substr(8, 2) == "AI";
            const bool named = task.exe.filename() == "EpicGamesLauncher";
            if (!named && !appimage_marker && !known_image) {
              // Not a launcher candidate: it needs a stable negative, not a full identity.
              require(read.stable_negative(task), "launcher_inventory_changed");
              continue;
            }
            require(!appimage_marker || known_image, "launcher_census_unresolved");
            const auto process = read.identity(task);
            require(read.unchanged(process), "launcher_inventory_changed");
            count += named || known_image;
          }
          read.recheck_placed("launcher_inventory_changed");
          require(boot == read.boot(), "launcher_inventory_changed");
          if (known_launcher && !verify_launcher(*known_launcher, reason)) {
            return std::nullopt;
          }
          return count;
        }
        catch (const refusal_t &error) {
          reason = error.what();
        }
        catch (const std::exception &) {
          reason = "launcher_inventory_failed";
        }
        return std::nullopt;
      }

      static std::optional<private_session_t> pin_private_session(
        std::shared_ptr<const vrr_clock::compositor_lease_t> lease, const std::string &generation,
        std::string &reason, std::shared_ptr<backend_t> backend) {
        reason.clear();
        try {
          auto context = private_context(std::move(lease), generation, std::move(backend));
          auto state = std::make_shared<private_session_t::state_t>(private_session_t::state_t {std::move(context)});
          return private_session_t(std::move(state));
        }
        catch (const refusal_t &error) {
          reason = error.what();
        }
        catch (const std::exception &) {
          reason = "private_session_proof_failed";
        }
        return std::nullopt;
      }

      static observation_t observe_in_session(const title_t &title, const private_session_t &session, std::uint64_t not_before_ticks) {
        if (!session.state_) {
          return {false, "missing_private_session", "", "unknown", 0, 0};
        }
        const auto &context = session.state_->context;
        return safe_snapshot(title, *context.backend, &context, not_before_ticks);
      }

      static std::unique_ptr<adopted_game_t> adopt_in_session(
        const title_t &title, const private_session_t &session, std::uint64_t not_before_ticks,
        const observation_t &observation, std::string &error) {
        if (!session.state_) {
          error = "missing_private_session";
          return nullptr;
        }
        const auto &context = session.state_->context;
        return adopt(title, observation, error, context.backend, context, not_before_ticks);
      }

      static std::optional<launcher_proof_t> prove_launcher(std::string &reason, std::shared_ptr<backend_t> backend) {
        reason.clear();
        try {
          require(bool(backend), "missing_process_reader");
          const auto owner = backend->owner_uid();
          auto image = launcher_path(*backend);
          require(owner != 0 && owner == backend->owner_uid(), "launcher_owner_changed");
          auto state = std::make_shared<launcher_proof_t::state_t>(launcher_proof_t::state_t {std::move(image), owner, backend});
          return launcher_proof_t(std::move(state));
        }
        catch (const refusal_t &error) {
          reason = error.what();
        }
        catch (const std::exception &) {
          reason = "launcher_proof_failed";
        }
        return std::nullopt;
      }

      static bool verify_launcher(const launcher_proof_t &proof, std::string &reason) {
        reason.clear();
        try {
          require(bool(proof.state_), "missing_launcher_proof");
          const auto &state = *proof.state_;
          reader_t read {*state.backend};
          const auto &image = state.image;
          require(state.owner != 0 && state.owner == state.backend->owner_uid(), "launcher_owner_changed");
          require(safe_absolute_path(image.path) && read.canonical(image.path) == image.path &&
                    safe_file(image.file, state.owner, true) && (!image.appimage || image.file.uid == state.owner) &&
                    same_image(image.file, read.stat(image.path)), "launcher_file_changed");
          const auto header = needed(state.backend->prefix(image.path, image.appimage ? 11 : 4), "unreadable_launcher_executable");
          require(header.substr(0, 4) == std::string("\177ELF", 4) &&
                    (!image.appimage || header.substr(8, 3) == std::string("AI\002", 3)), "unknown_launcher_executable");
          require(read.canonical(image.path) == image.path && same_image(image.file, read.stat(image.path)), "launcher_file_changed");
          return true;
        }
        catch (const refusal_t &error) {
          reason = error.what();
        }
        catch (const std::exception &) {
          reason = "launcher_verification_failed";
        }
        return false;
      }

      static std::unique_ptr<adopted_game_t> adopt(const title_t &title, const observation_t &observation, std::string &error, std::shared_ptr<backend_t> backend,
                                                std::optional<private_context_t> session = std::nullopt, std::uint64_t not_before_ticks = 0) {
        error.clear();
        auto current = safe_snapshot(title, *backend, session ? &*session : nullptr, not_before_ticks);
        if (!same_observation(current, observation)) {
          error = current.available ? "stale_observation" : current.reason;
          return nullptr;
        }
        auto state = std::make_unique<adopted_game_t::state_t>();
        state->title = title;
        state->observation = current;
        state->backend = std::move(backend);
        state->session = std::move(session);
        state->not_before_ticks = not_before_ticks;
        state->fd = state->backend->open_pidfd(current.pid);
        if (state->fd < 0) {
          error = "pidfd_open_failed";
          return nullptr;
        }
        // The descriptor stays owned across every subsequent check and every refusal path.
        const auto verified = state->inspect();
        if (!verified.available) {
          error = verified.reason;
          return nullptr;
        }
        return std::unique_ptr<adopted_game_t>(new adopted_game_t(std::move(state)));
      }
    };
  }

  adopted_game_t::adopted_game_t(std::unique_ptr<state_t> state): state_(std::move(state)) {}
  adopted_game_t::~adopted_game_t() = default;
  adopted_game_t::adopted_game_t(adopted_game_t &&) noexcept = default;
  adopted_game_t &adopted_game_t::operator=(adopted_game_t &&) noexcept = default;

  launcher_proof_t::launcher_proof_t(std::shared_ptr<const state_t> state): state_(std::move(state)) {}

  private_session_t::private_session_t(std::shared_ptr<const state_t> state): state_(std::move(state)) {}

  std::filesystem::path launcher_proof_t::path() const {
    return state_ ? state_->image.path : std::filesystem::path {};
  }

  observation_t observe(const title_t &title) {
    return safe_snapshot(title, *production_backend());
  }

  std::unique_ptr<adopted_game_t> adopt(const title_t &title, const observation_t &observation, std::string &error) {
    return detail::engine_t::adopt(title, observation, error, production_backend());
  }

  observation_t inspect(const adopted_game_t &game) {
    if (!game.state_) {
      return {false, "no_adoption", "", "unknown", 0, 0};
    }
    const std::lock_guard<std::mutex> lock(game.state_->mutex);
    return game.state_->inspect();
  }

  close_result_t ask_to_close(adopted_game_t &game, std::chrono::milliseconds timeout) {
    if (!game.state_) {
      return {false, false, "no_adoption"};
    }
    auto &state = *game.state_;
    const std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.inspect().available) {
      const auto exited = state.backend->wait(state.fd, std::chrono::milliseconds::zero()) == exit_state_t::exited;
      return {exited, state.signalled, exited ? "adopted_game_exited" : "close_refused_identity_changed"};
    }
    if (!state.signalled) {
      if (!state.backend->term(state.fd)) {
        state.revoked = true;
        const auto exited = state.backend->wait(state.fd, std::chrono::milliseconds::zero()) == exit_state_t::exited;
        return {exited, false, exited ? "adopted_game_exited" : "pidfd_sigterm_failed"};
      }
      state.signalled = true;
    }
    const auto status = state.backend->wait(state.fd, std::clamp(timeout, std::chrono::milliseconds::zero(), std::chrono::milliseconds(10000)));
    if (status == exit_state_t::exited) {
      state.revoked = true;
      return {true, true, "adopted_game_exited"};
    }
    if (status == exit_state_t::unavailable) {
      state.revoked = true;
    }
    return {false, true, status == exit_state_t::alive ? "close_timeout" : "retained_pidfd_unavailable"};
  }

  std::optional<fs::path> desktop_launcher_executable(std::string &reason) {
    return testing::desktop_launcher_executable(reason, production_backend());
  }

  bool desktop_launcher_running(std::string &reason) {
    return desktop_launcher_executable(reason).has_value();
  }

  std::optional<std::size_t> launcher_instances(std::string &reason, const launcher_proof_t *known_launcher) {
    return detail::engine_t::launcher_instances(reason, production_backend(), known_launcher);
  }

  std::optional<launcher_proof_t> prove_launcher(std::string &reason) {
    return detail::engine_t::prove_launcher(reason, production_backend());
  }

  bool verify_launcher(const launcher_proof_t &proof, std::string &reason) {
    return detail::engine_t::verify_launcher(proof, reason);
  }

  std::optional<bool> title_runtime_absent(const title_t &title, std::string &reason) {
    return runtime_absent(title, reason, *production_backend());
  }

  std::optional<private_session_t> pin_private_session(
    std::shared_ptr<const vrr_clock::compositor_lease_t> lease, const std::string &generation, std::string &reason) {
    return detail::engine_t::pin_private_session(std::move(lease), generation, reason, production_backend());
  }

  observation_t observe_in_session(const title_t &title, const private_session_t &session, std::uint64_t not_before_ticks) {
    return detail::engine_t::observe_in_session(title, session, not_before_ticks);
  }

  std::unique_ptr<adopted_game_t> adopt_in_session(
    const title_t &title, const private_session_t &session, std::uint64_t not_before_ticks,
    const observation_t &observation, std::string &error) {
    return detail::engine_t::adopt_in_session(title, session, not_before_ticks, observation, error);
  }

  namespace testing {
    observation_t observe(const title_t &title, std::shared_ptr<backend_t> backend) {
      return backend ? safe_snapshot(title, *backend) : observation_t {false, "missing_process_reader", "", "unknown", 0, 0};
    }

    std::unique_ptr<adopted_game_t> adopt(const title_t &title, const observation_t &observation, std::string &error, std::shared_ptr<backend_t> backend) {
      if (!backend) {
        error = "missing_process_reader";
        return nullptr;
      }
      return detail::engine_t::adopt(title, observation, error, std::move(backend));
    }

    std::optional<fs::path> desktop_launcher_executable(std::string &reason, std::shared_ptr<backend_t> backend) {
      reason.clear();
      try {
        require(bool(backend), "missing_process_reader");
        return launcher_path(*backend).path;
      }
      catch (const refusal_t &error) {
        reason = error.what();
      }
      catch (const std::exception &) {
        reason = "desktop_launcher_observation_failed";
      }
      return std::nullopt;
    }

    std::optional<std::size_t> launcher_instances(std::string &reason, std::shared_ptr<backend_t> backend, const launcher_proof_t *known_launcher) {
      return detail::engine_t::launcher_instances(reason, std::move(backend), known_launcher);
    }

    std::optional<launcher_proof_t> prove_launcher(std::string &reason, std::shared_ptr<backend_t> backend) {
      return detail::engine_t::prove_launcher(reason, std::move(backend));
    }

    std::optional<bool> title_runtime_absent(const title_t &title, std::string &reason, std::shared_ptr<backend_t> backend) {
      if (!backend) {
        reason = "missing_process_reader";
        return std::nullopt;
      }
      return runtime_absent(title, reason, *backend);
    }

    std::optional<private_session_t> pin_private_session(
      std::shared_ptr<const vrr_clock::compositor_lease_t> lease, const std::string &generation,
      std::string &reason, std::shared_ptr<backend_t> backend) {
      return detail::engine_t::pin_private_session(std::move(lease), generation, reason, std::move(backend));
    }

    std::optional<std::string> read_bounded(const fs::path &path, std::size_t limit) {
      return read_file(path, limit, false);
    }
  }  // namespace testing
}  // namespace platf::epic

#endif
