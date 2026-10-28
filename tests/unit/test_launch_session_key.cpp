/**
 * @file tests/unit/test_launch_session_key.cpp
 * @brief Validate client key length before OpenSSL receives the key buffer.
 */

#include <src/crypto.h>
#include <src/httpcommon.h>
#include <src/nvhttp.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

TEST(LaunchSessionVrrRequest, AuthenticatedLaunchRecordsEachRequestFormWithoutRefusing) {
  struct case_t {
    std::vector<std::string> values;
    vrr_policy::requested_e expected;
  };
  const std::vector<case_t> cases {
    {{}, vrr_policy::requested_e::absent},
    {{"1"}, vrr_policy::requested_e::on},
    {{"true"}, vrr_policy::requested_e::on},
    {{"0"}, vrr_policy::requested_e::off},
    {{"maybe"}, vrr_policy::requested_e::malformed},
    {{"1", "1"}, vrr_policy::requested_e::malformed},
  };
  for (const auto &item : cases) {
    auto cert = client_cert();
    auto args = launch_args_for_key_bytes(crypto::cipher::key_size);
    for (const auto &value : item.values) {
      args.emplace("clientVrrRequested", value);
    }
    const auto session = nvhttp::make_launch_session(true, false, args, cert.get());
    // A malformed or repeated flag is recorded as such; it never refuses an otherwise valid launch.
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(session->vrr_requested, item.expected);
    EXPECT_EQ(session->vrr_admission, vrr_policy::admission_e::launch);
  }
}

TEST(LaunchSessionVrrRequest, ResumeAdmissionIsNamedAndTheFlagIsCaseInsensitive) {
  auto cert = client_cert();
  auto args = launch_args_for_key_bytes(crypto::cipher::key_size);
  args.emplace("CLIENTVRRREQUESTED", "1");
  const auto session = nvhttp::make_launch_session(
    true, false, args, cert.get(), false, nullptr, vrr_policy::admission_e::resume
  );
  ASSERT_NE(session, nullptr);
  EXPECT_EQ(session->vrr_requested, vrr_policy::requested_e::on);
  EXPECT_EQ(session->vrr_admission, vrr_policy::admission_e::resume);
}

TEST(LaunchSessionVrrRequest, AWatchRequestKeepsItsFlagButIsWatchOnly) {
  auto cert = client_cert();
  auto args = launch_args_for_key_bytes(crypto::cipher::key_size);
  args.emplace("clientVrrRequested", "1");
  args.emplace("watch", "1");
  const auto session = nvhttp::make_launch_session(true, false, args, cert.get());
  ASSERT_NE(session, nullptr);
  EXPECT_TRUE(session->watch_only);
  EXPECT_EQ(session->vrr_requested, vrr_policy::requested_e::on);
  const auto record = vrr_policy::make_record(
    9, session->vrr_requested, session->vrr_admission, session->watch_only, session->input_only
  );
  EXPECT_EQ(vrr_policy::decide(record, "source_driven").fallback_reason, "watch_or_input_only");
}

TEST(LaunchSessionKey, HostBuiltSessionNeedsNoClientKey) {
  auto cert = client_cert(http::unique_id);
  nvhttp::args_t args;
  args.emplace("mode", "1280x720x60");

  const auto session = nvhttp::make_launch_session(true, false, args, cert.get());

  ASSERT_NE(session, nullptr);
  EXPECT_TRUE(session->gcm_key.empty());
}

TEST(NvhttpLaunchQuery, CoreverKeepsMissingEmptyAndNondigitArithmetic) {
  const std::vector<std::pair<std::string, std::int64_t>> cases {
    {"", 0}, {"corever", 0}, {"COREVER=", 0}, {"corever=0", 0},
    {"corever=1", 1}, {"corever=-1", -1}, {"corever=x", 72}
  };
  for (const auto &[query, expected] : cases) {
    SCOPED_TRACE(query);
    const auto args = SimpleWeb::QueryString::parse(query);
    EXPECT_EQ(nvhttp::decode_client_corever(args), expected);
  }
}

TEST(NvhttpLaunchQuery, RikeyIdKeepsRequiredEmptyNarrowAndBigEndianBytes) {
  EXPECT_THROW(nvhttp::decode_client_iv_prefix(nvhttp::args_t {}), std::out_of_range);
  const std::vector<std::pair<std::string, std::array<unsigned char, 4>>> cases {
    {"rikeyid", {0, 0, 0, 0}}, {"RIKEYID=", {0, 0, 0, 0}},
    {"rikeyid=0", {0, 0, 0, 0}}, {"rikeyid=1", {0, 0, 0, 1}},
    {"rikeyid=16909060", {1, 2, 3, 4}}, {"rikeyid=-1", {255, 255, 255, 255}},
    {"rikeyid=4294967296", {0, 0, 0, 0}}, {"rikeyid=x", {0, 0, 0, 72}}
  };
  for (const auto &[query, expected] : cases) {
    SCOPED_TRACE(query);
    const auto args = SimpleWeb::QueryString::parse(query);
    const auto prefix = nvhttp::decode_client_iv_prefix(args);
    ASSERT_EQ(sizeof(prefix), expected.size());
    const auto *bytes = reinterpret_cast<const unsigned char *>(&prefix);
    for (std::size_t i = 0; i < expected.size(); ++i) {
      EXPECT_EQ(bytes[i], expected[i]) << i;
    }
  }
}

TEST(NvhttpLaunchQuery, RikeyKeepsLenientHexAndRightmostOddPairs) {
  EXPECT_THROW(nvhttp::decode_client_rikey(nvhttp::args_t {}), std::out_of_range);
  const auto decode = [](const std::string &value) {
    nvhttp::args_t args;
    args.emplace("rikey", value);
    return nvhttp::decode_client_rikey(args);
  };
  EXPECT_TRUE(decode("").empty());
  EXPECT_EQ(decode("0102"), std::string("\x01\x02", 2));
  EXPECT_EQ(decode(std::string(32, '0')), std::string(16, '\0'));
  EXPECT_EQ(decode(std::string(32, 'z')), std::string(16, '3'));
  EXPECT_EQ(decode("a!b?c"), std::string(1, static_cast<char>(0xbc)));
  EXPECT_EQ(decode("f" + std::string(32, '0')), std::string(16, '\0'));
  const auto escaped = SimpleWeb::QueryString::parse("RIKEY=%61%62%63&ri%6bey=ff");
  EXPECT_EQ(nvhttp::decode_client_rikey(escaped), std::string(1, static_cast<char>(0xbc)));
}

TEST(LaunchSessionKey, ShortKeyRefusesBeforeMissingRikeyId) {
  auto cert = client_cert();
  auto args = launch_args_for_key_bytes(1);
  args.erase("rikeyid");
  args.emplace("corever", "1");
  EXPECT_NO_THROW({
    EXPECT_EQ(nvhttp::make_launch_session(true, false, args, cert.get()), nullptr);
  });
}
