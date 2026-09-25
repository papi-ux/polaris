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
