/**
 * @file tests/unit/test_video_frame_pacing.cpp
 * @brief Tests for encoder frame timestamp pacing.
 */
#include "../tests_common.h"

#include <src/video_frame_pacing.h>

namespace {
  std::size_t count_encoded_frames(
    std::chrono::nanoseconds source_interval,
    std::chrono::nanoseconds target_interval,
    std::size_t source_frame_count
  ) {
    video::frame_timestamp_pacer_t pacer {target_interval, target_interval / 4};
    std::size_t encoded_frames = 0;
    auto source_timestamp = std::chrono::steady_clock::time_point {};
    std::optional<std::chrono::steady_clock::time_point> last_encode_timestamp;

    for (std::size_t i = 0; i < source_frame_count; ++i) {
      const auto decision = pacer.evaluate(source_timestamp);
      if (decision.should_encode) {
        if (last_encode_timestamp) {
          EXPECT_GT(decision.timestamp, *last_encode_timestamp);
        }
        last_encode_timestamp = decision.timestamp;
        encoded_frames++;
      }
      source_timestamp += source_interval;
    }

    return encoded_frames;
  }
}  // namespace

TEST(VideoFrameTimestampPacingTests, ResamplesSlightlyFastSourceWithoutPhaseResetLoss) {
  constexpr auto target_interval = std::chrono::nanoseconds {8'333'333};
  constexpr auto measured_source_interval = std::chrono::nanoseconds {8'119'500};
  constexpr std::size_t source_frames = 12'000;

  const auto encoded_frames = count_encoded_frames(
    measured_source_interval,
    target_interval,
    source_frames
  );
  const auto expected_frames = static_cast<std::size_t>(
    (measured_source_interval.count() * (source_frames - 1)) / target_interval.count() + 1
  );

  EXPECT_LE(
    std::max(encoded_frames, expected_frames) - std::min(encoded_frames, expected_frames),
    1
  );
}

TEST(VideoFrameTimestampPacingTests, PassesEveryFrameFromSourceBelowTargetRate) {
  EXPECT_EQ(
    count_encoded_frames(
      std::chrono::nanoseconds {16'666'667},
      std::chrono::nanoseconds {8'333'333},
      600
    ),
    600
  );
}

TEST(VideoFrameTimestampPacingTests, DownsamplesSourceAtTwiceTargetRate) {
  EXPECT_EQ(
    count_encoded_frames(
      std::chrono::nanoseconds {4'166'667},
      std::chrono::nanoseconds {8'333'333},
      1'200
    ),
    600
  );
}

TEST(VideoFrameTimestampPacingTests, PreservesFractionalPhaseAfterLongGap) {
  constexpr auto target_interval = std::chrono::nanoseconds {8'333'333};
  video::frame_timestamp_pacer_t pacer {target_interval, target_interval / 4};
  const auto epoch = std::chrono::steady_clock::time_point {};

  EXPECT_TRUE(pacer.evaluate(epoch).should_encode);
  EXPECT_TRUE(pacer.evaluate(epoch + std::chrono::nanoseconds {8'119'500}).should_encode);
  EXPECT_TRUE(pacer.evaluate(epoch + std::chrono::milliseconds {100}).should_encode);
  EXPECT_TRUE(pacer.evaluate(epoch + std::chrono::nanoseconds {108'119'500}).should_encode);
}
