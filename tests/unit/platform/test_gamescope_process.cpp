/**
 * @file tests/unit/platform/test_gamescope_process.cpp
 * @brief Behavioral tests for Polaris gamescope ownership and XWayland routing.
 */
#include "../../tests_common.h"

#if !defined(_WIN32)
  #include "src/platform/linux/gamescope_process.h"

  #include <chrono>
  #include <filesystem>
  #include <fstream>
  #include <string>
  #include <vector>

  #include <fcntl.h>
  #include <sys/file.h>
  #include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  namespace gp = stream_runtime::gamescope_process;

  class fake_proc_tree_t {
  public:
    fake_proc_tree_t() {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      root = fs::temp_directory_path() / ("polaris-gamescope-process-" + std::to_string(nonce));
      proc = root / "proc";
      runtime = root / "run";
      x11 = root / "tmp" / ".X11-unix";
      fs::create_directories(proc / "net");
      fs::create_directories(runtime);
      fs::create_directories(x11);
    }

    ~fake_proc_tree_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    void add_process(
      int pid,
      int ppid,
      std::uint64_t start_time,
      const std::vector<std::string> &argv,
      const std::vector<std::uint64_t> &socket_inodes = {},
      const fs::path &executable_override = {},
      int pgid = -1,
      int session = -1
    ) {
      const auto dir = proc / std::to_string(pid);
      fs::remove_all(dir);
      fs::create_directories(dir / "fd");
      const auto executable = !executable_override.empty() ? executable_override :
                              argv.empty() ? fs::path {"/usr/bin/process"} : fs::path {argv.front()};
      fs::create_symlink(executable, dir / "exe");

      // gamescope_process rejects pgid/session <= 1 (init / kthreadd). Default to a
      // setsid()-style identity so ownership validation matches real gamescope.
      if (pgid <= 1) {
        pgid = pid;
      }
      if (session <= 1) {
        session = pid;
      }

      std::ofstream stat(dir / "stat");
      stat << pid << " (" << (argv.empty() ? "process" : fs::path(argv.front()).filename().string())
           << ") S " << ppid << ' ' << pgid << ' ' << session;
      // Fields 7..21 are irrelevant here; starttime is field 22.
      for (int field = 7; field <= 21; ++field) {
        stat << " 0";
      }
      stat << ' ' << start_time << '\n';

      std::ofstream cmdline(dir / "cmdline", std::ios::binary);
      for (const auto &arg : argv) {
        cmdline.write(arg.data(), static_cast<std::streamsize>(arg.size()));
        cmdline.put('\0');
      }

      int fd = 3;
      for (const auto inode : socket_inodes) {
        fs::create_symlink("socket:[" + std::to_string(inode) + "]", dir / "fd" / std::to_string(fd++));
      }
    }

    void add_unix_socket(std::uint64_t inode, const fs::path &path) {
      std::ofstream(path).put('\n');
      unix_rows += "0000000000000000: 00000002 00000000 00010000 0001 01 " +
                   std::to_string(inode) + " " + path.string() + "\n";
    }

    void set_cgroup(int pid, const std::string &cgroup) const {
      std::ofstream(proc / std::to_string(pid) / "cgroup") << cgroup << '\n';
    }

    void flush_unix_sockets() const {
      std::ofstream(proc / "net" / "unix")
        << "Num RefCount Protocol Flags Type St Inode Path\n"
        << unix_rows;
    }

    void replace_unix_socket(std::uint64_t inode, const fs::path &path) {
      unix_rows.clear();
      add_unix_socket(inode, path);
      flush_unix_sockets();
    }

    fs::path root;
    fs::path proc;
    fs::path runtime;
    fs::path x11;
    std::string unix_rows;
  };

  gp::lookup_paths_t paths_for(const fake_proc_tree_t &tree) {
    return {
      .proc_root = tree.proc,
      .proc_net_unix = tree.proc / "net" / "unix",
      .x11_socket_dir = tree.x11,
    };
  }
}  // namespace

TEST(GamescopeProcessOwnershipTests, MarkerRequiresExactGenerationRoleAndHeadlessGamescopeIdentity) {
  fake_proc_tree_t tree;
  tree.add_process(410, 1, 9001, {"/usr/bin/gamescope", "--backend", "headless", "--", "sleep", "infinity"});
  const auto marker_path = tree.runtime / "polaris-gamescope.pid";

  ASSERT_TRUE(gp::write_marker(marker_path, {
    .pid = 410, .start_time = 9001, .role = "idle", .executable = "/usr/bin/gamescope"
  }));
  EXPECT_TRUE(gp::validated_marker(marker_path, "idle", paths_for(tree)).has_value());
  EXPECT_FALSE(gp::validated_marker(marker_path, "nested", paths_for(tree)).has_value());

  ASSERT_TRUE(gp::write_marker(marker_path, {
    .pid = 410, .start_time = 9000, .role = "idle", .executable = "/usr/bin/gamescope"
  }));
  EXPECT_FALSE(gp::validated_marker(marker_path, "idle", paths_for(tree)).has_value());

  tree.add_process(410, 1, 9001, {"/usr/bin/unrelated-compositor", "--backend", "headless"});
  ASSERT_TRUE(gp::write_marker(marker_path, {
    .pid = 410, .start_time = 9001, .role = "idle", .executable = "/usr/bin/gamescope"
  }));
  EXPECT_FALSE(gp::validated_marker(marker_path, "idle", paths_for(tree)).has_value());

  // argv[0] alone is forgeable; /proc/<pid>/exe must also resolve to gamescope.
  tree.add_process(410, 1, 9001, {"/usr/bin/gamescope", "--backend", "headless"});
  fs::remove(tree.proc / "410" / "exe");
  fs::create_symlink("/usr/bin/sleep", tree.proc / "410" / "exe");
  EXPECT_FALSE(gp::validated_marker(marker_path, "idle", paths_for(tree)).has_value());
}

TEST(GamescopeProcessOwnershipTests, AcceptsAndPinsNixWrappedGamescopeExecutable) {
  fake_proc_tree_t tree;
  const fs::path wrapper = "/nix/store/fake-gamescope/bin/gamescope";
  const fs::path wrapped = "/nix/store/fake-gamescope/bin/.gamescope-wrapped";
  tree.add_process(420, 1, 9200, {wrapper.string(), "--backend", "headless"}, {}, wrapped);

  const auto marker = gp::marker_for_pid(420, "idle", paths_for(tree));
  ASSERT_TRUE(marker.has_value());
  EXPECT_EQ(marker->executable, wrapped);

  const auto marker_path = tree.runtime / "polaris-gamescope.pid";
  ASSERT_TRUE(gp::write_marker(marker_path, *marker));
  EXPECT_TRUE(gp::validated_marker(marker_path, "idle", paths_for(tree)).has_value());

  fs::remove(tree.proc / "420" / "exe");
  fs::create_symlink("/nix/store/other-gamescope/bin/.gamescope-wrapped", tree.proc / "420" / "exe");
  EXPECT_FALSE(gp::validated_marker(marker_path, "idle", paths_for(tree)).has_value());
}

TEST(GamescopeProcessOwnershipTests, CapturesGenerationFromProcAndReadsExactArguments) {
  fake_proc_tree_t tree;
  tree.add_process(410, 1, 9001, {"/usr/bin/gamescope", "--backend=headless", "--hdr-enabled"});

  const auto marker = gp::marker_for_pid(410, "runtime", paths_for(tree));
  ASSERT_TRUE(marker.has_value());
  EXPECT_EQ(*marker, (gp::marker_t {
    .pid = 410,
    .start_time = 9001,
    .role = "runtime",
    .executable = "/usr/bin/gamescope",
  }));
  EXPECT_TRUE(gp::process_has_argument(*marker, "--hdr-enabled", paths_for(tree)));
  EXPECT_FALSE(gp::process_has_argument(*marker, "--hdr-debug-force-output", paths_for(tree)));
}

TEST(GamescopeProcessOwnershipTests, SelectsOnlyXwaylandDescendedFromMarkedRuntimeAndPreservesHostXZero) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  const auto host_x0 = tree.x11 / "X0";
  const auto stale_x2 = tree.x11 / "X2";
  const auto spoofed_x3 = tree.x11 / "X3";
  const auto owned_x4 = tree.x11 / "X4";

  tree.add_unix_socket(500, gamescope_socket);
  tree.add_unix_socket(600, host_x0);
  tree.add_unix_socket(602, stale_x2);
  tree.add_unix_socket(603, spoofed_x3);
  tree.add_unix_socket(604, owned_x4);
  tree.flush_unix_sockets();

  tree.add_process(410, 1, 9001,
                   {"/usr/bin/gamescope", "--backend", "headless", "--xwayland-count", "2"}, {500});
  tree.add_process(411, 410, 9002, {"/usr/bin/Xwayland", ":4"}, {604});
  tree.add_process(413, 1, 8000, {"/usr/bin/Xwayland", ":2"}, {602});
  tree.set_cgroup(410, "0::/user.slice/polaris.service");
  tree.set_cgroup(413, "0::/user.slice/polaris.service");
  tree.add_process(412, 410, 9003, {"/usr/bin/Xwayland", ":3"}, {603});
  fs::remove(tree.proc / "412" / "exe");
  fs::create_symlink("/usr/bin/sleep", tree.proc / "412" / "exe");
  tree.add_process(99, 1, 100, {"/usr/bin/Xorg", ":0"}, {600});

  const gp::marker_t marker {
    .pid = 410, .start_time = 9001, .role = "idle", .executable = "/usr/bin/gamescope"
  };
  EXPECT_TRUE(gp::process_tree_owns_socket(marker, gamescope_socket, paths_for(tree)));
  const auto original_unix_rows = tree.unix_rows;
  auto rebound_wayland_paths = paths_for(tree);
  rebound_wayland_paths.before_socket_ownership_return_for_tests = [&]() {
    tree.replace_unix_socket(501, gamescope_socket);
  };
  EXPECT_FALSE(gp::process_tree_owns_socket(marker, gamescope_socket, rebound_wayland_paths));
  tree.unix_rows = original_unix_rows;
  tree.flush_unix_sockets();
  EXPECT_EQ(gp::discover_owned_x11_display(marker, paths_for(tree)), std::optional<std::string>(":4"));
  auto rebound_paths = paths_for(tree);
  rebound_paths.before_x11_return_for_tests = [&]() { tree.replace_unix_socket(605, owned_x4); };
  EXPECT_FALSE(gp::discover_owned_x11_display(marker, rebound_paths));
  EXPECT_TRUE(fs::exists(host_x0));
}

TEST(GamescopeProcessOwnershipTests, FailsClosedWhenOnlyUnrelatedXwaylandExists) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  const auto unrelated_x2 = tree.x11 / "X2";

  tree.add_unix_socket(500, gamescope_socket);
  tree.add_unix_socket(602, unrelated_x2);
  tree.flush_unix_sockets();

  tree.add_process(410, 1, 9001, {"/usr/bin/gamescope", "--backend=headless"}, {500});
  tree.add_process(777, 1, 300, {"Xwayland.bin", ":2"}, {602});

  const gp::marker_t marker {
    .pid = 410, .start_time = 9001, .role = "runtime", .executable = "/usr/bin/gamescope"
  };
  EXPECT_FALSE(gp::discover_owned_x11_display(marker, paths_for(tree)).has_value());
  EXPECT_TRUE(fs::exists(unrelated_x2));
}
TEST(GamescopeProcessOwnershipTests, FailsClosedOnDuplicateSocketPathRows) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  std::ofstream(gamescope_socket.string() + ".lock").put('\n');
  const auto x4 = tree.x11 / "X4";

  // /proc/net/unix may retain an unlinked old listener while a successor has
  // rebound the same pathname. Neither row is authoritative by pathname.
  tree.add_unix_socket(500, gamescope_socket);
  tree.add_unix_socket(501, gamescope_socket);
  tree.add_unix_socket(604, x4);
  tree.add_unix_socket(605, x4);
  tree.flush_unix_sockets();
  tree.add_process(410, 1, 9001, {"/usr/bin/gamescope", "--backend=headless"}, {500});
  tree.add_process(411, 410, 9002, {"Xwayland", ":4"}, {604});

  const gp::marker_t marker {
    .pid = 410, .start_time = 9001, .role = "runtime", .executable = "/usr/bin/gamescope"
  };
  // Pathname is ambiguous → cannot claim exclusive ownership of gamescope-0.
  EXPECT_FALSE(gp::process_tree_owns_socket(marker, gamescope_socket, paths_for(tree)));
  // Duplicate X11 pathname rows are ambiguous after unlink/rebind; routing must
  // fail closed even when one stale inode is held by a related Xwayland.
  EXPECT_EQ(gp::discover_owned_x11_display(marker, paths_for(tree)), std::nullopt);
  // Ambiguous pathnames are not safe to reclaim.
  EXPECT_TRUE(gp::socket_has_live_holder(gamescope_socket, paths_for(tree)));
  EXPECT_FALSE(gp::remove_orphan_socket(gamescope_socket, paths_for(tree)));
  EXPECT_TRUE(fs::exists(gamescope_socket));
}

TEST(GamescopeProcessOwnershipTests, ReclaimsFilesystemResidueWithoutListener) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  const fs::path lock_path {gamescope_socket.string() + ".lock"};
  std::ofstream(gamescope_socket).put('\n');
  std::ofstream(lock_path).put('\n');
  tree.flush_unix_sockets();  // header only — no listener row

  EXPECT_FALSE(gp::socket_has_live_holder(gamescope_socket, paths_for(tree)));
  EXPECT_TRUE(gp::remove_orphan_socket(gamescope_socket, paths_for(tree)));
  EXPECT_FALSE(fs::exists(gamescope_socket));
  EXPECT_TRUE(fs::exists(lock_path));
}

TEST(GamescopeProcessOwnershipTests, RefusesReclaimWhenWaylandLockIsMissing) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  const fs::path lock_path {gamescope_socket.string() + ".lock"};
  std::ofstream(gamescope_socket).put('\n');
  tree.flush_unix_sockets();

  EXPECT_FALSE(gp::remove_orphan_socket(gamescope_socket, paths_for(tree)));
  EXPECT_TRUE(fs::exists(gamescope_socket));
  EXPECT_FALSE(fs::exists(lock_path));
}

TEST(GamescopeProcessOwnershipTests, RefusesReplacementLockWhileDeletedLockIsHeld) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  const fs::path lock_path {gamescope_socket.string() + ".lock"};
  std::ofstream(gamescope_socket).put('\n');
  std::ofstream(lock_path).put('\n');
  tree.flush_unix_sockets();
  const auto fd_dir = tree.proc / "990" / "fd";
  fs::create_directories(fd_dir);
  fs::create_symlink(lock_path.string() + " (deleted)", fd_dir / "8");

  EXPECT_FALSE(gp::remove_orphan_socket(gamescope_socket, paths_for(tree)));
  EXPECT_TRUE(fs::exists(gamescope_socket));
  EXPECT_TRUE(fs::exists(lock_path));
}

TEST(GamescopeProcessOwnershipTests, RefusesLockReplacementBetweenValidationAndUnlink) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  const fs::path lock_path {gamescope_socket.string() + ".lock"};
  std::ofstream(gamescope_socket).put('\n');
  std::ofstream(lock_path).put('\n');
  tree.flush_unix_sockets();
  auto paths = paths_for(tree);
  paths.before_socket_unlink_for_tests = [&]() {
    fs::remove(lock_path);
    std::ofstream(lock_path).put('\n');
  };

  EXPECT_FALSE(gp::remove_orphan_socket(gamescope_socket, paths));
  EXPECT_TRUE(fs::exists(gamescope_socket));
  EXPECT_TRUE(fs::exists(lock_path));
}

TEST(GamescopeProcessOwnershipTests, RefusesSocketReplacementBeforeUnlink) {
  fake_proc_tree_t tree;
  const auto socket = tree.runtime / "gamescope-0";
  std::ofstream(socket).put('\n');
  std::ofstream(socket.string() + ".lock").put('\n');
  tree.flush_unix_sockets();
  auto paths = paths_for(tree);
  paths.before_socket_unlink_for_tests = [&]() {
    fs::remove(socket);
    std::ofstream(socket).put('x');
  };
  EXPECT_FALSE(gp::remove_orphan_socket(socket, paths));
  EXPECT_TRUE(fs::exists(socket));
}

TEST(GamescopeProcessOwnershipTests, MissingSocketDoesNotUnlinkPotentiallyHeldLock) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  const fs::path lock_path {gamescope_socket.string() + ".lock"};
  std::ofstream(lock_path).put('\n');

  EXPECT_TRUE(gp::remove_orphan_socket(gamescope_socket, paths_for(tree)));
  EXPECT_TRUE(fs::exists(lock_path));
}

TEST(GamescopeProcessOwnershipTests, HeldWaylandSocketLockBlocksReclaim) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  const fs::path lock_path {gamescope_socket.string() + ".lock"};
  std::ofstream(gamescope_socket).put('\n');
  tree.flush_unix_sockets();

  const int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  ASSERT_GE(lock_fd, 0);
  ASSERT_EQ(flock(lock_fd, LOCK_EX | LOCK_NB), 0);

  EXPECT_FALSE(gp::remove_orphan_socket(gamescope_socket, paths_for(tree)));
  EXPECT_TRUE(fs::exists(gamescope_socket));

  EXPECT_EQ(flock(lock_fd, LOCK_UN), 0);
  EXPECT_EQ(close(lock_fd), 0);
}

TEST(GamescopeProcessOwnershipTests, RefusesKernelSocketRowWithoutVisibleHolder) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  std::ofstream(gamescope_socket.string() + ".lock").put('\n');
  tree.add_unix_socket(700, gamescope_socket);
  tree.flush_unix_sockets();
  // Procfs permissions can hide the holder. A kernel row is still unsafe.

  EXPECT_TRUE(gp::socket_has_live_holder(gamescope_socket, paths_for(tree)));
  EXPECT_FALSE(gp::remove_orphan_socket(gamescope_socket, paths_for(tree)));
  EXPECT_TRUE(fs::exists(gamescope_socket));
}

TEST(GamescopeProcessOwnershipTests, RefusesReclaimWhenProcEnumerationIsUnavailable) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  std::ofstream(gamescope_socket).put('\n');
  std::ofstream(gamescope_socket.string() + ".lock").put('\n');
  tree.flush_unix_sockets();
  auto paths = paths_for(tree);
  paths.proc_root = tree.root / "missing-proc";

  EXPECT_FALSE(gp::remove_orphan_socket(gamescope_socket, paths));
  EXPECT_TRUE(fs::exists(gamescope_socket));
}

TEST(GamescopeProcessOwnershipTests, RefusesReclaimWhenProcSocketMetadataIsUnavailable) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  std::ofstream(gamescope_socket).put('\n');
  std::ofstream(gamescope_socket.string() + ".lock").put('\n');
  auto paths = paths_for(tree);
  paths.proc_net_unix = tree.proc / "net" / "missing-unix";

  EXPECT_TRUE(gp::socket_has_live_holder(gamescope_socket, paths));
  EXPECT_FALSE(gp::remove_orphan_socket(gamescope_socket, paths));
  EXPECT_TRUE(fs::exists(gamescope_socket));
}

TEST(GamescopeProcessOwnershipTests, RefusesLiveUnownedSocketReclaim) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  std::ofstream(gamescope_socket.string() + ".lock").put('\n');
  tree.add_unix_socket(701, gamescope_socket);
  tree.flush_unix_sockets();
  tree.add_process(440, 1, 9400, {"/usr/bin/gamescope", "--backend=headless"}, {701});

  EXPECT_TRUE(gp::socket_has_live_holder(gamescope_socket, paths_for(tree)));
  EXPECT_FALSE(gp::remove_orphan_socket(gamescope_socket, paths_for(tree)));
  EXPECT_TRUE(fs::exists(gamescope_socket));
}
#endif
