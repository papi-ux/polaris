#include <gtest/gtest.h>
#include "src/tailscale_connection.h"
#include <chrono>
#include <thread>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <vector>
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
  EXPECT_EQ(evaluate(s)["tailscale"]["reason"], "peer_not_confirmed");
  s = status(); s["BackendState"] = "Stopped";
  EXPECT_FALSE(evaluate(s)["tailscale"]["confirmed"]);
  s = status(); s["Peer"]["second-peer"] = peer(s);
  EXPECT_FALSE(evaluate(s)["tailscale"]["confirmed"]);
  s = status(); peer(s)["Online"] = "true";
  EXPECT_FALSE(evaluate(s)["tailscale"]["confirmed"]);
  EXPECT_FALSE(tailscale_connection::from_status("{", "100.64.0.2", "100.64.0.1", now_ms)["tailscale"]["confirmed"]);
  EXPECT_FALSE(tailscale_connection::from_status(std::string(tailscale_connection::max_status_bytes + 1, ' '), "100.64.0.2", "100.64.0.1", now_ms)["tailscale"]["confirmed"]);
}

// A daemon that is not running is unavailable, and a status without a usable peer map is
// malformed: neither is a peer that failed to match.
TEST(TailscaleConnection, DaemonStatesCarryTheirOwnReasons) {
  for (const auto *state : {"Stopped", "NeedsLogin", "Starting", "NoState"}) {
    auto s = status(); s["BackendState"] = state;
    const auto result = evaluate(s);
    EXPECT_EQ(result["status"], "unknown") << state;
    EXPECT_EQ(result["tailscale"]["reason"], "localapi_unavailable") << state;
  }
  auto s = status(); s.erase("BackendState");
  EXPECT_EQ(evaluate(s)["tailscale"]["reason"], "localapi_unavailable");
  s = status(); s.erase("Peer");
  EXPECT_EQ(evaluate(s)["tailscale"]["reason"], "malformed_status");
  s = status(); s["Peer"] = nlohmann::json::array();
  EXPECT_EQ(evaluate(s)["tailscale"]["reason"], "malformed_status");
  EXPECT_EQ(tailscale_connection::from_status("[]", "100.64.0.2", "100.64.0.1", now_ms)["tailscale"]["reason"], "malformed_status");
  EXPECT_EQ(tailscale_connection::from_status("{", "100.64.0.2", "100.64.0.1", now_ms)["tailscale"]["reason"], "malformed_status");
  // No read at all (missing socket, refusal, deadline, overload) is the daemon being unavailable.
#ifdef __linux__
  EXPECT_EQ(tailscale_connection::evaluate(std::nullopt, now_ms, "100.64.0.2", "100.64.0.1")["tailscale"]["reason"], "localapi_unavailable");
  EXPECT_EQ(tailscale_connection::evaluate(status().dump(), now_ms, "100.64.0.2", "100.64.0.1")["status"], "tailscale");
#else
  EXPECT_EQ(tailscale_connection::evaluate(status().dump(), now_ms, "100.64.0.2", "100.64.0.1")["tailscale"]["reason"], "unsupported_platform");
#endif
}

namespace {
  /// A daemon read that blocks until released, counting reads and reporting when one starts.
  struct gated_reader_t {
    std::mutex mutex;
    std::condition_variable changed;
    bool open = false;
    int entered = 0;
    std::string body = status().dump();

    std::optional<std::string> read() {
      std::unique_lock lock {mutex};
      ++entered;
      changed.notify_all();
      // Bounded, so a failed assertion cannot leave a collector joining forever.
      if (!changed.wait_for(lock, std::chrono::seconds(10), [this] { return open; })) return std::nullopt;
      return body;
    }

    bool wait_entered(int count) {
      std::unique_lock lock {mutex};
      return changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered >= count; });
    }

    void release() {
      std::lock_guard lock {mutex};
      open = true;
      changed.notify_all();
    }
  };

  struct answers_t {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<nlohmann::json> received;

    tailscale_connection::status_collector_t::waiter_t waiter(std::string remote = "100.64.0.2", std::string local = "100.64.0.1",
                                                              std::string scope = "control_connection") {
      return [this, remote, local, scope](const std::optional<std::string> &status, std::int64_t observed_at_ms) {
        auto evidence = tailscale_connection::evaluate(status, observed_at_ms, remote, local, scope);
        std::lock_guard lock {mutex};
        received.push_back(std::move(evidence));
        changed.notify_all();
      };
    }

    bool wait_for(std::size_t count) {
      std::unique_lock lock {mutex};
      return changed.wait_for(lock, std::chrono::seconds(5), [&] { return received.size() >= count; });
    }

    std::size_t size() {
      std::lock_guard lock {mutex};
      return received.size();
    }
  };
}  // namespace

#ifdef __linux__
// The cross-route try_lock answered "unavailable" to whichever caller lost a race. Overlapping
// callers now wait for a real read: every one of them gets the daemon's answer.
TEST(TailscaleCollector, OverlappingCallersAllGetTheDaemonsAnswerNeverAFalseUnavailable) {
  gated_reader_t daemon;
  answers_t answers;
  tailscale_connection::status_collector_t collector {[&] { return daemon.read(); }, [] { return now_ms; }};
  ASSERT_TRUE(collector.submit(answers.waiter()));
  ASSERT_TRUE(daemon.wait_entered(1));
  // Two routes (paired and admin) and several clients ask while the first read is still running.
  std::vector<std::thread> callers;
  for (int i = 0; i < 8; ++i) callers.emplace_back([&] { EXPECT_TRUE(collector.submit(answers.waiter())); });
  for (auto &caller : callers) caller.join();
  EXPECT_EQ(answers.size(), 0u);
  daemon.release();
  ASSERT_TRUE(answers.wait_for(9));
  for (const auto &answer : answers.received) {
    EXPECT_EQ(answer["status"], "tailscale");
    EXPECT_EQ(answer["tailscale"]["reason"], "observed");
  }
  // One read for the first caller, and one shared by everyone who arrived during it.
  EXPECT_EQ(collector.reads(), 2u);
}

// Two servers' collectors read independently; neither turns the other's read into "unavailable".
TEST(TailscaleCollector, IndependentRouteCollectorsDoNotContend) {
  gated_reader_t daemon;
  answers_t paired, admin;
  tailscale_connection::status_collector_t paired_collector {[&] { return daemon.read(); }, [] { return now_ms; }};
  tailscale_connection::status_collector_t admin_collector {[&] { return daemon.read(); }, [] { return now_ms; }};
  ASSERT_TRUE(paired_collector.submit(paired.waiter()));
  ASSERT_TRUE(admin_collector.submit(admin.waiter("100.64.0.2", {}, "peer_status")));
  ASSERT_TRUE(daemon.wait_entered(2));
  daemon.release();
  ASSERT_TRUE(paired.wait_for(1));
  ASSERT_TRUE(admin.wait_for(1));
  EXPECT_EQ(paired.received.front()["status"], "tailscale");
  EXPECT_EQ(admin.received.front()["status"], "tailscale");
  EXPECT_EQ(admin.received.front()["tailscale"]["scope"], "peer_status");
}

// Bounded: the request being read and those waiting together never exceed max_waiters. Past
// that the caller is refused at once and answers unknown itself.
TEST(TailscaleCollector, OverloadIsRefusedImmediatelyAndBounded) {
  constexpr auto cap = tailscale_connection::status_collector_t::max_waiters;
  gated_reader_t daemon;
  answers_t answers;
  tailscale_connection::status_collector_t collector {[&] { return daemon.read(); }, [] { return now_ms; }};
  ASSERT_TRUE(collector.submit(answers.waiter()));
  ASSERT_TRUE(daemon.wait_entered(1));
  // One in the read under way, so cap - 1 more may wait.
  for (std::size_t i = 1; i < cap; ++i)
    ASSERT_TRUE(collector.submit(answers.waiter())) << i;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(collector.submit(answers.waiter()));
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
  EXPECT_FALSE(collector.submit({}));
  daemon.release();
  ASSERT_TRUE(answers.wait_for(cap));
  EXPECT_EQ(collector.reads(), 2u);
  // Every accepted request was answered exactly once, and the cap frees again afterwards.
  EXPECT_EQ(answers.size(), cap);
  ASSERT_TRUE(collector.submit(answers.waiter()));
  ASSERT_TRUE(answers.wait_for(cap + 1));
}

// Stopping answers every waiting request (unknown) and the in-flight batch (its read), joins the
// worker, and refuses anything after.
TEST(TailscaleCollector, StopAnswersEveryoneAndJoins) {
  gated_reader_t daemon;
  answers_t answers;
  tailscale_connection::status_collector_t collector {[&] { return daemon.read(); }, [] { return now_ms; }};
  ASSERT_TRUE(collector.submit(answers.waiter()));
  ASSERT_TRUE(daemon.wait_entered(1));
  ASSERT_TRUE(collector.submit(answers.waiter()));
  // stop() takes the waiting request at once, then joins the worker, which is still reading;
  // the daemon answers a little later.
  std::thread releaser {[&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); daemon.release(); }};
  collector.stop();
  releaser.join();
  ASSERT_EQ(answers.size(), 2u);
  int confirmed = 0, unavailable = 0;
  for (const auto &answer : answers.received) {
    confirmed += answer["status"] == "tailscale";
    unavailable += answer["tailscale"]["reason"] == "localapi_unavailable";
  }
  EXPECT_EQ(confirmed, 1);
  EXPECT_EQ(unavailable, 1);
  EXPECT_FALSE(collector.submit(answers.waiter()));
  EXPECT_EQ(collector.reads(), 1u);
  collector.stop();  // idempotent
}

// Nothing is kept between reads: each new request after an answer causes a new daemon read.
TEST(TailscaleCollector, EveryAnswerComesFromAReadAfterItsRequest) {
  int reads = 0;
  answers_t answers;
  tailscale_connection::status_collector_t collector {[&] { ++reads; return std::optional<std::string> {status().dump()}; }, [] { return now_ms; }};
  for (std::size_t i = 1; i <= 3; ++i) {
    ASSERT_TRUE(collector.submit(answers.waiter()));
    ASSERT_TRUE(answers.wait_for(i));
  }
  EXPECT_EQ(collector.reads(), 3u);
  EXPECT_EQ(reads, 3);
}
#endif

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
