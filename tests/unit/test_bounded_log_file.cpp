/**
 * @file tests/unit/test_bounded_log_file.cpp
 * @brief Test record-atomic bounded active and backup log storage.
 */

#include <gtest/gtest.h>
#include <src/bounded_log_file.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
  namespace fs = std::filesystem;

  class BoundedLogFileTest: public testing::Test {
  protected:
    void SetUp() override {
      root_ = fs::temp_directory_path() / ("polaris-bounded-log-" + std::to_string(counter_++));
      std::error_code error;
      fs::remove_all(root_, error);
      fs::create_directories(root_);
      active_ = root_ / "polaris.log";
      backup_ = root_ / "polaris.log.backup";
    }

    void TearDown() override {
      std::error_code error;
      fs::remove_all(root_, error);
    }

    static std::string read(const fs::path &path) {
      std::ifstream input {path, std::ios::binary};
      return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
    }

    inline static unsigned counter_ = 0;
    fs::path root_;
    fs::path active_;
    fs::path backup_;
  };
}

TEST_F(BoundedLogFileTest, RotatesBeforeARecordAndKeepsExactlyOneBoundedBackup) {
  logging::bounded_log_file_t file {active_, backup_, 12};

  EXPECT_EQ(file.write_record("one"), logging::bounded_log_write_result_e::written);
  EXPECT_EQ(file.write_record("two"), logging::bounded_log_write_result_e::written);
  EXPECT_EQ(file.write_record("three"), logging::bounded_log_write_result_e::rotated);
  EXPECT_EQ(read(backup_), "one\ntwo\n");
  EXPECT_EQ(read(active_), "three\n");

  EXPECT_EQ(file.write_record("four"), logging::bounded_log_write_result_e::written);
  EXPECT_EQ(file.write_record("five"), logging::bounded_log_write_result_e::rotated);
  EXPECT_EQ(read(backup_), "three\nfour\n");
  EXPECT_EQ(read(active_), "five\n");
  EXPECT_LE(fs::file_size(active_), 12);
  EXPECT_LE(fs::file_size(backup_), 12);
}

TEST_F(BoundedLogFileTest, PublishesRotationBeforeWritingNewGenerationBytes) {
  unsigned rotations = 0;
  std::string active_at_rotation;
  logging::bounded_log_file_t file {
    active_,
    backup_,
    8,
    [&]() {
      ++rotations;
      active_at_rotation = read(active_);
    }
  };

  ASSERT_EQ(file.write_record("one"), logging::bounded_log_write_result_e::written);
  ASSERT_EQ(file.write_record("two"), logging::bounded_log_write_result_e::written);
  ASSERT_EQ(file.write_record("three"), logging::bounded_log_write_result_e::rotated);

  EXPECT_EQ(rotations, 1);
  EXPECT_TRUE(active_at_rotation.empty());
  EXPECT_EQ(read(backup_), "one\ntwo\n");
  EXPECT_EQ(read(active_), "three\n");
}

TEST_F(BoundedLogFileTest, RejectsARecordLargerThanTheActiveFileBound) {
  logging::bounded_log_file_t file {active_, backup_, 4};

  EXPECT_EQ(file.write_record("four"), logging::bounded_log_write_result_e::rejected);
  EXPECT_TRUE(read(active_).empty());
  EXPECT_FALSE(fs::exists(backup_));
}

TEST_F(BoundedLogFileTest, RejectsIdenticalActiveAndBackupPaths) {
  EXPECT_THROW(
    (logging::bounded_log_file_t {active_, active_, 8}),
    std::invalid_argument
  );
  EXPECT_FALSE(logging::bounded_log_file_t::preserve_existing(active_, active_, 8));
  EXPECT_FALSE(logging::bounded_log_file_t::preserve_existing(active_, backup_, 0));
}

TEST_F(BoundedLogFileTest, ClearRemovesTheBackupAndReopensTheActiveFile) {
  logging::bounded_log_file_t file {active_, backup_, 8};
  ASSERT_EQ(file.write_record("one"), logging::bounded_log_write_result_e::written);
  ASSERT_EQ(file.write_record("two"), logging::bounded_log_write_result_e::written);
  ASSERT_EQ(file.write_record("x"), logging::bounded_log_write_result_e::rotated);
  ASSERT_TRUE(fs::exists(backup_));

  ASSERT_TRUE(file.clear());
  EXPECT_FALSE(fs::exists(backup_));
  EXPECT_TRUE(read(active_).empty());
  EXPECT_EQ(file.size(), 0);

  EXPECT_EQ(file.write_record("after"), logging::bounded_log_write_result_e::written);
  EXPECT_EQ(read(active_), "after\n");
}

TEST_F(BoundedLogFileTest, PreservesOnlyTheNewestBytesFromAnOversizedPriorLog) {
  {
    std::ofstream output {active_, std::ios::binary};
    output << "0123456789abcdef";
  }

  ASSERT_TRUE(logging::bounded_log_file_t::preserve_existing(active_, backup_, 8));
  EXPECT_FALSE(fs::exists(active_));
  EXPECT_EQ(read(backup_), "89abcdef");
  EXPECT_EQ(fs::file_size(backup_), 8);
}

TEST_F(BoundedLogFileTest, RenamesAnAlreadyBoundedPriorLogWithoutChangingIt) {
  {
    std::ofstream output {active_, std::ios::binary};
    output << "prior";
  }

  ASSERT_TRUE(logging::bounded_log_file_t::preserve_existing(active_, backup_, 8));
  EXPECT_FALSE(fs::exists(active_));
  EXPECT_EQ(read(backup_), "prior");
}

TEST_F(BoundedLogFileTest, PreservesEmbeddedNulBytesWithoutCorruption) {
  logging::bounded_log_file_t file {active_, backup_, 16};
  const std::string record {"a\0b", 3};

  ASSERT_EQ(file.write_record(record), logging::bounded_log_write_result_e::written);
  EXPECT_EQ(read(active_), std::string("a\0b\n", 4));
  EXPECT_EQ(file.size(), 4);
}

TEST_F(BoundedLogFileTest, RotationStressKeepsOnlyTwoBoundedWholeRecordFiles) {
  constexpr std::uintmax_t max_bytes = 128;
  logging::bounded_log_file_t file {active_, backup_, max_bytes};

  for (unsigned index = 0; index < 5000; ++index) {
    const auto record = "record-" + std::to_string(index);
    ASSERT_NE(file.write_record(record), logging::bounded_log_write_result_e::rejected);
  }

  ASSERT_TRUE(fs::exists(active_));
  ASSERT_TRUE(fs::exists(backup_));
  EXPECT_LE(fs::file_size(active_), max_bytes);
  EXPECT_LE(fs::file_size(backup_), max_bytes);
  EXPECT_EQ(std::distance(fs::directory_iterator(root_), fs::directory_iterator {}), 2);

  for (const auto &path : std::vector<fs::path> {active_, backup_}) {
    const auto contents = read(path);
    ASSERT_FALSE(contents.empty());
    EXPECT_EQ(contents.back(), '\n');

    std::size_t start = 0;
    while (start < contents.size()) {
      const auto end = contents.find('\n', start);
      ASSERT_NE(end, std::string::npos);
      EXPECT_EQ(contents.compare(start, 7, "record-"), 0);
      start = end + 1;
    }
  }
}

TEST_F(BoundedLogFileTest, ClearStressRemovesEveryBackupAndRemainsWritable) {
  logging::bounded_log_file_t file {active_, backup_, 8};

  for (unsigned index = 0; index < 100; ++index) {
    ASSERT_EQ(file.write_record("one"), logging::bounded_log_write_result_e::written);
    ASSERT_EQ(file.write_record("two"), logging::bounded_log_write_result_e::written);
    ASSERT_EQ(file.write_record("three"), logging::bounded_log_write_result_e::rotated);
    ASSERT_TRUE(fs::exists(backup_));

    ASSERT_TRUE(file.clear());
    EXPECT_FALSE(fs::exists(backup_));
    EXPECT_TRUE(read(active_).empty());
    EXPECT_EQ(file.size(), 0);
  }

  EXPECT_EQ(file.write_record("after"), logging::bounded_log_write_result_e::written);
  EXPECT_EQ(read(active_), "after\n");
}

TEST_F(BoundedLogFileTest, OversizedSparsePriorLogProducesOnlyABoundedTailBackup) {
  constexpr std::uintmax_t prior_bytes = 1024U * 1024U;
  constexpr std::uintmax_t max_bytes = 4096;
  const std::string marker = "newest-tail-data";
  {
    std::ofstream output {active_, std::ios::binary};
    output.seekp(static_cast<std::streamoff>(prior_bytes - marker.size()));
    output.write(marker.data(), static_cast<std::streamsize>(marker.size()));
  }
  ASSERT_EQ(fs::file_size(active_), prior_bytes);

  ASSERT_TRUE(logging::bounded_log_file_t::preserve_existing(active_, backup_, max_bytes));
  EXPECT_FALSE(fs::exists(active_));
  ASSERT_EQ(fs::file_size(backup_), max_bytes);
  const auto backup = read(backup_);
  ASSERT_EQ(backup.size(), max_bytes);
  EXPECT_EQ(backup.substr(backup.size() - marker.size()), marker);
}

TEST_F(BoundedLogFileTest, OversizedOrphanedBackupIsAlsoReducedToItsNewestBoundedTail) {
  {
    std::ofstream output {backup_, std::ios::binary};
    output << "0123456789abcdef";
  }

  ASSERT_TRUE(logging::bounded_log_file_t::preserve_existing(active_, backup_, 8));
  EXPECT_FALSE(fs::exists(active_));
  EXPECT_EQ(read(backup_), "89abcdef");
  EXPECT_EQ(fs::file_size(backup_), 8);
}

TEST(BoundedLogOwnerLock, ExcludesSecondAcquireWhileHeldAndReleasesOnDestruction) {
  const auto root = std::filesystem::temp_directory_path() / "polaris-bounded-log-owner-lock";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  const auto lock_path = root / "polaris.log.lock";

  {
    logging::bounded_log_owner_lock_t first {lock_path};
    ASSERT_TRUE(first.owned());

    logging::bounded_log_owner_lock_t second {lock_path};
    EXPECT_FALSE(second.owned());
  }

  logging::bounded_log_owner_lock_t third {lock_path};
  EXPECT_TRUE(third.owned());

  std::filesystem::remove_all(root, error);
}
