/**
 * @file tests/unit/platform/test_kms_enable.cpp
 * @brief Test that --setup-host turns DRM/KMS capture on only once the account's session can use it.
 */
#include "../../tests_common.h"

#ifdef __linux__

#include <src/platform/linux/kms_enable.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace ke = platf::kms_enable;
namespace uu = platf::user_unit;

namespace {
  constexpr std::uint32_t streamer_uid = 1000;
  constexpr std::uint32_t kms_gid = 957;

  fs::path make_scratch() {
    auto pattern = (fs::temp_directory_path() / "polaris-kms-enable-XXXXXX").string();
    const char *made = mkdtemp(pattern.data());
    if (made == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    return made;
  }

  void write(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
  }

  std::string read(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  /**
   * Everything host setup would touch, under a scratch directory: the helper, the guide's copy, the
   * packaged binary and the account's home. Commands are recorded, never run, and the account
   * database and /proc answer what each test says they answer.
   */
  struct fake_host_t {
    fs::path root = make_scratch();
    ke::account_t account {"streamer", root / "home/streamer", streamer_uid, streamer_uid};
    std::set<fs::path> capabilities;
    bool member = true;  ///< what the account database says
    ke::session_group_e session = ke::session_group_e::live;  ///< what the running manager holds
    std::vector<std::string> commands;
    std::vector<fs::path> handed;
    bool lingering = false;
    std::vector<std::string> failing;  ///< commands starting with one of these fail
    ke::service_run_e service = ke::service_run_e::other;  ///< what the account's polaris user service runs now
    std::optional<std::string> capture;  ///< the capture the account's polaris.conf sets; nothing when unread

    fake_host_t() {
      fs::create_directories(account.home);
    }

    fake_host_t(const fake_host_t &) = delete;
    fake_host_t &operator=(const fake_host_t &) = delete;

    ~fake_host_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    fs::path helper() const {
      return root / "usr/libexec/polaris/polaris-kms";
    }

    fs::path copy() const {
      return root / "usr/local/bin/polaris-kms";
    }

    fs::path exe() const {
      return root / "usr/bin/polaris";
    }

    fs::path active() const {
      return ke::drop_in_dir(account) / "20-polaris-kms.conf";
    }

    fs::path parked() const {
      return ke::drop_in_dir(account) / "20-polaris-kms.conf.disabled-until-relogin";
    }

    /// The drop-in the old Bazzite recipe had people write by hand, pointing at its copy.
    fs::path recipe_drop_in() const {
      return ke::drop_in_dir(account) / "10-bazzite-kms.conf";
    }

    void write_recipe_drop_in() {
      write(recipe_drop_in(), "[Service]\nExecStart=\nExecStart=" + copy().string() + "\n");
    }

    /// What 1.4.13's --enable-kms wrote whether or not the group was live.
    void write_active_drop_in() {
      write(active(), "[Service]\nExecStart=\nExecStart=" + helper().string() + "\n");
    }

    void install_helper() {
      write(helper(), "helper");
      capabilities.insert(helper());
    }

    /// The Bazzite recipe: a copy with its own capability, and a packaged binary that may hold one too.
    void follow_the_old_recipe() {
      write(exe(), "packaged");
      write(copy(), "copy");
      capabilities.insert(copy());
      capabilities.insert(exe());
    }

    bool ran(const std::string &prefix) const {
      return std::any_of(commands.begin(), commands.end(), [&](const std::string &command) {
        return command.starts_with(prefix);
      });
    }

    ke::host_t host() {
      ke::host_t host;
      host.helper = helper();
      host.runtime_copy = copy();
      host.holds_capability = [this](const fs::path &path) {
        return capabilities.contains(path);
      };
      host.in_group = [this](const ke::account_t &) {
        return member;
      };
      host.session_group = [this](const ke::account_t &) {
        return session;
      };
      host.run = [this](const std::string &, const std::string &command) {
        commands.push_back(command);
        if (std::any_of(failing.begin(), failing.end(), [&](const std::string &prefix) {
              return command.starts_with(prefix);
            })) {
          return false;
        }
        if (command.starts_with("usermod")) {
          // The database changes at once; the running session does not.
          member = true;
        }
        return true;
      };
      host.hand_to = [this](const fs::path &path, const ke::account_t &) {
        handed.push_back(path);
      };
      host.lingering = [this](const ke::account_t &) {
        return lingering;
      };
      host.service_running = [this](const ke::account_t &, const fs::path &binary) {
        return binary == helper() ? service : ke::service_run_e::other;
      };
      host.configured_capture = [this](const ke::account_t &) {
        return capture;
      };
      return host;
    }
  };

  /// A /proc with the processes a test names, and nothing else.
  struct fake_proc_t {
    fs::path root = make_scratch();

    fake_proc_t(const fake_proc_t &) = delete;
    fake_proc_t &operator=(const fake_proc_t &) = delete;
    fake_proc_t() = default;

    ~fake_proc_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    void process(int pid, const std::string &name, std::uint32_t uid, const std::string &groups, const std::string &cmdline) {
      const auto dir = root / std::to_string(pid);
      write(
        dir / "status",
        "Name:\t" + name + "\nUmask:\t0022\nState:\tS (sleeping)\n"
                           "Uid:\t" +
          std::to_string(uid) + "\t" + std::to_string(uid) + "\t" + std::to_string(uid) + "\t" + std::to_string(uid) +
          "\n"
          "Gid:\t" +
          std::to_string(uid) + "\t" + std::to_string(uid) + "\t" + std::to_string(uid) + "\t" + std::to_string(uid) +
          "\n"
          "Groups:\t" +
          groups + "\nVmPeak:\t   22556 kB\n"
      );
      write(dir / "cmdline", cmdline);
    }

    void user_manager(int pid, std::uint32_t uid, const std::string &groups) {
      process(pid, "systemd", uid, groups, std::string {"/usr/lib/systemd/systemd\0--user\0", 32});
    }

    /// A process in a cgroup, whose exe link names what the kernel says it runs.
    void running(int pid, std::uint32_t uid, const std::string &cgroup, const std::string &exe) {
      process(pid, fs::path {exe}.filename().string(), uid, std::to_string(uid), exe + std::string {"\0", 1});
      write(root / std::to_string(pid) / "cgroup", cgroup);
      fs::create_symlink(exe, root / std::to_string(pid) / "exe");
    }
  };
}  // namespace

TEST(KmsEnableTests, ProcStatusGivesTheGroupsTheKernelChecks) {
  const auto status = ke::parse_process_status(
    "Name:\tsystemd\n"
    "Uid:\t1000\t1000\t1000\t1000\n"
    "Gid:\t1000\t1000\t1000\t1000\n"
    "Groups:\t6 10 18 39 957 \n"
  );
  EXPECT_EQ(status.name, "systemd");
  EXPECT_EQ(status.real_uid, 1000u);
  EXPECT_TRUE(status.holds(957)) << "a supplementary group";
  EXPECT_TRUE(status.holds(1000)) << "the filesystem gid counts as well as the supplementary groups";
  EXPECT_FALSE(status.holds(958));

  const auto no_groups = ke::parse_process_status("Name:\tsystemd\nUid:\t1000\t1000\t1000\t1000\nGid:\t1000\t1000\t1000\t1000\nGroups:\t\n");
  EXPECT_FALSE(no_groups.holds(957));
}

TEST(KmsEnableTests, OnlyTheAccountsOwnServiceManagerDecides) {
  fake_proc_t proc;
  // The system manager, somebody else's manager with the group, and one of the account's own
  // processes started after a login that already has it: none of them is what the service inherits.
  proc.process(1, "systemd", 0, "", std::string {"/usr/lib/systemd/systemd\0--switched-root\0--system\0", 50});
  proc.user_manager(500, 1001, "957 1001");
  proc.process(900, "bash", streamer_uid, "957 1000", std::string {"bash\0", 5});
  write(proc.root / "self/status", "Name:\tsystemd\nUid:\t1000\t1000\t1000\t1000\nGroups:\t957\n");

  EXPECT_EQ(ke::user_manager_group(proc.root, streamer_uid, kms_gid), ke::session_group_e::no_manager);

  proc.user_manager(982, streamer_uid, "6 10 1000");
  EXPECT_EQ(ke::user_manager_group(proc.root, streamer_uid, kms_gid), ke::session_group_e::not_live)
    << "the manager started before usermod, so the group reaches no user service yet";

  proc.user_manager(982, streamer_uid, "6 10 957 1000");
  EXPECT_EQ(ke::user_manager_group(proc.root, streamer_uid, kms_gid), ke::session_group_e::live);

  EXPECT_EQ(ke::user_manager_group(proc.root / "missing", streamer_uid, kms_gid), ke::session_group_e::no_manager);
}

TEST(KmsEnableTests, EnableKmsWithTheGroupLivePointsTheServiceAtTheHelper) {
  fake_host_t fake;
  fake.install_helper();

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
  EXPECT_NE(read(fake.active()).find("ExecStart=\nExecStart=" + fake.helper().string() + "\n"), std::string::npos);
  EXPECT_FALSE(fs::exists(fake.parked()));
  EXPECT_TRUE(fake.commands.empty()) << "already a member, so nothing to add";
  EXPECT_NE(outcome.summary.find("DRM/KMS capture is on"), std::string::npos);
  EXPECT_NE(std::find(fake.handed.begin(), fake.handed.end(), fake.active()), fake.handed.end()) << "written as root into the account's home";
}

TEST(KmsEnableTests, EnableKmsBeforeTheGroupIsLiveParksTheDropIn) {
  fake_host_t fake;
  fake.install_helper();
  fake.member = false;
  fake.session = ke::session_group_e::not_live;

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  ASSERT_EQ(fake.commands.size(), 1u);
  EXPECT_TRUE(fake.ran(R"(usermod -aG "polaris-kms" "streamer")"));
  EXPECT_FALSE(fs::exists(fake.active())) << "an active drop-in here is a service that cannot start (203/EXEC)";
  EXPECT_NE(read(fake.parked()).find("ExecStart=" + fake.helper().string()), std::string::npos);
  EXPECT_NE(outcome.summary.find("joined the polaris-kms group just now"), std::string::npos);
  EXPECT_NE(outcome.summary.find("log out and back in, then run\n  sudo -H polaris --setup-host --enable-kms\nagain"), std::string::npos)
    << outcome.summary;
  EXPECT_NE(outcome.summary.find("safe to run more than once;\nsudo -H polaris --setup-host on its own finishes it too."), std::string::npos);
}

TEST(KmsEnableTests, ParkingTakesAwayTheDropInThatStoppedTheService) {
  // What 1.4.13 left on a host: a member whose session predates the group, and an active drop-in.
  fake_host_t fake;
  fake.install_helper();
  fake.session = ke::session_group_e::not_live;
  fake.write_active_drop_in();

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_TRUE(fs::exists(fake.parked()));
  EXPECT_NE(outcome.summary.find("its session started before that"), std::string::npos);
  EXPECT_NE(outcome.summary.find("Removed " + fake.active().string() + ", which pointed the service at the helper\nbefore its session could execute it."), std::string::npos)
    << outcome.summary;
  // Already a member, so a logout may already have been tried and left the manager running.
  EXPECT_NE(outcome.summary.find("an SSH login counts"), std::string::npos);
  // systemd keeps the ExecStart it read until a reload, so the service stays down without these.
  EXPECT_NE(outcome.summary.find("Once reloaded, the service runs the packaged binary again.\n"
                                 "Reload and restart it now, as streamer:\n"
                                 "  systemctl --user daemon-reload\n"
                                 "  systemctl --user restart polaris\n"),
            std::string::npos)
    << outcome.summary;
  // It was not starting at all, so "the way it does now" is not a way to carry on.
  EXPECT_EQ(outcome.summary.find("capture carries on the way it does now"), std::string::npos) << outcome.summary;
}

TEST(KmsEnableTests, ParkingOnADownedOldRecipeHostAlsoRetiresTheDropInForTheDeletedCopy) {
  // A Bazzite host that 1.4.13's plain --setup-host moved off the copy while the group was not live:
  // 20-polaris-kms.conf points at a helper the session cannot execute, the copy is gone, and the old
  // recipe's 10-bazzite-kms.conf still names it. Removing only the first leaves the second in charge.
  fake_host_t fake;
  fake.install_helper();
  fake.session = ke::session_group_e::not_live;
  fake.write_recipe_drop_in();
  fake.write_active_drop_in();
  ASSERT_FALSE(fs::exists(fake.copy()));

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_FALSE(fs::exists(fake.recipe_drop_in())) << "left behind, it is a service that execs a file that is gone (203/EXEC)";
  EXPECT_FALSE(uu::effective_exec_override(ke::drop_in_dir(fake.account)).active());
  EXPECT_NE(outcome.summary.find("Removed " + fake.recipe_drop_in().string() + ", which pointed the service at " + fake.copy().string() + ",\na copy that is no longer there."), std::string::npos)
    << outcome.summary;
  EXPECT_NE(outcome.summary.find("the service runs the packaged binary again"), std::string::npos);
  EXPECT_NE(outcome.summary.find("systemctl --user daemon-reload"), std::string::npos);
}

TEST(KmsEnableTests, ParkingLeavesTheOldRecipesDropInWhileItsCopyStillCaptures) {
  // The same drop-ins with the copy still there: beneath the helper's, the copy is what can capture.
  fake_host_t fake;
  fake.install_helper();
  fake.follow_the_old_recipe();
  fake.session = ke::session_group_e::not_live;
  fake.write_recipe_drop_in();
  fake.write_active_drop_in();

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_TRUE(fs::exists(fake.recipe_drop_in()));
  EXPECT_TRUE(fs::exists(fake.copy()));
  EXPECT_NE(outcome.summary.find("Once reloaded, the service runs " + fake.copy().string() + ", which " + fake.recipe_drop_in().string() + " names"), std::string::npos)
    << outcome.summary;
}

TEST(KmsEnableTests, WhileLingeringOnlyARebootBringsTheGroupIn) {
  // Lingering keeps the service manager from boot to shutdown, so "log out and back in" would
  // leave the group exactly as missing as before. Headless boot is one way to linger, not the only
  // one: pc-papi lingers for other user services and has no headless boot at all.
  fake_host_t fake;
  fake.install_helper();
  fake.session = ke::session_group_e::not_live;
  fake.lingering = true;

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::parked);
  EXPECT_NE(outcome.summary.find("Lingering (loginctl enable-linger, which headless boot turns on) keeps the service manager\n"
                                 "for streamer running from boot to shutdown"),
            std::string::npos)
    << outcome.summary;
  EXPECT_EQ(outcome.summary.find("Headless boot keeps"), std::string::npos);
  EXPECT_NE(outcome.summary.find("To finish, reboot, then run\n  sudo -H polaris --setup-host --enable-kms\nagain"), std::string::npos)
    << outcome.summary;
  EXPECT_EQ(outcome.summary.find("log out and back in, then run"), std::string::npos);
}

TEST(KmsEnableTests, LingeringCountsTheLingerFileAndAHeadlessBootThisRunTurnsOn) {
  fake_host_t fake;
  const auto linger = fake.root / "var/lib/systemd/linger";
  fs::create_directories(linger);

  EXPECT_FALSE(ke::lingers(linger, "streamer", false));
  // --enable-kms --enable-headless-boot: the headless boot step runs loginctl enable-linger after
  // DRM/KMS setup, so by the time anyone reads the advice, a logout no longer restarts the manager.
  EXPECT_TRUE(ke::lingers(linger, "streamer", true));
  write(linger / "streamer", "");
  EXPECT_TRUE(ke::lingers(linger, "streamer", false));
  EXPECT_FALSE(ke::lingers(linger, "someone-else", false));
}

TEST(KmsEnableTests, RunningAgainAfterTheLoginTurnsTheParkedDropInOn) {
  fake_host_t fake;
  fake.install_helper();
  fake.member = false;
  fake.session = ke::session_group_e::not_live;

  ASSERT_EQ(ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms").result, ke::result_e::parked);
  const auto parked_once = read(fake.parked());
  ASSERT_EQ(ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms").result, ke::result_e::parked)
    << "still no login, so still parked";
  EXPECT_EQ(read(fake.parked()), parked_once);
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_EQ(fake.commands.size(), 1u) << "usermod ran once; the second run found the account already a member";

  fake.session = ke::session_group_e::live;
  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host");
  ASSERT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
  EXPECT_TRUE(fs::exists(fake.active()));
  EXPECT_FALSE(fs::exists(fake.parked())) << "turned on, not left behind to be found again";
  EXPECT_NE(outcome.summary.find("replaces the drop-in an earlier run parked"), std::string::npos);

  ASSERT_EQ(ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host").result, ke::result_e::on);
  EXPECT_TRUE(fs::exists(fake.active()));
  EXPECT_FALSE(fs::exists(fake.parked()));
}

TEST(KmsEnableTests, NoRunningSessionParksRatherThanGuesses) {
  fake_host_t fake;
  fake.install_helper();
  fake.session = ke::session_group_e::no_manager;

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::parked);
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_NE(outcome.summary.find("No session is running for streamer"), std::string::npos);
  EXPECT_NE(outcome.summary.find("log in as streamer, then run"), std::string::npos);
}

TEST(KmsEnableTests, EnableKmsWithoutAUsableHelperChangesNothing) {
  fake_host_t fake;
  fake.member = false;
  fake.session = ke::session_group_e::not_live;

  auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");
  EXPECT_EQ(outcome.result, ke::result_e::failed);
  EXPECT_NE(outcome.refusal.find("needs the polaris-kms package"), std::string::npos);

  write(fake.helper(), "helper");
  outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");
  EXPECT_EQ(outcome.result, ke::result_e::failed);
  EXPECT_NE(outcome.refusal.find("carries no cap_sys_admin"), std::string::npos);

  EXPECT_TRUE(fake.commands.empty());
  EXPECT_FALSE(fs::exists(ke::drop_in_dir(fake.account)));
}

TEST(KmsEnableTests, SetupHostWithTheGroupLiveRetiresTheOldRecipesCopy) {
  fake_host_t fake;
  fake.install_helper();
  fake.follow_the_old_recipe();

  const auto outcome = ke::move_off_runtime_copy(fake.account, fake.host(), fake.exe(), false);

  ASSERT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
  EXPECT_TRUE(fs::exists(fake.active()));
  EXPECT_FALSE(fs::exists(fake.copy()));
  EXPECT_TRUE(fake.ran("setcap -r \"" + fake.exe().string() + "\""));
  EXPECT_FALSE(outcome.copy_refreshed);
}

TEST(KmsEnableTests, SetupHostBeforeTheGroupIsLiveKeepsTheCopyThatCapturesToday) {
  // A plain `sudo -H polaris --setup-host`, which postinst prints, on a host still on the old recipe.
  fake_host_t fake;
  fake.install_helper();
  fake.follow_the_old_recipe();
  fake.member = false;
  fake.session = ke::session_group_e::not_live;

  const auto outcome = ke::move_off_runtime_copy(fake.account, fake.host(), fake.exe(), false);

  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  EXPECT_TRUE(fs::exists(fake.copy())) << "the copy is the only thing here that can capture until the login";
  EXPECT_FALSE(fake.ran("setcap -r")) << "no capability comes off anything yet";
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_TRUE(fs::exists(fake.parked()));
  EXPECT_NE(outcome.summary.find("log out and back in, then run\n  sudo -H polaris --setup-host\nagain"), std::string::npos) << outcome.summary;
  EXPECT_EQ(outcome.summary.find("on its own finishes it too"), std::string::npos) << "it is already the plain command";
  EXPECT_NE(outcome.summary.find("removes that copy"), std::string::npos);

  // After the login the same command finishes the move.
  fake.session = ke::session_group_e::live;
  const auto finished = ke::move_off_runtime_copy(fake.account, fake.host(), fake.exe(), false);
  ASSERT_EQ(finished.result, ke::result_e::on) << finished.refusal;
  EXPECT_FALSE(fs::exists(fake.copy()));
  EXPECT_FALSE(fs::exists(fake.parked()));
  EXPECT_TRUE(fs::exists(fake.active()));
  EXPECT_TRUE(fake.ran("setcap -r \"" + fake.exe().string() + "\""));
}

TEST(KmsEnableTests, AParkedMoveStillRefreshesACopyThatFellBehind) {
  fake_host_t fake;
  fake.install_helper();
  fake.follow_the_old_recipe();
  fake.session = ke::session_group_e::not_live;

  const auto outcome = ke::move_off_runtime_copy(fake.account, fake.host(), fake.exe(), true);

  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  EXPECT_TRUE(outcome.copy_refreshed);
  EXPECT_TRUE(fake.ran("install -D -m 0755 \"" + fake.exe().string() + "\" \"" + fake.copy().string() + "\""));
  EXPECT_TRUE(fake.ran("setcap cap_sys_admin+ep \"" + fake.copy().string() + "\""));
  EXPECT_FALSE(fake.ran("setcap -r"));
}

TEST(KmsEnableTests, WithoutTheHelperSetupHostOnlyRefreshesAStaleCopy) {
  fake_host_t fake;
  fake.follow_the_old_recipe();

  auto outcome = ke::move_off_runtime_copy(fake.account, fake.host(), fake.exe(), false);
  EXPECT_EQ(outcome.result, ke::result_e::unchanged);
  EXPECT_TRUE(fake.commands.empty());

  outcome = ke::move_off_runtime_copy(fake.account, fake.host(), fake.exe(), true);
  EXPECT_EQ(outcome.result, ke::result_e::unchanged);
  EXPECT_TRUE(outcome.copy_refreshed);
  EXPECT_TRUE(fs::exists(fake.copy()));
  EXPECT_FALSE(fs::exists(ke::drop_in_dir(fake.account)));
}

TEST(KmsEnableTests, MovingOffTheCopyTakesTheRecipesDropInWithIt) {
  // Left behind, 10-bazzite-kms.conf names a copy that is gone, and it takes over the moment
  // 20-polaris-kms.conf goes, which --disable-kms does.
  fake_host_t fake;
  fake.install_helper();
  fake.follow_the_old_recipe();
  fake.write_recipe_drop_in();

  const auto outcome = ke::move_off_runtime_copy(fake.account, fake.host(), fake.exe(), false);

  ASSERT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
  EXPECT_TRUE(fs::exists(fake.active()));
  EXPECT_FALSE(fs::exists(fake.copy()));
  EXPECT_FALSE(fs::exists(fake.recipe_drop_in()));
  EXPECT_EQ(uu::effective_exec_override(ke::drop_in_dir(fake.account)).binary, fake.helper());
  EXPECT_NE(outcome.summary.find("Moved DRM/KMS capture off " + fake.copy().string()), std::string::npos) << outcome.summary;
  EXPECT_NE(outcome.summary.find("Removed " + fake.recipe_drop_in().string() + ", the old recipe's drop-in"), std::string::npos) << outcome.summary;
  EXPECT_NE(outcome.summary.find("nothing to re-run after one"), std::string::npos);
}

TEST(KmsEnableTests, AMoveWhoseLastStepFailsStillSaysWhatItChanged) {
  // By the time setcap -r fails, the service points at the helper and the copy is gone; a failed
  // host setup never reaches its summary, so the failure has to carry it.
  fake_host_t fake;
  fake.install_helper();
  fake.follow_the_old_recipe();
  fake.write_recipe_drop_in();
  fake.failing.push_back("setcap -r");

  const auto outcome = ke::move_off_runtime_copy(fake.account, fake.host(), fake.exe(), false);

  ASSERT_EQ(outcome.result, ke::result_e::failed);
  const auto report = ke::failure_report(outcome);
  EXPECT_EQ(report.find("Could not remove cap_sys_admin from " + fake.exe().string()), 0u) << report;
  EXPECT_NE(report.find("Pointed the polaris user service at " + fake.helper().string()), std::string::npos) << report;
  EXPECT_NE(report.find("systemctl --user daemon-reload"), std::string::npos) << report;
  EXPECT_NE(report.find("Removed " + fake.copy().string()), std::string::npos) << report;

  // Parked, and then the copy could not be refreshed: the parked drop-in is still worth knowing about.
  fake_host_t parked;
  parked.install_helper();
  parked.follow_the_old_recipe();
  parked.session = ke::session_group_e::not_live;
  parked.failing.push_back("install -D");
  const auto stuck = ke::move_off_runtime_copy(parked.account, parked.host(), parked.exe(), true);
  ASSERT_EQ(stuck.result, ke::result_e::failed);
  EXPECT_NE(ke::failure_report(stuck).find("The drop-in waits at " + parked.parked().string()), std::string::npos) << ke::failure_report(stuck);

  // Nothing done, nothing more to say.
  ke::outcome_t refused;
  refused.refusal = "why\n";
  EXPECT_EQ(ke::failure_report(refused), "why\n");
}

TEST(KmsEnableTests, PlainSetupHostParksTheDropIn1413WroteBeforeTheGroupWasLive) {
  // 1.4.13's --enable-kms wrote the drop-in active at once. On a host still in that state a plain
  // `sudo -H polaris --setup-host` said "nothing to do" while the service failed with 203/EXEC.
  fake_host_t fake;
  fake.install_helper();
  fake.session = ke::session_group_e::not_live;
  fake.write_active_drop_in();

  const auto facts = ke::read_facts({}, &fake.account, fake.host());
  EXPECT_TRUE(facts.helper_unreachable);
  EXPECT_FALSE(facts.parked);
  ASSERT_EQ(ke::setup_step(facts), ke::setup_step_e::settle) << "not nothing to do";
  EXPECT_NE(ke::waiting_notice(facts, fake.account, fake.host()).find("status=203/EXEC"), std::string::npos);

  const auto outcome = ke::settle(fake.account, fake.host());

  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_TRUE(fs::exists(fake.parked()));
  EXPECT_TRUE(fake.commands.empty());
  EXPECT_NE(outcome.summary.find("systemctl --user daemon-reload"), std::string::npos) << outcome.summary;
  EXPECT_NE(outcome.summary.find("log out and back in, then run\n  sudo -H polaris --setup-host\nagain"), std::string::npos) << outcome.summary;

  // Once it is parked, the next plain run is the one that finishes it.
  const auto after = ke::read_facts({}, &fake.account, fake.host());
  EXPECT_TRUE(after.parked);
  EXPECT_FALSE(after.helper_unreachable);
  EXPECT_EQ(ke::setup_step(after), ke::setup_step_e::settle);
}

TEST(KmsEnableTests, PlainSetupHostTurnsAParkedDropInOnAfterTheLogin) {
  fake_host_t fake;
  fake.install_helper();
  fake.member = false;
  fake.session = ke::session_group_e::not_live;
  ASSERT_EQ(ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms").result, ke::result_e::parked);

  // Still no login: it stays parked, and saying so is not a failure.
  auto outcome = ke::settle(fake.account, fake.host());
  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  EXPECT_FALSE(fs::exists(fake.active()));

  fake.session = ke::session_group_e::live;
  const auto facts = ke::read_facts({}, &fake.account, fake.host());
  ASSERT_EQ(ke::setup_step(facts), ke::setup_step_e::settle);
  outcome = ke::settle(fake.account, fake.host());
  ASSERT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
  EXPECT_TRUE(fs::exists(fake.active()));
  EXPECT_FALSE(fs::exists(fake.parked()));
  EXPECT_EQ(fake.commands.size(), 1u) << "the usermod --enable-kms ran; the plain runs added nothing";

  // Done: the next plain run has nothing to settle.
  EXPECT_EQ(ke::setup_step(ke::read_facts({}, &fake.account, fake.host())), ke::setup_step_e::none);
  EXPECT_EQ(ke::settle(fake.account, fake.host()).result, ke::result_e::unchanged);
}

TEST(KmsEnableTests, PlainSetupHostNeverAddsTheAccountToTheGroup) {
  // Someone took the account out of polaris-kms on purpose. The command every install prints must
  // not put it back; that is what --enable-kms is asked for.
  fake_host_t fake;
  fake.install_helper();
  write(fake.parked(), "[Service]\nExecStart=\nExecStart=" + fake.helper().string() + "\n");
  fake.member = false;
  fake.session = ke::session_group_e::live;

  const auto outcome = ke::settle(fake.account, fake.host());

  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  EXPECT_TRUE(fake.commands.empty());
  EXPECT_TRUE(fs::exists(fake.parked()));
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_NE(outcome.summary.find("streamer is not in the polaris-kms group"), std::string::npos) << outcome.summary;
  EXPECT_NE(outcome.summary.find("sudo -H polaris --setup-host --enable-kms"), std::string::npos);
  EXPECT_NE(outcome.summary.find("sudo -H polaris --setup-host --disable-kms"), std::string::npos);
}

TEST(KmsEnableTests, PlainSetupHostLeavesAParkedDropInWhenTheHelperIsGone) {
  // polaris-kms was removed after the drop-in was parked. The plain run used to fail host setup as a
  // whole over it, telling someone who ran the package's own step to install a package.
  fake_host_t fake;
  write(fake.parked(), "[Service]\nExecStart=\nExecStart=" + fake.helper().string() + "\n");

  const auto outcome = ke::settle(fake.account, fake.host());

  ASSERT_EQ(outcome.result, ke::result_e::parked) << outcome.refusal;
  EXPECT_TRUE(outcome.refusal.empty());
  EXPECT_TRUE(fs::exists(fake.parked()));
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_TRUE(fake.commands.empty());
  EXPECT_NE(outcome.summary.find(fake.helper().string() + " is not installed"), std::string::npos) << outcome.summary;
  EXPECT_NE(outcome.summary.find("--disable-kms"), std::string::npos);
}

TEST(KmsEnableTests, OnlyAManagerThatCannotExecuteTheHelperNeedsSettling) {
  fake_host_t fake;
  fake.install_helper();
  fake.write_active_drop_in();

  EXPECT_FALSE(ke::read_facts({}, &fake.account, fake.host()).helper_unreachable) << "the group is live: capture works";

  // No manager yet: the next one starts with what the account database says.
  fake.session = ke::session_group_e::no_manager;
  EXPECT_FALSE(ke::read_facts({}, &fake.account, fake.host()).helper_unreachable);
  fake.member = false;
  EXPECT_TRUE(ke::read_facts({}, &fake.account, fake.host()).helper_unreachable);

  // An uninstalled helper is the missing-binary advice's case, with its own two ways out.
  fake.session = ke::session_group_e::not_live;
  fs::remove(fake.helper());
  EXPECT_FALSE(ke::read_facts({}, &fake.account, fake.host()).helper_unreachable);

  // A drop-in of someone's own that names the helper is not one host setup wrote.
  fake.install_helper();
  fs::remove(fake.active());
  write(ke::drop_in_dir(fake.account) / "30-mine.conf", "[Service]\nExecStart=\nExecStart=" + fake.helper().string() + "\n");
  EXPECT_FALSE(ke::read_facts({}, &fake.account, fake.host()).helper_unreachable);

  EXPECT_FALSE(ke::read_facts({}, nullptr, fake.host()).helper_unreachable) << "no account, nothing to read";
}

TEST(KmsEnableTests, NothingToDoAndTheStepTakenAreOneDecision) {
  using step = ke::setup_step_e;
  EXPECT_EQ(ke::setup_step({}), step::none);
  EXPECT_EQ(ke::setup_step({.parked = true}), step::settle);
  EXPECT_EQ(ke::setup_step({.helper_unreachable = true}), step::settle);
  EXPECT_EQ(ke::setup_step({.runtime_copy_in_use = true, .parked = true}), step::move_off_copy) << "the move finishes a parked drop-in itself";
  EXPECT_EQ(ke::setup_step({.enable_kms = true, .runtime_copy_in_use = true}), step::enable);
  // Refreshing or moving the copy would put back what --disable-kms was asked to take away.
  EXPECT_EQ(ke::setup_step({.disable_kms = true, .runtime_copy_in_use = true, .parked = true}), step::disable);
}

TEST(KmsEnableTests, DisableKmsTakesEveryDropInThatWouldTakeOver) {
  // 1.4.13 moved this host off the copy and left the old recipe's drop-in naming it. Removing only
  // 20-polaris-kms.conf handed the service to that drop-in, which cannot exec (203/EXEC), while the
  // summary said the service ran the packaged binary again.
  fake_host_t fake;
  fake.install_helper();
  write(fake.exe(), "packaged");
  fake.write_recipe_drop_in();
  fake.write_active_drop_in();

  const auto outcome = ke::disable(&fake.account, fake.host(), fake.exe());

  ASSERT_EQ(outcome.result, ke::result_e::off) << outcome.refusal;
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_FALSE(fs::exists(fake.recipe_drop_in()));
  EXPECT_FALSE(uu::effective_exec_override(ke::drop_in_dir(fake.account)).active());
  EXPECT_NE(outcome.summary.find("Once reloaded, the service runs the packaged binary again."), std::string::npos) << outcome.summary;
  EXPECT_NE(outcome.summary.find("systemctl --user daemon-reload"), std::string::npos);
  EXPECT_NE(outcome.summary.find("DRM/KMS capture is off"), std::string::npos);
}

TEST(KmsEnableTests, DisableKmsKeepsSomeoneElsesDropInAndSaysWhatRuns) {
  fake_host_t fake;
  fake.install_helper();
  write(fake.exe(), "packaged");
  const auto mine = fake.root / "home/streamer/build/polaris";
  write(mine, "a build of my own");
  fs::permissions(mine, fs::perms::owner_all, fs::perm_options::add);
  const auto my_drop_in = ke::drop_in_dir(fake.account) / "10-my-build.conf";
  write(my_drop_in, "[Service]\nExecStart=\nExecStart=" + mine.string() + "\n");
  fake.write_active_drop_in();

  const auto outcome = ke::disable(&fake.account, fake.host(), fake.exe());

  ASSERT_EQ(outcome.result, ke::result_e::off) << outcome.refusal;
  EXPECT_FALSE(fs::exists(fake.active()));
  EXPECT_TRUE(fs::exists(my_drop_in));
  EXPECT_NE(outcome.summary.find("Once reloaded, the service runs " + mine.string() + ", which " + my_drop_in.string() + " names"), std::string::npos)
    << outcome.summary;
  EXPECT_EQ(outcome.summary.find("packaged binary again"), std::string::npos);
  EXPECT_EQ(outcome.summary.find("cannot start"), std::string::npos);
}

TEST(KmsEnableTests, DisableKmsRemovesAParkedDropInAndEveryOldPiece) {
  fake_host_t fake;
  fake.install_helper();
  fake.follow_the_old_recipe();
  fake.write_recipe_drop_in();
  write(fake.parked(), "[Service]\nExecStart=\nExecStart=" + fake.helper().string() + "\n");

  const auto outcome = ke::disable(&fake.account, fake.host(), fake.exe());

  ASSERT_EQ(outcome.result, ke::result_e::off) << outcome.refusal;
  EXPECT_FALSE(fs::exists(fake.parked())) << "left behind, the next --setup-host would turn capture back on";
  EXPECT_FALSE(fs::exists(fake.recipe_drop_in()));
  EXPECT_FALSE(fs::exists(fake.copy()));
  EXPECT_TRUE(fake.ran("setcap -r \"" + fake.exe().string() + "\""));

  fake_host_t untouched;
  write(untouched.exe(), "packaged");
  const auto nothing = ke::disable(&untouched.account, untouched.host(), untouched.exe());
  EXPECT_EQ(nothing.result, ke::result_e::unchanged);
  EXPECT_NE(nothing.summary.find("Nothing to remove"), std::string::npos);
  EXPECT_TRUE(untouched.commands.empty());
}

TEST(KmsEnableTests, HostSetupRunsTheRealSeamsThroughOneDecision) {
  // The decisions above are only as good as what host setup hands them. These are the lines that
  // made a stand in of /proc, lingering and "nothing to do" possible to get wrong unseen.
  const auto source = read(fs::path {POLARIS_SOURCE_DIR} / "src/entry_handler.cpp");
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("return platf::kms_enable::user_manager_group(\"/proc\", account.uid, gr->gr_gid);"), std::string::npos)
    << "the group check reads the running manager";
  EXPECT_NE(source.find("return platf::kms_enable::lingers(\"/var/lib/systemd/linger\", account.name, enabling_headless_boot);"), std::string::npos);
  EXPECT_NE(source.find("const auto kms_host = kms_setup_host(enable_headless_boot);"), std::string::npos)
    << "a headless boot turned on in the same run lingers by the time anyone reads the advice";
  EXPECT_NE(source.find("if (!headless_boot_requested && kms_step == platf::kms_enable::setup_step_e::none && udev_from_package"), std::string::npos)
    << "nothing to do only when the DRM/KMS decision says so";
  EXPECT_NE(source.find("ok &= take_kms_step(platf::kms_enable::settle(*kms_account, kms_host));"), std::string::npos);
  EXPECT_EQ(source.find("platf::kms_enable::enable(*kms_account, kms_host, \"sudo -H polaris --setup-host\")"), std::string::npos)
    << "a plain run settles; it does not run --enable-kms, which adds the account to the group";
  EXPECT_NE(source.find("service_override_advice.clear();"), std::string::npos)
    << "the copy's own advice does not outlive the move off the copy";
  EXPECT_NE(source.find("return platf::kms_enable::user_service_running(\"/proc\", account.uid, binary);"), std::string::npos)
    << "whether the service already runs the helper is read from the service's own processes";
  EXPECT_NE(source.find("platf::kms_enable::read_small_file(account.home / \".config/polaris/polaris.conf\")"), std::string::npos)
    << "the capture the summary names is the account's own";
}

TEST(KmsEnableTests, EnableKmsOnAServiceAlreadyRunningTheHelperNeedsNoReload) {
  // A host set up for DRM/KMS whose service runs the helper already. Running --enable-kms again used
  // to rewrite the drop-in and ask for a daemon-reload and a restart that changed nothing.
  fake_host_t fake;
  fake.install_helper();
  write(fake.active(), "# kept exactly as it was\n[Service]\nExecStart=\nExecStart=" + fake.helper().string() + "\n");
  fake.service = ke::service_run_e::runs;
  const auto written = read(fake.active());

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
  EXPECT_NE(outcome.summary.find("The polaris user service already runs " + fake.helper().string() + " (" + fake.active().string() + "),\n"
                                 "so nothing needs reloading or restarting.\n"),
            std::string::npos)
    << outcome.summary;
  EXPECT_EQ(outcome.summary.find("daemon-reload"), std::string::npos) << outcome.summary;
  EXPECT_EQ(outcome.summary.find("still runs the old command"), std::string::npos) << outcome.summary;
  EXPECT_NE(outcome.summary.find("DRM/KMS capture is on"), std::string::npos);
  // A rewrite alone makes systemd ask for a daemon-reload, and would have dropped the first line.
  EXPECT_EQ(read(fake.active()), written);
  EXPECT_EQ(std::find(fake.handed.begin(), fake.handed.end(), fake.active()), fake.handed.end()) << "nothing written, nothing to hand back";
}

TEST(KmsEnableTests, APointedServiceThatRunsSomethingElseStillGetsTheReloadSteps) {
  // The drop-in is right, and the service started before it.
  fake_host_t fake;
  fake.install_helper();
  fake.write_active_drop_in();

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
  EXPECT_NE(outcome.summary.find("The polaris user service is already pointed at " + fake.helper().string() + " (" + fake.active().string() + "),\n"
                                 "but it is not running it now. Reload and restart it now, as streamer:\n"
                                 "  systemctl --user daemon-reload\n"
                                 "  systemctl --user restart polaris\n"),
            std::string::npos)
    << outcome.summary;
  EXPECT_EQ(outcome.summary.find("nothing needs reloading"), std::string::npos) << outcome.summary;
}

TEST(KmsEnableTests, AServiceStillRunningAReplacedHelperGetsTheRestart) {
  // An update replaced the helper under a running service: the kernel names it "(deleted)", and the
  // service keeps the older copy until it restarts.
  fake_host_t fake;
  fake.install_helper();
  fake.write_active_drop_in();
  fake.service = ke::service_run_e::runs_replaced;

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
  EXPECT_NE(outcome.summary.find("but runs an older copy of it, which an update replaced. Reload and restart it now, as streamer:\n"
                                 "  systemctl --user daemon-reload\n"
                                 "  systemctl --user restart polaris\n"),
            std::string::npos)
    << outcome.summary;
  EXPECT_EQ(outcome.summary.find("nothing needs reloading"), std::string::npos) << outcome.summary;
}

TEST(KmsEnableTests, ADropInThisRunWritesAlwaysGetsTheReloadSteps) {
  // systemd reads a new drop-in only after a reload, whatever the service runs meanwhile.
  fake_host_t fake;
  fake.install_helper();
  fake.service = ke::service_run_e::runs;

  const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");

  ASSERT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
  EXPECT_NE(outcome.summary.find("Pointed the polaris user service at " + fake.helper().string()), std::string::npos) << outcome.summary;
  EXPECT_NE(outcome.summary.find("  systemctl --user daemon-reload\n  systemctl --user restart polaris\n"), std::string::npos) << outcome.summary;
  EXPECT_EQ(outcome.summary.find("nothing needs reloading"), std::string::npos) << outcome.summary;
}

TEST(KmsEnableTests, TurningKmsOnSaysWhenCaptureGoesAnotherWay) {
  // The helper only gives Polaris the capability. A host set to portal or kwin gives it up at
  // startup, and "DRM/KMS capture is on" sent someone to look for KMS in a stream that never uses it.
  const auto line_for = [](std::optional<std::string> capture) {
    fake_host_t fake;
    fake.install_helper();
    fake.capture = std::move(capture);
    const auto outcome = ke::enable(fake.account, fake.host(), "sudo -H polaris --setup-host --enable-kms");
    EXPECT_EQ(outcome.result, ke::result_e::on) << outcome.refusal;
    return outcome.summary;
  };

  for (const auto *capture : {"kms", "drm"}) {
    const auto summary = line_for(std::string {capture});
    EXPECT_NE(summary.find("DRM/KMS capture is on, and an update cannot take it away again.\n"), std::string::npos) << capture;
  }
  // polaris.conf could not be read, so there is nothing to say about capture.
  EXPECT_NE(line_for(std::nullopt).find("DRM/KMS capture is on"), std::string::npos);

  for (const auto *capture : {"portal", "kwin"}) {
    const auto summary = line_for(std::string {capture});
    EXPECT_EQ(summary.find("DRM/KMS capture is on"), std::string::npos) << capture;
    EXPECT_NE(summary.find("The helper is ready, and an update cannot take it away again. But polaris.conf sets capture = " + std::string {capture} + ",\n"
                           "and for that Polaris gives the capability up at startup, because the portal and KWin refuse\n"
                           "a program that holds it. To capture through KMS, set capture = kms"),
              std::string::npos)
      << summary;
  }

  const auto autodetect = line_for(std::string {});
  EXPECT_NE(autodetect.find("But polaris.conf leaves capture on\nAutodetect, which uses KMS only when its search reaches it"), std::string::npos) << autodetect;
  const auto wlr = line_for(std::string {"wlr"});
  EXPECT_NE(wlr.find("so Polaris captures through that and not through KMS. To capture through KMS, set capture = kms"), std::string::npos) << wlr;
  EXPECT_NE(wlr.find("after choosing the stream mode, which sets capture\ntoo) and restart Polaris.\n"), std::string::npos) << wlr;
}

TEST(KmsEnableTests, OnlyTheServicesOwnProcessRunningTheBinaryCounts) {
  const fs::path helper {"/usr/libexec/polaris/polaris-kms"};
  const std::string service = "1:net_cls:/\n0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n";
  const std::string desktop = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-dev.polaris\\x2dstream.app.Polaris@7.service\n";

  {
    fake_proc_t proc;
    EXPECT_EQ(ke::user_service_running(proc.root, streamer_uid, helper), ke::service_run_e::other) << "nothing runs";

    proc.running(100, streamer_uid, desktop, helper.string());
    EXPECT_EQ(ke::user_service_running(proc.root, streamer_uid, helper), ke::service_run_e::other) << "a desktop launch never reads the drop-in";

    proc.running(101, streamer_uid + 1, service, helper.string());
    EXPECT_EQ(ke::user_service_running(proc.root, streamer_uid, helper), ke::service_run_e::other) << "another account's service";

    proc.running(102, streamer_uid, service, "/usr/bin/polaris");
    EXPECT_EQ(ke::user_service_running(proc.root, streamer_uid, helper), ke::service_run_e::other) << "the service runs the plain binary";
    EXPECT_EQ(ke::user_service_running(proc.root, streamer_uid, "/usr/bin/polaris"), ke::service_run_e::runs);

    proc.running(103, streamer_uid, service, helper.string() + " (deleted)");
    EXPECT_EQ(ke::user_service_running(proc.root, streamer_uid, helper), ke::service_run_e::runs_replaced) << "an update replaced the helper it runs";

    proc.running(104, streamer_uid, service, helper.string());
    EXPECT_EQ(ke::user_service_running(proc.root, streamer_uid, helper), ke::service_run_e::runs);
  }
  fake_proc_t empty;
  EXPECT_EQ(ke::user_service_running(empty.root / "missing", streamer_uid, helper), ke::service_run_e::other);
}

#endif
