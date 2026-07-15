/**
 * @file tests/unit/test_private_state_file.cpp
 * @brief Tests for secure authorization-adjacent private state persistence.
 */
#include "../tests_common.h"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <src/private_state_file.h>

namespace {
  std::atomic_uint64_t path_counter {0};

  class PrivateStateFileTest: public testing::Test {
  protected:
    void SetUp() override {
      const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
      directory = std::filesystem::temp_directory_path() /
                  ("polaris-private-state-" + std::to_string(nonce) + "-" +
                   std::to_string(path_counter.fetch_add(1)));
      std::filesystem::create_directories(directory);
      target = directory / "state.json";
      private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
    }

    void TearDown() override {
      private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
      std::error_code ec;
      std::filesystem::remove_all(directory, ec);
    }

    std::filesystem::path directory;
    std::filesystem::path target;
  };
}

TEST_F(PrivateStateFileTest, DoesNotCarryUnsupportedWindowsSecurityImplementation) {
  std::ifstream source(std::string {POLARIS_SOURCE_DIR} + "/src/private_state_file.cpp");
  ASSERT_TRUE(source.is_open());
  std::ostringstream contents;
  contents << source.rdbuf();

  EXPECT_EQ(contents.str().find("CreateFileW"), std::string::npos);
  EXPECT_EQ(contents.str().find("SetSecurityInfo"), std::string::npos);
  EXPECT_EQ(contents.str().find("MoveFileExW"), std::string::npos);
}

TEST_F(PrivateStateFileTest, DistinguishesMissingFiles) {
  const auto result = private_state_file::read_secure(target, 1024);
  EXPECT_EQ(result.status, private_state_file::read_status_e::missing);
  EXPECT_TRUE(result.payload.empty());
}

TEST_F(PrivateStateFileTest, RoundTripsPrivateBoundedPayload) {
  ASSERT_TRUE(private_state_file::write_atomic(target, R"({"status":true})"));
  const auto result = private_state_file::read_secure(target, 1024);
  ASSERT_EQ(result.status, private_state_file::read_status_e::ok);
  EXPECT_EQ(result.payload, R"({"status":true})");

#ifndef _WIN32
  const auto permissions = std::filesystem::status(target).permissions();
  EXPECT_EQ(
    permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
    std::filesystem::perms::none
  );
  const auto lock_permissions = std::filesystem::status(target.string() + ".lock").permissions();
  EXPECT_EQ(
    lock_permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
    std::filesystem::perms::none
  );
#endif
}

TEST_F(PrivateStateFileTest, RejectsOversizedPayload) {
  ASSERT_TRUE(private_state_file::write_atomic(target, std::string(32, 'x')));
  const auto result = private_state_file::read_secure(target, 8);
  EXPECT_EQ(result.status, private_state_file::read_status_e::rejected);
  EXPECT_TRUE(result.payload.empty());
}

#ifndef _WIN32
TEST_F(PrivateStateFileTest, RejectsLoosePermissions) {
  ASSERT_TRUE(private_state_file::write_atomic(target, "private"));
  std::filesystem::permissions(
    target,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
      std::filesystem::perms::group_read,
    std::filesystem::perm_options::replace
  );
  const auto result = private_state_file::read_secure(target, 1024);
  EXPECT_EQ(result.status, private_state_file::read_status_e::rejected);
}

TEST_F(PrivateStateFileTest, RejectsSymlinkTargets) {
  const auto real_target = directory / "real.json";
  {
    std::ofstream output(real_target);
    output << "private";
  }
  std::filesystem::permissions(
    real_target,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace
  );
  std::filesystem::create_symlink(real_target, target);
  const auto result = private_state_file::read_secure(target, 1024);
  EXPECT_EQ(result.status, private_state_file::read_status_e::rejected);
}


TEST_F(PrivateStateFileTest, AtomicWriteReplacesSymlinkWithoutChangingReferent) {
  const auto real_target = directory / "real.json";
  {
    std::ofstream output(real_target);
    output << "referent";
  }
  std::filesystem::create_symlink(real_target, target);

  ASSERT_TRUE(private_state_file::write_atomic(target, "replacement"));
  EXPECT_FALSE(std::filesystem::is_symlink(target));
  const auto replacement = private_state_file::read_secure(target, 1024);
  ASSERT_EQ(replacement.status, private_state_file::read_status_e::ok);
  EXPECT_EQ(replacement.payload, "replacement");

  std::ifstream original(real_target);
  std::string original_payload;
  original >> original_payload;
  EXPECT_EQ(original_payload, "referent");
}

TEST_F(PrivateStateFileTest, RejectsSymlinkedLockFileWithoutChangingReferent) {
  const auto lock_referent = directory / "lock-referent";
  {
    std::ofstream output(lock_referent);
    output << "sentinel";
  }
  std::filesystem::create_symlink(lock_referent, target.string() + ".lock");

  EXPECT_FALSE(private_state_file::write_atomic(target, "private"));
  std::ifstream original(lock_referent);
  std::string original_payload;
  original >> original_payload;
  EXPECT_EQ(original_payload, "sentinel");
}
#endif

TEST_F(PrivateStateFileTest, PreCommitFaultsPreserveOldPayloadAndCleanTemps) {
  ASSERT_TRUE(private_state_file::write_atomic(target, "old"));
  constexpr std::array faults {
    private_state_file::write_fault_e::open,
    private_state_file::write_fault_e::short_write,
    private_state_file::write_fault_e::flush,
    private_state_file::write_fault_e::sync,
    private_state_file::write_fault_e::rename,
  };

  for (const auto fault : faults) {
    private_state_file::set_write_fault_for_tests(fault);
    EXPECT_FALSE(private_state_file::write_atomic(target, "new"));
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
    const auto result = private_state_file::read_secure(target, 1024);
    ASSERT_EQ(result.status, private_state_file::read_status_e::ok);
    EXPECT_EQ(result.payload, "old");
  }

  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    EXPECT_EQ(entry.path().filename().string().find("state.json.tmp."), std::string::npos);
  }
}

#if defined(__linux__)
TEST_F(PrivateStateFileTest, SyncFailureClosesTemporaryDescriptor) {
  const auto descriptor_count = []() {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      ++count;
    }
    return count;
  };
  const auto before = descriptor_count();
  for (int attempt = 0; attempt < 32; ++attempt) {
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::sync);
    EXPECT_FALSE(private_state_file::write_atomic(target, "private"));
  }
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  const auto after = descriptor_count();
  EXPECT_LE(after, before + 1);
}
#endif

TEST_F(PrivateStateFileTest, PostRenameDurabilityFaultReportsCommittedWrite) {
  ASSERT_TRUE(private_state_file::write_atomic(target, "old"));
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::post_rename_durability);
  EXPECT_TRUE(private_state_file::write_atomic(target, "new"));
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  const auto result = private_state_file::read_secure(target, 1024);
  ASSERT_EQ(result.status, private_state_file::read_status_e::ok);
  EXPECT_EQ(result.payload, "new");
}