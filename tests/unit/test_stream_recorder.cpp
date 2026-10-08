/**
 * @file tests/unit/test_stream_recorder.cpp
 * @brief Test the recorder mode that src/stream_recorder.* publishes from configuration.
 */

#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <atomic>
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
