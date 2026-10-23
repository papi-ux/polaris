/**
 * @file tests/unit/platform/test_epic_process.cpp
 * @brief Fixture-only Epic observation and retained pidfd authority regressions.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include <src/platform/linux/epic_process.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <utility>

namespace {
  namespace ep = platf::epic;
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  using ep::testing::exit_state_t;
  using ep::testing::file_t;

  const fs::path install = "/fixture/owner/Games/A Game";
  const fs::path data = "/fixture/owner/.local/share/com.epicgames.eglv2";
  const fs::path image = install / "Game.exe";
  const fs::path runtime = data / "compat/runtime/GE-Proton10-34";
  const fs::path prefix_path = data / "compat/compatdata/artifact/pfx";
  const std::string boot = "9dd26721-4424-4907-ab80-4f023729a22f\n";

  std::string stat_line(std::uint64_t pid, std::uint64_t start, char state = 'S') {
    std::ostringstream text;
    // The kernel comm can contain right parentheses, spaces, and left parentheses.
    text << pid << " (game ) with (parens)) " << state << " 1";
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

  struct fixture_t final: ep::testing::backend_t {
    std::map<fs::path, std::string> files, headers;
    std::map<fs::path, fs::path> links, resolved;
    std::map<fs::path, file_t> identities;
    std::map<fs::path, std::vector<std::string>> listings;
    std::function<void(const fs::path &)> after_read;
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

    void add_process(std::uint64_t pid, const fs::path &exe, const fs::path &cwd, std::uint64_t start) {
      const auto path = fs::path("/proc") / std::to_string(pid);
      pids.push_back(std::to_string(pid));
      identities[path] = directory(pid);
      if (!identities.count(exe)) {
        identities[exe] = regular(pid * 100);
      }
      identities[path / "exe"] = identities[exe];
      headers[exe] = std::string("\177ELF", 4);
      links[path / "exe"] = exe;
      links[path / "cwd"] = cwd;
      links[path / "ns/cgroup"] = "cgroup:[123]";
      links[path / "map_files/1000-2000"] = image;
      links[path / "map_files/4000-5000"] = image;
      files[path / "stat"] = stat_line(pid, start);
      files[path / "status"] = "Name:\tfixture\nUid:\t1000\t1000\t1000\t1000\n";
      files[path / "cgroup"] = "0::/user.slice/app.slice/epic.scope\n";
      files[path / "maps"] = "9000-a000 rw-p 00000000 00:00 0\n";
      files[path / "mountinfo"] = "21 1 0:1 / / rw - tmpfs tmpfs rw\n52 21 0:34 /@fixture /fixture rw - btrfs /dev/fixture rw\n";
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

TEST_F(EpicProcess, MissingUnreadableOrMalformedRowsCannotHideAnAmbiguousGame) {
  host->add_process(43, runtime / "files/bin/wine64-preloader", install, 10002);
  host->files.erase("/proc/43/maps");
  EXPECT_FALSE(observe().available);
  host->files["/proc/43/maps"] = host->maps();
  EXPECT_EQ(observe().reason, "ambiguous_game_process");
  host->files["/proc/43/stat"] = "43 (truncated) S 1\n";
  EXPECT_EQ(observe().reason, "incomplete_process_stat");
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
