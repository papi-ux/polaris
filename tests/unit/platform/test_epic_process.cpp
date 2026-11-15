/**
 * @file tests/unit/platform/test_epic_process.cpp
 * @brief Fixture-only Epic observation and retained pidfd authority regressions.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include <src/platform/linux/epic_process.h>
#include <src/launch_failure.h>
#include <src/steamos_session.h>
#include <src/vrr_compositor_identity.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {
  namespace ep = platf::epic;
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  using ep::testing::backend_t;
  using ep::testing::exit_state_t;
  using ep::testing::file_t;

  const fs::path install = "/fixture/owner/Games/A Game";
  const fs::path data = "/fixture/owner/.local/share/com.epicgames.eglv2";
  const fs::path image = install / "Game.exe";
  const fs::path runtime = data / "compat/runtime/GE-Proton10-34";
  const fs::path prefix_path = data / "compat/compatdata/artifact/pfx";
  const std::string boot = "9dd26721-4424-4907-ab80-4f023729a22f\n";

  std::string stat_line(std::uint64_t pid, std::uint64_t start, char state = 'S', std::uint64_t parent = 1,
                        const std::string &name = "game ) with (parens)", std::uint64_t flags = 0x400000) {
    std::ostringstream text;
    // The kernel comm can contain right parentheses, spaces, and left parentheses.
    text << pid << " (" << name << ") " << state << ' ' << parent;
    for (int field = 2; field < 19; ++field) {
      text << ' ' << (field == 6 ? flags : 0);
    }
    text << ' ' << start << " 0 0\n";
    return text.str();
  }

  std::string escaped(const fs::path &path) {
    std::string result;
    for (char c : path.string()) {
      result += c == ' ' ? "\\040" : std::string(1, c);
    }
    return result;
  }

  file_t regular(std::uint64_t inode, std::uint64_t uid = 1000) {
    return {makedev(0, 52), inode, uid, S_IFREG | 0755, 12345, 123, 456};
  }

  file_t directory(std::uint64_t inode, std::uint64_t uid = 1000) {
    return {makedev(0, 52), inode, uid, S_IFDIR | 0755, 4096, 111, 222};
  }

  struct fixture_lease_t final: vrr_clock::compositor_lease_t {
    fixture_lease_t(): compositor_lease_t({101, 1000, 210}) {}
    bool published {true};
    bool current() const override { return published; }
  };

  struct fixture_t final: ep::testing::backend_t {
    std::map<fs::path, std::string> files, headers;
    std::map<fs::path, fs::path> links, resolved;
    std::map<fs::path, file_t> identities;
    std::map<std::string, epic_library::flatpak_document_t> documents;
    std::map<fs::path, fs::path> portal_origins;
    std::map<fs::path, std::vector<std::string>> listings;
    std::map<std::string, ep::testing::unit_t> units;
    std::vector<std::string> unit_queries;
    std::set<fs::path> denied_links, denied_canonical;
    std::map<fs::path, int> status_errors, directory_errors, absence_probe_errors;
    std::set<fs::path> live_absence_probes;
    std::vector<fs::path> closed_absence_probes;
    std::function<void(const fs::path &)> after_read;
    std::function<void(const fs::path &)> after_missing_status;
    // After any read, link or stat that found what it asked for, and before each /proc listing.
    std::function<void(const fs::path &)> after_access;
    std::function<void()> before_listing;
    std::map<fs::path, int> access_counts;
    int proc_listings {0};
    std::vector<std::string> pids;
    std::vector<fs::path> reads;
    std::vector<std::uint64_t> opened;
    std::vector<int> signals, closed;
    std::vector<std::chrono::milliseconds> waits;
    std::function<void()> after_open;
    std::uint64_t uid {1000}, fd_pid {41};
    int fd {77};
    bool fail_signal {false}, exit_after_signal {true};
    exit_state_t exit_state {exit_state_t::alive};

    fixture_t() {
      identities[install] = directory(10);
      identities[data] = directory(11);
      identities[data / "compat/runtime"] = directory(12);
      identities[runtime] = directory(13);
      identities[prefix_path] = directory(0xe2a1113);
      identities[image] = regular(236793710);
      headers[image] = std::string("MZ\0\0", 4);
      files["/proc/sys/kernel/random/boot_id"] = boot;
      links["/proc/self/ns/cgroup"] = "cgroup:[123]";
      files["/proc/self/cgroup"] = "1:net_cls:/\n0::/user.slice/polaris.service\n";
      add_process(41, runtime / "files/bin/wine64-preloader", install, 10001);
      add_process(42, runtime / "files/bin/wineserver", "/tmp/.wine-1000/server-34-e2a1113", 10000);
      files["/proc/41/maps"] = maps();
    }

    std::string maps(std::string device = "00:22", std::uint64_t inode = 236793710) const {
      return "1000-2000 r--p 00000000 " + device + " " + std::to_string(inode) + "     " + escaped(image) + "\n" +
             "4000-5000 r-xp 00001000 " + device + " " + std::to_string(inode) + "     " + escaped(image) + "\n";
    }

    void add_process(std::uint64_t pid, const fs::path &exe, const fs::path &cwd, std::uint64_t start, std::uint64_t parent = 1) {
      const auto path = fs::path("/proc") / std::to_string(pid);
      pids.push_back(std::to_string(pid));
      identities[path] = directory(pid);
      if (!identities.count(exe)) {
        identities[exe] = regular(pid * 100);
      }
      identities[path / "exe"] = identities[exe];
      headers[exe] = std::string("\177ELF\0\0\0\0\0\0\0", 11);
      links[path / "exe"] = exe;
      links[path / "cwd"] = cwd;
      links[path / "ns/cgroup"] = "cgroup:[123]";
      links[path / "map_files/1000-2000"] = image;
      links[path / "map_files/4000-5000"] = image;
      files[path / "stat"] = stat_line(pid, start, 'S', parent);
      files[path / "status"] = "Name:\tfixture\nUid:\t1000\t1000\t1000\t1000\n";
      files[path / "cgroup"] = "0::/user.slice/app.slice/epic.scope\n";
      files[path / "maps"] = "9000-a000 rw-p 00000000 00:00 0\n";
      files[path / "mountinfo"] = "21 1 0:1 / / rw - tmpfs tmpfs rw\n52 21 0:34 /@fixture /fixture rw - btrfs /dev/fixture rw\n";
    }

    std::shared_ptr<fixture_lease_t> private_tree() {
      add_process(10, "/usr/bin/polaris-fixture", "/fixture/owner", 100);
      add_process(100, "/usr/bin/polaris-fixture", "/fixture/owner", 200, 10);
      add_process(101, "/usr/bin/labwc", "/fixture/owner", 210, 100);
      files["/proc/self/stat"] = files["/proc/10/stat"];
      for (const int pid : {10, 100, 101}) {
        files[fs::path("/proc") / std::to_string(pid) / "cgroup"] = files["/proc/self/cgroup"];
      }
      return std::make_shared<fixture_lease_t>();
    }

    void private_game(std::uint64_t pid = 81, std::uint64_t start = 400, std::uint64_t parent = 101) {
      add_process(pid, runtime / "files/bin/wine64-preloader", install, start, parent);
      const auto path = fs::path("/proc") / std::to_string(pid);
      files[path / "maps"] = maps();
      files[path / "cgroup"] = files["/proc/self/cgroup"];
    }

    void private_server(std::uint64_t start = 390) {
      add_process(82, runtime / "files/bin/wineserver", "/tmp/.wine-1000/server-34-e2a1113", start, 101);
      files["/proc/82/cgroup"] = files["/proc/self/cgroup"];
    }

    fs::path fuse_launcher() {
      const fs::path exe = "/tmp/.mount_EpicXXX/usr/bin/EpicGamesLauncher";
      const fs::path appimage = "/fixture/owner/My Desktop/EpicGamesLauncher.appimage";
      add_process(43, exe, "/fixture/owner", 10003);
      identities[exe].device = makedev(0, 90);
      identities["/proc/43/exe"] = identities[exe];
      add_process(44, appimage, "/fixture/owner", 10004);
      headers[appimage] = std::string("\177ELF\0\0\0\0AI\002", 11);
      const std::string mounts = "21 1 0:1 / / rw - tmpfs tmpfs rw\n60 21 0:90 / /tmp/.mount_EpicXXX ro - fuse.AppImage EpicGamesLauncher.appimage ro\n";
      for (const char *proc : {"/proc/self", "/proc/43", "/proc/44"}) {
        links[fs::path(proc) / "ns/mnt"] = "mnt:[321]";
        files[fs::path(proc) / "mountinfo"] = mounts;
      }
      identities["/dev/fuse"] = {makedev(0, 7), 267, 0, S_IFCHR | 0600, 0, 111, 222};
      listings["/proc/44/fd"] = {"3", "5"};
      links["/proc/44/fd/3"] = appimage;
      identities["/proc/44/fd/3"] = identities[appimage];
      links["/proc/44/fd/5"] = "/dev/fuse";
      identities["/proc/44/fd/5"] = identities["/dev/fuse"];
      files["/proc/44/fdinfo/5"] = "pos:\t0\nflags:\t02100002\nmnt_id:\t39\nino:\t267\nfuse_connection:\t90\n";
      return appimage;
    }

    ep::title_t title() const {
      return {"namespace", "item", "artifact", install, "Game.exe", data, "Windows"};
    }

    std::uint64_t owner_uid() const override { return uid; }

    std::optional<std::string> read(const fs::path &path, std::size_t limit) override {
      reads.push_back(path);
      const auto found = files.find(path);
      if (found == files.end() || found->second.size() > limit) {
        return std::nullopt;
      }
      const auto value = found->second;
      if (after_read) {
        after_read(path);
      }
      accessed(path);
      return value;
    }

    void accessed(const fs::path &path) {
      ++access_counts[path];
      if (after_access) {
        after_access(path);
      }
    }

    ep::testing::status_result_t initial_status(const fs::path &process, std::size_t limit) override {
      auto text = read(process / "status", limit);
      if (text) {
        return {std::move(text)};
      }
      // Missing fixture entries model ENOENT; other failures require explicit injection.
      const auto error = status_errors.find(process);
      if (error != status_errors.end() && error->second != ENOENT) {
        return {};
      }
      if (after_missing_status) {
        after_missing_status(process);
      }
      const auto directory_error = directory_errors.find(process);
      if (identities.count(process) || (directory_error != directory_errors.end() && directory_error->second != ENOENT)) {
        return {};
      }
      if (live_absence_probes.count(process)) {
        closed_absence_probes.push_back(process);
        return {};
      }
      const auto probe_error = absence_probe_errors.find(process);
      return probe_error == absence_probe_errors.end() || probe_error->second == ESRCH ?
        ep::testing::status_result_t {std::nullopt, ep::testing::process_absence_t::vanished} : ep::testing::status_result_t {};
    }

    std::optional<std::string> prefix(const fs::path &path, std::size_t length) override {
      // A task's exe handle opens the file it runs, wherever that file's own path points.
      const auto handle = links.find(path);
      const auto found = headers.find(path.filename() == "exe" && handle != links.end() ? handle->second : path);
      if (found == headers.end() || found->second.size() < length) {
        return std::nullopt;
      }
      auto value = found->second.substr(0, length);
      accessed(path);
      return value;
    }

    std::optional<fs::path> link(const fs::path &path) override {
      const auto found = links.find(path);
      if (found == links.end()) {
        return std::nullopt;
      }
      auto value = found->second;
      accessed(path);
      return value;
    }

    ep::testing::link_result_t inspect_link(const fs::path &path) override {
      if (denied_links.count(path)) {
        accessed(path);
        return {std::nullopt, true};
      }
      return {link(path), false};
    }

    /**
     * A task as the host shows it: the same name in stat and status, all four UIDs and its unified
     * cgroup. A protected task's executable link is denied; a root task never enters an owner census.
     */
    void add_task(std::uint64_t pid, const std::string &name, std::uint64_t start, std::uint64_t parent,
                  const std::string &group, std::uint64_t task_uid = 1000, bool protected_task = false) {
      add_process(pid, "/usr/bin/" + name, "/", start, parent);
      const auto path = fs::path("/proc") / std::to_string(pid);
      const auto id = std::to_string(task_uid);
      files[path / "stat"] = stat_line(pid, start, 'S', parent, name);
      files[path / "status"] = "Name:\t" + name + "\nUid:\t" + id + "\t" + id + "\t" + id + "\t" + id + "\n";
      files[path / "cgroup"] = "0::" + group + "\n";
      if (protected_task) {
        links.erase(path / "exe");
        denied_links.insert(path / "exe");
      }
    }

    /**
     * A user service the system installed, as the user manager reports it on pc-papi: loaded, not
     * transient, in this exact cgroup, from a root-owned vendor file with the vendor drop-in.
     */
    void vendor_unit(const std::string &group) {
      const auto name = fs::path(group).filename().string();
      const fs::path file = fs::path("/usr/lib/systemd/user") / name;
      const fs::path drop_in = "/usr/lib/systemd/user/service.d/10-timeout-abort.conf";
      units[name] = {name, "loaded", file.string(), group, {drop_in.string()}, false};
      identities["/usr/lib/systemd/user"] = directory(20, 0);
      identities["/usr/lib/systemd/user/service.d"] = directory(21, 0);
      identities[file] = regular(30000 + units.size(), 0);
      identities[file].mode = S_IFREG | 0644;
      identities[drop_in] = regular(29999, 0);
      identities[drop_in].mode = S_IFREG | 0644;
    }

    std::optional<ep::testing::unit_t> user_unit(const std::string &name) override {
      unit_queries.push_back(name);
      // The real backend's bus read binds the manager's name by this same check on its cgroup file.
      if (manager_pid_bound_by_cgroup && !prelaunch::steamos::cgroup_is_user_manager(files["/proc/" + std::to_string(*manager_pid_bound_by_cgroup) + "/cgroup"], static_cast<std::uint32_t>(uid))) {
        return std::nullopt;
      }
      const auto found = units.find(name);
      return found == units.end() ? std::nullopt : std::make_optional(found->second);
    }

    void drop(std::uint64_t pid) {
      pids.erase(std::remove(pids.begin(), pids.end(), std::to_string(pid)), pids.end());
    }

    template<class Map>
    static void erase_under(Map &map, const fs::path &process) {
      for (auto entry = map.begin(); entry != map.end();) {
        const auto text = entry->first.string();
        const auto root = process.string();
        entry = text == root || text.compare(0, root.size() + 1, root + "/") == 0 ? map.erase(entry) : std::next(entry);
      }
    }

    // The task's /proc entry goes: its files and links no longer read, while a listing already taken still names it.
    void hide(std::uint64_t pid) {
      const auto path = fs::path("/proc") / std::to_string(pid);
      erase_under(files, path);
      erase_under(links, path);
      erase_under(identities, path);
      for (auto denied = denied_links.begin(); denied != denied_links.end();) {
        denied = denied->parent_path() == path ? denied_links.erase(denied) : std::next(denied);
      }
    }

    // The task has exited and been reaped.
    void vanish(std::uint64_t pid) {
      hide(pid);
      drop(pid);
    }

    // The task has exited and its parent has not reaped it: it still has a stat and a status, and nothing else.
    void zombie(std::uint64_t pid) {
      const auto path = fs::path("/proc") / std::to_string(pid);
      const auto text = files[path / "stat"];
      const auto right = text.rfind(") S ");
      files[path / "stat"] = text.substr(0, right) + ") Z " + text.substr(right + 4);
      links.erase(path / "exe");
      links.erase(path / "cwd");
      identities.erase(path / "exe");
      denied_links.erase(path / "exe");
      files[path / "maps"] = "";
    }

    // The task has begun to exit and dropped its memory, as read on pc-papi: still R in its stat, with
    // PF_EXITING in its flags, and its links gone. A large process can stay like this for seconds.
    void exiting(std::uint64_t pid) {
      const auto path = fs::path("/proc") / std::to_string(pid);
      const auto text = files[path / "stat"];
      std::istringstream fields(text.substr(text.rfind(')') + 2));
      std::vector<std::string> parts;
      for (std::string field; fields >> field;) {
        parts.push_back(field);
      }
      std::ostringstream rebuilt;
      rebuilt << text.substr(0, text.rfind(')') + 1) << " R " << parts[1];
      for (std::size_t field = 2; field < parts.size(); ++field) {
        rebuilt << ' ' << (field == 6 ? std::string {"4194316"} : parts[field]);
      }
      files[path / "stat"] = rebuilt.str() + "\n";
      links.erase(path / "exe");
      links.erase(path / "cwd");
      identities.erase(path / "exe");
      denied_links.erase(path / "exe");
      files[path / "maps"] = "";
    }

    // Another user's task, as the census reads it: a stat, a status, and nothing it may open.
    void add_foreign(std::uint64_t pid, std::uint64_t start, const std::string &name = "nvidia-smi", std::uint64_t foreign_uid = 0) {
      add_task(pid, name, start, 1, "/system.slice/foreign.service", foreign_uid);
      const auto path = fs::path("/proc") / std::to_string(pid);
      identities[path].uid = foreign_uid;
      links.erase(path / "exe");
      links.erase(path / "cwd");
    }

    /**
     * pc-papi's eight protected owner tasks and their ancestors, as read on 2026-10-30: every
     * one's exe link is denied, while its stat, status and cgroup stay world-readable.
     */
    void desktop_session_host() {
      files["/proc/self/cgroup"] = "0::" + manager + "/app.slice/polaris.service\n";
      add_task(1784, "systemd", 1293, 1, manager + "/init.scope", 1000, true);
      add_task(1827, "(sd-pam)", 1295, 1784, manager + "/init.scope", 1000, true);
      add_task(4954, "kwin_wayland_wr", 2784, 1784, manager + "/session.slice/plasma-kwin_wayland.service");
      add_task(4966, "kwin_wayland", 2785, 4954, manager + "/session.slice/plasma-kwin_wayland.service", 1000, true);
      add_task(5148, "polkit-kde-auth", 2905, 1784, manager + "/background.slice/plasma-polkit-agent.service", 1000, true);
      add_task(5157, "ssh-agent", 2905, 1784, manager + "/app.slice/ssh-agent.service", 1000, true);
      add_task(561444, "sshd-session", 26805728, 1, user + "/session-1061.scope", 0);
      add_task(561538, "sshd-session", 26805742, 561444, user + "/session-1061.scope", 1000, true);
      add_task(561872, "sshd-session", 26805784, 1, user + "/session-1062.scope", 0);
      add_task(561981, "sshd-session", 26805797, 561872, user + "/session-1062.scope", 1000, true);
      add_task(3652675, "sshd", 70119596, 1, "/system.slice/sshd.service", 0);
      add_task(1941259, "sshd-session", 72641468, 3652675, user + "/session-6028.scope", 0);
      add_task(1941372, "sshd-session", 72641483, 1941259, user + "/session-6028.scope", 1000, true);
      for (const char *unit : {"/session.slice/plasma-kwin_wayland.service", "/background.slice/plasma-polkit-agent.service", "/app.slice/ssh-agent.service"}) {
        vendor_unit(manager + unit);
      }
    }

    /// Mullvad's split tunnelling mounts a v1 net_cls hierarchy, so every cgroup file on the host lists it.
    void list_net_cls_in_every_cgroup_file() {
      for (auto &[path, text] : files) {
        if (path.filename() == "cgroup" && !text.starts_with("1:net_cls:")) {
          text.insert(0, "1:net_cls:/\n");
        }
      }
    }

    const std::string user = "/user.slice/user-1000.slice";
    const std::string manager = user + "/user@1000.service";
    std::optional<std::uint64_t> manager_pid_bound_by_cgroup;

    void add_protected_process(std::uint64_t pid = 43, std::uint64_t start = 10003, std::uint64_t parent = 1) {
      add_process(pid, "/usr/lib/systemd/systemd", "/fixture/owner", start, parent);
      const auto exe = fs::path("/proc") / std::to_string(pid) / "exe";
      links.erase(exe);
      denied_links.insert(exe);
    }

    std::optional<fs::path> canonical(const fs::path &path) override {
      if (denied_canonical.count(path)) return std::nullopt;
      if (resolved.count(path)) {
        return resolved.at(path);
      }
      return identities.count(path) ? std::make_optional(path) : std::nullopt;
    }

    std::optional<file_t> stat(const fs::path &path) override {
      if (identities.count(path)) {
        const auto value = identities.at(path);
        accessed(path);
        return value;
      }
      const auto text = path.string();
      const auto root = text.find("/root/");
      if (text.compare(0, 6, "/proc/") == 0 && root != std::string::npos) {
        return stat(text.substr(root + 5));
      }
      return std::nullopt;
    }

    std::optional<file_t> nofollow_stat(const fs::path &path) override {
      // Every sandbox view must be explicit. stat() above has a legacy host-view
      // convenience fallback, which must never fabricate a same-path proof.
      const auto found = identities.find(path);
      if (found == identities.end()) return {};
      const auto value = found->second;
      accessed(path);
      return value;
    }

    std::optional<epic_library::flatpak_document_t> document(const std::string &id, const fs::path &) override {
      const auto it = documents.find(id);
      return it == documents.end() ? std::nullopt : std::make_optional(it->second);
    }

    std::optional<fs::path> portal_host_path(const fs::path &path) override {
      const auto it = portal_origins.find(path);
      return it == portal_origins.end() ? std::nullopt : std::make_optional(it->second);
    }

    std::optional<std::vector<std::string>> list(const fs::path &path, std::size_t limit) override {
      if (path == "/proc") {
        ++proc_listings;
        if (before_listing) {
          before_listing();
        }
        return pids.size() <= limit ? std::make_optional(pids) : std::nullopt;
      }
      const auto found = listings.find(path);
      return found != listings.end() && found->second.size() <= limit ? std::make_optional(found->second) : std::nullopt;
    }

    int open_pidfd(std::uint64_t pid) override {
      opened.push_back(pid);
      if (after_open) {
        after_open();
      }
      return fd;
    }

    std::optional<std::uint64_t> pidfd_pid(int) override { return fd_pid; }

    exit_state_t wait(int descriptor, std::chrono::milliseconds timeout) override {
      EXPECT_EQ(descriptor, 77);
      waits.push_back(timeout);
      return exit_state;
    }

    bool term(int descriptor) override {
      signals.push_back(descriptor);
      if (exit_after_signal && !fail_signal) {
        exit_state = exit_state_t::exited;
      }
      return !fail_signal;
    }

    void close(int descriptor) override { closed.push_back(descriptor); }
  };

  struct EpicProcess: testing::Test {
    std::shared_ptr<fixture_t> host = std::make_shared<fixture_t>();
    std::string error;

    ep::observation_t observe() { return ep::testing::observe(host->title(), host); }

    std::unique_ptr<ep::adopted_game_t> adopt() {
      const auto seen = observe();
      EXPECT_TRUE(seen.available) << seen.reason;
      return ep::testing::adopt(host->title(), seen, error, host);
    }
  };

  // Either admission census: whether it admits, what it counted, and why it does not admit.
  struct census_answer_t {
    bool admits {false};
    std::optional<std::size_t> launchers;
    std::optional<bool> absent;
    std::string reason;
  };

  struct census_t {
    const char *name, *changed, *incomplete;
    bool runtime;

    census_answer_t operator()(const std::shared_ptr<fixture_t> &host) const {
      census_answer_t answer;
      if (runtime) {
        answer.absent = ep::testing::title_runtime_absent(host->title(), answer.reason, host);
        answer.admits = answer.absent == true;
      }
      else {
        answer.launchers = ep::testing::launcher_instances(answer.reason, host);
        answer.admits = answer.launchers == std::optional<std::size_t> {0};
      }
      return answer;
    }
  };

  const std::vector<census_t> censuses {
    {"launcher census", "launcher_inventory_changed", "launcher_inventory_incomplete", false},
    {"runtime census", "title_runtime_inventory_changed", "title_runtime_inventory_incomplete", true},
  };

  // A host with the game closed: one unrelated owner task, 60, and another user's, 61.
  std::shared_ptr<fixture_t> quiet_host() {
    auto host = std::make_shared<fixture_t>();
    host->pids.clear();
    host->add_process(60, "/usr/bin/tool", "/fixture/owner", 10060);
    host->add_foreign(61, 10061);
    return host;
  }

  // A session with its game and server up, one unrelated owner task and two protected ones outside.
  std::shared_ptr<fixture_t> session_host(std::shared_ptr<fixture_lease_t> &lease) {
    auto h = std::make_shared<fixture_t>();
    lease = h->private_tree();
    h->private_game();
    h->private_server();
    h->fd_pid = 81;
    h->add_process(60, "/usr/bin/tool", "/fixture/owner", 10060);
    h->add_protected_process(43, 50);
    h->add_protected_process(45, 500, 43);
    h->add_protected_process(46, 510, 45);
    return h;
  }

  bool under(const fs::path &path, std::uint64_t pid) {
    const auto text = path.string();
    const auto root = "/proc/" + std::to_string(pid);
    return text == root || text.compare(0, root.size() + 1, root + "/") == 0;
  }

  /**
   * Runs @p run once per read of a task's /proc entry, doing @p change just after the nth read, for
   * as many n as the run has reads. Between them that is every seam, whatever the code reads next.
   */
  void at_every_read(std::uint64_t pid, const std::function<std::shared_ptr<fixture_t>()> &make,
                     const std::function<void(fixture_t &)> &change,
                     const std::function<void(const std::shared_ptr<fixture_t> &, int nth, int total)> &run) {
    int total = 0;
    {
      const auto host = make();
      host->after_access = [&](const fs::path &path) { total += under(path, pid); };
      run(host, 0, 0);
    }
    ASSERT_GT(total, 5) << "the task was never read";
    for (int nth = 1; nth <= total; ++nth) {
      const auto host = make();
      int seen = 0;
      host->after_access = [&, raw = host.get()](const fs::path &path) {
        if (under(path, pid) && ++seen == nth) {
          change(*raw);
        }
      };
      run(host, nth, total);
    }
  }
}  // namespace

TEST_F(EpicProcess, BtrfsMapsUseMountDeviceWhilePrefixUsesStatDevice) {
  const auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.pid, 41);
  EXPECT_EQ(seen.start_ticks, 10001);
  EXPECT_EQ(seen.runtime, "proton");
  EXPECT_EQ(seen.revision.size(), 64);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
  for (const auto &path : host->reads) {
    const auto name = path.filename();
    EXPECT_TRUE(name == "stat" || name == "status" || name == "maps" || name == "mountinfo" || name == "cgroup" || name == "boot_id") << path;
  }
  host->files["/proc/41/maps"] = host->maps("00:34");
  EXPECT_EQ(observe().reason, "mapped_game_identity_mismatch");
}

TEST_F(EpicProcess, ExactMapFilesAndProcessRootAreRequired) {
  host->links["/proc/41/map_files/1000-2000"] = "/fixture/owner/Games/Other/Game.exe";
  EXPECT_EQ(observe().reason, "mapped_game_link_mismatch");
  host->links["/proc/41/map_files/1000-2000"] = image;
  host->identities[fs::path("/proc/41/root") / image.relative_path()] = regular(999);
  EXPECT_EQ(observe().reason, "game_process_root_mismatch");
}

TEST_F(EpicProcess, MapsParseTheWholeEscapedOrUnescapedPathAndRejectBadPermissions) {
  host->files["/proc/41/maps"] = "1000-2000 r-xp 00000000 00:22 236793710 " + image.string() + "\n";
  EXPECT_TRUE(observe().available);
  host->files["/proc/41/maps"] = "1000-2000 rwxp 00000000 00:22 236793710 " + escaped(image) + "\n";
  EXPECT_TRUE(observe().available);
  host->files["/proc/41/maps"] = "1000-2000 zzzz 00000000 00:22 236793710 " + escaped(image) + "\n";
  EXPECT_EQ(observe().reason, "malformed_map_permissions");
}

TEST_F(EpicProcess, CensusRowVanishedBeforeInitialStatusDoesNotInvalidateVerifiedGame) {
  const auto before = observe();
  ASSERT_TRUE(before.available) << before.reason;
  // The census contains a row that vanished before its first owner-status read.
  host->pids.push_back("43");
  const auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.pid, 41);
  EXPECT_EQ(seen.start_ticks, 10001);
  EXPECT_EQ(seen.revision, before.revision);
  EXPECT_EQ(seen.uninspected_process_count, 0);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, InitialStatusAbsenceIsUnprovenByDefaultAndOnAccessOrLookupFailure) {
  host->pids.push_back("43");
  const auto default_result = host->backend_t::initial_status("/proc/43", 65536);
  EXPECT_FALSE(default_result.text);
  EXPECT_EQ(default_result.absence, ep::testing::process_absence_t::unproven);
  for (const int error : {EACCES, EPERM, EIO}) {
    host->status_errors["/proc/43"] = error;
    EXPECT_EQ(observe().reason, "incomplete_process_observation");
  }
  host->status_errors["/proc/43"] = ENOENT;
  for (const int error : {EACCES, EPERM, EIO}) {
    host->directory_errors["/proc/43"] = error;
    EXPECT_EQ(observe().reason, "incomplete_process_observation");
  }
  host->directory_errors["/proc/43"] = ENOENT;
  for (const int error : {EACCES, EPERM, EIO, ENOSYS, EMFILE, EINVAL}) {
    host->absence_probe_errors["/proc/43"] = error;
    EXPECT_EQ(observe().reason, "incomplete_process_observation");
  }
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, MissingInitialStatusCannotHideALiveOrReusedPid) {
  host->add_process(43, runtime / "files/bin/wine64-preloader", install, 10003);
  host->files.erase("/proc/43/status");
  EXPECT_EQ(observe().reason, "incomplete_process_observation");
  host->identities.erase("/proc/43");
  host->after_missing_status = [&](const fs::path &process) {
    if (process == "/proc/43") {
      // Replacement arrives between status ENOENT and the directory absence check.
      host->identities[process] = directory(543);
      host->files[process / "stat"] = stat_line(43, 20003);
    }
  };
  EXPECT_EQ(observe().reason, "incomplete_process_observation");
  host->after_missing_status = {};
  host->identities.erase("/proc/43");
  // A live PID hidden from procfs, or reused after lstat, fails the kernel probe.
  host->live_absence_probes.insert("/proc/43");
  EXPECT_EQ(observe().reason, "incomplete_process_observation");
  EXPECT_EQ(host->closed_absence_probes, (std::vector<fs::path> {"/proc/43"}));
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, VanishedCensusRowDoesNotChangeRetainedPidfdAuthority) {
  auto game = adopt();
  ASSERT_NE(game, nullptr) << error;
  host->pids.push_back("43");
  EXPECT_TRUE(ep::inspect(*game).available);
  EXPECT_EQ(host->opened, (std::vector<std::uint64_t> {41}));
  EXPECT_TRUE(host->signals.empty());
  EXPECT_TRUE(ep::ask_to_close(*game, 10ms).closed);
  EXPECT_EQ(host->opened, (std::vector<std::uint64_t> {41}));
  EXPECT_EQ(host->signals, (std::vector<int> {77}));
}

TEST_F(EpicProcess, DisappearanceAfterInitialStatusAndMalformedStatusStillRefuse) {
  host->add_process(43, "/fixture/owner/OtherProgram", "/fixture/owner", 10003);
  host->files["/proc/43/status"] = "Name:\tmalformed\n";
  EXPECT_EQ(observe().reason, "missing_process_uid");
  host->files["/proc/43/status"] = "Uid:\t1000\t1000\t1000\t1000\n";
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/43/status") host->identities.erase("/proc/43");
  };
  EXPECT_EQ(observe().reason, "unreadable_file_identity");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, VanishedGameAndChangedProtectedSetCannotAuthorizeAdoptionOrClose) {
  host->add_protected_process();
  const auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  host->files.erase("/proc/43/status");
  host->identities.erase("/proc/43");
  EXPECT_EQ(ep::testing::adopt(host->title(), seen, error, host), nullptr);
  EXPECT_EQ(error, "stale_observation");
  EXPECT_TRUE(host->opened.empty());
  auto game = adopt();
  ASSERT_NE(game, nullptr) << error;
  host->files.erase("/proc/41/status");
  host->identities.erase("/proc/41");
  EXPECT_EQ(observe().reason, "game_not_observed");
  EXPECT_FALSE(ep::ask_to_close(*game, 10ms).signalled);
  EXPECT_EQ(host->opened, (std::vector<std::uint64_t> {41}));
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, MissingUnreadableOrMalformedRowsCannotHideAnAmbiguousGame) {
  host->add_process(43, runtime / "files/bin/wine64-preloader", install, 10002);
  host->files.erase("/proc/43/maps");
  EXPECT_FALSE(observe().available);
  host->files["/proc/43/maps"] = host->maps();
  EXPECT_EQ(observe().reason, "ambiguous_game_process");
  host->files["/proc/43/stat"] = "43 (truncated) S 1\n";
  EXPECT_EQ(observe().reason, "incomplete_process_stat");
}

TEST_F(EpicProcess, UnmappedUnrelatedProgramNeedsNoRetainedImageIdentity) {
  const fs::path unrelated = "/fixture/owner/OldProgram";
  host->add_process(43, unrelated, "/fixture/owner/old-working-directory", 10003);
  host->identities.erase(unrelated);
  const auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.pid, 41);
  EXPECT_EQ(seen.uninspected_process_count, 0);
  // If this same process maps the game it needs all positive identity proofs again.
  host->files["/proc/43/maps"] = host->maps();
  EXPECT_FALSE(observe().available);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, UnmappedProgramsStillNeedReadableMapsAndStablePidIdentity) {
  host->add_process(43, "/fixture/owner/OtherProgram", "/fixture/owner", 10003);
  host->files.erase("/proc/43/maps");
  EXPECT_FALSE(observe().available);
  host->files["/proc/43/maps"] = "9000-a000 rw-p 00000000 00:00 0\n";
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/43/maps") host->files["/proc/43/stat"] = stat_line(43, 20003);
  };
  EXPECT_FALSE(observe().available);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, ProtectedDesktopProcessIsDisclosedButCannotSupplyCloseAuthority) {
  host->add_protected_process();
  const auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.pid, 41);
  EXPECT_EQ(seen.uninspected_process_count, 1);
  EXPECT_TRUE(host->opened.empty());
  auto game = ep::testing::adopt(host->title(), seen, error, host);
  ASSERT_NE(game, nullptr) << error;
  EXPECT_EQ(host->opened, (std::vector<std::uint64_t> {41}));
  EXPECT_TRUE(ep::ask_to_close(*game, 10ms).closed);
  EXPECT_EQ(host->signals, (std::vector<int> {77}));
}

TEST_F(EpicProcess, MissingLinksAndIncompleteInspectableCandidatesStillRefuseBesideProtectedProcesses) {
  host->add_protected_process();
  host->add_process(44, runtime / "files/bin/wine64-preloader", install, 10004);
  host->links.erase("/proc/44/exe");
  EXPECT_EQ(observe().reason, "unreadable_process_link");
  host->links["/proc/44/exe"] = runtime / "files/bin/wine64-preloader";
  host->files.erase("/proc/44/maps");
  EXPECT_EQ(observe().reason, "incomplete_process_observation");
  host->files["/proc/44/maps"] = host->maps();
  EXPECT_EQ(observe().reason, "ambiguous_game_process");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, ProtectedGameItselfCanNeverBeOfferedForConsent) {
  host->denied_links.insert("/proc/41/exe");
  EXPECT_EQ(observe().reason, "game_not_observed");
  EXPECT_EQ(ep::testing::adopt(host->title(), observe(), error, host), nullptr);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, ProtectedOwnerStatusAndProcessDirectoryMustStillBeTrusted) {
  host->add_protected_process();
  host->identities["/proc/43"].uid = 0;
  EXPECT_EQ(observe().reason, "unreadable_owner_process");
  host->identities["/proc/43"].uid = 1000;
  host->files["/proc/43/status"] = "Uid:\t1000\t0\t1000\t1000\n";
  EXPECT_EQ(observe().reason, "foreign_process_uid");
  host->files["/proc/43/status"] = "Uid:\t1000\t1000\t1000\t1000\n";
  host->files["/proc/43/stat"] = "43 (incomplete) S 1\n";
  EXPECT_EQ(observe().reason, "incomplete_process_stat");
}

TEST_F(EpicProcess, ProtectedSetIdentityAndCountAreBoundToConsentRevision) {
  const auto complete = observe();
  ASSERT_TRUE(complete.available);
  host->add_protected_process();
  const auto partial = observe();
  ASSERT_TRUE(partial.available) << partial.reason;
  EXPECT_NE(complete.revision, partial.revision);
  host->files["/proc/43/stat"] = stat_line(43, 20003);
  const auto restarted = observe();
  ASSERT_TRUE(restarted.available) << restarted.reason;
  EXPECT_NE(partial.revision, restarted.revision);
  host->add_protected_process(44, 20004);
  const auto two = observe();
  ASSERT_TRUE(two.available) << two.reason;
  EXPECT_EQ(two.uninspected_process_count, 2);
  EXPECT_NE(restarted.revision, two.revision);
  std::reverse(host->pids.begin(), host->pids.end());
  EXPECT_EQ(two.revision, observe().revision);
  EXPECT_EQ(ep::testing::adopt(host->title(), partial, error, host), nullptr);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, ProtectedProcessChangingDuringObservationIsUnavailable) {
  host->add_protected_process();
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/41/mountinfo") host->files["/proc/43/stat"] = stat_line(43, 20003);
  };
  EXPECT_EQ(observe().reason, "protected_process_changed");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, ProtectedProcessChangeAtPidfdOpenOrBeforeEndNeverSignals) {
  host->add_protected_process();
  const auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  host->after_open = [&] { host->files["/proc/43/stat"] = stat_line(43, 20003); };
  EXPECT_EQ(ep::testing::adopt(host->title(), seen, error, host), nullptr);
  EXPECT_TRUE(host->signals.empty());
  host->after_open = {};
  auto game = adopt();
  ASSERT_NE(game, nullptr) << error;
  host->denied_links.erase("/proc/43/exe");
  EXPECT_FALSE(ep::ask_to_close(*game, 10ms).signalled);
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, ForeignUidAndServiceLifetimeNeverSupplyACandidate) {
  host->files["/proc/41/status"] = "Uid:\t1000\t1001\t1000\t1000\n";
  EXPECT_EQ(observe().reason, "foreign_process_uid");
  host->files["/proc/41/status"] = "Uid:\t1000\t1000\t1000\t1000\n";
  host->files["/proc/41/cgroup"] = "0::/user.slice/polaris.service/children\n";
  EXPECT_EQ(observe().reason, "game_identity_incomplete");
  host->files["/proc/41/cgroup"] = "0::/user.slice/app.slice/epic.scope\n";
  host->identities["/proc/41"].uid = 1001;
  host->files["/proc/41/status"] = "Uid:\t1001\t1001\t1001\t1001\n";
  EXPECT_EQ(observe().reason, "game_not_observed");
}

TEST_F(EpicProcess, PrivilegedMountHelperIsOutsideOwnerAuthorityAndNotAnUninspectedOwnerProcess) {
  host->add_process(43, "/usr/bin/fusermount3", "/fixture/owner", 10003);
  host->files["/proc/43/status"] = "Uid:\t1000\t0\t0\t0\n";
  host->identities["/proc/43"].uid = 0;
  host->links.erase("/proc/43/exe");
  const auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.pid, 41);
  EXPECT_EQ(seen.uninspected_process_count, 0);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivilegedGameOrPrefixServerCannotSupplyOwnerConsent) {
  for (const auto pid : {41, 42}) {
    const auto fixture = std::make_shared<fixture_t>();
    fixture->files[fs::path("/proc") / std::to_string(pid) / "status"] = "Uid:\t1000\t0\t0\t0\n";
    const auto seen = ep::testing::observe(fixture->title(), fixture);
    EXPECT_FALSE(seen.available);
    EXPECT_EQ(ep::testing::adopt(fixture->title(), seen, error, fixture), nullptr);
    EXPECT_TRUE(fixture->opened.empty());
    EXPECT_TRUE(fixture->signals.empty());
  }
}

TEST_F(EpicProcess, OtherMixedUidPatternsAreStillRefused) {
  for (const auto *uids : {"1000\t0\t1000\t1000", "1000\t1001\t1000\t1000", "0\t1000\t0\t0", "1000\t1000\t0\t1000"}) {
    host->files["/proc/41/status"] = std::string("Uid:\t") + uids + "\n";
    EXPECT_EQ(observe().reason, "foreign_process_uid");
    EXPECT_TRUE(host->opened.empty());
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, PrefixMustHaveOneServerFromTheSameExactRuntime) {
  host->links["/proc/42/exe"] = data / "compat/runtime/Other/files/bin/wineserver";
  host->identities[host->links["/proc/42/exe"]] = host->identities["/proc/42/exe"];
  EXPECT_EQ(observe().reason, "prefix_runtime_mismatch");
  host->links["/proc/42/exe"] = runtime / "files/bin/wineserver";
  host->add_process(43, runtime / "files/bin/wineserver", "/tmp/.wine-1000/server-34-e2a1113", 10003);
  EXPECT_EQ(observe().reason, "ambiguous_prefix_server");
}

TEST_F(EpicProcess, DeletedImageOrAnotherMappedInodeIsRefused) {
  host->files["/proc/41/maps"] = host->maps("00:22", 999);
  EXPECT_EQ(observe().reason, "mapped_game_identity_mismatch");
  host->files["/proc/41/maps"] = "1000-2000 r-xp 0 00:22 236793710 " + escaped(image) + " (deleted)\n";
  EXPECT_EQ(observe().reason, "deleted_game_image");
}

TEST_F(EpicProcess, UnknownRuntimeAndTraversalAreUnavailable) {
  auto title = host->title();
  title.platform = "unknown";
  EXPECT_EQ(ep::testing::observe(title, host).reason, "unknown_title_runtime");
  title = host->title();
  title.launch_executable = "../Game.exe";
  EXPECT_EQ(ep::testing::observe(title, host).reason, "invalid_title_executable");
  host->links["/proc/41/exe"] = "/usr/bin/wine64-preloader";
  host->identities["/usr/bin/wine64-preloader"] = host->identities["/proc/41/exe"];
  EXPECT_EQ(observe().reason, "unknown_game_runtime");
}

TEST_F(EpicProcess, NativeRequiresTheExactElfAsExecutableAndMappedImage) {
  auto title = host->title();
  title.platform = "Linux";
  host->headers[image] = std::string("\177ELF", 4);
  host->links["/proc/41/exe"] = image;
  host->identities["/proc/41/exe"] = host->identities[image];
  auto seen = ep::testing::observe(title, host);
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.runtime, "native");
  host->links["/proc/41/exe"] = runtime / "files/bin/wine64-preloader";
  host->identities["/proc/41/exe"] = host->identities[runtime / "files/bin/wine64-preloader"];
  EXPECT_EQ(ep::testing::observe(title, host).reason, "native_executable_mismatch");
}

TEST_F(EpicProcess, StaleRevisionNeverOpensAPidfd) {
  auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  host->files["/proc/41/stat"] = stat_line(41, 20001);
  EXPECT_FALSE(ep::testing::adopt(host->title(), seen, error, host));
  EXPECT_EQ(error, "stale_observation");
  EXPECT_TRUE(host->opened.empty());
}

TEST_F(EpicProcess, PidReuseDuringPidfdOpenClosesDescriptorWithoutSignal) {
  const auto seen = observe();
  host->after_open = [&] { host->files["/proc/41/stat"] = stat_line(41, 20001); };
  EXPECT_FALSE(ep::testing::adopt(host->title(), seen, error, host));
  EXPECT_EQ(error, "adopted_game_identity_changed");
  EXPECT_EQ(host->closed, std::vector<int> {77});
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PidfdFailureAndWrongKernelPidNeverAuthorizeClose) {
  const auto seen = observe();
  host->fd = -1;
  EXPECT_FALSE(ep::testing::adopt(host->title(), seen, error, host));
  EXPECT_EQ(error, "pidfd_open_failed");
  EXPECT_TRUE(host->closed.empty());
  host->fd = 77;
  host->fd_pid = 999;
  EXPECT_FALSE(ep::testing::adopt(host->title(), seen, error, host));
  EXPECT_EQ(error, "retained_pidfd_unavailable");
  EXPECT_EQ(host->closed, std::vector<int> {77});
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, ExitingDuringPidfdOpenNeverAuthorizesASignal) {
  const auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  host->after_open = [&] { host->exit_state = exit_state_t::exited; };
  EXPECT_FALSE(ep::testing::adopt(host->title(), seen, error, host));
  EXPECT_EQ(error, "adopted_game_exited");
  EXPECT_EQ(host->closed, std::vector<int> {77});
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, DestructionAndMoveReleaseOnlyTheRetainedDescriptor) {
  auto game = adopt();
  ASSERT_TRUE(game) << error;
  auto moved = std::make_unique<ep::adopted_game_t>(std::move(*game));
  EXPECT_FALSE(ep::inspect(*game).available);
  game.reset();
  EXPECT_TRUE(host->closed.empty());
  moved.reset();
  EXPECT_EQ(host->closed, std::vector<int> {77});
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, ImageReplacementBeforeCloseRevokesWithoutRetargeting) {
  auto game = adopt();
  ASSERT_TRUE(game) << error;
  host->identities[image].changed_ns++;
  const auto result = ep::ask_to_close(*game, 1s);
  EXPECT_FALSE(result.closed);
  EXPECT_FALSE(result.signalled);
  EXPECT_TRUE(host->signals.empty());
  host->identities[image].changed_ns--;
  EXPECT_EQ(ep::inspect(*game).reason, "adoption_revoked");
  EXPECT_EQ(host->opened, std::vector<std::uint64_t> {41});
}

TEST_F(EpicProcess, BootPrefixAndServerChangesInvalidateAnExistingGrant) {
  for (int change = 0; change < 3; ++change) {
    host = std::make_shared<fixture_t>();
    auto game = adopt();
    ASSERT_TRUE(game) << error;
    if (change == 0) {
      host->files["/proc/sys/kernel/random/boot_id"] = "add26721-4424-4907-ab80-4f023729a22f\n";
    }
    else if (change == 1) {
      host->identities[prefix_path].inode++;
    }
    else {
      host->files["/proc/42/stat"] = stat_line(42, 20000);
    }
    EXPECT_FALSE(ep::inspect(*game).available);
    EXPECT_FALSE(ep::ask_to_close(*game, 1s).signalled);
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, ReplacementProcessIsNeverOpenedOrSignalled) {
  auto game = adopt();
  ASSERT_TRUE(game) << error;
  host->exit_state = exit_state_t::exited;
  host->pids = {"42"};
  host->add_process(43, runtime / "files/bin/wine64-preloader", install, 10003);
  host->files["/proc/43/maps"] = host->maps();
  EXPECT_FALSE(ep::inspect(*game).available);
  const auto result = ep::ask_to_close(*game, 1s);
  EXPECT_TRUE(result.closed);
  EXPECT_FALSE(result.signalled);
  EXPECT_EQ(host->opened, std::vector<std::uint64_t> {41});
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SigtermUsesOnlyRetainedDescriptorAndReportsActualExit) {
  auto game = adopt();
  ASSERT_TRUE(game) << error;
  const auto result = ep::ask_to_close(*game, 1s);
  EXPECT_TRUE(result.signalled);
  EXPECT_TRUE(result.closed);
  EXPECT_EQ(host->signals, std::vector<int> {77});
  EXPECT_EQ(host->opened, std::vector<std::uint64_t> {41});
}

TEST_F(EpicProcess, TimeoutIsBoundedAndDoesNotEscalateOrRepeatSignal) {
  auto game = adopt();
  ASSERT_TRUE(game) << error;
  host->exit_after_signal = false;
  const auto result = ep::ask_to_close(*game, 1h);
  EXPECT_FALSE(result.closed);
  EXPECT_TRUE(result.signalled);
  EXPECT_EQ(result.reason, "close_timeout");
  EXPECT_EQ(host->waits.back(), 10s);
  EXPECT_FALSE(ep::ask_to_close(*game, 0ms).closed);
  EXPECT_EQ(host->signals, std::vector<int> {77});
}

TEST_F(EpicProcess, FailedSignalRevokesGrantWithoutFallback) {
  auto game = adopt();
  ASSERT_TRUE(game) << error;
  host->fail_signal = true;
  const auto result = ep::ask_to_close(*game, 1s);
  EXPECT_FALSE(result.closed);
  EXPECT_FALSE(result.signalled);
  EXPECT_EQ(result.reason, "pidfd_sigterm_failed");
  EXPECT_FALSE(ep::ask_to_close(*game, 1s).signalled);
  EXPECT_EQ(host->signals, std::vector<int> {77});
}

TEST_F(EpicProcess, AdmissionRequiresBothGameAndExactPrefixServerAbsentInACompleteSample) {
  std::string reason;
  // The requested game can be gone while its exact prefix server still owns the runtime.
  host->pids = {"42"};
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), false);
  EXPECT_EQ(reason, "title_runtime_busy");
  host->pids.clear();
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), true);
  EXPECT_EQ(reason, "title_runtime_absent");
  host->pids = {"41"};
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), false);
  host->files.erase("/proc/41/maps");
  EXPECT_FALSE(ep::testing::title_runtime_absent(host->title(), reason, host));
  host->pids.clear();
  host->add_protected_process();
  EXPECT_FALSE(ep::testing::title_runtime_absent(host->title(), reason, host));
  EXPECT_EQ(reason, "title_runtime_inventory_incomplete");
  host->pids.clear();
  host->identities.erase(prefix_path);
  EXPECT_FALSE(ep::testing::title_runtime_absent(host->title(), reason, host));
  auto native = host->title();
  native.platform = "Linux";
  host->headers[image] = std::string("\177ELF", 4);
  EXPECT_EQ(ep::testing::title_runtime_absent(native, reason, host), true);
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/sys/kernel/random/boot_id") {
      ++host->identities[image].changed_ns;
    }
  };
  EXPECT_FALSE(ep::testing::title_runtime_absent(native, reason, host));
  EXPECT_EQ(reason, "title_runtime_input_changed");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, AdmissionCensusesStayDefiniteBesideUnrelatedFlatpakAndDeletedTasks) {
  std::string reason;
  host->pids.clear();
  // A Flatpak app runs a sandbox path in its own cgroup namespace, and an updated daemon still
  // runs its replaced executable. The host can open neither path; both tasks stay readable.
  host->add_process(60, "/app/bin/flatapp", "/fixture/owner", 10060);
  host->links["/proc/60/ns/cgroup"] = "cgroup:[999]";
  host->identities.erase("/app/bin/flatapp");
  host->add_process(61, "/usr/bin/olddaemon (deleted)", "/", 10061);
  host->identities.erase("/usr/bin/olddaemon (deleted)");
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), true);
  EXPECT_EQ(reason, "title_runtime_absent");
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 0U);
  EXPECT_TRUE(error.empty());
  // A launcher candidate still needs its full identity, so the same sandbox view refuses.
  host->add_process(62, "/app/bin/EpicGamesLauncher", "/fixture/owner", 10062);
  host->identities.erase("/app/bin/EpicGamesLauncher");
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "unreadable_file_identity");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivateAdmissionSetsAsideTheRealHostsProtectedSessionAndSystemTasks) {
  std::string reason;
  host->pids.clear();
  host->desktop_session_host();
  // Before D-A (B) these eight protected owner tasks left both admission censuses unknown.
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 0U);
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), true);
  EXPECT_EQ(reason, "title_runtime_absent");
  // Setting them aside never hides a readable launcher, the game or its prefix server.
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 80000000, 1784);
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 1U);
  host->drop(43);
  host->pids.push_back("42");
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), false);
  EXPECT_EQ(reason, "title_runtime_busy");
  // Mirror consent is unchanged: the same eight stay disclosed as uninspected, never placed.
  host->pids.push_back("41");
  const auto seen = observe();
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.uninspected_process_count, 8U);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivateAdmissionSetsAsideTheProtectedTasksOnAHostWithACgroupV1Hierarchy) {
  std::string reason;
  host->pids.clear();
  host->desktop_session_host();
  host->list_net_cls_in_every_cgroup_file();
  host->manager_pid_bound_by_cgroup = 1784;
  // pc-papi with Mullvad, 2026-10-30: this shape refused every launch as launcher_inventory_incomplete.
  ASSERT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;
  EXPECT_TRUE(error.empty()) << error;
  ASSERT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), true) << reason;
  EXPECT_EQ(reason, "title_runtime_absent");
  EXPECT_FALSE(host->unit_queries.empty());

  // Setting them aside still never hides a readable launcher, the game or its prefix server.
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 80000000, 1784);
  host->list_net_cls_in_every_cgroup_file();
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 1U);
  host->drop(43);
  host->pids.push_back("42");
  host->list_net_cls_in_every_cgroup_file();
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), false);
  EXPECT_EQ(reason, "title_runtime_busy");
  host->drop(42);

  const auto refuses = [&](const char *what) {
    EXPECT_FALSE(ep::testing::launcher_instances(error, host)) << what;
    EXPECT_EQ(error, "launcher_inventory_incomplete") << what;
    EXPECT_FALSE(ep::testing::title_runtime_absent(host->title(), reason, host)) << what;
    EXPECT_EQ(reason, "title_runtime_inventory_incomplete") << what;
  };
  // An unrelated protected task in a custom unit still refuses, the line beside it changing nothing.
  const auto group = host->manager + "/session.slice/mylauncher.service";
  host->add_task(7500, "helper", 90000700, 1784, group, 1000, true);
  host->list_net_cls_in_every_cgroup_file();
  refuses("no such unit loaded");
  host->vendor_unit(group);
  host->units["mylauncher.service"].fragment_path = "/fixture/owner/.config/systemd/user/mylauncher.service";
  refuses("a ~/.config user unit");
  host->units["mylauncher.service"].fragment_path = "/run/user/1000/systemd/transient/mylauncher.service";
  host->units["mylauncher.service"].transient = true;
  refuses("a transient unit");
  host->drop(7500);
  ASSERT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;

  // So does a manager its cgroup file does not prove, or does not prove once.
  const auto manager_cgroup = host->files["/proc/1784/cgroup"];
  for (const auto &text : {
         "1:net_cls:/\n0::" + host->manager + "/app.slice/impostor.service\n",
         manager_cgroup + "0::" + host->manager + "/init.scope\n",
         "1:net_cls\n0::" + host->manager + "/init.scope\n",
       }) {
    host->files["/proc/1784/cgroup"] = text;
    refuses(text.c_str());
  }
}

TEST_F(EpicProcess, PrivateAdmissionRefusesAProtectedTaskOutsideTheSessionAndSystemRoles) {
  std::string reason;
  host->pids.clear();
  host->desktop_session_host();
  const auto refuses = [&](const char *placement) {
    EXPECT_FALSE(ep::testing::launcher_instances(error, host)) << placement;
    EXPECT_EQ(error, "launcher_inventory_incomplete") << placement;
    EXPECT_FALSE(ep::testing::title_runtime_absent(host->title(), reason, host)) << placement;
    EXPECT_EQ(reason, "title_runtime_inventory_incomplete") << placement;
  };
  // Started by the user manager, but as an application, a transient run or a D-Bus activation.
  host->add_task(7001, "helper", 90000000, 1784, host->manager + "/app.slice/app-org.kde.konsole@a1.service", 1000, true);
  refuses("application launch unit");
  for (const auto &group : {
         host->manager + "/app.slice/app-flatpak-com.example.App-77.scope",
         host->manager + "/app.slice/run-r1.service",
         host->manager + "/session.slice/dbus-:1.2-org.example.Helper@0.service",
         host->manager + "/app.slice/polaris.service",
         host->manager + "/session.slice/helper.service/nested",
         host->manager + "/init.scope/nested",
         host->user + "/session-6028.scope/nested",
         std::string("/user.slice/user-1001.slice/user@1001.service/session.slice/helper.service"),
         std::string("/system.slice/helper.service"),
         std::string("/user.slice/app.slice/epic.scope"),
       }) {
    host->files["/proc/7001/cgroup"] = "0::" + group + "\n";
    refuses(group.c_str());
  }
  // The same task in a vendor session service's placement is set aside.
  host->vendor_unit(host->manager + "/session.slice/helper.service");
  host->files["/proc/7001/cgroup"] = "0::" + host->manager + "/session.slice/helper.service\n";
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), true) << reason;
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivateAdmissionRefusesAProtectedTaskWhoseLineageIsNotItsRole) {
  host->pids.clear();
  host->desktop_session_host();
  const auto unknown = [&](const char *lineage) {
    EXPECT_FALSE(ep::testing::launcher_instances(error, host)) << lineage;
    EXPECT_EQ(error, "launcher_inventory_incomplete") << lineage;
  };
  const auto service = host->manager + "/session.slice/plasma-kwin_wayland.service";
  // A session service's unit, reached from an application rather than from the user manager.
  host->add_task(7100, "konsole", 90000000, 1784, host->manager + "/app.slice/app-org.kde.konsole@a1.service");
  host->add_task(7101, "helper", 90000100, 7100, service, 1000, true);
  unknown("service unit under an application");
  // Under a task of its own unit it is set aside, as kwin_wayland is under its wrapper.
  host->files["/proc/7101/stat"] = stat_line(7101, 90000100, 'S', 4954, "helper");
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;
  // A reused parent PID, younger than its child, or a vanished parent is unproven.
  host->files["/proc/7101/stat"] = stat_line(7101, 2000, 'S', 4954, "helper");
  unknown("parent younger than child");
  host->files["/proc/7101/stat"] = stat_line(7101, 90000100, 'S', 7199, "helper");
  unknown("vanished parent");
  // A manager whose credentials are not wholly the owner's (here a setuid-root shape, which the
  // census itself skips) is not this owner's user manager.
  host->files["/proc/1784/status"] = "Name:\tsystemd\nUid:\t1000\t0\t0\t0\n";
  host->files["/proc/7101/stat"] = stat_line(7101, 90000100, 'S', 4954, "helper");
  unknown("mixed-credential manager");
  host->files["/proc/1784/status"] = "Name:\tsystemd\nUid:\t1000\t1000\t1000\t1000\n";
  host->drop(7100);
  host->drop(7101);
  ASSERT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;

  // In the manager's own scope, only the manager and its direct child qualify.
  host->add_task(7110, "helper", 90000110, 1827, host->manager + "/init.scope", 1000, true);
  unknown("grandchild in init.scope");
  host->files["/proc/7110/stat"] = stat_line(7110, 90000110, 'S', 4954, "helper");
  unknown("init.scope task under a service");
  host->drop(7110);

  // A login session's task under an owner shell, or under root started from that shell.
  host->add_task(7200, "zsh", 90000200, 1941372, host->user + "/session-6028.scope");
  host->add_task(7201, "helper", 90000300, 7200, host->user + "/session-6028.scope", 1000, true);
  unknown("owner shell in a login session");
  host->add_task(7202, "sudo", 90000250, 7200, host->user + "/session-6028.scope", 0);
  host->files["/proc/7201/stat"] = stat_line(7201, 90000300, 'S', 7202, "helper");
  unknown("root helper started from an owner shell");
  // A root parent in another scope, or with mixed credentials, is not the login's own stack.
  host->files["/proc/7201/stat"] = stat_line(7201, 90000300, 'S', 3652675, "helper");
  unknown("root parent outside the session scope");
  host->files["/proc/7201/stat"] = stat_line(7201, 90000300, 'S', 1941259, "helper");
  ASSERT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;
  host->files["/proc/1941259/status"] = "Name:\tsshd-session\nUid:\t1000\t0\t0\t0\n";
  unknown("mixed-credential login parent");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivateAdmissionNameScreenOnlyRefusesAndEveryRoleIsBookended) {
  host->pids.clear();
  host->desktop_session_host();
  const auto service = host->manager + "/session.slice/helper.service";
  host->vendor_unit(service);
  // An Epic-, AppImage-, Wine-, Proton- or game-like name refuses even as a real session service.
  for (const char *name : {"EpicGamesLaunch", "epic games laun", "Tool.AppImage", "wineserver", "wine64-preloade", "AlanWake2.exe", "proton"}) {
    host->add_task(7300, name, 90000300, 1784, service, 1000, true);
    EXPECT_FALSE(ep::testing::launcher_instances(error, host)) << name;
    EXPECT_EQ(error, "launcher_inventory_incomplete") << name;
    host->drop(7300);
  }
  // The stat and status names must agree.
  host->add_task(7300, "helper", 90000300, 1784, service, 1000, true);
  ASSERT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;
  host->files["/proc/7300/status"] = "Name:\tother\nUid:\t1000\t1000\t1000\t1000\n";
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "launcher_inventory_incomplete");
  host->drop(7300);

  // An edge that changes during its own role proof leaves the role unproven.
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/5157/cgroup") host->files["/proc/5157/stat"] = stat_line(5157, 2905, 'S', 1, "ssh-agent");
  };
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "launcher_inventory_incomplete");
  host->after_read = {};
  host->files["/proc/5157/stat"] = stat_line(5157, 2905, 'S', 1784, "ssh-agent");
  ASSERT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;

  // A set-aside task that runs a readable executable before the census ends was never checked.
  host->add_task(7400, "plasmashell", 90000400, 1784, host->manager + "/session.slice/plasma-plasmashell.service");
  int reads = 0;
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/7400/stat" && ++reads == 2) {
      host->denied_links.erase("/proc/5157/exe");
      host->links["/proc/5157/exe"] = "/usr/bin/ssh-agent";
    }
  };
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "launcher_inventory_changed");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, AdmissionNamesAPrefixlessTitleAsFirstPlayInMirrorDesktop) {
  std::string reason;
  host->pids.clear();
  // A title that has never run has no prefix yet; that stays unknown, but says why (N3).
  host->identities.erase(prefix_path);
  EXPECT_FALSE(ep::testing::title_runtime_absent(host->title(), reason, host));
  EXPECT_EQ(reason, "title_prefix_unavailable");
  host->identities[prefix_path] = directory(0xe2a1113);
  host->resolved[prefix_path] = data / "compat/compatdata/other/pfx";
  EXPECT_FALSE(ep::testing::title_runtime_absent(host->title(), reason, host));
  EXPECT_EQ(reason, "redirected_title_prefix");
  host->resolved.erase(prefix_path);
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), true);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivateSessionPinRefusesASupervisorThatStaysNonDumpable) {
  // N6: Polaris reads its own /proc entry even when its file capabilities make it non-dumpable
  // (the kernel exempts a task's own thread group), and its /proc directory stays the owner's.
  // The supervisor re-executes that binary and restores dumpability before starting labwc. If it
  // did not, its links stay denied and pinning refuses rather than treating it as outside.
  const auto lease = host->private_tree();
  ASSERT_TRUE(ep::testing::pin_private_session(lease, "private-generation", error, host)) << error;
  host->links.erase("/proc/100/exe");
  host->denied_links.insert("/proc/100/exe");
  EXPECT_FALSE(ep::testing::pin_private_session(lease, "private-generation", error, host));
  EXPECT_EQ(error, "unreadable_process_link");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, EpicObserveInSessionNeverTakesTheDesktopCopy) {
  const auto lease = host->private_tree();
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 250);
  const auto session = ep::testing::pin_private_session(lease, "private-generation", error, host);
  ASSERT_TRUE(session) << error;
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 1U);
  auto seen = ep::observe_in_session(host->title(), *session, 300);
  EXPECT_FALSE(seen.available);
  EXPECT_EQ(seen.reason, "game_not_observed");
  EXPECT_FALSE(ep::adopt_in_session(host->title(), *session, 300, seen, error));
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());

  // No running launcher in the private tree is needed. The desktop game and its
  // prefix server remain outside; a different in-tree title also supplies no match.
  host->private_game();
  host->private_server();
  host->add_process(86, "/fixture/owner/Games/Other/Other.exe", "/fixture/owner/Games/Other", 410, 101);
  seen = ep::observe_in_session(host->title(), *session, 300);
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.pid, 81);
  EXPECT_NE(seen.pid, 41);
  EXPECT_NE(seen.pid, 86);
  EXPECT_TRUE(host->opened.empty());
  host->fd_pid = 81;
  auto grant = ep::adopt_in_session(host->title(), *session, 300, seen, error);
  ASSERT_TRUE(grant) << error;
  EXPECT_TRUE(ep::inspect(*grant).available);
  EXPECT_EQ(host->opened, std::vector<std::uint64_t> {81});
  grant.reset();
  EXPECT_EQ(host->closed, std::vector<int> {77});
  EXPECT_TRUE(host->signals.empty());
  host->opened.clear();

  host->private_game(83, 401);
  seen = ep::observe_in_session(host->title(), *session, 300);
  EXPECT_FALSE(seen.available);
  EXPECT_EQ(seen.reason, "ambiguous_game_process");
  EXPECT_FALSE(ep::adopt_in_session(host->title(), *session, 300, seen, error));
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivateSelectionRefusesPredispatchUnreadableAndStaleGenerationInputs) {
  const auto lease = host->private_tree();
  host->private_game();
  host->private_server();
  const auto session = ep::testing::pin_private_session(lease, "private-generation", error, host);
  ASSERT_TRUE(session) << error;
  ASSERT_TRUE(ep::observe_in_session(host->title(), *session, 300).available);
  // A kiosk request can precede supervisor/compositor startup; generation ancestry
  // still requires a fresh descendant, rather than rejecting that valid lower bound.
  EXPECT_TRUE(ep::observe_in_session(host->title(), *session, 150).available);
  EXPECT_EQ(ep::observe_in_session(host->title(), *session, 0).reason, "invalid_private_dispatch_bound");
  host->files["/proc/81/stat"] = stat_line(81, 299, 'S', 101);
  EXPECT_EQ(ep::observe_in_session(host->title(), *session, 300).reason, "game_predates_private_dispatch");
  host->files["/proc/81/stat"] = stat_line(81, 400, 'S', 101);
  host->files.erase("/proc/81/maps");
  auto seen = ep::observe_in_session(host->title(), *session, 300);
  EXPECT_FALSE(seen.available);
  EXPECT_FALSE(ep::adopt_in_session(host->title(), *session, 300, seen, error));
  host->files["/proc/81/maps"] = host->maps();
  host->files["/proc/100/stat"] = stat_line(100, 201, 'S', 10);
  seen = ep::observe_in_session(host->title(), *session, 300);
  EXPECT_FALSE(seen.available);
  EXPECT_EQ(seen.reason, "private_supervisor_changed");
  EXPECT_FALSE(ep::adopt_in_session(host->title(), *session, 300, seen, error));
  host->files["/proc/100/stat"] = stat_line(100, 200, 'S', 10);
  // A protected task inside the generation (here under its compositor) still refuses.
  host->add_protected_process(84, 10003, 101);
  seen = ep::observe_in_session(host->title(), *session, 300);
  EXPECT_FALSE(seen.available);
  EXPECT_EQ(seen.reason, "private_process_inventory_incomplete");
  EXPECT_FALSE(ep::adopt_in_session(host->title(), *session, 300, seen, error));
  lease->published = false;
  seen = ep::observe_in_session(host->title(), *session, 300);
  EXPECT_FALSE(seen.available);
  EXPECT_EQ(seen.reason, "private_session_not_current");
  EXPECT_FALSE(ep::adopt_in_session(host->title(), *session, 300, seen, error));
  EXPECT_FALSE(ep::testing::pin_private_session(lease, "retired-generation", error, host));
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivateCensusExcludesOnlyProtectedTasksProvedOutsideTheGeneration) {
  const auto lease = host->private_tree();
  host->private_game();
  host->private_server();
  // Desktop tasks born before the supervisor. One has a root-owned proc directory, so only its
  // status and stat are readable. The desktop copy of the title descends from a protected one.
  host->add_protected_process(43, 50);
  host->add_protected_process(44, 60);
  host->identities["/proc/44"].uid = 0;
  host->files["/proc/41/stat"] = stat_line(41, 10001, 'S', 43);
  host->files["/proc/42/stat"] = stat_line(42, 10000, 'S', 43);
  const auto session = ep::testing::pin_private_session(lease, "private-generation", error, host);
  ASSERT_TRUE(session) << error;
  const auto seen = ep::observe_in_session(host->title(), *session, 300);
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.pid, 81);
  host->fd_pid = 81;
  auto grant = ep::adopt_in_session(host->title(), *session, 300, seen, error);
  ASSERT_TRUE(grant) << error;

  // A later SSH login or polkit prompt is born after the supervisor, under a desktop task.
  host->add_protected_process(45, 500, 43);
  host->add_protected_process(46, 510, 45);
  host->identities["/proc/46"].uid = 0;
  EXPECT_TRUE(ep::inspect(*grant).available);
  // A reused parent PID, younger than its child, leaves the ancestry unproven.
  host->files["/proc/45/stat"] = stat_line(45, 520, 'S', 43);
  EXPECT_EQ(ep::observe_in_session(host->title(), *session, 300).reason, "private_ancestry_unproven");
  host->files["/proc/45/stat"] = stat_line(45, 500, 'S', 43);
  EXPECT_TRUE(ep::inspect(*grant).available);
  // Outside the private census, the same unreadable owner task still refuses.
  EXPECT_EQ(observe().reason, "unreadable_owner_process");
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "unreadable_owner_process");
  // After its outside proof, the same task may run a readable file and stays outside.
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/81/mountinfo") {
      host->denied_links.erase("/proc/43/exe");
      host->links["/proc/43/exe"] = "/usr/bin/ssh-agent";
    }
  };
  EXPECT_TRUE(ep::inspect(*grant).available);
  // But if task 45 exits after its proof and a new task inside the generation reuses its PID,
  // the final bookend refuses.
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/81/mountinfo") {
      host->files["/proc/45/stat"] = stat_line(45, 600, 'S', 101);
    }
  };
  EXPECT_EQ(ep::inspect(*grant).reason, "protected_process_changed");
  EXPECT_FALSE(ep::ask_to_close(*grant, 1s).signalled);
  EXPECT_EQ(host->opened, std::vector<std::uint64_t> {81});
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivateSelectionRequiresAMonotonicKnownAncestryAndFreshPrefixServer) {
  const auto lease = host->private_tree();
  host->private_game(81, 400, 83);
  host->private_server();
  const auto session = ep::testing::pin_private_session(lease, "private-generation", error, host);
  ASSERT_TRUE(session) << error;
  EXPECT_FALSE(ep::observe_in_session(host->title(), *session, 300).available);
  host->add_process(83, "/bin/sh", "/fixture/owner", 401, 101);
  // A parent younger than its child (a reused PID) already fails the stat-only membership walk.
  EXPECT_EQ(ep::observe_in_session(host->title(), *session, 300).reason, "private_ancestry_unproven");
  host->files["/proc/83/stat"] = stat_line(83, 350, 'S', 101);
  host->files["/proc/82/stat"] = stat_line(82, 299, 'S', 101);
  const auto seen = ep::observe_in_session(host->title(), *session, 300);
  EXPECT_EQ(seen.reason, "prefix_server_predates_private_dispatch");
  EXPECT_FALSE(ep::adopt_in_session(host->title(), *session, 300, seen, error));
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, RetiredPrivateLeaseDuringPidfdAdoptionClosesWithoutSignalling) {
  const auto lease = host->private_tree();
  host->private_game();
  host->private_server();
  const auto session = ep::testing::pin_private_session(lease, "private-generation", error, host);
  ASSERT_TRUE(session) << error;
  const auto seen = ep::observe_in_session(host->title(), *session, 300);
  ASSERT_TRUE(seen.available) << seen.reason;
  host->fd_pid = 81;
  host->after_open = [&] { lease->published = false; };
  EXPECT_FALSE(ep::adopt_in_session(host->title(), *session, 300, seen, error));
  EXPECT_EQ(host->opened, std::vector<std::uint64_t> {81});
  EXPECT_EQ(host->closed, std::vector<int> {77});
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, PrivateGrantRevokesWhenItsSupervisorChangesBeforeClose) {
  const auto lease = host->private_tree();
  host->private_game();
  host->private_server();
  const auto session = ep::testing::pin_private_session(lease, "private-generation", error, host);
  ASSERT_TRUE(session) << error;
  const auto seen = ep::observe_in_session(host->title(), *session, 300);
  ASSERT_TRUE(seen.available) << seen.reason;
  host->fd_pid = 81;
  auto grant = ep::adopt_in_session(host->title(), *session, 300, seen, error);
  ASSERT_TRUE(grant) << error;
  host->files["/proc/100/stat"] = stat_line(100, 201, 'S', 10);
  EXPECT_FALSE(ep::ask_to_close(*grant, 1s).signalled);
  host->files["/proc/100/stat"] = stat_line(100, 200, 'S', 10);
  EXPECT_EQ(ep::inspect(*grant).reason, "adoption_revoked");
  EXPECT_EQ(host->opened, std::vector<std::uint64_t> {81});
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, AnUnreadableOwnerProcessAndNonBtrfsDeviceMismatchAreRefused) {
  host->identities["/proc/41"].uid = 0;
  EXPECT_EQ(observe().reason, "unreadable_owner_process");
  host->identities["/proc/41"].uid = 1000;
  host->files["/proc/41/mountinfo"] = "21 1 0:1 / / rw - tmpfs tmpfs rw\n52 21 0:34 / /fixture rw - ext4 /dev/fixture rw\n";
  EXPECT_EQ(observe().reason, "mapped_game_device_mismatch");
}

TEST_F(EpicProcess, CgroupNamespaceMustMakeServicePathsComparable) {
  host->links["/proc/41/ns/cgroup"] = "cgroup:[999]";
  EXPECT_EQ(observe().reason, "foreign_cgroup_namespace");
  host->links.erase("/proc/self/ns/cgroup");
  EXPECT_EQ(observe().reason, "unknown_cgroup_namespace");
}

TEST_F(EpicProcess, UnavailablePidfdPollRevokesWithoutSignal) {
  auto game = adopt();
  ASSERT_TRUE(game) << error;
  host->exit_state = exit_state_t::unavailable;
  EXPECT_FALSE(ep::ask_to_close(*game, 1s).signalled);
  EXPECT_TRUE(host->signals.empty());
  EXPECT_EQ(ep::inspect(*game).reason, "adoption_revoked");
}

TEST_F(EpicProcess, ALauncherMustBeUniqueOwnedAndOutsideTheService) {
  EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
  EXPECT_EQ(error, "open_epic_on_host_desktop");
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  auto found = ep::testing::desktop_launcher_executable(error, host);
  ASSERT_TRUE(found) << error;
  EXPECT_EQ(*found, "/usr/bin/EpicGamesLauncher");
  host->files["/proc/43/cgroup"] = "0::/user.slice/polaris.service\n";
  EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
  EXPECT_EQ(error, "launcher_in_polaris_cgroup");
  host->files["/proc/43/cgroup"] = "0::/user.slice/app.slice/epic.scope\n";
  host->add_process(44, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10004);
  EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
  EXPECT_EQ(error, "ambiguous_desktop_launcher");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, LauncherInventoryDistinguishesCompleteZeroAndCountsEveryOwnerCgroup) {
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 0U);
  EXPECT_TRUE(error.empty());
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  host->add_process(44, "/opt/epic/EpicGamesLauncher", "/fixture/owner", 10004);
  host->files["/proc/44/cgroup"] = "0::/user.slice/polaris.service\n";
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 2U);
  host->add_protected_process(45);
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "launcher_inventory_incomplete");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, LauncherInventoryNeverReportsZeroForAnUnreadableOwnerExecutable) {
  host->links.erase("/proc/41/exe");
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "unreadable_process_link");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, LauncherInventoryDoesNotCallARenamedUnassociatedAppImageAbsent) {
  host->fuse_launcher();
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  // The proven runtime and inner launcher are two known process identities.
  EXPECT_EQ(ep::testing::launcher_instances(error, host, &*proof), 2U);
  host->pids = {"44"};
  EXPECT_EQ(ep::testing::launcher_instances(error, host, &*proof), 1U);
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "launcher_census_unresolved");
  const fs::path renamed = "/fixture/owner/renamed-runtime";
  host->add_process(45, renamed, "/fixture/owner", 10005);
  host->headers[renamed] = std::string("\177ELF\0\0\0\0AI\002", 11);
  host->pids = {"45"};
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, &*proof));
  EXPECT_EQ(error, "launcher_census_unresolved");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, BlockerLabelNamesOnlyThePinnedAppImageBasenameWithoutChangingRefusal) {
  const fs::path app = "/fixture/owner/private-directory/Orca";
  host->add_process(45, app, "/fixture/owner", 10005);
  host->headers[app] = std::string("\177ELF\0\0\0\0AI\002", 11);
  std::string label = "stale";
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label));
  EXPECT_EQ(error, "launcher_census_unresolved");
  EXPECT_EQ(label, "Orca");
  EXPECT_EQ(host->proc_listings, 3); // one refusing census, two presentation bookends; no retry
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "launcher_census_unresolved");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, BlockerLabelDoesNotGuessAProgramFromAnUnknownOrUnsafeName) {
  for (const auto &name : {std::string("<img onerror=alert(1)>"), std::string("Orca\nInjected"),
         std::string("Orca\033[31m"), std::string("Orca\177"), std::string("Orca\302\240"),
         std::string(65, 'a'), std::string(" Orca"), std::string("Orca "), std::string("owner@example.com")}) {
    host = std::make_shared<fixture_t>();
    const auto app = fs::path("/fixture/owner") / name;
    host->add_process(45, app, "/fixture/owner", 10005);
    host->headers[app] = std::string("\177ELF\0\0\0\0AI\002", 11);
    std::string label = "stale";
    EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label));
    EXPECT_EQ(error, "launcher_census_unresolved");
    EXPECT_TRUE(label.empty()) << name;
  }
}

TEST_F(EpicProcess, BlockerLabelAllowsBoundedPlainBasenamesAndRejectsUntrustedFiles) {
  const auto app = fs::path("/fixture/owner") / std::string(64, 'a');
  host->add_process(45, app, "/fixture/owner", 10005);
  host->headers[app] = std::string("\177ELF\0\0\0\0AI\002", 11);
  std::string label;
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label));
  EXPECT_EQ(label, std::string(64, 'a'));
  for (const auto mode : {S_IFREG | 0777, S_IFREG | 04755}) {
    host->identities[app].mode = mode;
    host->identities["/proc/45/exe"] = host->identities[app];
    EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label));
    EXPECT_EQ(error, "launcher_census_unresolved");
    EXPECT_TRUE(label.empty());
  }
}

TEST_F(EpicProcess, BlockerLabelIsGenericForMultipleAppImageIdentitiesEvenWithTheSameName) {
  for (const auto &name : {"Other", "Orca"}) {
    host = std::make_shared<fixture_t>();
    for (const auto &[pid, app] : std::vector<std::pair<int, fs::path>> {{45, "/fixture/owner/Orca"}, {46, fs::path("/opt/other") / name}}) {
      host->add_process(pid, app, "/fixture/owner", 10000 + pid);
      host->headers[app] = std::string("\177ELF\0\0\0\0AI\002", 11);
    }
    std::string label = "stale";
    EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label));
    EXPECT_EQ(error, "launcher_census_unresolved");
    EXPECT_TRUE(label.empty());
  }
}

TEST_F(EpicProcess, BlockerLabelIsGenericWhenThePinnedIdentityChangesDuringProof) {
  for (const auto &change : {"start", "exe", "file", "owner", "boot", "header", "listing"}) {
    host = std::make_shared<fixture_t>();
    const fs::path app = "/fixture/owner/Orca";
    host->add_process(45, app, "/fixture/owner", 10005);
    host->headers[app] = std::string("\177ELF\0\0\0\0AI\002", 11);
    host->after_access = [&, change](const fs::path &path) {
      // The first header read is the admission's blocker observation. Mutate afterwards.
      if (path != "/proc/45/exe" || host->access_counts[path] != 3) return;
      if (std::string(change) == "start") host->files["/proc/45/stat"] = stat_line(45, 99999);
      if (std::string(change) == "exe") host->links["/proc/45/exe"] = "/fixture/owner/Other";
      if (std::string(change) == "file") ++host->identities["/proc/45/exe"].inode;
      if (std::string(change) == "owner") host->files["/proc/45/status"] = "Uid:\t1001\t1001\t1001\t1001\n";
      if (std::string(change) == "boot") host->files["/proc/sys/kernel/random/boot_id"] = "add26721-4424-4907-ab80-4f023729a22f\n";
      if (std::string(change) == "header") host->headers[app] = std::string(11, 'x');
      if (std::string(change) == "listing") host->drop(45);
    };
    std::string label = "stale";
    EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label)) << change;
    EXPECT_EQ(error, "launcher_census_unresolved") << change;
    EXPECT_TRUE(label.empty()) << change;
    EXPECT_TRUE(host->opened.empty());
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, BlockerLabelNeverSurvivesUnknownOrSuccessfulOrForeignCensuses) {
  std::string label = "stale";
  EXPECT_EQ(ep::testing::launcher_instances(error, host, nullptr, &label), 0U);
  EXPECT_TRUE(label.empty());
  host->add_process(45, "/fixture/owner/Orca", "/fixture/owner", 10005);
  host->headers["/fixture/owner/Orca"] = std::string("\177ELF\0\0\0\0AI\002", 11);
  host->identities["/proc/45"].uid = 1001;
  host->files["/proc/45/status"] = "Uid:\t1001\t1001\t1001\t1001\n";
  EXPECT_EQ(ep::testing::launcher_instances(error, host, nullptr, &label), 0U);
  EXPECT_TRUE(label.empty());
  host->links.erase("/proc/41/exe");
  label = "stale";
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label));
  EXPECT_EQ(error, "unreadable_process_link");
  EXPECT_TRUE(label.empty());
}

TEST_F(EpicProcess, BlockerLabelIsGenericForIncompleteOrLateChangingBlockerSets) {
  // A late birth that is not an AppImage no longer empties the name (#588); a late AppImage birth still does.
  for (const auto &change : {"protected", "unreadable", "late_appimage", "late_foreign_owner", "late_appimage_birth"}) {
    host = std::make_shared<fixture_t>();
    const fs::path app = "/fixture/owner/Orca";
    host->add_process(45, app, "/fixture/owner", 10005);
    host->headers[app] = std::string("\177ELF\0\0\0\0AI\002", 11);
    if (std::string(change) == "protected") host->add_protected_process(46);
    if (std::string(change) == "unreadable") host->headers.erase(runtime / "files/bin/wine64-preloader");
    if (std::string(change) == "late_foreign_owner") {
      host->add_process(46, "/fixture/other/Other", "/", 10006);
      host->headers["/fixture/other/Other"] = std::string("\177ELF\0\0\0\0AI\002", 11);
      host->identities["/proc/46"].uid = 1001;
      host->files["/proc/46/status"] = "Uid:\t1001\t1001\t1001\t1001\n";
    }
    host->before_listing = [&, change] {
      if (host->proc_listings != 3) return;
      if (std::string(change) == "late_appimage") {
        host->links["/proc/41/exe"] = "/fixture/owner/Other";
        host->headers["/fixture/owner/Other"] = std::string("\177ELF\0\0\0\0AI\002", 11);
      }
      if (std::string(change) == "late_foreign_owner") {
        host->identities["/proc/46"].uid = 1000;
        host->files["/proc/46/status"] = "Uid:\t1000\t1000\t1000\t1000\n";
      }
      if (std::string(change) == "late_appimage_birth") {
        host->add_process(46, "/fixture/owner/Other", "/", 10006);
        host->headers["/fixture/owner/Other"] = std::string("\177ELF\0\0\0\0AI\002", 11);
      }
    };
    // Put the known blocker first so the original reason is independent of later unreadability.
    host->pids.erase(std::find(host->pids.begin(), host->pids.end(), "45"));
    host->pids.insert(host->pids.begin(), "45");
    std::string label = "stale";
    EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label));
    EXPECT_EQ(error, "launcher_census_unresolved") << change;
    EXPECT_TRUE(label.empty()) << change;
    EXPECT_LE(host->proc_listings, 3);
  }
}

TEST_F(EpicProcess, BlockerLabelUsesOnlyTheRefusingCensusRemainingListingBudget) {
  host->pids.clear();
  host->before_listing = [&] {
    if (host->proc_listings < 15) {
      host->add_process(100 + host->proc_listings, "/usr/bin/worker", "/", 20000 + host->proc_listings);
    } else if (host->proc_listings == 15) {
      host->add_process(45, "/fixture/owner/Orca", "/", 10005);
      host->headers["/fixture/owner/Orca"] = std::string("\177ELF\0\0\0\0AI\002", 11);
    }
  };
  std::string label = "stale";
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label));
  EXPECT_EQ(error, "launcher_census_unresolved");
  EXPECT_TRUE(label.empty());
  EXPECT_EQ(host->proc_listings, 16);
  EXPECT_TRUE(host->signals.empty());
}

namespace {
  /**
   * papi's Orca on pc-papi as Lane B read it for #588 on 2026-11-11 (36a2999f): the AppImage runtime, which serves its
   * mount, and the Electron app inside the mount, both under the user manager; the app's helpers and crash handler;
   * and its node daemon, a plain executable unpacked outside the mount, in a systemd scope of its own. Only the
   * runtime carries the AppImage magic.
   */
  fs::path add_orca(fixture_t &host) {
    const fs::path mount = "/tmp/.mount_orca-lgdDMad";
    const fs::path runtime = "/fixture/owner/.local/opt/orca/1.4.180/orca-linux.AppImage";
    host.add_process(3885638, mount / "orca-ide", "/fixture/owner", 39019682, 1784);
    host.add_process(3885655, runtime, "/fixture/owner", 39019684, 1784);
    host.headers[runtime] = std::string("\177ELF\0\0\0\0AI\002", 11);
    for (const auto &[pid, start, parent] : std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> {
           {3885774, 39019695, 3885638}, {3885781, 39019696, 3885638}, {3885799, 39019699, 3885781}, {3886315, 39019762, 3885638},
           {3886325, 39019763, 3885638}, {3886507, 39019782, 3885638}, {3886640, 39019791, 3885799}, {3886752, 39019801, 3885638}}) {
      host.add_process(pid, mount / "orca-ide", "/fixture/owner", start, parent);
    }
    host.add_process(3886217, mount / "chrome_crashpad_handler", "/fixture/owner", 39019755, 1784);
    host.add_process(3886651, "/fixture/owner/.cache/orca/appimage/c8a1bc32c3d21cae77fd9dfa/efa87ac6f57503cff829d54d/resources/node-runtime/bin/node",
                     "/fixture/owner", 39019793, 3885638);
    host.files["/proc/3886651/cgroup"] = "0::" + host.manager + "/app.slice/orca-daemon.scope\n";
    return runtime;
  }
}  // namespace

TEST_F(EpicProcess, BlockerLabelNamesOrcaBesideTheRealHostsProtectedSessionTasks) {
  // #588 on pc-papi: with papi's Orca AppImage running, one owner launch answered with the generic refusal. The census
  // sets pc-papi's eight protected owner tasks aside by their session and system roles (D-A (B)); the name's own pass
  // refused on the first of them, so no desktop with a user manager could be named. Placed as the census places them,
  // they cannot be another AppImage, and Orca's runtime is the only task carrying the magic.
  host->pids.clear();
  host->desktop_session_host();
  add_orca(*host);
  std::string label = "stale";
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label));
  EXPECT_EQ(error, "launcher_census_unresolved");
  EXPECT_EQ(label, "orca-linux.AppImage");
  EXPECT_EQ(host->proc_listings, 3); // one refusing census, two presentation bookends; no retry
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, BlockerLabelIsGenericWhenAPlacedProtectedTaskChangesItsRoleBeforeTheNameIsShown) {
  // The same desktop, but kwin_wayland's unit changes between its placement and the name's last bookend: the
  // placement no longer holds, so the refusal stays generic, as it does for any other change during that proof.
  host->pids.clear();
  host->desktop_session_host();
  add_orca(*host);
  host->before_listing = [&] {
    if (host->proc_listings == 3) host->files["/proc/4966/cgroup"] = "0::" + host->manager + "/app.slice/mylauncher.service\n";
  };
  std::string label = "stale", why;
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label, &why));
  EXPECT_EQ(error, "launcher_census_unresolved");
  EXPECT_TRUE(label.empty());
  EXPECT_EQ(why, "blocker_set_changed");
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, BlockerLabelNamesOrcaWhileUnrelatedTasksComeAndGoDuringItsPass) {
  // #588 on RC4 (74a8ad4f), pc-papi 2026-11-12: Orca was the only AppImage, and the owner launch was still refused with
  // the generic text. The host's process list changed in 15 of 25 listings taken 100 ms apart, and the name pass
  // required two whole-system listings to be identical. It now judges only what each listing adds or changes, as the
  // census does, so births, exits and an exec that are not AppImages leave the name in place.
  host->pids.clear();
  host->desktop_session_host();
  add_orca(*host);
  host->add_process(700, "/usr/bin/docker", "/", 50700);
  host->add_process(701, "/usr/bin/bash", "/", 50701);
  host->add_foreign(702, 50702, "containerd-shim");
  host->before_listing = [&] {
    // The census refuses on its first listing. The pass's first two listings each see births and exits, the owner's and
    // another user's, and an unrelated task execs into another plain program. Its third listing is quiet.
    if (host->proc_listings == 2) {
      host->add_process(800, "/usr/bin/docker", "/", 50800);
      host->add_foreign(801, 50801, "runc");
      host->vanish(700);
      host->vanish(702);
    }
    if (host->proc_listings == 3) {
      host->add_process(810, "/usr/bin/sleep", "/", 50810);
      host->add_foreign(811, 50811, "runc");
      host->vanish(800);
      host->vanish(801);
      host->identities["/usr/bin/git"] = regular(70100);
      host->headers["/usr/bin/git"] = std::string("\177ELF\0\0\0\0\0\0\0", 11);
      host->links["/proc/701/exe"] = "/usr/bin/git";
      host->identities["/proc/701/exe"] = host->identities["/usr/bin/git"];
    }
  };
  // A new unrelated task exits while the pass reads it: listed, owned, then gone before its executable is opened.
  host->after_access = [&](const fs::path &path) {
    if (path == "/proc/810/exe") host->vanish(810);
  };
  std::string label = "stale", why = "stale";
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label, &why));
  EXPECT_EQ(error, "launcher_census_unresolved");
  EXPECT_EQ(label, "orca-linux.AppImage");
  EXPECT_TRUE(why.empty());
  EXPECT_EQ(host->proc_listings, 4); // one refusing census, three for the name; no retry
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());

  // The name reaches only the signed-in host console. A paired client gets the same generic refusal as before.
  const std::string generic = "Another AppImage app is running on the host, and Polaris can't tell it apart from Epic.";
  const std::string action = "Quit other AppImage apps on the host, then try Private Stream again, or use Mirror Desktop.";
  launch_failure::clear();
  launch_failure::refuse(503, "epic_other_appimage_running", generic, action, label + " (an AppImage) is running; quit it to use Private Stream.");
  const auto refusal = launch_failure::take();
  ASSERT_TRUE(refusal);
  EXPECT_EQ(launch_failure::status_message(*refusal), generic + " " + action);
  EXPECT_EQ(launch_failure::authenticated_owner_status_message(*refusal),
            "orca-linux.AppImage (an AppImage) is running; quit it to use Private Stream. " + action);
}

TEST_F(EpicProcess, BlockerLabelIsGenericWithAReasonWhenThePinnedAppImageGoesDuringItsPass) {
  // The one candidate the pass keeps: if Orca exits, is reaped or its PID is reused before the name is proved, the
  // refusal stays generic and the journal can say why.
  for (const auto &change : {"vanished", "exiting", "reused"}) {
    host = std::make_shared<fixture_t>();
    host->pids.clear();
    host->desktop_session_host();
    add_orca(*host);
    host->add_process(700, "/usr/bin/docker", "/", 50700);
    host->before_listing = [&, change] {
      if (host->proc_listings == 2) host->add_process(800, "/usr/bin/docker", "/", 50800);
      if (host->proc_listings != 3) return;
      host->vanish(700);
      if (std::string(change) == "vanished") host->vanish(3885655);
      if (std::string(change) == "exiting") host->exiting(3885655);
      if (std::string(change) == "reused") host->files["/proc/3885655/stat"] = stat_line(3885655, 39999999, 'S', 1784);
    };
    std::string label = "stale", why;
    EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label, &why)) << change;
    EXPECT_EQ(error, "launcher_census_unresolved") << change;
    EXPECT_TRUE(label.empty()) << change;
    EXPECT_EQ(why, "blocker_set_changed") << change;
    EXPECT_EQ(host->proc_listings, 3) << change;
    EXPECT_TRUE(host->opened.empty());
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, BlockerLabelStaysGenericForASecondAppImageWhetherItRunsThroughoutOrIsBornAmidChurn) {
  for (const auto &when : {"throughout", "born_amid_churn"}) {
    host = std::make_shared<fixture_t>();
    host->pids.clear();
    host->desktop_session_host();
    add_orca(*host);
    const fs::path other = "/fixture/owner/Applications/Other.AppImage";
    host->add_process(700, "/usr/bin/docker", "/", 50700);
    const auto second = [&] {
      host->add_process(900, other, "/fixture/owner", 50900, 1784);
      host->headers[other] = std::string("\177ELF\0\0\0\0AI\002", 11);
    };
    if (std::string(when) == "throughout") second();
    host->before_listing = [&, when] {
      if (host->proc_listings != 3) return;
      host->add_process(800, "/usr/bin/docker", "/", 50800);
      host->vanish(700);
      if (std::string(when) == "born_amid_churn") second();
    };
    std::string label = "stale", why;
    EXPECT_FALSE(ep::testing::launcher_instances(error, host, nullptr, &label, &why)) << when;
    EXPECT_EQ(error, "launcher_census_unresolved") << when;
    EXPECT_TRUE(label.empty()) << when;
    EXPECT_EQ(why, "multiple_appimage_blockers") << when;
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, BlockerReasonIsOneFixedCodeWheneverTheNameIsLeftOut) {
  // What the journal gets beside "launcher census unknown": never the name or a path, and nothing once it is named.
  const auto expect = [&](const std::optional<std::size_t> &count, const std::string &name, const std::string &reason) {
    std::string label = "stale", why = "stale";
    EXPECT_EQ(ep::testing::launcher_instances(error, host, nullptr, &label, &why), count) << reason;
    EXPECT_EQ(label, name) << reason;
    EXPECT_EQ(why, reason) << name;
    EXPECT_TRUE(host->signals.empty());
  };
  const auto orca = [&](const fs::path &app = "/fixture/owner/Orca") {
    host = std::make_shared<fixture_t>();
    host->add_process(45, app, "/fixture/owner", 10005);
    host->headers[app] = std::string("\177ELF\0\0\0\0AI\002", 11);
    host->pids.erase(std::find(host->pids.begin(), host->pids.end(), "45"));
    host->pids.insert(host->pids.begin(), "45");
  };

  // Admitted, or refused for another reason: there is nothing to name, and no reason.
  expect(0U, "", "");
  host->links.erase("/proc/41/exe");
  expect(std::nullopt, "", "");
  EXPECT_EQ(error, "unreadable_process_link");

  // A header that cannot be read refuses before any AppImage is pinned.
  host = std::make_shared<fixture_t>();
  host->headers.erase(runtime / "files/bin/wine64-preloader");
  expect(std::nullopt, "", "blocker_unpinned");
  EXPECT_EQ(error, "launcher_census_unresolved");

  orca("/fixture/owner/Orca\nInjected");
  expect(std::nullopt, "", "blocker_name_unpresentable");

  orca();
  host->identities["/fixture/owner/Orca"].mode = S_IFREG | 0777;
  host->identities["/proc/45/exe"] = host->identities["/fixture/owner/Orca"];
  expect(std::nullopt, "", "unsafe_blocker_file");

  orca();
  host->add_protected_process(46);
  expect(std::nullopt, "", "blocker_identity_incomplete");

  // Churn that never leaves a listing with nothing new to judge spends the census's listing budget.
  orca();
  host->before_listing = [&] { host->add_process(100 + host->proc_listings, "/usr/bin/worker", "/", 20000 + host->proc_listings); };
  expect(std::nullopt, "", "blocker_budget_exhausted");
  EXPECT_EQ(host->proc_listings, 16);

  // Named: no reason, and the census's own reason is unchanged.
  orca();
  expect(std::nullopt, "Orca", "");
  EXPECT_EQ(error, "launcher_census_unresolved");
}

TEST_F(EpicProcess, RetainedLauncherProofUsesTheOriginalAppImageAfterItsMountDisappears) {
  const auto appimage = host->fuse_launcher();
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  EXPECT_EQ(proof->path(), appimage);
  // The launcher, keeper, their descriptors and the mount no longer exist. Only the
  // already proven original AppImage file remains; no path is inferred from the mount.
  host->pids.clear();
  host->files.clear();
  host->links.clear();
  host->listings.clear();
  host->identities.erase("/tmp/.mount_EpicXXX/usr/bin/EpicGamesLauncher");
  host->reads.clear();
  EXPECT_TRUE(ep::verify_launcher(*proof, error)) << error;
  EXPECT_TRUE(host->reads.empty());
  host->identities[appimage].inode++;
  EXPECT_FALSE(ep::verify_launcher(*proof, error));
  EXPECT_EQ(error, "launcher_file_changed");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, RetainedNativeLauncherProofRechecksMagicAndOwnerWithoutAProcess) {
  const fs::path exe = "/usr/bin/EpicGamesLauncher";
  host->add_process(43, exe, "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->pids.clear();
  EXPECT_TRUE(ep::verify_launcher(*proof, error)) << error;
  host->headers[exe] = "nope";
  EXPECT_FALSE(ep::verify_launcher(*proof, error));
  EXPECT_EQ(error, "unknown_launcher_executable");
  host->headers[exe] = std::string("\177ELF", 4);
  host->uid = 1001;
  EXPECT_FALSE(ep::verify_launcher(*proof, error));
  EXPECT_EQ(error, "launcher_owner_changed");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupColdOpenObservationDoesNotInventAScanOrPublishAProof) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  ep::setup_history_t history;
  const auto facts = ep::testing::evaluate_setup(nullptr, nullptr, true, ep::prerequisite_state_t::unknown, host);
  EXPECT_EQ(facts.desktop, ep::desktop_state_t::open);
  EXPECT_EQ(facts.proof, ep::proof_state_t::required);
  EXPECT_EQ(facts.state, ep::readiness_state_t::needs_action);
  EXPECT_FALSE(history.proof());
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::required);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupReceiptRequiresSuccessfulMetadataAndMatchingDesktopBookends) {
  const auto appimage = host->fuse_launcher();
  const auto before = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(before) << error;
  ep::setup_history_t history;
  auto scan = history.begin_scan();
  EXPECT_EQ(history.observe_scan(scan, *before).state, ep::capture_state_t::captured);
  EXPECT_EQ(history.last_scan(), ep::scan_state_t::checking);
  const auto after = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(after) << error;
  const epic_library::snapshot_t empty {true, {}, {}};
  EXPECT_EQ(history.finish_scan(scan, empty, &*after, true), ep::scan_state_t::complete);
  EXPECT_GT(history.receipt_revision(), 0U);
  EXPECT_EQ(history.receipt(empty, true), ep::receipt_state_t::complete);
  auto different_metadata = empty;
  different_metadata.warnings.push_back("The metadata snapshot changed");
  EXPECT_EQ(history.receipt(different_metadata, true), ep::receipt_state_t::previous);
  // Only the proven original is needed after Epic exits and its AppImage mount disappears.
  host->pids.clear();
  host->identities.erase("/tmp/.mount_EpicXXX/usr/bin/EpicGamesLauncher");
  const auto facts = ep::testing::evaluate_setup(&*before, nullptr, true, ep::prerequisite_state_t::unknown, host);
  EXPECT_EQ(facts.desktop, ep::desktop_state_t::closed);
  EXPECT_EQ(facts.state, ep::readiness_state_t::launcher_closed);
  EXPECT_EQ(history.receipt(empty, facts.proof == ep::proof_state_t::valid), ep::receipt_state_t::complete);
  EXPECT_EQ(before->path(), appimage);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupProviderAndLaunchDiscoveryNeverCreateAScanReceipt) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  ep::setup_history_t history;
  EXPECT_EQ(history.capture(*proof, ep::capture_origin_t::provider_choice).state, ep::capture_state_t::captured);
  const auto generation = history.generation();
  EXPECT_EQ(history.capture(*proof, ep::capture_origin_t::launch_discovery).state, ep::capture_state_t::unchanged);
  EXPECT_EQ(history.origin(), ep::capture_origin_t::launch_discovery);
  EXPECT_EQ(history.generation(), generation);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::required);
  auto closed_scan = history.begin_scan();
  EXPECT_EQ(history.finish_scan(closed_scan, {true, {}, {}}, nullptr, true), ep::scan_state_t::launcher_unobserved);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::required);
}

TEST_F(EpicProcess, SetupFailedRescanKeepsOnlyExplicitPreviousHistory) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  ep::setup_history_t history;
  auto first = history.begin_scan();
  history.observe_scan(first, *proof);
  EXPECT_EQ(history.finish_scan(first, {false, {}, {}}, &*proof, true), ep::scan_state_t::metadata_unavailable);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::required);
  auto good = history.begin_scan();
  history.observe_scan(good, *proof);
  ASSERT_EQ(history.finish_scan(good, {true, {}, {}}, &*proof, true), ep::scan_state_t::complete);
  const auto receipt_revision = history.receipt_revision();
  auto failed = history.begin_scan();
  history.observe_scan(failed, *proof);
  EXPECT_EQ(history.finish_scan(failed, {false, {}, {}}, &*proof, true), ep::scan_state_t::metadata_unavailable);
  EXPECT_EQ(history.receipt_revision(), receipt_revision);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::previous);
  EXPECT_EQ(history.receipt({false, {}, {}}, true), ep::receipt_state_t::unknown);
  EXPECT_EQ(history.receipt({true, {}, {}}, false), ep::receipt_state_t::unknown);
}

TEST_F(EpicProcess, SetupSameFileRecapturePreservesReceiptButChangedFileDoesNot) {
  const fs::path exe = "/usr/bin/EpicGamesLauncher";
  host->add_process(43, exe, "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  ep::setup_history_t history;
  auto scan = history.begin_scan();
  history.observe_scan(scan, *proof);
  ASSERT_EQ(history.finish_scan(scan, {true, {}, {}}, &*proof, true), ep::scan_state_t::complete);
  const auto generation = history.generation(), receipt = history.receipt_revision();
  const auto recaptured = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(recaptured) << error;
  EXPECT_EQ(history.capture(*recaptured, ep::capture_origin_t::provider_choice).state, ep::capture_state_t::unchanged);
  EXPECT_EQ(history.generation(), generation);
  EXPECT_EQ(history.receipt_revision(), receipt);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::complete);
  ++host->identities[exe].changed_ns;
  host->identities["/proc/43/exe"] = host->identities[exe];
  const auto changed = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(changed) << error;
  EXPECT_FALSE(ep::same_launcher_file(*proof, *changed));
  EXPECT_EQ(history.capture(*changed, ep::capture_origin_t::launch_discovery).state, ep::capture_state_t::captured);
  EXPECT_GT(history.generation(), generation);
  EXPECT_EQ(history.receipt_revision(), 0U);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::required);
}

TEST_F(EpicProcess, SetupLauncherRestartOrExecBetweenBookendsCannotCompleteScan) {
  const fs::path exe = "/usr/bin/EpicGamesLauncher";
  host->add_process(43, exe, "/fixture/owner", 10003);
  const auto before = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(before) << error;
  ep::setup_history_t history;
  auto scan = history.begin_scan();
  history.observe_scan(scan, *before);
  host->files["/proc/43/stat"] = stat_line(43, 20003);
  const auto restarted = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(restarted) << error;
  EXPECT_TRUE(ep::same_launcher_file(*before, *restarted));
  EXPECT_FALSE(ep::same_launcher_observation(*before, *restarted));
  EXPECT_EQ(history.finish_scan(scan, {true, {}, {}}, &*restarted, true), ep::scan_state_t::launcher_changed);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::required);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupOldScanOrOldProcessEpochCannotOverwriteNewerReceipt) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  ep::setup_history_t history;
  auto old = history.begin_scan();
  history.observe_scan(old, *proof);
  auto newer = history.begin_scan();
  history.observe_scan(newer, *proof);
  ASSERT_EQ(history.finish_scan(newer, {true, {}, {}}, &*proof, true), ep::scan_state_t::complete);
  const auto revision = history.revision();
  EXPECT_EQ(history.finish_scan(old, {false, {}, {}}, nullptr, true), ep::scan_state_t::superseded);
  EXPECT_EQ(history.observe_scan(old, *proof).state, ep::capture_state_t::superseded);
  EXPECT_EQ(history.revision(), revision);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::complete);
  ep::setup_history_t restarted;
  auto first = restarted.begin_scan();
  restarted.observe_scan(first, *proof);
  EXPECT_EQ(restarted.finish_scan(newer, {true, {}, {}}, &*proof, true), ep::scan_state_t::superseded);
  EXPECT_EQ(restarted.receipt({true, {}, {}}, true), ep::receipt_state_t::required);
}

TEST_F(EpicProcess, SetupSessionChangeAndInvalidationRetireOutstandingScans) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  ep::setup_history_t history;
  auto scan = history.begin_scan();
  history.observe_scan(scan, *proof);
  EXPECT_EQ(history.finish_scan(scan, {true, {}, {}}, &*proof, false), ep::scan_state_t::session_busy);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::required);
  auto later = history.begin_scan();
  history.observe_scan(later, *proof);
  history.invalidate_receipt();
  EXPECT_EQ(history.finish_scan(later, {true, {}, {}}, &*proof, true), ep::scan_state_t::superseded);
}

TEST_F(EpicProcess, SetupReadyRequiresSelectedRuntimeAndKnownPrerequisitesButNotAReceipt) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->pids.clear();
  const auto title = host->title();
  const auto generic = ep::testing::evaluate_setup(&*proof, nullptr, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(generic.state, ep::readiness_state_t::launcher_closed);
  EXPECT_EQ(generic.runtime, ep::runtime_state_t::not_selected);
  EXPECT_EQ(ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::unknown, host).state, ep::readiness_state_t::unknown);
  EXPECT_EQ(ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::unavailable, host).state, ep::readiness_state_t::needs_action);
  const auto ready = ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(ready.desktop, ep::desktop_state_t::closed);
  EXPECT_EQ(ready.proof, ep::proof_state_t::valid);
  EXPECT_EQ(ready.runtime, ep::runtime_state_t::absent);
  EXPECT_EQ(ready.state, ep::readiness_state_t::ready) << ready.reason;
  const auto unproved = ep::testing::evaluate_setup(nullptr, &title, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(unproved.state, ep::readiness_state_t::needs_action);
  EXPECT_EQ(unproved.reason, "epic_launcher_proof_required");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupUnknownLauncherCensusAndPrivateCopyNeverMeanDesktopClosedOrOpen) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->files["/proc/43/cgroup"] = "0::/user.slice/polaris.service\n";
  auto facts = ep::testing::evaluate_setup(&*proof, nullptr, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.desktop, ep::desktop_state_t::unknown);
  EXPECT_EQ(facts.reason, "launcher_present_unplaced");
  host->pids.clear();
  host->add_protected_process(45);
  facts = ep::testing::evaluate_setup(&*proof, nullptr, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.desktop, ep::desktop_state_t::unknown);
  EXPECT_EQ(facts.state, ep::readiness_state_t::unknown);
  EXPECT_EQ(facts.reason, "launcher_inventory_incomplete");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupUnreadableHeaderHasGenericUnresolvedReason) {
  host->headers.erase(runtime / "files/bin/wine64-preloader");
  const auto facts = ep::testing::evaluate_setup(nullptr, nullptr, true, ep::prerequisite_state_t::unknown, host);
  EXPECT_EQ(facts.desktop, ep::desktop_state_t::unknown);
  EXPECT_EQ(facts.state, ep::readiness_state_t::unknown);
  EXPECT_EQ(facts.reason, "launcher_census_unresolved");
}

TEST_F(EpicProcess, SetupWindowsPrefixUnknownIsDistinctFromNativeAndBusyRuntime) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->vanish(43);
  auto title = host->title();
  auto facts = ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.runtime, ep::runtime_state_t::busy);
  EXPECT_EQ(facts.state, ep::readiness_state_t::needs_action);
  host->pids.clear();
  host->identities.erase(prefix_path);
  facts = ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.runtime, ep::runtime_state_t::unknown);
  EXPECT_EQ(facts.reason, "title_prefix_unavailable");
  title.platform = "Linux";
  host->headers[image] = std::string("\177ELF", 4);
  facts = ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.runtime, ep::runtime_state_t::absent);
  EXPECT_EQ(facts.state, ep::readiness_state_t::ready) << facts.reason;
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupUnknownPrerequisitesStillDiagnoseMissingAndUnreadableWindowsPrefix) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->pids.clear();
  const auto title = host->title();
  const auto prefix = host->identities.at(prefix_path);
  for (const bool unreadable : {false, true}) {
    if (unreadable) {
      host->identities[prefix_path] = prefix;
      host->denied_canonical.insert(prefix_path); // Present, but canonical inspection is refused.
    } else host->identities.erase(prefix_path);
    const auto facts = ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::unknown, host);
    EXPECT_EQ(facts.runtime, ep::runtime_state_t::unknown);
    EXPECT_EQ(facts.state, ep::readiness_state_t::unknown);
    EXPECT_EQ(facts.reason, "title_prefix_unavailable");
  }
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupUnknownPrerequisitesStillReportBusyRuntime) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->vanish(43); // Keep the fixture's exact game and wineserver running.
  const auto title = host->title();
  const auto facts = ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::unknown, host);
  EXPECT_EQ(facts.desktop, ep::desktop_state_t::closed);
  EXPECT_EQ(facts.runtime, ep::runtime_state_t::busy);
  EXPECT_EQ(facts.state, ep::readiness_state_t::needs_action);
  EXPECT_EQ(facts.reason, "epic_game_already_running");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupUnknownPrerequisitesReportAbsentRuntimeWithoutReadyForWindowsAndNative) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->pids.clear();
  auto title = host->title();
  for (const auto *platform : {"Windows", "Linux"}) {
    title.platform = platform;
    if (title.platform == "Linux") host->headers[image] = std::string("\177ELF", 4);
    const auto facts = ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::unknown, host);
    EXPECT_EQ(facts.desktop, ep::desktop_state_t::closed);
    EXPECT_EQ(facts.proof, ep::proof_state_t::valid);
    EXPECT_EQ(facts.runtime, ep::runtime_state_t::absent);
    EXPECT_EQ(facts.state, ep::readiness_state_t::unknown);
    EXPECT_EQ(facts.reason, "private_mode_unavailable");
  }
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupBusyReadDoesNoIoAndPreservesThreadLocalLaunchFailure) {
  launch_failure::refuse(418, "sentinel", "Existing attempt");
  const auto facts = ep::testing::evaluate_setup(nullptr, nullptr, false, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.state, ep::readiness_state_t::busy);
  EXPECT_EQ(facts.desktop, ep::desktop_state_t::not_checked);
  EXPECT_TRUE(host->reads.empty());
  const auto pending = launch_failure::take();
  ASSERT_TRUE(pending);
  EXPECT_EQ(pending->code, "sentinel");
  launch_failure::refuse(418, "second_sentinel", "Another attempt");
  ep::testing::evaluate_setup(nullptr, nullptr, true, ep::prerequisite_state_t::unknown, host);
  const auto after_idle_check = launch_failure::take();
  ASSERT_TRUE(after_idle_check);
  EXPECT_EQ(after_idle_check->code, "second_sentinel");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, SetupFileAndContextChangesNeverStayReady) {
  const fs::path exe = "/usr/bin/EpicGamesLauncher";
  host->add_process(43, exe, "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->pids.clear();
  ++host->identities[exe].inode;
  auto facts = ep::testing::evaluate_setup(&*proof, nullptr, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.proof, ep::proof_state_t::changed);
  EXPECT_NE(facts.state, ep::readiness_state_t::launcher_closed);
  --host->identities[exe].inode;
  host->links["/proc/self/ns/cgroup"] = "cgroup:[999]";
  facts = ep::testing::evaluate_setup(&*proof, nullptr, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.proof, ep::proof_state_t::unavailable);
  EXPECT_NE(facts.state, ep::readiness_state_t::launcher_closed);
  host->links["/proc/self/ns/cgroup"] = "cgroup:[123]";
  host->files["/proc/sys/kernel/random/boot_id"] = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
  facts = ep::testing::evaluate_setup(&*proof, nullptr, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.proof, ep::proof_state_t::unavailable);
  EXPECT_NE(facts.state, ep::readiness_state_t::launcher_closed);
}

TEST_F(EpicProcess, SetupCannotMixBackendsOrReuseAReceiptAfterObservedFileInvalidation) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  auto foreign = std::make_shared<fixture_t>();
  const auto facts = ep::testing::evaluate_setup(&*proof, nullptr, true, ep::prerequisite_state_t::available, foreign);
  EXPECT_EQ(facts.state, ep::readiness_state_t::unknown);
  EXPECT_EQ(facts.reason, "inspection_context_changed");
  EXPECT_TRUE(foreign->reads.empty());
  ep::setup_history_t history;
  auto scan = history.begin_scan();
  history.observe_scan(scan, *proof);
  ASSERT_EQ(history.finish_scan(scan, {true, {}, {}}, &*proof, true), ep::scan_state_t::complete);
  history.invalidate_receipt();
  // Restoring the same bytes or returning an older reply cannot restore the invalidated receipt.
  EXPECT_EQ(history.capture(*proof, ep::capture_origin_t::provider_choice).state, ep::capture_state_t::unchanged);
  EXPECT_EQ(history.receipt({true, {}, {}}, true), ep::receipt_state_t::required);
  EXPECT_EQ(history.finish_scan(scan, {true, {}, {}}, &*proof, true), ep::scan_state_t::superseded);
}

TEST_F(EpicProcess, SetupFinalProofCheckRejectsAnUpdateDuringTheSelectedRuntimeRead) {
  const fs::path exe = "/usr/bin/EpicGamesLauncher";
  host->add_process(43, exe, "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->pids.clear();
  bool updated = false;
  host->after_access = [&](const fs::path &path) {
    if (!updated && path == prefix_path) { ++host->identities[exe].changed_ns; updated = true; }
  };
  const auto title = host->title();
  const auto facts = ep::testing::evaluate_setup(&*proof, &title, true, ep::prerequisite_state_t::available, host);
  EXPECT_TRUE(updated);
  EXPECT_EQ(facts.proof, ep::proof_state_t::changed);
  EXPECT_EQ(facts.state, ep::readiness_state_t::unknown);
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, MountedLauncherElfCannotBeUsedAsAnUnprovenAppImage) {
  const fs::path exe = "/tmp/.mount_EpicXXX/usr/bin/EpicGamesLauncher";
  host->add_process(43, exe, "/fixture/owner", 10003);
  const std::string mounts = "21 1 0:1 / / rw - tmpfs tmpfs rw\n60 21 0:90 / /tmp/.mount_EpicXXX ro - fuse.AppImage ";
  host->files["/proc/43/mountinfo"] = mounts + "EpicGamesLauncher.appimage ro\n";
  EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
  EXPECT_EQ(error, "launcher_appimage_path_unproven");

  const fs::path appimage = "/fixture/owner/My Desktop/EpicGamesLauncher.appimage";
  host->identities[appimage] = regular(9900);
  host->headers[appimage] = std::string("\177ELF\0\0\0\0AI\002", 11);
  host->files["/proc/43/mountinfo"] = mounts + escaped(appimage) + " ro\n";
  const auto found = ep::testing::desktop_launcher_executable(error, host);
  ASSERT_TRUE(found) << error;
  EXPECT_EQ(*found, appimage);
  host->identities[appimage].uid = 1001;
  EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
  EXPECT_EQ(error, "unsafe_launcher_appimage");
}

TEST_F(EpicProcess, KnownOwnerAppImageProcessDoesNotProveTheMountedLauncherSource) {
  const fs::path exe = "/tmp/.mount_EpicXXX/usr/bin/EpicGamesLauncher";
  const fs::path appimage = "/fixture/owner/My Desktop/EpicGamesLauncher.appimage";
  host->add_process(43, exe, "/fixture/owner", 10003);
  host->add_process(44, appimage, "/fixture/owner", 10004);
  host->headers[appimage] = std::string("\177ELF\0\0\0\0AI\002", 11);
  // A matching live executable and its open original file still do not identify which FUSE
  // mount it serves. Without that relationship, the basename is not a backing-file proof.
  host->links["/proc/44/fd/3"] = appimage;
  host->identities["/proc/44/fd/3"] = host->identities[appimage];
  const std::string mounts = "21 1 0:1 / / rw - tmpfs tmpfs rw\n60 21 0:90 / /tmp/.mount_EpicXXX ro - fuse.AppImage ";
  for (const std::string source : {"EpicGamesLauncher.appimage", "../EpicGamesLauncher.appimage", "/fixture/owner/../owner/My\\040Desktop/EpicGamesLauncher.appimage"}) {
    host->files["/proc/43/mountinfo"] = mounts + source + " ro\n";
    EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
    EXPECT_EQ(error, "launcher_appimage_path_unproven");
  }
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, TheRetainedProofSaysWhetherItIsAnAppImageThatMountsItself) {
  const auto appimage = host->fuse_launcher();
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  EXPECT_TRUE(proof->appimage());
  // Its runtime is read up to the file's size, and only a runtime naming the flag qualifies.
  host->identities[appimage].size = 11;
  EXPECT_FALSE(ep::appimage_mounts_itself(*proof));
  auto with_flag = std::string("\177ELF\0\0\0\0AI\002", 11) + "usage: --appimage-mount\n";
  host->headers[appimage] = with_flag;
  EXPECT_FALSE(ep::appimage_mounts_itself(*proof)) << "the retained size still bounds the read";
  const auto retained = [&] {
    host = std::make_shared<fixture_t>();
    const auto path = host->fuse_launcher();
    host->identities[path].size = with_flag.size();
    host->headers[path] = with_flag;
    host->identities["/proc/44/exe"] = host->identities[path];
    host->identities["/proc/44/fd/3"] = host->identities[path];
    return ep::testing::prove_launcher(error, host);
  }();
  ASSERT_TRUE(retained) << error;
  EXPECT_TRUE(ep::appimage_mounts_itself(*retained));
  // A plain ELF launcher is not an AppImage, whatever its bytes say.
  host = std::make_shared<fixture_t>();
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto native = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(native) << error;
  EXPECT_FALSE(native->appimage());
  host->headers["/usr/bin/EpicGamesLauncher"] += "--appimage-mount";
  EXPECT_FALSE(ep::appimage_mounts_itself(*native));
}

TEST_F(EpicProcess, FuseConnectionAndOpenImageIdentifyTheOriginalAppImage) {
  const auto appimage = host->fuse_launcher();
  const auto found = ep::testing::desktop_launcher_executable(error, host);
  ASSERT_TRUE(found) << error;
  EXPECT_EQ(*found, appimage);
  EXPECT_NE(*found, "/tmp/.mount_EpicXXX/usr/bin/EpicGamesLauncher");
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, LauncherPresenceDoesNotRequireUnrelatedProcessAccessOrGrantGameAuthority) {
  const auto appimage = host->fuse_launcher();
  // The game process is uninspectable. It cannot supply a launcher candidate, and must still
  // make the strict game observation unavailable rather than silently authorizing adoption.
  host->links.erase("/proc/41/exe");
  const auto found = ep::testing::desktop_launcher_executable(error, host);
  ASSERT_TRUE(found) << error;
  EXPECT_EQ(*found, appimage);
  const auto game = observe();
  EXPECT_FALSE(game.available);
  EXPECT_EQ(game.reason, "unreadable_process_link");
  EXPECT_FALSE(ep::testing::adopt(host->title(), game, error, host));
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());

  // A visible candidate with incomplete identity still refuses; this is no blanket skip.
  host->links.erase("/proc/43/cwd");
  EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
  EXPECT_EQ(error, "unreadable_process_link");
}

TEST_F(EpicProcess, FuseConnectionMetadataMustBeUniquePositiveAndMatchTheMount) {
  host->fuse_launcher();
  for (const std::string info : {"mnt_id: 90\n", "fuse_connection: 91\n", "fuse_connection: 0\n",
                                 "fuse_connection: -90\n", "fuse_connection: 90 extra\n",
                                 "fuse_connection: 90\nfuse_connection: 90\n"}) {
    host->files["/proc/44/fdinfo/5"] = info;
    EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host)) << info;
    EXPECT_EQ(error, "launcher_appimage_path_unproven");
  }
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, FuseProofRequiresTheOriginalOpenImageAndRealFuseDevice) {
  const std::vector<std::function<void()>> changes {
    [&] { host->listings["/proc/44/fd"] = {"5"}; },
    [&] { host->identities["/proc/44/fd/3"].inode++; },
    [&] { host->links["/proc/44/fd/3"] = "/fixture/owner/other.appimage"; },
    [&] { host->identities["/proc/44/fd/5"].inode++; },
    [&] { host->identities["/dev/fuse"].mode = S_IFREG | 0600; },
    [&] { host->headers[host->links["/proc/44/exe"]] = std::string("\177ELF\0\0\0\0\0\0\0", 11); },
    [&] { host->identities[host->links["/proc/44/exe"]].mode |= S_IWGRP; },
  };
  for (const auto &change : changes) {
    host = std::make_shared<fixture_t>();
    host->fuse_launcher();
    change();
    EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
    EXPECT_TRUE(host->opened.empty());
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, FuseProofRejectsDifferentNamespaceMountOrServiceAndAmbiguousDescriptors) {
  const std::vector<std::function<void()>> changes {
    [&] { host->links["/proc/44/ns/mnt"] = "mnt:[322]"; },
    [&] { host->links.erase("/proc/self/ns/mnt"); },
    [&] { host->files["/proc/44/mountinfo"] = "60 21 0:91 / /tmp/.mount_EpicXXX ro - fuse.AppImage EpicGamesLauncher.appimage ro\n"; },
    [&] { host->files["/proc/44/cgroup"] = "0::/user.slice/polaris.service\n"; },
    [&] { host->listings["/proc/44/fd"].push_back("5"); },
    [&] { host->listings["/proc/44/fd"] = std::vector<std::string>(257, "5"); },
    [&] {
      host->listings["/proc/44/fd"].push_back("6");
      host->links["/proc/44/fd/6"] = "/dev/fuse";
      host->identities["/proc/44/fd/6"] = host->identities["/dev/fuse"];
      host->files["/proc/44/fdinfo/6"] = host->files["/proc/44/fdinfo/5"];
    },
  };
  for (const auto &change : changes) {
    host = std::make_shared<fixture_t>();
    host->fuse_launcher();
    change();
    EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
    EXPECT_EQ(error, "launcher_appimage_path_unproven");
  }
}

TEST_F(EpicProcess, FuseProofBookendsProcessImageNamespaceMountAndSelectedDescriptors) {
  const std::vector<std::function<void()>> changes {
    [&] { host->files["/proc/44/stat"] = stat_line(44, 10005); },
    [&] { host->identities[host->links["/proc/44/exe"]].inode++; },
    [&] { host->links["/proc/44/ns/mnt"] = "mnt:[322]"; },
    [&] { host->files["/proc/44/mountinfo"] = "60 21 0:91 / /tmp/.mount_EpicXXX ro - fuse.AppImage EpicGamesLauncher.appimage ro\n"; },
    [&] { host->links["/proc/44/fd/3"] = "/fixture/owner/replaced.appimage"; },
    [&] { host->links["/proc/44/fd/5"] = "/dev/null"; },
    [&] { host->files["/proc/44/fdinfo/5"] = "fuse_connection: 91\n"; },
  };
  for (const auto &change : changes) {
    host = std::make_shared<fixture_t>();
    host->fuse_launcher();
    bool changed = false;
    host->after_read = [&](const fs::path &path) {
      if (!changed && path == "/proc/44/fdinfo/5") {
        changed = true;
        change();
      }
    };
    EXPECT_FALSE(ep::testing::desktop_launcher_executable(error, host));
    EXPECT_TRUE(changed);
    EXPECT_TRUE(host->opened.empty());
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, Peer582ReadableHardlinkOfProvenNativeLauncherCannotCountAsZero) {
  host->pids.clear();
  const fs::path original = "/usr/bin/EpicGamesLauncher";
  host->add_process(7501, original, "/fixture/owner", 90001000);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  // Executed through another hard-link dentry: identical inode/file identity, different exe path.
  const fs::path alias = "/fixture/owner/launcher-alias";
  host->identities[alias] = host->identities[original];
  host->headers[alias] = host->headers[original];
  host->links["/proc/7501/exe"] = alias;
  ASSERT_TRUE(ep::verify_launcher(*proof, error)) << error;
  const auto result = ep::testing::launcher_instances(error, host, &*proof);
  ASSERT_TRUE(result.has_value()) << error;
  EXPECT_GT(*result, 0U) << "same retained native launcher inode was counted as absent";
}

TEST_F(EpicProcess, LauncherInventoryKnowsTheRetainedFileByIdentityAndBookendsTheCandidate) {
  host->pids.clear();
  const fs::path original = "/usr/bin/EpicGamesLauncher";
  const fs::path alias = "/fixture/owner/launcher-alias";
  host->add_process(7501, original, "/fixture/owner", 90001000);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->identities[alias] = host->identities[original];
  host->headers[alias] = host->headers[original];
  host->links["/proc/7501/exe"] = alias;
  // Once the identity matches, the alias is a full candidate: a path that no longer opens the same
  // file, or a link that changes while it is read, is unknown rather than a zero.
  host->identities.erase(alias);
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, &*proof));
  EXPECT_EQ(error, "unreadable_file_identity");
  host->identities[alias] = host->identities[original];
  int reads = 0;
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/7501/cgroup" && ++reads == 1) host->links["/proc/7501/exe"] = "/fixture/owner/other-alias";
  };
  EXPECT_FALSE(ep::testing::launcher_instances(error, host, &*proof));
  host->after_read = {};
  host->links["/proc/7501/exe"] = alias;
  EXPECT_EQ(ep::testing::launcher_instances(error, host, &*proof), 1U) << error;
  // A different file at a launcher-like path is still only a stable negative.
  host->add_process(7502, "/fixture/owner/launcher-copy", "/fixture/owner", 90001010);
  EXPECT_EQ(ep::testing::launcher_instances(error, host, &*proof), 1U) << error;
  host->drop(7501);
  EXPECT_EQ(ep::testing::launcher_instances(error, host, &*proof), 0U) << error;
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, Peer582CustomNamedProtectedApplicationServiceMustRemainUnknown) {
  host->pids.clear();
  host->desktop_session_host();
  // The same structural shape as systemd-run --user --unit=mylauncher --slice=session.slice.
  // An Epic-related protected task may choose an ordinary comm, or a Windows name may truncate.
  host->add_task(7500, "HogwartsLegacy.", 90000700, 1784,
    host->manager + "/session.slice/mylauncher.service", 1000, true);
  const auto launchers = ep::testing::launcher_instances(error, host);
  EXPECT_FALSE(launchers.has_value()) << "unexpected launcher census=" << (launchers ? std::to_string(*launchers) : error);
  const auto absent = ep::testing::title_runtime_absent(host->title(), error, host);
  EXPECT_FALSE(absent.has_value()) << "unexpected runtime absence=" << (absent ? (*absent ? "true" : "false") : error);
}

TEST_F(EpicProcess, PrivateAdmissionSetsAsideOnlyAUserServiceTheManagerReportsAsVendorInstalled) {
  std::string reason;
  host->pids.clear();
  host->desktop_session_host();
  // pc-papi's shape admits: its three user services are vendor units, each asked about once by a
  // census and once more at its end.
  ASSERT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;
  auto queries = host->unit_queries;
  std::sort(queries.begin(), queries.end());
  EXPECT_EQ(queries, (std::vector<std::string> {
    "plasma-kwin_wayland.service", "plasma-kwin_wayland.service", "plasma-polkit-agent.service",
    "plasma-polkit-agent.service", "ssh-agent.service", "ssh-agent.service"}));
  ASSERT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), true) << reason;

  const auto group = host->manager + "/session.slice/mylauncher.service";
  host->add_task(7500, "helper", 90000700, 1784, group, 1000, true);
  const auto refuses = [&](const char *unit) {
    EXPECT_FALSE(ep::testing::launcher_instances(error, host)) << unit;
    EXPECT_EQ(error, "launcher_inventory_incomplete") << unit;
    EXPECT_FALSE(ep::testing::title_runtime_absent(host->title(), reason, host)) << unit;
    EXPECT_EQ(reason, "title_runtime_inventory_incomplete") << unit;
  };
  // A manager that does not answer, or has no such unit loaded.
  refuses("unanswered query");
  const auto set = [&](const std::function<void(ep::testing::unit_t &)> &change) {
    host->vendor_unit(group);
    change(host->units["mylauncher.service"]);
  };
  // systemd-run --user --unit=mylauncher: a transient unit, whatever it is called.
  set([](auto &unit) { unit.transient = true; unit.fragment_path = "/run/user/1000/systemd/transient/mylauncher.service"; unit.drop_in_paths.clear(); });
  refuses("systemd-run transient");
  set([](auto &unit) { unit.fragment_path = "/run/user/1000/systemd/transient/mylauncher.service"; });
  refuses("a unit file outside the vendor directories");
  // A unit in the user's own unit directory, or a vendor unit a user drop-in changes.
  set([](auto &unit) { unit.fragment_path = "/fixture/owner/.config/systemd/user/mylauncher.service"; });
  refuses("a ~/.config user unit");
  set([](auto &unit) { unit.drop_in_paths.push_back("/fixture/owner/.config/systemd/user/mylauncher.service.d/override.conf"); });
  refuses("a user drop-in");
  set([](auto &unit) { unit.fragment_path.clear(); });
  refuses("a drop-in-only unit");
  // An answer about another unit or cgroup, or a unit that is not loaded.
  set([](auto &unit) { unit.control_group += "x"; });
  refuses("another cgroup");
  set([](auto &unit) { unit.id = "other.service"; });
  refuses("an alias");
  set([](auto &unit) { unit.load_state = "not-found"; });
  refuses("not loaded");
  // A vendor path is only one when its file and directory are root's and nobody else can write them.
  host->vendor_unit(group);
  host->identities["/usr/lib/systemd/user/mylauncher.service"].uid = 1000;
  refuses("an owner-written vendor path");
  host->vendor_unit(group);
  host->identities["/usr/lib/systemd/user/mylauncher.service"].mode |= S_IWOTH;
  refuses("a world-writable vendor file");
  host->vendor_unit(group);
  host->resolved["/usr/lib/systemd/user/mylauncher.service"] = "/fixture/owner/mylauncher.service";
  refuses("a vendor path that links elsewhere");
  host->resolved.clear();
  // The rule is the manager's account, not a name: the same task in a vendor unit is set aside.
  host->vendor_unit(group);
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 0U) << error;
  EXPECT_EQ(ep::testing::title_runtime_absent(host->title(), reason, host), true) << reason;
  EXPECT_TRUE(host->opened.empty());
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, Peer582RoleFactsChangedAtFinalBookendMustRemainUnknown) {
  for (const bool runtime_census : {false, true}) {
    host = std::make_shared<fixture_t>();
    host->pids.clear();
    host->desktop_session_host();
    int stat_reads = 0;
    host->after_read = [&](const fs::path &path) {
      // owner census first+bookend, role name, initial+final lineage, final census bookend.
      if (path == "/proc/5157/stat" && ++stat_reads == 6) {
        host->files["/proc/5157/stat"] = stat_line(5157, 2905, 'S', 1784, "EpicGamesLaunch");
        host->files["/proc/5157/status"] = "Name:\tEpicGamesLaunch\nUid:\t1000\t1000\t1000\t1000\n";
        host->files["/proc/5157/cgroup"] = "0::" + host->manager + "/app.slice/app-epic.service\n";
      }
    };
    if (runtime_census) {
      const auto result = ep::testing::title_runtime_absent(host->title(), error, host);
      EXPECT_FALSE(result.has_value()) << "runtime absence stayed definite after role facts changed";
    } else {
      const auto result = ep::testing::launcher_instances(error, host);
      EXPECT_FALSE(result.has_value()) << "launcher census stayed definite after role facts changed";
    }
    EXPECT_GE(stat_reads, 6);
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, PrivateAdmissionRequiresEveryRoleFactAgainAtTheCensusEnd) {
  // Each change keeps the task the same protected task (pid, start, owner, denied exe) and lands
  // after its role was proved, at the census's own final bookend.
  struct change_t {
    const char *what;
    std::function<void(fixture_t &)> apply;
  };
  const std::vector<change_t> changes {
    {"its name, to one the veto refuses", [](fixture_t &h) {
       h.files["/proc/5157/stat"] = stat_line(5157, 2905, 'S', 1784, "wineserver");
       h.files["/proc/5157/status"] = "Name:\twineserver\nUid:\t1000\t1000\t1000\t1000\n";
     }},
    {"its status name alone", [](fixture_t &h) { h.files["/proc/5157/status"] = "Name:\tssh-agent2\nUid:\t1000\t1000\t1000\t1000\n"; }},
    {"its cgroup, to another vendor unit", [&](fixture_t &h) {
       h.vendor_unit(h.manager + "/app.slice/other.service");
       h.files["/proc/5157/cgroup"] = "0::" + h.manager + "/app.slice/other.service\n";
     }},
    {"its parent, reparented to PID 1", [](fixture_t &h) { h.files["/proc/5157/stat"] = stat_line(5157, 2905, 'S', 1, "ssh-agent"); }},
    {"an ancestor's credentials", [](fixture_t &h) { h.files["/proc/1784/status"] = "Name:\tsystemd\nUid:\t1000\t0\t0\t0\n"; }},
    {"a login session's root parent, replaced", [](fixture_t &h) { h.files["/proc/561444/stat"] = stat_line(561444, 26805729, 'S', 1, "sshd-session"); }},
    {"what the manager reports for its unit", [](fixture_t &h) { h.units["ssh-agent.service"].drop_in_paths.push_back("/fixture/owner/.config/systemd/user/ssh-agent.service.d/x.conf"); }},
    {"the manager no longer answering", [](fixture_t &h) { h.units.erase("ssh-agent.service"); }},
  };
  for (const auto &change : changes) {
    for (const bool runtime_census : {false, true}) {
      host = std::make_shared<fixture_t>();
      host->pids.clear();
      host->desktop_session_host();
      // Placement reads Polaris's own cgroup once, and the final bookend reads it again first.
      int self_reads = 0;
      bool changed = false;
      host->after_read = [&](const fs::path &path) {
        if (path == "/proc/self/cgroup" && ++self_reads == 2 && !changed) {
          changed = true;
          change.apply(*host);
        }
      };
      std::string reason;
      const auto result = runtime_census ? ep::testing::title_runtime_absent(host->title(), reason, host).has_value() :
                                           ep::testing::launcher_instances(reason, host).has_value();
      EXPECT_TRUE(changed) << change.what;
      EXPECT_FALSE(result) << change.what << (runtime_census ? " (runtime)" : " (launcher)");
      EXPECT_EQ(reason, runtime_census ? "title_runtime_inventory_changed" : "launcher_inventory_changed") << change.what;
      EXPECT_TRUE(host->signals.empty());
    }
  }
}

TEST_F(EpicProcess, PrivateStatusIsUnverifiedAfterTheLaunchWindowYetAnEarlyBornGameIsStillRetained) {
  constexpr std::uint64_t request = 300, upper = 3300, after = 9000;
  const auto lease = host->private_tree();
  const auto session = ep::testing::pin_private_session(lease, "private-generation", error, host);
  ASSERT_TRUE(session) << error;
  // The URI reached a desktop launcher: only the desktop copy (41 and its server 42) exists.
  auto seen = ep::observe_in_session(host->title(), *session, request);
  ASSERT_EQ(seen.reason, "game_not_observed");
  EXPECT_EQ(ep::private_claim(seen, request, upper), ep::private_claim_t::keep_looking);
  EXPECT_EQ(ep::private_game_state(false, false, seen, 1000, upper), ep::private_game_t::waiting);
  EXPECT_EQ(ep::private_game_state(false, false, seen, upper, upper), ep::private_game_t::waiting);
  // After the window the desktop-only copy, however long it persists, reads as unverified, and
  // with no clock to tell the window either.
  EXPECT_EQ(ep::private_game_state(false, false, seen, upper + 1, upper), ep::private_game_t::unverified);
  EXPECT_EQ(ep::private_game_state(false, false, seen, after, upper), ep::private_game_t::unverified);
  EXPECT_EQ(ep::private_game_state(false, false, seen, std::nullopt, upper), ep::private_game_t::unverified);
  seen = ep::observe_in_session(host->title(), *session, request);
  EXPECT_EQ(ep::private_claim(seen, request, upper), ep::private_claim_t::keep_looking) << "the claim is not expired";
  EXPECT_TRUE(host->opened.empty());

  // An exact game born inside the window, first seen at a later poll or End, is still retained.
  host->private_game(81, 400);
  host->private_server(390);
  seen = ep::observe_in_session(host->title(), *session, request);
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(seen.start_ticks, 400U);
  ASSERT_EQ(ep::private_claim(seen, request, upper), ep::private_claim_t::retain);
  host->fd_pid = 81;
  const auto grant = ep::adopt_in_session(host->title(), *session, request, seen, error);
  ASSERT_TRUE(grant) << error;
  EXPECT_EQ(ep::private_game_state(true, false, ep::inspect(*grant), after, upper), ep::private_game_t::managed);
  EXPECT_TRUE(host->signals.empty());

  // One born after the window, or one that is ambiguous, retires the claim for good.
  host->files["/proc/81/stat"] = stat_line(81, upper + 1, 'S', 101);
  seen = ep::observe_in_session(host->title(), *session, request);
  ASSERT_TRUE(seen.available) << seen.reason;
  EXPECT_EQ(ep::private_claim(seen, request, upper), ep::private_claim_t::retire);
  EXPECT_EQ(ep::private_game_state(false, true, seen, 1000, upper), ep::private_game_t::unverified);
  host->files["/proc/81/stat"] = stat_line(81, 400, 'S', 101);
  host->private_game(83, 401);
  seen = ep::observe_in_session(host->title(), *session, request);
  EXPECT_EQ(seen.reason, "ambiguous_game_process");
  EXPECT_EQ(ep::private_claim(seen, request, upper), ep::private_claim_t::retire);
  EXPECT_EQ(ep::private_game_state(false, false, seen, 1000, upper), ep::private_game_t::unverified);
  EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, ChurnOfOtherUsersTasksLeavesBothCensusesDefinite) {
  for (const auto &census : censuses) {
    SCOPED_TRACE(census.name);
    // Born while the first listing's tasks are read, and gone before the census ends.
    host = quiet_host();
    int reads = 0;
    host->after_read = [&](const fs::path &path) {
      if (path == "/proc/60/status" && ++reads == 1) {
        host->add_foreign(7000, 90000000);
        host->vanish(61);
      }
    };
    const auto answer = census(host);
    EXPECT_TRUE(answer.admits) << census.name << ": " << answer.reason;
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, AKernelThreadStartedAtBootIsAnOrdinaryForeignTask) {
  for (const auto &census : censuses) {
    host = quiet_host();
    // kthreadd reports a start time of 0, which is no owner task's identity.
    host->add_foreign(2, 10002, "kthreadd");
    host->files["/proc/2/stat"] = stat_line(2, 0, 'S', 0, "kthreadd");
    const auto answer = census(host);
    EXPECT_TRUE(answer.admits) << census.name << ": " << answer.reason;
  }
}

TEST_F(EpicProcess, AnUnrelatedOwnerTaskBornMidCensusIsJudgedAndAdmits) {
  for (const auto &census : censuses) {
    host = quiet_host();
    int reads = 0;
    bool judged = false;
    host->after_read = [&](const fs::path &path) {
      if (path == "/proc/60/status" && ++reads == 1) {
        host->add_process(7000, "/usr/bin/docker", "/fixture/owner", 90000000);
      }
    };
    host->after_access = [&](const fs::path &path) { judged = judged || path == "/proc/7000/exe"; };
    const auto answer = census(host);
    EXPECT_TRUE(answer.admits) << census.name << ": " << answer.reason;
    EXPECT_TRUE(judged) << census.name << ": a newborn must be judged, not just tolerated";
  }
}

TEST_F(EpicProcess, AnOwnerTaskGoingAtAnyReadLeavesBothCensusesDefinite) {
  for (const auto &census : censuses) {
    at_every_read(60, quiet_host, [](fixture_t &h) { h.vanish(60); }, [&](const auto &h, int nth, int) {
      const auto answer = census(h);
      EXPECT_TRUE(answer.admits) << census.name << " at read " << nth << ": " << answer.reason;
      EXPECT_NE(answer.reason, "launcher_census_unresolved") << nth;
    });
  }
}

// A task exits before it is reaped: its status still reads, so only its stat's state can prove it.
TEST_F(EpicProcess, AnExitedTaskItsParentHasNotReapedCountsAsGone) {
  const auto desktop = [] {
    auto h = std::make_shared<fixture_t>();
    h->pids.clear();
    h->desktop_session_host();
    return h;
  };
  for (const auto &census : censuses) {
    // Its status still reads and its stat says so, while its links and maps read as missing.
    at_every_read(60, quiet_host, [](fixture_t &h) { h.zombie(60); }, [&](const auto &h, int nth, int) {
      const auto answer = census(h);
      EXPECT_TRUE(answer.admits) << census.name << " at read " << nth << ": " << answer.reason;
    });
    at_every_read(5157, desktop, [](fixture_t &h) { h.zombie(5157); }, [&](const auto &h, int nth, int) {
      const auto answer = census(h);
      EXPECT_TRUE(answer.admits) << census.name << " protected, at read " << nth << ": " << answer.reason;
    });
    // The stat's own state decides it. A live task with the same missing link is still a refusal.
    host = quiet_host();
    host->links.erase("/proc/60/exe");
    const auto answer = census(host);
    EXPECT_FALSE(answer.admits) << census.name;
    EXPECT_EQ(answer.reason, "unreadable_process_link") << census.name;
  }
  // So does a session's observation, where a refusal would retire the claim.
  std::shared_ptr<fixture_lease_t> lease;
  for (const std::uint64_t task : {60, 46}) {
    at_every_read(task, [&] { return session_host(lease); }, [task](fixture_t &h) { h.zombie(task); }, [&](const auto &h, int nth, int) {
      const auto session = ep::testing::pin_private_session(lease, "private-generation", error, h);
      ASSERT_TRUE(session) << error;
      const auto seen = ep::observe_in_session(h->title(), *session, 300);
      EXPECT_TRUE(seen.available) << task << " at read " << nth << ": " << seen.reason;
    });
  }
}

// As read on pc-papi at a refusal: state R, flags 0x40000c, no memory, no exe link. Mostly docker's tasks.
TEST_F(EpicProcess, ATaskTheKernelHasMarkedExitingCountsAsGone) {
  const auto desktop = [] {
    auto h = std::make_shared<fixture_t>();
    h->pids.clear();
    h->desktop_session_host();
    return h;
  };
  for (const auto &census : censuses) {
    // At any read, and however long it stays listed: a slow exit leaves no listing unquiet.
    at_every_read(60, quiet_host, [](fixture_t &h) { h.exiting(60); }, [&](const auto &h, int nth, int) {
      const auto answer = census(h);
      EXPECT_TRUE(answer.admits) << census.name << " at read " << nth << ": " << answer.reason;
    });
    at_every_read(5157, desktop, [](fixture_t &h) { h.exiting(5157); }, [&](const auto &h, int nth, int) {
      const auto answer = census(h);
      EXPECT_TRUE(answer.admits) << census.name << " protected, at read " << nth << ": " << answer.reason;
    });
    // Only that flag says so. A live task with every other flag set is still a refusal.
    host = quiet_host();
    host->files["/proc/60/stat"] = stat_line(60, 10060, 'R', 1, "tool", ~std::uint64_t {0x4});
    host->links.erase("/proc/60/exe");
    const auto answer = census(host);
    EXPECT_FALSE(answer.admits) << census.name;
    EXPECT_EQ(answer.reason, "unreadable_process_link") << census.name;
    // A candidate on its way out still changed: it is never skipped once known.
    host = quiet_host();
    host->add_process(70, census.runtime ? fs::path(runtime / "files/bin/wineserver") : fs::path("/usr/bin/EpicGamesLauncher"),
                      census.runtime ? fs::path("/tmp/.wine-1000/server-other") : fs::path("/fixture/owner"), 10070);
    host->before_listing = [&, listings = 0]() mutable {
      if (++listings == 2) {
        host->exiting(70);
      }
    };
    const auto changed = census(host);
    EXPECT_FALSE(changed.admits) << census.name;
    EXPECT_EQ(changed.reason, census.changed) << census.name;
  }
  // So does a session's observation, where a refusal would retire the claim.
  std::shared_ptr<fixture_lease_t> lease;
  for (const std::uint64_t task : {60, 46}) {
    at_every_read(task, [&] { return session_host(lease); }, [task](fixture_t &h) { h.exiting(task); }, [&](const auto &h, int nth, int) {
      const auto session = ep::testing::pin_private_session(lease, "private-generation", error, h);
      ASSERT_TRUE(session) << error;
      const auto seen = ep::observe_in_session(h->title(), *session, 300);
      EXPECT_TRUE(seen.available) << task << " at read " << nth << ": " << seen.reason;
    });
  }
}

// A thread's exec makes its leader a zombie until the thread takes over its PID and start time.
TEST_F(EpicProcess, ATaskReadAsExitedThatRunsAgainIsJudgedAgain) {
  for (const auto &census : censuses) {
    host = quiet_host();
    host->add_process(62, "/usr/bin/later", "/fixture/owner", 10062);
    host->zombie(60);
    host->after_read = [&, done = false](const fs::path &path) mutable {
      if (path == "/proc/62/status" && !done) {
        done = true;
        host->files["/proc/60/stat"] = stat_line(60, 10060);
        const fs::path exe = census.runtime ? fs::path(runtime / "files/bin/wine64-preloader") : fs::path("/usr/bin/EpicGamesLauncher");
        host->links["/proc/60/exe"] = exe;
        host->links["/proc/60/cwd"] = install;
        host->identities[exe] = regular(779);
        host->identities["/proc/60/exe"] = regular(779);
        host->headers[exe] = std::string("\177ELF\0\0\0\0\0\0\0", 11);
        host->files["/proc/60/maps"] = host->maps();
      }
    };
    const auto answer = census(host);
    EXPECT_FALSE(answer.admits) << census.name << ": " << answer.reason;
    if (census.runtime) {
      EXPECT_EQ(answer.absent, false) << answer.reason;
    }
    else {
      EXPECT_EQ(answer.launchers, std::optional<std::size_t> {1}) << answer.reason;
    }
  }
}

TEST_F(EpicProcess, AnOwnerTaskWhoseAbsenceTheKernelDoesNotProveStillRefuses) {
  for (const auto &census : censuses) {
    const std::vector<std::pair<const char *, std::function<void(fixture_t &)>>> unproven {
      {"status EIO", [](fixture_t &h) { h.hide(60); h.status_errors["/proc/60"] = EIO; }},
      {"directory EACCES", [](fixture_t &h) { h.hide(60); h.directory_errors["/proc/60"] = EACCES; }},
      {"a live or reused PID", [](fixture_t &h) { h.hide(60); h.live_absence_probes.insert("/proc/60"); }},
      {"a pidfd that cannot tell", [](fixture_t &h) { h.hide(60); h.absence_probe_errors["/proc/60"] = EMFILE; }},
    };
    for (const auto &[what, change] : unproven) {
      at_every_read(60, quiet_host, change, [&, what = what](const auto &h, int nth, int total) {
        const auto answer = census(h);
        // After the very last read of the last listing, nothing looks at the task again.
        if (nth == 0 || nth == total) {
          EXPECT_TRUE(answer.admits) << census.name << ' ' << what << ": " << answer.reason;
        }
        else {
          EXPECT_FALSE(answer.admits) << census.name << ' ' << what << " at read " << nth;
        }
      });
    }
  }
}

TEST_F(EpicProcess, ATaskThatExecsDuringItsOwnJudgementIsJudgedAgain) {
  for (const auto &census : censuses) {
    at_every_read(60, quiet_host, [](fixture_t &h) {
      h.links["/proc/60/exe"] = "/usr/bin/other";
      h.identities["/usr/bin/other"] = regular(777);
      h.identities["/proc/60/exe"] = regular(777);
      h.headers["/usr/bin/other"] = std::string("\177ELF\0\0\0\0\0\0\0", 11);
    }, [&](const auto &h, int nth, int) {
      const auto answer = census(h);
      EXPECT_TRUE(answer.admits) << census.name << " at read " << nth << ": " << answer.reason;
    });
  }
}

TEST_F(EpicProcess, ALauncherBornMidCensusIsCounted) {
  const auto with_birth = [](const std::function<void(fixture_t &)> &born) {
    auto h = quiet_host();
    h->after_read = [raw = h.get(), born, reads = 0](const fs::path &path) mutable {
      if (path == "/proc/60/status" && ++reads == 1) {
        born(*raw);
      }
    };
    return h;
  };
  // Named as the launcher.
  host = with_birth([](fixture_t &h) { h.add_process(7000, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 90000000); });
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 1U) << error;
  // Any other AppImage cannot be told apart from it.
  host = with_birth([](fixture_t &h) {
    h.add_process(7000, "/fixture/owner/renamed-runtime", "/fixture/owner", 90000000);
    h.headers["/fixture/owner/renamed-runtime"] = std::string("\177ELF\0\0\0\0AI\002", 11);
  });
  EXPECT_FALSE(ep::testing::launcher_instances(error, host));
  EXPECT_EQ(error, "launcher_census_unresolved");
  // The retained launcher's file, by a hard link.
  const fs::path original = "/usr/bin/EpicGamesLauncher";
  const fs::path alias = "/fixture/owner/launcher-alias";
  host = quiet_host();
  host->add_process(7501, original, "/fixture/owner", 90001000);
  const auto proof = ep::testing::prove_launcher(error, host);
  ASSERT_TRUE(proof) << error;
  host->drop(7501);
  int reads = 0;
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/60/status" && ++reads == 1) {
      host->identities[alias] = host->identities[original];
      host->headers[alias] = host->headers[original];
      host->add_process(7000, alias, "/fixture/owner", 90000000);
    }
  };
  EXPECT_EQ(ep::testing::launcher_instances(error, host, &*proof), 1U) << error;
}

TEST_F(EpicProcess, TheGameOrItsServerBornMidCensusIsBusy) {
  const auto census = censuses[1];
  // A task that maps the title image.
  host = quiet_host();
  int reads = 0;
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/60/status" && ++reads == 1) {
      host->add_process(7000, runtime / "files/bin/wine64-preloader", install, 90000000);
      host->files["/proc/7000/maps"] = host->maps();
    }
  };
  auto answer = census(host);
  EXPECT_EQ(answer.absent, false) << answer.reason;
  EXPECT_EQ(answer.reason, "title_runtime_busy");
  // A Wine server in the title's exact server directory.
  host = quiet_host();
  reads = 0;
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/60/status" && ++reads == 1) {
      host->add_process(7000, runtime / "files/bin/wineserver", "/tmp/.wine-1000/server-34-e2a1113", 90000000);
    }
  };
  answer = census(host);
  EXPECT_EQ(answer.absent, false) << answer.reason;
  EXPECT_EQ(answer.reason, "title_runtime_busy");
}

TEST_F(EpicProcess, ACandidateThatChangesOrVanishesStillRefuses) {
  // A Wine server of another title is a row that does not make this one busy, so what it does is all
  // that decides the answer. So is a launcher only for the census that counts it.
  const auto with_row = [](const census_t &census) {
    auto h = quiet_host();
    if (census.runtime) {
      h->add_process(70, runtime / "files/bin/wineserver", "/tmp/.wine-1000/server-other", 10070);
    }
    else {
      h->add_process(70, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10070);
    }
    return h;
  };
  for (const auto &census : censuses) {
    // Each seam leaves it either not known to be a candidate yet, so skipped as any gone task, or
    // refused as a change: never skipped once known, and never read as another AppImage. A row is
    // known from its executable's link, and a launcher from the header read through that link's handle.
    const int known_after = census.runtime ? 1 : 3;
    at_every_read(70, [&] { return with_row(census); }, [](fixture_t &h) { h.vanish(70); }, [&](const auto &h, int nth, int total) {
      const auto answer = census(h);
      // After the very last read of the last listing, nothing looks at the task again.
      if (nth == 0 || nth == total) {
        return;
      }
      if (h->access_counts["/proc/70/exe"] >= known_after) {
        EXPECT_FALSE(answer.admits) << census.name << " at read " << nth << ": skipped once known to be a candidate";
        EXPECT_EQ(answer.reason, census.changed) << census.name << " at read " << nth;
      }
      else {
        EXPECT_TRUE(answer.admits) << census.name << " at read " << nth << ": " << answer.reason;
      }
    });

    // Born, judged and gone before the census ends.
    host = quiet_host();
    int reads = 0;
    host->after_read = [&](const fs::path &path) {
      if (path == "/proc/60/status" && ++reads == 1) {
        if (census.runtime) {
          host->add_process(70, runtime / "files/bin/wineserver", "/tmp/.wine-1000/server-other", 90000000);
        }
        else {
          host->add_process(70, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 90000000);
        }
      }
    };
    host->before_listing = [&, listings = 0]() mutable {
      if (++listings == 3) {
        host->vanish(70);
      }
    };
    const auto answer = census(host);
    EXPECT_FALSE(answer.admits) << census.name;
    EXPECT_EQ(answer.reason, census.changed) << census.name;
  }

  // A row that changes, or that the bookend cannot read any more.
  for (const auto &change : std::vector<std::function<void(fixture_t &)>> {
         [](fixture_t &h) { h.files["/proc/70/stat"] = stat_line(70, 20070); },
         [](fixture_t &h) { h.links["/proc/70/cwd"] = "/tmp/.wine-1000/server-moved"; },
         [](fixture_t &h) { h.hide(70); },
       }) {
    host = with_row(censuses[1]);
    host->before_listing = [&, listings = 0]() mutable {
      if (++listings == 2) {
        change(*host);
      }
    };
    const auto answer = censuses[1](host);
    EXPECT_FALSE(answer.absent.has_value());
    EXPECT_EQ(answer.reason, "title_runtime_inventory_changed");
  }
}

TEST_F(EpicProcess, ATaskThatExecsAfterItsJudgementIsJudgedAgain) {
  // Task 62 is read after 60 in the listing, so 60 has been judged by the time this happens.
  const auto host_with_exec = [](const fs::path &exe, const std::function<void(fixture_t &)> &extra) {
    auto h = quiet_host();
    h->add_process(62, "/usr/bin/later", "/fixture/owner", 10062);
    h->after_read = [raw = h.get(), exe, extra, done = false](const fs::path &path) mutable {
      if (path == "/proc/62/status" && !done) {
        done = true;
        raw->links["/proc/60/exe"] = exe;
        raw->identities["/proc/60/exe"] = regular(778);
        extra(*raw);
      }
    };
    return h;
  };
  // Same pid, same start time: the old name list could not see this.
  host = host_with_exec("/usr/bin/EpicGamesLauncher", [](fixture_t &h) {
    h.identities["/usr/bin/EpicGamesLauncher"] = regular(778);
    h.headers["/usr/bin/EpicGamesLauncher"] = std::string("\177ELF\0\0\0\0\0\0\0", 11);
  });
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 1U) << error;
  host = host_with_exec(runtime / "files/bin/wine64-preloader", [](fixture_t &h) {
    h.files["/proc/60/maps"] = h.maps();
    h.identities[runtime / "files/bin/wine64-preloader"] = regular(778);
    h.links["/proc/60/cwd"] = install;
    h.links["/proc/60/map_files/1000-2000"] = image;
  });
  const auto answer = censuses[1](host);
  EXPECT_EQ(answer.absent, false) << answer.reason;
  EXPECT_EQ(answer.reason, "title_runtime_busy");
}

TEST_F(EpicProcess, AForeignNewbornThatBecomesTheOwnersIsJudgedAgain) {
  host = quiet_host();
  int reads = 0;
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/60/status" && ++reads == 1) {
      host->add_foreign(7000, 90000000);
    }
    // Once judged as root's, it drops to the owner and execs the launcher, as a setuid helper can.
    if (path == "/proc/7000/status" && host->identities["/proc/7000"].uid == 0) {
      host->add_process(7000, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 90000000);
      host->drop(7000);
      host->pids.push_back("7000");
    }
  };
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 1U) << error;
}

TEST_F(EpicProcess, AnOwnerTaskReusingAForeignTasksPidIsNewAtTheNextListing) {
  host = quiet_host();
  int reads = 0;
  host->after_read = [&](const fs::path &path) {
    if (path == "/proc/62/status" && ++reads == 1) {
      host->vanish(61);
      host->add_process(61, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 90000000);
    }
  };
  host->add_process(62, "/usr/bin/later", "/fixture/owner", 10062);
  EXPECT_EQ(ep::testing::launcher_instances(error, host), 1U) << error;
}

TEST_F(EpicProcess, AProtectedTaskGoingLeavesTheCensusDefiniteAndItsUnitIsNotAskedAgain) {
  const auto desktop = [] {
    auto h = std::make_shared<fixture_t>();
    h->pids.clear();
    h->desktop_session_host();
    return h;
  };
  for (const auto &census : censuses) {
    // At any read of it, before or after it was placed.
    at_every_read(5157, desktop, [](fixture_t &h) { h.vanish(5157); }, [&](const auto &h, int nth, int) {
      const auto answer = census(h);
      EXPECT_TRUE(answer.admits) << census.name << " at read " << nth << ": " << answer.reason;
    });

    // Between its placement and the census end, its unit is asked about once, not twice.
    host = desktop();
    int self_reads = 0;
    host->after_read = [&](const fs::path &path) {
      if (path == "/proc/self/cgroup" && ++self_reads == 2) {
        host->vanish(5157);
      }
    };
    const auto answer = census(host);
    EXPECT_TRUE(answer.admits) << census.name << ": " << answer.reason;
    EXPECT_EQ(std::count(host->unit_queries.begin(), host->unit_queries.end(), "ssh-agent.service"), 1) << census.name;
    EXPECT_EQ(std::count(host->unit_queries.begin(), host->unit_queries.end(), "plasma-polkit-agent.service"), 2) << census.name;

    // A task whose absence the kernel does not prove is still a refusal, as is one that lives on and changes.
    host = desktop();
    self_reads = 0;
    host->after_read = [&](const fs::path &path) {
      if (path == "/proc/self/cgroup" && ++self_reads == 2) {
        host->hide(5157);
        host->live_absence_probes.insert("/proc/5157");
      }
    };
    EXPECT_FALSE(census(host).admits) << census.name;
  }
}

TEST_F(EpicProcess, AProtectedNewbornIsPlacedOrRefuses) {
  const auto desktop_with_birth = [](const std::function<void(fixture_t &)> &born) {
    auto h = std::make_shared<fixture_t>();
    h->pids.clear();
    h->desktop_session_host();
    // Born at the final bookend, so only the listing after it can see it.
    h->after_read = [raw = h.get(), born, self_reads = 0](const fs::path &path) mutable {
      if (path == "/proc/self/cgroup" && ++self_reads == 2) {
        born(*raw);
      }
    };
    return h;
  };
  for (const auto &census : censuses) {
    // A new SSH login's unprivileged task places as the login session's.
    host = desktop_with_birth([](fixture_t &h) {
      h.add_task(7600, "sshd-session", 90000000, 1941259, h.user + "/session-6028.scope", 1000, true);
    });
    auto answer = census(host);
    EXPECT_TRUE(answer.admits) << census.name << ": " << answer.reason;
    // Anything else refuses, as it does at the start of a census.
    host = desktop_with_birth([](fixture_t &h) {
      h.add_task(7600, "helper", 90000000, 1784, h.manager + "/session.slice/mylauncher.service", 1000, true);
    });
    answer = census(host);
    EXPECT_FALSE(answer.admits) << census.name;
    EXPECT_EQ(answer.reason, census.incomplete) << census.name;
  }
}

TEST_F(EpicProcess, ChurnThatNeverLeavesAQuietListingRefusesAtTheCapWithBoundedReads) {
  for (const auto &census : censuses) {
    host = quiet_host();
    std::uint64_t next = 7000;
    host->before_listing = [&] { host->add_process(next, "/usr/bin/docker", "/fixture/owner", 90000000 + next); ++next; };
    const auto answer = census(host);
    EXPECT_FALSE(answer.admits) << census.name;
    EXPECT_EQ(answer.reason, census.changed) << census.name;
    EXPECT_EQ(host->proc_listings, 16) << census.name;
    EXPECT_LE(host->reads.size(), 2000U) << census.name;
    EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, BootOwnerAndCgroupNamespaceChangesStillRefuse) {
  for (const auto &census : censuses) {
    for (const auto &change : std::vector<std::function<void(fixture_t &)>> {
           [](fixture_t &h) { h.files["/proc/sys/kernel/random/boot_id"] = "add26721-4424-4907-ab80-4f023729a22f\n"; },
           [](fixture_t &h) { h.uid = 1001; },
           [](fixture_t &h) { h.links["/proc/self/ns/cgroup"] = "cgroup:[999]"; },
         }) {
      host = quiet_host();
      host->before_listing = [&, listings = 0]() mutable {
        if (++listings == 2) {
          change(*host);
        }
      };
      const auto answer = census(host);
      EXPECT_FALSE(answer.admits) << census.name;
    }
  }
}

TEST_F(EpicProcess, ATaskVanishingDuringAPrivateObservationKeepsTheClaimAndTheGrant) {
  constexpr std::uint64_t request = 300, upper = 3300;
  for (const std::uint64_t task : {60, 46}) {
    // The claim is not retired, and a later look still retains the game.
    std::shared_ptr<fixture_lease_t> lease;
    at_every_read(task, [&] { return session_host(lease); }, [task](fixture_t &h) { h.vanish(task); }, [&](const auto &h, int nth, int) {
      const auto session = ep::testing::pin_private_session(lease, "private-generation", error, h);
      ASSERT_TRUE(session) << error;
      const auto seen = ep::observe_in_session(h->title(), *session, request);
      ASSERT_TRUE(seen.available) << task << " at read " << nth << ": " << seen.reason;
      EXPECT_EQ(seen.pid, 81);
      EXPECT_EQ(ep::private_claim(seen, request, upper), ep::private_claim_t::retain) << nth;
      EXPECT_TRUE(h->signals.empty());
    });
    // A retained grant is not revoked, and End can still close the exact game.
    const auto grant_host = session_host(lease);
    const auto session = ep::testing::pin_private_session(lease, "private-generation", error, grant_host);
    ASSERT_TRUE(session) << error;
    const auto seen = ep::observe_in_session(grant_host->title(), *session, request);
    ASSERT_TRUE(seen.available) << seen.reason;
    auto grant = ep::adopt_in_session(grant_host->title(), *session, request, seen, error);
    ASSERT_TRUE(grant) << error;
    int reads = 0;
    grant_host->after_access = [&](const fs::path &path) {
      if (under(path, task) && ++reads == 2) {
        grant_host->vanish(task);
      }
    };
    const auto inspected = ep::inspect(*grant);
    EXPECT_TRUE(inspected.available) << task << ": " << inspected.reason;
    EXPECT_EQ(inspected.revision, seen.revision);
    EXPECT_TRUE(ep::ask_to_close(*grant, 1s).closed);
    EXPECT_EQ(grant_host->signals, std::vector<int> {77});
  }
}

TEST_F(EpicProcess, AnAncestorExitingDuringAPrivateWalkReparentsTheTaskAndIsReadAfresh) {
  // Task 45 exits, so the kernel makes its child's parent the desktop's subreaper. Wherever that
  // lands in the walk, 46 is still proved outside rather than retiring the claim.
  std::shared_ptr<fixture_lease_t> lease;
  at_every_read(45, [&] { return session_host(lease); }, [](fixture_t &h) {
    h.vanish(45);
    h.files["/proc/46/stat"] = stat_line(46, 510, 'S', 43);
  }, [&](const auto &h, int nth, int) {
    const auto session = ep::testing::pin_private_session(lease, "private-generation", error, h);
    ASSERT_TRUE(session) << error;
    const auto seen = ep::observe_in_session(h->title(), *session, 300);
    EXPECT_TRUE(seen.available) << "at read " << nth << ": " << seen.reason;
  });
  // A member reparents to the supervisor, a subreaper, and is a member still. So one that stays
  // unproven refuses rather than being read as outside.
  const auto member = session_host(lease);
  const auto session = ep::testing::pin_private_session(lease, "private-generation", error, member);
  ASSERT_TRUE(session) << error;
  member->add_protected_process(47, 520, 101);
  member->add_protected_process(48, 530, 47);
  int reads = 0;
  member->after_read = [&](const fs::path &path) {
    if (path == "/proc/48/status" && ++reads == 2) {
      member->vanish(47);
      member->files["/proc/48/stat"] = stat_line(48, 530, 'S', 100);
    }
  };
  EXPECT_EQ(ep::observe_in_session(member->title(), *session, 300).reason, "private_process_inventory_incomplete");
}

TEST_F(EpicProcess, ThePrivateGamesOwnIdentityChangingStillRevokesTheGrant) {
  std::shared_ptr<fixture_lease_t> lease;
  const auto changes = std::vector<std::pair<const char *, std::function<void(fixture_t &)>>> {
    {"its start time, a reused pid", [](fixture_t &h) { h.files["/proc/81/stat"] = stat_line(81, 401, 'S', 101); }},
    {"its executable", [](fixture_t &h) { h.links["/proc/81/exe"] = runtime / "files/bin/other"; }},
    {"its mapping of the image", [](fixture_t &h) { h.files["/proc/81/maps"] = h.maps("00:22", 999); }},
    {"its prefix server", [](fixture_t &h) { h.files["/proc/82/stat"] = stat_line(82, 391, 'S', 101); }},
    {"the task's parent", [](fixture_t &h) { h.files["/proc/81/stat"] = stat_line(81, 400, 'S', 100); }},
    {"gone from under the grant", [](fixture_t &h) { h.vanish(81); }},
  };
  for (const auto &[what, change] : changes) {
    host = session_host(lease);
    const auto session = ep::testing::pin_private_session(lease, "private-generation", error, host);
    ASSERT_TRUE(session) << error;
    const auto seen = ep::observe_in_session(host->title(), *session, 300);
    ASSERT_TRUE(seen.available) << seen.reason;
    auto grant = ep::adopt_in_session(host->title(), *session, 300, seen, error);
    ASSERT_TRUE(grant) << error;
    change(*host);
    EXPECT_FALSE(ep::inspect(*grant).available) << what;
    EXPECT_FALSE(ep::inspect(*grant).available) << what;
    EXPECT_EQ(ep::inspect(*grant).reason, "adoption_revoked") << what;
    EXPECT_FALSE(ep::ask_to_close(*grant, 1s).signalled) << what;
    EXPECT_TRUE(host->signals.empty()) << what;
  }
}

TEST_F(EpicProcess, AGameBeingReadWhenItVanishesInASessionIsStillNotASkippedRow) {
  // Only a task not yet known to map the game can vanish unnoticed. Once it is a row, a look that
  // loses it is not a game that was never there: game_not_observed would keep the claim open.
  std::shared_ptr<fixture_lease_t> lease;
  std::set<std::string> reasons;
  at_every_read(81, [&] { return session_host(lease); }, [](fixture_t &h) { h.vanish(81); }, [&](const auto &h, int nth, int total) {
    const auto session = ep::testing::pin_private_session(lease, "private-generation", error, h);
    ASSERT_TRUE(session) << error;
    const auto seen = ep::observe_in_session(h->title(), *session, 300);
    // After its very last read nothing looks at the game again until the next observation.
    if (nth != 0 && nth != total) {
      EXPECT_FALSE(seen.available) << "at read " << nth;
      reasons.insert(seen.reason);
    }
  });
  EXPECT_TRUE(reasons.count("game_not_observed"));
  EXPECT_GT(reasons.size(), 1U);
}

// An exiting task's status can open and then fail to read, which is no different from failing to open.
TEST(EpicProcessReader, ATaskExitingBetweenTheOpenAndTheReadBeginsTheSameAbsenceProof) {
  EXPECT_TRUE(ep::testing::status_failure_may_be_absence(ENOENT));
  EXPECT_TRUE(ep::testing::status_failure_may_be_absence(ESRCH));
  for (const int error : {0, EACCES, EPERM, EIO, EMFILE, EINVAL}) {
    EXPECT_FALSE(ep::testing::status_failure_may_be_absence(error)) << error;
  }
  if (!fs::exists("/proc/self/stat")) {
    GTEST_SKIP() << "needs a real /proc";
  }
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    pause();
    _exit(0);
  }
  const fs::path process = fs::path("/proc") / std::to_string(child);
  // A live task's status reads.
  const auto live = ep::testing::real_initial_status(process, 65536);
  ASSERT_TRUE(live.text.has_value());
  // The kernel's own behaviour this rests on: the file opened first, the task gone before the read.
  const int opened = ::open((process / "status").c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(opened, 0);
  ASSERT_EQ(::kill(child, SIGKILL), 0);
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  char buffer[64];
  errno = 0;
  EXPECT_EQ(::read(opened, buffer, sizeof(buffer)), -1);
  EXPECT_EQ(errno, ESRCH);
  ::close(opened);
  // And once reaped, the whole proof holds: nothing at the path, and no pidfd for the pid.
  const auto reaped = ep::testing::real_initial_status(process, 65536);
  EXPECT_FALSE(reaped.text.has_value());
  EXPECT_EQ(reaped.absence, ep::testing::process_absence_t::vanished);
}

TEST(EpicProcessReader, FixtureFileReadsRequireEofWithinTheCapAndRefuseSymlinks) {
  auto pattern = (fs::temp_directory_path() / "polaris-epic-reader-XXXXXX").string();
  const auto made = mkdtemp(pattern.data());
  ASSERT_NE(made, nullptr);
  const fs::path root = made;
  struct cleanup_t {
    fs::path path;
    ~cleanup_t() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } cleanup {root};
  const auto file = root / "stat";
  const auto bytes = stat_line(41, 10001);
  std::ofstream(file, std::ios::binary) << bytes;
  EXPECT_EQ(ep::testing::read_bounded(file, bytes.size()), bytes);
  EXPECT_FALSE(ep::testing::read_bounded(file, bytes.size() - 1));
  EXPECT_FALSE(ep::testing::read_bounded(file, 0));
  fs::create_symlink(file, root / "link");
  EXPECT_FALSE(ep::testing::read_bounded(root / "link", 4096));
  std::ofstream(root / "empty", std::ios::binary);
  EXPECT_EQ(ep::testing::read_bounded(root / "empty", 4096), "");
  EXPECT_FALSE(ep::testing::read_bounded(root, 4096));
}

namespace {
  epic_library::flatpak_deployment_t flatpak_desktop_fixture(fixture_t &host) {
    epic_library::flatpak_deployment_t d {"user", "", "x86_64", "stable", std::string(64, 'a'),
      "/fixture/owner/.local/share/flatpak", {}, "/fixture/owner/.var/app/com.epicgames.launcher",
      "[Application]\nname=com.epicgames.launcher\ncommand=EpicGamesLauncher\nruntime=org.gnome.Platform/x86_64/51\n", ""};
    d.directory = d.root / "app/com.epicgames.launcher/x86_64/stable" / d.commit;
    const auto deployed = d.directory / "files/bin/EpicGamesLauncher";
    const auto file = regular(5000);
    host.identities[d.root] = directory(4999); // canonical(root) requires an explicit fixture directory.
    d.image_stamp = std::to_string(file.device) + ':' + std::to_string(file.inode) + ':' + std::to_string(file.uid) + ':' +
      std::to_string(file.mode) + ':' + std::to_string(file.size) + ':' + std::to_string(file.modified_ns) + ':' + std::to_string(file.changed_ns);
    host.identities[deployed] = file;
    host.headers[deployed] = std::string("\177ELF", 4);
    host.files[d.directory / "metadata"] = d.metadata;
    host.resolved[d.directory.parent_path() / "active"] = d.directory;
    host.add_process(43, "/app/bin/EpicGamesLauncher", "/fixture/owner", 230, 52);
    host.add_process(51, "/usr/bin/bwrap", "/fixture/owner", 210);
    host.add_process(52, "/usr/bin/bwrap", "/fixture/owner", 220, 51);
    // /app is absent in the host namespace. Only the process-root/deployment join works.
    host.identities.erase("/app/bin/EpicGamesLauncher");
    host.identities["/proc/43/exe"] = file;
    host.identities["/proc/43/root/app/bin/EpicGamesLauncher"] = file;
    host.links["/proc/self/ns/pid"] = "pid:[111]";
    host.links["/proc/self/ns/mnt"] = "mnt:[222]";
    host.links["/proc/43/ns/pid"] = host.links["/proc/52/ns/pid"] = "pid:[333]";
    host.links["/proc/43/ns/mnt"] = "mnt:[444]";
    host.links["/proc/43/ns/cgroup"] = "cgroup:[555]";
    const std::string group = "/user.slice/user-1000.slice/user@1000.service/app.slice/app-flatpak-com.epicgames.launcher-123456.scope";
    host.files["/proc/43/cgroup"] = host.files["/proc/52/cgroup"] = "0::" + group + '\n';
    host.files["/proc/43/mountinfo"] = "21 1 0:1 / / rw - tmpfs tmpfs rw\n52 21 0:52 /files /app ro - btrfs /dev/fixture ro\n53 21 0:1 /info /.flatpak-info ro - tmpfs tmpfs ro\n";
    const std::string info = "[Application]\nname=com.epicgames.launcher\n[Instance]\ninstance-id=123456\narch=x86_64\nbranch=stable\napp-commit=" + d.commit +
      "\napp-path=" + (d.directory / "files").string() + "\ninstance-path=" + d.app_home.string() + '\n';
    const fs::path record = "/run/user/1000/.flatpak/123456";
    host.identities[record] = directory(100);
    host.files[record / "info"] = host.files["/proc/43/root/.flatpak-info"] = info;
    host.files[record / "pid"] = "51\n";
    host.files[record / "bwrapinfo.json"] = "{\"child-pid\":52,\"pid-namespace\":333}";
    return d;
  }
  epic_library::installation_t direct_games_desktop_fixture(fixture_t &host) {
    const auto d = flatpak_desktop_fixture(host);
    epic_library::installation_t game {{"namespace", "item", "artifact"}, "install1", "A Game", "bin/Game.exe", "1",
      "/fixture/owner/Games/A Game", d.app_home / "data/com.epicgames.eglv2", "Windows", d};
    const auto image = game.install_dir / game.launch_executable;
    fs::path path = "/";
    std::uint64_t inode = 7000;
    bool shared = false;
    for (const auto &part : image.relative_path()) {
      path /= part;
      shared = shared || path == fs::path("/fixture/owner/Games");
      auto file = path == image ? regular(inode++) : directory(inode++);
      host.identities[path] = file;
      if (!shared) file.inode += 10000;
      host.identities[fs::path("/proc/43/root") / path.relative_path()] = file;
    }
    host.headers[image] = host.headers[fs::path("/proc/43/root") / image.relative_path()] = "MZfixture";
    return game;
  }
  epic_library::installation_t portal_games_desktop_fixture(fixture_t &host) {
    auto game = direct_games_desktop_fixture(host);
    game.document = epic_library::flatpak_document_t {"fixture-doc", std::string(32, 'a'), ":1.44", 1000, 70,
      "/run/user/1000/doc", "/fixture/owner/Games", "/run/user/1000/doc/fixture-doc/Games/A Game", {"read", "write"}};
    host.documents["fixture-doc"] = *game.document;
    const fs::path process_root = "/proc/43/root", shared = "/fixture/owner/Games";
    const fs::path exported = process_root / "run/flatpak/doc/fixture-doc/Games";
    const auto image = game.install_dir / game.launch_executable;
    fs::path prefix = "/";
    std::uint64_t inode = 8000;
    for (const auto &part : fs::path("/run/flatpak/doc/fixture-doc/Games").relative_path()) {
      prefix /= part; host.identities[process_root / prefix.relative_path()] = directory(inode++);
    }
    bool sharing = false;
    prefix = "/";
    for (const auto &part : image.relative_path()) {
      prefix /= part; sharing = sharing || prefix == shared;
      if (!sharing) continue;
      const auto view = prefix == shared ? exported : exported / prefix.lexically_relative(shared);
      auto file = host.identities.at(prefix); file.inode += 50000; file.device = makedev(0, 99);
      host.identities[view] = file; host.portal_origins[view] = prefix;
      host.identities.erase(process_root / prefix.relative_path());
    }
    host.headers[exported / image.lexically_relative(shared)] = "MZfixture";
    host.identities[process_root / "run/user"] = directory(inode++);
    host.identities[process_root / "run/user/1000"] = directory(inode++);
    host.links[process_root / "run/user/1000/doc"] = "../../flatpak/doc";
    host.files["/proc/self/mountinfo"] = "80 1 0:99 / /run/user/1000/doc rw - fuse.portal portal rw\n";
    host.files["/proc/43/mountinfo"] += "81 21 0:99 /by-app/com.epicgames.launcher /run/flatpak/doc rw - fuse.portal portal rw\n";
    return game;
  }
}

TEST_F(EpicProcess, FlatpakPortalGamesProofBindsDocumentsMountBackingPathsAndExactDesktopWithoutOwnership) {
  const auto game = portal_games_desktop_fixture(*host);
  const auto proof = ep::testing::prove_flatpak_desktop(game, error, host);
  ASSERT_TRUE(proof) << error;
  EXPECT_TRUE(ep::verify_flatpak_desktop(*proof, error)) << error;
  auto target = host->title(); target.flatpak = game.flatpak;
  const auto seen = ep::testing::observe(target, host);
  EXPECT_FALSE(ep::testing::adopt(target, seen, error, host));
  EXPECT_EQ(error, "epic_flatpak_game_control_unavailable");
  EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, FlatpakPortalGamesProofRefusesRevokedOriginServiceMountInstanceAndBackingPathDrift) {
  for (int change = 0; change < 10; ++change) {
    host = std::make_shared<fixture_t>();
    const auto game = portal_games_desktop_fixture(*host);
    const auto proof = ep::testing::prove_flatpak_desktop(game, error, host);
    ASSERT_TRUE(proof) << error;
    const fs::path view = "/proc/43/root/run/flatpak/doc/fixture-doc/Games/A Game/bin/Game.exe";
    switch (change) {
      case 0: host->documents["fixture-doc"].permissions = {"read"}; break;
      case 1: host->documents["fixture-doc"].origin = "/fixture/owner/Other"; break;
      case 2: host->documents["fixture-doc"].service = ":1.45"; break;
      case 3: {
        const std::string expected_root = "/by-app/com.epicgames.launcher";
        host->files["/proc/43/mountinfo"].replace(host->files["/proc/43/mountinfo"].find(expected_root), expected_root.size(), "/by-app/another.app");
        break;
      }
      case 4: host->portal_origins.erase(view); break;
      case 5: host->portal_origins[view] = "/fixture/owner/Games/Other/Game.exe"; break;
      case 6: host->files["/proc/43/stat"] = stat_line(43, 231, 'S', 52); break;
      case 7: host->identities[view].inode++; break;
      case 8: host->resolved[game.flatpak->directory.parent_path() / "active"] = game.flatpak->directory.parent_path() / std::string(64, 'b'); break;
      case 9: host->files["/proc/43/mountinfo"] += "82 81 0:100 / /run/flatpak/doc/fixture-doc rw - tmpfs tmpfs rw\n"; break;
    }
    EXPECT_FALSE(ep::verify_flatpak_desktop(*proof, error)) << change;
    const char *expected[] {"epic_flatpak_game_path_unavailable", "epic_flatpak_game_path_unavailable", "epic_flatpak_game_path_unavailable",
      "epic_flatpak_game_path_unavailable", "epic_flatpak_game_path_unavailable", "epic_flatpak_game_path_unavailable",
      "epic_flatpak_desktop_changed", "epic_flatpak_desktop_changed", "epic_flatpak_deployment_changed", "epic_flatpak_game_path_unavailable"};
    EXPECT_EQ(error, expected[change]) << change;
    EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, FlatpakDirectGamesProofPinsTheSelectedFilesToTheVerifiedDesktopWithoutOwnership) {
  const auto game = direct_games_desktop_fixture(*host);
  const auto proof = ep::testing::prove_flatpak_desktop(game, error, host);
  ASSERT_TRUE(proof) << error;
  EXPECT_TRUE(ep::verify_flatpak_desktop(*proof, error)) << error;
  auto target = host->title(); target.flatpak = game.flatpak;
  const auto seen = ep::testing::observe(target, host);
  EXPECT_FALSE(seen.available); EXPECT_EQ(seen.reason, "epic_flatpak_game_control_unavailable");
  EXPECT_FALSE(ep::testing::adopt(target, seen, error, host));
  EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, FlatpakDirectGamesProofNeverPromotesHostExistenceIntoAMissingSandboxView) {
  const auto game = direct_games_desktop_fixture(*host);
  ASSERT_TRUE(ep::testing::prove_flatpak_desktop(game, error, host)) << error;
  const auto view = fs::path("/proc/43/root") / (game.install_dir / game.launch_executable).relative_path();
  host->identities.erase(view);
  ASSERT_TRUE(host->stat(view)); // The older fixture accessor's host fallback is insufficient evidence.
  EXPECT_FALSE(ep::testing::prove_flatpak_desktop(game, error, host));
  EXPECT_EQ(error, "epic_flatpak_game_path_unavailable");
  EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, FlatpakDirectGamesProofRejectsDeploymentPidNamespaceAndPathDriftBeforeDispatch) {
  for (int change = 0; change < 9; ++change) {
    host = std::make_shared<fixture_t>();
    const auto game = direct_games_desktop_fixture(*host);
    const auto proof = ep::testing::prove_flatpak_desktop(game, error, host);
    ASSERT_TRUE(proof) << error;
    const auto file = game.install_dir / game.launch_executable;
    const auto view = fs::path("/proc/43/root") / file.relative_path();
    switch (change) {
      case 0: host->identities[view].inode++; break;
      case 1: host->identities[file].inode++; host->identities[view] = host->identities[file]; break;
      case 2: host->identities[fs::path("/proc/43/root") / game.install_dir.relative_path()].inode++; break;
      case 3: host->identities["/proc/43/root/fixture/owner"].mode = S_IFLNK | 0777; break;
      case 4: host->files["/proc/43/stat"] = stat_line(43, 231, 'S', 52); break;
      case 5: host->links["/proc/43/ns/mnt"] = "mnt:[666]"; break;
      case 6: host->resolved[game.flatpak->directory.parent_path() / "active"] = game.flatpak->directory.parent_path() / std::string(64, 'b'); break;
      case 7: host->resolved[game.install_dir] = "/fixture/owner/Games/Another"; break;
      case 8: host->headers.erase(view); break;
    }
    EXPECT_FALSE(ep::verify_flatpak_desktop(*proof, error)) << change;
    const char *expected[] {"epic_flatpak_game_path_unavailable", "epic_flatpak_desktop_changed", "epic_flatpak_game_path_unavailable",
      "epic_flatpak_game_path_unavailable", "epic_flatpak_desktop_changed", "epic_flatpak_desktop_changed",
      "epic_flatpak_deployment_changed", "epic_flatpak_game_path_unavailable", "epic_flatpak_game_path_unavailable"};
    EXPECT_EQ(error, expected[change]) << change;
    EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, FlatpakDesktopProofJoinsDeploymentKernelInstanceAndReadOnlyMountWithoutNativeAuthority) {
  const auto d = flatpak_desktop_fixture(*host);
  const auto proof = ep::testing::prove_flatpak_desktop(d, error, host);
  ASSERT_TRUE(proof) << error;
  EXPECT_TRUE(ep::verify_flatpak_desktop(*proof, error)) << error;
  EXPECT_FALSE(ep::testing::prove_launcher(error, host));
  EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
  EXPECT_EQ(host->access_counts["/app/bin/EpicGamesLauncher"], 0);
  for (const auto &path : host->reads) {
    EXPECT_NE(path.filename(), "environ"); EXPECT_NE(path.filename(), "cmdline");
    EXPECT_EQ(path.string().find("account"), std::string::npos);
  }
}

TEST_F(EpicProcess, FlatpakBtrfsBindingUsesExactExecutableViewsWithoutEquatingStatAndMountDevices) {
  const auto d = flatpak_desktop_fixture(*host);
  host->files["/proc/43/mountinfo"] = "21 1 0:1 / / rw - tmpfs tmpfs rw\n52 21 0:99 /files /app ro - btrfs /dev/fixture ro\n53 21 0:1 /info /.flatpak-info ro - tmpfs tmpfs ro\n";
  const auto proof = ep::testing::prove_flatpak_desktop(d, error, host);
  ASSERT_TRUE(proof) << error;
  EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, FlatpakNsfsUserHandleDoesNotInvalidateTheProvenDesktopAndTitle) {
  const auto game = direct_games_desktop_fixture(*host);
  ASSERT_TRUE(ep::testing::prove_flatpak_desktop(game, error, host)) << error;
  // Measured kernel row shape; these identifiers are inert fixture values.
  host->files["/proc/43/mountinfo"] += "60 21 0:7 user:[700] /run/.userns ro,nosuid,nodev - nsfs nsfs rw\n";
  const auto proof = ep::testing::prove_flatpak_desktop(game, error, host);
  ASSERT_TRUE(proof) << error;
  EXPECT_TRUE(ep::verify_flatpak_desktop(*proof, error)) << error;
  EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, FlatpakNsfsRetainsStrictRootPointHandleEscapeAndNumberValidation) {
  const std::pair<const char *, const char *> rows[] {
    {"60 21 0:7 user:[700] /run/.userns ro - tmpfs tmpfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 relative /run/.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 unknown:[700] /run/.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[] /run/.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[0] /run/.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[0700] /run/.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[-1] /run/.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[18446744073709551616] /run/.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[700]suffix /run/.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[700] run/.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[700] /run/../.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[700] /run/./.userns ro - nsfs nsfs rw\n", "malformed_mount_path"},
    {"60 21 0:7 user:[\\999] /run/.userns ro - nsfs nsfs rw\n", "malformed_proc_path"},
    {"60 21 0:7 user:[700] /run/\\999 ro - nsfs nsfs rw\n", "malformed_proc_path"},
    {"60 21 0:x user:[700] /run/.userns ro - nsfs nsfs rw\n", "malformed_proc_number"},
    {"60 21 0:7 /files /app/../app ro - btrfs /dev/fixture ro\n", "malformed_mount_path"},
    {"60 21 0:7 /files/../files /app ro - btrfs /dev/fixture ro\n", "malformed_mount_path"},
    {"60 21 invalid user:[700] /run/.userns ro - nsfs nsfs rw\n", "malformed_mount_device"},
    {"60 21 0:7 user:[700] /run/.userns ro nsfs nsfs rw\n", "malformed_mountinfo"},
  };
  for (const auto &[row, expected] : rows) {
    host = std::make_shared<fixture_t>();
    const auto game = direct_games_desktop_fixture(*host);
    ASSERT_TRUE(ep::testing::prove_flatpak_desktop(game, error, host)) << error;
    host->files["/proc/43/mountinfo"] += row;
    EXPECT_FALSE(ep::testing::prove_flatpak_desktop(game, error, host)) << row;
    EXPECT_EQ(error, expected) << row;
    EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, FlatpakNsfsCanNeverSupplyAnImageContainingFilesystem) {
  for (const auto *point : {"/", "/app", "/app/bin/EpicGamesLauncher", "/.flatpak-info"}) {
    host = std::make_shared<fixture_t>();
    const auto game = direct_games_desktop_fixture(*host);
    ASSERT_TRUE(ep::testing::prove_flatpak_desktop(game, error, host)) << error;
    host->files["/proc/43/mountinfo"] += "60 21 0:7 user:[700] " + std::string(point) + " ro - nsfs nsfs rw\n";
    EXPECT_FALSE(ep::testing::prove_flatpak_desktop(game, error, host));
    EXPECT_EQ(error, "namespace_image_mount") << point;
    EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, FlatpakNsfsDoesNotBypassRequiredFilesystemMountProofs) {
  for (int failure = 0; failure < 4; ++failure) {
    host = std::make_shared<fixture_t>();
    const auto game = direct_games_desktop_fixture(*host);
    auto &mounts = host->files["/proc/43/mountinfo"];
    mounts += "60 21 0:7 user:[700] /run/.userns ro - nsfs nsfs rw\n";
    ASSERT_TRUE(ep::testing::prove_flatpak_desktop(game, error, host)) << error;
    switch (failure) {
      case 0: mounts += "61 21 0:52 /files /app ro - btrfs /dev/fixture ro\n"; break;
      case 1: mounts.replace(mounts.find("/app ro"), 7, "/app rw"); break;
      case 2: mounts.replace(mounts.find("/files /app"), 11, "relative /app"); break;
      case 3: mounts.replace(mounts.find("/.flatpak-info ro"), 17, "/.flatpak-info rw"); break;
    }
    EXPECT_FALSE(ep::testing::prove_flatpak_desktop(game, error, host));
    const char *expected[] {"ambiguous_image_mount", "epic_flatpak_mount_mismatch", "malformed_mount_path", "epic_flatpak_mount_mismatch"};
    EXPECT_EQ(error, expected[failure]) << failure;
    EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, NativeImageProofAlsoAcceptsUnrelatedNsfsWithoutAcquiringAuthority) {
  const auto before = ep::testing::observe(host->title(), host);
  ASSERT_TRUE(before.available) << before.reason;
  host->files["/proc/41/mountinfo"] += "60 21 0:7 user:[700] /run/.userns ro - nsfs nsfs rw\n";
  const auto after = ep::testing::observe(host->title(), host);
  EXPECT_TRUE(after.available) << after.reason;
  EXPECT_EQ(after.pid, before.pid);
  EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, FlatpakDesktopProofRefusesMissingDifferentAndAmbiguousInstances) {
  for (int failure = 0; failure < 9; ++failure) {
    host = std::make_shared<fixture_t>();
    const auto d = flatpak_desktop_fixture(*host);
    ASSERT_TRUE(ep::testing::prove_flatpak_desktop(d, error, host)) << error;
    switch (failure) {
      case 0: host->links.erase("/proc/43/exe"); break;
      case 1: host->files["/proc/43/root/.flatpak-info"] += "app-commit=duplicate\n"; break;
      case 2: host->identities["/proc/43/exe"].inode++; break;
      case 3: host->files["/proc/52/stat"] = stat_line(52, 220, 'S', 99); break;
      case 4: host->links["/proc/52/ns/pid"] = "pid:[777]"; break;
      case 5: host->files["/proc/43/cgroup"] = host->files["/proc/self/cgroup"]; break;
      case 6: host->resolved[d.directory.parent_path() / "active"] = d.directory.parent_path() / std::string(64, 'b'); break;
      case 7: host->files["/proc/43/mountinfo"] = "21 1 0:1 / / rw - tmpfs tmpfs rw\n"; break;
      case 8: host->add_process(44, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 240); break;
    }
    EXPECT_FALSE(ep::testing::prove_flatpak_desktop(d, error, host)) << failure;
    const char *expected[] {"epic_flatpak_desktop_required", "epic_flatpak_desktop_mismatch", "epic_flatpak_desktop_mismatch",
      "epic_flatpak_record_stale", "epic_flatpak_record_stale", "epic_flatpak_not_desktop", "epic_flatpak_deployment_changed",
      "epic_flatpak_mount_mismatch", "epic_flatpak_desktop_ambiguous"};
    EXPECT_EQ(error, expected[failure]) << failure;
    EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, FlatpakDesktopProofRejectsDeploymentProcessNamespaceAndRecordDriftBeforeDispatch) {
  for (int change = 0; change < 7; ++change) {
    host = std::make_shared<fixture_t>();
    const auto d = flatpak_desktop_fixture(*host);
    const auto proof = ep::testing::prove_flatpak_desktop(d, error, host);
    ASSERT_TRUE(proof) << error;
    switch (change) {
      case 0: host->files["/proc/43/stat"] = stat_line(43, 231, 'S', 52); break;
      case 1: host->identities[d.directory / "files/bin/EpicGamesLauncher"].modified_ns++; break;
      case 2: host->files[d.directory / "metadata"] += "# changed\n"; break;
      case 3: host->links["/proc/43/ns/mnt"] = "mnt:[666]"; break;
      case 4: host->files["/run/user/1000/.flatpak/123456/pid"] = "99\n"; break;
      case 5: host->uid++; break;
      case 6: host->files["/proc/sys/kernel/random/boot_id"] = "1dd26721-4424-4907-ab80-4f023729a22f\n"; break;
    }
    EXPECT_FALSE(ep::verify_flatpak_desktop(*proof, error)) << change;
    const char *expected[] {"epic_flatpak_desktop_changed", "epic_flatpak_deployment_changed", "epic_flatpak_deployment_changed",
      "epic_flatpak_desktop_changed", "incomplete_process_observation", "epic_flatpak_deployment_changed", "epic_flatpak_desktop_changed"};
    EXPECT_EQ(error, expected[change]) << change;
    EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
  }
}

TEST_F(EpicProcess, FlatpakGamesNeverEnterNativeObservationAdoptionOrPrivateReadiness) {
  auto title = host->title(); title.flatpak = flatpak_desktop_fixture(*host);
  host->reads.clear();
  const auto seen = ep::testing::observe(title, host);
  EXPECT_FALSE(seen.available); EXPECT_EQ(seen.reason, "epic_flatpak_game_control_unavailable");
  EXPECT_TRUE(host->reads.empty());
  // Even a current valid native game observation cannot substitute for sandbox ownership.
  const auto native = ep::testing::observe(host->title(), host); ASSERT_TRUE(native.available) << native.reason;
  EXPECT_FALSE(ep::testing::adopt(title, native, error, host));
  EXPECT_EQ(error, "epic_flatpak_game_control_unavailable");
  EXPECT_FALSE(ep::testing::title_runtime_absent(title, error, host));
  const auto facts = ep::testing::evaluate_setup(nullptr, &title, true, ep::prerequisite_state_t::available, host);
  EXPECT_EQ(facts.reason, "epic_flatpak_mirror_only");
  EXPECT_EQ(facts.proof, ep::proof_state_t::unavailable);
  EXPECT_EQ(facts.runtime, ep::runtime_state_t::unknown);
  EXPECT_NE(facts.state, ep::readiness_state_t::ready);
  EXPECT_TRUE(host->opened.empty()); EXPECT_TRUE(host->signals.empty());
}

TEST_F(EpicProcess, FlatpakMetadataCannotCreateNativeSetupReceiptEvenAlongsideANativeDesktopProof) {
  host->add_process(43, "/usr/bin/EpicGamesLauncher", "/fixture/owner", 10003);
  const auto proof = ep::testing::prove_launcher(error, host); ASSERT_TRUE(proof) << error;
  ep::setup_history_t history;
  auto scan = history.begin_scan(); history.observe_scan(scan, *proof);
  epic_library::installation_t game; game.flatpak = epic_library::flatpak_deployment_t {};
  const epic_library::snapshot_t metadata {true, {game}, {}};
  EXPECT_EQ(history.finish_scan(scan, metadata, &*proof, true), ep::scan_state_t::unsupported);
  EXPECT_EQ(history.receipt(metadata, true), ep::receipt_state_t::required);
  const epic_library::snapshot_t empty_flatpak {true, {}, {}, true};
  scan = history.begin_scan(); history.observe_scan(scan, *proof);
  EXPECT_EQ(history.finish_scan(scan, empty_flatpak, &*proof, true), ep::scan_state_t::unsupported);
  EXPECT_EQ(history.receipt(empty_flatpak, true), ep::receipt_state_t::required);
}

#endif
