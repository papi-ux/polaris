/**
 * @file tests/unit/test_web_session_store.cpp
 * @brief Tests for durable fail-closed Web UI session state.
 */
#include "../tests_common.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <src/private_state_file.h>
#include <src/utility.h>
#include <src/web_session_store.h>

using namespace std::chrono_literals;

namespace {
  std::atomic_uint64_t path_counter {0};

  web_session_store::clock_snapshot_t at(std::int64_t wall_seconds, std::int64_t monotonic_seconds) {
    return {
      .wall = std::chrono::system_clock::time_point {std::chrono::seconds {wall_seconds}},
      .monotonic = std::chrono::steady_clock::time_point {std::chrono::seconds {monotonic_seconds}},
    };
  }

  std::string digest(char value) {
    return std::string(64, value);
  }

  class WebSessionStoreTest: public testing::Test {
  protected:
    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      directory = std::filesystem::temp_directory_path() /
                  ("polaris-web-session-store-" + std::to_string(nonce) + "-" +
                   std::to_string(path_counter.fetch_add(1)));
      std::filesystem::create_directories(directory);
      target = directory / "web_sessions.json";
      policy = {
        .absolute_lifetime = 100s,
        .idle_timeout = 20s,
        .touch_interval = 5s,
        .max_sessions = 2,
        .max_file_bytes = 16 * 1024,
      };
      private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
    }

    void TearDown() override {
      private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
      std::error_code ec;
      std::filesystem::remove_all(directory, ec);
    }

    std::filesystem::path directory;
    std::filesystem::path target;
    web_session_store::policy_t policy;
    const std::string fingerprint = digest('f');
  };
}

TEST_F(WebSessionStoreTest, ValidatesExactSha256Hex) {
  EXPECT_TRUE(web_session_store::is_valid_sha256_hex(digest('a')));
  EXPECT_TRUE(web_session_store::is_valid_sha256_hex(digest('9')));
  EXPECT_FALSE(web_session_store::is_valid_sha256_hex(std::string(63, 'a')));
  EXPECT_FALSE(web_session_store::is_valid_sha256_hex(std::string(64, 'A')));
  EXPECT_FALSE(web_session_store::is_valid_sha256_hex(std::string(64, 'g')));
}

TEST_F(WebSessionStoreTest, CanonicalizesUppercaseDigestInputs) {
  const auto uppercase_fingerprint = digest('F');
  const auto uppercase_id = digest('A');
  web_session_store::manager_t manager(target, uppercase_fingerprint, policy);

  ASSERT_TRUE(manager.create(uppercase_id, at(1000, 0)));
  EXPECT_TRUE(manager.fingerprint_matches(uppercase_fingerprint));
  EXPECT_EQ(manager.validate(uppercase_id, at(1001, 1)), web_session_store::validation_status_e::valid);

  const auto payload = nlohmann::json::parse(
    private_state_file::read_secure(target, policy.max_file_bytes).payload
  );
  EXPECT_EQ(payload.at("credential_fingerprint"), digest('f'));
  ASSERT_EQ(payload.at("sessions").size(), 1U);
  EXPECT_EQ(payload.at("sessions").at(0).at("id"), digest('a'));

  web_session_store::manager_t restarted(target, uppercase_fingerprint, policy);
  EXPECT_EQ(restarted.load(at(1002, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(restarted.validate(uppercase_id, at(1002, 0)), web_session_store::validation_status_e::valid);
}

TEST_F(WebSessionStoreTest, AcceptsActualPolarisHexEncoderOutput) {
  std::array<unsigned char, 32> fingerprint_bytes {};
  std::array<unsigned char, 32> session_bytes {};
  for (std::size_t index = 0; index < fingerprint_bytes.size(); ++index) {
    fingerprint_bytes[index] = static_cast<unsigned char>(0xA0U + index);
    session_bytes[index] = static_cast<unsigned char>(0xC0U + index);
  }
  const auto encoded_fingerprint = util::hex(fingerprint_bytes).to_string();
  const auto encoded_session = util::hex(session_bytes).to_string();
  ASSERT_EQ(encoded_fingerprint.size(), 64U);
  ASSERT_EQ(encoded_session.size(), 64U);
  ASSERT_NE(encoded_fingerprint.find_first_of("ABCDEF"), std::string::npos);
  ASSERT_NE(encoded_session.find_first_of("ABCDEF"), std::string::npos);

  web_session_store::manager_t manager(target, encoded_fingerprint, policy);
  ASSERT_TRUE(manager.create(encoded_session, at(1000, 0)));

  web_session_store::manager_t restarted(target, encoded_fingerprint, policy);
  ASSERT_EQ(restarted.load(at(1001, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(
    restarted.validate(encoded_session, at(1001, 1)),
    web_session_store::validation_status_e::valid
  );
}

TEST_F(WebSessionStoreTest, PersistsAndRestoresSessionAcrossManagerRestart) {
  const auto id = digest('a');
  web_session_store::manager_t first(target, fingerprint, policy);
  EXPECT_EQ(first.load(at(1000, 0)), web_session_store::load_status_e::missing);
  ASSERT_TRUE(first.create(id, at(1000, 0)));
  EXPECT_EQ(first.validate(id, at(1001, 1)), web_session_store::validation_status_e::valid);

  web_session_store::manager_t restarted(target, fingerprint, policy);
  EXPECT_EQ(restarted.load(at(1002, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(restarted.validate(id, at(1002, 0)), web_session_store::validation_status_e::valid);
  EXPECT_EQ(restarted.size(), 1U);
}

TEST_F(WebSessionStoreTest, RejectsCredentialFingerprintMismatch) {
  web_session_store::manager_t first(target, fingerprint, policy);
  ASSERT_TRUE(first.create(digest('a'), at(1000, 0)));

  web_session_store::manager_t restarted(target, digest('e'), policy);
  EXPECT_EQ(restarted.load(at(1001, 0)), web_session_store::load_status_e::rejected);
  EXPECT_EQ(restarted.size(), 0U);
}

TEST_F(WebSessionStoreTest, ExpiresAtIdleBoundaryAfterRestart) {
  const auto id = digest('a');
  web_session_store::manager_t first(target, fingerprint, policy);
  ASSERT_TRUE(first.create(id, at(1000, 0)));

  web_session_store::manager_t restarted(target, fingerprint, policy);
  EXPECT_EQ(restarted.load(at(1020, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(restarted.validate(id, at(1020, 0)), web_session_store::validation_status_e::invalid);
  EXPECT_EQ(restarted.size(), 0U);
}

TEST_F(WebSessionStoreTest, ExpiresAtAbsoluteBoundaryAfterRestart) {
  auto absolute_policy = policy;
  absolute_policy.idle_timeout = 200s;
  const auto id = digest('a');
  web_session_store::manager_t first(target, fingerprint, absolute_policy);
  ASSERT_TRUE(first.create(id, at(1000, 0)));

  web_session_store::manager_t restarted(target, fingerprint, absolute_policy);
  EXPECT_EQ(restarted.load(at(1100, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(restarted.validate(id, at(1100, 0)), web_session_store::validation_status_e::invalid);
}

TEST_F(WebSessionStoreTest, MonotonicDeadlinePreventsClockRollbackExtension) {
  const auto id = digest('a');
  web_session_store::manager_t manager(target, fingerprint, policy);
  ASSERT_TRUE(manager.create(id, at(1000, 0)));
  EXPECT_EQ(manager.validate(id, at(900, 20)), web_session_store::validation_status_e::invalid);
}

TEST_F(WebSessionStoreTest, FailedCreateNeverBecomesVisible) {
  const auto id = digest('a');
  web_session_store::manager_t manager(target, fingerprint, policy);
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::open);
  EXPECT_FALSE(manager.create(id, at(1000, 0)));
  EXPECT_EQ(manager.size(), 0U);
  EXPECT_EQ(manager.validate(id, at(1001, 1)), web_session_store::validation_status_e::invalid);
}

TEST_F(WebSessionStoreTest, FailedRevocationLeavesExistingSessionValid) {
  const auto id = digest('a');
  web_session_store::manager_t manager(target, fingerprint, policy);
  ASSERT_TRUE(manager.create(id, at(1000, 0)));
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::rename);
  EXPECT_FALSE(manager.invalidate(id));
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  EXPECT_EQ(manager.validate(id, at(1001, 1)), web_session_store::validation_status_e::valid);
}

TEST_F(WebSessionStoreTest, TouchIntervalPersistsIdleExtension) {
  const auto id = digest('a');
  web_session_store::manager_t manager(target, fingerprint, policy);
  ASSERT_TRUE(manager.create(id, at(1000, 0)));
  ASSERT_EQ(manager.validate(id, at(1006, 6)), web_session_store::validation_status_e::valid);

  web_session_store::manager_t restarted(target, fingerprint, policy);
  EXPECT_EQ(restarted.load(at(1025, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(restarted.validate(id, at(1025, 0)), web_session_store::validation_status_e::valid);
}

TEST_F(WebSessionStoreTest, FailedRequiredTouchFailsClosedAndRetriesPersistence) {
  const auto id = digest('a');
  web_session_store::manager_t manager(target, fingerprint, policy);
  ASSERT_TRUE(manager.create(id, at(1000, 0)));

  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::rename);
  EXPECT_EQ(
    manager.validate(id, at(1006, 6)),
    web_session_store::validation_status_e::io_error
  );

  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  EXPECT_EQ(manager.validate(id, at(1007, 7)), web_session_store::validation_status_e::valid);

  web_session_store::manager_t restarted(target, fingerprint, policy);
  EXPECT_EQ(restarted.load(at(1026, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(restarted.validate(id, at(1026, 0)), web_session_store::validation_status_e::valid);
}

TEST_F(WebSessionStoreTest, FailedStartupPruneRejectsRemainingSessions) {
  const auto expired_id = digest('a');
  const auto valid_id = digest('b');
  web_session_store::manager_t first(target, fingerprint, policy);
  ASSERT_TRUE(first.create(expired_id, at(1000, 0)));
  ASSERT_TRUE(first.create(valid_id, at(1010, 10)));

  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::rename);
  web_session_store::manager_t restarted(target, fingerprint, policy);
  EXPECT_EQ(restarted.load(at(1020, 0)), web_session_store::load_status_e::io_error);
  EXPECT_EQ(restarted.size(), 0U);
  EXPECT_EQ(
    restarted.validate(valid_id, at(1020, 0)),
    web_session_store::validation_status_e::io_error
  );
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  EXPECT_FALSE(restarted.invalidate(valid_id));
}

TEST_F(WebSessionStoreTest, FailedClockRollbackRevocationIsUnavailableAndRetryable) {
  const auto id = digest('a');
  web_session_store::manager_t manager(target, fingerprint, policy);
  ASSERT_TRUE(manager.create(id, at(1000, 0)));

  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::rename);
  EXPECT_EQ(
    manager.validate(id, at(900, 1)),
    web_session_store::validation_status_e::io_error
  );

  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  EXPECT_EQ(
    manager.validate(id, at(1001, 2)),
    web_session_store::validation_status_e::valid
  );
}

TEST_F(WebSessionStoreTest, ClockRollbackCreateFailsWithoutPruningExistingSession) {
  const auto old_id = digest('a');
  const auto new_id = digest('b');
  web_session_store::manager_t manager(target, fingerprint, policy);
  ASSERT_TRUE(manager.create(old_id, at(1000, 0)));

  EXPECT_FALSE(manager.create(new_id, at(900, 1)));
  EXPECT_EQ(
    manager.validate(old_id, at(1001, 2)),
    web_session_store::validation_status_e::valid
  );
  EXPECT_EQ(
    manager.validate(new_id, at(1001, 2)),
    web_session_store::validation_status_e::invalid
  );
}

TEST_F(WebSessionStoreTest, RejectsMalformedDocumentsAsAWhole) {
  nlohmann::json malformed = {
    {"version", 1},
    {"credential_fingerprint", fingerprint},
    {"sessions", nlohmann::json::array({
      {{"id", digest('a')}, {"created_at", 1000}, {"last_seen_at", 1000}},
      {{"id", digest('a')}, {"created_at", 1000}, {"last_seen_at", 1000}},
    })},
  };
  ASSERT_TRUE(private_state_file::write_atomic(target, malformed.dump()));

  web_session_store::manager_t manager(target, fingerprint, policy);
  EXPECT_EQ(manager.load(at(1001, 0)), web_session_store::load_status_e::rejected);
  EXPECT_EQ(manager.size(), 0U);
}

TEST_F(WebSessionStoreTest, CapacityEvictsLeastRecentlySeenSession) {
  const auto first = digest('a');
  const auto second = digest('b');
  const auto third = digest('c');
  web_session_store::manager_t manager(target, fingerprint, policy);
  ASSERT_TRUE(manager.create(first, at(1000, 0)));
  ASSERT_TRUE(manager.create(second, at(1001, 1)));
  ASSERT_TRUE(manager.create(third, at(1002, 2)));

  EXPECT_EQ(manager.validate(first, at(1003, 3)), web_session_store::validation_status_e::invalid);
  EXPECT_EQ(manager.validate(second, at(1003, 3)), web_session_store::validation_status_e::valid);
  EXPECT_EQ(manager.validate(third, at(1003, 3)), web_session_store::validation_status_e::valid);
}

TEST_F(WebSessionStoreTest, FailedCredentialRotationKeepsOldGenerationValid) {
  const auto old_id = digest('a');
  const auto new_fingerprint = digest('e');
  web_session_store::manager_t manager(target, fingerprint, policy);
  ASSERT_TRUE(manager.create(old_id, at(1000, 0)));
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::rename);
  EXPECT_FALSE(manager.rotate_credentials(new_fingerprint, at(1001, 1)));
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  EXPECT_TRUE(manager.fingerprint_matches(fingerprint));
  EXPECT_EQ(manager.validate(old_id, at(1001, 1)), web_session_store::validation_status_e::valid);

  web_session_store::manager_t restarted(target, fingerprint, policy);
  EXPECT_EQ(restarted.load(at(1002, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(restarted.validate(old_id, at(1002, 0)), web_session_store::validation_status_e::valid);
}

TEST_F(WebSessionStoreTest, RejectsFutureTimestamps) {
  nlohmann::json future = {
    {"version", 1},
    {"credential_fingerprint", fingerprint},
    {"sessions", nlohmann::json::array({
      {{"id", digest('a')}, {"created_at", 1002}, {"last_seen_at", 1002}},
    })},
  };
  ASSERT_TRUE(private_state_file::write_atomic(target, future.dump()));
  web_session_store::manager_t manager(target, fingerprint, policy);
  EXPECT_EQ(manager.load(at(1001, 0)), web_session_store::load_status_e::rejected);
}

TEST_F(WebSessionStoreTest, CredentialRotationPersistsEmptyNewGeneration) {
  const auto old_id = digest('a');
  const auto new_fingerprint = digest('e');
  web_session_store::manager_t manager(target, fingerprint, policy);
  ASSERT_TRUE(manager.create(old_id, at(1000, 0)));
  ASSERT_TRUE(manager.rotate_credentials(new_fingerprint, at(1001, 1)));
  EXPECT_TRUE(manager.fingerprint_matches(new_fingerprint));
  EXPECT_EQ(manager.validate(old_id, at(1001, 1)), web_session_store::validation_status_e::invalid);

  web_session_store::manager_t restarted(target, new_fingerprint, policy);
  EXPECT_EQ(restarted.load(at(1002, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(restarted.size(), 0U);
}

TEST_F(WebSessionStoreTest, ConcurrentCredentialRotationAndSessionOperationsAreSynchronized) {
  const auto session = digest('a');
  web_session_store::manager_t manager(target, fingerprint, policy);
  std::barrier start {3};

  std::thread rotator([&]() {
    start.arrive_and_wait();
    for (int attempt = 0; attempt < 256; ++attempt) {
      (void) manager.rotate_credentials(attempt % 2 == 0 ? digest('e') : fingerprint, at(1000, attempt));
    }
  });
  std::thread session_worker([&]() {
    start.arrive_and_wait();
    for (int attempt = 0; attempt < 256; ++attempt) {
      (void) manager.create(session, at(1000, attempt));
      (void) manager.validate(session, at(1000, attempt));
    }
  });

  start.arrive_and_wait();
  rotator.join();
  session_worker.join();
  SUCCEED();
}

TEST_F(WebSessionStoreTest, StaleCredentialReconcileDoesNotDeleteSessionFromWinningRequest) {
  const auto new_fingerprint = digest('e');
  const auto winning_session = digest('a');
  web_session_store::manager_t manager(target, fingerprint, policy);

  // Model two callers that both observed the previous generation. The first caller
  // rotates and publishes a session before the stale second caller reconciles.
  ASSERT_FALSE(manager.fingerprint_matches(new_fingerprint));
  ASSERT_TRUE(manager.rotate_credentials(new_fingerprint, at(1000, 0)));
  ASSERT_TRUE(manager.create(winning_session, at(1001, 1)));

  ASSERT_TRUE(manager.rotate_credentials(new_fingerprint, at(1002, 2)));
  EXPECT_EQ(
    manager.validate(winning_session, at(1002, 2)),
    web_session_store::validation_status_e::valid
  );

  web_session_store::manager_t restarted(target, new_fingerprint, policy);
  ASSERT_EQ(restarted.load(at(1003, 0)), web_session_store::load_status_e::loaded);
  EXPECT_EQ(
    restarted.validate(winning_session, at(1003, 0)),
    web_session_store::validation_status_e::valid
  );
}
