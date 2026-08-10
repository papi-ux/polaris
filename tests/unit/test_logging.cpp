/**
 * @file tests/unit/test_logging.cpp
 * @brief Test src/logging.*.
 */
#include "../tests_common.h"
#include "../tests_log_checker.h"
#include "../tests_paths.h"

#include <boost/log/core.hpp>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>
#include <src/logging.h>

namespace {
  std::array log_levels = {
    std::tuple("verbose", &verbose),
    std::tuple("debug", &debug),
    std::tuple("info", &info),
    std::tuple("warning", &warning),
    std::tuple("error", &error),
    std::tuple("fatal", &fatal),
  };

}  // namespace

struct LogLevelsTest: testing::TestWithParam<decltype(log_levels)::value_type> {};

INSTANTIATE_TEST_SUITE_P(
  Logging,
  LogLevelsTest,
  testing::ValuesIn(log_levels),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(LogLevelsTest, PutMessage) {
  auto [label, plogger] = GetParam();
  ASSERT_TRUE(plogger);
  auto &logger = *plogger;

  std::random_device rand_dev;
  std::mt19937_64 rand_gen(rand_dev());
  auto test_message = std::format("{}{}", rand_gen(), rand_gen());
  BOOST_LOG(logger) << test_message;

  ASSERT_TRUE(log_checker::line_contains(test_paths::log_file().string(), test_message));
}

// Records must be stamped at creation. The formatter runs on the asynchronous
// sink's consumer thread; without a creation-time attribute it can only print
// the wall clock at format time, and a stalled consumer then rewrites history
// with flush-time stamps (a whole session's backlog was observed draining in
// one second with every line stamped at the drain moment). init() registers a
// global local_clock as "TimeStamp" — this is the registration proof.
TEST(LoggingTimeStamp, GlobalTimeStampAttributeIsRegistered) {
  ASSERT_EQ(boost::log::core::get()->get_global_attributes().count("TimeStamp"), 1u)
    << "init() must register the TimeStamp global attribute so records carry "
       "their creation time to the async formatter";
}

TEST(LoggingTimeStamp, PrintedTimestampTracksRecordCreation) {
  std::random_device rand_dev;
  std::mt19937_64 rand_gen(rand_dev());
  const auto test_message = std::format("stamp{}{}", rand_gen(), rand_gen());

  const auto before = std::chrono::system_clock::now();
  BOOST_LOG(info) << test_message;
  ASSERT_TRUE(log_checker::line_contains(test_paths::log_file().string(), test_message));
  const auto after = std::chrono::system_clock::now();

  // Find the line and parse its "[YYYY-MM-DD HH:MM:SS.mmm]: " prefix.
  std::ifstream file(test_paths::log_file().string());
  ASSERT_TRUE(file.good());
  std::string line;
  std::string matched;
  while (std::getline(file, line)) {
    if (line.find(test_message) != std::string::npos) {
      matched = line;
    }
  }
  ASSERT_FALSE(matched.empty());

  const std::regex stamp_re(R"(^\[(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{3})\]: )");
  std::smatch parts;
  ASSERT_TRUE(std::regex_search(matched, parts, stamp_re))
    << "log line does not start with a parseable timestamp: " << matched;

  std::tm parsed {};
  parsed.tm_year = std::stoi(parts[1]) - 1900;
  parsed.tm_mon = std::stoi(parts[2]) - 1;
  parsed.tm_mday = std::stoi(parts[3]);
  parsed.tm_hour = std::stoi(parts[4]);
  parsed.tm_min = std::stoi(parts[5]);
  parsed.tm_sec = std::stoi(parts[6]);
  parsed.tm_isdst = -1;  // formatter prints local time
  const auto parsed_time = std::chrono::system_clock::from_time_t(std::mktime(&parsed)) +
                           std::chrono::milliseconds(std::stoi(parts[7]));

  // A generous window: this cannot distinguish record time from flush time
  // when the consumer is healthy (they are milliseconds apart), but it fails
  // loudly on the bugs a broken extraction produces — an epoch-zero stamp, a
  // UTC/local mixup, or garbage. The record-vs-flush discrimination itself is
  // pinned by the source guard below.
  EXPECT_GE(parsed_time, before - std::chrono::seconds(5));
  EXPECT_LE(parsed_time, after + std::chrono::seconds(5));
}

// Source guard: the formatter must print the record's TimeStamp attribute and
// keep the wall clock only as a fallback for records logged before init().
TEST(LoggingTimeStamp, FormatterPrintsTheRecordTimeStampAttribute) {
  const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/logging.cpp";
  std::ifstream file {path};
  ASSERT_TRUE(file.good()) << "could not read src/logging.cpp via POLARIS_SOURCE_DIR";
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const auto source = buffer.str();

  const auto formatter_start = source.find("void formatter(");
  ASSERT_NE(formatter_start, std::string::npos);
  const auto formatter_body = source.substr(formatter_start, source.find("\n  }", formatter_start) - formatter_start);

  EXPECT_NE(formatter_body.find("[\"TimeStamp\"].extract<boost::posix_time::ptime>()"), std::string::npos)
    << "the formatter must extract the record's TimeStamp attribute instead of "
       "stamping records with the consumer thread's wall clock at flush time";
  EXPECT_NE(source.find("add_global_attribute(\"TimeStamp\""), std::string::npos)
    << "init() must register the TimeStamp global attribute";
}

// Source guard: a broken GL context must not be able to flood the log file.
// See the "bound GL info-log length" fix -- an uninitialized GL_INFO_LOG_LENGTH
// turned string.resize() into a multi-megabyte NUL record, and the formatter now
// also caps any single record as a backstop.
TEST(LoggingRunaway, FormatterCapsRecordSize) {
  const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/logging.cpp";
  std::ifstream file {path};
  ASSERT_TRUE(file.good()) << "could not read src/logging.cpp via POLARIS_SOURCE_DIR";
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const auto source = buffer.str();

  const auto formatter_start = source.find("void formatter(");
  ASSERT_NE(formatter_start, std::string::npos);
  const auto formatter_body = source.substr(formatter_start, source.find("\n  }", formatter_start) - formatter_start);

  EXPECT_NE(formatter_body.find("max_message_chars"), std::string::npos)
    << "the formatter must cap the per-record message size so one pathological "
       "message cannot grow the log file without bound";
}

TEST(LoggingRunaway, GlInfoLogLengthIsInitializedAndGuarded) {
  const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/platform/linux/graphics.cpp";
  std::ifstream file {path};
  ASSERT_TRUE(file.good()) << "could not read src/platform/linux/graphics.cpp via POLARIS_SOURCE_DIR";
  std::ostringstream buffer;
  buffer << file.rdbuf();
  const auto source = buffer.str();

  for (const auto *fn : {"shader_t::err_str()", "program_t::err_str()"}) {
    const auto start = source.find(fn);
    ASSERT_NE(start, std::string::npos) << fn << " not found in graphics.cpp";
    const auto body = source.substr(start, source.find("\n  }", start) - start);

    // A failed GetShader/Programiv(GL_INFO_LOG_LENGTH) leaves length untouched;
    // an uninitialized garbage length made string.resize() allocate a
    // multi-megabyte run of NULs that flooded the log.
    EXPECT_NE(body.find("int length = 0"), std::string::npos)
      << fn << " must initialize the info-log length to 0";
    EXPECT_NE(body.find("length <= 0"), std::string::npos)
      << fn << " must return early on a non-positive info-log length";
  }
}
