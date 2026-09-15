/**
 * @file tests/unit/test_port_holder.cpp
 * @brief Finding who listens on a port Polaris could not bind.
 */
#include <src/network.h>

#include <gtest/gtest.h>

TEST(PortHolder, ParsesListeningSocketsOnThePortOnly) {
  // 0xBB8A is 48010, the RTSP port. Only state 0A (LISTEN) on that port counts.
  const std::string proc_net_tcp =
    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
    "   0: 00000000:BB8A 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 12345 1 0000000000000000 100 0 0 10 0\n"
    "   1: 0100007F:BB8A 0100007F:9C40 01 00000000:00000000 00:00000000 00000000  1000        0 12346 1 0000000000000000 20 4 30 10 -1\n"
    "   2: 00000000:BB7A 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 12347 1 0000000000000000 100 0 0 10 0\n"
    "   3: 00000000:BB8A 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 99 1 0000000000000000 100 0 0 10 0\n";

  const auto inodes = net::listening_socket_inodes(proc_net_tcp, 48010);
  ASSERT_EQ(inodes.size(), 2u);
  EXPECT_EQ(inodes[0], 12345u);
  EXPECT_EQ(inodes[1], 99u);
  EXPECT_TRUE(net::listening_socket_inodes(proc_net_tcp, 47989).empty());
}

TEST(PortHolder, ParsesTheIpv6Table) {
  const std::string proc_net_tcp6 =
    "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
    "   0: 00000000000000000000000000000000:BB8A 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 555 1 0000000000000000 100 0 0 10 0\n";
  const auto inodes = net::listening_socket_inodes(proc_net_tcp6, 48010);
  ASSERT_EQ(inodes.size(), 1u);
  EXPECT_EQ(inodes[0], 555u);
}

TEST(PortHolder, IgnoresGarbage) {
  EXPECT_TRUE(net::listening_socket_inodes("", 48010).empty());
  EXPECT_TRUE(net::listening_socket_inodes("header only\n", 48010).empty());
  EXPECT_TRUE(net::listening_socket_inodes("header\n   0: nonsense\n", 48010).empty());
}

TEST(PortHolder, AnUnboundPortHasNoHolder) {
  // Nothing listens on a port this high in a test environment; the description must be empty
  // rather than inventing a suspect.
  EXPECT_TRUE(net::describe_port_holder(65123).empty());
}
