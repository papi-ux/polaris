/**
 * @file src/platform/linux/flatpak_session_instances.cpp
 * @brief Which live Flatpak instances a private session started.
 */

#include "flatpak_session_instances.h"

#ifdef __linux__

  #include <algorithm>
  #include <cctype>
  #include <cerrno>
  #include <charconv>
  #include <cstdio>
  #include <dirent.h>
  #include <fcntl.h>
  #include <linux/nsfs.h>
  #include <set>
  #include <sstream>
  #include <sys/ioctl.h>
  #include <sys/stat.h>
  #include <system_error>
  #include <unistd.h>

  #include <nlohmann/json.hpp>

using namespace std::literals;

namespace flatpak_session_instances {
  namespace {

    std::optional<std::string> read_whole_file(const std::string &path) {
      const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        return std::nullopt;
      }
      std::string bytes;
      char chunk[4096];
      for (;;) {
        const auto count = ::read(fd, chunk, sizeof(chunk));
        if (count > 0) {
          bytes.append(chunk, static_cast<std::size_t>(count));
          continue;
        }
        if (count == 0) {
          break;
        }
        if (errno == EINTR) {
          continue;
        }
        close(fd);
        return std::nullopt;
      }
      close(fd);
      return bytes;
    }

    template<typename T>
    std::optional<T> parse_number(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
      }
      T value {};
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc {} || end != text.data() + text.size() || text.empty()) {
        return std::nullopt;
      }
      return value;
    }

    std::vector<pid_t> parse_pid_list(std::string_view text) {
      std::vector<pid_t> pids;
      std::size_t start = 0;
      while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) {
          end = text.size();
        }
        if (const auto pid = parse_number<pid_t>(text.substr(start, end - start)); pid && *pid > 0) {
          pids.push_back(*pid);
        }
        start = end + 1;
      }
      return pids;
    }

    /// The stat fields after the comm, which may itself hold spaces and parentheses.
    std::optional<std::vector<std::string_view>> stat_fields_after_comm(std::string_view stat) {
      const auto comm_end = stat.rfind(')');
      if (comm_end == std::string_view::npos) {
        return std::nullopt;
      }
      std::vector<std::string_view> fields;
      std::size_t pos = comm_end + 1;
      while (pos < stat.size()) {
        while (pos < stat.size() && stat[pos] == ' ') {
          ++pos;
        }
        const auto end = std::min(stat.find(' ', pos), stat.size());
        if (end > pos) {
          fields.push_back(stat.substr(pos, end - pos));
        }
        pos = end;
      }
      return fields;
    }

    std::optional<process_t> read_process(pid_t pid, const proc_reader_t &reader) {
      const auto stat = reader.read(pid, "stat");
      if (!stat) {
        return std::nullopt;
      }
      const auto start = start_time_from_stat(*stat);
      if (!start) {
        return std::nullopt;
      }
      return process_t {pid, *start};
    }

    std::optional<pid_t> read_parent(pid_t pid, const proc_reader_t &reader) {
      const auto stat = reader.read(pid, "stat");
      return stat ? parent_from_stat(*stat) : std::nullopt;
    }

    bool same_process(const process_t &process, const proc_reader_t &reader) {
      const auto now = read_process(process.pid, reader);
      return now && now->start_time == process.start_time;
    }

    std::string trim(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
      }
      return std::string {text};
    }

    /// [Application] name= in a Flatpak instance's info key file.
    std::optional<std::string> app_id_from_info(std::string_view info) {
      bool in_application = false;
      std::istringstream lines {std::string {info}};
      std::string line;
      while (std::getline(lines, line)) {
        const auto text = trim(line);
        if (text.starts_with('[')) {
          in_application = text == "[Application]";
          continue;
        }
        if (in_application && text.starts_with("name=")) {
          auto value = trim(std::string_view {text}.substr(5));
          if (!value.empty()) {
            return value;
          }
        }
      }
      return std::nullopt;
    }

    struct bwrapinfo_t {
      pid_t child_pid = 0;
      std::uint64_t pid_namespace = 0;
    };

    std::optional<bwrapinfo_t> parse_bwrapinfo(std::string_view text) {
      try {
        const auto json = nlohmann::json::parse(text);
        if (!json.is_object() || !json.contains("child-pid") || !json.contains("pid-namespace") ||
            !json["child-pid"].is_number_integer() || !json["pid-namespace"].is_number_integer()) {
          return std::nullopt;
        }
        bwrapinfo_t info;
        info.child_pid = json["child-pid"].get<pid_t>();
        info.pid_namespace = json["pid-namespace"].get<std::uint64_t>();
        if (info.child_pid <= 1 || info.pid_namespace == 0) {
          return std::nullopt;
        }
        return info;
      } catch (const nlohmann::json::exception &) {
        return std::nullopt;
      }
    }

    std::string cgroup_basename(std::string_view cgroup) {
      const auto slash = cgroup.find_last_of('/');
      return std::string {slash == std::string_view::npos ? cgroup : cgroup.substr(slash + 1)};
    }

    /// Flatpak names each instance's scope app-flatpak-<escaped app id>-<instance>.scope. Before
    /// instance ids named scopes it used the pid of `flatpak run`, which the outer bwrap keeps.
    bool cgroup_is_instance_scope(std::string_view cgroup, const instance_t &instance) {
      const auto name = cgroup_basename(cgroup);
      const auto prefix = "app-flatpak-" + escape_unit_name(instance.app_id) + "-";
      return name == prefix + instance.id + ".scope" ||
             name == prefix + std::to_string(instance.bwrap.pid) + ".scope";
    }

    bool is_all_digits(std::string_view text) {
      return !text.empty() && std::all_of(text.begin(), text.end(), [](char ch) {
        return std::isdigit(static_cast<unsigned char>(ch));
      });
    }

    std::optional<member_t> read_member(pid_t pid, std::string_view token, const proc_reader_t &reader) {
      const auto stat = reader.read(pid, "stat");
      if (!stat) {
        return std::nullopt;
      }
      const auto start = start_time_from_stat(*stat);
      const auto parent = parent_from_stat(*stat);
      if (!start || !parent) {
        return std::nullopt;
      }
      member_t member;
      member.process = {pid, *start};
      member.parent = *parent;
      if (const auto status = reader.read(pid, "status")) {
        member.nspid = nspid_from_status(*status);
      }
      if (const auto comm = reader.read(pid, "comm")) {
        member.comm = trim(*comm);
      }
      if (!token.empty()) {
        if (const auto environ = reader.read(pid, "environ")) {
          member.carries_token = environ_carries_token(*environ, token);
        }
      }
      return member;
    }

  }  // namespace

  std::optional<std::uint64_t> start_time_from_stat(std::string_view stat) {
    const auto fields = stat_fields_after_comm(stat);
    // Field 3 is the first after the comm, so field 22 is the twentieth.
    if (!fields || fields->size() < 20) {
      return std::nullopt;
    }
    return parse_number<std::uint64_t>((*fields)[19]);
  }

  std::optional<pid_t> parent_from_stat(std::string_view stat) {
    const auto fields = stat_fields_after_comm(stat);
    if (!fields || fields->size() < 2) {
      return std::nullopt;
    }
    return parse_number<pid_t>((*fields)[1]);
  }

  std::vector<pid_t> nspid_from_status(std::string_view status) {
    std::vector<pid_t> pids;
    const auto label = status.find("NSpid:");
    if (label == std::string_view::npos || (label != 0 && status[label - 1] != '\n')) {
      return pids;
    }
    auto end = status.find('\n', label);
    if (end == std::string_view::npos) {
      end = status.size();
    }
    std::istringstream values {std::string {status.substr(label + 6, end - label - 6)}};
    long long value = 0;
    while (values >> value) {
      pids.push_back(static_cast<pid_t>(value));
    }
    return pids;
  }

  std::string cgroup_from_proc(std::string_view cgroup) {
    std::string systemd_path;
    std::istringstream lines {std::string {cgroup}};
    std::string line;
    while (std::getline(lines, line)) {
      const auto first = line.find(':');
      const auto second = first == std::string::npos ? std::string::npos : line.find(':', first + 1);
      if (second == std::string::npos) {
        continue;
      }
      const auto hierarchy = std::string_view {line}.substr(0, first);
      const auto controllers = std::string_view {line}.substr(first + 1, second - first - 1);
      const auto path = line.substr(second + 1);
      if (hierarchy == "0" && controllers.empty()) {
        return path;
      }
      if (controllers == "name=systemd") {
        systemd_path = path;
      }
    }
    return systemd_path;
  }

  std::string escape_unit_name(std::string_view app_id) {
    std::string escaped;
    escaped.reserve(app_id.size());
    for (const char ch : app_id) {
      if (std::isalnum(static_cast<unsigned char>(ch)) || ch == ':' || ch == '_' || ch == '.') {
        escaped.push_back(ch);
      } else {
        char hex[5];
        std::snprintf(hex, sizeof(hex), "\\x%02x", static_cast<unsigned char>(ch));
        escaped += hex;
      }
    }
    return escaped;
  }

  bool environ_carries_token(std::string_view environ, std::string_view token) {
    if (token.empty()) {
      return false;
    }
    const auto wanted = "POLARIS_SESSION_INSTANCE_ID="s + std::string {token};
    std::size_t start = 0;
    while (start < environ.size()) {
      auto end = environ.find('\0', start);
      if (end == std::string_view::npos) {
        end = environ.size();
      }
      if (environ.substr(start, end - start) == wanted) {
        return true;
      }
      start = end + 1;
    }
    return false;
  }

  std::string_view role_name(role_e role) {
    switch (role) {
      case role_e::launcher:
        return "launcher"sv;
      case role_e::game:
        return "game"sv;
      case role_e::helper:
        return "helper"sv;
    }
    return "helper"sv;
  }

  proc_reader_t live_proc_reader() {
    proc_reader_t reader;
    reader.read = [](pid_t pid, std::string_view name) {
      return read_whole_file("/proc/" + std::to_string(pid) + "/" + std::string {name});
    };
    reader.pid_namespace = [](pid_t pid) -> std::optional<std::uint64_t> {
      struct stat info {};
      if (stat(("/proc/" + std::to_string(pid) + "/ns/pid").c_str(), &info) != 0) {
        return std::nullopt;
      }
      return static_cast<std::uint64_t>(info.st_ino);
    };
    reader.parent_pid_namespace = [](pid_t pid) -> std::optional<std::uint64_t> {
      const int fd = open(("/proc/" + std::to_string(pid) + "/ns/pid").c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        return std::nullopt;
      }
      const int parent = ioctl(fd, NS_GET_PARENT);
      close(fd);
      if (parent < 0) {
        return std::nullopt;
      }
      struct stat info {};
      const bool read = fstat(parent, &info) == 0;
      close(parent);
      if (!read) {
        return std::nullopt;
      }
      return static_cast<std::uint64_t>(info.st_ino);
    };
    reader.cgroup_procs = [](const std::string &cgroup) -> std::optional<std::vector<pid_t>> {
      if (cgroup.empty() || cgroup.front() != '/' || cgroup.find("/..") != std::string::npos) {
        return std::nullopt;
      }
      for (const auto *root : {"/sys/fs/cgroup", "/sys/fs/cgroup/unified", "/sys/fs/cgroup/systemd"}) {
        if (const auto procs = read_whole_file(std::string {root} + cgroup + "/cgroup.procs")) {
          return parse_pid_list(*procs);
        }
      }
      return std::nullopt;
    };
    reader.all_pids = []() {
      std::vector<pid_t> pids;
      DIR *dir = opendir("/proc");
      if (!dir) {
        return pids;
      }
      while (auto *entry = readdir(dir)) {
        if (is_all_digits(entry->d_name)) {
          if (const auto pid = parse_number<pid_t>(entry->d_name)) {
            pids.push_back(*pid);
          }
        }
      }
      closedir(dir);
      return pids;
    };
    return reader;
  }

  instances_t read_live_instances(const std::filesystem::path &runtime_dir, const proc_reader_t &reader) {
    instances_t result;
    std::error_code error;
    const auto root = runtime_dir / ".flatpak";
    std::filesystem::directory_iterator entries {root, error};
    if (error) {
      return result;
    }
    for (const auto &entry : entries) {
      std::error_code entry_error;
      const auto id = entry.path().filename().string();
      if (!is_all_digits(id) || !entry.is_directory(entry_error)) {
        continue;
      }
      const auto dir = entry.path();
      const auto pid_text = read_whole_file((dir / "pid").string());
      const auto bwrap_pid = pid_text ? parse_number<pid_t>(*pid_text) : std::nullopt;
      if (!bwrap_pid || *bwrap_pid <= 1) {
        continue;  // no running sandbox yet, or never one
      }
      const auto bwrap = read_process(*bwrap_pid, reader);
      if (!bwrap) {
        continue;  // the sandbox is gone and Flatpak has not collected its record yet
      }

      instance_t instance;
      instance.id = id;
      instance.bwrap = *bwrap;
      const auto info = read_whole_file((dir / "info").string());
      const auto app_id = info ? app_id_from_info(*info) : std::nullopt;
      const auto bwrapinfo_text = read_whole_file((dir / "bwrapinfo.json").string());
      const auto bwrapinfo = bwrapinfo_text ? parse_bwrapinfo(*bwrapinfo_text) : std::nullopt;
      if (app_id) {
        instance.app_id = *app_id;
      }
      if (!app_id || !bwrapinfo) {
        // Still starting: Flatpak writes bwrapinfo.json once bwrap has made the sandbox. Its bwrap
        // is a live process nothing may signal on this record's word.
        result.unresolved.push_back(std::move(instance));
        continue;
      }
      instance.pid_namespace = bwrapinfo->pid_namespace;

      const auto init = read_process(bwrapinfo->child_pid, reader);
      const auto init_parent = init ? read_parent(init->pid, reader) : std::nullopt;
      if (!init || !init_parent || *init_parent != instance.bwrap.pid ||
          init->start_time < instance.bwrap.start_time) {
        // The init is gone, or its pid now names a process that is not this sandbox's: the record
        // is stale, and its bwrap pid was reused.
        continue;
      }
      instance.init = *init;
      const auto init_namespace = reader.pid_namespace(init->pid);
      if (!init_namespace || *init_namespace != instance.pid_namespace) {
        continue;
      }
      const auto cgroup_text = reader.read(init->pid, "cgroup");
      const auto cgroup = cgroup_text ? cgroup_from_proc(*cgroup_text) : std::string {};
      if (!cgroup_is_instance_scope(cgroup, instance)) {
        // A sandbox that is running but not in its own scope, as when Flatpak could not reach the
        // user's systemd. It is known, so nothing else signals its bwrap, but not owned.
        result.unresolved.push_back(std::move(instance));
        continue;
      }
      instance.cgroup = cgroup;
      result.live.push_back(std::move(instance));
    }
    std::sort(result.live.begin(), result.live.end(), [](const auto &a, const auto &b) {
      return a.bwrap.start_time < b.bwrap.start_time;
    });
    return result;
  }

  bool descends_through_rising_start_times(
    const process_t &process,
    const process_t &ancestor,
    const proc_reader_t &reader
  ) {
    if (ancestor.pid <= 1 || process.pid <= 1 || process.pid == ancestor.pid) {
      return false;
    }
    std::set<pid_t> visited;
    process_t current = process;
    while (current.pid > 1 && visited.insert(current.pid).second) {
      const auto stat = reader.read(current.pid, "stat");
      const auto parent_pid = stat ? parent_from_stat(*stat) : std::nullopt;
      const auto current_start = stat ? start_time_from_stat(*stat) : std::nullopt;
      if (!parent_pid || !current_start || *current_start != current.start_time) {
        return false;  // gone, or the pid names another process now
      }
      const auto parent = read_process(*parent_pid, reader);
      if (!parent || parent->start_time > current.start_time) {
        return false;  // a parent younger than its child is a reused pid
      }
      if (parent->pid == ancestor.pid) {
        return parent->start_time == ancestor.start_time;
      }
      current = *parent;
    }
    return false;
  }

  std::optional<std::vector<member_t>> scope_members(
    const instance_t &instance,
    std::string_view token,
    const std::vector<instance_t> &live,
    const proc_reader_t &reader
  ) {
    std::optional<std::vector<pid_t>> pids;
    if (!instance.cgroup.empty() && reader.cgroup_procs) {
      pids = reader.cgroup_procs(instance.cgroup);
    }
    if (!pids) {
      const bool shared = std::any_of(live.begin(), live.end(), [&instance](const auto &other) {
        return other.id != instance.id && other.pid_namespace == instance.pid_namespace;
      });
      if (shared || !reader.all_pids) {
        return std::nullopt;
      }
      pids.emplace();
      for (const auto pid : reader.all_pids()) {
        const auto ns = reader.pid_namespace(pid);
        if (ns && *ns == instance.pid_namespace) {
          pids->push_back(pid);
        }
      }
      pids->push_back(instance.bwrap.pid);
    }
    std::vector<member_t> members;
    std::set<pid_t> seen;
    for (const auto pid : *pids) {
      if (!seen.insert(pid).second) {
        continue;
      }
      if (auto member = read_member(pid, token, reader)) {
        members.push_back(std::move(*member));
      }
    }
    return members;
  }

  std::vector<member_t> app_processes(const instance_t &instance, const std::vector<member_t> &members) {
    std::vector<member_t> apps;
    for (const auto &member : members) {
      const bool bwrap = member.process.pid == instance.bwrap.pid || member.process.pid == instance.init.pid;
      if (!bwrap && member.nspid.size() >= 2 && member.nspid.back() != 1) {
        apps.push_back(member);
      }
    }
    return apps;
  }

  std::vector<instance_t> instances_of(const instances_t &instances, std::string_view app_id) {
    std::vector<instance_t> matching;
    for (const auto &instance : instances.live) {
      if (instance.app_id == app_id) {
        matching.push_back(instance);
      }
    }
    return matching;
  }

  bool is_instance_bwrap(const process_t &process, const std::vector<instance_t> &instances) {
    return std::any_of(instances.begin(), instances.end(), [&process](const auto &instance) {
      const auto matches = [&process](const process_t &known) {
        return known.pid == process.pid && known.start_time == process.start_time;
      };
      return matches(instance.bwrap) || (instance.init.pid > 0 && matches(instance.init));
    });
  }

  attribution_t attribute(const instances_t &instances, const evidence_t &evidence, const proc_reader_t &reader) {
    attribution_t result;
    result.unresolved = instances.unresolved;

    enum class state_e {
      pending,
      owned,
      never,
    };

    struct candidate_t {
      const instance_t *instance;
      state_e state = state_e::pending;
      role_e role = role_e::helper;
      rule_e rule = rule_e::shared_namespace;
      std::optional<std::uint64_t> parent_namespace;
    };

    std::vector<candidate_t> candidates;
    for (const auto &instance : instances.live) {
      candidate_t candidate {&instance};
      if (evidence.supervisor.pid <= 1 || instance.bwrap.start_time < evidence.supervisor.start_time ||
          !same_process(evidence.supervisor, reader)) {
        // Older than the session, or no live session to compare with: never the session's.
        candidate.state = state_e::never;
      } else if (descends_through_rising_start_times(instance.bwrap, evidence.supervisor, reader)) {
        candidate.state = state_e::owned;
        candidate.role = role_e::launcher;
        candidate.rule = rule_e::lineage;
      } else if (const auto members = scope_members(instance, evidence.token, instances.live, reader);
                 members && std::any_of(members->begin(), members->end(), [](const auto &member) {
                   return member.carries_token;
                 })) {
        candidate.state = state_e::owned;
        candidate.role = role_e::game;
        candidate.rule = rule_e::token;
      }
      if (candidate.state == state_e::pending && reader.parent_pid_namespace) {
        candidate.parent_namespace = reader.parent_pid_namespace(instance.init.pid);
      }
      candidates.push_back(candidate);
    }

    // R2 until nothing changes: an instance in, or nested in, an owned instance's pid namespace
    // lives and dies with it, as a Chromium helper sub-sandbox does with its launcher.
    for (bool changed = true; changed;) {
      changed = false;
      for (auto &candidate : candidates) {
        if (candidate.state != state_e::pending) {
          continue;
        }
        const bool shares = std::any_of(candidates.begin(), candidates.end(), [&candidate](const auto &owner) {
          return owner.state == state_e::owned &&
                 (owner.instance->pid_namespace == candidate.instance->pid_namespace ||
                  (candidate.parent_namespace && *candidate.parent_namespace == owner.instance->pid_namespace));
        });
        if (shares) {
          candidate.state = state_e::owned;
          candidate.role = role_e::helper;
          candidate.rule = rule_e::shared_namespace;
          changed = true;
        }
      }
    }

    std::set<std::string> session_app_ids {evidence.launched_app_ids.begin(), evidence.launched_app_ids.end()};
    for (const auto &candidate : candidates) {
      if (candidate.state == state_e::owned) {
        result.owned.push_back({*candidate.instance, candidate.role, candidate.rule});
        session_app_ids.insert(candidate.instance->app_id);
      }
    }
    for (const auto &candidate : candidates) {
      if (candidate.state == state_e::owned) {
        continue;
      }
      ++result.left_alone;
      if (session_app_ids.contains(candidate.instance->app_id)) {
        result.left_alone_same_app.push_back(*candidate.instance);
      }
    }
    return result;
  }

}  // namespace flatpak_session_instances

#endif
