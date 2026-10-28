/**
 * @file tests/unit/platform/test_epic_process.cpp
 * @brief Fixture-only Epic observation and retained pidfd authority regressions.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include <src/platform/linux/epic_process.h>
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
#include <sys/stat.h>
#include <sys/sysmacros.h>
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

  std::string stat_line(std::uint64_t pid, std::uint64_t start, char state = 'S', std::uint64_t parent = 1) {
    std::ostringstream text;
    // The kernel comm can contain right parentheses, spaces, and left parentheses.
    text << pid << " (game ) with (parens)) " << state << ' ' << parent;
    for (int field = 2; field < 19; ++field) {
      text << " 0";
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

  file_t directory(std::uint64_t inode) {
    return {makedev(0, 52), inode, 1000, S_IFDIR | 0755, 4096, 111, 222};
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
    std::map<fs::path, std::vector<std::string>> listings;
    std::set<fs::path> denied_links;
    std::map<fs::path, int> status_errors, directory_errors, absence_probe_errors;
    std::set<fs::path> live_absence_probes;
    std::vector<fs::path> closed_absence_probes;
    std::function<void(const fs::path &)> after_read;
    std::function<void(const fs::path &)> after_missing_status;
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
      return value;
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
      const auto found = headers.find(path);
      if (found == headers.end() || found->second.size() < length) {
        return std::nullopt;
      }
      return found->second.substr(0, length);
    }

    std::optional<fs::path> link(const fs::path &path) override {
      const auto found = links.find(path);
      return found == links.end() ? std::nullopt : std::make_optional(found->second);
    }

    ep::testing::link_result_t inspect_link(const fs::path &path) override {
      return denied_links.count(path) ? ep::testing::link_result_t {std::nullopt, true} :
        ep::testing::link_result_t {link(path), false};
    }

    void add_protected_process(std::uint64_t pid = 43, std::uint64_t start = 10003) {
      add_process(pid, "/usr/lib/systemd/systemd", "/fixture/owner", start);
      const auto exe = fs::path("/proc") / std::to_string(pid) / "exe";
      links.erase(exe);
      denied_links.insert(exe);
    }

    std::optional<fs::path> canonical(const fs::path &path) override {
      if (resolved.count(path)) {
        return resolved.at(path);
      }
      return identities.count(path) ? std::make_optional(path) : std::nullopt;
    }

    std::optional<file_t> stat(const fs::path &path) override {
      if (identities.count(path)) {
        return identities.at(path);
      }
      const auto text = path.string();
      const auto root = text.find("/root/");
      if (text.compare(0, 6, "/proc/") == 0 && root != std::string::npos) {
        return stat(text.substr(root + 5));
      }
      return std::nullopt;
    }

    std::optional<std::vector<std::string>> list(const fs::path &path, std::size_t limit) override {
      if (path == "/proc") {
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
  host->add_protected_process(84);
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

TEST_F(EpicProcess, PrivateSelectionRequiresAMonotonicKnownAncestryAndFreshPrefixServer) {
  const auto lease = host->private_tree();
  host->private_game(81, 400, 83);
  host->private_server();
  const auto session = ep::testing::pin_private_session(lease, "private-generation", error, host);
  ASSERT_TRUE(session) << error;
  EXPECT_FALSE(ep::observe_in_session(host->title(), *session, 300).available);
  host->add_process(83, "/bin/sh", "/fixture/owner", 401, 101);
  EXPECT_EQ(ep::observe_in_session(host->title(), *session, 300).reason, "private_parent_younger_than_child");
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
    [&] { host->headers[host->links["/proc/44/exe"]] = std::string("\177ELF", 11); },
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

#endif
