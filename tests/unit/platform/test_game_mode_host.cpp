/**
 * @file tests/unit/platform/test_game_mode_host.cpp
 * @brief Test gamescope Steam session (Game Mode) host detection and the guidance built on it.
 */
#include "../../tests_common.h"

#ifdef __linux__

#include <src/platform/linux/game_mode_host.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
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
  EXPECT_NE(running.summary.find("not supported yet"), std::string::npos);
  EXPECT_NE(running.action.find("Switch to Desktop Mode"), std::string::npos);

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
  EXPECT_EQ(needs_flag.find("not supported yet"), std::string::npos);

  const auto just_enabled = gm::setup_host_advice(game_mode, state_t::headless_boot_enabled_now, "/usr/bin/polaris");
  EXPECT_NE(just_enabled.find("With headless boot on"), std::string::npos);
  EXPECT_NE(just_enabled.find("not supported yet"), std::string::npos);
  EXPECT_EQ(just_enabled.find("--enable-headless-boot"), std::string::npos);

  const auto already = gm::setup_host_advice(game_mode, state_t::already_independent, "/usr/bin/polaris");
  EXPECT_NE(already.find("already starts at boot"), std::string::npos);
  EXPECT_NE(already.find("docs/handhelds.md"), std::string::npos);

  const auto just_disabled = gm::setup_host_advice(game_mode, state_t::headless_boot_disabled_now, "/usr/bin/polaris");
  EXPECT_NE(just_disabled.find("With headless boot off"), std::string::npos);
  EXPECT_NE(just_disabled.find("Turn it back on with:\n  sudo -H /usr/bin/polaris --setup-host --enable-headless-boot"), std::string::npos);
  EXPECT_EQ(just_disabled.find("Make it start at boot instead"), std::string::npos);
}

#endif
