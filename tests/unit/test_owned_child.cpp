/**
 * @file tests/unit/test_owned_child.cpp
 * @brief The #204 owned child: exact argv and environment, no inherited descriptors, kill and reap.
 *
 * The children are /bin/sh fixture scripts in a scratch directory; none of them is the real
 * steamosctl, and nothing here changes the machine.
 */
#include <src/owned_child.h>

#include <gtest/gtest.h>

#ifndef _WIN32
  #include <algorithm>
  #include <chrono>
  #include <csignal>
  #include <fcntl.h>
  #include <filesystem>
  #include <fstream>
  #include <iostream>
  #include <random>
  #include <set>
  #include <sstream>
  #include <string>
  #include <sys/resource.h>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
  #include <vector>

using namespace std::literals;
using owned_child::child_t;
using owned_child::state_e;

namespace {
  struct scratch_t {
    std::filesystem::path root;

    scratch_t() {
      std::random_device device;
      root = std::filesystem::temp_directory_path() / ("polaris-child-" + std::to_string(device()));
      std::filesystem::create_directories(root);
    }

    ~scratch_t() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }

    std::string script(const std::string &name, const std::string &body) const {
      const auto path = root / name;
      std::ofstream {path} << "#!/bin/sh\n" << body;
      ::chmod(path.c_str(), 0755);
      return path.string();
    }

    std::string read(const std::string &name) const {
      std::ifstream input {root / name};
      std::stringstream text;
      text << input.rdbuf();
      return text.str();
    }
  };

  /// Polls until the child leaves `running`, or the timeout.
  state_e wait_done(child_t &child, std::chrono::milliseconds timeout = 5s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (child.poll() == state_e::running && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(5ms);
    return child.state();
  }

  bool process_gone(pid_t pid, std::chrono::milliseconds timeout = 3s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until) {
      if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

  bool grandchild_terminated(pid_t pid, std::chrono::milliseconds timeout = 3s) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until) {
      if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
#ifdef __linux__
      // The test owns/reaps its direct child, but cannot waitpid a grandchild
      // adopted by container PID1. A retained zombie is exited, not running.
      std::ifstream stat {"/proc/" + std::to_string(pid) + "/stat"};
      std::string record;
      std::getline(stat, record);
      const auto end = record.rfind(')');
      if (end != std::string::npos && record.size() > end + 2 &&
          (record[end + 2] == 'Z' || record[end + 2] == 'X')) {
        std::cout << "The killed grandchild is retained as an exited /proc state " << record[end + 2] << '\n';
        return true;
      }
#endif
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
}  // namespace

TEST(OwnedChildTests, TheChildGetsExactlyItsArgvAndEnvironmentAndNoInheritedDescriptor) {
  scratch_t scratch;
  const auto out = (scratch.root / "seen").string();
  // A descriptor the parent leaves open without close-on-exec: the child must not see it.
  const int leaked = ::open(out.c_str(), O_WRONLY | O_CREAT, 0600);
  ASSERT_GE(leaked, 3);
  const auto command = scratch.script("record", R"(
{
  printf 'arg:%s\n' "$0" "$@"
  /usr/bin/env | /usr/bin/sort | sed 's/^/env:/'
  for fd in 0 1 2 )" + std::to_string(leaked) + R"(; do
    if { true >&$fd; } 2>/dev/null; then echo "fd:$fd"; fi
  done
} > "$OUT"
)");
  child_t child;
  ASSERT_TRUE(child.start({command, "switch-to-desktop-mode"}, {"OUT=" + out, "PATH=/usr/bin:/bin", "LANG=C"}));
  ASSERT_EQ(wait_done(child), state_e::exited);
  ::close(leaked);
  EXPECT_EQ(child.exit_code(), 0);
  std::istringstream lines {scratch.read("seen")};
  std::vector<std::string> args;
  std::vector<std::string> env;
  std::set<std::string> fds;
  for (std::string line; std::getline(lines, line);) {
    if (line.starts_with("arg:")) args.push_back(line.substr(4));
    if (line.starts_with("env:")) env.push_back(line.substr(4));
    if (line.starts_with("fd:")) fds.insert(line.substr(3));
  }
  EXPECT_EQ(args, (std::vector<std::string> {command, "switch-to-desktop-mode"}));
  // sh may add PWD/SHLVL/_ of its own; nothing of the parent's environment may arrive.
  for (const auto &entry : env) {
    const auto key = entry.substr(0, entry.find('='));
    EXPECT_TRUE(key == "OUT" || key == "PATH" || key == "LANG" || key == "PWD" || key == "SHLVL" || key == "_" || key == "OLDPWD")
      << "unexpected " << entry;
  }
  EXPECT_NE(std::find(env.begin(), env.end(), "LANG=C"), env.end());
  EXPECT_EQ(std::count_if(env.begin(), env.end(), [](const auto &e) { return e.starts_with("DBUS_") || e.starts_with("HOME=") || e.starts_with("DISPLAY="); }), 0);
  EXPECT_EQ(fds, (std::set<std::string> {"0", "1", "2"})) << "stdio on /dev/null only";
}

namespace {
  /// The descriptors the child's own `ls` sees, minus the one `ls` opens for the listing.
  std::set<int> child_descriptors(const scratch_t &scratch, const owned_child::options_t &options) {
    const auto out = (scratch.root / "fds").string();
    child_t child {options};
    EXPECT_TRUE(child.start({"/bin/sh", "-c", "/bin/ls /dev/fd/ > \"$OUT\""}, {"OUT=" + out}));
    EXPECT_EQ(wait_done(child), state_e::exited);
    EXPECT_EQ(child.exit_code(), 0);
    std::set<int> fds;
    std::istringstream text {scratch.read("fds")};
    for (std::string line; std::getline(text, line);) {
      if (!line.empty()) fds.insert(std::stoi(line));
    }
    return fds;
  }

  /// A descriptor at `number` without close-on-exec, closed when the holder goes.
  struct held_fd_t {
    int fd = -1;

    explicit held_fd_t(int number) {
      const int opened = ::open("/dev/null", O_RDONLY);
      fd = ::dup2(opened, number);
      ::close(opened);
    }

    ~held_fd_t() {
      if (fd >= 0) ::close(fd);
    }
  };
}  // namespace

TEST(OwnedChildTests, AHighNumberedDescriptorIsClosedByCloseRangeAndByTheBoundedFallback) {
  scratch_t scratch;
  struct rlimit limit {};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &limit), 0);
  const int high = static_cast<int>(std::min<rlim_t>(limit.rlim_cur - 1, 4000));
  held_fd_t held {high};
  ASSERT_EQ(held.fd, high);
  for (const bool allow_close_range : {true, false}) {
    owned_child::options_t options;
    options.allow_close_range = allow_close_range;
    const auto plan = owned_child::plan_descriptor_closure(options);
    ASSERT_TRUE(plan) << allow_close_range;
    if (!allow_close_range) {
      EXPECT_FALSE(plan->close_range);
      EXPECT_GT(plan->bound, high) << "the fallback reaches the highest open descriptor";
    }
    const auto fds = child_descriptors(scratch, options);
    EXPECT_FALSE(fds.contains(high)) << allow_close_range;
    EXPECT_TRUE(fds.contains(0) && fds.contains(1) && fds.contains(2));
    // Beyond stdio only the few low descriptors `ls` opens for itself: nothing inherited survives.
    EXPECT_TRUE(std::all_of(fds.begin(), fds.end(), [](int fd) { return fd < 10; })) << allow_close_range;
  }
}

TEST(OwnedChildTests, ADescriptorAboveALoweredLimitIsStillInsideTheFallbackBound) {
  scratch_t scratch;
  struct rlimit limit {};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &limit), 0);
  ASSERT_GT(limit.rlim_cur, 250u);
  held_fd_t held {240};
  ASSERT_EQ(held.fd, 240);
  struct rlimit lowered = limit;
  lowered.rlim_cur = 64;
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &lowered), 0);
  owned_child::options_t options;
  options.allow_close_range = false;
  const auto plan = owned_child::plan_descriptor_closure(options);
  const auto fds = child_descriptors(scratch, options);
  ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &limit), 0);
  ASSERT_TRUE(plan);
  EXPECT_GT(plan->bound, 240) << "from the listing, not only the lowered limit";
  EXPECT_FALSE(fds.contains(240));
}

TEST(OwnedChildTests, WithoutAProvableClosureNothingIsStarted) {
  scratch_t scratch;
  owned_child::options_t options;
  options.allow_close_range = false;
  options.fallback_cap = 8;  // below any real limit: no bound can be proven
  EXPECT_FALSE(owned_child::plan_descriptor_closure(options));
  child_t child {options};
  const auto marker = (scratch.root / "ran").string();
  EXPECT_FALSE(child.start({"/bin/sh", "-c", "echo ran > \"$M\""}, {"M=" + marker}));
  EXPECT_EQ(child.state(), state_e::failed);
  std::this_thread::sleep_for(100ms);
  EXPECT_FALSE(std::filesystem::exists(marker)) << "refused before fork";
}

TEST(OwnedChildTests, ExitStatusIsReportedOnlyFromItsOwnReap) {
  scratch_t scratch;
  {
    child_t child;
    ASSERT_TRUE(child.start({scratch.script("ok", "exit 0\n")}, {}));
    EXPECT_EQ(wait_done(child), state_e::exited);
    EXPECT_EQ(child.exit_code(), 0);
  }
  {
    child_t child;
    ASSERT_TRUE(child.start({scratch.script("three", "exit 3\n")}, {}));
    EXPECT_EQ(wait_done(child), state_e::exited);
    EXPECT_EQ(child.exit_code(), 3);
  }
  {
    // Signalled is not an exit status.
    child_t child;
    ASSERT_TRUE(child.start({scratch.script("selfkill", "kill -KILL $$\n")}, {}));
    EXPECT_EQ(wait_done(child), state_e::killed);
    EXPECT_FALSE(child.exit_code());
  }
  {
    // Someone else took its status: gone, outcome unknown, never "exited 0".
    child_t child;
    ASSERT_TRUE(child.start({scratch.script("stolen", "exit 0\n")}, {}));
    const auto pid = child.pid();
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    EXPECT_EQ(wait_done(child), state_e::killed);
    EXPECT_FALSE(child.exit_code());
  }
  {
    child_t child;
    EXPECT_FALSE(child.start({"relative/steamosctl"}, {})) << "only an absolute path, never a PATH search";
    EXPECT_EQ(child.state(), state_e::failed);
    child_t missing;
    ASSERT_TRUE(missing.start({(scratch.root / "absent").string()}, {}));
    EXPECT_EQ(wait_done(missing), state_e::exited);
    EXPECT_EQ(missing.exit_code(), 127) << "exec failed: a failure, never success";
  }
}

TEST(OwnedChildTests, AbortStopsTheChildsWholeGroupAndReapsOnlyIt) {
  scratch_t scratch;
  // An unrelated child of this process, in its own group: abort must not touch it.
  const pid_t bystander = ::fork();
  ASSERT_GE(bystander, 0);
  if (bystander == 0) {
    ::setpgid(0, 0);
    ::execl("/bin/sleep", "sleep", "30", static_cast<char *>(nullptr));
    ::_exit(127);
  }
  // The script and its grandchild both ignore TERM, so only the KILL after the grace ends them.
  const auto command = scratch.script("tree", "trap '' TERM\n/bin/sleep 30 &\necho $! > \"$PIDS\"\nwait\n");
  child_t child;
  ASSERT_TRUE(child.start({command}, {"PIDS=" + (scratch.root / "pids").string()}));
  const auto pid = child.pid();
  // Wait for the grandchild's pid to be written.
  std::string grandchild;
  for (int i = 0; i < 300 && grandchild.empty(); ++i) {
    std::this_thread::sleep_for(10ms);
    grandchild = scratch.read("pids");
  }
  ASSERT_FALSE(grandchild.empty());
  const auto start = std::chrono::steady_clock::now();
  child.abort(100ms);
  const auto took = std::chrono::steady_clock::now() - start;
  EXPECT_GE(took, 100ms) << "TERM first, with its grace";
  EXPECT_LT(took, 2s) << "TERM ignored: KILL after the grace";
  EXPECT_EQ(child.state(), state_e::killed);
  EXPECT_FALSE(child.exit_code());
  EXPECT_EQ(child.pid(), -1);
  EXPECT_TRUE(process_gone(pid));
  EXPECT_TRUE(grandchild_terminated(static_cast<pid_t>(std::stol(grandchild)))) << "its group went with it";
  errno = 0;
  EXPECT_EQ(::waitpid(pid, nullptr, WNOHANG), -1) << "the owned parent was already reaped";
  EXPECT_EQ(errno, ECHILD);
  int status = 0;
  EXPECT_EQ(::waitpid(bystander, &status, WNOHANG), 0) << "the bystander is untouched and unreaped";
  ::kill(bystander, SIGKILL);
  EXPECT_EQ(::waitpid(bystander, &status, 0), bystander);
  child.abort();
  EXPECT_EQ(child.state(), state_e::killed) << "idempotent";
}

TEST(OwnedChildTests, DestroyingARunningChildStopsAndReapsIt) {
  scratch_t scratch;
  pid_t pid = -1;
  {
    child_t child;
    ASSERT_TRUE(child.start({scratch.script("long", "exec /bin/sleep 30\n")}, {}));
    pid = child.pid();
    EXPECT_EQ(child.poll(), state_e::running);
  }
  EXPECT_TRUE(process_gone(pid));
  int status = 0;
  EXPECT_EQ(::waitpid(pid, &status, WNOHANG), -1) << "already reaped by its owner";
}

TEST(OwnedChildTests, RegisteredCasesAreFrozen) {
  const std::set<std::string> expected {
    "TheChildGetsExactlyItsArgvAndEnvironmentAndNoInheritedDescriptor",
    "AHighNumberedDescriptorIsClosedByCloseRangeAndByTheBoundedFallback",
    "ADescriptorAboveALoweredLimitIsStillInsideTheFallbackBound",
    "WithoutAProvableClosureNothingIsStarted",
    "ExitStatusIsReportedOnlyFromItsOwnReap",
    "AbortStopsTheChildsWholeGroupAndReapsOnlyIt",
    "DestroyingARunningChildStopsAndReapsIt",
    "RegisteredCasesAreFrozen",
  };
  std::set<std::string> registered;
  const auto *unit = ::testing::UnitTest::GetInstance();
  for (int suite = 0; suite < unit->total_test_suite_count(); ++suite) {
    const auto *info = unit->GetTestSuite(suite);
    if (std::string {info->name()} != "OwnedChildTests") continue;
    for (int test = 0; test < info->total_test_count(); ++test) registered.insert(info->GetTestInfo(test)->name());
  }
  EXPECT_EQ(registered, expected);
}
#endif
