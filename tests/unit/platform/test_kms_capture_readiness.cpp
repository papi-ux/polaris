/**
 * @file tests/unit/platform/test_kms_capture_readiness.cpp
 * @brief Test that the console reads DRM/KMS capture against the capture the host is set to, and
 *        names the one step still missing only where capture would use KMS.
 */
#include "../../tests_common.h"

#ifdef __linux__

#include <src/platform/linux/kms_capture_readiness.h>
#ifdef POLARIS_BUILD_PORTAL
  #include <src/platform/linux/portal_capability.h>
#endif

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace kr = platf::kms_readiness;
namespace ke = platf::kms_enable;
namespace sdp = stream_display_policy;

namespace {
  /// A host set up the way 1.4.13 intends: the service runs the helper, capture = kms, the capability permitted.
  kr::facts_t ready_host() {
    kr::facts_t facts;
    facts.capture = "kms";
    facts.route = "kms";
    facts.cap_sys_admin = true;
    facts.helper_installed = true;
    facts.helper_has_capability = true;
    facts.running_helper = true;
    facts.in_service = true;
    facts.service_points_at_helper = true;
    facts.group_member = true;
    facts.session_group = ke::session_group_e::live;
    return facts;
  }

  /// capture = kms, the package installed, and nothing else done yet.
  kr::facts_t installed_host() {
    kr::facts_t facts;
    facts.capture = "kms";
    facts.route = "kms";
    facts.helper_installed = true;
    facts.helper_has_capability = true;
    facts.in_service = true;
    return facts;
  }

  /// The same host after --enable-kms pointed the service at the helper, before anything restarted.
  kr::facts_t pointed_host() {
    auto facts = installed_host();
    facts.service_points_at_helper = true;
    facts.group_member = true;
    facts.session_group = ke::session_group_e::live;
    return facts;
  }

  std::string read_source(const char *relative) {
    std::ifstream in(std::filesystem::path {POLARIS_SOURCE_DIR} / relative, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
}  // namespace

TEST(KmsCaptureReadinessTests, APortalOrKwinHostRunningTheHelperIsNotInUseRatherThanBroken) {
  // papi's host: capture = portal, the service runs the helper, and Polaris gave CAP_SYS_ADMIN up at
  // startup because the portal and KWin refuse a program that holds it. The first version of this
  // row called that a helper without its capability and told him to reinstall polaris-kms.
  auto facts = ready_host();
  facts.capture = "portal";  // kwin reads as portal, the way dispatch reads it
  facts.route = "portal";
  facts.cap_sys_admin = false;
  facts.capability_set_aside = true;
  EXPECT_EQ(kr::route_of(facts), kr::route_e::other);
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_in_use);

  // Whatever else is or is not set up, a capture that goes another way needs nothing from KMS.
  facts.capability_set_aside = false;
  facts.helper_installed = false;
  facts.helper_has_capability = false;
  facts.running_helper = false;
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_in_use);
  for (const auto *route : {"wlr", "x11", "nvfbc"}) {
    facts.capture = route;
    facts.route = route;
    EXPECT_EQ(kr::decide(facts), kr::state_e::not_in_use) << route;
  }
}

TEST(KmsCaptureReadinessTests, AutodetectThatStartedWithoutCapabilitiesIsNotInUse) {
  // Autodetect in Mirror Desktop starts Polaris without capabilities, so the portal accepts it, and
  // the search then passes over KMS. Nothing a user could run makes KMS answer there.
  auto facts = ready_host();
  facts.capture.clear();
  facts.route.clear();
  facts.cap_sys_admin = false;
  facts.capability_set_aside = true;
  EXPECT_EQ(kr::route_of(facts), kr::route_e::other);
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_in_use);

  // The plain binary in the same mode has nothing to give up, and the same search.
  kr::facts_t plain;
  plain.capability_set_aside = true;
  EXPECT_EQ(kr::decide(plain), kr::state_e::not_in_use);
}

#ifdef POLARIS_BUILD_PORTAL
TEST(KmsCaptureReadinessTests, NoCaptureTheStartupDropServesIsEverASetupStep) {
  // The console and the startup capability policy have to agree, or the console calls a drop the
  // policy made on purpose a fault. For every capture and mode the policy starts unprivileged, a
  // fully set up helper without its capability reads as not in use, never as a step to take.
  const std::vector<std::string> captures {"", "auto", "portal", "kwin", "kms", "drm", "wlr", "x11", "nvfbc"};
  const std::vector<std::string> modes {
    "",
    std::string {sdp::k_headless_stream},
    std::string {sdp::k_windowed_stream},
    std::string {sdp::k_host_virtual_display},
    std::string {sdp::k_desktop_takeover},
    std::string {sdp::k_desktop_display},
    std::string {sdp::k_gamescope_stream},
    std::string {sdp::k_headless_dongle},
  };
  int unprivileged = 0;
  for (const auto &virtual_display_backend : {"auto", "kwin"}) {
    for (const auto &capture : captures) {
      for (const auto &mode : modes) {
        if (!portal_capability::requires_unprivileged_process(capture, mode, virtual_display_backend)) {
          continue;
        }
        ++unprivileged;
        auto facts = ready_host();
        facts.capture = sdp::canonical_capture_backend(capture);
        facts.route = sdp::canonical_capture_backend(sdp::capture_for_mode(
          sdp::capture_filled_for_mode(mode, capture),
          mode,
          sdp::legacy_booleans_for_selection(mode).use_cage_compositor,
          false,
          false
        ));
        facts.cap_sys_admin = false;
        facts.capability_set_aside = true;
        const auto state = kr::decide(facts);
        EXPECT_TRUE(state == kr::state_e::not_in_use || state == kr::state_e::mode_sets_kms_aside)
          << "capture=[" << capture << "] mode=[" << mode << "] virtual_display=[" << virtual_display_backend
          << "] reads " << kr::id(state);
      }
    }
  }
  EXPECT_GT(unprivileged, 0) << "the policy starts some configuration unprivileged";
}
#endif

TEST(KmsCaptureReadinessTests, AHostRunningTheHelperWithCaptureSetToKmsIsReady) {
  EXPECT_EQ(kr::route_of(ready_host()), kr::route_e::kms);
  EXPECT_EQ(kr::decide(ready_host()), kr::state_e::ready);
  EXPECT_EQ(kr::id(kr::state_e::ready), "ready");
}

TEST(KmsCaptureReadinessTests, TheCapabilityDecidesWhicheverBinaryHoldsIt) {
  // The Bazzite guide's copy and a hand applied setcap both capture, and saying otherwise would send
  // someone to fix a host that works.
  auto facts = ready_host();
  facts.running_helper = false;
  facts.service_points_at_helper = false;
  facts.helper_installed = false;
  EXPECT_EQ(kr::decide(facts), kr::state_e::ready);
}

TEST(KmsCaptureReadinessTests, AKmsThatFoundNothingIsNotCalledReady) {
  // capture = kms, the capability held, and KMS found no screen it could read, so the automatic
  // search stands in: the route asks for KMS and the stream gets something else.
  auto facts = ready_host();
  facts.route.clear();
  facts.kms_substituted = true;
  EXPECT_EQ(kr::route_of(facts), kr::route_e::kms);
  EXPECT_EQ(kr::decide(facts), kr::state_e::kms_found_nothing);

  // Without the capability, the missing step is the answer, as for any host set to KMS.
  facts = installed_host();
  facts.route.clear();
  facts.kms_substituted = true;
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_enabled);
}

TEST(KmsCaptureReadinessTests, AutodetectWithTheCapabilityCanReachKms) {
  auto facts = ready_host();
  facts.capture.clear();
  facts.route.clear();
  EXPECT_EQ(kr::route_of(facts), kr::route_e::automatic);
  EXPECT_EQ(kr::decide(facts), kr::state_e::automatic);
}

TEST(KmsCaptureReadinessTests, AutodetectWithoutThePackageCapturesAnotherWay) {
  kr::facts_t facts;
  EXPECT_EQ(kr::route_of(facts), kr::route_e::automatic);
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_in_use);

  // An installed package is someone partway into KMS, and the next step is theirs to see.
  facts.helper_installed = true;
  facts.helper_has_capability = true;
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_enabled);
}

TEST(KmsCaptureReadinessTests, AStreamModeThatCapturesAnotherWaySetsKmsAside) {
  // capture = kms in Private Stream, which captures its own compositor through wlroots, or in Host
  // Virtual Display, whose display the portal captures. Neither is a fault of the helper.
  for (const auto *route : {"wlr", "portal"}) {
    auto facts = ready_host();
    facts.route = route;
    EXPECT_EQ(kr::decide(facts), kr::state_e::mode_sets_kms_aside) << route;
    facts.cap_sys_admin = false;
    facts.helper_installed = false;
    EXPECT_EQ(kr::decide(facts), kr::state_e::mode_sets_kms_aside) << route;
  }
}

TEST(KmsCaptureReadinessTests, WithoutThePackageNothingElseMatters) {
  kr::facts_t facts;
  facts.capture = "kms";
  facts.route = "kms";
  facts.service_points_at_helper = true;  // a drop-in left behind by an uninstall
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_installed);
}

TEST(KmsCaptureReadinessTests, AHelperWithoutItsCapabilityNeedsTheReinstall) {
  // The package's install step could not finish, or the filesystem is mounted nosuid. This is the
  // one state where reinstalling is the advice.
  auto facts = pointed_host();
  facts.helper_has_capability = false;
  EXPECT_EQ(kr::decide(facts), kr::state_e::helper_broken);
}

TEST(KmsCaptureReadinessTests, TheHelperRunningWithoutTheCapabilityIsTheResidual) {
  // The file carries it, the process runs the file, and the process did not get it: something in
  // how Polaris was started withholds file capabilities.
  auto facts = ready_host();
  facts.cap_sys_admin = false;
  EXPECT_EQ(kr::decide(facts), kr::state_e::no_capability);

  // A process asked at startup for something that dropped it needs a restart on the new setting.
  facts.capability_set_aside = true;
  EXPECT_EQ(kr::decide(facts), kr::state_e::restart_needed);
}

TEST(KmsCaptureReadinessTests, AnInstalledPackageNobodyTurnedOnNeedsEnableKms) {
  auto facts = installed_host();
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_enabled);
  // --enable-kms adds the account to the group itself, so that is still the one command.
  facts.group_member = false;
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_enabled);
}

TEST(KmsCaptureReadinessTests, ADropInForAnAccountOutsideTheGroupNeedsEnableKms) {
  // No login gives the account a group it is not in.
  auto facts = pointed_host();
  facts.group_member = false;
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_in_group);

  facts.service_points_at_helper = false;
  facts.parked = true;
  EXPECT_EQ(kr::decide(facts), kr::state_e::not_in_group);
}

TEST(KmsCaptureReadinessTests, ASessionWithoutTheGroupWaitsForALoginBeforeAnyRestart) {
  // Restarting now would stop Polaris from starting at all (status=203/EXEC).
  auto facts = pointed_host();
  facts.session_group = ke::session_group_e::not_live;
  EXPECT_EQ(kr::decide(facts), kr::state_e::waiting_for_login);

  // What host setup leaves until the next login: the drop-in parked under a name systemd ignores.
  facts.service_points_at_helper = false;
  facts.parked = true;
  EXPECT_EQ(kr::decide(facts), kr::state_e::waiting_for_login);
  facts.session_group = ke::session_group_e::no_manager;
  EXPECT_EQ(kr::decide(facts), kr::state_e::waiting_for_login);
}

TEST(KmsCaptureReadinessTests, AParkedDropInAfterTheLoginNeedsHostSetupToFinish) {
  auto facts = pointed_host();
  facts.service_points_at_helper = false;
  facts.parked = true;
  EXPECT_EQ(kr::decide(facts), kr::state_e::finish_setup);
}

TEST(KmsCaptureReadinessTests, APolarisStartedOutsideTheServiceNeverReadsTheDropIn) {
  // The desktop entry falls back to the plain binary when the service does not start, and a Polaris
  // started from a terminal runs under the session. Restarting the service changes neither.
  auto facts = pointed_host();
  facts.in_service = false;
  EXPECT_EQ(kr::decide(facts), kr::state_e::outside_service);
}

TEST(KmsCaptureReadinessTests, APointedServiceThatStartedEarlierNeedsARestart) {
  // Also the service still running a helper an update replaced: that path reads "(deleted)", so it
  // is not the file on disk now.
  EXPECT_EQ(kr::decide(pointed_host()), kr::state_e::restart_needed);
}

TEST(KmsCaptureReadinessTests, OnlyAHostPartwayThroughSetupWalksProc) {
  EXPECT_FALSE(kr::needs_session_group(ready_host()));
  EXPECT_FALSE(kr::needs_session_group(installed_host()));
  EXPECT_FALSE(kr::needs_session_group(kr::facts_t {}));
  EXPECT_TRUE(kr::needs_session_group(pointed_host()));
  auto parked = installed_host();
  parked.parked = true;
  parked.group_member = true;
  EXPECT_TRUE(kr::needs_session_group(parked));
  parked.group_member = false;
  EXPECT_FALSE(kr::needs_session_group(parked)) << "not_in_group decides before the manager is read";
}

TEST(KmsCaptureReadinessTests, TheModesTheConsoleNamesForKmsCaptureThroughIt) {
  // The console says Mirror Desktop and the Headless Dongle capture the real screen through KMS once
  // capture is set to KMS, and that Private Stream, Host Virtual Display and Desktop Takeover
  // capture their own display another way. That sentence has to stay what a launch does.
  EXPECT_TRUE(kr::kms_possible_in_mode(sdp::k_desktop_display));
  EXPECT_TRUE(kr::kms_possible_in_mode(sdp::k_headless_dongle));
  EXPECT_TRUE(kr::kms_possible_in_mode(sdp::k_gamescope_stream));
  EXPECT_FALSE(kr::kms_possible_in_mode(sdp::k_headless_stream));
  EXPECT_FALSE(kr::kms_possible_in_mode(sdp::k_windowed_stream));
  EXPECT_FALSE(kr::kms_possible_in_mode(sdp::k_host_virtual_display));
  EXPECT_FALSE(kr::kms_possible_in_mode(sdp::k_desktop_takeover));
}

TEST(KmsCaptureReadinessTests, TheStartupRecordIsWhatTheConsoleReads) {
  const bool before = kr::capability_set_aside();
  kr::note_capability_set_aside(true);
  EXPECT_TRUE(kr::capability_set_aside());
  kr::note_capability_set_aside(false);
  EXPECT_FALSE(kr::capability_set_aside());
  kr::note_capability_set_aside(before);
}

TEST(KmsCaptureReadinessTests, ReadsThePermittedSetNotTheEffectiveOne) {
  // An idle helper: permitted, and raised into the effective set only around a framebuffer read.
  const std::string idle =
    "Name:\tpolaris-kms\n"
    "CapInh:\t0000000000000000\n"
    "CapPrm:\t0000000000200000\n"
    "CapEff:\t0000000000000000\n"
    "CapBnd:\t000001ffffffffff\n";
  EXPECT_TRUE(kr::cap_sys_admin_permitted(idle));

  // The plain binary, or the helper after the portal drop: nothing permitted, whatever the bounding set allows.
  const std::string plain =
    "Name:\tpolaris\n"
    "CapPrm:\t0000000000000000\n"
    "CapEff:\t0000000000000000\n"
    "CapBnd:\t000001ffffffffff\n";
  EXPECT_FALSE(kr::cap_sys_admin_permitted(plain));

  // Another capability alone is not this one.
  EXPECT_FALSE(kr::cap_sys_admin_permitted("CapPrm:\t0000000000800000\n"));
  EXPECT_TRUE(kr::cap_sys_admin_permitted("CapPrm:\t000001ffffffffff\n"));
  EXPECT_FALSE(kr::cap_sys_admin_permitted(""));
  EXPECT_FALSE(kr::cap_sys_admin_permitted("CapPrm:\tnot-hex\n"));
}

TEST(KmsCaptureReadinessTests, OnlyThePolarisServicesOwnCgroupIsTheService) {
  EXPECT_TRUE(platf::user_unit::in_polaris_service("0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service\n"));
  EXPECT_TRUE(platf::user_unit::in_polaris_service("12:pids:/\n0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service"));
  // The desktop entry's own unit, a terminal's session scope, and another service are not it.
  EXPECT_FALSE(platf::user_unit::in_polaris_service("0::/user.slice/user-1000.slice/user@1000.service/app.slice/app-dev.polaris\\x2dstream.app.Polaris@7.service\n"));
  EXPECT_FALSE(platf::user_unit::in_polaris_service("0::/user.slice/user-1000.slice/session-4.scope\n"));
  EXPECT_FALSE(platf::user_unit::in_polaris_service("0::/user.slice/user-1000.slice/user@1000.service/app.slice/polaris.service.d\n"));
  EXPECT_FALSE(platf::user_unit::in_polaris_service(""));
}

TEST(KmsCaptureReadinessTests, TheSystemStatsReadTheFactsFromThisProcess) {
  // The decision is only as good as what the route and main() hand it.
  const auto confighttp = read_source("src/confighttp.cpp");
  ASSERT_FALSE(confighttp.empty());
  EXPECT_NE(confighttp.find("output[\"kms_capture\"] = report;"), std::string::npos);
  EXPECT_NE(confighttp.find("kms.cap_sys_admin = kr::cap_sys_admin_permitted(*status);"), std::string::npos)
    << "the capability is this process's own, from /proc/self/status";
  EXPECT_NE(confighttp.find("kms.capability_set_aside = kr::capability_set_aside();"), std::string::npos);
  EXPECT_NE(confighttp.find("kms.route = stream_display_policy::canonical_capture_backend(stream_display_policy::capture_for_launch_into_current_mode());"), std::string::npos)
    << "the route a launch into the host's mode asks for, the one the Doctor reads";
  EXPECT_NE(confighttp.find("running_replaced_helper = running->string() == kms_helper.string() + \" (deleted)\";"), std::string::npos)
    << "a helper an update replaced is still the helper the console words its advice for";
  EXPECT_NE(confighttp.find("report[\"streams_running\"] = streams.clients.size();"), std::string::npos)
    << "the readout knows a stream runs when the host names no single capture";

  const auto main_cpp = read_source("src/main.cpp");
  const auto drop = main_cpp.find("portal_capability::prepare_process_for_capture(");
  const auto record = main_cpp.find("platf::kms_readiness::note_capability_set_aside(portal_capability::requires_unprivileged_process(");
  ASSERT_NE(drop, std::string::npos);
  ASSERT_NE(record, std::string::npos) << "main() records the startup policy the console reads";
  EXPECT_GT(record, drop);
}

#endif
