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
      const std::vector<std::uint64_t> &socket_inodes = {}
    ) {
      const auto dir = proc / std::to_string(pid);
      fs::create_directories(dir / "fd");

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

  ASSERT_TRUE(gp::write_marker(marker_path, {.pid = 410, .start_time = 9001, .role = "idle"}));
  EXPECT_TRUE(gp::validated_marker(marker_path, "idle", paths_for(tree)).has_value());
  EXPECT_FALSE(gp::validated_marker(marker_path, "nested", paths_for(tree)).has_value());

  ASSERT_TRUE(gp::write_marker(marker_path, {.pid = 410, .start_time = 9000, .role = "idle"}));
  EXPECT_FALSE(gp::validated_marker(marker_path, "idle", paths_for(tree)).has_value());

  tree.add_process(410, 1, 9001, {"/usr/bin/unrelated-compositor", "--backend", "headless"});
  ASSERT_TRUE(gp::write_marker(marker_path, {.pid = 410, .start_time = 9001, .role = "idle"}));
  EXPECT_FALSE(gp::validated_marker(marker_path, "idle", paths_for(tree)).has_value());
}

TEST(GamescopeProcessOwnershipTests, CapturesGenerationFromProcAndReadsExactArguments) {
  fake_proc_tree_t tree;
  tree.add_process(410, 1, 9001, {"gamescope", "--backend=headless", "--hdr-enabled"});

  const auto marker = gp::marker_for_pid(410, "runtime", paths_for(tree));
  ASSERT_TRUE(marker.has_value());
  EXPECT_EQ(*marker, (gp::marker_t {.pid = 410, .start_time = 9001, .role = "runtime"}));
  EXPECT_TRUE(gp::process_has_argument(*marker, "--hdr-enabled", paths_for(tree)));
  EXPECT_FALSE(gp::process_has_argument(*marker, "--hdr-debug-force-output", paths_for(tree)));
}

TEST(GamescopeProcessOwnershipTests, SelectsOnlyXwaylandDescendedFromMarkedRuntimeAndPreservesHostXZero) {
  fake_proc_tree_t tree;
  const auto gamescope_socket = tree.runtime / "gamescope-0";
  const auto host_x0 = tree.x11 / "X0";
  const auto owned_x4 = tree.x11 / "X4";

  tree.add_unix_socket(500, gamescope_socket);
  tree.add_unix_socket(600, host_x0);
  tree.add_unix_socket(604, owned_x4);
  tree.flush_unix_sockets();

  tree.add_process(410, 1, 9001,
                   {"/usr/bin/gamescope", "--backend", "headless", "--xwayland-count", "2"}, {500});
  tree.add_process(411, 410, 9002, {"/usr/bin/Xwayland", ":4"}, {604});
  tree.add_process(99, 1, 100, {"/usr/bin/Xorg", ":0"}, {600});

  const gp::marker_t marker {.pid = 410, .start_time = 9001, .role = "idle"};
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

  tree.add_process(410, 1, 9001, {"gamescope", "--backend=headless"}, {500});
  tree.add_process(777, 1, 300, {"Xwayland.bin", ":2"}, {602});

  const gp::marker_t marker {.pid = 410, .start_time = 9001, .role = "runtime"};
  EXPECT_FALSE(gp::discover_owned_x11_display(marker, paths_for(tree)).has_value());
  EXPECT_TRUE(fs::exists(unrelated_x2));
}
#endif
