/**
 * @file tests/unit/test_network.cpp
 * @brief Test src/network.*
 */
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include <src/network.h>
#include <src/config.h>
#include <src/platform/common.h>

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

TEST(ClientNetworkPath, SharedAddressSpaceDoesNotProveLocalTrust) {
  for (const auto *address : {"100.64.0.0", "100.64.0.1", "100.100.100.100",
                             "100.127.255.254", "100.127.255.255",
                             "::ffff:100.64.0.0", "::ffff:100.100.100.100", "::ffff:100.127.255.255"}) {
    SCOPED_TRACE(address);
    EXPECT_EQ(net::from_address(address), net::WAN);
    EXPECT_GT(net::from_address(address), net::LAN); // LAN-only origin gates reject it.
    EXPECT_EQ(net::describe_client_network_path(address), "cgnat");
  }
  for (const auto *address : {"100.63.255.255", "100.128.0.0", "::ffff:100.63.255.255", "::ffff:100.128.0.0"}) {
    EXPECT_EQ(net::from_address(address), net::WAN);
    EXPECT_EQ(net::describe_client_network_path(address), "public");
  }
}

TEST(NetworkAccessPolicy, PreservesPrivateLanAndLoopback) {
  for (const auto *address : {"10.1.2.3", "172.16.1.2", "192.168.1.2",
                             "169.254.1.2", "fd00::1", "fe80::1",
                             "::ffff:192.168.1.2"}) {
    SCOPED_TRACE(address);
    EXPECT_EQ(net::from_address(address), net::LAN);
  }
  EXPECT_EQ(net::from_address("127.0.0.1"), net::PC);
  EXPECT_EQ(net::from_address("::1"), net::PC);
}

TEST(NetworkAccessPolicy, SharedSpaceUsesWanEncryptionPolicy) {
  const auto old_lan = config::stream.lan_encryption_mode;
  const auto old_wan = config::stream.wan_encryption_mode;
  auto restore = util::fail_guard([&] {
    config::stream.lan_encryption_mode = old_lan;
    config::stream.wan_encryption_mode = old_wan;
  });
  config::stream.lan_encryption_mode = config::ENCRYPTION_MODE_NEVER;
  config::stream.wan_encryption_mode = config::ENCRYPTION_MODE_MANDATORY;
  EXPECT_EQ(net::encryption_mode_for_address(boost::asio::ip::make_address("100.100.100.100")),
            config::ENCRYPTION_MODE_MANDATORY);
  EXPECT_EQ(net::encryption_mode_for_address(boost::asio::ip::make_address("::ffff:100.100.100.100")),
            config::ENCRYPTION_MODE_MANDATORY);
  EXPECT_EQ(net::encryption_mode_for_address(boost::asio::ip::make_address("192.168.1.2")),
            config::ENCRYPTION_MODE_NEVER);
}

TEST(NetworkPathProbe, ClassifiesNativeProbeHostsWithoutTouchingTheNetwork) {
  EXPECT_EQ(net::network_path_probe_classification("127.0.0.1"), "pc");
  EXPECT_EQ(net::network_path_probe_classification("192.168.50.25"), "lan");
  EXPECT_EQ(net::network_path_probe_classification("fd7a:115c:a1e0::1"), "lan");
  EXPECT_EQ(net::network_path_probe_classification("100.100.100.100"), "wan");
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

namespace {
  // A desktop host: a wired LAN card holding the default route, Wi-Fi beside it, a Docker bridge,
  // Tailscale and WireGuard tunnels, and loopback. MACs come from the RFC 7042 documentation range.
  std::vector<net::host_interface_t> fake_host_interfaces() {
    return {
      {"lo", "00:00:00:00:00:00", {"127.0.0.1", "::1"}},
      {"eno2", "00:00:5e:00:53:1a", {"192.168.1.20", "fd00::20", "fe80::200:5eff:fe00:531a"}},
      {"wlo1", "00:00:5e:00:53:2b", {"192.168.1.21", "fe80::200:5eff:fe00:532b"}},
      {"docker0", "00:00:5e:00:53:3c", {"172.17.0.1"}},
      {"tailscale0", "", {"100.101.102.103", "fd7a:115c:a1e0::1234"}},
      {"wg0", "", {"10.8.0.1"}},
    };
  }

  constexpr std::string_view proc_net_route_header =
    "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n";
}  // namespace

TEST(WakeOnLanMac, AcceptsOnlyAddressesAMagicPacketCanTarget) {
  EXPECT_TRUE(net::is_wake_on_lan_mac("00:00:5e:00:53:1a"));
  EXPECT_TRUE(net::is_wake_on_lan_mac("00:00:5E:00:53:1A"));
  // WireGuard and Tailscale read as an empty line, loopback and the placeholder as zeros.
  EXPECT_FALSE(net::is_wake_on_lan_mac(""));
  EXPECT_FALSE(net::is_wake_on_lan_mac("00:00:00:00:00:00"));
  EXPECT_FALSE(net::is_wake_on_lan_mac(net::no_wake_on_lan_mac));
  EXPECT_FALSE(net::is_wake_on_lan_mac("ff:ff:ff:ff:ff:ff"));
  // An InfiniBand address is 20 octets, more than a magic packet carries.
  EXPECT_FALSE(net::is_wake_on_lan_mac("00:00:00:67:fe:80:00:00:00:00:00:00:00:02:c9:03:00:0e:a4:31"));
  EXPECT_FALSE(net::is_wake_on_lan_mac("00-00-5e-00-53-1a"));
  EXPECT_FALSE(net::is_wake_on_lan_mac("00:00:5e:00:53:1g"));
  EXPECT_FALSE(net::is_wake_on_lan_mac("00:00:5e:00:53:1a "));
}

TEST(WakeOnLanMac, ReadsDefaultRoutesFromProcNetRoute) {
  // A one-card host: one default route on eno2, then connected routes.
  const auto single = std::string {proc_net_route_header} +
                      "eno2\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
                      "eno2\t0001A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
                      "docker0\t000011AC\t00000000\t0001\t0\t0\t0\t0000FFFF\t0\t0\t0\n";
  EXPECT_EQ(net::default_route_interfaces(single), std::vector<std::string>({"eno2"}));

  // Lowest metric first and each interface once. A reject route and a line cut short are left
  // out. The kernel writes RTF_UP on every route, one on a card with its cable out too, so the
  // docker0 route stays in and link state is left to the interface table.
  const auto several = std::string {proc_net_route_header} +
                       "wlo1\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
                       "wg0\t00000000\t00000000\t0001\t0\t0\t50\t00000000\t0\t0\t0\n"
                       "eno2\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
                       "eno2\t00000000\t0101A8C0\t0003\t0\t0\t700\t00000000\t0\t0\t0\n"
                       "virbr0\t00000000\t00000000\t0201\t0\t0\t10\t00000000\t0\t0\t0\n"
                       "docker0\t00000000\t010011AC\t0003\t0\t0\t20\t00000000\t0\t0\t0\n"
                       "truncated\t00000000\n";
  EXPECT_EQ(net::default_route_interfaces(several), std::vector<std::string>({"docker0", "wg0", "eno2", "wlo1"}));

  EXPECT_TRUE(net::default_route_interfaces("").empty());
  EXPECT_TRUE(net::default_route_interfaces(proc_net_route_header).empty());
}

TEST(WakeOnLanMac, TunnelRequestsGetTheLanCardHoldingTheDefaultRoute) {
  const auto interfaces = fake_host_interfaces();
  const std::vector<std::string> default_route {"eno2"};
  // Tailscale, WireGuard and loopback have no MAC of their own. The client reached the host
  // through one of them, but a magic packet has to reach the wired card.
  for (const auto *address : {"100.101.102.103", "fd7a:115c:a1e0::1234", "10.8.0.1", "127.0.0.1", "::1"}) {
    SCOPED_TRACE(address);
    EXPECT_EQ(net::wake_on_lan_mac(interfaces, address, default_route), "00:00:5e:00:53:1a");
  }
  // So does an address no interface holds any more.
  EXPECT_EQ(net::wake_on_lan_mac(interfaces, "192.0.2.10", default_route), "00:00:5e:00:53:1a");
}

TEST(WakeOnLanMac, KeepsTheCardTheClientReached) {
  const auto interfaces = fake_host_interfaces();
  const std::vector<std::string> default_route {"eno2"};
  EXPECT_EQ(net::wake_on_lan_mac(interfaces, "192.168.1.20", default_route), "00:00:5e:00:53:1a");
  EXPECT_EQ(net::wake_on_lan_mac(interfaces, "192.168.1.21", default_route), "00:00:5e:00:53:2b");
  EXPECT_EQ(net::wake_on_lan_mac(interfaces, "172.17.0.1", default_route), "00:00:5e:00:53:3c");
  // boost writes a link-local address with its zone, and the interface table has none.
  EXPECT_EQ(net::wake_on_lan_mac(interfaces, "fe80::200:5eff:fe00:532b%wlo1", default_route), "00:00:5e:00:53:2b");
}

TEST(WakeOnLanMac, SkipsDefaultRouteCardsWithNoLink) {
  // The wired card keeps its default route, the lower metric one, with its cable out, and the
  // host reaches the network over Wi-Fi. A magic packet cannot reach the unplugged card.
  auto interfaces = fake_host_interfaces();
  for (auto &candidate : interfaces) {
    if (candidate.name == "eno2") {
      candidate.link_up = false;
    }
  }
  const std::vector<std::string> default_route {"eno2", "wlo1"};
  EXPECT_EQ(net::wake_on_lan_mac(interfaces, "100.101.102.103", default_route), "00:00:5e:00:53:2b");

  // No default route card with a link: the placeholder, so the client keeps the MAC it stored.
  EXPECT_EQ(net::wake_on_lan_mac(interfaces, "100.101.102.103", {"eno2"}), net::no_wake_on_lan_mac);
}

TEST(WakeOnLanMac, SkipsDefaultRoutesWithoutAMacAndNeverReportsEmpty) {
  const auto interfaces = fake_host_interfaces();
  // A tunnel default route ahead of the LAN ones: the first card with a MAC answers.
  EXPECT_EQ(net::wake_on_lan_mac(interfaces, "100.101.102.103", {"wg0", "tailscale0", "wlo1", "eno2"}), "00:00:5e:00:53:2b");

  // Nothing usable anywhere: the placeholder clients skip, so they keep the MAC they stored.
  for (const auto &default_route : {std::vector<std::string> {}, std::vector<std::string> {"wg0"}, std::vector<std::string> {"missing0"}}) {
    const auto mac = net::wake_on_lan_mac(interfaces, "100.101.102.103", default_route);
    EXPECT_EQ(mac, net::no_wake_on_lan_mac);
    EXPECT_FALSE(mac.empty());
  }
  EXPECT_EQ(net::wake_on_lan_mac({}, "192.168.1.20", {"eno2"}), net::no_wake_on_lan_mac);
}

#ifdef __linux__
TEST(WakeOnLanMac, LoopbackRequestGetsTheLinkedDefaultRouteCardOnThisHost) {
  // The Linux lookup itself, against this machine: loopback has only the zeros MAC, so a request
  // on 127.0.0.1 takes the fallback. The answer is the first default route card that has a link
  // and a usable MAC, read here from /proc/net/route and sysfs, or the placeholder on a runner
  // with no such card.
  const auto read_first_line = [](const std::string &path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
  };
  std::stringstream routes;
  if (std::ifstream route_file("/proc/net/route"); route_file) {
    routes << route_file.rdbuf();
  }
  std::string expected {net::no_wake_on_lan_mac};
  for (const auto &name : net::default_route_interfaces(routes.str())) {
    // IFF_RUNNING is set exactly when the operational state is up or unknown.
    const auto operstate = read_first_line("/sys/class/net/" + name + "/operstate");
    const auto mac = read_first_line("/sys/class/net/" + name + "/address");
    if ((operstate == "up" || operstate == "unknown") && net::is_wake_on_lan_mac(mac)) {
      expected = mac;
      break;
    }
  }
  SCOPED_TRACE(routes.str());
  EXPECT_EQ(platf::get_mac_address("127.0.0.1"), expected);
}
#endif
