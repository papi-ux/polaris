/**
 * @file tests/unit/test_network.cpp
 * @brief Test src/network.*
 */
#include "../tests_common.h"

#include <src/network.h>

struct MdnsInstanceNameTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(MdnsInstanceNameTest, Run) {
  auto [input, expected] = GetParam();
  ASSERT_EQ(net::mdns_instance_name(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  MdnsInstanceNameTests,
  MdnsInstanceNameTest,
  testing::Values(
    std::make_tuple("shortname-123", "shortname-123"),
    std::make_tuple("space 123", "space-123"),
    std::make_tuple("hostname.domain.test", "hostname"),
    std::make_tuple("&", "Apollo"),
    std::make_tuple("", "Apollo"),
    std::make_tuple("😁", "Apollo"),
    std::make_tuple(std::string(128, 'a'), std::string(63, 'a'))
  )
);

TEST(NetworkPathProbe, ClassifiesNativeProbeHostsWithoutTouchingTheNetwork) {
  EXPECT_EQ(net::network_path_probe_classification("127.0.0.1"), "pc");
  EXPECT_EQ(net::network_path_probe_classification("192.168.50.25"), "lan");
  EXPECT_EQ(net::network_path_probe_classification("fd7a:115c:a1e0::1"), "lan");
  EXPECT_EQ(net::network_path_probe_classification("203.0.113.40"), "wan");
  EXPECT_EQ(net::network_path_probe_classification("polaris-host.local"), "lan");
  EXPECT_EQ(net::network_path_probe_classification("tailscale-host.ts.net"), "vpn");
}

TEST(NetworkPathProbe, BuildsSafeNormalUserPortContracts) {
  const auto ports = net::network_path_probe_ports(47989);

  ASSERT_EQ(ports.size(), 5);
  EXPECT_EQ(ports[0].key, "control_https");
  EXPECT_EQ(ports[0].transport, "tcp");
  EXPECT_EQ(ports[0].port, 47990);
  EXPECT_EQ(ports[1].key, "rtsp_setup");
  EXPECT_EQ(ports[1].port, 48010);
  EXPECT_EQ(ports[2].key, "control_udp");
  EXPECT_EQ(ports[2].transport, "udp");
  EXPECT_EQ(ports[2].port, 47999);
  EXPECT_EQ(ports[3].key, "video_udp");
  EXPECT_EQ(ports[3].port, 47998);
  EXPECT_EQ(ports[4].key, "audio_udp");
  EXPECT_EQ(ports[4].port, 48000);
}
