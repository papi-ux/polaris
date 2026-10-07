/**
 * @file tests/unit/test_steamos_session.cpp
 * @brief #204 SteamOS session switch: the portable rules over fixture records.
 *
 * Every record here is a fixture laid out like its source (pacman's local database, logind and
 * systemd property shapes, /proc). Nothing reads the machine's own sessions or runs a command. These
 * are rules, not hardware acceptance: they say what the adapter will refuse, not that a real Steam
 * Deck behaves as the upstream sources say.
 */
#include <src/steamos_session.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

#ifndef _WIN32
  #include <sys/socket.h>
  #include <sys/stat.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

namespace steamos = prelaunch::steamos;
using steamos::mode_e;
using steamos::session_identity_t;
using steamos::snapshot_t;

namespace {
  struct scratch_t {
    std::filesystem::path root;

    scratch_t() {
      std::random_device device;
      root = std::filesystem::temp_directory_path() / ("polaris-steamos-" + std::to_string(device()));
      std::filesystem::create_directories(root);
    }

    ~scratch_t() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path write(const std::filesystem::path &relative, const std::string &text) const {
      const auto path = root / relative;
      std::filesystem::create_directories(path.parent_path());
      std::ofstream {path, std::ios::binary} << text;
      return path;
    }
  };

  std::string desc_text(const std::string &name, const std::string &version) {
    return "%NAME%\n" + name + "\n\n%VERSION%\n" + version + "\n\n%DESC%\nSteamOS Manager\n\n";
  }

  const std::string owning_files = "%FILES%\nusr/\nusr/bin/\nusr/bin/steamosctl\nusr/lib/\nusr/lib/steamos-manager\n\n";

  /// One installed record under a scratch local database.
  void install(const scratch_t &db, const std::string &dir, const std::string &desc, const std::string &files = owning_files) {
    db.write(dir + "/desc", desc);
    db.write(dir + "/files", files);
  }

  const std::string boot = "6c1b5f3e-2a4d-4c8e-9f10-0123456789ab";
  const std::string game_target = "0123456789abcdef0123456789abcdef";

  steamos::session_record_t record_of(const std::string &id, std::uint32_t uid, std::uint32_t leader, std::uint64_t started) {
    steamos::session_record_t record;
    record.id = id;
    record.uid = uid;
    record.seat = "seat0";
    record.type = "wayland";
    record.klass = "user";
    record.state = "active";
    record.service = "sddm-autologin";
    record.active = true;
    record.remote = false;
    record.leader = leader;
    record.timestamp_monotonic = started;
    return record;
  }

  /// A host in Game Mode: account 1000's one autologin session on seat0, its target running.
  snapshot_t game_mode_snapshot() {
    snapshot_t snapshot;
    snapshot.boot_id = boot;
    snapshot.uid = 1000;
    snapshot.seat0_active_id = "2";
    snapshot.rows.push_back({"2", 1000, "deck", "seat0", "/org/freedesktop/login1/session/_32"});
    snapshot.records["2"] = record_of("2", 1000, 1234, 5000000);
    snapshot.leader_start_ticks = 777;
    snapshot.target = {true, "active", game_target, {}, {}, {}, {}, ""};
    snapshot.game_mode = {true, "active", "fedcba9876543210fedcba9876543210", {}, {}, {}, {}, ""};
    snapshot.polaris = {true, "active", "00112233445566778899aabbccddeeff", {}, {}, {}, {}, ""};
    return snapshot;
  }

  steamos::verdict_t bind(const snapshot_t &snapshot, session_identity_t *out = nullptr) {
    session_identity_t identity;
    auto verdict = steamos::bind_session(snapshot, identity);
    if (out) *out = identity;
    return verdict;
  }
}  // namespace

TEST(SteamosSessionTests, PackageVersionsParseStrictlyAsPacmanWritesThem) {
  const auto plain = steamos::parse_package_version("26.4.1-1");
  ASSERT_TRUE(plain);
  EXPECT_EQ(plain->epoch, 0u);
  EXPECT_EQ(plain->pkgver, "26.4.1");
  EXPECT_EQ(plain->pkgrel, "1");
  const auto epoch = steamos::parse_package_version("2:26.4.1-3.1");
  ASSERT_TRUE(epoch);
  EXPECT_EQ(epoch->epoch, 2u);
  EXPECT_EQ(epoch->pkgrel, "3.1");
  for (const auto *bad : {"26.4.1", "26.4.1-", "-1", "26.4.1-1.2.3", "26.4.1-a", "x:26.4.1-1", ":26.4.1-1", "26.4-1-1",
                          "26.4.1 -1", "26.4.1-1\n", "1234567890:26.4.1-1", "26.4.1-.1"}) {
    EXPECT_FALSE(steamos::parse_package_version(bad)) << bad;
  }
}

TEST(SteamosSessionTests, TheInstalledRecordMustBeUniqueSelfNamingAuditedAndOwnTheCommand) {
  {
    scratch_t db;
    install(db, "steamos-manager-26.4.1-1", desc_text("steamos-manager", "26.4.1-1"));
    // Another package whose name merely starts with ours is not a second record.
    install(db, "steamos-manager-git-26.5-1", desc_text("steamos-manager-git", "26.5-1"));
    steamos::package_record_t record;
    ASSERT_TRUE(steamos::read_installed_package(db.root, "steamos-manager", record).ok());
    EXPECT_EQ(record.version.pkgver, "26.4.1");
    EXPECT_TRUE(steamos::package_supported(record).ok());
  }
  struct case_t {
    std::string name;
    std::function<void(const scratch_t &)> lay_out;
    std::string code;
  };
  const std::vector<case_t> cases {
    {"not installed", [](const scratch_t &) {}, "prelaunch_adapter_package_unavailable"},
    {"two records", [](const scratch_t &db) {
       install(db, "steamos-manager-26.4.1-1", desc_text("steamos-manager", "26.4.1-1"));
       install(db, "steamos-manager-26.4.1-2", desc_text("steamos-manager", "26.4.1-2"));
     }, "prelaunch_adapter_package_unavailable"},
    {"directory and desc disagree", [](const scratch_t &db) {
       install(db, "steamos-manager-26.4.1-1", desc_text("steamos-manager", "26.4.2-1"));
     }, "prelaunch_adapter_package_malformed"},
    {"desc names another package", [](const scratch_t &db) {
       install(db, "steamos-manager-26.4.1-1", desc_text("steamos-manager-fork", "26.4.1-1"));
     }, "prelaunch_adapter_package_malformed"},
    {"a repeated version section", [](const scratch_t &db) {
       install(db, "steamos-manager-26.4.1-1", desc_text("steamos-manager", "26.4.1-1") + "%VERSION%\n26.4.1-1\n\n");
     }, "prelaunch_adapter_package_malformed"},
    {"two version values", [](const scratch_t &db) {
       install(db, "steamos-manager-26.4.1-1", "%NAME%\nsteamos-manager\n\n%VERSION%\n26.4.1-1\n26.4.2-1\n\n");
     }, "prelaunch_adapter_package_malformed"},
    {"no files section", [](const scratch_t &db) {
       install(db, "steamos-manager-26.4.1-1", desc_text("steamos-manager", "26.4.1-1"), "%BACKUP%\netc/x\n\n");
     }, "prelaunch_adapter_package_malformed"},
    {"no desc", [](const scratch_t &db) {
       db.write("steamos-manager-26.4.1-1/files", owning_files);
     }, "prelaunch_adapter_package_unreadable"},
    {"an unaudited release", [](const scratch_t &db) {
       install(db, "steamos-manager-26.4.2-1", desc_text("steamos-manager", "26.4.2-1"));
     }, "prelaunch_adapter_version_unaudited"},
    {"an epoch on the audited version", [](const scratch_t &db) {
       install(db, "steamos-manager-1:26.4.1-1", desc_text("steamos-manager", "1:26.4.1-1"));
     }, "prelaunch_adapter_version_unaudited"},
    {"the package does not own steamosctl", [](const scratch_t &db) {
       install(db, "steamos-manager-26.4.1-1", desc_text("steamos-manager", "26.4.1-1"), "%FILES%\nusr/lib/steamos-manager\n\n");
     }, "prelaunch_adapter_package_malformed"},
    {"the package does not own the manager", [](const scratch_t &db) {
       install(db, "steamos-manager-26.4.1-1", desc_text("steamos-manager", "26.4.1-1"), "%FILES%\nusr/bin/steamosctl\n\n");
     }, "prelaunch_adapter_package_malformed"},
  };
  for (const auto &c : cases) {
    scratch_t db;
    c.lay_out(db);
    steamos::package_record_t record;
    auto verdict = steamos::read_installed_package(db.root, "steamos-manager", record);
    if (verdict.ok()) verdict = steamos::package_supported(record);
    EXPECT_EQ(verdict.code, c.code) << c.name;
    EXPECT_FALSE(verdict.reason.empty()) << c.name;
  }
  steamos::package_record_t record;
  EXPECT_EQ(steamos::read_installed_package("/nonexistent/polaris-test/pacman", "steamos-manager", record).code,
    "prelaunch_adapter_package_unreadable");
}

#ifndef _WIN32
TEST(SteamosSessionTests, OnlyAnOwnedUnwritableExecutableUnderOwnedDirectoriesIsTrusted) {
  scratch_t scratch;
  const auto me = static_cast<std::uint32_t>(::getuid());
  const auto root = scratch.root / "root";
  const auto bin = root / "usr" / "bin";
  const auto command = scratch.write("root/usr/bin/steamosctl", "#!/bin/sh\nexit 0\n");
  for (const auto &dir : {root, root / "usr", bin}) ::chmod(dir.c_str(), 0755);
  ::chmod(command.c_str(), 0755);
  ASSERT_TRUE(steamos::executable_trusted(command, me, root).ok());

  EXPECT_EQ(steamos::executable_trusted(command, me + 1, root).code, "prelaunch_command_untrusted") << "another owner";
  EXPECT_EQ(steamos::executable_trusted(command, me).code, "prelaunch_command_untrusted") << "the scratch root is not trusted";
  EXPECT_EQ(steamos::executable_trusted("usr/bin/steamosctl", me, root).code, "prelaunch_command_untrusted") << "relative";
  EXPECT_EQ(steamos::executable_trusted(bin / "missing", me, root).code, "prelaunch_command_unavailable");
  for (const mode_t mode : {0775, 0757, 0644}) {
    ::chmod(command.c_str(), mode);
    EXPECT_EQ(steamos::executable_trusted(command, me, root).code, "prelaunch_command_untrusted") << std::oct << mode;
  }
  ::chmod(command.c_str(), 0755);
  ::chmod(bin.c_str(), 0775);
  EXPECT_EQ(steamos::executable_trusted(command, me, root).code, "prelaunch_command_untrusted") << "a group-writable directory";
  ::chmod(bin.c_str(), 0755);
  const auto link = bin / "linked";
  std::filesystem::create_symlink(command, link);
  EXPECT_EQ(steamos::executable_trusted(link, me, root).code, "prelaunch_command_untrusted") << "a symlink is not the file";
  // The production default: a root-owned system tool is trusted from "/"; a file here is not.
  EXPECT_TRUE(steamos::executable_trusted("/usr/bin/env", 0).ok());
  EXPECT_EQ(steamos::executable_trusted(command, 0).code, "prelaunch_command_untrusted");
}

TEST(SteamosSessionTests, TheUserBusMustBeThisAccountsPrivateSocket) {
  scratch_t scratch;
  const auto me = static_cast<std::uint32_t>(::getuid());
  const auto runtime = scratch.root / "r";
  std::filesystem::create_directories(runtime);
  ::chmod(runtime.c_str(), 0700);
  EXPECT_FALSE(steamos::user_bus_trusted(runtime, me).ok()) << "no socket";
  scratch.write("r/bus", "");
  EXPECT_FALSE(steamos::user_bus_trusted(runtime, me).ok()) << "a regular file is not a bus";
  std::filesystem::remove(runtime / "bus");
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_un address {};
  address.sun_family = AF_UNIX;
  const auto path = (runtime / "bus").string();
  ASSERT_LT(path.size(), sizeof(address.sun_path));
  std::copy(path.begin(), path.end(), address.sun_path);
  ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
  EXPECT_TRUE(steamos::user_bus_trusted(runtime, me).ok());
  EXPECT_FALSE(steamos::user_bus_trusted(runtime, me + 1).ok()) << "another account's runtime directory";
  ::chmod(runtime.c_str(), 0750);
  EXPECT_FALSE(steamos::user_bus_trusted(runtime, me).ok()) << "a directory others can enter";
  ::close(fd);
}
#endif

TEST(SteamosSessionTests, TheOneLocalAutologinGraphicalSessionIsBoundWithItsGeneration) {
  session_identity_t identity;
  ASSERT_TRUE(bind(game_mode_snapshot(), &identity).ok());
  EXPECT_EQ(identity.boot_id, boot);
  EXPECT_EQ(identity.logind_id, "2");
  EXPECT_EQ(identity.logind_started, 5000000u);
  EXPECT_EQ(identity.uid, 1000u);
  EXPECT_EQ(identity.leader_pid, 1234u);
  EXPECT_EQ(identity.leader_start_ticks, 777u);
  EXPECT_EQ(identity.target_invocation, game_target);
  EXPECT_EQ(identity.mode, mode_e::game);

  for (const auto *state : {"inactive", "failed"}) {
    auto desktop = game_mode_snapshot();
    desktop.game_mode.active_state = state;
    ASSERT_TRUE(bind(desktop, &identity).ok()) << state;
    EXPECT_EQ(identity.mode, mode_e::desktop) << state;
  }
  auto unloaded = game_mode_snapshot();
  unloaded.game_mode = {};
  ASSERT_TRUE(bind(unloaded, &identity).ok());
  EXPECT_EQ(identity.mode, mode_e::desktop) << "no Game Mode unit loaded";

  // A non-graphical session of the same account (an ssh login) does not make it ambiguous.
  auto with_ssh = game_mode_snapshot();
  with_ssh.rows.push_back({"7", 1000, "deck", "", "/org/freedesktop/login1/session/_37"});
  auto ssh = record_of("7", 1000, 4000, 9000000);
  ssh.type = "tty";
  ssh.seat = "";
  ssh.remote = true;
  ssh.service = "sshd";
  with_ssh.records["7"] = ssh;
  EXPECT_TRUE(bind(with_ssh).ok());
}

TEST(SteamosSessionTests, AnythingButOneProvableSessionIsRefusedWithAFixedCode) {
  struct case_t {
    std::string name;
    std::function<void(snapshot_t &)> change;
    std::string code;
  };
  const std::vector<case_t> cases {
    {"boot identity unread", [](auto &s) { s.boot_id.clear(); }, "prelaunch_session_identity_unavailable"},
    {"no active session on seat0", [](auto &s) { s.seat0_active_id.clear(); }, "prelaunch_session_identity_unavailable"},
    {"the active session is not listed", [](auto &s) { s.seat0_active_id = "9"; }, "prelaunch_session_identity_unavailable"},
    {"the active session belongs to another account", [](auto &s) {
       s.rows.push_back({"3", 1001, "guest", "seat0", "/org/freedesktop/login1/session/_33"});
       s.records["3"] = record_of("3", 1001, 2000, 6000000);
       s.seat0_active_id = "3";
     }, "prelaunch_session_ambiguous"},
    {"a second graphical session of this account", [](auto &s) {
       s.rows.push_back({"4", 1000, "deck", "seat0", "/org/freedesktop/login1/session/_34"});
       auto other = record_of("4", 1000, 2100, 6100000);
       other.active = false;
       other.state = "online";
       s.records["4"] = other;
     }, "prelaunch_session_ambiguous"},
    {"a session of this account that could not be read", [](auto &s) {
       s.rows.push_back({"5", 1000, "deck", "seat0", "/org/freedesktop/login1/session/_35"});
     }, "prelaunch_session_identity_unavailable"},
    {"a greeter, not a user session", [](auto &s) { s.records["2"].klass = "greeter"; }, "prelaunch_session_ambiguous"},
    {"not graphical", [](auto &s) { s.records["2"].type = "tty"; }, "prelaunch_session_ambiguous"},
    {"remote", [](auto &s) { s.records["2"].remote = true; }, "prelaunch_session_ambiguous"},
    {"inactive", [](auto &s) { s.records["2"].active = false; }, "prelaunch_session_ambiguous"},
    {"closing", [](auto &s) { s.records["2"].state = "closing"; }, "prelaunch_session_ambiguous"},
    {"another seat", [](auto &s) { s.records["2"].seat = "seat1"; }, "prelaunch_session_ambiguous"},
    {"the record names another account", [](auto &s) { s.records["2"].uid = 1001; }, "prelaunch_session_ambiguous"},
    {"a manual SDDM login", [](auto &s) { s.records["2"].service = "sddm"; }, "prelaunch_session_not_autologin"},
    {"no service recorded", [](auto &s) { s.records["2"].service.clear(); }, "prelaunch_session_not_autologin"},
    {"the leader's start time unread", [](auto &s) { s.leader_start_ticks = 0; }, "prelaunch_session_identity_unavailable"},
    {"no leader", [](auto &s) { s.records["2"].leader = 0; }, "prelaunch_session_identity_unavailable"},
    {"no start timestamp", [](auto &s) { s.records["2"].timestamp_monotonic = 0; }, "prelaunch_session_identity_unavailable"},
    {"the target is not active", [](auto &s) { s.target.active_state = "deactivating"; }, "prelaunch_session_identity_unavailable"},
    {"the target has no invocation", [](auto &s) { s.target.invocation.clear(); }, "prelaunch_session_identity_unavailable"},
    {"the target is not loaded", [](auto &s) { s.target = {}; }, "prelaunch_session_identity_unavailable"},
    {"Game Mode starting", [](auto &s) { s.game_mode.active_state = "activating"; }, "prelaunch_session_mode_unknown"},
    {"Game Mode stopping", [](auto &s) { s.game_mode.active_state = "deactivating"; }, "prelaunch_session_mode_unknown"},
    {"Game Mode reloading", [](auto &s) { s.game_mode.active_state = "reloading"; }, "prelaunch_session_mode_unknown"},
  };
  for (const auto &c : cases) {
    auto snapshot = game_mode_snapshot();
    c.change(snapshot);
    session_identity_t identity;
    identity.logind_id = "untouched";
    const auto verdict = steamos::bind_session(snapshot, identity);
    EXPECT_EQ(verdict.code, c.code) << c.name;
    EXPECT_FALSE(verdict.reason.empty()) << c.name;
    EXPECT_EQ(identity.logind_id, "untouched") << c.name << ": nothing is bound on a refusal";
  }
}

TEST(SteamosSessionTests, ASessionRetiresOnlyWhenItAndItsTargetGenerationHaveBothEnded) {
  session_identity_t pre;
  ASSERT_TRUE(bind(game_mode_snapshot(), &pre).ok());
  const auto new_target = std::string {"ffffffffffffffffffffffffffffffff"};
  struct case_t {
    std::string name;
    std::function<void(snapshot_t &)> change;
    bool retired;
  };
  const std::vector<case_t> cases {
    {"unchanged", [](auto &) {}, false},
    {"the session closes but the target generation runs on", [](auto &s) { s.records["2"].state = "closing"; }, false},
    {"a new target generation while the old session stays", [&](auto &s) { s.target.invocation = new_target; }, false},
    {"listed but unreadable is not proof", [&](auto &s) {
       s.records.erase("2");
       s.target.invocation = new_target;
     }, false},
    {"closing, and the target stopped", [](auto &s) {
       s.records["2"].state = "closing";
       s.target.active_state = "inactive";
     }, true},
    {"gone, and a new target generation", [&](auto &s) {
       s.rows.clear();
       s.records.clear();
       s.target.invocation = new_target;
     }, true},
    {"its id reused by a later session, and a new target generation", [&](auto &s) {
       s.records["2"].timestamp_monotonic = 9000000;
       s.target.invocation = new_target;
     }, true},
    {"the host rebooted", [](auto &s) { s.boot_id = "00000000-0000-0000-0000-000000000000"; }, true},
  };
  for (const auto &c : cases) {
    auto snapshot = game_mode_snapshot();
    c.change(snapshot);
    EXPECT_EQ(steamos::retired(snapshot, pre), c.retired) << c.name;
  }
}

TEST(SteamosSessionTests, IdentityEqualityCatchesAReusedPidARestartedTargetAndAModeChange) {
  session_identity_t pre;
  ASSERT_TRUE(bind(game_mode_snapshot(), &pre).ok());
  const std::vector<std::function<void(session_identity_t &)>> changes {
    [](auto &i) { i.leader_start_ticks += 1; },
    [](auto &i) { i.leader_pid += 1; },
    [](auto &i) { i.target_invocation[0] = 'f'; },
    [](auto &i) { i.logind_started += 1; },
    [](auto &i) { i.logind_id = "3"; },
    [](auto &i) { i.boot_id[0] = 'f'; },
    [](auto &i) { i.uid += 1; },
    [](auto &i) { i.mode = mode_e::desktop; },
  };
  for (std::size_t index = 0; index < changes.size(); ++index) {
    auto changed = pre;
    changes[index](changed);
    EXPECT_FALSE(changed == pre) << index;
  }
  auto incomplete = pre;
  incomplete.target_invocation.clear();
  EXPECT_FALSE(incomplete.valid());
}

TEST(SteamosSessionTests, TheCommandIsTheFixedPathAndOneVerbWithAMinimalEnvironment) {
  EXPECT_EQ(steamos::command_argv(steamos::switch_e::to_desktop),
    (std::vector<std::string> {"/usr/bin/steamosctl", "switch-to-desktop-mode"}));
  EXPECT_EQ(steamos::command_argv(steamos::switch_e::to_game_mode),
    (std::vector<std::string> {"/usr/bin/steamosctl", "switch-to-game-mode"}));
  // The account's own user bus by its fixed path, never an inherited address, display or home.
  EXPECT_EQ(steamos::command_env(1000),
    (std::vector<std::string> {"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus", "PATH=/usr/bin:/bin", "LANG=C"}));
  EXPECT_EQ(steamos::user_bus_address(1001), "unix:path=/run/user/1001/bus");
  EXPECT_EQ(std::string {steamos::k_system_bus_address}, "unix:path=/run/dbus/system_bus_socket");
}

TEST(SteamosSessionTests, PolarisSurvivesOnlyAsThisActiveUnitOutsideTheProvenStopClosure) {
  const auto self = std::string {"0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n"};
  const auto surviving = [&]() {
    auto snapshot = game_mode_snapshot();
    snapshot.self_cgroup = self;
    snapshot.polaris.control_group = "/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service";
    snapshot.stop_closure = {true, {"gamescope-session.service", "graphical-session.target", "steam.service"}};
    return snapshot;
  };
  EXPECT_TRUE(steamos::polaris_independent_of_session(surviving()).ok());
  struct case_t {
    std::string name;
    std::function<void(snapshot_t &)> change;
  };
  const std::vector<case_t> cases {
    {"not loaded", [](auto &s) { s.polaris = {}; }},
    {"loaded but inactive (a stale unit)", [](auto &s) { s.polaris.active_state = "inactive"; }},
    {"another process's unit", [](auto &s) { s.self_cgroup = "0::/user.slice/user-1000.slice/session-2.scope\n"; }},
    {"this process's cgroup unread", [](auto &s) { s.self_cgroup.clear(); }},
    {"a cgroup v1 host", [](auto &s) { s.self_cgroup = "1:name=systemd:/x\n0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n"; }},
    {"PartOf the target", [](auto &s) { s.polaris.part_of = {"graphical-session.target"}; }},
    {"BindsTo Game Mode", [](auto &s) { s.polaris.binds_to = {"gamescope-session.service"}; }},
    {"Requisite of the pre target", [](auto &s) { s.polaris.requisite = {"graphical-session-pre.target"}; }},
    {"stop propagated from the target", [](auto &s) { s.polaris.stop_propagated_from = {"graphical-session.target"}; }},
    {"reached indirectly by the stop", [](auto &s) { s.stop_closure.units.insert(s.stop_closure.units.begin() + 1, "polaris.service"); }},
    {"the closure was not walked or overflowed", [](auto &s) { s.stop_closure.complete = false; }},
  };
  for (const auto &c : cases) {
    auto snapshot = surviving();
    c.change(snapshot);
    EXPECT_EQ(steamos::polaris_independent_of_session(snapshot).code, "prelaunch_service_not_independent") << c.name;
  }
}

TEST(SteamosSessionTests, GameModeIsReadFromItsUnitAndAnythingInBetweenIsTransitional) {
  using steamos::game_mode_e;
  EXPECT_EQ(steamos::game_mode_of({}), game_mode_e::inactive) << "not loaded";
  for (const auto *state : {"inactive", "failed"}) EXPECT_EQ(steamos::game_mode_of({true, state, "", {}, {}, {}, {}, ""}), game_mode_e::inactive);
  EXPECT_EQ(steamos::game_mode_of({true, "active", "", {}, {}, {}, {}, ""}), game_mode_e::active);
  for (const auto *state : {"activating", "deactivating", "reloading", "maintenance", ""}) {
    EXPECT_EQ(steamos::game_mode_of({true, state, "", {}, {}, {}, {}, ""}), game_mode_e::transitional) << state;
  }
}

TEST(SteamosSessionTests, BusOwnersMustSitInTheCgroupsSystemdGaveThem) {
  EXPECT_TRUE(steamos::cgroup_is_logind("0::/system.slice/systemd-logind.service\n"));
  EXPECT_FALSE(steamos::cgroup_is_logind("0::/user.slice/user-1000.slice/user@1000.service/app.slice/systemd-logind.service\n"));
  EXPECT_FALSE(steamos::cgroup_is_logind("0::/system.slice/systemd-logind.service/extra\n"));
  EXPECT_TRUE(steamos::cgroup_is_user_manager("0::/user.slice/user-1000.slice/user@1000.service/init.scope\n", 1000));
  EXPECT_FALSE(steamos::cgroup_is_user_manager("0::/user.slice/user-1001.slice/user@1001.service/init.scope\n", 1000));
  EXPECT_FALSE(steamos::cgroup_is_user_manager("0::/user.slice/user-1000.slice/user@1000.service/app.slice/fake.service\n", 1000));
  EXPECT_EQ(steamos::cgroup_v2_path("0::/a/b\n"), "/a/b");
  EXPECT_EQ(steamos::cgroup_v2_path("0::relative\n"), "");
  EXPECT_EQ(steamos::cgroup_v2_path("0::/a\n0::/b\n"), "");
}

TEST(SteamosSessionTests, TheManagerProcessMustBeThisAccountsUnitBySystemdsCgroup) {
  EXPECT_TRUE(steamos::cgroup_is_manager_unit("0::/user.slice/user-1000.slice/user@1000.service/app.slice/steamos-manager.service\n", 1000));
  EXPECT_FALSE(steamos::cgroup_is_manager_unit("0::/user.slice/user-1001.slice/user@1001.service/app.slice/steamos-manager.service\n", 1000));
  EXPECT_FALSE(steamos::cgroup_is_manager_unit("0::/user.slice/user-1000.slice/session-2.scope\n", 1000)) << "started inside a login session";
  EXPECT_FALSE(steamos::cgroup_is_manager_unit("0::/system.slice/steamos-manager.service\n", 1000)) << "the root manager is not this account's";
  EXPECT_FALSE(steamos::cgroup_is_manager_unit("0::/user.slice/user-1000.slice/user@1000.service/app.slice/fake-steamos-manager.service.d\n", 1000));
  EXPECT_FALSE(steamos::cgroup_is_manager_unit(
    "1:name=systemd:/x\n0::/user.slice/user-1000.slice/user@1000.service/app.slice/steamos-manager.service\n", 1000)) << "cgroup v2 only";
  EXPECT_FALSE(steamos::cgroup_is_manager_unit("", 1000));
}

TEST(SteamosSessionTests, InvocationIdsAndProcessStartTimesAreReadExactly) {
  std::vector<std::uint8_t> bytes(16);
  for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 17);
  EXPECT_EQ(steamos::invocation_hex(bytes), "00112233445566778899aabbccddeeff");
  EXPECT_EQ(steamos::invocation_hex(std::vector<std::uint8_t>(15, 1)), "");
  EXPECT_EQ(steamos::invocation_hex(std::vector<std::uint8_t>(16, 0)), "") << "all zeros is no invocation";

  scratch_t proc;
  std::string stat = "1234 (x) (y z) S 1";
  for (int field = 5; field <= 21; ++field) stat += " 0";
  proc.write("1234/stat", stat + " 987654 0 0\n");
  EXPECT_EQ(steamos::process_start_ticks(proc.root, 1234), 987654u) << "past a comm holding ') ('";
  proc.write("1235/stat", "1235 (short) S 1 0\n");
  EXPECT_FALSE(steamos::process_start_ticks(proc.root, 1235)) << "truncated";
  EXPECT_FALSE(steamos::process_start_ticks(proc.root, 999));
  EXPECT_FALSE(steamos::process_start_ticks(proc.root, 1)) << "never pid 1";
  proc.write("1236/stat", std::string(5000, 'x'));
  EXPECT_FALSE(steamos::process_start_ticks(proc.root, 1236)) << "oversized is refused, not truncated";
}

TEST(SteamosSessionTests, RegisteredCasesAreFrozen) {
  // A case that is renamed or dropped must be noticed here, not silently stop running.
  const std::set<std::string> expected {
    "PackageVersionsParseStrictlyAsPacmanWritesThem",
    "TheInstalledRecordMustBeUniqueSelfNamingAuditedAndOwnTheCommand",
#ifndef _WIN32
    "OnlyAnOwnedUnwritableExecutableUnderOwnedDirectoriesIsTrusted",
    "TheUserBusMustBeThisAccountsPrivateSocket",
#endif
    "TheOneLocalAutologinGraphicalSessionIsBoundWithItsGeneration",
    "AnythingButOneProvableSessionIsRefusedWithAFixedCode",
    "ASessionRetiresOnlyWhenItAndItsTargetGenerationHaveBothEnded",
    "IdentityEqualityCatchesAReusedPidARestartedTargetAndAModeChange",
    "TheCommandIsTheFixedPathAndOneVerbWithAMinimalEnvironment",
    "PolarisSurvivesOnlyAsThisActiveUnitOutsideTheProvenStopClosure",
    "GameModeIsReadFromItsUnitAndAnythingInBetweenIsTransitional",
    "BusOwnersMustSitInTheCgroupsSystemdGaveThem",
    "TheManagerProcessMustBeThisAccountsUnitBySystemdsCgroup",
    "InvocationIdsAndProcessStartTimesAreReadExactly",
    "RegisteredCasesAreFrozen",
  };
  std::set<std::string> registered;
  const auto *unit = ::testing::UnitTest::GetInstance();
  for (int suite = 0; suite < unit->total_test_suite_count(); ++suite) {
    const auto *info = unit->GetTestSuite(suite);
    if (std::string {info->name()} != "SteamosSessionTests") continue;
    for (int test = 0; test < info->total_test_count(); ++test) registered.insert(info->GetTestInfo(test)->name());
  }
  EXPECT_EQ(registered, expected);
}
