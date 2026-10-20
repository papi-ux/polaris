/** Real authenticated console requests must not queue behind app teardown. */
#include "../tests_common.h"

#include <Simple-Web-Server/client_https.hpp>
#include <src/config.h>
#include <src/confighttp.h>
#include <src/crypto.h>
#include <src/process.h>
#include <src/stream_stats.h>
#include <src/tailscale_connection.h>
#include <src/utility.h>

#include <atomic>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <fstream>
#include <future>

using namespace std::chrono_literals;

namespace {
  struct apps_stats_contention: testing::Test {
    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      directory = std::filesystem::temp_directory_path() / ("polaris-apps-http-" + std::to_string(nonce));
      std::filesystem::create_directory(directory);
      old_username = std::exchange(config::sunshine.username, "http-fixture-user");
      old_key = std::exchange(config::sunshine.api_key, "http-fixture-key");
      old_apps_file = std::exchange(config::stream.file_apps, (directory / "apps.json").string());
      std::ofstream(config::stream.file_apps) << R"({"apps":[{"uuid":"app-a","name":"Fixture A"},{"uuid":"app-b","name":"Fixture B"}]})";
      const auto credentials = crypto::gen_creds("localhost", 2048);
      const auto cert = directory / "cert.pem", key = directory / "key.pem";
      std::ofstream(cert) << credentials.x509;
      std::ofstream(key) << credentials.pkey;
      std::filesystem::permissions(key, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
      server = std::async(std::launch::async, [&, cert, key]() {
        try {
          confighttp::run_apps_stats_server_for_tests(cert.string(), key.string(),
            [&](unsigned short port, std::function<void()> stop) {
              stop_server = std::move(stop);
              listening.set_value(port);
            }, [&]() {
              if (!entered.exchange(true)) apps_entered.set_value();
            }, [&](std::function<bool(std::size_t, std::chrono::milliseconds)> wait) {
              // Set before the listener is ready, so it happens-before the test's first use.
              wait_network_queued = std::move(wait);
            });
        } catch (...) {
          if (!stop_server) listening.set_exception(std::current_exception());
          else throw;
        }
      });
      port = listening.get_future().get();
    }

    void TearDown() override {
      if (stop_server) stop_server();
      if (server.valid()) server.get();
      // The wait referred to the server's own collector, which is gone now.
      wait_network_queued = nullptr;
      tailscale_connection::set_default_status_reader_for_tests({});
      proc::proc.clear_running_app_for_tests();
      config::sunshine.username = old_username;
      config::sunshine.api_key = old_key;
      config::stream.file_apps = old_apps_file;
      std::filesystem::remove_all(directory);
    }

    auto request(const std::string &path, const std::string &token = "http-fixture-key") {
      return std::async(std::launch::async, [=, this]() {
        SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(port), false};
        client.config.timeout = 2;
        client.config.timeout_connect = 2;
        SimpleWeb::CaseInsensitiveMultimap headers {{"Authorization", "Bearer " + token}};
        auto response = client.request("GET", path, "", headers);
        return std::pair {response->status_code, response->content.string()};
      });
    }

    void set_app(std::string uuid) {
      proc::ctx_t app {};
      app.id = "167";
      app.uuid = std::move(uuid);
      proc::proc.set_running_app_for_tests(app);
    }

    std::filesystem::path directory;
    std::string old_username, old_key, old_apps_file;
    std::promise<unsigned short> listening;
    std::promise<void> apps_entered;
    std::atomic<bool> entered {false};
    std::function<void()> stop_server;
    /// True once that many accepted network readouts are queued for a read not yet started.
    std::function<bool(std::size_t, std::chrono::milliseconds)> wait_network_queued;
    std::future<void> server;
    unsigned short port = 0;
  };
}

TEST_F(apps_stats_contention, AppsAndStatsReturnWhileTeardownHoldsTheLifecycleLock) {
  set_app("app-a");
  stream_stats::add_client("127.0.0.167", "Ended HTTP fixture", 920167);
  stream_stats::remove_client("127.0.0.167", 920167);
  const auto history = nlohmann::json::parse(stream_stats::get_current().to_json()).at("last_session");
  std::promise<void> locked, release;
  auto released = release.get_future().share();
  auto teardown = std::async(std::launch::async, [&]() {
    proc::proc.with_session_lifecycle_lock_for_tests([&]() { locked.set_value(); released.wait(); });
  });
  auto unlock = util::fail_guard([&]() { release.set_value(); teardown.get(); });
  locked.get_future().wait();
  auto apps = request("/api/apps");
  ASSERT_EQ(apps_entered.get_future().wait_for(2s), std::future_status::ready);
  auto stats = request("/api/stats/stream");
  const auto apps_ready = apps.wait_for(500ms), stats_ready = stats.wait_for(500ms);
  // Unblock the original implementation before reporting its failure.
  release.set_value(); teardown.get(); unlock.disable();
  const auto [apps_status, apps_body] = apps.get();
  const auto [stats_status, stats_body] = stats.get();
  EXPECT_EQ(apps_ready, std::future_status::ready);
  EXPECT_EQ(stats_ready, std::future_status::ready);
  ASSERT_TRUE(apps_status.starts_with("200"));
  ASSERT_TRUE(stats_status.starts_with("200"));
  const auto apps_json = nlohmann::json::parse(apps_body), stats_json = nlohmann::json::parse(stats_body);
  ASSERT_TRUE(apps_json.contains("current_app_available"));
  EXPECT_FALSE(apps_json.at("current_app_available").get<bool>());
  EXPECT_EQ(apps_json.at("current_app"), "");
  EXPECT_EQ(apps_json.at("apps").size(), 2);
  EXPECT_EQ(stats_json.at("auto_quality").at("blocked_reason"), "session_changing");
  EXPECT_EQ(stats_json.at("last_session"), history);
  EXPECT_TRUE(stats_json.contains("doctor"));

  auto recovered = nlohmann::json::parse(request("/api/apps").get().second);
  EXPECT_TRUE(recovered.at("current_app_available").get<bool>());
  EXPECT_EQ(recovered.at("current_app"), "app-a");
  set_app("app-b");
  recovered = nlohmann::json::parse(request("/api/apps").get().second);
  EXPECT_EQ(recovered.at("current_app"), "app-b");
  proc::proc.clear_running_app_for_tests();
  recovered = nlohmann::json::parse(request("/api/apps").get().second);
  EXPECT_TRUE(recovered.at("current_app_available").get<bool>());
  EXPECT_EQ(recovered.at("current_app"), "");
}

TEST_F(apps_stats_contention, BothReadOnlyHandlersStillRequireAuthentication) {
  proc::proc.with_session_lifecycle_lock_for_tests([&]() {
    for (const auto *path : {"/api/apps", "/api/stats/stream"}) {
      auto reply = request(path, "wrong-fixture-key");
      ASSERT_EQ(reply.wait_for(500ms), std::future_status::ready);
      const auto [status, body] = reply.get();
      EXPECT_TRUE(status.starts_with("401"));
      EXPECT_EQ(body.find("Fixture A"), std::string::npos);
    }
  });
}

TEST_F(apps_stats_contention, NetworkEvidenceRequiresAuthenticationBeforeReadingTheDaemon) {
  proc::proc.with_session_lifecycle_lock_for_tests([&]() {
    auto reply = request("/api/network/connection", "wrong-fixture-key");
    ASSERT_EQ(reply.wait_for(500ms), std::future_status::ready);
    const auto [status, body] = reply.get();
    EXPECT_TRUE(status.starts_with("401"));
    EXPECT_EQ(body.find("tailscaled"), std::string::npos);
  });
}

TEST_F(apps_stats_contention, IdleNetworkEvidenceCannotBeSelectedWithQueryParameters) {
  ASSERT_FALSE(stream_stats::get_current().streaming);
  const auto [status, body] = request("/api/network/connection?peer=100.64.0.2&hostname=private.example.ts.net").get();
  ASSERT_TRUE(status.starts_with("200"));
  const auto evidence = nlohmann::json::parse(body);
  EXPECT_EQ(evidence.at("schema_version"), 1);
  EXPECT_EQ(evidence.at("status"), "unknown");
  EXPECT_EQ(evidence.at("tailscale").at("scope"), "peer_status");
  EXPECT_EQ(evidence.at("tailscale").at("confirmed"), false);
  EXPECT_EQ(evidence.at("tailscale").at("reason"), "no_active_stream");
  EXPECT_EQ(body.find("100.64.0.2"), std::string::npos);
  EXPECT_EQ(body.find("private.example.ts.net"), std::string::npos);
  const auto stats = nlohmann::json::parse(request("/api/stats/stream").get().second);
  EXPECT_EQ(stats.at("stream_instance_id"), "");
}

namespace {
  constexpr auto fence_client_ip = "127.0.0.168";
  constexpr auto fence_device = "tailscale-fence-device";

  /// A running daemon that knows the stream's client as one online, recently active peer.
  std::string admin_tailnet_status() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc {};
    gmtime_r(&now, &utc);
    char written[32] {};
    std::strftime(written, sizeof(written), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return nlohmann::json {
      {"BackendState", "Running"}, {"TailscaleIPs", {"100.64.0.1"}},
      {"Peer", {{"nodekey:admin-fixture", {{"TailscaleIPs", {fence_client_ip}}, {"Online", true}, {"Active", true},
        {"LastWrite", written}, {"CurAddr", ""}, {"Relay", "fixture-region"}, {"PeerRelay", ""}}}}},
    }.dump();
  }

  /// A daemon read that blocks until released, counting how many reads began.
  struct stalled_admin_daemon_t {
    std::mutex mutex;
    std::condition_variable changed;
    int entered = 0;
    bool open = false;

    std::optional<std::string> read() {
      std::unique_lock lock {mutex};
      ++entered;
      changed.notify_all();
      if (!changed.wait_for(lock, std::chrono::seconds(10), [this] { return open; })) return std::nullopt;
      return admin_tailnet_status();
    }

    bool wait_entered(int count = 1) {
      std::unique_lock lock {mutex};
      return changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered >= count; });
    }

    void release() {
      std::lock_guard lock {mutex};
      open = true;
      changed.notify_all();
    }
  };

  /// An active stream from fence_client_ip under `generation`, ended when the guard goes.
  auto start_fixture_stream(std::uint64_t generation) {
    stream_stats::add_client(fence_client_ip, "Tailscale fence fixture", generation);
    stream_stats::start_session_timing(fence_device, generation);
    return util::fail_guard([generation]() {
      stream_stats::stop_session_timing(fence_device, generation);
      stream_stats::remove_client(fence_client_ip, generation);
    });
  }
}  // namespace

// An active stream's peer evidence, read off the admin server's thread, for that stream only.
TEST_F(apps_stats_contention, ActiveStreamPeerEvidenceNamesItsStreamAndNoAddress) {
  auto stream = start_fixture_stream(920168);
  tailscale_connection::set_default_status_reader_for_tests([] { return std::optional<std::string> {admin_tailnet_status()}; });
  const auto [status, body] = request("/api/network/connection").get();
  ASSERT_TRUE(status.starts_with("200")) << status;
  const auto evidence = nlohmann::json::parse(body);
  EXPECT_EQ(evidence.at("status"), "tailscale");
  EXPECT_EQ(evidence.at("tailscale").at("scope"), "peer_status");
  EXPECT_EQ(evidence.at("tailscale").at("transport"), "derp");
  EXPECT_EQ(evidence.at("stream_instance_id"), stream_stats::stream_instance_id(920168));
  for (const auto *secret : {fence_client_ip, "nodekey", "fixture-region", "100.64.0.1"})
    EXPECT_EQ(body.find(secret), std::string::npos) << secret;
}

// The generation fence: the stream changes generation while the daemon is being read, so the
// answer is about a stream that is no longer current and is withheld.
TEST_F(apps_stats_contention, AGenerationChangeDuringTheReadIsFenced) {
  auto stream = start_fixture_stream(920168);
  stalled_admin_daemon_t daemon;
  tailscale_connection::set_default_status_reader_for_tests([&daemon] { return daemon.read(); });
  auto release = util::fail_guard([&] { daemon.release(); });

  auto reply = request("/api/network/connection");
  ASSERT_TRUE(daemon.wait_entered());
  // The same client reconnects: a new generation, the same address.
  stream_stats::start_session_timing(fence_device, 920169);
  auto newer = util::fail_guard([] { stream_stats::stop_session_timing(fence_device, 920169); });
  daemon.release();
  release.disable();

  const auto [status, body] = reply.get();
  ASSERT_TRUE(status.starts_with("200")) << status;
  const auto evidence = nlohmann::json::parse(body);
  EXPECT_EQ(evidence.at("status"), "unknown");
  EXPECT_EQ(evidence.at("tailscale").at("confirmed"), false);
  EXPECT_EQ(evidence.at("tailscale").at("reason"), "stream_changed");
  EXPECT_EQ(evidence.at("stream_instance_id"), stream_stats::stream_instance_id(920168));
}

// F1 and F2 on the admin server: overlapping readouts wait for the daemon instead of one of them
// reading "unavailable", and a stalled daemon does not hold other admin requests.
TEST_F(apps_stats_contention, OverlappingReadoutsShareTheDaemonAndDoNotHoldTheServer) {
  auto stream = start_fixture_stream(920170);
  stalled_admin_daemon_t daemon;
  tailscale_connection::set_default_status_reader_for_tests([&daemon] { return daemon.read(); });
  auto release = util::fail_guard([&] { daemon.release(); });

  auto first = request("/api/network/connection");
  ASSERT_TRUE(daemon.wait_entered());
  auto second = request("/api/network/connection");
  auto third = request("/api/network/connection");
  // Declared after the futures above, so a failed check below frees the daemon before they join.
  auto release_before_join = util::fail_guard([&] { daemon.release(); });
  // Both overlapping readouts are accepted waiters while the first read is held, so they must
  // share the next read; within the requests' own 2 s client deadlines.
  ASSERT_TRUE(wait_network_queued(2, 1s));
  // The admin server still answers other requests while the daemon is stalled.
  auto stats = request("/api/stats/stream");
  ASSERT_EQ(stats.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(stats.get().first.starts_with("200"));
  EXPECT_EQ(first.wait_for(0ms), std::future_status::timeout);

  daemon.release();
  release.disable();
  release_before_join.disable();
  for (auto *reply : {&first, &second, &third}) {
    const auto [status, body] = reply->get();
    ASSERT_TRUE(status.starts_with("200")) << status;
    const auto evidence = nlohmann::json::parse(body);
    EXPECT_EQ(evidence.at("status"), "tailscale");
    EXPECT_NE(evidence.at("tailscale").at("reason"), "localapi_unavailable");
  }
  // The first read, and at most one more shared by the requests that arrived during it.
  std::lock_guard lock {daemon.mutex};
  EXPECT_LE(daemon.entered, 2);
}
