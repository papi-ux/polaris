/**
 * @file src/network.h
 * @brief Declarations for networking related functions.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// lib includes
#include <boost/asio.hpp>
#include <enet/enet.h>

// local includes
#include "utility.h"

namespace net {
  void free_host(ENetHost *host);

  /**
   * @brief Map a specified port based on the base port.
   * @param port The port to map as a difference from the base port.
   * @return The mapped port number.
   * @examples
   * std::uint16_t mapped_port = net::map_port(1);
   * @examples_end
   * @todo Ensure port is not already in use by another application.
   */
  std::uint16_t map_port(int port);

  using host_t = util::safe_ptr<ENetHost, free_host>;
  using peer_t = ENetPeer *;
  using packet_t = util::safe_ptr<ENetPacket, enet_packet_destroy>;

  enum net_e : int {
    PC,  ///< PC
    LAN,  ///< LAN
    WAN  ///< WAN
  };

  enum af_e : int {
    IPV4,  ///< IPv4 only
    BOTH  ///< IPv4 and IPv6
  };

  net_e from_enum_string(const std::string_view &view);
  std::string_view to_enum_string(net_e net);

  // Address-only policy. Shared IPv4 space (100.64/10), including VPN peers,
  // receives WAN policy; a numeric prefix cannot prove a trusted tunnel.
  net_e from_address(const std::string_view &view);

  /**
   * @brief Describe the path a client reached this host by, for diagnostics only.
   * @details Never use this for access decisions; from_address does that, and it
   *          treats the shared 100.64.0.0/10 range as WAN.
   * @return One of loopback, lan, cgnat (the shared IPv4 range, which Tailscale
   *         uses), tailscale (its IPv6 range), link-local, public, or unknown when
   *         the text is not an address.
   */
  std::string_view describe_client_network_path(const std::string_view &address);

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port);

  /**
   * @brief Get the address family enum value from a string.
   * @param view The config option value.
   * @return The address family enum value.
   */
  af_e af_from_enum_string(const std::string_view &view);

  /**
   * @brief Get the wildcard binding address for a given address family.
   * @param af Address family.
   * @return Normalized address.
   */
  std::string_view af_to_any_address_string(af_e af);

  /**
   * @brief Convert an address to a normalized form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address.
   */
  boost::asio::ip::address normalize_address(boost::asio::ip::address address);

  /**
   * @brief Get the given address in normalized string form.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize.
   * @return Normalized address in string form.
   */
  std::string addr_to_normalized_string(boost::asio::ip::address address);

  /**
   * @brief Get the given address in a normalized form for the host portion of a URL.
   * @details Normalization converts IPv4-mapped IPv6 addresses into IPv4 addresses.
   * @param address The address to normalize and escape.
   * @return Normalized address in URL-escaped string.
   */
  std::string addr_to_url_escaped_string(boost::asio::ip::address address);

  /**
   * @brief Get the encryption mode for the given remote endpoint address.
   * @param address The address used to look up the desired encryption mode.
   * @return The WAN or LAN encryption mode, based on the provided address.
   */
  int encryption_mode_for_address(boost::asio::ip::address address);

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname);

  struct network_path_probe_port_t {
    std::string key;
    std::string label;
    std::uint16_t port;
    std::string transport;
  };

  /**
   * @brief Classify a host/address for safe network-path self-test copy.
   * @details This is intentionally side-effect free; callers may combine it with
   * bounded probes, but classification itself never opens sockets.
   */
  std::string network_path_probe_classification(const std::string_view &host);

  /**
   * @brief Build the normal-user-safe Polaris/Moonlight port contract.
   * @param base_port Configured Sunshine/Polaris base port, normally config::sunshine.port.
   */
  std::vector<network_path_probe_port_t> network_path_probe_ports(std::uint16_t base_port);

  /**
   * @brief Socket inodes listening on a TCP port, parsed from /proc/net/tcp or /proc/net/tcp6 text.
   * @param proc_net_tcp The file's content: a header line, then one socket per line.
   * @param port The local port, host byte order.
   */
  std::vector<unsigned long> listening_socket_inodes(std::string_view proc_net_tcp, std::uint16_t port);

  /**
   * @brief Who holds a TCP port this process could not bind, as far as this account can see.
   * @return "held by <comm> (pid N)" when the owner is visible, a sentence naming the usual
   *         suspects when a listener exists but belongs to another account, empty when nothing
   *         listens on the port (the bind failed for another reason). Linux only; empty elsewhere.
   */
  std::string describe_port_holder(std::uint16_t port);

  /**
   * @brief What serverinfo reports as the MAC address when the host has none to give.
   * @details Moonlight clients, Nova among them, skip exactly this value and keep the MAC they
   *          stored earlier, so it is how serverinfo says "none".
   */
  inline constexpr std::string_view no_wake_on_lan_mac = "00:00:00:00:00:00";

  /**
   * @brief One of the host's network interfaces, as the Wake-on-LAN MAC choice sees it.
   */
  struct host_interface_t {
    std::string name;  ///< Interface name, such as "eno1" or "wg0".
    std::string mac;  ///< Hardware address as the OS reports it: empty for WireGuard or Tailscale, zeros for loopback.
    std::vector<std::string> addresses;  ///< Every IP address on it, without an IPv6 zone.
    bool link_up = true;  ///< Whether it has a link: a card with its cable out cannot receive a magic packet.
  };

  /**
   * @brief Whether a string is a MAC address a Wake-on-LAN packet can target.
   * @details Six colon separated hex octets that are neither all zeros (loopback, or the
   *          placeholder) nor all ones (broadcast). The empty address a WireGuard or Tailscale
   *          interface reports is not one.
   */
  bool is_wake_on_lan_mac(std::string_view mac);

  /**
   * @brief The interfaces holding an IPv4 default route, parsed from /proc/net/route text.
   * @param proc_net_route The file's content: a header line, then one route per line.
   * @return Each interface once, lowest metric first, leaving out reject routes. The kernel marks
   *         every route in this file up, one on a card with its cable out too, so link state has to
   *         come from the interface table.
   */
  std::vector<std::string> default_route_interfaces(std::string_view proc_net_route);

  /**
   * @brief The MAC address serverinfo gives a paired client for waking this host.
   * @param interfaces The host's interfaces.
   * @param request_address The host address the client reached, as addr_to_normalized_string writes it.
   * @param default_route_interfaces The interfaces holding a default route, preferred first.
   * @return The MAC of the interface holding request_address when it has a usable one. A request
   *         over WireGuard, Tailscale or loopback arrives on an interface without one, and then the
   *         first default route interface with a link and a usable MAC answers: the LAN card a
   *         magic packet has to reach. no_wake_on_lan_mac when neither has one, never an empty
   *         string.
   * @details A client replaces its stored MAC with any value but the placeholder. On a host with
   *          two cards, where the client's own network reaches the one without the default route,
   *          a tunnel request therefore swaps the right MAC for the default route card's until the
   *          client next reaches the host on its own network. The host cannot tell which network
   *          the client lives on, so this is accepted, and docs/configuration.md says so. A tunnel
   *          that carries Ethernet, such as ZeroTier, has a MAC of its own and still reports it.
   */
  std::string wake_on_lan_mac(const std::vector<host_interface_t> &interfaces, std::string_view request_address, const std::vector<std::string> &default_route_interfaces);
}  // namespace net
