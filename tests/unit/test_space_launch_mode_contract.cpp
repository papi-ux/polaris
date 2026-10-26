/** Literal wire pin: deliberately independent of generated names and schema. */
#include <src/nvhttp.h>
#include <gtest/gtest.h>
#include <set>
#include <string>

#ifdef __linux__
TEST(SpaceLaunchModeContractTests, ProductionObjectKeepsExactlyFourLiteralKeys) {
  const auto value = nvhttp::space_launch_mode_json_for_tests("Owned Space");
  const std::set<std::string> expected {"allowed_modes", "mode_reason", "preferred_mode", "recommended_mode"};
  std::set<std::string> actual;
  for (const auto &[key, unused] : value.items()) actual.insert(key);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(value.at("preferred_mode"), "gamescope_stream");
  EXPECT_EQ(value.at("recommended_mode"), "gamescope_stream");
  EXPECT_EQ(value.at("allowed_modes"), nlohmann::json::array({"gamescope_stream"}));
  EXPECT_EQ(value.at("mode_reason"), "Runs in Owned Space");
  for (const auto *key : {"follows_host_default", "launch_as", "launch_as_available", "launch_as_unavailable_reason"}) {
    EXPECT_FALSE(value.contains(key)) << key;
  }
}
#endif
