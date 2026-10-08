#include <gtest/gtest.h>
#include "src/tailscale_connection.h"
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#ifdef __linux__
#include <atomic>
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {
  constexpr std::int64_t now_ms = 1791446400000;  // 2026-10-08 08:00:00Z
  nlohmann::json status() {
    return {{"BackendState", "Running"}, {"TailscaleIPs", {"100.64.0.1"}},
      {"Peer", {{"secret-node-key", {{"TailscaleIPs", {"100.64.0.2"}}, {"Online", true},
        {"Active", true}, {"LastWrite", "2026-10-08T07:59:59.500Z"},
        {"CurAddr", "192.0.2.4:41641"}, {"Relay", "example-region"}, {"PeerRelay", ""},
        {"HostName", "private-name"}, {"DNSName", "private-name.example.ts.net."}}}}}};
  }
  nlohmann::json &peer(nlohmann::json &s) { return s["Peer"]["secret-node-key"]; }
  nlohmann::json evaluate(const nlohmann::json &s, std::string_view remote = "100.64.0.2",
                           std::string_view local = "100.64.0.1", std::string_view scope = "control_connection") {
    return tailscale_connection::from_status(s.dump(), remote, local, now_ms, scope);
  }
}

TEST(TailscaleConnection, ConfirmsOnlyTheActualTailnetControlEndpoints) {
  const auto result = evaluate(status());
  EXPECT_EQ(result["status"], "tailscale");
  EXPECT_EQ(result["tailscale"]["transport"], "direct");
  EXPECT_EQ(result["tailscale"]["scope"], "control_connection");
  EXPECT_EQ(result["tailscale"]["observed_at_ms"], now_ms);
  EXPECT_FALSE(evaluate(status(), "100.64.0.2", "192.168.0.1")["tailscale"]["confirmed"]);
  EXPECT_FALSE(evaluate(status(), "100.64.0.3")["tailscale"]["confirmed"]);
}

TEST(TailscaleConnection, RangesNamesAndRoutesDoNotSubstituteForMatchingPeers) {
  auto s = status();
  s["Peer"] = nlohmann::json::object();
  for (const auto remote : {"100.64.0.2", "192.168.0.2", "host.example.ts.net", "fd7a:115c:a1e0::2"})
    EXPECT_FALSE(evaluate(s, remote)["tailscale"]["confirmed"]);
}

TEST(TailscaleConnection, NormalizesMappedIpv4AndIpv6Addresses) {
  EXPECT_TRUE(evaluate(status(), "::ffff:100.64.0.2", "::ffff:100.64.0.1")["tailscale"]["confirmed"]);
  auto s = status();
  s["TailscaleIPs"] = {"fd7a:115c:a1e0::1"};
  peer(s)["TailscaleIPs"] = {"fd7a:115c:a1e0::2"};
  EXPECT_TRUE(evaluate(s, "fd7a:115c:a1e0:0:0:0:0:2", "fd7a:115c:a1e0::1")["tailscale"]["confirmed"]);
}

TEST(TailscaleConnection, DistinguishesDerpPeerRelayAndDirectWithoutReturningEndpoints) {
  auto s = status();
  peer(s)["CurAddr"] = "";
  EXPECT_EQ(evaluate(s)["tailscale"]["transport"], "derp");
  peer(s)["PeerRelay"] = "192.0.2.10:1234:vni:5";
  EXPECT_EQ(evaluate(s)["tailscale"]["transport"], "peer_relay");
  peer(s)["PeerRelay"] = "";
  peer(s)["CurAddr"] = "192.0.2.4:41641";
  EXPECT_EQ(evaluate(s)["tailscale"]["transport"], "direct");
  peer(s)["PeerRelay"] = "192.0.2.10:1234:vni:5";
  EXPECT_EQ(evaluate(s)["tailscale"]["transport"], "unknown");
}

TEST(TailscaleConnection, IdleOldAndFutureTrafficDoNotCertifyTransport) {
  auto s = status();
  peer(s)["Active"] = false;
  EXPECT_EQ(evaluate(s)["tailscale"]["transport"], "unknown");
  peer(s)["Active"] = true;
  for (const auto timestamp : {"2026-10-08T07:59:49Z", "2026-10-08T08:00:01Z", "0001-01-01T00:00:00Z", ""}) {
    peer(s)["LastWrite"] = timestamp;
    EXPECT_EQ(evaluate(s)["tailscale"]["transport"], "unknown");
  }
}

TEST(TailscaleConnection, ValidatesFreshnessBoundaryNanosecondsAndTimezones) {
  auto s = status();
  for (const auto timestamp : {"2026-10-08T07:59:50Z", "2026-10-08T03:59:59.123456789-04:00", "2026-10-08T09:59:59+02:00"}) {
    peer(s)["LastWrite"] = timestamp;
    EXPECT_EQ(evaluate(s)["tailscale"]["transport"], "direct") << timestamp;
  }
  for (const auto timestamp : {"2026-10-08T07:59:49.999Z", "2026-02-30T07:59:59Z", "2026-10-08T07:99:59Z", "2026-10-08T07:59:59.+00:00", "2026-10-08T07:59:59.1234567890Z"}) {
    peer(s)["LastWrite"] = timestamp;
    EXPECT_EQ(evaluate(s)["tailscale"]["transport"], "unknown") << timestamp;
  }
}

TEST(TailscaleConnection, OfflineStoppedDuplicateAndMalformedPeersRemainUnknown) {
  auto s = status();
  peer(s)["Online"] = false;
  EXPECT_FALSE(evaluate(s)["tailscale"]["confirmed"]);
  s = status(); s["BackendState"] = "Stopped";
  EXPECT_FALSE(evaluate(s)["tailscale"]["confirmed"]);
  s = status(); s["Peer"]["second-peer"] = peer(s);
  EXPECT_FALSE(evaluate(s)["tailscale"]["confirmed"]);
  s = status(); peer(s)["Online"] = "true";
  EXPECT_FALSE(evaluate(s)["tailscale"]["confirmed"]);
  EXPECT_FALSE(tailscale_connection::from_status("{", "100.64.0.2", "100.64.0.1", now_ms)["tailscale"]["confirmed"]);
  EXPECT_FALSE(tailscale_connection::from_status(std::string(tailscale_connection::max_status_bytes + 1, ' '), "100.64.0.2", "100.64.0.1", now_ms)["tailscale"]["confirmed"]);
}

TEST(TailscaleConnection, PeerObservationCannotBecomeControlOrMediaRouteProof) {
  auto result = evaluate(status(), "100.64.0.2", {}, "peer_status");
  EXPECT_TRUE(result["tailscale"]["confirmed"]);
  EXPECT_EQ(result["tailscale"]["scope"], "peer_status");
  EXPECT_FALSE(evaluate(status(), "100.64.0.2", {}, "control_connection")["tailscale"]["confirmed"]);
  EXPECT_FALSE(evaluate(status(), "100.64.0.2", {}, "media_path")["tailscale"]["confirmed"]);
}

TEST(TailscaleConnection, ExportsOnlyTheWhitelistedAddressFreeVocabulary) {
  const auto serialized = evaluate(status()).dump();
  for (const auto secret : {"100.64.0.1", "100.64.0.2", "192.0.2.4", "secret-node-key", "private-name", "example-region", "DNSName", "Peer", "TxBytes"})
    EXPECT_EQ(serialized.find(secret), std::string::npos) << secret;
  EXPECT_EQ(evaluate(status()).size(), 4);
  EXPECT_EQ(evaluate(status())["tailscale"].size(), 7);
  EXPECT_FALSE(evaluate(status())["tailscale"].contains("bitrate"));
}

#ifdef __linux__
namespace {
  class LocalStatusServer {
  public:
    std::string directory, path, received;
    int listener = -1;
    std::thread worker;
    std::atomic<bool> stop {false};
    LocalStatusServer(const std::string &body, int code = 200, bool delay = false) {
      char name[] = "/tmp/polaris-tail-XXXXXX";
      const auto created = mkdtemp(name);
      if (!created) throw std::runtime_error("fixture directory");
      directory = created; path = directory + "/socket";
      listener = socket(AF_UNIX, SOCK_STREAM, 0);
      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      std::copy(path.begin(), path.end(), addr.sun_path);
      if (listener < 0 || bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || listen(listener, 1)) {
        if (listener >= 0) close(listener);
        std::filesystem::remove_all(directory);
        throw std::runtime_error("fixture socket");
      }
      worker = std::thread([this, body, code, delay] {
        pollfd waiting {listener, POLLIN, 0};
        while (!stop && poll(&waiting, 1, 100) <= 0) {}
        if (stop) return;
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0) return;
        pollfd input {client, POLLIN, 0};
        if (poll(&input, 1, 1000) > 0) {
          char request[2048]; const auto count = recv(client, request, sizeof(request), 0);
          if (count > 0) received.assign(request, count);
        }
        if (delay) {
          while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else {
          const auto response = "HTTP/1.1 " + std::to_string(code) + " fixture\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
          std::size_t sent = 0;
          while (sent < response.size()) {
            const auto count = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
            if (count <= 0) break;
            sent += count;
          }
        }
        close(client);
      });
    }
    ~LocalStatusServer() {
      stop = true;
      if (worker.joinable()) worker.join();
      if (listener >= 0) close(listener);
      std::filesystem::remove_all(directory);
    }
  };
}

TEST(TailscaleLocalApi, ReadsOnlyTheFixedStatusRouteThroughTheSelectedUnixSocket) {
  LocalStatusServer server(status().dump());
  const auto body = tailscale_connection::read_status_socket(server.path);
  ASSERT_TRUE(body);
  EXPECT_EQ(*body, status().dump());
  server.stop = true; server.worker.join();
  EXPECT_EQ(server.received.find("GET /localapi/v0/status?peers=true HTTP/1.1\r\n"), 0);
  EXPECT_EQ(server.received.find("POST "), std::string::npos);
}

TEST(TailscaleLocalApi, MissingDeniedRedirectedAndOversizedRepliesFailClosed) {
  EXPECT_FALSE(tailscale_connection::read_status_socket("/tmp/polaris-no-such-status-socket"));
  for (const int code : {403, 302, 500}) {
    LocalStatusServer server("private-error", code);
    EXPECT_FALSE(tailscale_connection::read_status_socket(server.path));
  }
  LocalStatusServer server(std::string(tailscale_connection::max_status_bytes + 1, 'x'));
  EXPECT_FALSE(tailscale_connection::read_status_socket(server.path));
}

TEST(TailscaleLocalApi, UnresponsiveDaemonHasABoundedDeadline) {
  LocalStatusServer server("", 200, true);
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(tailscale_connection::read_status_socket(server.path));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(4));
}
#endif
