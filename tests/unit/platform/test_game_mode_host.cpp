/**
 * @file tests/unit/platform/test_game_mode_host.cpp
 * @brief Test gamescope Steam session (Game Mode) host detection and the guidance built on it.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include <src/platform/linux/game_mode_host.h>
#include <src/platform/linux/user_unit_override.h>
#include <src/utility.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace std::literals;
namespace fs = std::filesystem;
namespace gm = platf::game_mode_host;

namespace {
  struct scratch_t {
    fs::path root;

    scratch_t() {
      auto pattern = (fs::temp_directory_path() / "polaris-game-mode-host-XXXXXX").string();
      const char *made = mkdtemp(pattern.data());
      if (made == nullptr) {
        throw std::runtime_error("mkdtemp failed");
      }
      root = made;
    }

    ~scratch_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    fs::path file(const std::string &relative, const std::string &content, bool executable = false) const {
      const auto path = root / relative;
      fs::create_directories(path.parent_path());
      {
        std::ofstream out(path, std::ios::binary);
        out << content;
      }
      fs::permissions(
        path,
        executable ? fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec :
                     fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
        fs::perm_options::replace
      );
      return path;
    }

    fs::path dir(const std::string &relative) const {
      const auto path = root / relative;
      fs::create_directories(path);
      return path;
    }

    /// A fake /proc entry owned by the test account: cmdline is NUL separated.
    void process(unsigned pid, const std::vector<std::string> &argv) const {
      std::string cmdline;
      for (const auto &arg : argv) {
        cmdline += arg;
        cmdline.push_back('\0');
      }
      file("proc/" + std::to_string(pid) + "/cmdline", cmdline);
    }

    /// The scratch /proc entries are owned by whoever runs the test, so that uid is "this account".
    gm::probe_t probe(uid_t uid = ::getuid()) const {
      gm::probe_t probe;
      probe.session_dirs = {root / "sessions", root / "sessions-local"};
      probe.path_dirs = {root / "bin"};
      probe.os_release = root / "os-release";
      probe.proc_root = root / "proc";
      probe.uid = uid;
      return probe;
    }
  };

  bool mentions(const std::vector<std::string> &evidence, std::string_view needle) {
    for (const auto &item : evidence) {
      if (item.find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
}  // namespace

TEST(GameModeHostTests, PlainDesktopHostIsNotDetected) {
  scratch_t scratch;
  scratch.file("sessions/plasma.desktop", "[Desktop Entry]\nName=Plasma (Wayland)\nExec=/usr/bin/startplasma-wayland\nDesktopNames=KDE\n");
  scratch.file("bin/gamescope", "#!/bin/sh\n", true);
  scratch.file("os-release", "NAME=\"Fedora Linux\"\nID=fedora\nVARIANT_ID=kde\n");
  scratch.process(400, {"/usr/bin/plasmashell"});

  const auto detection = gm::detect(scratch.probe());
  EXPECT_FALSE(detection.installed);
  EXPECT_FALSE(detection.session_active);
  EXPECT_TRUE(detection.evidence.empty());
}

TEST(GameModeHostTests, ChimeraStyleSessionEntryCountsWithItsExecLine) {
  scratch_t scratch;
  scratch.file("sessions/plasma.desktop", "[Desktop Entry]\nExec=/usr/bin/startplasma-wayland\n");
  scratch.file("sessions/gamescope-session-steam.desktop", "[Desktop Entry]\nName=Steam Big Picture\nExec=gamescope-session-plus steam\nType=Application\nDesktopNames=gamescope\n");

  const auto detection = gm::detect(scratch.probe());
  EXPECT_TRUE(detection.installed);
  EXPECT_FALSE(detection.session_active);
  ASSERT_EQ(detection.evidence.size(), 1U);
  EXPECT_TRUE(mentions(detection.evidence, "gamescope-session-steam.desktop"));
  EXPECT_TRUE(mentions(detection.evidence, "Exec=gamescope-session-plus steam"));
}

TEST(GameModeHostTests, ExecLinesSurviveEnvPrefixesQuotesAndSteamOsNaming) {
  scratch_t scratch;
  scratch.file("sessions-local/env-flags.desktop", "[Desktop Entry]\nExec=env -u FOO BAR=1 /usr/bin/gamescope-session\n");
  scratch.file("sessions-local/quoted.desktop", "[Desktop Entry]\nExec=\"/usr/bin/gamescope-session-plus\" steam\n");
  // SteamOS 3.7+ ships this Exec; the tool name still contains gamescope-session.
  scratch.file("sessions-local/gamescope-wayland.desktop", "[Desktop Entry]\nName=Gamescope\nExec=start-gamescope-session\n");
  // DesktopNames alone is enough when the Exec is unrecognisable.
  scratch.file("sessions-local/wrapper.desktop", "[Desktop Entry]\nExec=/opt/vendor/launch-steam\nDesktopNames=gamescope\n");
  // Polaris' own launcher is not a Game Mode session.
  scratch.file("sessions-local/polaris.desktop", "[Desktop Entry]\nExec=polaris-gamescope-session steam\n");

  const auto detection = gm::detect(scratch.probe());
  EXPECT_TRUE(detection.installed);
  ASSERT_EQ(detection.evidence.size(), 4U);
  EXPECT_TRUE(mentions(detection.evidence, "env-flags.desktop"));
  EXPECT_TRUE(mentions(detection.evidence, "quoted.desktop"));
  EXPECT_TRUE(mentions(detection.evidence, "gamescope-wayland.desktop"));
  EXPECT_TRUE(mentions(detection.evidence, "wrapper.desktop (DesktopNames=gamescope)"));
  EXPECT_FALSE(mentions(detection.evidence, "polaris.desktop"));
}

TEST(GameModeHostTests, SessionToolsOnPathCountOnceEachAndMustBeExecutable) {
  scratch_t scratch;
  scratch.file("bin/steamos-session-select", "#!/bin/sh\n", true);
  scratch.file("bin/gamescope-session-plus", "#!/bin/sh\n", false);

  const auto detection = gm::detect(scratch.probe());
  EXPECT_TRUE(detection.installed);
  ASSERT_EQ(detection.evidence.size(), 1U);
  EXPECT_TRUE(mentions(detection.evidence, "steamos-session-select on PATH"));
  EXPECT_FALSE(mentions(detection.evidence, "gamescope-session-plus"));
}

TEST(GameModeHostTests, OsReleaseIdentifiesSteamOsAndDeckVariants) {
  scratch_t steamos;
  steamos.file("os-release", "NAME=\"SteamOS\"\nID=steamos\nVARIANT_ID=steamdeck\n");
  const auto deck = gm::detect(steamos.probe());
  EXPECT_TRUE(deck.installed);
  EXPECT_TRUE(mentions(deck.evidence, "os-release ID=steamos"));
  EXPECT_TRUE(mentions(deck.evidence, "os-release VARIANT_ID=steamdeck"));

  scratch_t bazzite;
  bazzite.file("os-release", "ID=bazzite\nVARIANT_ID=\"bazzite-deck\"\n");
  const auto image = gm::detect(bazzite.probe());
  EXPECT_TRUE(image.installed);
  ASSERT_EQ(image.evidence.size(), 1U);
  EXPECT_TRUE(mentions(image.evidence, "os-release VARIANT_ID=bazzite-deck"));
}

TEST(GameModeHostTests, LiveSessionCountsAsInstalledAndOnlyForThisAccount) {
  scratch_t scratch;
  scratch.process(500, {"/bin/bash", "/usr/bin/gamescope-session-plus", "steam"});
  const auto shell_hosted = gm::detect(scratch.probe());
  EXPECT_TRUE(shell_hosted.session_active);
  EXPECT_TRUE(shell_hosted.installed) << "a running session is the strongest install signal, even from a prefix nothing else probes";
  EXPECT_TRUE(mentions(shell_hosted.evidence, "gamescope-session-plus running as this account (pid 500)"));

  const auto other_account = gm::detect(scratch.probe(::getuid() + 1));
  EXPECT_FALSE(other_account.session_active);
  EXPECT_FALSE(other_account.installed);

  scratch_t direct;
  direct.process(20, {"/usr/bin/gamescope-session"});
  EXPECT_TRUE(gm::detect(direct.probe()).session_active);
}

TEST(GameModeHostTests, LookalikeProcessesDoNotCountAsALiveSession) {
  scratch_t scratch;
  scratch.process(30, {"grep", "gamescope-session"});
  scratch.process(31, {"/usr/bin/polaris-gamescope-session", "steam"});
  scratch.process(32, {"/usr/local/bin/polaris-gamescope-idle"});
  scratch.process(33, {"/usr/bin/gamescope", "--steam", "--", "steam"});
  scratch.file("proc/not-a-pid/cmdline", "/usr/bin/gamescope-session");

  const auto detection = gm::detect(scratch.probe());
  EXPECT_FALSE(detection.session_active);
  EXPECT_FALSE(detection.installed);
  EXPECT_TRUE(detection.evidence.empty());
}

TEST(GameModeHostTests, EvidenceOrderIsStableAcrossSignalsAndTheHeadlineCountsTheRest) {
  scratch_t scratch;
  scratch.file("sessions/gamescope-session-steam.desktop", "[Desktop Entry]\nExec=gamescope-session-plus steam\n");
  scratch.file("bin/steamos-session-select", "#!/bin/sh\n", true);
  scratch.file("os-release", "ID=steamos\n");
  scratch.process(77, {"/bin/bash", "/usr/bin/gamescope-session-plus", "steam"});

  const auto detection = gm::detect(scratch.probe());
  ASSERT_EQ(detection.evidence.size(), 4U);
  EXPECT_NE(detection.evidence[0].find("session entry"), std::string::npos);
  EXPECT_NE(detection.evidence[1].find("on PATH"), std::string::npos);
  EXPECT_NE(detection.evidence[2].find("os-release"), std::string::npos);
  EXPECT_NE(detection.evidence[3].find("running as this account"), std::string::npos);
  EXPECT_EQ(gm::headline_evidence(detection), detection.evidence[0] + " and 3 more signals");

  gm::detection_t two;
  two.evidence = {"a", "b"};
  EXPECT_EQ(gm::headline_evidence(two), "a and 1 more signal");
  EXPECT_EQ(gm::headline_evidence(gm::detection_t {}), "no evidence recorded");
}

TEST(GameModeHostTests, DefaultBootPathsReadWhereTheHeadlessBootWriterWrites) {
  const auto paths = gm::default_boot_paths("deck", "/srv/deck");
  EXPECT_EQ(paths.linger_dir, fs::path("/var/lib/systemd/linger"));
  EXPECT_EQ(paths.user, "deck");
  EXPECT_EQ(paths.config_home, fs::path("/srv/deck/.config"));
  EXPECT_EQ(paths.system_wants_dir, fs::path("/etc/systemd/user/default.target.wants"));
  EXPECT_TRUE(gm::default_boot_paths("deck", "").config_home.empty());
}

TEST(GameModeHostTests, BootReadinessNeedsBothLingerAndTheWantLink) {
  scratch_t scratch;
  gm::boot_paths_t paths {scratch.dir("linger"), "deck", scratch.dir("config"), scratch.dir("system-wants")};

  EXPECT_FALSE(gm::boot_readiness(paths).independent());

  scratch.file("linger/deck", "");
  auto readiness = gm::boot_readiness(paths);
  EXPECT_TRUE(readiness.linger_enabled);
  EXPECT_FALSE(readiness.boot_start_linked);
  EXPECT_FALSE(readiness.independent());

  scratch.file("config/systemd/user/default.target.wants/polaris.service", "");
  readiness = gm::boot_readiness(paths);
  EXPECT_TRUE(readiness.independent());

  // A system-wide want counts the same as the account's own link.
  std::error_code ec;
  fs::remove(scratch.root / "config/systemd/user/default.target.wants/polaris.service", ec);
  EXPECT_FALSE(gm::boot_readiness(paths).boot_start_linked);
  scratch.file("system-wants/polaris.service", "");
  EXPECT_TRUE(gm::boot_readiness(paths).independent());
}

TEST(GameModeHostTests, TrayPolicyKeepsOrdinaryMenuAndInteractiveLaunches) {
  scratch_t scratch;
  const gm::boot_paths_t paths {scratch.dir("linger"), "deck", scratch.dir("config"), scratch.dir("system-wants")};
  const auto service = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n"sv;
  EXPECT_FALSE(gm::suppress_tray_for_headless_boot(service, paths)) << "ordinary menu-started service";

  scratch.file("linger/deck", "");
  scratch.file("config/systemd/user/default.target.wants/polaris.service", "");
  for (const auto cgroup : {
         "0::/user.slice/user-1000.slice/session-4.scope\n"sv,
         "0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-dev.polaris@42.service\n"sv,
         "0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service.d\n"sv,
         ""sv,
       }) {
    EXPECT_FALSE(gm::suppress_tray_for_headless_boot(cgroup, paths)) << cgroup;
  }
}

TEST(GameModeHostTests, TrayPolicyRequiresBothHeadlessBootOptIns) {
  scratch_t scratch;
  const gm::boot_paths_t paths {scratch.dir("linger"), "deck", scratch.dir("config"), scratch.dir("system-wants")};
  const auto service = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n"sv;
  scratch.file("linger/deck", "");
  EXPECT_FALSE(gm::suppress_tray_for_headless_boot(service, paths));

  fs::remove(scratch.root / "linger/deck");
  scratch.file("config/systemd/user/default.target.wants/polaris.service", "");
  EXPECT_FALSE(gm::suppress_tray_for_headless_boot(service, paths));

  scratch.file("linger/deck", "");
  EXPECT_TRUE(gm::suppress_tray_for_headless_boot(service, paths));
}

TEST(GameModeHostTests, TrayPolicyCoversAKmsExecStartOverride) {
  scratch_t scratch;
  const gm::boot_paths_t paths {scratch.dir("linger"), "deck", scratch.dir("config"), scratch.dir("system-wants")};
  scratch.file("linger/deck", "");
  scratch.file("config/systemd/user/default.target.wants/polaris.service", "");
  const auto helper = scratch.file("usr/libexec/polaris/polaris-kms", "fixture", true);
  const auto drop_in = scratch.file("config/systemd/user/polaris.service.d/20-polaris-kms.conf",
                                   "[Service]\nExecStart=\nExecStart=" + helper.string() + "\n");
  const auto override = platf::user_unit::effective_exec_override(drop_in.parent_path());
  ASSERT_TRUE(override.active());
  ASSERT_EQ(override.binary, helper);
  ASSERT_FALSE(override.binary_missing);
  EXPECT_TRUE(gm::suppress_tray_for_headless_boot(
    "0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n", paths));
}

TEST(GameModeHostTests, TrayPolicyHonorsTheExistingSystemWideBootWant) {
  scratch_t scratch;
  const gm::boot_paths_t paths {scratch.dir("linger"), "deck", scratch.dir("config"), scratch.dir("system-wants")};
  scratch.file("linger/deck", "");
  scratch.file("system-wants/polaris.service", "");
  EXPECT_TRUE(gm::suppress_tray_for_headless_boot(
    "0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n", paths));
}

TEST(GameModeHostTests, BootReadinessGuidanceNamesGameModeOnlyWhenTheHostHasOne) {
  gm::detection_t plain;
  gm::detection_t game_mode;
  game_mode.installed = true;

  const auto independent = gm::boot_readiness_guidance(game_mode, true);
  EXPECT_EQ(independent.status, "boot_independent");
  EXPECT_EQ(independent.action, "No action needed.");

  const auto bound_game_mode = gm::boot_readiness_guidance(game_mode, false);
  EXPECT_EQ(bound_game_mode.status, "session_bound");
  EXPECT_NE(bound_game_mode.summary.find("Steam Game Mode session"), std::string::npos);
  EXPECT_NE(bound_game_mode.summary.find("goes offline when the host returns to Game Mode"), std::string::npos);
  EXPECT_NE(bound_game_mode.action.find("sudo -H polaris --setup-host --enable-headless-boot"), std::string::npos);

  const auto bound_plain = gm::boot_readiness_guidance(plain, false);
  EXPECT_EQ(bound_plain.status, "session_bound");
  EXPECT_EQ(bound_plain.summary.find("Steam Game Mode session"), std::string::npos);
  EXPECT_NE(bound_plain.action.find("sudo -H polaris --setup-host --enable-headless-boot"), std::string::npos);
}

// polaris#626. Two people in two weeks had a Game Mode host Polaris could be reached on and could
// not stream from: every mode failed, each for its own reason. A Game Mode host has one screen, so a
// stream from it is a stream of that screen, captured from the session's own gamescope.
TEST(GameModeHostTests, OnlyAMirrorOfALiveSessionIsAStreamOfTheGameModeScreen) {
  EXPECT_TRUE(gm::streams_session_screen("desktop_display", false, false, true));
  EXPECT_FALSE(gm::streams_session_screen("desktop_display", false, false, false))
    << "the same host in Desktop Mode mirrors a real desktop";
  EXPECT_FALSE(gm::streams_session_screen("gamescope_stream", false, false, true))
    << "Polaris's own gamescope is a different compositor with its own rules";
  EXPECT_FALSE(gm::streams_session_screen("headless_stream", true, false, true));
  EXPECT_FALSE(gm::streams_session_screen("desktop_display", true, false, true))
    << "a private compositor is never the session's screen";
  EXPECT_FALSE(gm::streams_session_screen("desktop_display", false, true, true));
  EXPECT_FALSE(gm::streams_session_screen("", false, false, true));
}

TEST(GameModeHostTests, TheLiveSessionAnswerCanBePinnedAndReleased) {
  gm::set_session_live_for_tests(true);
  EXPECT_TRUE(gm::session_live());
  gm::set_session_live_for_tests(false);
  EXPECT_FALSE(gm::session_live());
  gm::set_session_live_for_tests(std::nullopt);
  // Released, it is the real scan again, and asking twice gives one answer.
  EXPECT_EQ(gm::session_live(), gm::session_live());
}

TEST(GameModeHostTests, DisplaySessionGuidanceFollowsTheHostKind) {
  gm::detection_t plain;
  gm::detection_t installed;
  installed.installed = true;
  gm::detection_t live;
  live.installed = true;
  live.session_active = true;

  // A live session wins even while the process still carries the desktop's
  // WAYLAND_DISPLAY from before the mode switch.
  const auto running = gm::display_session_guidance(live, true, true, false);
  EXPECT_EQ(running.status, "game_mode_session");
  EXPECT_NE(running.summary.find("shows the Game Mode screen"), std::string::npos);
  EXPECT_EQ(running.summary.find("not supported"), std::string::npos) << "a Game Mode host streams now";
  EXPECT_NE(running.action.find("No action needed"), std::string::npos);
  EXPECT_NE(running.action.find("comes back in Desktop Mode"), std::string::npos);

  const auto wayland = gm::display_session_guidance(installed, false, true, false);
  EXPECT_EQ(wayland.status, "healthy");
  EXPECT_NE(wayland.summary.find("Wayland"), std::string::npos);
  const auto x11 = gm::display_session_guidance(plain, false, false, true);
  EXPECT_EQ(x11.status, "healthy");
  EXPECT_NE(x11.summary.find("X11"), std::string::npos);

  const auto headless = gm::display_session_guidance(plain, true, false, false);
  EXPECT_EQ(headless.status, "missing_display_environment");
  EXPECT_NE(headless.summary.find("expected on a headless-boot host"), std::string::npos);

  const auto game_mode_host = gm::display_session_guidance(installed, false, false, false);
  EXPECT_EQ(game_mode_host.status, "missing_display_environment");
  EXPECT_NE(game_mode_host.summary.find("Game Mode host"), std::string::npos);
  EXPECT_NE(game_mode_host.action.find("--enable-headless-boot"), std::string::npos);

  const auto desktop = gm::display_session_guidance(plain, false, false, false);
  EXPECT_EQ(desktop.status, "missing_display_environment");
  EXPECT_NE(desktop.action.find("Restart Polaris from the desktop session"), std::string::npos);
  EXPECT_EQ(desktop.action.find("--enable-headless-boot"), std::string::npos);
}

TEST(GameModeHostTests, SetupHostAdviceIsSilentOffGameModeHostsAndFollowsWhatTheRunDid) {
  using state_t = gm::setup_host_state_t;
  gm::detection_t plain;
  EXPECT_TRUE(gm::setup_host_advice(plain, state_t::needs_headless_boot, "/usr/bin/polaris").empty());

  gm::detection_t game_mode;
  game_mode.installed = true;
  game_mode.evidence = {"steamos-session-select on PATH (/usr/bin/steamos-session-select)"};

  const auto needs_flag = gm::setup_host_advice(game_mode, state_t::needs_headless_boot, "/usr/bin/polaris");
  EXPECT_NE(needs_flag.find("Steam Game Mode session detected: steamos-session-select on PATH (/usr/bin/steamos-session-select)."), std::string::npos);
  EXPECT_NE(needs_flag.find("Make it start at boot instead:\n  sudo -H /usr/bin/polaris --setup-host --enable-headless-boot"), std::string::npos);
  EXPECT_EQ(needs_flag.find("shows the Game Mode screen"), std::string::npos)
    << "a host that is about to go offline is told how to stay up, nothing else";

  const auto just_enabled = gm::setup_host_advice(game_mode, state_t::headless_boot_enabled_now, "/usr/bin/polaris");
  EXPECT_NE(just_enabled.find("With headless boot on"), std::string::npos);
  EXPECT_NE(just_enabled.find("every stream shows the Game Mode screen"), std::string::npos);
  EXPECT_EQ(just_enabled.find("not supported"), std::string::npos);
  EXPECT_EQ(just_enabled.find("--enable-headless-boot"), std::string::npos);

  const auto already = gm::setup_host_advice(game_mode, state_t::already_independent, "/usr/bin/polaris");
  EXPECT_NE(already.find("already starts at boot"), std::string::npos);
  EXPECT_NE(already.find("docs/handhelds.md"), std::string::npos);

  const auto just_disabled = gm::setup_host_advice(game_mode, state_t::headless_boot_disabled_now, "/usr/bin/polaris");
  EXPECT_NE(just_disabled.find("With headless boot off"), std::string::npos);
  EXPECT_NE(just_disabled.find("Turn it back on with:\n  sudo -H /usr/bin/polaris --setup-host --enable-headless-boot"), std::string::npos);
  EXPECT_EQ(just_disabled.find("Make it start at boot instead"), std::string::npos);
}


TEST(GameModeHostTests, AFreshScanIsNeverReplacedByABackgroundRefreshThatStartedBeforeIt) {
  // #204: a launch leaving Game Mode acts on a scan taken after the switch. A refresh that read the
  // host before the switch, and finishes after it, must not put Game Mode back.
  gm::set_session_live_for_tests(std::nullopt);
  std::mutex mutex;
  std::condition_variable changed;
  bool release = false;
  std::atomic<bool> host_live {true};
  std::atomic<int> background_scans {0};
  const auto test_thread = std::this_thread::get_id();
  gm::set_session_scan_for_tests([&]() {
    const bool seen = host_live.load();
    if (std::this_thread::get_id() != test_thread) {
      ++background_scans;
      std::unique_lock lock {mutex};
      changed.wait(lock, [&]() { return release; });
    }
    return seen;
  });
  auto restore = util::fail_guard([&]() {
    {
      std::lock_guard lock {mutex};
      release = true;
    }
    changed.notify_all();
    for (int wait = 0; wait < 200 && gm::session_live_refresh_pending_for_tests(); ++wait) std::this_thread::sleep_for(5ms);
    gm::set_session_scan_for_tests({});
  });

  ASSERT_TRUE(gm::session_live()) << "first answer: a scan on the caller's thread";
  gm::age_session_live_answer_for_tests(3s);
  ASSERT_TRUE(gm::session_live()) << "a kept answer, while a refresh starts in the background";
  for (int wait = 0; wait < 400 && background_scans.load() == 0; ++wait) std::this_thread::sleep_for(5ms);
  ASSERT_EQ(background_scans.load(), 1) << "the background refresh read the host before the switch";

  host_live = false;  // the session switch
  EXPECT_FALSE(gm::session_live_fresh());

  {
    std::lock_guard lock {mutex};
    release = true;
  }
  changed.notify_all();
  for (int wait = 0; wait < 400 && gm::session_live_refresh_pending_for_tests(); ++wait) std::this_thread::sleep_for(5ms);
  ASSERT_FALSE(gm::session_live_refresh_pending_for_tests());
  EXPECT_FALSE(gm::session_live()) << "the stale refresh was discarded; the fresh scan stands";
}

TEST(GameModeHostTests, OverlappingFirstScansNeverReadAMissingAnswerOrRepublishAnOvertakenOne) {
  // #204: two callers find no kept answer and both scan. The older finishes first, while the newer
  // is still out: it acts on its own scan, publishes nothing, and never reads the empty answer.
  gm::set_session_live_for_tests(std::nullopt);
  std::mutex mutex;
  std::condition_variable changed;
  int entered = 0;
  bool release_first = false;
  bool release_second = false;
  gm::set_session_scan_for_tests([&]() {
    std::unique_lock lock {mutex};
    const int index = ++entered;
    changed.notify_all();
    changed.wait(lock, [&]() { return index == 1 ? release_first : release_second; });
    // The first scan saw Game Mode; the second, taken after the switch, did not.
    return index == 1;
  });
  std::optional<bool> first;
  std::optional<bool> second;
  std::thread older;
  std::thread newer;
  auto restore = util::fail_guard([&]() {
    {
      std::lock_guard lock {mutex};
      release_first = release_second = true;
    }
    changed.notify_all();
    if (older.joinable()) older.join();
    if (newer.joinable()) newer.join();
    gm::set_session_scan_for_tests({});
  });
  const auto wait_for_entries = [&](int count) {
    std::unique_lock lock {mutex};
    return changed.wait_for(lock, 2s, [&]() { return entered >= count; });
  };

  older = std::thread([&]() { first = gm::session_live(); });
  ASSERT_TRUE(wait_for_entries(1));
  newer = std::thread([&]() { second = gm::session_live(); });
  ASSERT_TRUE(wait_for_entries(2)) << "both callers are scanning at once";

  {
    std::lock_guard lock {mutex};
    release_first = true;
  }
  changed.notify_all();
  older.join();
  ASSERT_TRUE(first.has_value());
  EXPECT_TRUE(*first) << "the older caller acts on its own scan";
  EXPECT_FALSE(gm::session_live_answer_for_tests().has_value())
    << "an overtaken scan is not published, and the newer one has not answered yet";

  {
    std::lock_guard lock {mutex};
    release_second = true;
  }
  changed.notify_all();
  newer.join();
  ASSERT_TRUE(second.has_value());
  EXPECT_FALSE(*second);
  EXPECT_EQ(gm::session_live_answer_for_tests(), std::optional<bool> {false}) << "the newest scan is the kept answer";
}

#endif
