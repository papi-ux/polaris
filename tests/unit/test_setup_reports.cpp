/**
 * @file tests/unit/test_setup_reports.cpp
 * @brief The first-run setup reports, collected from the host the tests run on.
 */
#include <src/confighttp.h>
#include <src/utility.h>

#ifdef __linux__
  #include <src/platform/linux/misc.h>
#endif

#include <gtest/gtest.h>

#include <string>

TEST(SetupReportTests, DescribesTheGpusAndTheEncoderBeforeAnyStream) {
#ifdef __linux__
  // The test build pins the encoder node to nothing; this report is about the real host.
  platf::set_effective_encoder_render_device_for_tests(std::nullopt);
  auto restore = util::fail_guard([] {
    platf::set_effective_encoder_render_device_for_tests(std::string {});
  });
#endif
  const auto report = confighttp::setup_hardware_report();
  RecordProperty("setup_hardware", report.dump());

  EXPECT_TRUE(report.at("status").get<bool>());
  EXPECT_EQ(report.at("platform"), POLARIS_PLATFORM);
  ASSERT_TRUE(report.at("gpus").is_array());
  ASSERT_TRUE(report.at("encoder_choices").is_array());
  ASSERT_TRUE(report.at("advice").is_array());
  EXPECT_FALSE(report.at("encoder").at("expected").get<std::string>().empty());

  int selected = 0;
  for (const auto &gpu : report.at("gpus")) {
    EXPECT_EQ(gpu.at("render_node").get<std::string>().rfind("/dev/dri/renderD", 0), 0u) << gpu.dump();
    selected += gpu.at("selected").get<bool>() ? 1 : 0;
  }
  EXPECT_LE(selected, 1);
  if (report.at("gpus").empty()) {
    EXPECT_EQ(report.at("encoder").at("expected"), "software");
  }
}

TEST(SetupReportTests, OffersOnlyPrivateNetworksToTrust) {
  const auto report = confighttp::setup_networks_report();
  RecordProperty("setup_networks", report.dump());

  EXPECT_TRUE(report.at("status").get<bool>());
  ASSERT_TRUE(report.at("networks").is_array());
  for (const auto &network : report.at("networks")) {
    const auto cidr = network.at("cidr").get<std::string>();
    const bool private_range = cidr.rfind("10.", 0) == 0 || cidr.rfind("192.168.", 0) == 0 || cidr.rfind("172.", 0) == 0;
    EXPECT_TRUE(private_range) << cidr;
    EXPECT_NE(cidr.find('/'), std::string::npos) << cidr;
    EXPECT_FALSE(network.at("interface").get<std::string>().empty());
  }
}
