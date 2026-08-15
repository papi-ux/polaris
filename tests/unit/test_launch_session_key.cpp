/**
 * @file tests/unit/test_launch_session_key.cpp
 * @brief Validate client key length before OpenSSL receives the key buffer.
 */

#include <src/crypto.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

namespace {
  std::unique_ptr<crypto::named_cert_t> client_cert(std::string uuid = "remote-client") {
    auto cert = std::make_unique<crypto::named_cert_t>();
    cert->name = "test-client";
    cert->uuid = std::move(uuid);
    cert->perm = crypto::PERM::_game_control;
    cert->enable_legacy_ordering = false;
    cert->allow_client_commands = false;
    cert->always_use_virtual_display = false;
    return cert;
  }

  nvhttp::args_t launch_args_for_key_bytes(std::size_t bytes) {
    nvhttp::args_t args;
    args.emplace("rikey", std::string(bytes * 2, '0'));
    args.emplace("rikeyid", "1");
    args.emplace("mode", "1920x1080x60");
    return args;
  }
}

TEST(LaunchSessionKey, AcceptsExactlyOneAes128Key) {
  auto cert = client_cert();
  const auto session = nvhttp::make_launch_session(
    true,
    false,
    launch_args_for_key_bytes(crypto::cipher::key_size),
    cert.get()
  );

  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->gcm_key.size(), crypto::cipher::key_size);
}

TEST(LaunchSessionKey, RejectsShortClientKeys) {
  for (const auto bytes : {std::size_t {0}, std::size_t {1}, std::size_t {8}, std::size_t {15}}) {
    auto cert = client_cert();
    EXPECT_EQ(nvhttp::make_launch_session(true, false, launch_args_for_key_bytes(bytes), cert.get()), nullptr)
      << "accepted a " << bytes << "-byte key";
  }
}

TEST(LaunchSessionKey, RejectsLongClientKeys) {
  for (const auto bytes : {std::size_t {17}, std::size_t {32}}) {
    auto cert = client_cert();
    EXPECT_EQ(nvhttp::make_launch_session(true, false, launch_args_for_key_bytes(bytes), cert.get()), nullptr)
      << "accepted a " << bytes << "-byte key";
  }
}

TEST(LaunchSessionKey, HostBuiltSessionNeedsNoClientKey) {
  auto cert = client_cert(http::unique_id);
  nvhttp::args_t args;
  args.emplace("mode", "1280x720x60");

  const auto session = nvhttp::make_launch_session(true, false, args, cert.get());

  ASSERT_NE(session, nullptr);
  EXPECT_TRUE(session->gcm_key.empty());
}
