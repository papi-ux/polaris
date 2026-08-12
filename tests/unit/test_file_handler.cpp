/**
 * @file tests/unit/test_file_handler.cpp
 * @brief Test src/file_handler.*.
 */
#include "../tests_common.h"
#include "../tests_paths.h"

#include <format>
#include <src/file_handler.h>

struct FileHandlerParentDirectoryTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(FileHandlerParentDirectoryTest, Run) {
  auto [input, expected] = GetParam();
  EXPECT_EQ(file_handler::get_parent_directory(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerParentDirectoryTest,
  testing::Values(
    std::make_tuple("/path/to/file.txt", "/path/to"),
    std::make_tuple("/path/to/directory", "/path/to"),
    std::make_tuple("/path/to/directory/", "/path/to")
  )
);

struct FileHandlerMakeDirectoryTest: testing::TestWithParam<std::tuple<std::string, bool, bool>> {};

TEST_P(FileHandlerMakeDirectoryTest, Run) {
  auto [input, expected, remove] = GetParam();
  const auto test_root = std::filesystem::temp_directory_path() / "polaris-test-file-handler";
  const auto test_dir = test_root / "path";
  input = (test_dir / input).string();

  EXPECT_EQ(file_handler::make_directory(input), expected);
  EXPECT_TRUE(std::filesystem::exists(input));

  // remove test directory
  if (remove) {
    std::filesystem::remove_all(test_root);
    EXPECT_FALSE(std::filesystem::exists(test_root));
  }
}

INSTANTIATE_TEST_SUITE_P(
  FileHandlerTests,
  FileHandlerMakeDirectoryTest,
  testing::Values(
    std::make_tuple("dir_123", true, false),
    std::make_tuple("dir_123", true, true),
    std::make_tuple("dir_123/abc", true, false),
    std::make_tuple("dir_123/abc", true, true)
  )
);

struct FileHandlerTests: testing::TestWithParam<std::tuple<int, std::string>> {};

INSTANTIATE_TEST_SUITE_P(
  TestFiles,
  FileHandlerTests,
  testing::Values(
    std::make_tuple(0, ""),  // empty file
    std::make_tuple(1, "a"),  // single character
    std::make_tuple(2, "Mr. Blue Sky - Electric Light Orchestra"),  // single line
    std::make_tuple(3, R"(
Morning! Today's forecast calls for blue skies
The sun is shining in the sky
There ain't a cloud in sight
It's stopped raining
Everybody's in the play
And don't you know, it's a beautiful new day
Hey, hey, hey!
Running down the avenue
See how the sun shines brightly in the city
All the streets where once was pity
Mr. Blue Sky is living here today!
Hey, hey, hey!
    )")  // multi-line
  )
);

TEST_P(FileHandlerTests, WriteFileTest) {
  auto [fileNum, content] = GetParam();
  const std::string fileName = test_paths::write_file(fileNum).string();
  EXPECT_EQ(file_handler::write_file(fileName.c_str(), content), 0);
}

TEST_P(FileHandlerTests, ReadFileTest) {
  auto [fileNum, content] = GetParam();
  const std::string fileName = test_paths::write_file(fileNum).string();
  ASSERT_EQ(file_handler::write_file(fileName.c_str(), content), 0);
  EXPECT_EQ(file_handler::read_file(fileName.c_str()), content);
}

TEST(FileHandlerTests, ReadMissingFileTest) {
  // read missing file
  EXPECT_EQ(file_handler::read_file("non-existing-file.txt"), "");
}

namespace {
  std::filesystem::path tail_test_file(std::string_view name) {
    return test_paths::root() / std::format("tail_{}.bin", name);
  }
}  // namespace

TEST(FileHandlerTailTests, ReadsOnlyRequestedTailAndReportsOffsets) {
  const auto path = tail_test_file("lines");
  ASSERT_EQ(file_handler::write_file(path.string().c_str(), "line-1\nline-2\nline-3\n"), 0);

  const auto result = file_handler::read_file_tail(path.string().c_str(), 14);

  EXPECT_EQ(result.content, "line-2\nline-3\n");
  EXPECT_EQ(result.start_offset, 7);
  EXPECT_EQ(result.end_offset, 21);
  EXPECT_TRUE(result.truncated);
}

TEST(FileHandlerTailTests, MissingFileReturnsEmptyNonTruncatedResult) {
  const auto result = file_handler::read_file_tail("missing-tail-file.log", 1024);

  EXPECT_TRUE(result.content.empty());
  EXPECT_EQ(result.start_offset, 0);
  EXPECT_EQ(result.end_offset, 0);
  EXPECT_FALSE(result.truncated);
}

TEST(FileHandlerTailTests, EmptyFileReturnsEmptyNonTruncatedResult) {
  const auto path = tail_test_file("empty");
  ASSERT_EQ(file_handler::write_file(path.string().c_str(), ""), 0);

  const auto result = file_handler::read_file_tail(path.string().c_str(), 1024);

  EXPECT_TRUE(result.content.empty());
  EXPECT_EQ(result.start_offset, 0);
  EXPECT_EQ(result.end_offset, 0);
  EXPECT_FALSE(result.truncated);
}

TEST(FileHandlerTailTests, ExactBoundaryReadsWholeFileWithoutTruncation) {
  const auto path = tail_test_file("exact");
  const std::string content = "exactly-14-byt";
  ASSERT_EQ(content.size(), 14);
  ASSERT_EQ(file_handler::write_file(path.string().c_str(), content), 0);

  const auto result = file_handler::read_file_tail(path.string().c_str(), content.size());

  EXPECT_EQ(result.content, content);
  EXPECT_EQ(result.start_offset, 0);
  EXPECT_EQ(result.end_offset, content.size());
  EXPECT_FALSE(result.truncated);
}

TEST(FileHandlerTailTests, TruncatesAtByteBoundaryWhenNoNewlineIsPresent) {
  const auto path = tail_test_file("no-newline");
  ASSERT_EQ(file_handler::write_file(path.string().c_str(), "0123456789"), 0);

  const auto result = file_handler::read_file_tail(path.string().c_str(), 4);

  EXPECT_EQ(result.content, "6789");
  EXPECT_EQ(result.start_offset, 6);
  EXPECT_EQ(result.end_offset, 10);
  EXPECT_TRUE(result.truncated);
}

TEST(FileHandlerTailTests, EmbeddedNulBytesArePreserved) {
  const auto path = tail_test_file("nul");
  const std::string content {"abc\0def", 7};
  ASSERT_EQ(file_handler::write_file(path.string().c_str(), content), 0);

  const auto result = file_handler::read_file_tail(path.string().c_str(), 5);

  EXPECT_EQ(result.content, std::string("c\0def", 5));
  EXPECT_EQ(result.start_offset, 2);
  EXPECT_EQ(result.end_offset, 7);
  EXPECT_TRUE(result.truncated);
}

TEST(FileHandlerTailTests, MaximumSizeZeroIsRejected) {
  EXPECT_THROW(
    (void) file_handler::read_file_tail("any-file.log", 0),
    std::invalid_argument
  );
}

TEST(FileHandlerTailTests, LargeFileStillReturnsAtMostTheRequestedBytes) {
  const auto path = tail_test_file("large");
  const std::string content(1024 * 1024, 'x');
  ASSERT_EQ(file_handler::write_file(path.string().c_str(), content), 0);

  const auto result = file_handler::read_file_tail(path.string().c_str(), 4096);

  EXPECT_EQ(result.content.size(), 4096);
  EXPECT_EQ(result.start_offset, content.size() - 4096);
  EXPECT_EQ(result.end_offset, content.size());
  EXPECT_TRUE(result.truncated);
}
