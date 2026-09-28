/**
 * @file tests/unit/platform/test_flatpak_session_instances.cpp
 * @brief Which Flatpak instances a private session owns, against fixtures modelled on a live host.
 *
 * The desktop instances are copied from pc-papi on 2026-09-27: PrusaSlicer (instance 1925812001,
 * bwrap 2812294, init 2812304, pid namespace 4026534550) and LibreWolf (3247804733, 2848944,
 * 2849026, 4026534263), both children of systemd --user 982284 with no session token. The session
 * side follows the Alan Wake 2 teardown of the same night: Heroic's instance 1424202439 under the
 * session's labwc supervisor, and the game in portal sub-sandbox 2973451596 sharing Heroic's pid
 * namespace.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/flatpak_session_instances.h>

  #include <algorithm>
  #include <filesystem>
  #include <fstream>
  #include <map>
  #include <optional>
  #include <set>
  #include <string>
  #include <unistd.h>
  #include <vector>

namespace {
  namespace fsi = flatpak_session_instances;
  using namespace std::string_literals;

  struct fake_proc_t {
    pid_t parent = 1;
    std::uint64_t start = 0;
    std::string comm;
    std::vector<pid_t> nspid;  ///< empty means the host's namespace only: {pid}
    std::string cgroup = "/user.slice/user-1000.slice/user@1000.service/app.slice/app-polaris.scope";
    std::optional<std::string> environ = "HOME=/var/empty\0"s;
    std::uint64_t ns = 4026531836;  ///< the host's pid namespace
    std::optional<std::uint64_t> parent_ns;
  };

  constexpr std::uint64_t host_ns = 4026531836;
  constexpr std::uint64_t heroic_ns = 4026540001;
  const std::string token = "022C50EB74B10CE9BD3E1AF593A98022";
  const std::string scopes = "/user.slice/user-1000.slice/user@1000.service/app.slice/";

  std::string token_environ() {
    return "HOME=/var/empty\0POLARIS_SESSION_INSTANCE_ID="s + token + "\0POLARIS_PRIVATE_SESSION=1\0"s;
  }

  /// A host of fake processes and the Flatpak records under a runtime dir of its own.
  struct fake_host_t {
    std::map<pid_t, fake_proc_t> procs;
    std::map<std::string, std::vector<pid_t>> cgroups;
    std::filesystem::path runtime_dir;

    fake_host_t() {
      runtime_dir = std::filesystem::temp_directory_path() /
                    ("polaris-flatpak-instances-" + std::to_string(getpid()) + "-" + std::to_string(++serial()));
      std::filesystem::remove_all(runtime_dir);
      std::filesystem::create_directories(runtime_dir / ".flatpak");
      procs[982284] = {1, 1000, "systemd"};
    }

    ~fake_host_t() {
      std::error_code ignored;
      std::filesystem::remove_all(runtime_dir, ignored);
    }

    static int &serial() {
      static int value = 0;
      return value;
    }

    void add(pid_t pid, fake_proc_t proc) {
      if (proc.nspid.empty()) {
        proc.nspid = {pid};
      }
      procs[pid] = proc;
      cgroups[proc.cgroup].push_back(pid);
    }

    void write_record(const std::string &id, std::optional<pid_t> bwrap, const std::string &app_id, std::optional<std::string> bwrapinfo) {
      const auto dir = runtime_dir / ".flatpak" / id;
      std::filesystem::create_directories(dir);
      if (bwrap) {
        std::ofstream(dir / "pid") << *bwrap;
      }
      std::ofstream(dir / "info") << "[Application]\nname=" << app_id
                                  << "\nruntime=runtime/org.freedesktop.Platform/x86_64/25.08\n\n[Instance]\ninstance-id="
                                  << id << "\n";
      if (bwrapinfo) {
        std::ofstream(dir / "bwrapinfo.json") << *bwrapinfo;
      }
    }

    static std::string bwrapinfo(pid_t child, std::uint64_t mnt_ns, std::uint64_t pid_ns) {
      return "{\n    \"child-pid\": " + std::to_string(child) + ",\n    \"mnt-namespace\": " + std::to_string(mnt_ns) +
             ",\n    \"pid-namespace\": " + std::to_string(pid_ns) + "\n}";
    }

    /// A whole Flatpak instance: record, outer bwrap, init, and the app's processes in its scope.
    void add_instance(
      const std::string &id,
      const std::string &app_id,
      pid_t bwrap,
      pid_t bwrap_parent,
      std::uint64_t start,
      pid_t init,
      std::uint64_t ns,
      bool init_is_pid_one = true,
      std::optional<std::uint64_t> parent_ns = std::nullopt
    ) {
      const auto scope = scopes + "app-flatpak-" + fsi::escape_unit_name(app_id) + "-" + id + ".scope";
      add(bwrap, {bwrap_parent, start, "bwrap", {}, scope, ""s, host_ns});
      add(init, {bwrap, start + 4, "bwrap", {init, init_is_pid_one ? 1 : 600}, scope, ""s, ns, parent_ns});
      write_record(id, bwrap, app_id, bwrapinfo(init, ns - 1, ns));
    }

    std::string scope_of(const std::string &id, const std::string &app_id) const {
      return scopes + "app-flatpak-" + fsi::escape_unit_name(app_id) + "-" + id + ".scope";
    }

    fsi::proc_reader_t reader() const {
      fsi::proc_reader_t reader;
      reader.read = [this](pid_t pid, std::string_view name) -> std::optional<std::string> {
        const auto found = procs.find(pid);
        if (found == procs.end()) {
          return std::nullopt;
        }
        const auto &proc = found->second;
        if (name == "stat") {
          return std::to_string(pid) + " (" + proc.comm + ") S " + std::to_string(proc.parent) +
                 " 0 0 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0 " + std::to_string(proc.start) + " 0 0";
        }
        if (name == "status") {
          std::string nspid = "NSpid:";
          for (const auto value : proc.nspid) {
            nspid += "\t" + std::to_string(value);
          }
          return "Name:\t" + proc.comm + "\nPPid:\t" + std::to_string(proc.parent) + "\n" + nspid + "\n";
        }
        if (name == "cgroup") {
          return "1:net_cls:/\n0::" + proc.cgroup + "\n";
        }
        if (name == "environ") {
          return proc.environ;
        }
        if (name == "comm") {
          return proc.comm + "\n";
        }
        return std::nullopt;
      };
      reader.pid_namespace = [this](pid_t pid) -> std::optional<std::uint64_t> {
        const auto found = procs.find(pid);
        return found == procs.end() ? std::nullopt : std::optional<std::uint64_t> {found->second.ns};
      };
      reader.parent_pid_namespace = [this](pid_t pid) -> std::optional<std::uint64_t> {
        const auto found = procs.find(pid);
        return found == procs.end() ? std::nullopt : found->second.parent_ns;
      };
      reader.cgroup_procs = [this](const std::string &cgroup) -> std::optional<std::vector<pid_t>> {
        const auto found = cgroups.find(cgroup);
        if (found == cgroups.end()) {
          return std::nullopt;
        }
        std::vector<pid_t> live;
        for (const auto pid : found->second) {
          if (procs.contains(pid)) {
            live.push_back(pid);
          }
        }
        return live;
      };
      reader.all_pids = [this]() {
        std::vector<pid_t> pids;
        for (const auto &[pid, proc] : procs) {
          pids.push_back(pid);
        }
        return pids;
      };
      return reader;
    }

    /// PrusaSlicer and LibreWolf as they were open on pc-papi's desktop: children of systemd --user,
    /// their own pid namespaces, no session token anywhere.
    void add_desktop_instances() {
      add_instance("1925812001", "com.prusa3d.PrusaSlicer", 2812294, 982284, 21016711, 2812304, 4026534550);
      add(2812310, {2812304, 21016720, "prusa-slicer", {2812310, 2}, scope_of("1925812001", "com.prusa3d.PrusaSlicer"), "HOME=/var/empty\0DISPLAY=:0\0"s, 4026534550});
      add_instance("3247804733", "io.gitlab.librewolf-community", 2848944, 982284, 19645047, 2849026, 4026534263);
      add(2849100, {2849026, 19645060, "librewolf", {2849100, 2}, scope_of("3247804733", "io.gitlab.librewolf-community"), "HOME=/var/empty\0"s, 4026534263});
    }

    /// The session: Polaris, its labwc supervisor, labwc, and the startup chain to `flatpak run`.
    fsi::evidence_t add_session() {
      add(3231794, {982284, 21100000, "polaris-kms"});
      add(345544, {3231794, 21200000, "polaris-kms", {}, "/user.slice/user-1000.slice/user@1000.service/app.slice/app-polaris.scope", token_environ()});
      add(345580, {345544, 21200010, "labwc", {}, "/user.slice/user-1000.slice/user@1000.service/app.slice/app-polaris.scope", token_environ()});
      add(346000, {345580, 21205000, "sh", {}, "/user.slice/user-1000.slice/user@1000.service/app.slice/app-polaris.scope", token_environ()});
      add(346100, {346000, 21205010, "bwrap", {}, "/user.slice/user-1000.slice/user@1000.service/app.slice/app-polaris.scope", token_environ()});
      add(346200, {346100, 21205020, "sh", {}, "/user.slice/user-1000.slice/user@1000.service/app.slice/app-polaris.scope", token_environ()});
      return {{345544, 21200000}, token, {}};
    }

    /// Heroic's own instance, started by the session's `flatpak run` (R1), with heroic-run's shell,
    /// Electron's main process with its environ overwritten, and legendary under it.
    void add_heroic_launcher() {
      const std::string id = "1424202439";
      const std::string app = "com.heroicgameslauncher.hgl";
      add_instance(id, app, 346422, 346200, 21210000, 346430, heroic_ns);
      const auto scope = scope_of(id, app);
      add(346500, {346422, 21210002, "bwrap", {}, scope, token_environ(), host_ns});  // xdg-dbus-proxy's bwrap
      add(346781, {346430, 21210010, "sh", {346781, 2}, scope, token_environ(), heroic_ns});
      add(346819, {346781, 21210020, "heroic", {346819, 13}, scope, std::string(64, '\0'), heroic_ns});
      add(355000, {346819, 21214000, "legendary", {355000, 300}, scope, token_environ(), heroic_ns});
    }

    /// The game, spawned by flatpak-portal into a sub-sandbox sharing Heroic's pid namespace.
    void add_portal_game(std::uint64_t ns = heroic_ns, bool shared = true) {
      const std::string id = "2973451596";
      const std::string app = "com.heroicgameslauncher.hgl";
      if (!procs.contains(346826)) {
        add(346826, {982284, 21210050, "flatpak-portal"});
      }
      add_instance(id, app, 355400, 346826, 21215000, 355450, ns, !shared);
      add(361425, {355450, 21215100, "AlanWake2.exe", {361425, 631}, scope_of(id, app), token_environ(), ns});
    }

    /// Zypak's Chromium helper sub-sandbox, in Heroic's namespace, with no readable token.
    void add_zypak_helper() {
      const std::string id = "3747798030";
      const std::string app = "com.heroicgameslauncher.hgl";
      add_instance(id, app, 347000, 346826, 21211000, 347010, heroic_ns, false);
      add(347020, {347010, 21211010, "heroic", {347020, 21}, scope_of(id, app), std::string(32, '\0'), heroic_ns});
    }
  };

  const fsi::owned_instance_t *find_owned(const fsi::attribution_t &attribution, const std::string &id) {
    for (const auto &owned : attribution.owned) {
      if (owned.instance.id == id) {
        return &owned;
      }
    }
    return nullptr;
  }

  std::set<pid_t> owned_pids(const fsi::attribution_t &attribution, const fake_host_t &host) {
    std::set<pid_t> pids;
    for (const auto &owned : attribution.owned) {
      pids.insert(owned.instance.bwrap.pid);
      pids.insert(owned.instance.init.pid);
      for (const auto &[pid, proc] : host.procs) {
        if (proc.cgroup == owned.instance.cgroup) {
          pids.insert(pid);
        }
      }
    }
    return pids;
  }
}  // namespace

TEST(FlatpakSessionInstancesTests, AttributesRootInstanceBySupervisorLineage) {
  fake_host_t host;
  host.add_desktop_instances();
  auto evidence = host.add_session();
  host.add_heroic_launcher();
  const auto reader = host.reader();
  const auto instances = fsi::read_live_instances(host.runtime_dir, reader);
  ASSERT_EQ(instances.live.size(), 3u);
  const auto attribution = fsi::attribute(instances, evidence, reader);
  const auto *heroic = find_owned(attribution, "1424202439");
  ASSERT_NE(heroic, nullptr);
  EXPECT_EQ(heroic->role, fsi::role_e::launcher);
  EXPECT_EQ(heroic->rule, fsi::rule_e::lineage);
  EXPECT_EQ(heroic->instance.app_id, "com.heroicgameslauncher.hgl");
  EXPECT_EQ(heroic->instance.bwrap.pid, 346422);
  EXPECT_EQ(heroic->instance.init.pid, 346430);
  EXPECT_EQ(attribution.owned.size(), 1u);
  EXPECT_EQ(attribution.left_alone, 2);
}

TEST(FlatpakSessionInstancesTests, AttributesPortalGameBySharedPidNamespaceAndToken) {
  fake_host_t host;
  host.add_desktop_instances();
  auto evidence = host.add_session();
  host.add_heroic_launcher();
  host.add_portal_game();
  const auto reader = host.reader();
  const auto attribution = fsi::attribute(fsi::read_live_instances(host.runtime_dir, reader), evidence, reader);
  const auto *game = find_owned(attribution, "2973451596");
  ASSERT_NE(game, nullptr);
  // It descends from flatpak-portal under systemd --user, not from the session, and it shares
  // Heroic's namespace; the token in the game's environ is what makes it the game.
  EXPECT_EQ(game->role, fsi::role_e::game);
  EXPECT_EQ(game->rule, fsi::rule_e::token);
  EXPECT_EQ(game->instance.pid_namespace, heroic_ns);
  EXPECT_EQ(attribution.owned.size(), 2u);
}

TEST(FlatpakSessionInstancesTests, AttributesPortalGameByTokenAlone) {
  fake_host_t host;
  auto evidence = host.add_session();
  // No launcher instance at all, and a game in a namespace of its own: the token is the only proof.
  host.add_portal_game(4026540777, false);
  const auto reader = host.reader();
  const auto attribution = fsi::attribute(fsi::read_live_instances(host.runtime_dir, reader), evidence, reader);
  ASSERT_EQ(attribution.owned.size(), 1u);
  EXPECT_EQ(attribution.owned.front().role, fsi::role_e::game);
  EXPECT_EQ(attribution.owned.front().rule, fsi::rule_e::token);
}

TEST(FlatpakSessionInstancesTests, NestedHelperWithoutTokenIsHelper) {
  fake_host_t host;
  auto evidence = host.add_session();
  host.add_heroic_launcher();
  host.add_zypak_helper();
  // And one nested a level below Heroic's namespace, as a sandbox inside the sandbox would be.
  host.add_instance("3747798031", "com.heroicgameslauncher.hgl", 347100, 346826, 21211100, 347110, 4026540888, true, heroic_ns);
  const auto reader = host.reader();
  const auto attribution = fsi::attribute(fsi::read_live_instances(host.runtime_dir, reader), evidence, reader);
  ASSERT_EQ(attribution.owned.size(), 3u);
  for (const auto *id : {"3747798030", "3747798031"}) {
    const auto *helper = find_owned(attribution, id);
    ASSERT_NE(helper, nullptr) << id;
    EXPECT_EQ(helper->role, fsi::role_e::helper) << id;
    EXPECT_EQ(helper->rule, fsi::rule_e::shared_namespace) << id;
  }
}

TEST(FlatpakSessionInstancesTests, DesktopInstancesAreNeverOwned) {
  fake_host_t host;
  host.add_desktop_instances();
  auto evidence = host.add_session();
  host.add_heroic_launcher();
  host.add_portal_game();
  host.add_zypak_helper();
  const auto reader = host.reader();
  const auto instances = fsi::read_live_instances(host.runtime_dir, reader);
  const auto attribution = fsi::attribute(instances, evidence, reader);
  EXPECT_EQ(attribution.owned.size(), 3u);
  EXPECT_EQ(attribution.left_alone, 2);
  EXPECT_EQ(find_owned(attribution, "1925812001"), nullptr);
  EXPECT_EQ(find_owned(attribution, "3247804733"), nullptr);
  const auto pids = owned_pids(attribution, host);
  for (const pid_t desktop : {2812294, 2812304, 2812310, 2848944, 2849026, 2849100}) {
    EXPECT_FALSE(pids.contains(desktop)) << desktop << " belongs to the desktop";
  }
  EXPECT_TRUE(attribution.left_alone_same_app.empty()) << "neither is an app the session started";
}

TEST(FlatpakSessionInstancesTests, InstanceOlderThanSupervisorIsNeverOwnedEvenWithToken) {
  fake_host_t host;
  host.add_desktop_instances();
  auto evidence = host.add_session();
  host.add_heroic_launcher();
  // LibreWolf, started before the session, made to match every rule: its app carries the session
  // token and it shares Heroic's pid namespace. Nothing older than the session is the session's.
  host.procs[2849100].environ = token_environ();
  host.procs[2849026].ns = heroic_ns;
  host.write_record("3247804733", 2848944, "io.gitlab.librewolf-community", fake_host_t::bwrapinfo(2849026, 4026534262, heroic_ns));
  const auto reader = host.reader();
  const auto attribution = fsi::attribute(fsi::read_live_instances(host.runtime_dir, reader), evidence, reader);
  EXPECT_EQ(find_owned(attribution, "3247804733"), nullptr);
  EXPECT_NE(find_owned(attribution, "1424202439"), nullptr);
}

TEST(FlatpakSessionInstancesTests, StaleInstanceDirWithReusedPidIsSkipped) {
  fake_host_t host;
  auto evidence = host.add_session();
  // A record Flatpak has not collected: its bwrap pid now names an unrelated shell, whose child is
  // not the recorded init.
  host.add(400000, {345580, 21300000, "bash"});
  host.write_record("111111111", 400000, "com.heroicgameslauncher.hgl", fake_host_t::bwrapinfo(400010, 4026540100, 4026540101));
  host.add(400010, {1, 21300010, "sleep"});
  // And one whose bwrap is gone entirely.
  host.write_record("222222222", 400500, "com.heroicgameslauncher.hgl", fake_host_t::bwrapinfo(400510, 4026540200, 4026540201));
  const auto reader = host.reader();
  const auto instances = fsi::read_live_instances(host.runtime_dir, reader);
  EXPECT_TRUE(instances.live.empty());
  EXPECT_TRUE(instances.unresolved.empty());
  EXPECT_TRUE(fsi::attribute(instances, evidence, reader).owned.empty());
}

TEST(FlatpakSessionInstancesTests, DesktopHeroicAlreadyRunningIsReportedLeftAlone) {
  // Issue #234: Heroic was already open on the desktop, so the session's `flatpak run` handed it
  // the launch and exited. The game runs in the desktop Heroic's namespace with the desktop's
  // environment. None of it is the session's, and the host says so by name.
  fake_host_t host;
  auto evidence = host.add_session();
  evidence.launched_app_ids = {"com.heroicgameslauncher.hgl"};
  host.add_instance("555555555", "com.heroicgameslauncher.hgl", 300000, 982284, 20000000, 300010, heroic_ns);
  host.add(300100, {300010, 20000010, "heroic", {300100, 2}, host.scope_of("555555555", "com.heroicgameslauncher.hgl"), std::string(16, '\0'), heroic_ns});
  host.add(346826, {982284, 21210050, "flatpak-portal"});
  host.add_instance("666666666", "com.heroicgameslauncher.hgl", 360000, 346826, 21215000, 360010, heroic_ns, false);
  host.add(360100, {360010, 21215010, "AlanWake2.exe", {360100, 631}, host.scope_of("666666666", "com.heroicgameslauncher.hgl"), "HOME=/var/empty\0DISPLAY=:0\0"s, heroic_ns});
  const auto reader = host.reader();
  const auto attribution = fsi::attribute(fsi::read_live_instances(host.runtime_dir, reader), evidence, reader);
  EXPECT_TRUE(attribution.owned.empty());
  EXPECT_EQ(attribution.left_alone, 2);
  ASSERT_EQ(attribution.left_alone_same_app.size(), 2u);
  EXPECT_EQ(attribution.left_alone_same_app.front().app_id, "com.heroicgameslauncher.hgl");
}

TEST(FlatpakSessionInstancesTests, AppProcessesExcludeOuterBwrapProxyAndInit) {
  fake_host_t host;
  auto evidence = host.add_session();
  host.add_heroic_launcher();
  host.add_portal_game();
  const auto reader = host.reader();
  const auto instances = fsi::read_live_instances(host.runtime_dir, reader);
  const auto launcher = std::find_if(instances.live.begin(), instances.live.end(), [](const auto &instance) {
    return instance.id == "1424202439";
  });
  ASSERT_NE(launcher, instances.live.end());
  const auto members = fsi::scope_members(*launcher, token, instances.live, reader);
  ASSERT_TRUE(members);
  std::set<pid_t> apps;
  for (const auto &member : fsi::app_processes(*launcher, *members)) {
    apps.insert(member.process.pid);
  }
  EXPECT_EQ(apps, (std::set<pid_t> {346781, 346819, 355000}))
    << "the outer bwrap 346422, the proxy's bwrap 346500 and the init 346430 are never app processes";
  const auto carriers = std::count_if(members->begin(), members->end(), [](const auto &member) {
    return member.carries_token;
  });
  EXPECT_EQ(carriers, 3) << "heroic-run's shell, legendary and the proxy's bwrap; Electron overwrote its own environ";

  // The game's sub-sandbox shares Heroic's namespace, so its init is not pid 1 there; the record
  // names it instead.
  const auto game = std::find_if(instances.live.begin(), instances.live.end(), [](const auto &instance) {
    return instance.id == "2973451596";
  });
  ASSERT_NE(game, instances.live.end());
  const auto game_members = fsi::scope_members(*game, token, instances.live, reader);
  ASSERT_TRUE(game_members);
  const auto game_apps = fsi::app_processes(*game, *game_members);
  ASSERT_EQ(game_apps.size(), 1u);
  EXPECT_EQ(game_apps.front().process.pid, 361425);
  EXPECT_EQ(game_apps.front().nspid.back(), 631);
}

TEST(FlatpakSessionInstancesTests, InstanceWithoutBwrapinfoYetIsUnresolvedNotOwned) {
  fake_host_t host;
  auto evidence = host.add_session();
  // `flatpak run` has started bwrap under the session, but bwrap has not made the sandbox yet.
  host.add(346422, {346200, 21210000, "bwrap", {}, host.scope_of("1424202439", "com.heroicgameslauncher.hgl"), ""s});
  host.write_record("1424202439", 346422, "com.heroicgameslauncher.hgl", std::nullopt);
  const auto reader = host.reader();
  const auto instances = fsi::read_live_instances(host.runtime_dir, reader);
  EXPECT_TRUE(instances.live.empty());
  ASSERT_EQ(instances.unresolved.size(), 1u);
  EXPECT_EQ(instances.unresolved.front().bwrap.pid, 346422);
  const auto attribution = fsi::attribute(instances, evidence, reader);
  EXPECT_TRUE(attribution.owned.empty());
  ASSERT_EQ(attribution.unresolved.size(), 1u);
  EXPECT_TRUE(fsi::is_instance_bwrap({346422, 21210000}, attribution.unresolved))
    << "its bwrap is known, so nothing signals it on another rule's word";
}

TEST(FlatpakSessionInstancesTests, ARecordOutsideItsOwnScopeIsUnresolved) {
  fake_host_t host;
  auto evidence = host.add_session();
  host.add_heroic_launcher();
  host.procs[346430].cgroup = "/user.slice/user-1000.slice/user@1000.service/app.slice/app-polaris.scope";
  const auto reader = host.reader();
  const auto instances = fsi::read_live_instances(host.runtime_dir, reader);
  EXPECT_TRUE(instances.live.empty());
  ASSERT_EQ(instances.unresolved.size(), 1u);
  EXPECT_TRUE(fsi::attribute(instances, evidence, reader).owned.empty());
}

TEST(FlatpakSessionInstancesTests, LineageThroughAReusedPidIsNotLineage) {
  fake_host_t host;
  auto evidence = host.add_session();
  host.add_heroic_launcher();
  // The shell between the gamepad wrapper and `flatpak run` exited, and its pid now names a
  // process started after the bwrap it once parented.
  host.procs[346200].start = 21210005;
  const auto reader = host.reader();
  EXPECT_FALSE(fsi::descends_through_rising_start_times({346422, 21210000}, evidence.supervisor, reader));
  host.procs[346200].start = 21205020;
  EXPECT_TRUE(fsi::descends_through_rising_start_times({346422, 21210000}, evidence.supervisor, host.reader()));
  // A supervisor pid that now names another process is not the session's supervisor.
  EXPECT_FALSE(fsi::descends_through_rising_start_times({346422, 21210000}, {345544, 21199999}, host.reader()));
}

TEST(FlatpakSessionInstancesTests, ADesktopInstanceOfTheLaunchedAppIsFoundBeforeLaunch) {
  // What the launch warns about: Heroic already open on the desktop when a session's `flatpak run`
  // of Heroic is about to start.
  fake_host_t host;
  host.add_desktop_instances();
  host.add_instance("555555555", "com.heroicgameslauncher.hgl", 300000, 982284, 20000000, 300010, heroic_ns);
  const auto instances = fsi::read_live_instances(host.runtime_dir, host.reader());
  const auto running = fsi::instances_of(instances, "com.heroicgameslauncher.hgl");
  ASSERT_EQ(running.size(), 1u);
  EXPECT_EQ(running.front().id, "555555555");
  EXPECT_TRUE(fsi::instances_of(instances, "net.lutris.Lutris").empty());
}

TEST(FlatpakSessionInstancesTests, ParsesWhatTheKernelAndSystemdWrite) {
  EXPECT_EQ(fsi::escape_unit_name("io.gitlab.librewolf-community"), "io.gitlab.librewolf\\x2dcommunity");
  EXPECT_EQ(fsi::escape_unit_name("com.heroicgameslauncher.hgl"), "com.heroicgameslauncher.hgl");
  EXPECT_EQ(
    fsi::start_time_from_stat("2812294 (bwrap) S 982284 2812294 2812294 0 -1 4194560 1699 38 0 0 1 0 0 0 20 0 1 0 21016711 3911680 499"),
    21016711u
  );
  EXPECT_EQ(fsi::start_time_from_stat("42 (Web Content (x)) S 7 42 42 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 99 0 0"), 99u);
  EXPECT_EQ(fsi::parent_from_stat("42 (Web Content (x)) S 7 42 42 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 99 0 0"), 7);
  EXPECT_EQ(fsi::nspid_from_status("Name:\tbwrap\nNSpid:\t2812304\t1\nNSpgid:\t2812294\t0\n"), (std::vector<pid_t> {2812304, 1}));
  EXPECT_EQ(
    fsi::cgroup_from_proc("1:net_cls:/\n0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-flatpak-io.gitlab.librewolf\\x2dcommunity-3247804733.scope\n"),
    "/user.slice/user-1000.slice/user@1000.service/app.slice/app-flatpak-io.gitlab.librewolf\\x2dcommunity-3247804733.scope"
  );
  EXPECT_EQ(fsi::cgroup_from_proc("12:pids:/a\n1:name=systemd:/user.slice/x.scope\n"), "/user.slice/x.scope");
  EXPECT_TRUE(fsi::environ_carries_token("A=1\0POLARIS_SESSION_INSTANCE_ID=abc\0"s, "abc"));
  EXPECT_FALSE(fsi::environ_carries_token("A=1\0POLARIS_SESSION_INSTANCE_ID=abcd\0"s, "abc"));
  EXPECT_FALSE(fsi::environ_carries_token(std::string(64, '\0'), "abc"));
}

#endif
