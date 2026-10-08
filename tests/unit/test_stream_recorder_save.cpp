/**
 * @file tests/unit/test_stream_recorder_save.cpp
 * @brief Test the replay save in src/stream_recorder.*: an immutable snapshot taken under the
 * recorder lock, then checked, private, exclusive storage with no recorder lock held.
 *
 * Every save runs the production file flow in a directory this fixture created. Seams only
 * pause it, fix the name stamp, or wrap one native operation to inject a short write or a
 * failure; the bytes saved are synthetic, not playable media.
 */

#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using steady = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  using stream_recorder::detail::save_status_e;

  constexpr std::uint64_t k_session = 201;
  constexpr std::uint64_t k_other_session = 202;
  constexpr int k_hevc = 1;
  constexpr int k_av1 = 2;
  constexpr auto k_stamp = "20261008_120000";
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

  /**
   * Each test starts from the recorder's process start state, in replay mode with one eligible
   * session, and saves into a not-yet-created `recordings` directory inside a parent this
   * fixture created exclusively. TearDown always releases and joins a held save first, so an
   * assertion or timeout cannot leave a writer blocked; it removes only the owned parent.
   */
  class StreamRecorderReplaySaveTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-save-"} + info->name() + "-" +
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

    void install() {
      stream_recorder::detail::set_save_hooks_for_tests(hooks);
    }

    static void push(bool keyframe, steady::time_point at, std::size_t size, char fill, int video_format = k_hevc,
                     std::uint64_t generation = k_session) {
      const std::vector<std::uint8_t> payload(size, static_cast<std::uint8_t>(fill));
      stream_recorder::detail::push_packet_at(payload.data(), payload.size(), true, keyframe, 0, generation, video_format, at);
    }

    static stream_recorder::replay_stats_t stats(steady::time_point at) {
      return stream_recorder::detail::replay_stats_at(at);
    }

    static stream_recorder::detail::save_result_t save(steady::time_point at) {
      return stream_recorder::detail::save_replay_result_at(at);
    }

    /// Start a save on another thread that stops inside storage, after its snapshot, until released.
    void start_held_save(steady::time_point at) {
      hooks.before_write = [this](const fs::path &) {
        barrier.hold();
      };
      install();
      saver = std::thread([this, at] {
        held_result = save(at);
      });
    }

    stream_recorder::detail::save_result_t finish_held_save() {
      barrier.release();
      if (saver.joinable()) {
        saver.join();
      }
      return held_result;
    }

    /// Run `operation` on another thread; true if it finished within the bounded wait. On a
    /// timeout the held save is released first, so the operation can finish and be joined.
    bool completes_while_held(const std::function<void()> &operation) {
      std::mutex done_mutex;
      std::condition_variable done_changed;
      bool done = false;
      std::thread worker([&] {
        operation();
        std::lock_guard lock {done_mutex};
        done = true;
        done_changed.notify_all();
      });
      bool finished;
      {
        std::unique_lock lock {done_mutex};
        finished = done_changed.wait_for(lock, k_bounded_wait, [&] { return done; });
      }
      if (!finished) {
        barrier.release();
      }
      worker.join();
      return finished;
    }

    static std::string read_file(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    static void write_file(const fs::path &path, const std::string &text) {
      std::ofstream out(path, std::ios::binary);
      out << text;
    }

    /// Every name in `dir`, or nothing when it does not exist.
    static std::vector<std::string> names_in(const fs::path &dir) {
      std::vector<std::string> names;
      std::error_code ec;
      for (fs::directory_iterator it {dir, ec}, end; !ec && it != end; it.increment(ec)) {
        names.push_back(it->path().filename().string());
      }
      std::sort(names.begin(), names.end());
      return names;
    }

    fs::path final_path(const std::string &suffix = "") const {
      return output_dir / (std::string {"polaris_"} + k_stamp + "_replay" + suffix + ".hevc");
    }

    fs::path staging_path(int attempt) const {
      return output_dir / (std::string {".polaris_"} + k_stamp + "_replay.hevc.staging-" + std::to_string(attempt));
    }

    /// A keyframe of 4 'A' and a dependent frame of 3 'B', the run most tests save.
    void push_run(steady::time_point at) {
      push(true, at, 4, 'A');
      push(false, at, 3, 'B');
    }

    /// A storage failure publishes nothing and leaves no staging file.
    void expect_failed_cleanly(const stream_recorder::detail::save_result_t &result, const std::string &what) {
      EXPECT_EQ(result.status, save_status_e::storage_error);
      EXPECT_TRUE(result.path.empty());
      EXPECT_NE(result.error.find(what), std::string::npos) << result.error;
      EXPECT_TRUE(names_in(output_dir).empty());
      const auto s = stats(t0);
      EXPECT_EQ(s.saves_failed, 1u);
      EXPECT_EQ(s.saves_completed, 0u);
      EXPECT_FALSE(s.save_in_progress);
    }

    const steady::time_point t0 = steady::time_point {} + 1h;
    config::recording_t saved;
    fs::path owned_parent;
    fs::path output_dir;
    stream_recorder::detail::save_hooks_t hooks;
    barrier_t barrier;
    std::thread saver;
    stream_recorder::detail::save_result_t held_result;
  };

#ifndef _WIN32
  constexpr auto k_private = fs::perms::owner_read | fs::perms::owner_write;
#endif
}  // namespace

TEST_F(StreamRecorderReplaySaveTests, SavesCapturedBytesWithTheirOwnCodec) {
  push_run(t0);
  // Another codec published afterwards does not rename this run.
  stream_recorder::set_active_video_format(k_av1);

  const auto result = save(t0 + 1s);
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(result.path, final_path().string());
  EXPECT_EQ(read_file(result.path), "AAAABBB");
  EXPECT_EQ(result.packets, 2u);
  EXPECT_EQ(result.bytes, 7u);
  EXPECT_EQ(result.video_format, k_hevc);
  EXPECT_EQ(result.generation, k_session);
  // Only the published file remains: the staging name was removed.
  EXPECT_EQ(names_in(output_dir), std::vector<std::string> {final_path().filename().string()});
}

TEST_F(StreamRecorderReplaySaveTests, SnapshotDoesNotBlockPacketOrLifecycleOperations) {
  push_run(t0);
  start_held_save(t0);
  ASSERT_TRUE(barrier.wait_entered());

  // The writer is stopped inside storage; none of these may wait for it.
  EXPECT_TRUE(completes_while_held([&] {
    push(true, t0 + 1s, 5, 'C');
    push(false, t0 + 1s, 5, 'D');
  }));
  EXPECT_TRUE(completes_while_held([&] {
    const auto s = stats(t0 + 1s);
    EXPECT_TRUE(s.save_in_progress);
    EXPECT_EQ(s.packets, 4u);
  }));
  EXPECT_TRUE(completes_while_held([&] {
    stream_recorder::session_started(k_other_session, true);
    stream_recorder::session_ended(k_other_session);
  }));
  EXPECT_TRUE(completes_while_held([&] {
    publish(true, true);
    config::recording.replay_buffer_minutes = 2;
    stream_recorder::load_config();
  }));
  EXPECT_TRUE(stats(t0 + 1s).save_in_progress);

  const auto result = finish_held_save();
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  // Exactly the run captured when the save was admitted.
  EXPECT_EQ(read_file(result.path), "AAAABBB");
}

TEST_F(StreamRecorderReplaySaveTests, RejectsSecondSaveWithoutBlocking) {
  push_run(t0);
  start_held_save(t0);
  ASSERT_TRUE(barrier.wait_entered());

  stream_recorder::detail::save_result_t second;
  std::string legacy = "unset";
  EXPECT_TRUE(completes_while_held([&] {
    second = save(t0);
    legacy = stream_recorder::detail::save_replay_at(t0);
  }));
  EXPECT_EQ(second.status, save_status_e::busy);
  EXPECT_TRUE(second.path.empty());
  EXPECT_EQ(legacy, "");
  EXPECT_EQ(stats(t0).saves_refused_busy, 2u);

  const auto first = finish_held_save();
  ASSERT_EQ(first.status, save_status_e::saved) << first.error;
  // The gate is open again: a new request checks the current state and saves.
  hooks.before_write = nullptr;
  install();
  const auto next = save(t0);
  EXPECT_EQ(next.status, save_status_e::saved) << next.error;
}

TEST_F(StreamRecorderReplaySaveTests, CapturedSaveFinishesAcrossSessionAndConfigurationChanges) {
  push_run(t0);
  start_held_save(t0);
  ASSERT_TRUE(barrier.wait_entered());

  const auto moved = owned_parent / "moved";
  stream_recorder::session_ended(k_session);
  config::recording.output_dir = moved.string();
  config::recording.replay_buffer_minutes = 5;
  publish(false, true);

  const auto result = finish_held_save();
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  // Its captured directory, bytes, codec and generation, none of the later configuration.
  EXPECT_EQ(result.path, final_path().string());
  EXPECT_EQ(read_file(result.path), "AAAABBB");
  EXPECT_EQ(result.video_format, k_hevc);
  EXPECT_EQ(result.generation, k_session);
  EXPECT_FALSE(fs::exists(moved));

  // A new request reads the current state: replay is now disabled.
  EXPECT_EQ(save(t0).status, save_status_e::disabled);
  EXPECT_FALSE(fs::exists(moved));
}

TEST_F(StreamRecorderReplaySaveTests, SnapshotPayloadAndLiveRunStayWithinTheirSeparateCaps) {
  stream_recorder::detail::set_replay_limits_for_tests(100, 8);
  push(true, t0, 40, 'a');
  push(false, t0, 30, 'b');
  push(false, t0, 30, 'c');
  start_held_save(t0);
  ASSERT_TRUE(barrier.wait_entered());

  auto s = stats(t0);
  EXPECT_EQ(s.snapshot_packets, 3u);
  EXPECT_EQ(s.snapshot_bytes, 100u);
  EXPECT_EQ(s.snapshot_only_bytes, 0u);  // still shared with the live run, not copied
  EXPECT_EQ(s.unique_payload_bytes, 100u);
  EXPECT_EQ(s.metadata_entries, 6u);
  EXPECT_EQ(s.max_snapshot_bytes, 100u);
  EXPECT_EQ(s.max_snapshot_packets, 8u);
  EXPECT_EQ(s.max_unique_payload_bytes, 200u);
  EXPECT_EQ(s.max_metadata_entries, 16u);

  // The live run moves on and drops everything the snapshot holds.
  push(true, t0, 40, 'd');
  push(false, t0, 30, 'e');
  push(false, t0, 30, 'f');
  s = stats(t0);
  EXPECT_EQ(s.bytes, 100u);
  EXPECT_LE(s.bytes, s.max_bytes);
  EXPECT_LE(s.snapshot_bytes, s.max_snapshot_bytes);
  EXPECT_EQ(s.snapshot_only_bytes, 100u);
  EXPECT_EQ(s.unique_payload_bytes, 200u);
  EXPECT_LE(s.unique_payload_bytes, s.max_unique_payload_bytes);
  EXPECT_LE(s.metadata_entries, s.max_metadata_entries);

  const auto result = finish_held_save();
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(read_file(result.path), std::string(40, 'a') + std::string(30, 'b') + std::string(30, 'c'));
  s = stats(t0);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_bytes + s.snapshot_only_bytes + s.snapshot_packets, 0u);
  EXPECT_EQ(s.unique_payload_bytes, s.bytes);
}

TEST_F(StreamRecorderReplaySaveTests, EmptyOrStaleSaveCreatesNothing) {
  EXPECT_EQ(save(t0).status, save_status_e::empty);
  EXPECT_FALSE(fs::exists(output_dir));

  push_run(t0);
  // Idle past the duration: the run has expired at the save boundary.
  EXPECT_EQ(save(t0 + 62s).status, save_status_e::empty);
  EXPECT_EQ(stream_recorder::detail::save_replay_at(t0 + 62s), "");
  EXPECT_FALSE(fs::exists(output_dir));

  publish(false, true);
  EXPECT_EQ(save(t0).status, save_status_e::disabled);
  EXPECT_FALSE(fs::exists(output_dir));

  const auto s = stats(t0);
  EXPECT_EQ(s.saves_completed + s.saves_failed, 0u);
  EXPECT_FALSE(s.save_in_progress);

  // Refusing a failed admission must also create nothing, after the real pointer snapshot
  // was copied but before its configuration/accounting is published.
  publish(true, true);
  push_run(t0);
  hooks.after_snapshot_payloads = [] { throw std::bad_alloc {}; };
  install();
  const auto failed_admission = save(t0);
  EXPECT_EQ(failed_admission.status, save_status_e::storage_error);
  EXPECT_TRUE(failed_admission.path.empty());
  EXPECT_FALSE(fs::exists(output_dir));
  const auto after_admission = stats(t0);
  EXPECT_FALSE(after_admission.save_in_progress);
  EXPECT_EQ(after_admission.snapshot_packets + after_admission.snapshot_bytes + after_admission.snapshot_only_bytes, 0u);
  EXPECT_EQ(after_admission.bytes, 7u);
  EXPECT_EQ(after_admission.unique_payload_bytes, 7u);
  hooks.after_snapshot_payloads = {};
  install();
  const auto recovered = save(t0);
  ASSERT_EQ(recovered.status, save_status_e::saved);
  EXPECT_EQ(read_file(recovered.path), "AAAABBB");
}

TEST_F(StreamRecorderReplaySaveTests, ExclusiveStagingNeverClobbersAnExistingFile) {
  fs::create_directories(output_dir);
  write_file(staging_path(0), "not ours");
  push_run(t0);

  const auto result = save(t0);
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(read_file(staging_path(0)), "not ours");
  EXPECT_EQ(read_file(result.path), "AAAABBB");
  // Its own staging name (the next one) is gone; the existing file stays.
  EXPECT_FALSE(fs::exists(staging_path(1)));
  EXPECT_EQ(names_in(output_dir), (std::vector<std::string> {staging_path(0).filename().string(),
                                                              final_path().filename().string()}));
}

TEST_F(StreamRecorderReplaySaveTests, FinalCollisionNeverOverwritesExistingOutput) {
  fs::create_directories(output_dir);
  write_file(final_path(), "earlier save");
  write_file(final_path("-1"), "another earlier save");
  push_run(t0);

  const auto result = save(t0);
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(result.path, final_path("-2").string());
  EXPECT_EQ(read_file(result.path), "AAAABBB");
  EXPECT_EQ(read_file(final_path()), "earlier save");
  EXPECT_EQ(read_file(final_path("-1")), "another earlier save");
  EXPECT_EQ(names_in(output_dir).size(), 3u);
}

TEST_F(StreamRecorderReplaySaveTests, ShortWritesAreCompleted) {
  int calls = 0;
  bool interrupted = false;
  hooks.write = [&](int fd, const std::uint8_t *data, std::size_t size) -> long long {
    ++calls;
    if (!interrupted) {
      interrupted = true;
      errno = EINTR;
      return -1;
    }
    // The real write, of at most two bytes at a time.
    return stream_recorder::detail::native_write(fd, data, std::min<std::size_t>(size, 2));
  };
  install();
  push_run(t0);

  const auto result = save(t0);
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_EQ(read_file(result.path), "AAAABBB");
  // One interrupted call, then 2+2 and 2+1 bytes.
  EXPECT_EQ(calls, 5);
}

TEST_F(StreamRecorderReplaySaveTests, WriteFailurePublishesNothingAndCleansStaging) {
  int calls = 0;
  hooks.write = [&](int fd, const std::uint8_t *data, std::size_t size) -> long long {
    if (++calls == 1) {
      return stream_recorder::detail::native_write(fd, data, std::min<std::size_t>(size, 2));
    }
    errno = EIO;
    return -1;
  };
  install();
  push_run(t0);
  expect_failed_cleanly(save(t0), "write");

  // An allocation exception after a real partial write must unwind owned staging,
  // release the snapshot and return failure without trying to allocate a new error.
  calls = 0;
  hooks.write = [&](int fd, const std::uint8_t *data, std::size_t size) -> long long {
    if (++calls == 1) {
      return stream_recorder::detail::native_write(fd, data, std::min<std::size_t>(size, 2));
    }
    throw std::bad_alloc {};
  };
  install();
  const auto exceptional = save(t0);
  EXPECT_EQ(exceptional.status, save_status_e::storage_error);
  EXPECT_TRUE(exceptional.path.empty());
  EXPECT_TRUE(names_in(output_dir).empty());
  const auto after_exception = stats(t0);
  EXPECT_EQ(after_exception.saves_failed, 2u);
  EXPECT_FALSE(after_exception.save_in_progress);
  EXPECT_EQ(after_exception.snapshot_bytes + after_exception.snapshot_only_bytes, 0u);

  hooks.write = {};
  install();
  const auto recovered = save(t0);
  ASSERT_EQ(recovered.status, save_status_e::saved);
  EXPECT_EQ(read_file(recovered.path), "AAAABBB");
}

TEST_F(StreamRecorderReplaySaveTests, SyncFailurePublishesNothingAndCleansStaging) {
  hooks.sync = [](int) {
    errno = EIO;
    return -1;
  };
  install();
  push_run(t0);
  expect_failed_cleanly(save(t0), "sync");
}

TEST_F(StreamRecorderReplaySaveTests, CloseFailurePublishesNothingAndCleansStaging) {
  int closed = 0;
  hooks.close = [&](int fd) {
    // The descriptor is really released; the close is reported as failed.
    closed += stream_recorder::detail::native_close(fd) == 0;
    errno = EIO;
    return -1;
  };
  install();
  push_run(t0);
  expect_failed_cleanly(save(t0), "close");
  EXPECT_EQ(closed, 1);
}

TEST_F(StreamRecorderReplaySaveTests, PublicationFailurePublishesNothingAndCleansStaging) {
  // As on a filesystem without hard links: publication fails closed, never by replacing.
  hooks.publish = [](const fs::path &, const fs::path &) {
    return std::make_error_code(std::errc::operation_not_permitted);
  };
  install();
  push_run(t0);
  expect_failed_cleanly(save(t0), "publish");
}

TEST_F(StreamRecorderReplaySaveTests, FailedSaveReleasesItsSnapshotAndGate) {
  hooks.sync = [](int) {
    errno = EIO;
    return -1;
  };
  push_run(t0);
  start_held_save(t0);
  ASSERT_TRUE(barrier.wait_entered());
  auto s = stats(t0);
  EXPECT_TRUE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_packets, 2u);

  const auto result = finish_held_save();
  EXPECT_EQ(result.status, save_status_e::storage_error);
  s = stats(t0);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_packets + s.snapshot_bytes + s.snapshot_only_bytes, 0u);
  EXPECT_EQ(s.saves_failed, 1u);
  // Ending the session drops the live run; nothing stays pinned by the failed save.
  stream_recorder::session_ended(k_session);
  EXPECT_EQ(stats(t0).unique_payload_bytes, 0u);

  // The gate is free: a new save is admitted (and refused only for having nothing to save).
  hooks = {};
  hooks.name_stamp = [] { return std::string {k_stamp}; };
  install();
  EXPECT_EQ(save(t0).status, save_status_e::empty);
  EXPECT_EQ(stats(t0).saves_refused_busy, 0u);
}

TEST_F(StreamRecorderReplaySaveTests, CompletedSaveReleasesItsSnapshotAndGate) {
  push_run(t0);
  start_held_save(t0);
  ASSERT_TRUE(barrier.wait_entered());
  EXPECT_EQ(stats(t0).snapshot_bytes, 7u);

  const auto result = finish_held_save();
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  auto s = stats(t0);
  EXPECT_FALSE(s.save_in_progress);
  EXPECT_EQ(s.snapshot_packets + s.snapshot_bytes + s.snapshot_only_bytes, 0u);
  EXPECT_EQ(s.saves_completed, 1u);
  stream_recorder::session_ended(k_session);
  EXPECT_EQ(stats(t0).unique_payload_bytes, 0u);

  // The gate is free for the next request.
  stream_recorder::session_started(k_session, true);
  push_run(t0);
  hooks.before_write = nullptr;
  install();
  const auto next = save(t0);
  EXPECT_EQ(next.status, save_status_e::saved) << next.error;
  EXPECT_EQ(next.path, final_path("-1").string());
  EXPECT_EQ(stats(t0).saves_refused_busy, 0u);
}

TEST_F(StreamRecorderReplaySaveTests, FilesArePrivateAndCompleteBeforePublication) {
  bool checked = false;
  hooks.before_publish = [&](const fs::path &staging, const fs::path &final_name) {
    checked = true;
    EXPECT_EQ(staging, staging_path(0));
    EXPECT_EQ(final_name, final_path());
    // Complete, synced and closed, under its private staging name, and not yet published.
    EXPECT_EQ(read_file(staging), "AAAABBB");
    EXPECT_FALSE(fs::exists(final_name));
#ifndef _WIN32
    EXPECT_EQ(fs::status(staging).permissions() & fs::perms::all, k_private);
#endif
  };
  install();
  push_run(t0);

  const auto result = save(t0);
  ASSERT_EQ(result.status, save_status_e::saved) << result.error;
  EXPECT_TRUE(checked);
  EXPECT_FALSE(fs::exists(staging_path(0)));
#ifndef _WIN32
  EXPECT_EQ(fs::status(result.path).permissions() & fs::perms::all, k_private);
#endif
}

TEST_F(StreamRecorderReplaySaveTests, OutputDirectoryFailureDoesNotEscapeOrPublish) {
  // A regular file where the output directory's parent should be.
  const auto blocker = owned_parent / "blocker";
  write_file(blocker, "keep");
  output_dir = blocker / "recordings";
  config::recording.output_dir = output_dir.string();
  stream_recorder::load_config();
  push_run(t0);

  const auto result = save(t0);
  EXPECT_EQ(result.status, save_status_e::storage_error);
  EXPECT_TRUE(result.path.empty());
  EXPECT_NE(result.error.find("output directory"), std::string::npos) << result.error;
  // Nothing was written anywhere else in the owned parent, and the blocker is untouched.
  EXPECT_EQ(names_in(owned_parent), std::vector<std::string> {"blocker"});
  EXPECT_EQ(read_file(blocker), "keep");
  EXPECT_EQ(stats(t0).saves_failed, 1u);
}
