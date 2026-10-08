/**
 * @file tests/unit/test_stream_recorder.cpp
 * @brief Test the recorder mode that src/stream_recorder.* publishes from configuration,
 * and the bounded, session- and codec-isolated replay buffer.
 */

#include <src/config.h>
#include <src/stream_packet_owner.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {
  /**
   * Saves the recording configuration and restores it, and the mode published from it,
   * after each test, so no test leaves the process recorder in a mode it chose.
   */
  class StreamRecorderModeTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved = config::recording;
    }

    void TearDown() override {
      config::recording = saved;
      stream_recorder::load_config();
    }

    static void publish(bool enabled, bool replay_buffer) {
      config::recording.enabled = enabled;
      config::recording.replay_buffer = replay_buffer;
      stream_recorder::load_config();
    }

    config::recording_t saved;
  };

  bool is_known(stream_recorder::mode_t mode) {
    return mode == stream_recorder::mode_t::disabled || mode == stream_recorder::mode_t::continuous || mode == stream_recorder::mode_t::replay_buffer;
  }
}  // namespace

TEST_F(StreamRecorderModeTests, DisabledRecordingIsDisabledWhateverTheReplayFlag) {
  publish(false, true);
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::disabled);
  publish(false, false);
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::disabled);
  // Loading a mode never starts a recording.
  EXPECT_FALSE(stream_recorder::is_recording());
}

TEST_F(StreamRecorderModeTests, PublishesContinuousAndReplayAndFollowsEachReload) {
  publish(true, false);
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::continuous);
  publish(true, true);
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::replay_buffer);
  publish(false, true);
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::disabled);
  publish(true, false);
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::continuous);
  EXPECT_FALSE(stream_recorder::is_recording());
}

TEST_F(StreamRecorderModeTests, ReadersSeeOnlyPublishedModesWhileOneWriterReloads) {
  // One producer owns the configuration and reloads it; readers use only current_mode().
  // A clean run here does not prove the absence of a data race; that needs a race detector.
  constexpr int reloads = 400;
  constexpr int readers = 4;
  std::atomic<int> ready {0};
  std::atomic<bool> go {false};
  std::atomic<bool> done {false};
  std::atomic<int> unknown {0};
  std::atomic<long> reads {0};

  std::vector<std::thread> threads;
  for (int r = 0; r < readers; ++r) {
    threads.emplace_back([&] {
      ready.fetch_add(1);
      while (!go.load()) {
        std::this_thread::yield();
      }
      long local = 0;
      do {
        if (!is_known(stream_recorder::current_mode())) {
          unknown.fetch_add(1);
        }
        ++local;
      } while (!done.load());
      reads.fetch_add(local);
    });
  }

  std::thread producer([&] {
    ready.fetch_add(1);
    while (!go.load()) {
      std::this_thread::yield();
    }
    for (int i = 0; i < reloads; ++i) {
      // disabled, continuous, replay buffer, in turn.
      publish(i % 3 != 0, i % 3 == 2);
    }
    // Leave a known final configuration: replay buffer.
    publish(true, true);
    done.store(true);
  });

  while (ready.load() < readers + 1) {
    std::this_thread::yield();
  }
  go.store(true);
  producer.join();
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(unknown.load(), 0);
  EXPECT_GE(reads.load(), readers);
  // Once the writer has finished, the published mode is its last configuration.
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::replay_buffer);
  EXPECT_FALSE(stream_recorder::is_recording());
}

namespace {
  namespace fs = std::filesystem;
  using steady = std::chrono::steady_clock;
  using namespace std::chrono_literals;

  constexpr std::uint64_t k_first = 101;
  constexpr std::uint64_t k_second = 102;
  constexpr std::uint64_t k_watcher = 103;
  constexpr int k_hevc = 1;
  constexpr int k_av1 = 2;

  /**
   * Drives the real replay buffer with an explicit clock. Each test starts from the process
   * start state (no sessions, default caps, empty run) in replay mode with a one-minute
   * duration, and leaves the recorder and its configuration as they were. Saves go to a
   * not-yet-created `recordings` directory inside a parent this fixture created exclusively;
   * only that owned parent is ever removed, never a path that existed before the test.
   */
  class StreamRecorderReplayBufferTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-replay-"} + info->name() + "-" +
                                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                                       std::to_string(attempt));
        std::error_code ec;
        // create_directory() reports true only when this call made the directory.
        if (fs::create_directory(candidate, ec)) {
          owned_parent = candidate;
        }
      }
      ASSERT_FALSE(owned_parent.empty()) << "no exclusive temporary directory";
      output_dir = owned_parent / "recordings";
      config::recording.output_dir = output_dir.string();
      config::recording.replay_buffer_minutes = 1;
      publish(true, true);
    }

    void TearDown() override {
      stream_recorder::detail::reset_replay_for_tests();
      config::recording = saved;
      stream_recorder::load_config();
      if (!owned_parent.empty()) {
        std::error_code ec;
        fs::remove_all(owned_parent, ec);
      }
    }

    static void publish(bool enabled, bool replay_buffer) {
      config::recording.enabled = enabled;
      config::recording.replay_buffer = replay_buffer;
      stream_recorder::load_config();
    }

    /// One video packet of `size` bytes, each `fill`, from `generation` at `at`.
    static void push(std::uint64_t generation, bool keyframe, steady::time_point at, std::size_t size = 10,
                     int video_format = k_hevc, std::uint8_t fill = 0x5a) {
      const std::vector<std::uint8_t> payload(size, fill);
      stream_recorder::detail::push_packet_at(payload.data(), payload.size(), true, keyframe, 0, generation, video_format, at);
    }

    static stream_recorder::replay_stats_t stats(steady::time_point at) {
      return stream_recorder::detail::replay_stats_at(at);
    }

    const steady::time_point t0 = steady::time_point {} + 1h;
    config::recording_t saved;
    fs::path owned_parent;
    fs::path output_dir;
  };

  /// An address no test may read: refusing before the copy is the only way not to crash.
  const std::uint8_t *unreadable() {
    return reinterpret_cast<const std::uint8_t *>(static_cast<std::uintptr_t>(16));
  }

  struct fake_session_t {
    std::uint64_t session_generation;
  };
}  // namespace

TEST_F(StreamRecorderReplayBufferTests, TheHardCapsAndDurationAreTheFixedDefaults) {
  const auto s = stats(t0);
  EXPECT_EQ(s.max_bytes, std::size_t {128} * 1024 * 1024);
  EXPECT_EQ(s.max_packets, std::size_t {32768});
  EXPECT_EQ(s.max_bytes, stream_recorder::k_replay_max_payload_bytes);
  EXPECT_EQ(s.max_packets, stream_recorder::k_replay_max_packets);
  EXPECT_EQ(s.duration, 1min);
  EXPECT_EQ(s.selected_generation, 0u);
  EXPECT_EQ(s.registered_sessions, 0u);
  EXPECT_EQ(s.run_video_format, -1);
}

TEST_F(StreamRecorderReplayBufferTests, PacketsBeforeAnExplicitSessionAreRefusedAndARunStartsAtAKeyframe) {
  push(k_first, true, t0);
  EXPECT_EQ(stats(t0).packets, 0u);
  EXPECT_EQ(stats(t0).refused_unselected, 1u);
  // A packet never selects its own session.
  EXPECT_EQ(stats(t0).selected_generation, 0u);

  stream_recorder::session_started(k_first, true);
  EXPECT_EQ(stats(t0).selected_generation, k_first);
  push(k_first, false, t0);
  EXPECT_EQ(stats(t0).packets, 0u);
  EXPECT_EQ(stats(t0).refused_awaiting_keyframe, 1u);

  push(k_first, true, t0, 12);
  push(k_first, false, t0 + 1s, 8);
  const auto s = stats(t0 + 1s);
  EXPECT_EQ(s.packets, 2u);
  EXPECT_EQ(s.bytes, 20u);
  EXPECT_EQ(s.run_video_format, k_hevc);
}

TEST_F(StreamRecorderReplayBufferTests, ASecondSessionClearsAndRefusesUntilTheSoleSessionSendsAKeyframe) {
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0);
  push(k_first, false, t0);
  ASSERT_EQ(stats(t0).packets, 2u);

  stream_recorder::session_started(k_second, true);
  auto s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.selected_generation, 0u);
  EXPECT_EQ(s.registered_sessions, 2u);
  EXPECT_EQ(s.run_video_format, -1);
  push(k_first, true, t0);
  push(k_second, true, t0);
  EXPECT_EQ(stats(t0).packets, 0u);
  EXPECT_EQ(stats(t0).refused_unselected, 2u);

  // Back to one eligible session: it is selected again, but must restart at a keyframe.
  stream_recorder::session_ended(k_second);
  EXPECT_EQ(stats(t0).selected_generation, k_first);
  push(k_first, false, t0);
  EXPECT_EQ(stats(t0).packets, 0u);
  push(k_first, true, t0);
  EXPECT_EQ(stats(t0).packets, 1u);

  // A watcher or input-only session counts as a second session too.
  stream_recorder::session_started(k_watcher, false);
  EXPECT_EQ(stats(t0).packets, 0u);
  EXPECT_EQ(stats(t0).selected_generation, 0u);
  push(k_first, true, t0);
  EXPECT_EQ(stats(t0).packets, 0u);
  stream_recorder::session_ended(k_watcher);
  push(k_first, false, t0);
  EXPECT_EQ(stats(t0).packets, 0u);
  push(k_first, true, t0);
  EXPECT_EQ(stats(t0).packets, 1u);

  // The eligible session leaving a sole watcher selects nobody.
  stream_recorder::session_started(k_watcher, false);
  stream_recorder::session_ended(k_first);
  s = stats(t0);
  EXPECT_EQ(s.selected_generation, 0u);
  EXPECT_EQ(s.registered_sessions, 1u);
  EXPECT_EQ(s.packets, 0u);
}

TEST_F(StreamRecorderReplayBufferTests, ASoleIneligibleSessionIsRefusedAndAnEligibilityChangeIsATransition) {
  stream_recorder::session_started(k_watcher, false);
  push(k_watcher, true, t0);
  auto s = stats(t0);
  EXPECT_EQ(s.selected_generation, 0u);
  EXPECT_EQ(s.registered_sessions, 1u);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.refused_unselected, 1u);

  stream_recorder::session_started(k_watcher, true);
  EXPECT_EQ(stats(t0).selected_generation, k_watcher);
  push(k_watcher, true, t0);
  ASSERT_EQ(stats(t0).packets, 1u);
  stream_recorder::session_started(k_watcher, false);
  EXPECT_EQ(stats(t0).packets, 0u);
  EXPECT_EQ(stats(t0).selected_generation, 0u);
}

TEST_F(StreamRecorderReplayBufferTests, RepeatedUnknownAndZeroLifecycleCallsChangeNothing) {
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0);
  push(k_first, false, t0);
  stream_recorder::session_started(k_first, true);
  stream_recorder::session_ended(999);
  stream_recorder::session_ended(0);
  auto s = stats(t0);
  EXPECT_EQ(s.packets, 2u);
  EXPECT_EQ(s.selected_generation, k_first);

  stream_recorder::session_started(0, true);
  s = stats(t0);
  EXPECT_EQ(s.refused_lifecycle, 1u);
  EXPECT_EQ(s.registered_sessions, 1u);
  EXPECT_EQ(s.packets, 2u);

  stream_recorder::session_ended(k_first);
  stream_recorder::session_ended(k_first);
  s = stats(t0);
  EXPECT_EQ(s.registered_sessions, 0u);
  EXPECT_EQ(s.selected_generation, 0u);
  EXPECT_EQ(s.packets, 0u);
}

TEST_F(StreamRecorderReplayBufferTests, ZeroForeignAndRetiredGenerationsCannotTouchTheSelectedRun) {
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0);
  push(k_first, false, t0);
  push(0, true, t0);
  push(77, true, t0);
  // Malformed packets of another session are refused as foreign, not as an owner gap.
  stream_recorder::detail::push_packet_at(nullptr, 10, true, true, 0, 77, k_hevc, t0);
  stream_recorder::detail::push_packet_at(unreadable(), std::numeric_limits<std::size_t>::max(), true, true, 0, 0, k_hevc, t0);
  push(77, true, t0, 10, 9);
  push(77, true, t0 - 30s);
  auto s = stats(t0);
  EXPECT_EQ(s.packets, 2u);
  EXPECT_EQ(s.refused_unselected, 6u);
  EXPECT_EQ(s.discontinuities, 0u);
  EXPECT_EQ(s.refused_invalid + s.refused_oversized, 0u);

  // A retired generation cannot select itself or reach the next session's run.
  stream_recorder::session_ended(k_first);
  push(k_first, true, t0);
  EXPECT_EQ(stats(t0).selected_generation, 0u);
  stream_recorder::session_started(k_second, true);
  push(k_first, true, t0);
  push(k_second, true, t0);
  push(k_first, false, t0);
  s = stats(t0);
  EXPECT_EQ(s.selected_generation, k_second);
  EXPECT_EQ(s.packets, 1u);
  EXPECT_EQ(s.refused_unselected, 9u);
}

TEST_F(StreamRecorderReplayBufferTests, ACodecChangeClearsAndRestartsAtAKeyframe) {
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0, 10, k_hevc);
  push(k_first, false, t0, 10, k_hevc);
  push(k_first, false, t0, 10, k_av1);
  auto s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.discontinuities, 1u);
  EXPECT_EQ(s.refused_awaiting_keyframe, 1u);

  push(k_first, true, t0, 10, k_av1);
  push(k_first, false, t0, 10, k_av1);
  s = stats(t0);
  EXPECT_EQ(s.packets, 2u);
  EXPECT_EQ(s.run_video_format, k_av1);
  push(k_first, false, t0, 10, k_hevc);
  EXPECT_EQ(stats(t0).packets, 0u);
}

TEST_F(StreamRecorderReplayBufferTests, DisableAndDurationReloadsClearButKeepTheAdmittedSession) {
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0);
  push(k_first, false, t0);

  // Reloading the same replay configuration keeps the run.
  publish(true, true);
  EXPECT_EQ(stats(t0).packets, 2u);

  publish(false, true);
  auto s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.run_video_format, -1);
  EXPECT_EQ(s.selected_generation, k_first);
  EXPECT_EQ(s.registered_sessions, 1u);

  // Enabled again during the same admitted stream: no new registration, but a new keyframe.
  publish(true, true);
  push(k_first, false, t0);
  EXPECT_EQ(stats(t0).packets, 0u);
  push(k_first, true, t0);
  push(k_first, false, t0);
  ASSERT_EQ(stats(t0).packets, 2u);

  config::recording.replay_buffer_minutes = 2;
  stream_recorder::load_config();
  s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.duration, 2min);

  push(k_first, true, t0);
  publish(true, false);
  EXPECT_EQ(stats(t0).packets, 0u);
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::continuous);
}

TEST_F(StreamRecorderReplayBufferTests, ThePayloadCapTrimsWholeLeadingGroupsOfPictures) {
  stream_recorder::detail::set_replay_limits_for_tests(100, 1000);
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0, 40);
  push(k_first, false, t0, 30);
  push(k_first, false, t0, 30);
  auto s = stats(t0);
  EXPECT_EQ(s.bytes, 100u);  // exactly at the cap
  EXPECT_EQ(s.trimmed, 0u);

  // Room for the next keyframe takes the oldest keyframe and then its dependent frames.
  push(k_first, true, t0, 30);
  s = stats(t0);
  EXPECT_EQ(s.packets, 1u);
  EXPECT_EQ(s.bytes, 30u);
  EXPECT_EQ(s.trimmed, 3u);

  push(k_first, false, t0, 30);
  push(k_first, false, t0, 30);
  ASSERT_EQ(stats(t0).bytes, 90u);
  // A group larger than the cap cannot keep its dependent frames without its keyframe.
  push(k_first, false, t0, 30);
  s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.bytes, 0u);
  EXPECT_EQ(s.refused_awaiting_keyframe, 1u);
  push(k_first, true, t0, 30);
  EXPECT_EQ(stats(t0).packets, 1u);
}

TEST_F(StreamRecorderReplayBufferTests, ThePacketCapTrimsWholeLeadingGroupsOfPictures) {
  stream_recorder::detail::set_replay_limits_for_tests(1000, 4);
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0);
  push(k_first, false, t0);
  push(k_first, false, t0);
  push(k_first, true, t0);
  ASSERT_EQ(stats(t0).packets, 4u);
  // The oldest keyframe goes, then its dependent frames, leaving the newest group and this frame.
  push(k_first, false, t0);
  const auto s = stats(t0);
  EXPECT_EQ(s.packets, 2u);
  EXPECT_EQ(s.trimmed, 3u);
  EXPECT_LE(s.packets, s.max_packets);
}

TEST_F(StreamRecorderReplayBufferTests, OldPacketsExpireThroughTheNextKeyframeOnReadAndOnAppend) {
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0);
  push(k_first, false, t0 + 10s);
  push(k_first, true, t0 + 30s);
  push(k_first, false, t0 + 50s);

  EXPECT_EQ(stats(t0 + 59s).packets, 4u);
  // The first keyframe is past one minute, so its still-recent dependent frame goes with it,
  // and the read itself releases both.
  auto s = stats(t0 + 65s);
  EXPECT_EQ(s.packets, 2u);
  EXPECT_EQ(s.bytes, 20u);
  EXPECT_EQ(s.trimmed, 2u);
  // Released packets do not reappear for a read stamped earlier.
  EXPECT_EQ(stats(t0 + 59s).packets, 2u);

  push(k_first, false, t0 + 66s);
  s = stats(t0 + 66s);
  EXPECT_EQ(s.packets, 3u);
  EXPECT_EQ(s.trimmed, 2u);

  // Idle past the duration: nothing is fresh, and a dependent frame cannot restart the run.
  EXPECT_EQ(stats(t0 + 200s).packets, 0u);
  push(k_first, false, t0 + 200s);
  s = stats(t0 + 200s);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.refused_awaiting_keyframe, 1u);
  push(k_first, true, t0 + 201s);
  EXPECT_EQ(stats(t0 + 201s).packets, 1u);
}

TEST_F(StreamRecorderReplayBufferTests, AnOversizedOwnerFrameIsRefusedBeforeItIsReadAndBreaksTheRun) {
  stream_recorder::detail::set_replay_limits_for_tests(100, 1000);
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0);
  push(k_first, false, t0);

  // Neither size is ever read from: the pointer cannot be dereferenced.
  stream_recorder::detail::push_packet_at(unreadable(), 101, true, false, 0, k_first, k_hevc, t0);
  auto s = stats(t0);
  EXPECT_EQ(s.refused_oversized, 1u);
  EXPECT_EQ(s.discontinuities, 1u);
  EXPECT_EQ(s.packets, 0u);
  // The next dependent frame would follow a missing one, so it waits for a keyframe.
  push(k_first, false, t0);
  EXPECT_EQ(stats(t0).packets, 0u);
  push(k_first, true, t0);
  ASSERT_EQ(stats(t0).packets, 1u);

  stream_recorder::detail::push_packet_at(unreadable(), std::numeric_limits<std::size_t>::max(), true, true, 0, k_first, k_hevc, t0);
  s = stats(t0);
  EXPECT_EQ(s.refused_oversized, 2u);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.bytes, 0u);
}

TEST_F(StreamRecorderReplayBufferTests, AZeroByteOrPacketBudgetRefusesEveryFrameBeforeItIsRead) {
  stream_recorder::session_started(k_first, true);
  stream_recorder::detail::set_replay_limits_for_tests(0, 4);
  stream_recorder::detail::push_packet_at(unreadable(), 1, true, true, 0, k_first, k_hevc, t0);
  auto s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.refused_oversized, 1u);

  stream_recorder::detail::set_replay_limits_for_tests(100, 0);
  stream_recorder::detail::push_packet_at(unreadable(), 1, true, true, 0, k_first, k_hevc, t0);
  push(k_first, true, t0);
  s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.bytes, 0u);
  EXPECT_EQ(s.refused_oversized, 3u);
}

TEST_F(StreamRecorderReplayBufferTests, AnInvalidOwnerFrameBreaksTheRunUntilTheNextKeyframe) {
  stream_recorder::session_started(k_first, true);
  const std::vector<std::uint8_t> payload(10, 1);
  struct bad_t {
    const std::uint8_t *data;
    std::size_t size;
    int video_format;
  };

  const bad_t bad[] {
    {nullptr, 10, k_hevc},
    {payload.data(), 0, k_hevc},
    {payload.data(), payload.size(), -1},
    {payload.data(), payload.size(), 4},
  };
  std::uint64_t refused = 0;
  for (const auto &packet : bad) {
    push(k_first, true, t0);
    push(k_first, false, t0);
    ASSERT_EQ(stats(t0).packets, 2u);
    stream_recorder::detail::push_packet_at(packet.data, packet.size, true, false, 0, k_first, packet.video_format, t0);
    ++refused;
    const auto s = stats(t0);
    EXPECT_EQ(s.refused_invalid, refused);
    EXPECT_EQ(s.discontinuities, refused);
    EXPECT_EQ(s.packets, 0u);
    push(k_first, false, t0);
    EXPECT_EQ(stats(t0).packets, 0u);
  }
}

TEST_F(StreamRecorderReplayBufferTests, ATimeRegressionBreaksTheRun) {
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0 + 10s);
  push(k_first, false, t0 + 10s);  // equal times are not a regression
  ASSERT_EQ(stats(t0 + 10s).packets, 2u);
  push(k_first, false, t0 + 5s);
  auto s = stats(t0 + 10s);
  EXPECT_EQ(s.discontinuities, 1u);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.refused_awaiting_keyframe, 1u);
  push(k_first, true, t0 + 5s);
  EXPECT_EQ(stats(t0 + 5s).packets, 1u);
}

TEST_F(StreamRecorderReplayBufferTests, AStaleIdleBufferIsNeitherCountedNorSavedAndNoFileIsMade) {
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0);
  push(k_first, false, t0 + 1s);
  EXPECT_EQ(stats(t0 + 59s).packets, 2u);
  EXPECT_EQ(stats(t0 + 62s).packets, 0u);

  EXPECT_EQ(stream_recorder::detail::save_replay_at(t0 + 62s), "");
  EXPECT_FALSE(fs::exists(output_dir));
  EXPECT_EQ(stats(t0 + 62s).packets, 0u);
}

TEST_F(StreamRecorderReplayBufferTests, ASaveWritesTheFreshRunWithItsOwnCodecExtension) {
  stream_recorder::session_started(k_first, true);
  push(k_first, true, t0, 4, k_hevc, 'A');
  push(k_first, false, t0 + 1s, 3, k_hevc, 'B');
  // Another codec published elsewhere does not rename this run.
  stream_recorder::set_active_video_format(k_av1);

  const auto file = stream_recorder::detail::save_replay_at(t0 + 2s);
  ASSERT_FALSE(file.empty());
  const fs::path path {file};
  EXPECT_EQ(path.parent_path(), output_dir);
  const std::string suffix = "_replay.hevc";
  ASSERT_GE(file.size(), suffix.size());
  EXPECT_EQ(file.substr(file.size() - suffix.size()), suffix);
  std::ifstream in(path, std::ios::binary);
  const std::string written {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  EXPECT_EQ(written, "AAAABBB");
}

TEST_F(StreamRecorderReplayBufferTests, DisabledRecordingKeepsThePacketFastPath) {
  stream_recorder::session_started(k_first, true);
  publish(false, true);
  push(k_first, true, t0);
  push(77, true, t0);
  stream_recorder::detail::push_packet_at(unreadable(), std::numeric_limits<std::size_t>::max(), true, true, 0, k_first, k_hevc, t0);
  auto s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.refused_unselected + s.refused_invalid + s.refused_oversized + s.refused_awaiting_keyframe, 0u);
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::disabled);
  EXPECT_FALSE(stream_recorder::is_recording());
  EXPECT_EQ(stream_recorder::detail::save_replay_at(t0), "");
}

TEST_F(StreamRecorderReplayBufferTests, ProducersAndAReaderSeeOnlyTheSelectedSessionsBoundedRun) {
  // The configuration is set once before the threads start; nothing writes it while they run.
  // A clean run exercises the lock; it does not prove the absence of a data race.
  constexpr std::size_t packet_size = 8;
  constexpr std::size_t max_packets = 64;
  constexpr int owner_packets = 4000;
  constexpr int foreign_packets = 1000;
  stream_recorder::detail::set_replay_limits_for_tests(1024 * 1024, max_packets);
  stream_recorder::session_started(k_first, true);

  std::atomic<int> ready {0};
  std::atomic<bool> go {false};
  std::atomic<bool> done {false};
  std::atomic<int> bad_snapshots {0};
  std::atomic<long> reads {0};
  const auto wait_for_go = [&] {
    ready.fetch_add(1);
    while (!go.load()) {
      std::this_thread::yield();
    }
  };

  std::thread owner([&] {
    wait_for_go();
    for (int i = 0; i < owner_packets; ++i) {
      push(k_first, i % 16 == 0, t0, packet_size);
    }
  });
  std::thread foreign([&] {
    wait_for_go();
    for (int i = 0; i < foreign_packets; ++i) {
      push(k_second, i % 16 == 0, t0, packet_size + 1, k_av1);
    }
  });
  std::thread reader([&] {
    wait_for_go();
    long local = 0;
    do {
      const auto s = stats(t0);
      if (s.selected_generation != k_first || s.bytes != s.packets * packet_size || s.packets > max_packets ||
          (s.run_video_format != -1 && s.run_video_format != k_hevc)) {
        bad_snapshots.fetch_add(1);
      }
      ++local;
    } while (!done.load());
    reads.store(local);
  });

  while (ready.load() < 3) {
    std::this_thread::yield();
  }
  go.store(true);
  owner.join();
  foreign.join();
  done.store(true);
  reader.join();

  const auto s = stats(t0);
  EXPECT_EQ(bad_snapshots.load(), 0);
  EXPECT_GE(reads.load(), 1);
  EXPECT_EQ(s.refused_unselected, static_cast<std::uint64_t>(foreign_packets));
  EXPECT_EQ(s.packets + s.trimmed, static_cast<std::uint64_t>(owner_packets));
  EXPECT_GT(s.packets, 0u);
  EXPECT_LE(s.packets, max_packets);
  EXPECT_EQ(s.discontinuities, 0u);
}

TEST_F(StreamRecorderReplayBufferTests, ASessionEndedAfterItsPacketOwnerDrainsLeavesNothingBehind) {
  // Root's teardown order: close_and_wait() on the session's packet owner, then session_ended().
  fake_session_t session {k_first};
  stream_packets::owner_t owner {&session};
  const auto destination = owner.destination();
  stream_recorder::session_started(session.session_generation, true);

  std::atomic<bool> acquired {false};
  std::atomic<bool> closed {false};
  std::atomic<bool> sent_inside_delivery {false};
  std::atomic<std::size_t> packets_while_admitted {0};

  std::thread sender([&] {
    auto delivery = destination.acquire();
    if (!delivery) {
      acquired.store(true);
      return;
    }
    acquired.store(true);
    // Teardown has begun, but this admitted send still owns the session.
    while (!delivery.cancelled()) {
      std::this_thread::yield();
    }
    const auto *admitted = static_cast<const fake_session_t *>(delivery.get());
    sent_inside_delivery.store(!closed.load());
    push(admitted->session_generation, true, t0);
    packets_while_admitted.store(stats(t0).packets);
    delivery.reset();
  });
  while (!acquired.load()) {
    std::this_thread::yield();
  }
  std::thread teardown([&] {
    owner.close_and_wait();
    closed.store(true);
    stream_recorder::session_ended(session.session_generation);
  });
  sender.join();
  teardown.join();

  EXPECT_TRUE(sent_inside_delivery.load());
  EXPECT_EQ(packets_while_admitted.load(), 1u);
  EXPECT_FALSE(destination.acquire());
  const auto s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.selected_generation, 0u);
  EXPECT_EQ(s.registered_sessions, 0u);
  // A late packet of the retired generation cannot bring its run back.
  push(k_first, true, t0);
  EXPECT_EQ(stats(t0).packets, 0u);
  EXPECT_EQ(stats(t0).refused_unselected, 1u);
}
