/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stream {
  void concat_and_insert_into(std::vector<uint8_t> &result, uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
}

namespace nvhttp {
  std::optional<int> select_paired_client_launch_bitrate_for_tests(
    const std::optional<int> &target_bitrate_kbps,
    int paired_bitrate_kbps,
    bool applied_history_safe
  );
}

namespace proc {
  bool should_publish_stream_ended_after_terminate_for_tests(bool had_running_app, int active_sessions, std::string_view session_state);
}

namespace {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2) {
    std::vector<uint8_t> result;
    stream::concat_and_insert_into(result, insert_size, slice_size, data1, data2);
    return result;
  }
}

#include "../tests_common.h"

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(NvhttpOptimizerTests, PairedClientLaunchBitrateOverridesCachedLowerOptimizerTarget) {
  const auto selected = nvhttp::select_paired_client_launch_bitrate_for_tests(
    25000,
    80000,
    false
  );

  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(*selected, 80000);
}

TEST(NvhttpOptimizerTests, PairedClientLaunchBitrateKeepsHistorySafeRecoveryCap) {
  const auto selected = nvhttp::select_paired_client_launch_bitrate_for_tests(
    25000,
    80000,
    true
  );

  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(*selected, 25000);
}

TEST(ProcSessionLifecycleTests, TerminatedPausedAppPublishesStreamEndedWhenNoSessionsRemain) {
  EXPECT_TRUE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "paused"));
}

TEST(ProcSessionLifecycleTests, TerminatedAppDoesNotPublishStreamEndedWhileClientIsConnected) {
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 1, "streaming"));
}

TEST(ProcSessionLifecycleTests, AlreadyIdleTerminateDoesNotPublishDuplicateStreamEnded) {
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "idle"));
}

TEST(ProcSessionLifecycleTests, StreamingTerminateWaitsForStreamJoinToPublishStreamEnded) {
  EXPECT_FALSE(proc::should_publish_stream_ended_after_terminate_for_tests(true, 0, "streaming"));
}
