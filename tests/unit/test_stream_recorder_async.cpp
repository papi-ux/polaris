/**
 * @file tests/unit/test_stream_recorder_async.cpp
 * @brief Test the two-phase replay save in src/stream_recorder.*: admission captures the run
 * and takes the one-save reservation on the calling thread; a move-only handle owns both until
 * it is completed (storage) or dropped, on any thread, releasing the snapshot and its
 * accounting before the reservation, exactly once.
 *
 * Saved bytes are synthetic raw video; nothing here claims a playable clip.
 */

#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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
  using stream_recorder::detail::save_status_e;

  constexpr std::uint64_t k_session = 501;
  constexpr int k_hevc = 1;
  constexpr auto k_stamp = "20261009_001603";

  /**
   * Each test starts from the recorder's process start state, in replay mode with a one-minute
   * duration and one eligible session, saving into a not-yet-created directory inside a parent
   * created exclusively. Handles and worker threads are locals, released and joined before
   * TearDown resets the recorder and removes only that parent.
   */
  class RecorderAsyncSaveTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-async-save-"} + info->name() + "-" +
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
      stream_recorder::session_started(k_session, true);
      hooks.name_stamp = [] { return std::string {k_stamp}; };
      install();
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

    void install() {
      stream_recorder::detail::set_save_hooks_for_tests(hooks);
    }

    void push(bool keyframe, steady::time_point at, std::size_t size, char fill) {
      const std::vector<std::uint8_t> payload(size, static_cast<std::uint8_t>(fill));
      stream_recorder::detail::push_packet_at(payload.data(), payload.size(), true, keyframe, 0, k_session, k_hevc, at);
    }

    /// A keyframe of 4 'A' and a dependent frame of 3 'B'.
    void push_run() {
      push(true, t0, 4, 'A');
      push(false, t0 + 1s, 3, 'B');
    }

    static stream_recorder::replay_stats_t stats(steady::time_point at) {
      return stream_recorder::detail::replay_stats_at(at);
    }

    static std::string read_file(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    /// Complete `save` on another thread; the thread is joined before this returns.
    static stream_recorder::detail::save_result_t complete_on_another_thread(stream_recorder::detail::admitted_save_t save) {
      stream_recorder::detail::save_result_t result;
      std::thread::id caller = std::this_thread::get_id();
      std::thread::id worker;
      {
        std::jthread thread([&, owned = std::move(save)]() mutable {
          worker = std::this_thread::get_id();
          result = stream_recorder::detail::complete_replay_save(std::move(owned));
        });
      }
      EXPECT_NE(worker, caller);
      return result;
    }

    /// The reservation is free: a fresh admission is not busy. When a run is buffered the probe
    /// is admitted and dropped, which counts one failed save, so call it after counter checks.
    static bool reservation_free(steady::time_point at) {
      const auto probe = stream_recorder::detail::admit_replay_save(at);
      return probe.result.status != save_status_e::busy;
    }

    const steady::time_point t0 = steady::time_point {} + 1h;
    config::recording_t saved;
    fs::path owned_parent;
    fs::path output_dir;
    stream_recorder::detail::save_hooks_t hooks;
  };
}  // namespace

TEST_F(RecorderAsyncSaveTests, AnAdmittedSaveIsCompletedOnAnotherThread) {
  push_run();
  auto admission = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(admission.save);
  EXPECT_TRUE(admission.result.admitted);
  EXPECT_EQ(admission.result.packets, 2u);
  EXPECT_EQ(admission.result.bytes, 7u);
  EXPECT_EQ(admission.result.keyframes, 1u);
  EXPECT_EQ(admission.result.span, 1s);
  EXPECT_EQ(admission.result.video_format, k_hevc);
  EXPECT_EQ(admission.result.generation, k_session);
  // Admission stores nothing: no directory yet, and the save is in progress.
  EXPECT_FALSE(fs::exists(output_dir));
  auto s = stats(t0 + 2s);
  EXPECT_TRUE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_packets, 2u);

  // One save at a time, refused at once while the handle lives.
  const auto second = stream_recorder::detail::admit_replay_save(t0 + 2s);
  EXPECT_EQ(second.result.status, save_status_e::busy);
  EXPECT_FALSE(second.save);
  EXPECT_FALSE(second.result.admitted);
  EXPECT_EQ(stats(t0 + 2s).saves_refused_busy, 1u);

  const auto result = complete_on_another_thread(std::move(admission.save));
  EXPECT_FALSE(admission.save);
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_TRUE(result.admitted);
  EXPECT_EQ(result.packets, 2u);
  EXPECT_EQ(result.bytes, 7u);
  EXPECT_EQ(fs::path {result.path}.parent_path(), output_dir);
  EXPECT_EQ(read_file(result.path), "AAAABBB");

  s = stats(t0 + 2s);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_packets + s.snapshot_bytes + s.snapshot_only_bytes, 0u);
  EXPECT_EQ(s.saves_completed, 1u);
  EXPECT_TRUE(reservation_free(t0 + 2s));
}

TEST_F(RecorderAsyncSaveTests, TheCapturedRunSurvivesModeSessionAndConfigurationChanges) {
  push_run();
  auto admission = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(admission.save);

  const auto moved = owned_parent / "moved";
  stream_recorder::session_ended(k_session);
  config::recording.output_dir = moved.string();
  config::recording.replay_buffer_minutes = 5;
  publish(false, true);
  auto s = stats(t0 + 2s);
  EXPECT_FALSE(s.keyframe_start_run);
  EXPECT_TRUE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_packets, 2u);

  const auto result = complete_on_another_thread(std::move(admission.save));
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(fs::path {result.path}.parent_path(), output_dir);
  EXPECT_EQ(read_file(result.path), "AAAABBB");
  EXPECT_EQ(result.generation, k_session);
  EXPECT_FALSE(fs::exists(moved));
  // A new admission reads the current state: replay is disabled now.
  EXPECT_EQ(stream_recorder::detail::admit_replay_save(t0 + 2s).result.status, save_status_e::disabled);
}

TEST_F(RecorderAsyncSaveTests, DroppedAndMovedHandlesReleaseExactlyOnce) {
  push_run();
  {
    auto admission = stream_recorder::detail::admit_replay_save(t0 + 2s);
    ASSERT_TRUE(admission.save);
    auto moved = std::move(admission.save);
    EXPECT_FALSE(admission.save);
    EXPECT_TRUE(moved);
    // The moved-from handle owns nothing: dropping it releases nothing.
    admission.save = stream_recorder::detail::admitted_save_t {};
    EXPECT_TRUE(stats(t0 + 2s).save_in_progress);
    EXPECT_FALSE(reservation_free(t0 + 2s));
  }
  // Dropped without storage: released once, counted as a failed save, nothing published.
  auto s = stats(t0 + 2s);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_packets + s.snapshot_bytes, 0u);
  EXPECT_EQ(s.saves_failed, 1u);
  EXPECT_EQ(s.saves_completed, 0u);
  EXPECT_FALSE(fs::exists(output_dir));

  // Move-assigning over an admitted handle releases the one it held.
  auto first = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(first.save);
  first.save = stream_recorder::detail::admitted_save_t {};
  s = stats(t0 + 2s);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.saves_failed, 2u);

  // Dropped on another thread works the same.
  auto other = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(other.save);
  {
    std::jthread thread([owned = std::move(other.save)]() mutable {
      owned = stream_recorder::detail::admitted_save_t {};
    });
  }
  s = stats(t0 + 2s);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.saves_failed, 3u);
  EXPECT_TRUE(reservation_free(t0 + 2s));
}

TEST_F(RecorderAsyncSaveTests, TheReservationIsReleasedOnlyAfterTheSnapshotAccounting) {
  push_run();
  int observations = 0;
  bool in_progress_at_release = true;
  std::size_t snapshot_packets_at_release = 99;
  std::uint64_t completed_at_release = 0;
  bool busy_at_release = false;
  hooks.before_save_reservation_release = [&] {
    if (observations++ > 0) {
      return;
    }
    // No recorder lock is held here; the accounting must already be released while the
    // reservation is still held.
    const auto s = stats(t0 + 2s);
    in_progress_at_release = s.save_in_progress;
    snapshot_packets_at_release = s.snapshot_packets;
    completed_at_release = s.saves_completed;
    busy_at_release = stream_recorder::detail::admit_replay_save(t0 + 2s).result.status == save_status_e::busy;
  };
  install();

  auto admission = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(admission.save);
  const auto result = complete_on_another_thread(std::move(admission.save));
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(observations, 1);
  EXPECT_FALSE(in_progress_at_release);
  EXPECT_EQ(snapshot_packets_at_release, 0u);
  EXPECT_EQ(completed_at_release, 1u);
  EXPECT_TRUE(busy_at_release);
}

TEST_F(RecorderAsyncSaveTests, RefusalsValidateBeforeAllocatingAndHoldNothing) {
  int allocations = 0;
  hooks.before_admission_allocation = [&] {
    ++allocations;
  };
  install();

  // Empty: replay on, nothing buffered.
  auto admission = stream_recorder::detail::admit_replay_save(t0);
  EXPECT_EQ(admission.result.status, save_status_e::empty);
  EXPECT_FALSE(admission.save);
  EXPECT_FALSE(admission.result.admitted);

  // Stale: the run expired at the admission boundary.
  push_run();
  admission = stream_recorder::detail::admit_replay_save(t0 + 62s);
  EXPECT_EQ(admission.result.status, save_status_e::empty);
  EXPECT_FALSE(admission.save);

  // Disabled.
  publish(false, true);
  admission = stream_recorder::detail::admit_replay_save(t0);
  EXPECT_EQ(admission.result.status, save_status_e::disabled);
  EXPECT_FALSE(admission.save);

  // Busy: refused before the recorder is even locked.
  publish(true, true);
  push_run();
  auto held = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(held.save);
  EXPECT_EQ(allocations, 1);
  admission = stream_recorder::detail::admit_replay_save(t0 + 2s);
  EXPECT_EQ(admission.result.status, save_status_e::busy);
  EXPECT_FALSE(admission.save);

  // Only the one eligible admission allocated, and no refusal touched storage.
  EXPECT_EQ(allocations, 1);
  EXPECT_FALSE(fs::exists(output_dir));
  held.save = stream_recorder::detail::admitted_save_t {};
  EXPECT_TRUE(reservation_free(t0 + 2s));
}

TEST_F(RecorderAsyncSaveTests, AdmissionFailuresReleaseEverythingAndAdmitNothing) {
  push_run();
  for (const auto *boundary : {"allocation", "snapshot"}) {
    SCOPED_TRACE(boundary);
    hooks.before_admission_allocation = {};
    hooks.after_snapshot_payloads = {};
    if (std::string {boundary} == "allocation") {
      hooks.before_admission_allocation = [] {
        throw std::bad_alloc {};
      };
    } else {
      hooks.after_snapshot_payloads = [] {
        throw std::bad_alloc {};
      };
    }
    install();
    const auto admission = stream_recorder::detail::admit_replay_save(t0 + 2s);
    EXPECT_EQ(admission.result.status, save_status_e::storage_error);
    EXPECT_FALSE(admission.result.admitted);
    EXPECT_FALSE(admission.save);
    EXPECT_EQ(admission.result.packets, 0u);
    const auto s = stats(t0 + 2s);
    EXPECT_FALSE(s.save_in_progress);
    EXPECT_EQ(s.snapshot_packets + s.snapshot_bytes, 0u);
    // Nothing was admitted, so nothing is counted as a failed save; the live run is intact.
    EXPECT_EQ(s.saves_failed, 0u);
    EXPECT_EQ(s.bytes, 7u);
  }
  hooks.before_admission_allocation = {};
  hooks.after_snapshot_payloads = {};
  install();
  auto recovered = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(recovered.save);
  EXPECT_FALSE(fs::exists(output_dir));
}

TEST_F(RecorderAsyncSaveTests, AStorageFailureOnAnotherThreadKeepsTheCaptureAndReleases) {
  push_run();
  hooks.publish = [](const fs::path &, const fs::path &) {
    return std::make_error_code(std::errc::io_error);
  };
  install();
  auto admission = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(admission.save);

  const auto result = complete_on_another_thread(std::move(admission.save));
  EXPECT_EQ(result.status, save_status_e::storage_error);
  EXPECT_TRUE(result.admitted);
  EXPECT_EQ(result.packets, 2u);
  EXPECT_EQ(result.bytes, 7u);
  EXPECT_TRUE(result.path.empty());
  const auto s = stats(t0 + 2s);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.saves_failed, 1u);

  // A storage exception is released the same way (admitting again proves the reservation is free).
  hooks.publish = {};
  hooks.before_write = [](const fs::path &) {
    throw std::bad_alloc {};
  };
  install();
  auto again = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(again.save);
  const auto thrown = complete_on_another_thread(std::move(again.save));
  EXPECT_EQ(thrown.status, save_status_e::storage_error);
  EXPECT_TRUE(thrown.admitted);
  EXPECT_EQ(stats(t0 + 2s).saves_failed, 2u);
  EXPECT_TRUE(reservation_free(t0 + 2s));
}

TEST_F(RecorderAsyncSaveTests, AnEmptyHandleCompletesAsAStorageErrorAndTouchesNothing) {
  push_run();
  const auto result = stream_recorder::detail::complete_replay_save(stream_recorder::detail::admitted_save_t {});
  EXPECT_EQ(result.status, save_status_e::storage_error);
  EXPECT_FALSE(result.admitted);
  const auto s = stats(t0 + 2s);
  EXPECT_EQ(s.saves_completed + s.saves_failed, 0u);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_FALSE(fs::exists(output_dir));
  EXPECT_TRUE(reservation_free(t0 + 2s));
}

TEST_F(RecorderAsyncSaveTests, TheSynchronousSaveIsAdmissionThenCompletion) {
  push_run();
  auto held = stream_recorder::detail::admit_replay_save(t0 + 2s);
  ASSERT_TRUE(held.save);
  // The synchronous wrapper shares the reservation.
  EXPECT_EQ(stream_recorder::detail::save_replay_result_at(t0 + 2s).status, save_status_e::busy);
  EXPECT_EQ(stream_recorder::detail::save_replay_at(t0 + 2s), "");
  held.save = stream_recorder::detail::admitted_save_t {};

  const auto result = stream_recorder::detail::save_replay_result_at(t0 + 2s);
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_TRUE(result.admitted);
  EXPECT_EQ(result.packets, 2u);
  EXPECT_EQ(result.keyframes, 1u);
  EXPECT_EQ(result.span, 1s);
  EXPECT_EQ(read_file(result.path), "AAAABBB");
  const auto s = stats(t0 + 2s);
  EXPECT_EQ(s.saves_completed, 1u);
  EXPECT_EQ(s.saves_failed, 1u);
  EXPECT_EQ(s.saves_refused_busy, 2u);
  EXPECT_TRUE(reservation_free(t0 + 2s));
}
