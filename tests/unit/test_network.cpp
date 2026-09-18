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

struct ClientNetworkPathTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(ClientNetworkPathTest, Run) {
  auto [address, expected] = GetParam();
  EXPECT_EQ(net::describe_client_network_path(address), expected);
}

INSTANTIATE_TEST_SUITE_P(
  ClientNetworkPathTests,
  ClientNetworkPathTest,
  testing::Values(
    // The two client addresses from the support bundle that prompted this.
    std::make_tuple("192.168.1.192", "lan"),
    std::make_tuple("100.109.196.18", "cgnat"),
    std::make_tuple("10.0.0.4", "lan"),
    std::make_tuple("172.20.1.9", "lan"),
    std::make_tuple("169.254.3.4", "link-local"),
    std::make_tuple("127.0.0.1", "loopback"),
    std::make_tuple("8.8.8.8", "public"),
    std::make_tuple("fd7a:115c:a1e0::5", "tailscale"),
    std::make_tuple("fd00::1", "lan"),
    std::make_tuple("fe80::1", "link-local"),
    std::make_tuple("::1", "loopback"),
    std::make_tuple("2606:4700::1111", "public"),
    // A mapped address is the IPv4 address it carries.
    std::make_tuple("::ffff:100.109.196.18", "cgnat"),
    std::make_tuple("not an address", "unknown"),
    std::make_tuple("", "unknown")
  )
);

TEST(ClientNetworkPath, LeavesAccessDecisionsTreatingTheSharedRangeAsLan) {
  // The diagnostic split must not leak into access control. from_address decides
  // what a nearby client may do, and it counts the Tailscale range as LAN on
  // purpose; only the explanation of a stream tells the two apart.
  EXPECT_EQ(net::from_address("100.109.196.18"), net::LAN);
  EXPECT_EQ(net::describe_client_network_path("100.109.196.18"), "cgnat");
}

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
