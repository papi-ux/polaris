/**
 * @file tests/unit/platform/test_epic_appimage_stage.cpp
 * @brief Run the Epic AppImage stage through the real startup wrapping, with no FUSE or Epic behind it.
 *
 * A fake image stands in for the runtime's --appimage-mount: it prints a directory and pauses. A fake
 * bwrap, named through the isolation plan as the real one is, records its arguments and runs what
 * follows its --. Everything else is the shell the session really starts.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include "src/epic_library.h"
  #include "src/platform/linux/input/inputtino_gamepad_isolation.h"
  #include "src/platform/linux/labwc_startup_diagnostics.h"

  #include <chrono>
  #include <csignal>
  #include <fcntl.h>
  #include <filesystem>
  #include <fstream>
  #include <functional>
  #include <sstream>
  #include <string>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>

namespace {
  namespace fs = std::filesystem;
  namespace isolation = platf::gamepad::isolation;
  using namespace std::chrono_literals;

  const epic_library::identity_t title {"sandbox", "catalog", "artifact"};

  std::string read_all(const fs::path &path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
  }

  void write_script(const fs::path &path, const std::string &body) {
    std::ofstream(path) << "#!/bin/sh\n" << body;
    fs::permissions(path, fs::perms::owner_all);
  }

  bool wait_until(const std::function<bool()> &done, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(10ms);
    }
    return true;
  }

  // Not a zombie either: what the stage leaves is reaped by whatever PID 1 this host or container has.
  bool process_alive(pid_t pid) {
    const auto stat = read_all(fs::path("/proc") / std::to_string(pid) / "stat");
    const auto name_end = stat.rfind(") ");
    return name_end != std::string::npos && name_end + 2 < stat.size() && stat[name_end + 2] != 'Z' && stat[name_end + 2] != 'X';
  }

  // Single-quotes a path for the fixture's own scripts.
  std::string q(const fs::path &path) {
    std::string quoted = "'";
    for (const char c : path.string()) {
      quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return quoted + "'";
  }

  struct stage_fixture_t {
    fs::path root, install, mount, image, bwrap, trace;

    explicit stage_fixture_t(const std::string &image_name = "fake.appimage") {
      static int serial = 0;
      root = fs::temp_directory_path() / ("polaris-epic-stage-" + std::to_string(getpid()) + "-" + std::to_string(++serial));
      fs::remove_all(root);
      install = root / "A Game's dir";
      mount = root / "mnt";
      trace = root / "trace";
      fs::create_directories(install);
      fs::create_directories(mount);
      fs::create_directories(trace);
      image = root / image_name;
      bwrap = root / "fake-bwrap";
      write_script(bwrap, "printf '%s\\n' \"$*\" > " + q(trace / "bwrap") + "\n"
                          "printf '%s' \"$APPDIR\" > " + q(trace / "bwrap-appdir") + "\n"
                          "while [ \"$1\" != -- ]; do shift; done\nshift\nexec \"$@\"\n");
      write_script(mount / "AppRun", "printf 'APPDIR=%s\\nAPPIMAGE=%s\\nARGV0=%s\\nOWD=%s\\nPWD=%s\\nARGC=%s\\nARG1=%s\\n' "
                                     "\"$APPDIR\" \"$APPIMAGE\" \"$ARGV0\" \"$OWD\" \"$PWD\" \"$#\" \"$1\" > " + q(trace / "apprun") + "\n"
                                     "exit 7\n");
    }

    ~stage_fixture_t() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }

    // The runtime: it records its pid and arguments, prints where it mounted and pauses.
    void image_mounts(const fs::path &where) {
      write_script(image, "printf '%s' \"$$\" > " + q(trace / "holder") + "\n"
                          "printf '%s\\n' \"$*\" > " + q(trace / "image-args") + "\n"
                          "printf '%s\\n' " + q(where) + "\nexec sleep 60\n");
    }

    pid_t holder() const {
      const auto text = read_all(trace / "holder");
      return text.empty() ? -1 : static_cast<pid_t>(std::stol(text));
    }

    std::string stage() const {
      isolation::device_snapshot_t pad;
      pad.event_node = "/dev/input/event7";
      pad.name = "Sony DualSense Wireless Controller";
      pad.vendor_id = 0x054c;
      pad.product_id = 0x0ce6;
      pad.gamepad_class = true;
      isolation::strict_isolation_options_t options;
      options.bubblewrap_available = true;
      options.bubblewrap_path = bwrap.string();
      const auto plan = isolation::build_strict_gamepad_isolation_plan(isolation::classify_devices({pad}), {}, options);
      EXPECT_TRUE(plan.strict_applied()) << plan.reason;
      return epic_library::appimage_stage_command(image, install, title, [&plan](const std::string &inner) {
        return isolation::command_with_headless_gamepad_isolation(inner, plan);
      });
    }

    /// Starts @p stage as labwc does, through the same two wrappers, and returns its pid.
    pid_t start(const std::string &stage_text) {
      const auto command = labwc_startup_diagnostics::make_labwc_startup_command(
        labwc_startup_diagnostics::instrument_shell_command(stage_text, 3, 4));
      const auto child = fork();
      if (child == 0) {
        const auto err = ::open((trace / "stderr").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const auto status = ::open((trace / "status").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(err, 3);
        dup2(status, 4);
        // Its own group, so the test can end what it leaves behind.
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
        _exit(126);
      }
      return child;
    }

    int finish(pid_t child, std::chrono::milliseconds limit = 8s) {
      int status = -1;
      const auto done = wait_until([&] { return waitpid(child, &status, WNOHANG) == child; }, limit);
      if (!done) {
        ::kill(-child, SIGKILL);
        waitpid(child, &status, 0);
        return -1;
      }
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
  };
}  // namespace

TEST(EpicAppImageStageTests, AppRunRunsInsideTheWrapperWithTheRuntimesEnvironmentAndTheRequest) {
  // A space and a single quote pass end to end, through both real startup wrappers.
  stage_fixture_t fixture("My Desk's Epic.appimage");
  fixture.image_mounts(fixture.mount);
  const auto child = fixture.start(fixture.stage());
  ASSERT_TRUE(wait_until([&] { return !read_all(fixture.trace / "apprun").empty(); })) << read_all(fixture.trace / "stderr");
  const auto seen = read_all(fixture.trace / "apprun");
  const auto mount = fixture.mount.string();
  const auto image = fixture.image.string();
  EXPECT_NE(seen.find("APPDIR=" + mount + "\n"), std::string::npos) << seen;
  EXPECT_NE(seen.find("APPIMAGE=" + image + "\n"), std::string::npos) << seen;
  EXPECT_NE(seen.find("ARGV0=" + image + "\n"), std::string::npos) << seen;
  EXPECT_NE(seen.find("OWD=" + fixture.install.string() + "\n"), std::string::npos) << seen;
  EXPECT_NE(seen.find("PWD=" + fixture.install.string() + "\n"), std::string::npos) << seen;
  EXPECT_NE(seen.find("ARGC=1\nARG1=" + epic_library::launch_uri(title) + "\n"), std::string::npos) << seen;
  // The mount step got its flag and never the request; the wrapper ran with the mount already known.
  EXPECT_EQ(read_all(fixture.trace / "image-args"), "--appimage-mount\n");
  EXPECT_EQ(read_all(fixture.trace / "bwrap-appdir"), mount);
  EXPECT_NE(read_all(fixture.trace / "bwrap").find("--tmpfs /dev/input"), std::string::npos);

  // The launcher returning leaves the mount up, since its children can still run from it.
  EXPECT_TRUE(wait_until([&] { return read_all(fixture.trace / "stderr").find("returned status=7") != std::string::npos; }));
  const auto holder = fixture.holder();
  ASSERT_GT(holder, 0);
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(process_alive(holder));
  int status = 0;
  EXPECT_EQ(waitpid(child, &status, WNOHANG), 0) << "the stage waits for the mount";
  // End reaches the holder, and the stage reports the launcher's status.
  (void) ::kill(holder, SIGTERM);
  EXPECT_EQ(fixture.finish(child), 7);
  EXPECT_EQ(read_all(fixture.trace / "status"), "7\n");
}

TEST(EpicAppImageStageTests, AnImageThatPrintsNothingExits127WithoutRunningAppRun) {
  stage_fixture_t fixture;
  write_script(fixture.image, "exit 3\n");
  const auto child = fixture.start(fixture.stage());
  EXPECT_EQ(fixture.finish(child), 127);
  EXPECT_NE(read_all(fixture.trace / "stderr").find("polaris: the Epic AppImage did not mount"), std::string::npos);
  EXPECT_FALSE(fs::exists(fixture.trace / "apprun"));
  EXPECT_FALSE(fs::exists(fixture.trace / "bwrap"));
  EXPECT_EQ(read_all(fixture.trace / "status"), "127\n");
}

TEST(EpicAppImageStageTests, AMountWithNoAppRunEndsTheHolderAndExits127) {
  stage_fixture_t fixture;
  fixture.image_mounts(fixture.root / "not-a-mount");
  const auto child = fixture.start(fixture.stage());
  EXPECT_EQ(fixture.finish(child), 127);
  EXPECT_FALSE(fs::exists(fixture.trace / "bwrap"));
  EXPECT_TRUE(wait_until([&] { return !process_alive(fixture.holder()); }));
}

TEST(EpicAppImageStageTests, AHungImageTimesOutAndIsEnded) {
  stage_fixture_t fixture;
  write_script(fixture.image, "printf '%s' \"$$\" > " + q(fixture.trace / "holder") + "\nexec sleep 60\n");
  // The stage's bound, shortened here alone.
  auto stage = fixture.stage();
  const std::string bound = "read -r -t 10 ";
  ASSERT_NE(stage.find(bound), std::string::npos);
  stage.replace(stage.find(bound), bound.size(), "read -r -t 1 ");
  const auto started = std::chrono::steady_clock::now();
  const auto child = fixture.start(stage);
  EXPECT_EQ(fixture.finish(child), 127);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
  EXPECT_FALSE(fs::exists(fixture.trace / "bwrap"));
  EXPECT_TRUE(wait_until([&] { return !process_alive(fixture.holder()); }));
}

TEST(EpicAppImageStageTests, TheSandboxItselfStaysTheOneSteamUses) {
  // The stage only wraps. The isolation command never grows FUSE access or a different root, which is
  // what would let the mount happen inside at the cost of what the sandbox hides.
  stage_fixture_t fixture;
  const auto stage = fixture.stage();
  for (const auto *widened : {"/dev/fuse", "--cap-add", "--uid", "--gid", "--unshare-pid", "FUSERMOUNT"}) {
    EXPECT_EQ(stage.find(widened), std::string::npos) << widened;
  }
  EXPECT_NE(stage.find(" --bind / / --dev /dev --proc /proc --ro-bind /sys /sys --tmpfs /dev/input "), std::string::npos);
}
#endif
