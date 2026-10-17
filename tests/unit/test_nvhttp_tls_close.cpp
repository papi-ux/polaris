#include "../tests_common.h"
#include <src/nvhttp.h>

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <thread>

TEST(NvhttpTlsClose, CompletedResponseDoesNotWaitForThePeerToCloseTls) {
  using boost::asio::ip::tcp;
  const auto credentials = crypto::gen_creds("localhost", 2048);
  boost::asio::io_context server_io;
  boost::asio::ssl::context server_context(boost::asio::ssl::context::tls_server);
  server_context.use_certificate_chain(boost::asio::buffer(credentials.x509));
  server_context.use_private_key(boost::asio::buffer(credentials.pkey), boost::asio::ssl::context::pem);
  tcp::acceptor acceptor(server_io, {tcp::v4(), 0});
  const tcp::endpoint endpoint(boost::asio::ip::address_v4::loopback(), acceptor.local_endpoint().port());
  const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}";
  std::promise<void> received, release_peer;
  auto received_future = received.get_future();
  auto release_future = release_peer.get_future();
  std::jthread client([&] {
    try {
      boost::asio::io_context client_io;
      boost::asio::ssl::context client_context(boost::asio::ssl::context::tls_client);
      client_context.add_certificate_authority(boost::asio::buffer(credentials.x509));
      client_context.set_verify_mode(boost::asio::ssl::verify_peer);
      SimpleWeb::HTTPS peer(client_io, client_context);
      peer.lowest_layer().connect(endpoint);
      peer.handshake(boost::asio::ssl::stream_base::client);
      std::string body(response.size(), '\0');
      boost::asio::read(peer, boost::asio::buffer(body));
      if (body != response) throw std::runtime_error("incomplete HTTP response");
      received.set_value();
      // Keep TLS open after consuming the response. The deadline makes the old
      // blocking destructor fail this test without hanging the test process.
      release_future.wait_for(std::chrono::seconds(2));
    } catch (...) {
      received.set_exception(std::current_exception());
    }
  });

  auto server = std::make_unique<nvhttp::PolarisHTTPS>(server_io, server_context);
  acceptor.accept(server->lowest_layer());
  server->handshake(boost::asio::ssl::stream_base::server);
  boost::asio::write(*server, boost::asio::buffer(response));
  received_future.get();
  const auto start = std::chrono::steady_clock::now();
  // This runs on the same executor that must accept the next paired request.
  boost::asio::post(server_io, [&] { server.reset(); });
  bool next_request_can_run = false;
  boost::asio::post(server_io, [&] { next_request_can_run = true; });
  server_io.run();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  release_peer.set_value();
  EXPECT_TRUE(next_request_can_run);
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
}

#if defined(__linux__)
#include <Simple-Web-Server/client_https.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <src/config.h>
#include <src/rtsp.h>
#include <src/utility.h>
#include <src/uuid.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace {
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;
  using tree_t = boost::property_tree::ptree;

  class NativeEndHttp: public testing::Test {
  protected:
    struct credentials_t { fs::path cert, key; std::string pem; };
    credentials_t credentials(const std::string &name) {
      const auto c = crypto::gen_creds(name, 2048);
      credentials_t result {directory / (name + ".crt"), directory / (name + ".key"), c.x509};
      std::ofstream(result.cert) << c.x509;
      std::ofstream(result.key) << c.pkey;
      fs::permissions(result.key, fs::perms::owner_read | fs::perms::owner_write);
      return result;
    }
    void SetUp() override {
      directory = fs::temp_directory_path() / ("polaris-end-wire-" + uuid_util::uuid_t::generate().string());
      ASSERT_TRUE(fs::create_directory(directory));
      fs::permissions(directory, fs::perms::owner_all);
      host = credentials("host"); owner = credentials("owner"); other = credentials("other"); denied = credentials("denied"); unpaired = credentials("unpaired");
      previous_apps = proc::proc.get_apps();
      old_state = config::nvhttp.file_state; old_apps = config::stream.file_apps;
      old_fresh = config::sunshine.flags.test(config::flag::FRESH_STATE);
      configured = true;
      config::sunshine.flags.set(config::flag::FRESH_STATE);
      config::nvhttp.file_state = (directory / "state.json").string();
      config::stream.file_apps = (directory / "apps.json").string();
      std::ofstream(config::stream.file_apps) << R"({"apps":[]})";
      nvhttp::reset_pairing_state_for_tests();
      ASSERT_TRUE(authorize(owner, "end-owner", crypto::PERM::_game_control));
      ASSERT_TRUE(authorize(other, "other-owner", crypto::PERM::_game_control));
      ASSERT_TRUE(authorize(denied, "denied-owner", crypto::PERM::_no));
      auto launch = std::make_shared<rtsp_stream::launch_session_t>();
      launch->unique_id = "end-owner"; launch->device_name = "owner"; launch->session_token = "end-token";
      proc::ctx_t app; app.id = "424"; app.name = "Synthetic owned app"; app.uuid = "424-fixture";
      proc::proc.set_active_launch_for_tests(app, launch);
      proc::proc.set_running_app_for_tests(app); // placebo: no Steam, compositor or title process
      proc::proc.set_session_shutdown_observer_for_tests([this](bool complete) {
        if (complete) { ++cleanups; return; }
        std::unique_lock lock(mutex);
        entered = true; changed.notify_all();
        if (!changed.wait_for(lock, 15s, [this] { return released; })) barrier_expired = true;
      });
      route = std::make_unique<nvhttp::launch_route_for_tests_t>(host.cert.string(), host.key.string());
      listen_port = route->port();
      ASSERT_NE(listen_port, 0);
    }
    void TearDown() override {
      release();
      route.reset(); // stops admission/listener, then drains the owned End worker
      if (!configured) {
        if (!directory.empty()) { std::error_code ec; fs::remove_all(directory, ec); }
        return;
      }
      proc::proc.set_session_shutdown_observer_for_tests({});
      proc::proc.set_active_launch_for_tests(proc::ctx_t {}, nullptr);
      proc::proc.clear_running_app_for_tests();
      proc::proc.reload_configuration(proc::proc_t {boost::process::v1::environment {}, std::move(previous_apps)});
      nvhttp::reset_pairing_state_for_tests();
      config::nvhttp.file_state = old_state; config::stream.file_apps = old_apps;
      config::sunshine.flags.set(config::flag::FRESH_STATE, old_fresh);
      std::error_code ec; fs::remove_all(directory, ec);
    }
    bool authorize(const credentials_t &c, const std::string &id, crypto::PERM permission) {
      auto client = std::make_shared<crypto::named_cert_t>();
      client->name = id; client->uuid = id; client->cert = c.pem; client->enable_legacy_ordering = true;
      return nvhttp::add_authorized_client_for_tests(client, permission);
    }
    tree_t request(const credentials_t &c, const std::string &path) {
      SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(port()), false, c.cert.string(), c.key.string()};
      client.config.timeout = 5;
      const auto response = client.request("GET", path);
      tree_t tree; std::istringstream input(response->content.string()); boost::property_tree::read_xml(input, tree);
      return tree;
    }
    unsigned short port() const { return listen_port; }
    bool wait_entered() {
      std::unique_lock lock(mutex); return changed.wait_for(lock, 3s, [this] { return entered; });
    }
    void release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
    std::unique_ptr<nvhttp::launch_route_for_tests_t> route;
    credentials_t host, owner, other, denied, unpaired;
    fs::path directory;
    std::string old_state, old_apps;
    std::vector<proc::ctx_t> previous_apps;
    unsigned short listen_port = 0;
    bool old_fresh = false, entered = false, released = false, configured = false;
    std::atomic<bool> barrier_expired {false};
    std::atomic<int> cleanups {0};
    std::mutex mutex;
    std::condition_variable changed;
  };
}

TEST_F(NativeEndHttp, CancelAndInactiveServerinfoReachTheWireWhileRealCleanupIsHeld) {
  std::future<tree_t> cancel_reply, info_reply, busy_reply, list_reply, launch_reply;
  std::future<std::string> v1_reply;
  auto unblock = util::fail_guard([this] { release(); }); // releases before futures are destroyed
  cancel_reply = std::async(std::launch::async, [this] { return request(owner, "/cancel?sessiontoken=stale-owner-token"); });
  ASSERT_TRUE(wait_entered());
  EXPECT_EQ(cancel_reply.wait_for(1s), std::future_status::ready);
  // Deliberately occupy the same single executor with metadata readers/admission first.
  list_reply = std::async(std::launch::async, [this] { return request(owner, "/applist"); });
  EXPECT_EQ(list_reply.wait_for(1s), std::future_status::ready);
  launch_reply = std::async(std::launch::async, [this] { return request(owner, "/launch?appid=424"); });
  EXPECT_EQ(launch_reply.wait_for(1s), std::future_status::ready);
  v1_reply = std::async(std::launch::async, [this] {
    SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(port()), false, owner.cert.string(), owner.key.string()};
    client.config.timeout = 5;
    const auto reply = client.request("GET", "/polaris/v1/session/status");
    return reply->status_code + " " + reply->content.string();
  });
  EXPECT_EQ(v1_reply.wait_for(1s), std::future_status::ready);
  info_reply = std::async(std::launch::async, [this] { return request(owner, "/serverinfo"); });
  EXPECT_EQ(info_reply.wait_for(1s), std::future_status::ready);
  busy_reply = std::async(std::launch::async, [this] { return request(owner, "/cancel"); });
  EXPECT_EQ(busy_reply.wait_for(1s), std::future_status::ready);
  SimpleWeb::Client<SimpleWeb::HTTPS> unknown {"127.0.0.1:" + std::to_string(port()), false, owner.cert.string(), owner.key.string()};
  unknown.config.timeout = 5;
  EXPECT_EQ(unknown.request("GET", "/unknown-while-ending")->status_code.substr(0, 3), "404");
  EXPECT_EQ(cleanups.load(), 0);
  EXPECT_TRUE(proc::proc.session_shutdown_requested());
  EXPECT_FALSE(proc::proc.capture_session_launch_generation());
  // Read only replies already delivered; never release the barrier to make these assertions pass.
  if (cancel_reply.wait_for(0s) == std::future_status::ready) {
    const auto reply = cancel_reply.get();
    EXPECT_EQ(reply.get<int>("root.<xmlattr>.status_code"), 200);
    EXPECT_EQ(reply.get<int>("root.cancel"), 1);
  }
  if (info_reply.wait_for(0s) == std::future_status::ready) {
    const auto reply = info_reply.get();
    EXPECT_EQ(reply.get<int>("root.currentgame"), 0);
    EXPECT_EQ(reply.get<int>("root.session_cleanup_pending"), 1);
  }
  if (v1_reply.wait_for(0s) == std::future_status::ready) {
    const auto body = v1_reply.get();
    EXPECT_EQ(body.substr(0, 3), "503");
    EXPECT_NE(body.find("session_cleanup_pending"), std::string::npos);
  }
  if (list_reply.wait_for(0s) == std::future_status::ready)
    EXPECT_EQ(list_reply.get().get<int>("root.<xmlattr>.status_code"), 503);
  if (launch_reply.wait_for(0s) == std::future_status::ready)
    EXPECT_EQ(launch_reply.get().get<int>("root.<xmlattr>.status_code"), 409);
  if (busy_reply.wait_for(0s) == std::future_status::ready)
    EXPECT_EQ(busy_reply.get().get<int>("root.<xmlattr>.status_code"), 409);
  release();
  route.reset(); // joins the single admitted real terminate_impl, including its final gate release
  EXPECT_EQ(cleanups.load(), 1);
  EXPECT_FALSE(barrier_expired.load());
  EXPECT_FALSE(proc::proc.session_shutdown_requested());
  EXPECT_EQ(proc::proc.running(), 0);
}

TEST_F(NativeEndHttp, PermissionAndOtherOwnerRefusalsCannotAdmitCleanup) {
  auto unblock = util::fail_guard([this] { release(); });
  EXPECT_EQ(request(unpaired, "/cancel").get<int>("root.<xmlattr>.status_code"), 401);
  EXPECT_EQ(request(denied, "/cancel").get<int>("root.<xmlattr>.status_code"), 403);
  EXPECT_EQ(request(other, "/cancel").get<int>("root.<xmlattr>.status_code"), 470);
  SimpleWeb::Client<SimpleWeb::HTTPS> unknown {"127.0.0.1:" + std::to_string(port()), false, owner.cert.string(), owner.key.string()};
  unknown.config.timeout = 5;
  EXPECT_EQ(unknown.request("GET", "/unknown-424-path")->status_code.substr(0, 3), "404");
  EXPECT_EQ(cleanups.load(), 0);
  EXPECT_FALSE(entered);
  EXPECT_FALSE(proc::proc.session_shutdown_requested());
  EXPECT_EQ(proc::proc.running(), 424);
}

TEST_F(NativeEndHttp, ServerShutdownDrainsAcceptedEndBeforeDestroyingItsOwner) {
  std::future<tree_t> cancel_reply;
  std::future<void> shutdown;
  auto unblock = util::fail_guard([this] { release(); });
  cancel_reply = std::async(std::launch::async, [this] { return request(owner, "/cancel"); });
  ASSERT_TRUE(wait_entered());
  ASSERT_EQ(cancel_reply.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(cancel_reply.get().get<int>("root.cancel"), 1);
  shutdown = std::async(std::launch::async, [this] { route.reset(); });
  EXPECT_EQ(shutdown.wait_for(100ms), std::future_status::timeout);
  EXPECT_EQ(cleanups.load(), 0);
  release();
  ASSERT_EQ(shutdown.wait_for(3s), std::future_status::ready);
  shutdown.get();
  EXPECT_EQ(cleanups.load(), 1);
  EXPECT_FALSE(barrier_expired.load());
  EXPECT_FALSE(proc::proc.session_shutdown_requested());
}

TEST_F(NativeEndHttp, RevokedCertificateCannotReceiveAcceptedBusyOrRunTheV1Body) {
  std::future<tree_t> cancel_reply;
  auto unblock = util::fail_guard([this] { release(); });
  cancel_reply = std::async(std::launch::async, [this] { return request(owner, "/cancel"); });
  ASSERT_TRUE(wait_entered());
  ASSERT_EQ(cancel_reply.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(cancel_reply.get().get<int>("root.cancel"), 1);
  ASSERT_TRUE(nvhttp::unpair_client("other-owner")); // fixture-only nonowner, no running/pending session
  EXPECT_EQ(request(other, "/cancel").get<int>("root.<xmlattr>.status_code"), 401);
  SimpleWeb::Client<SimpleWeb::HTTPS> revoked {"127.0.0.1:" + std::to_string(port()), false, other.cert.string(), other.key.string()};
  revoked.config.timeout = 5;
  const auto reply = revoked.request("GET", "/polaris/v1/session/status");
  // The production TLS denial uses XML status401; an already-established connection
  // reaches the wrapper's HTTP401. Neither path is an authenticated busy503.
  if (reply->status_code.substr(0, 3) == "200") {
    tree_t xml; std::istringstream in(reply->content.string()); boost::property_tree::read_xml(in, xml);
    EXPECT_EQ(xml.get<int>("root.<xmlattr>.status_code"), 401);
  } else EXPECT_EQ(reply->status_code.substr(0, 3), "401");
  EXPECT_EQ(cleanups.load(), 0);
  release(); route.reset();
  EXPECT_EQ(cleanups.load(), 1);
}
#endif
