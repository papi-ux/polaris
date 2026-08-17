/**
 * @file tests/unit/test_launch_mode_contract.cpp
 * @brief Tests for the Polaris v1 game launch-mode recommendation contract.
 */

#include <src/nvhttp.h>

#include <gtest/gtest.h>

TEST(LaunchModeContractTests, HostHeadlessConfigurationWinsOverPerGameVirtualDisplayPreference) {
  const auto contract = nvhttp::build_launch_mode_contract_for_tests(
    true,
    "Indiana Jones and the Great Circle",
    true,
    true
  );

  EXPECT_EQ(contract.at("preferred_mode"), "host_virtual_display");
  EXPECT_EQ(contract.at("recommended_mode"), "headless_stream");
  bool allowed_host_virtual_display = false;
  for (const auto &mode : contract.at("allowed_modes")) {
    allowed_host_virtual_display = allowed_host_virtual_display || mode == "host_virtual_display";
  }
  EXPECT_TRUE(allowed_host_virtual_display);
  EXPECT_NE(contract.at("mode_reason").get<std::string>().find("already configured for Private Stream"), std::string::npos);
}

TEST(LaunchModeContractTests, SteamBigPictureOnHeadlessHostExplainsPrivateDesktopSafety) {
  const auto contract = nvhttp::build_launch_mode_contract_for_tests(
    true,
    "Steam Big Picture",
    true,
    true
  );

  EXPECT_EQ(contract.at("preferred_mode"), "host_virtual_display");
  EXPECT_EQ(contract.at("recommended_mode"), "headless_stream");
  const auto reason = contract.at("mode_reason").get<std::string>();
  EXPECT_NE(reason.find("Private Stream session"), std::string::npos);
  EXPECT_NE(reason.find("physical desktop"), std::string::npos);
}

TEST(LaunchModeContractTests, PerGameVirtualDisplayPreferenceIsRecommendedWhenHostIsNotHeadless) {
  const auto contract = nvhttp::build_launch_mode_contract_for_tests(
    true,
    "Indiana Jones and the Great Circle",
    true,
    false
  );

  EXPECT_EQ(contract.at("preferred_mode"), "host_virtual_display");
  EXPECT_EQ(contract.at("recommended_mode"), "host_virtual_display");
}

#ifdef __linux__
  #include <src/platform/linux/stream_display_policy.h>

// The client-settings POST validator used to hardcode four ids while
// allowed_modes advertised the full registry, so gamescope_stream and
// headless_dongle were advertised and then rejected with 400. The validator now
// delegates to stream_display_policy::selection_valid - the same check
// apply_selection itself runs - and this pins that they can never disagree.
TEST(LaunchModeContractTests, EveryAdvertisedModeVerdictMatchesTheValidator) {
  for (const bool virtual_display_available : {false, true}) {
    for (const auto &option : stream_display_policy::mode_options(virtual_display_available)) {
      std::string error;
      const bool valid = stream_display_policy::selection_valid(option.value, error);
      if (option.available) {
        EXPECT_TRUE(valid) << option.value << ": advertised available but rejected: " << error;
      } else {
        EXPECT_FALSE(valid) << option.value << ": advertised unavailable but accepted";
        EXPECT_FALSE(error.empty()) << option.value << ": rejection must carry the host's reason";
      }
    }
  }
}

TEST(LaunchModeContractTests, ReservedAndUnknownIdsAreRejectedWithGuidance) {
  for (const auto *id : {"family_isolated", "headless_evdi", "not_a_mode"}) {
    std::string error;
    EXPECT_FALSE(stream_display_policy::selection_valid(id, error)) << id;
    EXPECT_NE(error.find("known stream path id"), std::string::npos) << id;
  }
}

TEST(SessionStreamMode, AcceptsRegistryModesItCanRunPerSession) {
  // Deterministic on CI: these registry paths are statically available and do
  // not depend on host probes (gamescope/EVDI availability is environmental,
  // so those ids are deliberately not asserted here).
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("headless_stream"), "headless_stream");
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("windowed_stream"), "windowed_stream");
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("desktop_display"), "desktop_display");
}

TEST(SessionStreamMode, DerivesSessionOverridabilityFromTopologyNotAnIdList) {
  // The rule a client is told and the rule the gate enforces have to be the
  // same rule, or the client offers a mode the host silently drops. It is
  // derived from the path's topology so a future swapping path inherits it
  // without editing a list: swapping the host's primary output rearranges the
  // machine itself, which is a host decision rather than a per-launch one.
  EXPECT_FALSE(stream_display_policy::selection_session_overridable("headless_dongle"));

  EXPECT_TRUE(stream_display_policy::selection_session_overridable("headless_stream"));
  EXPECT_TRUE(stream_display_policy::selection_session_overridable("windowed_stream"));
  EXPECT_TRUE(stream_display_policy::selection_session_overridable("desktop_display"));

  // Unknown ids are not "restricted", they are simply not paths. The gate
  // checks validity first so they report as unknown rather than as reserved.
  EXPECT_FALSE(stream_display_policy::selection_session_overridable("garbage_mode"));
}

TEST(SessionStreamMode, RejectsDongleReservedUnknownAndEmpty) {
  // headless_dongle swaps host output topology: host-default-only by design.
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("headless_dongle"), "");
  // Reserved/unknown ids and absence all resolve to "host default applies".
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("family_isolated"), "");
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("garbage_mode"), "");
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests(""), "");
}
#endif
