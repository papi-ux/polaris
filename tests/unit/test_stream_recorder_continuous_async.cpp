/**
 * @file tests/unit/test_stream_recorder_continuous_async.cpp
 * @brief Test continuous recording's asynchronous storage in src/stream_recorder.*: the real
 * production push_packet() only copies an admitted packet into the file's fixed queue and never
 * waits for the real writer thread; capacity counts the packet being written; a packet that
 * does not fit is dropped and the file waits for a keyframe that fits; start, stop, shutdown,
 * failure and rollback keep one writer, one close and exact accounting.
 *
 * Bytes are synthetic raw video; nothing here claims a playable file, latency or durability.
 */

#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
  #include <fcntl.h>
  #include <unistd.h>
#endif

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  using stream_recorder::continuous_cause_e;
  using stream_recorder::continuous_error_e;
  using stream_recorder::continuous_state_e;

  constexpr std::uint64_t k_owner = 801;
  constexpr std::uint64_t k_other = 802;
  constexpr int k_hevc = 1;
  constexpr auto k_stamp = "20261009_024206";
  constexpr auto k_bounded = 10s;

  /// Set once; waited for with a bound.
  class event_t {
  public:
    void set() {
      {
        std::lock_guard lock {mutex_};
        set_ = true;
      }
      changed_.notify_all();
    }

    bool wait() {
      std::unique_lock lock {mutex_};
      return changed_.wait_for(lock, k_bounded, [this] { return set_; });
    }

    bool is_set() {
      std::lock_guard lock {mutex_};
      return set_;
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
  };

  /// Holds the threads that call hold() until release(); release is idempotent.
  class gate_t {
  public:
    void hold() {
      std::unique_lock lock {mutex_};
      ++entered_;
      changed_.notify_all();
      // Bounded, so a failed test cannot hang its writer forever; TearDown releases first.
      changed_.wait_for(lock, 3 * k_bounded, [this] { return released_; });
    }

    bool wait_entered(int count = 1) {
      std::unique_lock lock {mutex_};
      return changed_.wait_for(lock, k_bounded, [this, count] { return entered_ >= count; });
    }

    void release() {
      {
        std::lock_guard lock {mutex_};
        released_ = true;
      }
      changed_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int entered_ = 0;
    bool released_ = false;
  };

  /**
   * Each test starts from the recorder's process start state with recording disabled (default
   * off), one eligible HEVC owner, and a fixed name stamp, writing into a not-yet-created
   * directory inside a parent created exclusively. Every seam reads only fixture members, and
   * every helper thread is a fixture member. TearDown releases every gate, joins those threads,
   * then joins the writer and clears the seams (reset_replay_for_tests) before the members go.
   */
  class StreamRecorderContinuousAsyncTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_recording = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-continuous-async-"} + info->name() + "-" +
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
      publish(false, false);
      stream_recorder::session_started(k_owner, true, k_hevc);
      hooks.name_stamp = [] { return std::string {k_stamp}; };
      install();
    }

    void TearDown() override {
      write_gate.release();
      open_gate.release();
      threads.clear();  // Joins every helper thread.
      stream_recorder::detail::reset_replay_for_tests();
#ifndef _WIN32
      if (replacement_fd >= 0) {
        ::close(std::exchange(replacement_fd, -1));  // The fixture's own descriptor, closed once
      }
#endif
      config::recording = saved_recording;
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
      stream_recorder::detail::set_continuous_hooks_for_tests(hooks);
    }

    /// Run `work` on a fixture-owned thread; the event is set when it returns.
    std::shared_ptr<event_t> run(std::function<void()> work) {
      auto done = std::make_shared<event_t>();
      threads.emplace_back([done, work = std::move(work)] {
        work();
        done->set();
      });
      return done;
    }

    /// The real production entry, on a helper thread: true when it returned within the bound.
    bool push(std::uint64_t generation, bool keyframe, const std::string &bytes, int codec = k_hevc) {
      return run([generation, keyframe, bytes, codec] {
               stream_recorder::push_packet(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), true,
                                            keyframe, 0, generation, codec);
             })
        ->wait();
    }

    static bool drain() {
      return stream_recorder::detail::wait_continuous_drained_for_tests(k_bounded);
    }

    static stream_recorder::continuous_status_t continuous() {
      return stream_recorder::recorder_status().continuous;
    }

    static std::string read_file(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    std::vector<std::string> output_names() const {
      std::vector<std::string> names;
      std::error_code ec;
      for (fs::directory_iterator it {output_dir, ec}, end; !ec && it != end; it.increment(ec)) {
        names.push_back(it->path().filename().string());
      }
      std::sort(names.begin(), names.end());
      return names;
    }

    void note(std::string what) {
      std::lock_guard lock {events_mutex};
      events.push_back(std::move(what));
    }

    std::vector<std::string> noted() {
      std::lock_guard lock {events_mutex};
      return events;
    }

    /**
     * Every admitted packet is exactly one of written, discarded or queued. Call only once the
     * queue has drained: while a packet is being written, its accepted prefix is already in
     * bytes_written and the whole packet is still in queued_bytes, so the byte sum does not hold.
     */
    static void expect_accounted(const stream_recorder::continuous_status_t &s) {
      EXPECT_EQ(s.packets_admitted, s.packets_written + s.discarded_packets + s.queued_packets);
      EXPECT_EQ(s.bytes_admitted, s.bytes_written + s.discarded_bytes + s.queued_bytes);
    }

    static void expect_idle() {
      const auto s = stream_recorder::recorder_status();
      EXPECT_FALSE(s.recording);
      EXPECT_TRUE(s.file.empty());
      EXPECT_EQ(s.continuous.state, continuous_state_e::idle);
      EXPECT_EQ(s.continuous.packets_admitted, 0u);
      EXPECT_EQ(s.continuous.queued_packets, 0u);
      EXPECT_EQ(s.continuous.max_queue_bytes, 0u);
    }

    /// Write natively, after holding the first call on `write_gate`.
    void hold_first_write() {
      hooks.write = [this](int fd, const std::uint8_t *data, std::size_t size) -> long long {
        if (writes.fetch_add(1) == 0) {
          write_gate.hold();
        }
        note("write:" + std::string(reinterpret_cast<const char *>(data), size));
        return stream_recorder::detail::native_write(fd, data, size);
      };
    }

    config::recording_t saved_recording;
    fs::path owned_parent;
    fs::path output_dir;
    stream_recorder::detail::continuous_hooks_t hooks;
    // State the seams read or write: fixture members, declared before the threads that use them.
    gate_t write_gate;
    gate_t open_gate;
    event_t worker_started_event;
    event_t failure_published;
    event_t stop_joining;
    event_t stop_entered;
    std::atomic<int> writes {0};
    std::atomic<int> started {0};
    std::atomic<int> exited {0};
    std::atomic<int> closes {0};
    std::atomic<bool> publish_armed {false};
    std::atomic<int> rollback_closes {0};
    int replacement_fd = -1;  ///< A fixture-owned file placed on a rolled-back descriptor number
    int rolled_back_fd = -1;
    std::mutex events_mutex;
    std::vector<std::string> events;
    std::vector<std::size_t> write_sizes;
    std::vector<std::jthread> threads;  // Last: joined (in TearDown) before anything above goes.
  };
}  // namespace

TEST_F(StreamRecorderContinuousAsyncTests, HeldStorageNeverBlocksProductionPushStatusOrReplay) {
  publish(true, true);  // Replay is buffered from the same pushes.
  hold_first_write();
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;

  ASSERT_TRUE(push(k_owner, true, "KKKK"));
  ASSERT_TRUE(write_gate.wait_entered());
  // The writer is inside write(); production pushes and status still return.
  ASSERT_TRUE(push(k_owner, false, "ppp"));
  auto s = continuous();
  EXPECT_EQ(s.state, continuous_state_e::writing);
  EXPECT_EQ(s.packets_admitted, 2u);
  EXPECT_EQ(s.bytes_admitted, 7u);
  EXPECT_EQ(s.queued_packets, 2u);  // The packet being written still counts
  EXPECT_EQ(s.queued_bytes, 7u);
  EXPECT_EQ(s.packets_written, 0u);
  EXPECT_EQ(s.max_queue_bytes, stream_recorder::k_continuous_queue_bytes);
  EXPECT_EQ(s.max_queue_packets, stream_recorder::k_continuous_queue_slots);
  EXPECT_EQ(stream_recorder::replay_stats().packets, 2u);
  EXPECT_EQ(read_file(path), "");

  write_gate.release();
  ASSERT_TRUE(drain());
  s = continuous();
  EXPECT_EQ(s.packets_written, 2u);
  EXPECT_EQ(s.bytes_written, 7u);
  EXPECT_EQ(s.queued_packets, 0u);
  expect_accounted(s);
  EXPECT_EQ(read_file(path), "KKKKppp");
}

TEST_F(StreamRecorderContinuousAsyncTests, TheByteCapCountsThePacketBeingWritten) {
  stream_recorder::detail::set_continuous_limits_for_tests(10, 8);
  hold_first_write();
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;

  ASSERT_TRUE(push(k_owner, true, "AAAAAA"));  // 6 bytes, held in write()
  ASSERT_TRUE(write_gate.wait_entered());
  ASSERT_TRUE(push(k_owner, false, "bbbbb"));  // 6 + 5 > 10: dropped, unread
  auto s = continuous();
  EXPECT_EQ(s.dropped_backpressure, 1u);
  EXPECT_EQ(s.state, continuous_state_e::awaiting_keyframe);
  EXPECT_EQ(s.cause, continuous_cause_e::backpressure);
  EXPECT_EQ(s.discontinuities, 1u);
  ASSERT_TRUE(push(k_owner, false, "cccc"));   // Fits, but depends on the dropped frame
  s = continuous();
  EXPECT_EQ(s.refused_awaiting_keyframe, 1u);
  EXPECT_EQ(s.packets_admitted, 1u);
  ASSERT_TRUE(push(k_owner, true, "DDDD"));    // A keyframe that fits: 6 + 4 == 10
  s = continuous();
  EXPECT_EQ(s.packets_admitted, 2u);
  EXPECT_EQ(s.queued_bytes, 10u);
  EXPECT_EQ(s.state, continuous_state_e::writing);

  write_gate.release();
  ASSERT_TRUE(drain());
  expect_accounted(continuous());
  EXPECT_EQ(read_file(path), "AAAAAADDDD");
}

TEST_F(StreamRecorderContinuousAsyncTests, TheSlotCapCountsThePacketBeingWritten) {
  stream_recorder::detail::set_continuous_limits_for_tests(64, 2);
  hold_first_write();
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;

  ASSERT_TRUE(push(k_owner, true, "A"));
  ASSERT_TRUE(write_gate.wait_entered());
  ASSERT_TRUE(push(k_owner, false, "b"));
  EXPECT_EQ(continuous().queued_packets, 2u);
  ASSERT_TRUE(push(k_owner, false, "c"));  // No slot left
  ASSERT_TRUE(push(k_owner, false, "d"));  // Depends on the dropped frame
  auto s = continuous();
  EXPECT_EQ(s.dropped_backpressure, 1u);
  EXPECT_EQ(s.refused_awaiting_keyframe, 1u);
  EXPECT_EQ(s.packets_admitted, 2u);

  write_gate.release();
  ASSERT_TRUE(drain());
  ASSERT_TRUE(push(k_owner, true, "E"));
  ASSERT_TRUE(drain());
  expect_accounted(continuous());
  EXPECT_EQ(read_file(path), "AbE");
}

TEST_F(StreamRecorderContinuousAsyncTests, APacketThatWrapsIsWrittenInTwoExactSegments) {
  stream_recorder::detail::set_continuous_limits_for_tests(8, 4);
  hooks.write = [this](int fd, const std::uint8_t *data, std::size_t size) -> long long {
    {
      std::lock_guard lock {events_mutex};
      write_sizes.push_back(size);
    }
    return stream_recorder::detail::native_write(fd, data, size);
  };
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;

  ASSERT_TRUE(push(k_owner, true, "AAAAA"));
  ASSERT_TRUE(drain());
  ASSERT_TRUE(push(k_owner, false, "BBBBB"));  // Starts at ring offset 5 of 8: 3 + 2
  ASSERT_TRUE(drain());
  {
    std::lock_guard lock {events_mutex};
    EXPECT_EQ(write_sizes, (std::vector<std::size_t> {5, 3, 2}));
  }
  EXPECT_EQ(read_file(path), "AAAAABBBBB");
  const auto s = continuous();
  EXPECT_EQ(s.packets_written, 2u);
  EXPECT_EQ(s.queued_bytes, 0u);
  expect_accounted(s);
}

TEST_F(StreamRecorderContinuousAsyncTests, APacketLargerThanTheQueueDropsAndAFittingKeyframeRecovers) {
  stream_recorder::detail::set_continuous_limits_for_tests(8, 4);
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;

  ASSERT_TRUE(push(k_owner, true, std::string(9, 'X')));
  auto s = continuous();
  EXPECT_EQ(s.dropped_backpressure, 1u);
  EXPECT_EQ(s.refused_invalid, 0u);  // Within the packet cap: a queue drop, not invalid input
  EXPECT_EQ(s.cause, continuous_cause_e::backpressure);
  ASSERT_TRUE(push(k_owner, false, "pp"));
  ASSERT_TRUE(push(k_owner, true, "KKK"));
  ASSERT_TRUE(drain());
  s = continuous();
  EXPECT_EQ(s.refused_awaiting_keyframe, 1u);
  EXPECT_EQ(s.packets_written, 1u);
  EXPECT_EQ(read_file(path), "KKK");
}

TEST_F(StreamRecorderContinuousAsyncTests, StartRollsBackAtEveryOwnershipBoundary) {
  hooks.worker_started = [this] { ++started; };
  hooks.worker_exited = [this] { ++exited; };

  // Before the queue is allocated: nothing exists.
  hooks.before_block_allocation = [] { throw std::bad_alloc {}; };
  install();
  stream_recorder::start_recording();
  expect_idle();
  EXPECT_TRUE(output_names().empty());

  // The file exists, no writer thread yet: the file goes, no thread ever ran.
  hooks.before_block_allocation = {};
  hooks.before_launch = [] { throw std::system_error {std::make_error_code(std::errc::resource_unavailable_try_again)}; };
  install();
  stream_recorder::start_recording();
  expect_idle();
  EXPECT_TRUE(output_names().empty());
  EXPECT_EQ(started.load(), 0);

  // The writer runs, nothing is committed: it is woken and joined, then the file goes once.
  hooks.before_launch = {};
  hooks.after_launch = [] { throw std::bad_alloc {}; };
  install();
  stream_recorder::start_recording();
  expect_idle();
  EXPECT_TRUE(output_names().empty());
  EXPECT_EQ(started.load(), 1);  // The writer really ran
  EXPECT_EQ(exited.load(), 1);   // ... and was joined before start returned

  // A later start succeeds under the first name, and its descriptor (likely the reused number)
  // is never closed by anything left over: the write succeeds and the file stays writing.
  hooks.after_launch = {};
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;
  EXPECT_EQ(fs::path {path}.filename(), std::string {"polaris_"} + k_stamp + ".hevc");
  ASSERT_TRUE(push(k_owner, true, "K"));
  ASSERT_TRUE(drain());
  const auto s = continuous();
  EXPECT_EQ(s.state, continuous_state_e::writing);
  EXPECT_EQ(s.error, continuous_error_e::none);
  EXPECT_EQ(read_file(path), "K");
  stream_recorder::stop_recording();
  EXPECT_EQ(exited.load(), started.load());
}

TEST_F(StreamRecorderContinuousAsyncTests, StopDrainsEveryAdmittedPacketThenClosesOnce) {
  hold_first_write();
  hooks.before_stop_join = [this] { stop_joining.set(); };
  hooks.close = [this](int fd) {
    ++closes;
    note("close");
    return stream_recorder::detail::native_close(fd);
  };
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;
  ASSERT_TRUE(push(k_owner, true, "A"));
  ASSERT_TRUE(write_gate.wait_entered());
  ASSERT_TRUE(push(k_owner, false, "b"));
  ASSERT_TRUE(push(k_owner, false, "c"));

  const auto stopped = run([] { stream_recorder::stop_recording(); });
  ASSERT_TRUE(stop_joining.wait());
  // Still the open file while it drains, and it takes nothing more.
  auto status = stream_recorder::recorder_status();
  EXPECT_TRUE(status.recording);
  EXPECT_EQ(status.file, path);
  EXPECT_EQ(status.continuous.state, continuous_state_e::stopping);
  ASSERT_TRUE(push(k_owner, true, "X"));
  EXPECT_EQ(continuous().packets_admitted, 3u);
  EXPECT_FALSE(stopped->is_set());

  write_gate.release();
  ASSERT_TRUE(stopped->wait());
  EXPECT_EQ(closes.load(), 1);
  EXPECT_EQ(noted(), (std::vector<std::string> {"write:A", "write:b", "write:c", "close"}));
  expect_idle();
  EXPECT_EQ(read_file(path), "Abc");
}

TEST_F(StreamRecorderContinuousAsyncTests, AWriteFailureDiscardsQueuedPacketsWithExactAccounting) {
  hooks.write = [this](int fd, const std::uint8_t *data, std::size_t size) -> long long {
    switch (writes.fetch_add(1)) {
      case 0:
        write_gate.hold();
        return stream_recorder::detail::native_write(fd, data, std::min<std::size_t>(size, 2));
      default:
        errno = EIO;
        return -1;
    }
  };
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;
  ASSERT_TRUE(push(k_owner, true, "ABCD"));
  ASSERT_TRUE(write_gate.wait_entered());
  ASSERT_TRUE(push(k_owner, false, "efg"));
  ASSERT_TRUE(push(k_owner, false, "hi"));
  // Before the failure is published, no part of it is visible.
  auto s = continuous();
  EXPECT_EQ(s.state, continuous_state_e::writing);
  EXPECT_EQ(s.error, continuous_error_e::none);
  EXPECT_EQ(s.discarded_packets, 0u);

  write_gate.release();
  ASSERT_TRUE(drain());
  s = continuous();
  EXPECT_EQ(s.state, continuous_state_e::failed);
  EXPECT_EQ(s.cause, continuous_cause_e::storage);
  EXPECT_EQ(s.error, continuous_error_e::write);
  EXPECT_EQ(s.error_number, EIO);
  EXPECT_EQ(s.packets_admitted, 3u);
  EXPECT_EQ(s.bytes_admitted, 9u);
  EXPECT_EQ(s.packets_written, 0u);
  EXPECT_EQ(s.bytes_written, 2u);       // The torn prefix
  EXPECT_EQ(s.discarded_packets, 3u);   // The failed packet and both queued behind it
  EXPECT_EQ(s.discarded_bytes, 7u);     // 2 + 3 + 2 never written
  expect_accounted(s);
  EXPECT_EQ(writes.load(), 2);          // The queued packets were never attempted

  // Failure outranks later lifecycle, and the file takes nothing more until stop.
  stream_recorder::session_started(k_other, true, k_hevc);
  ASSERT_TRUE(push(k_owner, true, "Z"));
  s = continuous();
  EXPECT_EQ(s.state, continuous_state_e::failed);
  EXPECT_EQ(s.cause, continuous_cause_e::storage);
  EXPECT_EQ(s.packets_admitted, 3u);
  EXPECT_TRUE(stream_recorder::is_recording());
  stream_recorder::stop_recording();
  expect_idle();
  EXPECT_EQ(read_file(path), "AB");
}

TEST_F(StreamRecorderContinuousAsyncTests, AFailureRacingTheCopyLeavesThatPacketUnpublished) {
  hooks.write = [this](int, const std::uint8_t *, std::size_t) -> long long {
    write_gate.hold();
    errno = EIO;
    return -1;
  };
  hooks.after_worker_failure = [this] { failure_published.set(); };
  // In the production push, after the copy and before publication: make the file fail now.
  hooks.before_publish = [this] {
    if (publish_armed.exchange(false)) {
      write_gate.release();
      EXPECT_TRUE(failure_published.wait());
    }
  };
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;
  ASSERT_TRUE(push(k_owner, true, "A"));
  ASSERT_TRUE(write_gate.wait_entered());

  publish_armed = true;
  ASSERT_TRUE(push(k_owner, false, "b"));
  EXPECT_TRUE(failure_published.is_set());
  ASSERT_TRUE(drain());
  const auto s = continuous();
  EXPECT_EQ(s.state, continuous_state_e::failed);
  EXPECT_EQ(s.packets_admitted, 1u);  // "b" was copied but never admitted
  EXPECT_EQ(s.bytes_admitted, 1u);
  EXPECT_EQ(s.discarded_packets, 1u);
  EXPECT_EQ(s.queued_packets, 0u);
  expect_accounted(s);
  EXPECT_EQ(read_file(path), "");
}

TEST_F(StreamRecorderContinuousAsyncTests, AdmittedBytesSurviveLaterLifecycleAndModeChanges) {
  hold_first_write();
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;
  ASSERT_TRUE(push(k_owner, true, "A"));
  ASSERT_TRUE(write_gate.wait_entered());
  ASSERT_TRUE(push(k_owner, false, "b"));

  stream_recorder::session_started(k_other, true, k_hevc);
  publish(true, true);
  publish(false, false);
  stream_recorder::session_ended(k_owner);
  ASSERT_TRUE(push(k_owner, false, "c"));  // Retired: never admitted
  ASSERT_TRUE(push(k_other, true, "D"));   // Never the owner

  write_gate.release();
  ASSERT_TRUE(drain());
  const auto s = continuous();
  EXPECT_EQ(s.packets_admitted, 2u);
  EXPECT_EQ(s.packets_written, 2u);
  EXPECT_EQ(s.state, continuous_state_e::suspended);
  EXPECT_EQ(s.cause, continuous_cause_e::owner_ended);
  expect_accounted(s);
  EXPECT_EQ(read_file(path), "Ab");
}

TEST_F(StreamRecorderContinuousAsyncTests, AHeldOpenLetsPushAndStatusProceedAndSerializesStop) {
  hooks.open = [this](const fs::path &path) {
    open_gate.hold();
    return stream_recorder::detail::native_open_exclusive(path);
  };
  hooks.stop_entered = [this] { stop_entered.set(); };
  install();

  const auto started_recording = run([] { stream_recorder::start_recording(); });
  ASSERT_TRUE(open_gate.wait_entered());
  // Opening holds neither the packet path nor status.
  const auto status = stream_recorder::recorder_status();
  EXPECT_FALSE(status.recording);
  EXPECT_EQ(status.continuous.state, continuous_state_e::idle);
  EXPECT_TRUE(push(k_owner, true, "K"));

  // A stop issued during the open waits for that start, then stops what it opened.
  const auto stopped = run([] { stream_recorder::stop_recording(); });
  ASSERT_TRUE(stop_entered.wait());
  EXPECT_FALSE(stopped->is_set());
  open_gate.release();
  ASSERT_TRUE(started_recording->wait());
  ASSERT_TRUE(stopped->wait());
  expect_idle();
  EXPECT_EQ(output_names(), (std::vector<std::string> {std::string {"polaris_"} + k_stamp + ".hevc"}));
}

TEST_F(StreamRecorderContinuousAsyncTests, ShutdownStopsTheFileAndFencesEveryLaterStart) {
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;
  ASSERT_TRUE(push(k_owner, true, "K"));
  stream_recorder::shutdown();  // Drains, joins, closes
  expect_idle();
  EXPECT_EQ(read_file(path), "K");

  stream_recorder::start_recording();
  expect_idle();
  EXPECT_EQ(output_names(), (std::vector<std::string> {std::string {"polaris_"} + k_stamp + ".hevc"}));
  stream_recorder::shutdown();  // Repeating it changes nothing
  expect_idle();
}

TEST_F(StreamRecorderContinuousAsyncTests, TheFallbackOwnerWakesAndJoinsAnIdleWriter) {
  hooks.worker_started = [this] {
    ++started;
    worker_started_event.set();
  };
  hooks.worker_exited = [this] { ++exited; };
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;
  ASSERT_TRUE(worker_started_event.wait());  // Idle: waiting for work, never told to stop

  // The process-exit owner signals stop and wakes the writer before joining, or this would hang.
  EXPECT_TRUE(stream_recorder::detail::finish_continuous_writer_for_tests());
  EXPECT_EQ(exited.load(), 1);
  EXPECT_FALSE(stream_recorder::detail::finish_continuous_writer_for_tests());

  // The stopped writer takes nothing; stop still closes the file it left open.
  ASSERT_TRUE(push(k_owner, true, "K"));
  EXPECT_EQ(continuous().packets_admitted, 0u);
  EXPECT_TRUE(stream_recorder::is_recording());
  stream_recorder::stop_recording();
  expect_idle();
  EXPECT_EQ(read_file(path), "");
  EXPECT_EQ(started.load(), 1);
}

TEST_F(StreamRecorderContinuousAsyncTests, OnlyTheDiagnosticEntryWaitsAndOnlyForItsOwnPacket) {
  hold_first_write();
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;
  const std::string first {"K"};

  // Diagnostic compatibility entry, not production: it waits for the writer to finish its packet.
  const auto diagnostic = run([first] {
    stream_recorder::detail::push_packet_at(reinterpret_cast<const std::uint8_t *>(first.data()), first.size(), true, true,
                                            0, k_owner, k_hevc, std::chrono::steady_clock::now());
  });
  ASSERT_TRUE(write_gate.wait_entered());
  ASSERT_TRUE(push(k_owner, false, "p"));  // Production returns meanwhile
  EXPECT_FALSE(diagnostic->is_set());
  // A diagnostic push that admits nothing does not wait.
  const std::string foreign {"F"};
  const auto unadmitted = run([foreign] {
    stream_recorder::detail::push_packet_at(reinterpret_cast<const std::uint8_t *>(foreign.data()), foreign.size(), true,
                                            true, 0, k_other, k_hevc, std::chrono::steady_clock::now());
  });
  EXPECT_TRUE(unadmitted->wait());

  write_gate.release();
  ASSERT_TRUE(diagnostic->wait());
  ASSERT_TRUE(drain());
  EXPECT_EQ(read_file(path), "Kp");
}

TEST_F(StreamRecorderContinuousAsyncTests, AcceptedBytesArePublishedWhileTheWholePacketStaysQueued) {
  hooks.write = [this](int fd, const std::uint8_t *data, std::size_t size) -> long long {
    if (writes.fetch_add(1) == 0) {
      return stream_recorder::detail::native_write(fd, data, std::min<std::size_t>(size, 3));  // A prefix
    }
    write_gate.hold();
    errno = EIO;
    return -1;
  };
  install();
  stream_recorder::start_recording();
  const auto path = stream_recorder::recorder_status().file;
  ASSERT_TRUE(push(k_owner, true, "ABCDEFGH"));
  ASSERT_TRUE(write_gate.wait_entered());  // The second write() call, after a 3-byte prefix

  // In flight: the accepted prefix is reported, and the whole packet still holds its place.
  // (The byte sum of expect_accounted() does not hold here, by design.)
  auto s = continuous();
  EXPECT_EQ(s.bytes_written, 3u);
  EXPECT_EQ(s.queued_bytes, 8u);
  EXPECT_EQ(s.queued_packets, 1u);
  EXPECT_EQ(s.packets_written, 0u);
  EXPECT_EQ(s.discarded_bytes, 0u);
  EXPECT_EQ(s.state, continuous_state_e::writing);
  EXPECT_EQ(s.error, continuous_error_e::none);

  write_gate.release();
  ASSERT_TRUE(drain());
  s = continuous();
  EXPECT_EQ(s.state, continuous_state_e::failed);
  EXPECT_EQ(s.error_number, EIO);
  EXPECT_EQ(s.bytes_written, 3u);   // Counted once: no second count at completion
  EXPECT_EQ(s.discarded_packets, 1u);
  EXPECT_EQ(s.discarded_bytes, 5u);
  EXPECT_EQ(s.queued_bytes, 0u);
  expect_accounted(s);
  EXPECT_EQ(read_file(path), "ABC");
}

#ifndef _WIN32
TEST_F(StreamRecorderContinuousAsyncTests, ARolledBackStartClosesItsDescriptorExactlyOnce) {
  // The seam closes the real descriptor, then puts a fixture-owned file on the same number: a
  // second close of that number by the rollback would close the fixture's file instead.
  const auto replacement_path = owned_parent / "replacement";
  hooks.rollback_close = [this, replacement_path](int fd) {
    ++rollback_closes;
    rolled_back_fd = fd;
    ::close(fd);
    int replacement = ::open(replacement_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (replacement >= 0 && replacement != fd) {
      if (::dup2(replacement, fd) == fd) {
        ::close(replacement);
        replacement = fd;
      }
    }
    replacement_fd = replacement;
  };
  struct boundary_t {
    const char *name;
    std::function<void()> before_launch;
    std::function<void()> after_launch;
  };
  const boundary_t boundaries[] {
    {"before launch", [] { throw std::bad_alloc {}; }, {}},
    {"after launch", {}, [] { throw std::bad_alloc {}; }},
  };
  for (const auto &boundary : boundaries) {
    SCOPED_TRACE(boundary.name);
    rollback_closes = 0;
    hooks.before_launch = boundary.before_launch;
    hooks.after_launch = boundary.after_launch;
    install();
    stream_recorder::start_recording();
    expect_idle();
    EXPECT_EQ(rollback_closes.load(), 1);
    EXPECT_TRUE(output_names().empty());  // The original file was removed
    // After start returned, the descriptor number still belongs to the fixture's file.
    ASSERT_GE(replacement_fd, 0);
    ASSERT_EQ(replacement_fd, rolled_back_fd);  // Prove the replacement uses the closed number.
    EXPECT_NE(::fcntl(replacement_fd, F_GETFD), -1);
    EXPECT_EQ(::write(replacement_fd, "ok", 2), 2);
    EXPECT_EQ(read_file(replacement_path), "ok");
    ::close(std::exchange(replacement_fd, -1));
  }
}
#endif
