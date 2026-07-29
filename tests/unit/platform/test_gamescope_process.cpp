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
      const fs::path &executable_override = {}
    ) {
      const auto dir = proc / std::to_string(pid);
      fs::remove_all(dir);
      fs::create_directories(dir / "fd");
      const auto executable = !executable_override.empty() ? executable_override :
                              argv.empty() ? fs::path {"/usr/bin/process"} : fs::path {argv.front()};
      fs::create_symlink(executable, dir / "exe");

      std::ofstream stat(dir / "stat");
      stat << pid << " (" << (argv.empty() ? "process" : fs::path(argv.front()).filename().string())
           << ") S " << ppid;
      // Fields 5..21 are irrelevant here; starttime is field 22.
      for (int field = 5; field <= 21; ++field) {
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

    void flush_unix_sockets() const {
      std::ofstream(proc / "net" / "unix")
        << "Num RefCount Protocol Flags Type St Inode Path\n"
        << unix_rows;
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
  const auto spoofed_x3 = tree.x11 / "X3";
  const auto owned_x4 = tree.x11 / "X4";

  tree.add_unix_socket(500, gamescope_socket);
  tree.add_unix_socket(600, host_x0);
  tree.add_unix_socket(603, spoofed_x3);
  tree.add_unix_socket(604, owned_x4);
  tree.flush_unix_sockets();

  tree.add_process(410, 1, 9001,
                   {"/usr/bin/gamescope", "--backend", "headless", "--xwayland-count", "2"}, {500});
  tree.add_process(411, 410, 9002, {"/usr/bin/Xwayland", ":4"}, {604});
  tree.add_process(412, 410, 9003, {"/usr/bin/Xwayland", ":3"}, {603});
  fs::remove(tree.proc / "412" / "exe");
  fs::create_symlink("/usr/bin/sleep", tree.proc / "412" / "exe");
  tree.add_process(99, 1, 100, {"/usr/bin/Xorg", ":0"}, {600});

  const gp::marker_t marker {
    .pid = 410, .start_time = 9001, .role = "idle", .executable = "/usr/bin/gamescope"
  };
  EXPECT_TRUE(gp::process_tree_owns_socket(marker, gamescope_socket, paths_for(tree)));
  EXPECT_EQ(gp::discover_owned_x11_display(marker, paths_for(tree)), std::optional<std::string>(":4"));
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
  EXPECT_FALSE(gp::process_tree_owns_socket(marker, gamescope_socket, paths_for(tree)));
  EXPECT_FALSE(gp::discover_owned_x11_display(marker, paths_for(tree)).has_value());
  // Ambiguous pathnames are not safe to reclaim.
  EXPECT_TRUE(gp::socket_has_live_holder(gamescope_socket, paths_for(tree)));
  EXPECT_FALSE(gp::remove_orphan_socket(gamecope_socket, paths_for(tree)));
  EXPECT_TRUE(fs::exists(gamescope_socket));
}

TEST(GamescopeProcessOwnershipTests, ReclaimsFilesystemResidueWithoutListener) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  std::ofstream(gamescope_socket).put('\n');
  tree.flush_unix_sockets();  // header only — no listener row

  EXPECT_FALSE(gp::socket_has_live_holder(gamecope_socket, paths_for(tree)));
  EXPECT_TRUE(gp::remove_orphan_socket(gamecope_socket, paths_for(tree)));
  EXPECT_FALSE(fs::exists(gamecope_socket));
}

TEST(GamescopeProcessOwnershipTests, ReclaimsDeadListenerWithoutHolder) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  tree.add_unix_socket(700, gamescope_socket);
  tree.flush_unix_sockets();
  // No process holds inode 700.

  EXPECT_FALSE(gp::socket_has_live_holder(gamecope_socket, paths_for(tree)));
  EXPECT_TRUE(gp::remove_orphan_socket(gamecope_socket, paths_for(tree)));
  EXPECT_FALSE(fs::exists(gamecope_socket));
}

TEST(GamescopeProcessOwnershipTests, RefusesLiveUnownedSocketReclaim) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  tree.add_unix_socket(701, gamescope_socket);
  tree.flush_unix_sockets();
  tree.add_process(440, 1, 9400, {"/usr/bin/gamescope", "--backend=headless"}, {701});

  EXPECT_TRUE(gp::socket_has_live_holder(gamecope_socket, paths_for(tree)));
  EXPECT_FALSE(gp::remove_orphan_socket(gamecope_socket, paths_for(tree)));
  EXPECT_TRUE(fs::exists(gamecope_socket));
}
#endif
