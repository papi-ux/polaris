/**
 * @file src/wol.cpp
 * @brief Definitions for Wake-on-LAN support.
 */

// standard includes
#include <cstdio>
#include <cstring>

// platform includes
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <arpa/inet.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

// local includes
#include "logging.h"
#include "wol.h"

using namespace std::literals;

namespace wol {

  bool send_magic_packet(const std::string &mac_address) {
    // Parse MAC address string to 6 bytes
    unsigned char mac[6];
    if (std::sscanf(mac_address.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                    &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
      BOOST_LOG(warning) << "wol: Invalid MAC address format: "sv << mac_address;
      return false;
    }

    // Build magic packet: 6 bytes of 0xFF followed by the MAC address repeated 16 times
    unsigned char packet[102];
    std::memset(packet, 0xFF, 6);
    for (int i = 0; i < 16; i++) {
      std::memcpy(packet + 6 + i * 6, mac, 6);
    }

#ifdef _WIN32
    // Initialize Winsock if needed
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
      BOOST_LOG(warning) << "wol: WSAStartup failed"sv;
      return false;
    }
#endif

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
      BOOST_LOG(warning) << "wol: Failed to create socket"sv;
#ifdef _WIN32
      WSACleanup();
#endif
      return false;
    }

    // Enable broadcast
    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&broadcast), sizeof(broadcast));

    // Send to broadcast address on port 9
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    addr.sin_addr.s_addr = INADDR_BROADCAST;

    auto sent = sendto(sock, reinterpret_cast<const char *>(packet), sizeof(packet), 0,
                       reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));

#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif

    if (sent <= 0) {
      BOOST_LOG(warning) << "wol: Failed to send magic packet to "sv << mac_address;
      return false;
    }

    BOOST_LOG(info) << "wol: Sent magic packet to "sv << mac_address;
    return true;
  }

}  // namespace wol
