/**
 * @file tests/unit/test_private_state_file.cpp
 * @brief Tests for secure authorization-adjacent private state persistence.
 */
#include "../tests_common.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(__linux__)
  #include <csignal>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <thread>
  #include <unistd.h>
#endif

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
      private_state_file::set_parent_component_fault_index_for_tests(0);
      private_state_file::set_parent_eexist_race_for_tests(false);
    }

    void TearDown() override {
      private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
      private_state_file::set_parent_component_fault_index_for_tests(0);
      private_state_file::set_parent_eexist_race_for_tests(false);
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

  const auto source_text = contents.str();
  EXPECT_EQ(source_text.find("CreateFileW"), std::string::npos);
  EXPECT_EQ(source_text.find("SetSecurityInfo"), std::string::npos);
  EXPECT_EQ(source_text.find("MoveFileExW"), std::string::npos);
  const auto count_occurrences = [&](std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t offset = 0; (offset = source_text.find(needle, offset)) != std::string::npos;
         offset += needle.size()) {
      ++count;
    }
    return count;
  };
  EXPECT_GE(count_occurrences("::openat("), 3U);
  EXPECT_NE(source_text.find("::renameat("), std::string::npos);
  EXPECT_NE(source_text.find("::unlinkat("), std::string::npos);
  EXPECT_NE(source_text.find("::mkdirat("), std::string::npos);
  EXPECT_NE(source_text.find("path_.relative_path()"), std::string::npos);
  EXPECT_NE(source_text.find("metadata.st_uid == 0 || metadata.st_uid == effective_user"), std::string::npos);
  EXPECT_NE(source_text.find("metadata.st_mode & S_ISVTX"), std::string::npos);
  EXPECT_NE(source_text.find("name == \"..\""), std::string::npos);
  EXPECT_NE(source_text.find("directory.sync()"), std::string::npos);
  EXPECT_NE(source_text.find("if (!directory_synchronized)"), std::string::npos);
  EXPECT_NE(source_text.find("directory.close()"), std::string::npos);
  EXPECT_NE(source_text.find("if (!directory_close_succeeded)"), std::string::npos);
  EXPECT_EQ(source_text.find("std::filesystem::rename("), std::string::npos);

  std::ifstream session_source(std::string {POLARIS_SOURCE_DIR} + "/src/web_session_store.cpp");
  ASSERT_TRUE(session_source.is_open());
  std::ostringstream session_contents;
  session_contents << session_source.rdbuf();
  EXPECT_EQ(session_contents.str().find("std::filesystem::create_directories("), std::string::npos);
}

TEST_F(PrivateStateFileTest, SyncsCreatedParentEntryBeforeDescending) {
  std::ifstream source(std::string {POLARIS_SOURCE_DIR} + "/src/private_state_file.cpp");
  ASSERT_TRUE(source.is_open());
  std::ostringstream contents;
  contents << source.rdbuf();
  const auto source_text = contents.str();

  const auto create = source_text.find("::mkdirat(");
  const auto mark_for_sync = source_text.find("parent_entry_needs_sync = true", create);
  const auto reopen = source_text.find("next = ::openat(", mark_for_sync);
  const auto validate = source_text.find("if (next < 0 || !secure_directory_descriptor", reopen);
  const auto sync = source_text.find("::fsync(descriptor_)", validate);
  const auto checked_close = source_text.find("const bool parent_closed = ::close(descriptor_) == 0", sync);
  const auto close_failure = source_text.find("if (!parent_closed)", checked_close);
  const auto descend = source_text.find("descriptor_ = next", close_failure);
  ASSERT_NE(create, std::string::npos);
  ASSERT_NE(mark_for_sync, std::string::npos);
  ASSERT_NE(reopen, std::string::npos);
  ASSERT_NE(validate, std::string::npos);
  ASSERT_NE(sync, std::string::npos);
  ASSERT_NE(checked_close, std::string::npos);
  ASSERT_NE(close_failure, std::string::npos);
  ASSERT_NE(descend, std::string::npos);
  EXPECT_LT(create, mark_for_sync);
  EXPECT_LT(mark_for_sync, reopen);
  EXPECT_LT(reopen, validate);
  EXPECT_LT(validate, sync);
  EXPECT_LT(sync, checked_close);
  EXPECT_LT(checked_close, close_failure);
  EXPECT_LT(close_failure, descend);
}

TEST_F(PrivateStateFileTest, ClosesPostRenameDirectoryBeforeLeavingTheLockScope) {
  std::ifstream source(std::string {POLARIS_SOURCE_DIR} + "/src/private_state_file.cpp");
  ASSERT_TRUE(source.is_open());
  std::ostringstream contents;
  contents << source.rdbuf();
  const auto source_text = contents.str();

  const auto visible_replacement = source_text.find("temporary_exists = false");
  const auto unconditional_close = source_text.find(
    "const bool directory_closed = directory.close()",
    visible_replacement
  );
  const auto sync_failure = source_text.find("if (!directory_synchronized)", unconditional_close);
  const auto sync_failure_return = source_text.find(
    "return {write_status_e::durability_uncertain}",
    sync_failure
  );
  ASSERT_NE(visible_replacement, std::string::npos);
  ASSERT_NE(unconditional_close, std::string::npos);
  ASSERT_NE(sync_failure, std::string::npos);
  ASSERT_NE(sync_failure_return, std::string::npos);
  EXPECT_LT(visible_replacement, unconditional_close);
  EXPECT_LT(unconditional_close, sync_failure);
  EXPECT_LT(sync_failure, sync_failure_return);

  const auto injected_close_failure = source_text.find(
    "injected_fault != write_fault_e::directory_close",
    unconditional_close
  );
  ASSERT_NE(injected_close_failure, std::string::npos);
  EXPECT_LT(unconditional_close, injected_close_failure);
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

#if defined(__linux__)
TEST_F(PrivateStateFileTest, RejectsFifoTargetsWithoutBlocking) {
  ASSERT_EQ(::mkfifo(target.c_str(), S_IRUSR | S_IWUSR), 0);

  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const auto result = private_state_file::read_secure(target, 1024);
    ::_exit(result.status == private_state_file::read_status_e::rejected ? 0 : 1);
  }

  int status = 0;
  bool finished = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {2};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      finished = true;
      break;
    }
    ASSERT_TRUE(waited == 0 || (waited < 0 && errno == EINTR));
    std::this_thread::sleep_for(std::chrono::milliseconds {10});
  }
  if (!finished) {
    (void) ::kill(child, SIGKILL);
    (void) ::waitpid(child, &status, 0);
  }

  ASSERT_TRUE(finished) << "read_secure blocked while opening an owner-controlled FIFO";
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

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

TEST_F(PrivateStateFileTest, RejectsHardLinkedLockFileWithoutChangingReferent) {
  const auto lock_referent = directory / "lock-referent";
  {
    std::ofstream output(lock_referent);
    output << "sentinel";
  }
  std::filesystem::create_hard_link(lock_referent, target.string() + ".lock");

  EXPECT_FALSE(private_state_file::write_atomic(target, "private"));
  std::ifstream original(lock_referent);
  std::string original_payload;
  original >> original_payload;
  EXPECT_EQ(original_payload, "sentinel");
}

TEST_F(PrivateStateFileTest, RejectsHardLinkedStateFile) {
  const auto state_referent = directory / "state-referent";
  {
    std::ofstream output(state_referent);
    output << "private";
  }
  std::filesystem::permissions(
    state_referent,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace
  );
  std::filesystem::create_hard_link(state_referent, target);

  EXPECT_EQ(
    private_state_file::read_secure(target, 1024).status,
    private_state_file::read_status_e::rejected
  );
}

TEST_F(PrivateStateFileTest, RejectsSymlinkedParentDirectory) {
  const auto real_directory = directory / "real";
  const auto linked_directory = directory / "linked";
  std::filesystem::create_directories(real_directory);
  std::filesystem::create_directory_symlink(real_directory, linked_directory);

  EXPECT_FALSE(private_state_file::write_atomic(linked_directory / "state.json", "private"));
  EXPECT_FALSE(std::filesystem::exists(real_directory / "state.json"));
}

TEST_F(PrivateStateFileTest, RejectsGroupWritableParentDirectory) {
  std::filesystem::permissions(
    directory,
    std::filesystem::perms::owner_all | std::filesystem::perms::group_write,
    std::filesystem::perm_options::replace
  );

  EXPECT_FALSE(private_state_file::write_atomic(target, "private"));
  EXPECT_FALSE(std::filesystem::exists(target));
}

TEST_F(PrivateStateFileTest, RejectsSymlinkedAncestorDirectory) {
  const auto real_ancestor = directory / "real-ancestor";
  const auto nested_parent = real_ancestor / "nested";
  const auto real_target = nested_parent / "state.json";
  const auto linked_ancestor = directory / "linked-ancestor";
  std::filesystem::create_directories(nested_parent);
  ASSERT_TRUE(private_state_file::write_atomic(real_target, "trusted"));
  std::filesystem::create_directory_symlink(real_ancestor, linked_ancestor);

  const auto redirected_target = linked_ancestor / "nested" / "state.json";
  const auto redirected_read = private_state_file::read_secure(redirected_target, 1024);
  EXPECT_EQ(redirected_read.status, private_state_file::read_status_e::io_error);
  EXPECT_TRUE(redirected_read.payload.empty());
  EXPECT_FALSE(private_state_file::write_atomic(redirected_target, "redirected"));
  const auto trusted_read = private_state_file::read_secure(real_target, 1024);
  ASSERT_EQ(trusted_read.status, private_state_file::read_status_e::ok);
  EXPECT_EQ(trusted_read.payload, "trusted");
}

TEST_F(PrivateStateFileTest, RejectsWritableNonStickyAncestorDirectory) {
  const auto writable_ancestor = directory / "writable-ancestor";
  const auto nested_parent = writable_ancestor / "nested";
  const auto unsafe_target = nested_parent / "state.json";
  std::filesystem::create_directories(nested_parent);
  {
    std::ofstream planted_state(unsafe_target, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(planted_state.is_open());
    planted_state << "planted";
  }
  std::filesystem::permissions(
    unsafe_target,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace
  );
  std::filesystem::permissions(
    writable_ancestor,
    std::filesystem::perms::all,
    std::filesystem::perm_options::replace
  );

  const auto planted_read = private_state_file::read_secure(unsafe_target, 1024);
  EXPECT_EQ(planted_read.status, private_state_file::read_status_e::io_error);
  EXPECT_TRUE(planted_read.payload.empty());
  EXPECT_FALSE(private_state_file::write_atomic(unsafe_target, "private"));
}

TEST_F(PrivateStateFileTest, RejectsParentTraversalComponents) {
  const auto nested_parent = directory / "nested";
  std::filesystem::create_directory(nested_parent);
  const auto traversing_target = nested_parent / ".." / "escaped.json";

  EXPECT_FALSE(private_state_file::write_atomic(traversing_target, "private"));
  EXPECT_FALSE(std::filesystem::exists(directory / "escaped.json"));
}

TEST_F(PrivateStateFileTest, CreatesMissingParentsWithPrivatePermissions) {
  const auto first_parent = directory / "missing";
  const auto second_parent = first_parent / "nested";
  const auto nested_target = second_parent / "state.json";

  ASSERT_TRUE(private_state_file::write_atomic(nested_target, "private"));
  for (const auto &parent : {first_parent, second_parent}) {
    const auto permissions = std::filesystem::status(parent).permissions();
    EXPECT_EQ(
      permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all),
      std::filesystem::perms::none
    );
  }
  const auto stored = private_state_file::read_secure(nested_target, 1024);
  ASSERT_EQ(stored.status, private_state_file::read_status_e::ok);
  EXPECT_EQ(stored.payload, "private");
}

TEST_F(PrivateStateFileTest, ParentComponentSyncFailureCoversEveryCreatedLevelAndEexistRace) {
  const auto first_parent = directory / "missing";
  const auto nested_target = first_parent / "nested" / "state.json";

  for (std::size_t component = 0; component < 2; ++component) {
    std::filesystem::remove_all(first_parent);
    private_state_file::set_parent_component_fault_index_for_tests(component);
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::parent_sync);
    const auto outcome = private_state_file::write_atomic(nested_target, "private");
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);

    EXPECT_FALSE(outcome) << component;
    EXPECT_EQ(outcome.status, private_state_file::write_status_e::not_committed) << component;
    EXPECT_TRUE(std::filesystem::exists(first_parent)) << component;
    EXPECT_EQ(std::filesystem::exists(first_parent / "nested"), component == 1) << component;
    EXPECT_FALSE(std::filesystem::exists(nested_target)) << component;
  }

  const auto raced_parent = directory / "already-exists";
  std::filesystem::create_directory(raced_parent);
  const auto raced_target = raced_parent / "state.json";
  private_state_file::set_parent_component_fault_index_for_tests(0);
  private_state_file::set_parent_eexist_race_for_tests(true);
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::parent_sync);
  const auto raced_outcome = private_state_file::write_atomic(raced_target, "private");
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  private_state_file::set_parent_eexist_race_for_tests(false);

  EXPECT_FALSE(raced_outcome);
  EXPECT_EQ(raced_outcome.status, private_state_file::write_status_e::not_committed);
  EXPECT_FALSE(std::filesystem::exists(raced_target));
  ASSERT_TRUE(private_state_file::write_atomic(raced_target, "private"));
}

TEST_F(PrivateStateFileTest, ParentComponentCloseFailureAbortsWithoutTargetLeakAndPermitsRetry) {
  const auto first_parent = directory / "missing-close";
  const auto nested_target = first_parent / "nested" / "state.json";
#if defined(__linux__)
  const auto descriptor_count = []() {
    std::size_t count = 0;
    for ([[maybe_unused]] const auto &entry : std::filesystem::directory_iterator("/proc/self/fd")) {
      ++count;
    }
    return count;
  };
  const auto before = descriptor_count();
#endif

  for (std::size_t attempt = 0; attempt < 32; ++attempt) {
    std::filesystem::remove_all(first_parent);
    private_state_file::set_parent_component_fault_index_for_tests(attempt % 2);
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::parent_close);
    const auto outcome = private_state_file::write_atomic(nested_target, "private");
    private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);

    EXPECT_FALSE(outcome) << attempt;
    EXPECT_EQ(outcome.status, private_state_file::write_status_e::not_committed) << attempt;
    EXPECT_TRUE(std::filesystem::exists(first_parent)) << attempt;
    EXPECT_EQ(std::filesystem::exists(first_parent / "nested"), attempt % 2 == 1) << attempt;
    EXPECT_FALSE(std::filesystem::exists(nested_target)) << attempt;
  }
#if defined(__linux__)
  EXPECT_LE(descriptor_count(), before + 1);
#endif

  std::filesystem::remove_all(first_parent);
  ASSERT_TRUE(private_state_file::write_atomic(nested_target, "private"));
  EXPECT_EQ(private_state_file::read_secure(nested_target, 1024).status, private_state_file::read_status_e::ok);
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

TEST_F(PrivateStateFileTest, PostRenameDurabilityFaultFailsClosedAfterVisibleReplacement) {
  ASSERT_TRUE(private_state_file::write_atomic(target, "old"));
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::post_rename_durability);
  const auto outcome = private_state_file::write_atomic(target, "new");
  EXPECT_FALSE(outcome);
  EXPECT_EQ(outcome.status, private_state_file::write_status_e::durability_uncertain);
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  const auto result = private_state_file::read_secure(target, 1024);
  ASSERT_EQ(result.status, private_state_file::read_status_e::ok);
  EXPECT_EQ(result.payload, "new");
}

TEST_F(PrivateStateFileTest, DirectoryCloseFaultFailsClosedAfterVisibleReplacement) {
  ASSERT_TRUE(private_state_file::write_atomic(target, "old"));
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::directory_close);
  const auto outcome = private_state_file::write_atomic(target, "new");
  EXPECT_FALSE(outcome);
  EXPECT_EQ(outcome.status, private_state_file::write_status_e::durability_uncertain);
  private_state_file::set_write_fault_for_tests(private_state_file::write_fault_e::none);
  const auto result = private_state_file::read_secure(target, 1024);
  ASSERT_EQ(result.status, private_state_file::read_status_e::ok);
  EXPECT_EQ(result.payload, "new");
}