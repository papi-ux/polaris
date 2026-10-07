/**
 * @file src/steamos_session.cpp
 * @brief #204 SteamOS session switch: the portable rules.
 */
#include "steamos_session.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>

#ifndef _WIN32
  #include <sys/stat.h>
#endif

namespace prelaunch::steamos {
  namespace {
    bool all_digits(std::string_view text) {
      return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return c >= '0' && c <= '9';
      });
    }

    std::optional<std::string> read_small(const std::filesystem::path &path, std::size_t limit) {
      std::ifstream input {path, std::ios::binary};
      if (!input) return std::nullopt;
      std::string text;
      text.resize(limit + 1);
      input.read(text.data(), static_cast<std::streamsize>(text.size()));
      const auto got = static_cast<std::size_t>(input.gcount());
      if (got > limit) return std::nullopt;  // larger than any real record: refuse, never truncate
      text.resize(got);
      return text;
    }

    /// pacman section text: "%KEY%\nvalue\nvalue\n\n%NEXT%...". Each key's values, in order.
    std::optional<std::map<std::string, std::vector<std::string>>> sections(std::string_view text) {
      std::map<std::string, std::vector<std::string>> out;
      std::string current;
      bool in_section = false;
      std::size_t start = 0;
      while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') return std::nullopt;
        if (line.empty()) {
          in_section = false;
        } else if (line.size() > 2 && line.front() == '%' && line.back() == '%') {
          current = std::string {line.substr(1, line.size() - 2)};
          if (out.contains(current)) return std::nullopt;  // a repeated section is ambiguous
          out[current];
          in_section = true;
        } else if (in_section) {
          out[current].emplace_back(line);
        } else {
          return std::nullopt;  // a value outside any section
        }
        if (end == text.size()) break;
        start = end + 1;
      }
      return out;
    }
  }  // namespace

  std::optional<package_version_t> parse_package_version(std::string_view text) {
    package_version_t version;
    if (const auto colon = text.find(':'); colon != std::string_view::npos) {
      const auto epoch = text.substr(0, colon);
      if (!all_digits(epoch) || epoch.size() > 9) return std::nullopt;
      std::from_chars(epoch.data(), epoch.data() + epoch.size(), version.epoch);
      text.remove_prefix(colon + 1);
    }
    const auto dash = text.rfind('-');
    if (dash == std::string_view::npos || dash == 0) return std::nullopt;
    const auto pkgver = text.substr(0, dash);
    const auto pkgrel = text.substr(dash + 1);
    if (!std::all_of(pkgver.begin(), pkgver.end(), [](char c) {
          return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '+';
        })) {
      return std::nullopt;
    }
    const auto dot = pkgrel.find('.');
    if (dot == std::string_view::npos ? !all_digits(pkgrel) : (!all_digits(pkgrel.substr(0, dot)) || !all_digits(pkgrel.substr(dot + 1)))) {
      return std::nullopt;
    }
    version.pkgver = std::string {pkgver};
    version.pkgrel = std::string {pkgrel};
    return version;
  }

  std::optional<package_record_t> parse_package_desc(std::string_view desc) {
    const auto parsed = sections(desc);
    if (!parsed) return std::nullopt;
    const auto name = parsed->find("NAME");
    const auto version = parsed->find("VERSION");
    if (name == parsed->end() || version == parsed->end() || name->second.size() != 1 || version->second.size() != 1) {
      return std::nullopt;
    }
    const auto parsed_version = parse_package_version(version->second.front());
    if (!parsed_version) return std::nullopt;
    package_record_t record;
    record.name = name->second.front();
    record.version = *parsed_version;
    return record;
  }

  std::optional<std::vector<std::string>> parse_package_files(std::string_view files) {
    const auto parsed = sections(files);
    if (!parsed) return std::nullopt;
    const auto found = parsed->find("FILES");
    if (found == parsed->end() || found->second.empty()) return std::nullopt;
    return found->second;
  }

  verdict_t read_installed_package(const std::filesystem::path &local_db, std::string_view name, package_record_t &out) {
    std::error_code ec;
    std::optional<std::filesystem::path> match;
    int candidates = 0;
    const std::string prefix = std::string {name} + "-";
    for (std::filesystem::directory_iterator it {local_db, ec}, end; !ec && it != end; it.increment(ec)) {
      const auto dir_name = it->path().filename().string();
      if (!dir_name.starts_with(prefix)) continue;
      // "<name>-<pkgver>-<pkgrel>": exactly two dashes after the name, so steamos-manager-foo-1-1
      // (another package) is not taken for this one.
      const auto rest = std::string_view {dir_name}.substr(prefix.size());
      if (std::count(rest.begin(), rest.end(), '-') != 1) continue;
      ++candidates;
      match = it->path();
    }
    if (ec) return verdict_t::fail("prelaunch_adapter_package_unreadable", "The installed package database could not be read.");
    if (candidates != 1 || !match) {
      return verdict_t::fail("prelaunch_adapter_package_unavailable",
        candidates == 0 ? "steamos-manager is not installed." : "More than one steamos-manager record is installed.");
    }
    const auto desc = read_small(*match / "desc", 64 * 1024);
    const auto files = read_small(*match / "files", 4 * 1024 * 1024);
    if (!desc || !files) return verdict_t::fail("prelaunch_adapter_package_unreadable", "The steamos-manager package record could not be read.");
    auto record = parse_package_desc(*desc);
    auto file_list = parse_package_files(*files);
    if (!record || !file_list) return verdict_t::fail("prelaunch_adapter_package_malformed", "The steamos-manager package record is malformed.");
    const auto version = record->version;
    const auto expected_dir = std::string {name} + "-" + (version.epoch ? std::to_string(version.epoch) + ":" : std::string {}) +
                              version.pkgver + "-" + version.pkgrel;
    if (record->name != name || match->filename().string() != expected_dir) {
      return verdict_t::fail("prelaunch_adapter_package_malformed", "The steamos-manager package record does not name itself.");
    }
    record->files = std::move(*file_list);
    out = std::move(*record);
    return verdict_t::pass();
  }

  verdict_t package_supported(const package_record_t &record, const std::vector<std::string> &audited) {
    if (record.version.epoch != 0 || std::find(audited.begin(), audited.end(), record.version.pkgver) == audited.end()) {
      return verdict_t::fail("prelaunch_adapter_version_unaudited",
        "steamos-manager " + record.version.pkgver + "-" + record.version.pkgrel + " is not a release whose session switch was audited.");
    }
    const auto owns = [&](std::string_view absolute) {
      const auto relative = absolute.substr(1);
      return std::find(record.files.begin(), record.files.end(), relative) != record.files.end();
    };
    if (!owns(k_steamosctl) || !owns(k_manager_executable)) {
      return verdict_t::fail("prelaunch_adapter_package_malformed", "The steamos-manager package does not own its session switch command.");
    }
    return verdict_t::pass();
  }

  verdict_t executable_trusted(const std::filesystem::path &path, std::uint32_t owner_uid, const std::filesystem::path &trusted_root) {
#ifdef _WIN32
    (void) path;
    (void) owner_uid;
    (void) trusted_root;
    return verdict_t::fail("prelaunch_command_untrusted", "This platform has no SteamOS session command.");
#else
    if (!path.is_absolute()) return verdict_t::fail("prelaunch_command_untrusted", "The session command path is not absolute.");
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) return verdict_t::fail("prelaunch_command_unavailable", "The command that leaves Game Mode is not installed on this host.");
    if (!S_ISREG(st.st_mode) || st.st_uid != owner_uid || (st.st_mode & (S_IWGRP | S_IWOTH)) || !(st.st_mode & S_IXUSR)) {
      return verdict_t::fail("prelaunch_command_untrusted", "The command that leaves Game Mode is not a trusted executable.");
    }
    for (auto dir = path.parent_path(); ; dir = dir.parent_path()) {
      struct stat parent {};
      if (::lstat(dir.c_str(), &parent) != 0 || !S_ISDIR(parent.st_mode) || parent.st_uid != owner_uid ||
          (parent.st_mode & (S_IWGRP | S_IWOTH))) {
        return verdict_t::fail("prelaunch_command_untrusted", "A directory holding the command that leaves Game Mode is not trusted.");
      }
      if (dir == trusted_root) break;
      if (dir == dir.root_path() || dir.empty()) {
        return verdict_t::fail("prelaunch_command_untrusted", "The command that leaves Game Mode is outside its trusted location.");
      }
    }
    return verdict_t::pass();
#endif
  }

  std::optional<std::string> read_text_limited(const std::filesystem::path &path, std::size_t limit) {
    return read_small(path, limit);
  }

  std::optional<std::uint64_t> process_start_ticks(const std::filesystem::path &proc_root, std::uint32_t pid) {
    if (pid <= 1) return std::nullopt;
    const auto stat = read_small(proc_root / std::to_string(pid) / "stat", 4096);
    if (!stat) return std::nullopt;
    // Fields after the parenthesised comm, which may itself hold spaces and parentheses.
    const auto close = stat->rfind(')');
    if (close == std::string::npos) return std::nullopt;
    std::istringstream fields {stat->substr(close + 1)};
    std::string field;
    for (int index = 3; index <= 22; ++index) {
      if (!(fields >> field)) return std::nullopt;
    }
    std::uint64_t ticks = 0;
    const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), ticks);
    if (error != std::errc {} || end != field.data() + field.size() || ticks == 0) return std::nullopt;
    return ticks;
  }

  std::string invocation_hex(const std::vector<std::uint8_t> &bytes) {
    if (bytes.size() != 16 || std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; })) return {};
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const auto byte : bytes) {
      out.push_back(digits[byte >> 4]);
      out.push_back(digits[byte & 0x0f]);
    }
    return out;
  }

  verdict_t bind_session(const snapshot_t &snapshot, session_identity_t &out) {
    const auto unavailable = [](std::string reason) {
      return verdict_t::fail("prelaunch_session_identity_unavailable", std::move(reason));
    };
    if (snapshot.boot_id.size() != 36) return unavailable("The boot identity could not be read.");
    if (snapshot.seat0_active_id.empty()) return unavailable("seat0 has no active session.");

    // The seat's active session must be this account's, and the account's only graphical one.
    const session_row_t *active_row = nullptr;
    int graphical = 0;
    for (const auto &row : snapshot.rows) {
      if (row.id == snapshot.seat0_active_id) active_row = &row;
      if (row.uid != snapshot.uid) continue;
      const auto record = snapshot.records.find(row.id);
      if (record == snapshot.records.end()) {
        return unavailable("A session of this account could not be read.");
      }
      if (graphical_type(record->second.type) && record->second.klass == "user") ++graphical;
    }
    if (!active_row) return unavailable("seat0's active session is not listed.");
    if (active_row->uid != snapshot.uid) {
      return verdict_t::fail("prelaunch_session_ambiguous", "The host's active session belongs to another account.");
    }
    const auto record = snapshot.records.find(snapshot.seat0_active_id);
    if (record == snapshot.records.end()) return unavailable("seat0's active session could not be read.");
    const auto &session = record->second;
    if (graphical != 1 || !graphical_type(session.type) || session.klass != "user") {
      return verdict_t::fail("prelaunch_session_ambiguous",
        "This account does not have exactly one graphical session, so leaving Game Mode cannot be bound to it.");
    }
    if (session.id != snapshot.seat0_active_id || session.uid != snapshot.uid || session.seat != "seat0" ||
        session.state != "active" || !session.active || session.remote) {
      return verdict_t::fail("prelaunch_session_ambiguous", "The account's graphical session is not the local active session on seat0.");
    }
    if (session.service != "sddm-autologin") {
      return verdict_t::fail("prelaunch_session_not_autologin",
        "The graphical session was not started by SDDM autologin, so the host would not return to it on its own.");
    }
    if (session.leader <= 1 || session.timestamp_monotonic == 0 || snapshot.leader_start_ticks == 0) {
      return unavailable("The graphical session's leader could not be identified.");
    }
    if (snapshot.target.active_state != "active" || snapshot.target.invocation.size() != 32) {
      return unavailable("The account's graphical session target is not active.");
    }
    session_identity_t identity;
    switch (game_mode_of(snapshot.game_mode)) {
      case game_mode_e::inactive: identity.mode = mode_e::desktop; break;
      case game_mode_e::active: identity.mode = mode_e::game; break;
      default: return verdict_t::fail("prelaunch_session_mode_unknown", "Game Mode is starting or stopping.");
    }
    identity.boot_id = snapshot.boot_id;
    identity.logind_id = session.id;
    identity.logind_started = session.timestamp_monotonic;
    identity.uid = session.uid;
    identity.leader_pid = session.leader;
    identity.leader_start_ticks = snapshot.leader_start_ticks;
    identity.target_invocation = snapshot.target.invocation;
    if (!identity.valid()) return unavailable("The graphical session's identity is incomplete.");
    out = identity;
    return verdict_t::pass();
  }

  bool retired(const snapshot_t &snapshot, const session_identity_t &bound) {
    if (snapshot.boot_id != bound.boot_id) return true;
    bool session_gone = true;
    for (const auto &row : snapshot.rows) {
      if (row.id != bound.logind_id) continue;
      const auto record = snapshot.records.find(row.id);
      // Listed but unreadable is not proof it ended.
      if (record == snapshot.records.end()) return false;
      session_gone = record->second.state == "closing" || record->second.timestamp_monotonic != bound.logind_started;
    }
    const bool target_moved = snapshot.target.active_state != "active" || snapshot.target.invocation != bound.target_invocation;
    return session_gone && target_moved;
  }

  game_mode_e game_mode_of(const unit_record_t &unit) {
    if (!unit.loaded || unit.active_state == "inactive" || unit.active_state == "failed") return game_mode_e::inactive;
    if (unit.active_state == "active") return game_mode_e::active;
    return game_mode_e::transitional;
  }

  std::string cgroup_v2_path(std::string_view cgroup) {
    while (!cgroup.empty() && (cgroup.back() == '\n' || cgroup.back() == '\r')) cgroup.remove_suffix(1);
    if (!cgroup.starts_with("0::/") || cgroup.find('\n') != std::string_view::npos) return {};
    return std::string {cgroup.substr(3)};
  }

  bool cgroup_is_logind(std::string_view cgroup) {
    return cgroup_v2_path(cgroup) == "/system.slice/systemd-logind.service";
  }

  bool cgroup_is_user_manager(std::string_view cgroup, std::uint32_t uid) {
    const auto id = std::to_string(uid);
    return cgroup_v2_path(cgroup) == "/user.slice/user-" + id + ".slice/user@" + id + ".service/init.scope";
  }

  verdict_t polaris_independent_of_session(const snapshot_t &snapshot) {
    const auto refuse = [](std::string reason) {
      return verdict_t::fail("prelaunch_service_not_independent", std::move(reason));
    };
    const auto &polaris = snapshot.polaris;
    if (!polaris.loaded || polaris.active_state != "active") return refuse("polaris.service is not active in this account's user manager.");
    const auto self = cgroup_v2_path(snapshot.self_cgroup);
    if (self.empty() || polaris.control_group != self) {
      return refuse("This Polaris process is not the running polaris.service.");
    }
    for (const auto *list : {&polaris.part_of, &polaris.binds_to, &polaris.requisite, &polaris.stop_propagated_from}) {
      for (const auto &unit : *list) {
        if (unit == k_graphical_target || unit == k_game_mode_unit || unit == "graphical-session-pre.target") {
          return refuse("polaris.service would stop with the graphical session (" + unit + ").");
        }
      }
    }
    if (!snapshot.stop_closure.complete) {
      return refuse("What stopping the graphical session would also stop could not be established.");
    }
    const auto &closure = snapshot.stop_closure.units;
    if (std::find(closure.begin(), closure.end(), std::string {k_polaris_unit}) != closure.end()) {
      return refuse("Stopping the graphical session would also stop polaris.service.");
    }
    return verdict_t::pass();
  }

  std::vector<std::string> command_argv(switch_e direction) {
    return {std::string {k_steamosctl}, direction == switch_e::to_desktop ? "switch-to-desktop-mode" : "switch-to-game-mode"};
  }

  std::string user_bus_address(std::uint32_t uid) {
    return "unix:path=/run/user/" + std::to_string(uid) + "/bus";
  }

  std::vector<std::string> command_env(std::uint32_t uid) {
    return {"DBUS_SESSION_BUS_ADDRESS=" + user_bus_address(uid), "PATH=/usr/bin:/bin", "LANG=C"};
  }

  verdict_t user_bus_trusted(const std::filesystem::path &runtime_dir, std::uint32_t uid) {
#ifdef _WIN32
    (void) runtime_dir;
    (void) uid;
    return verdict_t::fail("prelaunch_session_management_unavailable", "This platform has no user bus.");
#else
    struct stat dir {};
    struct stat sock {};
    if (::lstat(runtime_dir.c_str(), &dir) != 0 || !S_ISDIR(dir.st_mode) || dir.st_uid != uid || (dir.st_mode & 077)) {
      return verdict_t::fail("prelaunch_session_management_unavailable", "This account's runtime directory is not private to it.");
    }
    if (::lstat((runtime_dir / "bus").c_str(), &sock) != 0 || !S_ISSOCK(sock.st_mode) || sock.st_uid != uid) {
      return verdict_t::fail("prelaunch_session_management_unavailable", "This account's user bus is not available.");
    }
    return verdict_t::pass();
#endif
  }

  bool cgroup_is_manager_unit(std::string_view cgroup, std::uint32_t uid) {
    // cgroup v2: a single "0::<path>" line. The path is assigned by systemd, not by the process.
    const auto owned = cgroup_v2_path(cgroup);
    const std::string_view path {owned};
    const auto user = "/user.slice/user-" + std::to_string(uid) + ".slice/user@" + std::to_string(uid) + ".service/";
    return path.starts_with(user) && path.ends_with("/steamos-manager.service") && path.find("/session-") == std::string_view::npos;
  }
}  // namespace prelaunch::steamos
