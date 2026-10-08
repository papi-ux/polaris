/**
 * @file tests/unit/test_stream_recorder_window.cpp
 * @brief Test the replay buffer's window accounting in src/stream_recorder.*: the buffered
 * keyframe-start run by arrival time, the last accepted keyframe, awaiting a keyframe, and the
 * cause of the last trim or reset.
 *
 * These are stored-run facts measured with an explicit clock. An arrival span is not a
 * presentation duration, a buffered run is not a decodable or playable clip, and the saved
 * bytes are synthetic.
 */

#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using steady = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  using stream_recorder::window_limit_e;
  using stream_recorder::detail::save_status_e;

  constexpr std::uint64_t k_session = 301;
  constexpr std::uint64_t k_other = 302;
  constexpr std::uint64_t k_watcher = 303;
  constexpr std::uint64_t k_next = 304;
  constexpr std::uint64_t k_foreign = 77;
  constexpr int k_hevc = 1;
  constexpr int k_av1 = 2;
  constexpr int k_pyrowave = 3;
  constexpr auto k_stamp = "20261008_220000";
  constexpr auto k_bounded_wait = 10s;

  /// Holds a save inside storage until released. Release is idempotent.
  class barrier_t {
  public:
    void hold() {
      std::unique_lock lock {mutex};
      entered = true;
      changed.notify_all();
      changed.wait(lock, [this] { return released; });
    }

    bool wait_entered() {
      std::unique_lock lock {mutex};
      return changed.wait_for(lock, k_bounded_wait, [this] { return entered; });
    }

    void release() {
      {
        std::lock_guard lock {mutex};
        released = true;
      }
      changed.notify_all();
    }

  private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
  };

  /// An address no test may read: refusing before the copy is the only way not to crash.
  const std::uint8_t *unreadable() {
    return reinterpret_cast<const std::uint8_t *>(static_cast<std::uintptr_t>(16));
  }

  /**
   * Each test starts from the recorder's process start state, in replay mode with a one-minute
   * duration and one eligible session, saving into a not-yet-created directory inside a parent
   * this fixture created exclusively. TearDown always releases and joins a held save before
   * resetting, so no assertion or timeout can leave a writer blocked; it removes only the parent.
   */
  class StreamRecorderReplayWindowTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-window-"} + info->name() + "-" +
                                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                                       std::to_string(attempt));
        std::error_code ec;
        // create_directory() reports true only when this call made the directory.
        if (fs::create_directory(candidate, ec)) {
          owned_parent = candidate;
        }
      }
      ASSERT_FALSE(owned_parent.empty()) << "no exclusive temporary directory";
      config::recording.output_dir = (owned_parent / "recordings").string();
      config::recording.replay_buffer_minutes = 1;
      publish(true, true);
      stream_recorder::session_started(k_session, true);
      hooks.name_stamp = [] { return std::string {k_stamp}; };
      stream_recorder::detail::set_save_hooks_for_tests(hooks);
    }

    void TearDown() override {
      barrier.release();
      if (saver.joinable()) {
        saver.join();
      }
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

    static void push(bool keyframe, steady::time_point at, std::size_t size = 10, int video_format = k_hevc,
                     std::uint64_t generation = k_session) {
      const std::vector<std::uint8_t> payload(size, 0x5a);
      stream_recorder::detail::push_packet_at(payload.data(), payload.size(), true, keyframe, 0, generation, video_format, at);
    }

    static stream_recorder::replay_stats_t stats(steady::time_point at) {
      return stream_recorder::detail::replay_stats_at(at);
    }

    /// The window fields, exactly, so a test states the whole window at once.
    static void expect_window(const stream_recorder::replay_stats_t &s, bool run, std::size_t keyframes,
                              steady::duration span, steady::duration front_age, bool awaiting, window_limit_e limit) {
      EXPECT_EQ(s.keyframe_start_run, run);
      EXPECT_EQ(s.keyframe_start_run, s.packets > 0);
      EXPECT_EQ(s.run_keyframes, keyframes);
      EXPECT_EQ(s.run_span, span);
      EXPECT_EQ(s.run_front_age, front_age);
      EXPECT_EQ(s.awaiting_keyframe, awaiting);
      EXPECT_EQ(s.last_window_limit, limit);
    }

    static void expect_marker(const stream_recorder::replay_stats_t &s, steady::duration age) {
      ASSERT_TRUE(s.last_accepted_keyframe_age.has_value());
      EXPECT_EQ(*s.last_accepted_keyframe_age, age);
    }

    /// Start a save on another thread that stops inside storage, after its snapshot, until released.
    void start_held_save(steady::time_point at) {
      hooks.before_write = [this](const fs::path &) {
        barrier.hold();
      };
      stream_recorder::detail::set_save_hooks_for_tests(hooks);
      saver = std::thread([this, at] {
        held_result = stream_recorder::detail::save_replay_result_at(at);
      });
    }

    stream_recorder::detail::save_result_t finish_held_save() {
      barrier.release();
      if (saver.joinable()) {
        saver.join();
      }
      return held_result;
    }

    const steady::time_point t0 = steady::time_point {} + 1h;
    const steady::duration zero = steady::duration::zero();
    config::recording_t saved;
    fs::path owned_parent;
    stream_recorder::detail::save_hooks_t hooks;
    barrier_t barrier;
    std::thread saver;
    stream_recorder::detail::save_result_t held_result;
  };
}  // namespace

TEST_F(StreamRecorderReplayWindowTests, EmptyRunIsAwaitingAKeyframeBeforeAnyPacket) {
  // Selecting the session reset the run, and nothing has been refused yet.
  auto s = stats(t0);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::selection);
  EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());
  EXPECT_EQ(s.refused_awaiting_keyframe, 0u);

  // A dependent frame is refused; the state it reports is the same.
  push(false, t0);
  s = stats(t0);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::selection);
  EXPECT_EQ(s.refused_awaiting_keyframe, 1u);
  EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());

  // Not awaiting without replay, or without one selected eligible session.
  publish(true, false);
  EXPECT_FALSE(stats(t0).awaiting_keyframe);
  publish(false, true);
  EXPECT_FALSE(stats(t0).awaiting_keyframe);
  publish(true, true);
  EXPECT_TRUE(stats(t0).awaiting_keyframe);
  stream_recorder::session_started(k_watcher, false);
  EXPECT_FALSE(stats(t0).awaiting_keyframe);
  stream_recorder::session_ended(k_watcher);
  EXPECT_TRUE(stats(t0).awaiting_keyframe);
  stream_recorder::session_ended(k_session);
  EXPECT_FALSE(stats(t0).awaiting_keyframe);
}

TEST_F(StreamRecorderReplayWindowTests, FreshKeyframeRunReportsSpanKeyframesAndFrontAge) {
  push(true, t0);
  push(false, t0 + 1s);
  push(false, t0 + 2s);
  push(true, t0 + 3s);
  push(false, t0 + 4s);

  const auto s = stats(t0 + 10s);
  expect_window(s, true, 2, 4s, 10s, false, window_limit_e::selection);
  expect_marker(s, 7s);
  EXPECT_EQ(s.packets, 5u);
}

TEST_F(StreamRecorderReplayWindowTests, TheFreshnessBoundaryIsExactAndExpiryIsTheCause) {
  push(true, t0);
  push(false, t0 + 1s);

  // Exactly the duration old is still fresh.
  auto s = stats(t0 + 60s);
  expect_window(s, true, 1, 1s, 60s, false, window_limit_e::selection);
  expect_marker(s, 60s);

  // One clock tick later the keyframe expires, and its dependent frame goes with it.
  const auto later = t0 + 60s + steady::duration {1};
  s = stats(later);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::duration);
  // Expiry keeps the marker: it still says when the last keyframe arrived.
  expect_marker(s, 60s + steady::duration {1});
  EXPECT_EQ(s.trimmed, 2u);
}

TEST_F(StreamRecorderReplayWindowTests, InfiniteGopCollapsesAndResumesAtTheNextKeyframe) {
  push(true, t0);
  for (int i = 1; i <= 5; ++i) {
    push(false, t0 + std::chrono::seconds {10 * i});
  }
  ASSERT_EQ(stats(t0 + 50s).run_keyframes, 1u);

  // The only keyframe expires: the run collapses although dependent frames keep arriving.
  auto s = stats(t0 + 61s);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::duration);
  expect_marker(s, 61s);

  push(false, t0 + 62s);
  s = stats(t0 + 62s);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::duration);
  EXPECT_EQ(s.refused_awaiting_keyframe, 1u);

  // The next keyframe starts a new run; the recorded cause stays until another trim or reset.
  push(true, t0 + 70s);
  push(false, t0 + 71s);
  s = stats(t0 + 72s);
  expect_window(s, true, 1, 1s, 2s, false, window_limit_e::duration);
  expect_marker(s, 2s);
}

TEST_F(StreamRecorderReplayWindowTests, SparseGopTrimsWholeGroupsAndReportsTheShorterSpan) {
  push(true, t0);
  push(false, t0 + 10s);
  push(true, t0 + 30s);
  push(false, t0 + 50s);
  auto s = stats(t0 + 59s);
  expect_window(s, true, 2, 50s, 59s, false, window_limit_e::selection);

  // The first group expires whole, though its dependent frame is younger than the duration.
  s = stats(t0 + 65s);
  expect_window(s, true, 1, 20s, 35s, false, window_limit_e::duration);
  expect_marker(s, 35s);
  EXPECT_EQ(s.packets, 2u);
}

TEST_F(StreamRecorderReplayWindowTests, PayloadAndEntryCapsAreRecordedAsTheCause) {
  stream_recorder::detail::set_replay_limits_for_tests(100, 1000);
  stream_recorder::session_started(k_session, true);
  push(true, t0, 40);
  push(false, t0 + 1s, 30);
  push(false, t0 + 2s, 30);
  push(true, t0 + 3s, 30);
  auto s = stats(t0 + 3s);
  expect_window(s, true, 1, zero, zero, false, window_limit_e::payload_cap);
  EXPECT_EQ(s.bytes, 30u);
  expect_marker(s, zero);

  // A new cap is a reconfiguration: the run and its marker reset.
  stream_recorder::detail::set_replay_limits_for_tests(1000, 4);
  s = stats(t0 + 3s);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::configuration);
  EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());

  push(true, t0 + 4s);
  push(false, t0 + 5s);
  push(false, t0 + 6s);
  push(true, t0 + 7s);
  push(false, t0 + 8s);
  s = stats(t0 + 8s);
  expect_window(s, true, 1, 1s, 1s, false, window_limit_e::entry_cap);
  EXPECT_EQ(s.packets, 2u);
}

TEST_F(StreamRecorderReplayWindowTests, WhenBothCapsApplyThePayloadCapIsRecorded) {
  // Full on both: 100 bytes and 3 entries.
  stream_recorder::detail::set_replay_limits_for_tests(100, 3);
  push(true, t0, 40);
  push(false, t0, 30);
  push(false, t0, 30);
  push(true, t0 + 1s, 30);
  auto s = stats(t0 + 1s);
  expect_window(s, true, 1, zero, zero, false, window_limit_e::payload_cap);

  // Full on entries only.
  stream_recorder::detail::set_replay_limits_for_tests(1000, 3);
  push(true, t0 + 2s);
  push(false, t0 + 2s);
  push(false, t0 + 2s);
  push(true, t0 + 3s);
  s = stats(t0 + 3s);
  expect_window(s, true, 1, zero, zero, false, window_limit_e::entry_cap);
}

TEST_F(StreamRecorderReplayWindowTests, SelectionOverlapResetsTheWindowAndForgetsTheMarker) {
  push(true, t0);
  expect_marker(stats(t0), zero);

  for (const auto [second, eligible] : {std::pair {k_other, true}, std::pair {k_watcher, false}}) {
    stream_recorder::session_started(second, eligible);
    auto s = stats(t0 + 1s);
    expect_window(s, false, 0, zero, zero, false, window_limit_e::selection);
    EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());
    // Neither session's keyframe is accepted while two are registered.
    push(true, t0 + 1s, 10, k_hevc, k_session);
    push(true, t0 + 1s, 10, k_hevc, second);
    s = stats(t0 + 1s);
    EXPECT_FALSE(s.keyframe_start_run);
    EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());

    // Back to the sole session: selected again, with no run and no marker until a keyframe.
    stream_recorder::session_ended(second);
    s = stats(t0 + 1s);
    expect_window(s, false, 0, zero, zero, true, window_limit_e::selection);
    EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());
    push(true, t0 + 2s);
    expect_marker(stats(t0 + 2s), zero);
  }
}

TEST_F(StreamRecorderReplayWindowTests, ForeignZeroRetiredInvalidAndOversizedPacketsNeverSetTheMarker) {
  stream_recorder::detail::set_replay_limits_for_tests(100, 1000);
  stream_recorder::session_started(k_session, true);
  push(true, t0);

  // Foreign and zero generations are refused before anything else: the window is untouched.
  push(true, t0 + 5s, 10, k_hevc, k_foreign);
  push(true, t0 + 5s, 10, k_hevc, 0);
  stream_recorder::detail::push_packet_at(nullptr, 10, true, true, 0, k_foreign, k_hevc, t0 + 5s);
  auto s = stats(t0 + 5s);
  expect_window(s, true, 1, zero, 5s, false, window_limit_e::configuration);
  expect_marker(s, 5s);

  // An invalid owner keyframe is a discontinuity: the run and the marker reset; it sets nothing.
  stream_recorder::detail::push_packet_at(nullptr, 10, true, true, 0, k_session, k_hevc, t0 + 6s);
  s = stats(t0 + 6s);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::discontinuity);
  EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());

  // So is an oversized owner keyframe, refused before it is read.
  push(true, t0 + 7s);
  stream_recorder::detail::push_packet_at(unreadable(), 101, true, true, 0, k_session, k_hevc, t0 + 8s);
  s = stats(t0 + 8s);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::discontinuity);
  EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());

  // A retired generation's keyframe after the next session is selected sets nothing either.
  stream_recorder::session_ended(k_session);
  stream_recorder::session_started(k_next, true);
  push(true, t0 + 9s, 10, k_hevc, k_session);
  s = stats(t0 + 9s);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::selection);
  EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());
  EXPECT_EQ(s.refused_unselected, 4u);
}

TEST_F(StreamRecorderReplayWindowTests, DurationModeCodecAndClockTransitionsRecordTheirCause) {
  push(true, t0);
  config::recording.replay_buffer_minutes = 2;
  stream_recorder::load_config();
  auto s = stats(t0);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::configuration);
  EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());

  push(true, t0, 10, k_hevc);
  push(false, t0 + 1s, 10, k_av1);
  s = stats(t0 + 1s);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::codec);
  EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());
  push(true, t0 + 2s, 10, k_av1);
  s = stats(t0 + 2s);
  expect_window(s, true, 1, zero, zero, false, window_limit_e::codec);
  EXPECT_EQ(s.run_video_format, k_av1);

  // A packet older than the run's newest is a discontinuity; its keyframe then starts a run.
  push(true, t0 + 10s, 10, k_av1);
  push(true, t0 + 5s, 10, k_av1);
  s = stats(t0 + 10s);
  expect_window(s, true, 1, zero, 5s, false, window_limit_e::discontinuity);
  expect_marker(s, 5s);

  publish(false, true);
  s = stats(t0 + 10s);
  expect_window(s, false, 0, zero, zero, false, window_limit_e::configuration);
  EXPECT_FALSE(s.last_accepted_keyframe_age.has_value());
}

TEST_F(StreamRecorderReplayWindowTests, IdenticalConfigurationReloadChangesNothing) {
  push(true, t0);
  push(false, t0 + 2s);
  const auto before = stats(t0 + 3s);
  publish(true, true);
  const auto after = stats(t0 + 3s);
  expect_window(after, before.keyframe_start_run, before.run_keyframes, before.run_span, before.run_front_age,
                before.awaiting_keyframe, before.last_window_limit);
  EXPECT_EQ(after.last_accepted_keyframe_age, before.last_accepted_keyframe_age);
  EXPECT_EQ(after.packets, 2u);
  EXPECT_EQ(after.trimmed, before.trimmed);
}

TEST_F(StreamRecorderReplayWindowTests, ClockRegressedReadsNeverGoNegativeOrRestoreExpiredPackets) {
  push(true, t0 + 10s);
  push(false, t0 + 11s);

  // A read stamped before every arrival: ages are zero, never negative; nothing changes.
  auto s = stats(t0);
  expect_window(s, true, 1, 1s, zero, false, window_limit_e::selection);
  expect_marker(s, zero);

  s = stats(t0 + 75s);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::duration);

  // A later read stamped earlier cannot bring the released run back.
  s = stats(t0 + 10s);
  expect_window(s, false, 0, zero, zero, true, window_limit_e::duration);
  expect_marker(s, zero);
}

TEST_F(StreamRecorderReplayWindowTests, AHeldSaveKeepsItsCapturedWindowWhileTheLiveRunChanges) {
  push(true, t0);
  push(false, t0 + 2s);
  push(true, t0 + 4s);
  start_held_save(t0 + 5s);
  ASSERT_TRUE(barrier.wait_entered());

  auto s = stats(t0 + 5s);
  EXPECT_TRUE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_keyframes, 2u);
  EXPECT_EQ(s.snapshot_span, 4s);

  // The live run moves on and then empties; the captured window does not change.
  push(false, t0 + 6s);
  push(true, t0 + 8s);
  s = stats(t0 + 8s);
  expect_window(s, true, 3, 8s, 8s, false, window_limit_e::selection);
  EXPECT_EQ(s.snapshot_keyframes, 2u);
  EXPECT_EQ(s.snapshot_span, 4s);
  stream_recorder::session_ended(k_session);
  s = stats(t0 + 8s);
  EXPECT_FALSE(s.keyframe_start_run);
  EXPECT_EQ(s.snapshot_keyframes, 2u);
  EXPECT_EQ(s.snapshot_span, 4s);

  const auto result = finish_held_save();
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(result.keyframes, 2u);
  EXPECT_EQ(result.span, 4s);
  EXPECT_EQ(result.packets, 3u);
  s = stats(t0 + 8s);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_keyframes, 0u);
  EXPECT_EQ(s.snapshot_span, zero);

  // Both a reported storage failure and an exception must release the new window fields.
  stream_recorder::session_started(k_next, true);
  push(true, t0 + 10s, 10, k_hevc, k_next);
  push(true, t0 + 13s, 10, k_hevc, k_next);
  hooks.before_write = {};
  hooks.publish = [](const fs::path &, const fs::path &) {
    return std::make_error_code(std::errc::io_error);
  };
  stream_recorder::detail::set_save_hooks_for_tests(hooks);
  auto failed = stream_recorder::detail::save_replay_result_at(t0 + 14s);
  EXPECT_EQ(failed.status, save_status_e::storage_error);
  EXPECT_EQ(failed.keyframes, 2u);
  EXPECT_EQ(failed.span, 3s);
  s = stats(t0 + 14s);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_keyframes, 0u);
  EXPECT_EQ(s.snapshot_span, zero);
  EXPECT_EQ(s.saves_failed, 1u);

  hooks.before_write = [](const fs::path &) { throw std::bad_alloc {}; };
  stream_recorder::detail::set_save_hooks_for_tests(hooks);
  failed = stream_recorder::detail::save_replay_result_at(t0 + 14s);
  EXPECT_EQ(failed.status, save_status_e::storage_error);
  s = stats(t0 + 14s);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_keyframes, 0u);
  EXPECT_EQ(s.snapshot_span, zero);
  EXPECT_EQ(s.saves_failed, 2u);
  expect_window(s, true, 2, 3s, 4s, false, window_limit_e::selection);
}

TEST_F(StreamRecorderReplayWindowTests, IntraOnlyRunTrimsPerFrameAndKeepsItsWindow) {
  for (int i = 0; i <= 10; ++i) {
    push(true, t0 + std::chrono::seconds {10 * i}, 10, k_pyrowave);
  }
  // Frames older than one minute go one by one: the run keeps a full minute of arrivals.
  auto s = stats(t0 + 100s);
  expect_window(s, true, 7, 60s, 60s, false, window_limit_e::duration);
  expect_marker(s, zero);

  const auto result = stream_recorder::detail::save_replay_result_at(t0 + 100s);
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(result.keyframes, 7u);
  EXPECT_EQ(result.span, 60s);
  EXPECT_EQ(result.video_format, k_pyrowave);
}

TEST_F(StreamRecorderReplayWindowTests, StatusReadsAreIdempotentExceptForExpiry) {
  push(true, t0);
  push(false, t0 + 1s);
  const auto first = stats(t0 + 5s);
  const auto second = stats(t0 + 5s);
  expect_window(second, first.keyframe_start_run, first.run_keyframes, first.run_span, first.run_front_age,
                first.awaiting_keyframe, first.last_window_limit);
  EXPECT_EQ(second.last_accepted_keyframe_age, first.last_accepted_keyframe_age);
  EXPECT_EQ(second.trimmed, first.trimmed);
  EXPECT_EQ(second.packets, first.packets);

  // Later reads change only the ages until something expires.
  const auto later = stats(t0 + 30s);
  expect_window(later, true, 1, 1s, 30s, false, first.last_window_limit);
  expect_marker(later, 30s);
  EXPECT_EQ(later.trimmed, first.trimmed);
}
