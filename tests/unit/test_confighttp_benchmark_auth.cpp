/**
 * @file tests/unit/test_confighttp_benchmark_auth.cpp
 * @brief Test src/confighttp_benchmark_auth.*.
 */
#include "../tests_common.h"

#include <src/confighttp_benchmark_auth.h>

using confighttp::benchmark_auth::rate_limiter_t;

TEST(BenchmarkAuthRateLimiterTests, AllowsRequestsUpToTheLimit) {
  rate_limiter_t limiter(3, std::chrono::seconds(10));
  const auto now = std::chrono::steady_clock::now();

  EXPECT_FALSE(limiter.is_rate_limited("127.0.0.1", now));
  limiter.record_request("127.0.0.1", now);
  EXPECT_FALSE(limiter.is_rate_limited("127.0.0.1", now));
  limiter.record_request("127.0.0.1", now);
  EXPECT_FALSE(limiter.is_rate_limited("127.0.0.1", now));
  limiter.record_request("127.0.0.1", now);

  EXPECT_TRUE(limiter.is_rate_limited("127.0.0.1", now))
    << "the third recorded request reached the limit of 3";
}

TEST(BenchmarkAuthRateLimiterTests, TracksEachIpIndependently) {
  rate_limiter_t limiter(1, std::chrono::seconds(10));
  const auto now = std::chrono::steady_clock::now();

  limiter.record_request("127.0.0.1", now);
  EXPECT_TRUE(limiter.is_rate_limited("127.0.0.1", now));
  EXPECT_FALSE(limiter.is_rate_limited("::1", now))
    << "a different IP must not share the first IP's count";
}

TEST(BenchmarkAuthRateLimiterTests, OldRequestsFallOutOfTheWindow) {
  rate_limiter_t limiter(1, std::chrono::seconds(10));
  const auto start = std::chrono::steady_clock::now();

  limiter.record_request("127.0.0.1", start);
  EXPECT_TRUE(limiter.is_rate_limited("127.0.0.1", start + std::chrono::seconds(5)))
    << "still within the 10s window";

  EXPECT_FALSE(limiter.is_rate_limited("127.0.0.1", start + std::chrono::seconds(11)))
    << "the one recorded request is now outside the window";
}

TEST(BenchmarkAuthRateLimiterTests, IsRateLimitedDoesNotItselfCountAsARequest) {
  rate_limiter_t limiter(1, std::chrono::seconds(10));
  const auto now = std::chrono::steady_clock::now();

  EXPECT_FALSE(limiter.is_rate_limited("127.0.0.1", now));
  EXPECT_FALSE(limiter.is_rate_limited("127.0.0.1", now))
    << "calling is_rate_limited() repeatedly without record_request() must never itself trip the limit";
}

TEST(BenchmarkAuthRateLimiterTests, RecordingAfterTheWindowExpiresEvictsStaleEntriesFirst) {
  rate_limiter_t limiter(1, std::chrono::seconds(10));
  const auto start = std::chrono::steady_clock::now();

  limiter.record_request("127.0.0.1", start);
  ASSERT_TRUE(limiter.is_rate_limited("127.0.0.1", start));

  // A second request arrives after the first has aged out of the window -
  // it must be accepted (recorded, not itself limited), not perpetually
  // blocked by a stale entry that record_request() failed to evict.
  const auto later = start + std::chrono::seconds(11);
  EXPECT_FALSE(limiter.is_rate_limited("127.0.0.1", later));
  limiter.record_request("127.0.0.1", later);
  EXPECT_TRUE(limiter.is_rate_limited("127.0.0.1", later))
    << "the new request alone should now be at the limit of 1";
}
