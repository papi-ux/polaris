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
  EXPECT_NE(contract.at("mode_reason").get<std::string>().find("already configured for Headless Stream"), std::string::npos);
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
  EXPECT_NE(reason.find("private headless session"), std::string::npos);
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
