/**
 * @file src/network.cpp
 * @brief Definitions for networking related functions.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>

// local includes
#include "config.h"
#include "logging.h"
#include "network.h"
#include "utility.h"

using namespace std::literals;

namespace ip = boost::asio::ip;

namespace net {
  std::vector<ip::network_v4> pc_ips_v4 {
    ip::make_network_v4("127.0.0.0/8"sv),
  };
  std::vector<ip::network_v4> lan_ips_v4 {
    ip::make_network_v4("192.168.0.0/16"sv),
    ip::make_network_v4("172.16.0.0/12"sv),
    ip::make_network_v4("10.0.0.0/8"sv),
    ip::make_network_v4("169.254.0.0/16"sv),
  };

  std::vector<ip::network_v6> pc_ips_v6 {
    ip::make_network_v6("::1/128"sv),
  };
  std::vector<ip::network_v6> lan_ips_v6 {
    ip::make_network_v6("fc00::/7"sv),
    ip::make_network_v6("fe80::/64"sv),
  };

  net_e from_enum_string(const std::string_view &view) {
    if (view == "wan") {
      return WAN;
    }
    if (view == "lan") {
      return LAN;
    }

    return PC;
  }

  net_e from_address(const std::string_view &view) {
    auto addr = normalize_address(ip::make_address(view));

    if (addr.is_v6()) {
      for (auto &range : pc_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v6) {
        if (range.hosts().find(addr.to_v6()) != range.hosts().end()) {
          return LAN;
        }
      }
    } else {
      for (auto &range : pc_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return PC;
        }
      }

      for (auto &range : lan_ips_v4) {
        if (range.hosts().find(addr.to_v4()) != range.hosts().end()) {
          return LAN;
        }
      }
    }

    return WAN;
  }

  std::string_view describe_client_network_path(const std::string_view &address) {
    // Diagnostic names never grant access. Shared IPv4 space identifies neither a
    // private LAN nor a trusted VPN: from_address applies WAN policy to it.
    static const auto cgnat = ip::make_network_v4("100.64.0.0/10"sv);
    static const auto link_local_v4 = ip::make_network_v4("169.254.0.0/16"sv);
    static const std::vector<ip::network_v4> private_v4 {
      ip::make_network_v4("10.0.0.0/8"sv),
      ip::make_network_v4("172.16.0.0/12"sv),
      ip::make_network_v4("192.168.0.0/16"sv),
    };
    // Tailscale's IPv6 range is specific to it, unlike the shared IPv4 range.
    static const auto tailscale_v6 = ip::make_network_v6("fd7a:115c:a1e0::/48"sv);
    static const auto unique_local_v6 = ip::make_network_v6("fc00::/7"sv);

    boost::system::error_code ec;
    const auto parsed = ip::make_address(std::string {address}, ec);
    if (ec) {
      return "unknown";
    }
    const auto addr = normalize_address(parsed);
    if (addr.is_loopback()) {
      return "loopback";
    }

    if (addr.is_v4()) {
      const auto v4 = addr.to_v4();
      const auto within = [&v4](const ip::network_v4 &range) {
        return range.hosts().find(v4) != range.hosts().end();
      };
      if ((v4.to_uint() & cgnat.netmask().to_uint()) == cgnat.network().to_uint()) {
        return "cgnat";
      }
      if (within(link_local_v4)) {
        return "link-local";
      }
      for (const auto &range : private_v4) {
        if (within(range)) {
          return "lan";
        }
      }
      return "public";
    }

    const auto v6 = addr.to_v6();
    if (tailscale_v6.hosts().find(v6) != tailscale_v6.hosts().end()) {
      return "tailscale";
    }
    if (v6.is_link_local()) {
      return "link-local";
    }
    if (unique_local_v6.hosts().find(v6) != unique_local_v6.hosts().end()) {
      return "lan";
    }
    return "public";
  }

  std::string_view to_enum_string(net_e net) {
    switch (net) {
      case PC:
        return "pc"sv;
      case LAN:
        return "lan"sv;
      case WAN:
        return "wan"sv;
    }

    // avoid warning
    return "wan"sv;
  }

  af_e af_from_enum_string(const std::string_view &view) {
    if (view == "ipv4") {
      return IPV4;
    }
    if (view == "both") {
      return BOTH;
    }

    // avoid warning
    return BOTH;
  }

  std::string_view af_to_any_address_string(af_e af) {
    switch (af) {
      case IPV4:
        return "0.0.0.0"sv;
      case BOTH:
        return "::"sv;
    }

    // avoid warning
    return "::"sv;
  }

  boost::asio::ip::address normalize_address(boost::asio::ip::address address) {
    // Convert IPv6-mapped IPv4 addresses into regular IPv4 addresses
    if (address.is_v6()) {
      auto v6 = address.to_v6();
      if (v6.is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6);
      }
    }

    return address;
  }

  std::string addr_to_normalized_string(boost::asio::ip::address address) {
    return normalize_address(address).to_string();
  }

  std::string addr_to_url_escaped_string(boost::asio::ip::address address) {
    address = normalize_address(address);
    if (address.is_v6()) {
      std::stringstream ss;
      ss << '[' << address.to_string() << ']';
      return ss.str();
    } else {
      return address.to_string();
    }
  }

  int encryption_mode_for_address(boost::asio::ip::address address) {
    auto nettype = net::from_address(address.to_string());
    if (nettype == net::net_e::PC || nettype == net::net_e::LAN) {
      return config::stream.lan_encryption_mode;
    } else {
      return config::stream.wan_encryption_mode;
    }
  }

  host_t host_create(af_e af, ENetAddress &addr, std::uint16_t port) {
    static std::once_flag enet_init_flag;
    std::call_once(enet_init_flag, []() {
      enet_initialize();
    });

    auto any_addr = net::af_to_any_address_string(af);
    enet_address_set_host(&addr, any_addr.data());
    enet_address_set_port(&addr, port);

    // Maximum of 128 clients, which should be enough for anyone
    auto host = host_t {enet_host_create(af == IPV4 ? AF_INET : AF_INET6, &addr, 128, 0, 0, 0)};

    // Enable opportunistic QoS tagging (automatically disables if the network appears to drop tagged packets)
    enet_socket_set_option(host->socket, ENET_SOCKOPT_QOS, 1);

    return host;
  }

  void free_host(ENetHost *host) {
    std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
      ENetPeer *peer = &peer_ref;

      if (peer) {
        enet_peer_disconnect_now(peer, 0);
      }
    });

    enet_host_destroy(host);
  }

  std::uint16_t map_port(int port) {
    // calculate the port from the config port
    auto mapped_port = (std::uint16_t) ((int) config::sunshine.port + port);

    // Ensure port is in the range of 1024-65535
    if (mapped_port < 1024 || mapped_port > 65535) {
      BOOST_LOG(warning) << "Port out of range: "sv << mapped_port;
    }

    return mapped_port;
  }

  /**
   * @brief Returns a string for use as the instance name for mDNS.
   * @param hostname The hostname to use for instance name generation.
   * @return Hostname-based instance name or "Sunshine" if hostname is invalid.
   */
  std::string mdns_instance_name(const std::string_view &hostname) {
    // Start with the unmodified hostname
    std::string instancename {hostname.data(), hostname.size()};

    // Truncate to 63 characters per RFC 6763 section 7.2.
    if (instancename.size() > 63) {
      instancename.resize(63);
    }

    for (auto i = 0; i < instancename.size(); i++) {
      // Replace any spaces with dashes
      if (instancename[i] == ' ') {
        instancename[i] = '-';
      } else if (!std::isalnum(instancename[i]) && instancename[i] != '-') {
        // Stop at the first invalid character
        instancename.resize(i);
        break;
      }
    }

    return !instancename.empty() ? instancename : "Apollo";
  }

  std::string network_path_probe_classification(const std::string_view &host) {
    std::string value {host.data(), host.size()};
    const auto bracketed_v6_end = value.find(']');
    if (!value.empty() && value.front() == '[' && bracketed_v6_end != std::string::npos) {
      value = value.substr(1, bracketed_v6_end - 1);
    } else if (const auto scheme = value.find("://"); scheme != std::string::npos) {
      value = value.substr(scheme + 3);
    }
    if (const auto slash = value.find('/'); slash != std::string::npos) {
      value = value.substr(0, slash);
    }
    if (const auto colon = value.find(':'); colon != std::string::npos && value.find(':', colon + 1) == std::string::npos) {
      value = value.substr(0, colon);
    }

    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    if (value.ends_with(".local") || value == "localhost") {
      return "lan";
    }
    if (value.ends_with(".ts.net") || value.find("tailscale") != std::string::npos || value.find("zerotier") != std::string::npos) {
      return "vpn";
    }

    try {
      return std::string {to_enum_string(from_address(value))};
    } catch (const std::exception &) {
      return "wan";
    }
  }

  std::vector<network_path_probe_port_t> network_path_probe_ports(std::uint16_t base_port) {
    return {
      {"control_https", "Web/control HTTPS", static_cast<std::uint16_t>(base_port + 1), "tcp"},
      {"rtsp_setup", "RTSP setup", static_cast<std::uint16_t>(base_port + 21), "tcp"},
      {"control_udp", "Input/control stream", static_cast<std::uint16_t>(base_port + 10), "udp"},
      {"video_udp", "Video stream", static_cast<std::uint16_t>(base_port + 9), "udp"},
      {"audio_udp", "Audio stream", static_cast<std::uint16_t>(base_port + 11), "udp"},
    };
  }

  std::vector<unsigned long> listening_socket_inodes(std::string_view proc_net_tcp, std::uint16_t port) {
    // Each line: "sl local_address rem_address st tx_queue:rx_queue tr:tm->when retrnsmt uid timeout inode ..."
    // local_address is hex "addr:port"; state 0A is LISTEN. The first line is the header.
    std::vector<unsigned long> inodes;
    std::size_t start = 0;
    bool header = true;
    while (start < proc_net_tcp.size()) {
      auto end = proc_net_tcp.find('\n', start);
      if (end == std::string_view::npos) {
        end = proc_net_tcp.size();
      }
      const auto line = proc_net_tcp.substr(start, end - start);
      start = end + 1;
      if (header) {
        header = false;
        continue;
      }
      std::vector<std::string_view> fields;
      std::size_t pos = 0;
      while (pos < line.size()) {
        while (pos < line.size() && line[pos] == ' ') {
          ++pos;
        }
        const auto field_end = line.find(' ', pos);
        if (pos >= line.size()) {
          break;
        }
        fields.push_back(line.substr(pos, field_end == std::string_view::npos ? std::string_view::npos : field_end - pos));
        if (field_end == std::string_view::npos) {
          break;
        }
        pos = field_end;
      }
      if (fields.size() < 10) {
        continue;
      }
      const auto colon = fields[1].rfind(':');
      if (colon == std::string_view::npos || fields[3] != "0A") {
        continue;
      }
      const auto port_hex = fields[1].substr(colon + 1);
      unsigned int local_port = 0;
      if (std::from_chars(port_hex.data(), port_hex.data() + port_hex.size(), local_port, 16).ec != std::errc {} ||
          local_port != port) {
        continue;
      }
      unsigned long inode = 0;
      if (std::from_chars(fields[9].data(), fields[9].data() + fields[9].size(), inode).ec == std::errc {} && inode != 0) {
        inodes.push_back(inode);
      }
    }
    return inodes;
  }

  std::string describe_port_holder(std::uint16_t port) {
#ifdef __linux__
    std::vector<unsigned long> inodes;
    for (const auto *table : {"/proc/net/tcp", "/proc/net/tcp6"}) {
      std::ifstream in(table);
      if (!in) {
        continue;
      }
      std::stringstream buffer;
      buffer << in.rdbuf();
      const auto found = listening_socket_inodes(buffer.str(), port);
      inodes.insert(inodes.end(), found.begin(), found.end());
    }
    if (inodes.empty()) {
      return {};
    }

    // Only this account's processes expose their descriptors; another user's
    // Polaris, Sunshine, Apollo or Hermes shows up as an inode nobody owns.
    std::error_code ec;
    for (const auto &process : std::filesystem::directory_iterator("/proc", ec)) {
      const auto pid = process.path().filename().string();
      if (pid.empty() || !std::all_of(pid.begin(), pid.end(), [](unsigned char c) { return std::isdigit(c); })) {
        continue;
      }
      std::error_code fd_ec;
      for (const auto &fd : std::filesystem::directory_iterator(process.path() / "fd", fd_ec)) {
        std::error_code link_ec;
        const auto target = std::filesystem::read_symlink(fd.path(), link_ec).string();
        if (link_ec || target.rfind("socket:[", 0) != 0) {
          continue;
        }
        unsigned long inode = 0;
        const auto digits = std::string_view {target}.substr(8, target.size() - 9);
        if (std::from_chars(digits.data(), digits.data() + digits.size(), inode).ec != std::errc {}) {
          continue;
        }
        if (std::find(inodes.begin(), inodes.end(), inode) == inodes.end()) {
          continue;
        }
        std::string comm = "unknown";
        if (std::ifstream comm_in(process.path() / "comm"); comm_in) {
          std::getline(comm_in, comm);
        }
        return "held by " + comm + " (pid " + pid + ")";
      }
    }
    return "held by a process this account cannot inspect; another user's Polaris, Sunshine, Apollo and Hermes all use this port";
#else
    (void) port;
    return {};
#endif
  }
}  // namespace net
