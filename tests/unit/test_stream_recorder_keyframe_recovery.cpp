/**
 * @file tests/unit/test_stream_recorder_keyframe_recovery.cpp
 * @brief Test the continuous recorder's keyframe recovery core in src/stream_recorder.*: only a
 * bound, unfenced, sole eligible owner with a registered channel is asked for a keyframe, one
 * reservation at a time, at most once per interval, never into a failed, stopping or full
 * writer, and the request is raised through the session's real mail event after the recorder
 * lock is released. Stop, shutdown, owner end, overlap and codec changes fence a paused request.
 *
 * Every request travels through a real safe::mail_raw_t idr event, observed only through that
 * event. Seams only pause or observe. The interval and capacity thresholds are proposals: nothing
 * here measures an encoder, IDR cost, latency or a playable file.
 */

#include <src/config.h>
#include <src/globals.h>
#include <src/stream_recorder.h>
#include <src/thread_safe.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace stream_recorder::detail {
  // Readable failure messages for tokens.
  inline void PrintTo(const keyframe_token_t &token, std::ostream *os) {
    *os << "{incarnation " << token.incarnation << ", generation " << token.generation << ", sequence "
        << token.sequence << "}";
  }
}  // namespace stream_recorder::detail

namespace {
  namespace fs = std::filesystem;
  using namespace std::chrono_literals;
  using clock_type = std::chrono::steady_clock;
  using stream_recorder::continuous_cause_e;
  using stream_recorder::continuous_error_e;
  using stream_recorder::continuous_state_e;
  using stream_recorder::keyframe_outcome_e;
  using token_t = stream_recorder::detail::keyframe_token_t;

  constexpr std::uint64_t k_owner = 901;
  constexpr std::uint64_t k_other = 902;
  constexpr std::uint64_t k_watcher = 903;
  constexpr std::uint64_t k_unknown = 999;
  constexpr int k_hevc = 1;
  constexpr int k_av1 = 2;
  constexpr auto k_stamp = "20261009_074743";
  constexpr auto k_bounded = 10s;
  constexpr auto k_max = std::numeric_limits<std::uint64_t>::max();

  /// Set once; waited for with a bound.
  class signal_t {
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

  /// Holds the threads that call hold() until release(); release is idempotent and may come first.
  class gate_t {
  public:
    void hold() {
      std::unique_lock lock {mutex_};
      ++entered_;
      changed_.notify_all();
      // Bounded, so a failed test cannot hang a helper forever; TearDown releases first.
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
   * Observes every dispatch through the keyframe seams. While holding, each dispatch pauses
   * before it reaches the request gate, on a gate of its own sequence, so two reservations can
   * be held and released independently.
   */
  class dispatches_t {
  public:
    void enter(const token_t &token) {
      std::shared_ptr<gate_t> gate;
      {
        std::lock_guard lock {mutex_};
        entered_.push_back(token);
        if (holding_) {
          gate = gate_locked(token.sequence);
        }
      }
      changed_.notify_all();
      if (gate) {
        gate->hold();
      }
    }

    void finish(const token_t &token, keyframe_outcome_e outcome) {
      {
        std::lock_guard lock {mutex_};
        finished_.emplace_back(token, outcome);
      }
      changed_.notify_all();
    }

    void hold(bool holding) {
      std::lock_guard lock {mutex_};
      holding_ = holding;
    }

    /// The `count`-th dispatch to enter (1-based), once it has entered.
    std::optional<token_t> wait_entered(std::size_t count) {
      std::unique_lock lock {mutex_};
      if (!changed_.wait_for(lock, k_bounded, [this, count] { return entered_.size() >= count; })) {
        return std::nullopt;
      }
      return entered_[count - 1];
    }

    /// How the dispatch of `sequence` ended, once it has.
    std::optional<keyframe_outcome_e> wait_finished(std::uint64_t sequence) {
      std::unique_lock lock {mutex_};
      std::optional<keyframe_outcome_e> outcome;
      changed_.wait_for(lock, k_bounded, [this, sequence, &outcome] {
        for (const auto &[token, result] : finished_) {
          if (token.sequence == sequence) {
            outcome = result;
            return true;
          }
        }
        return false;
      });
      return outcome;
    }

    void release(std::uint64_t sequence) {
      std::shared_ptr<gate_t> gate;
      {
        std::lock_guard lock {mutex_};
        gate = gate_locked(sequence);
      }
      gate->release();
    }

    void release_all() {
      std::vector<std::shared_ptr<gate_t>> gates;
      {
        std::lock_guard lock {mutex_};
        holding_ = false;
        for (auto &[sequence, gate] : gates_) {
          gates.push_back(gate);
        }
      }
      for (auto &gate : gates) {
        gate->release();
      }
    }

    std::vector<std::pair<token_t, keyframe_outcome_e>> finished() {
      std::lock_guard lock {mutex_};
      return finished_;
    }

  private:
    std::shared_ptr<gate_t> gate_locked(std::uint64_t sequence) {
      auto &gate = gates_[sequence];
      if (!gate) {
        gate = std::make_shared<gate_t>();
      }
      return gate;
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    bool holding_ = false;
    std::map<std::uint64_t, std::shared_ptr<gate_t>> gates_;
    std::vector<token_t> entered_;
    std::vector<std::pair<token_t, keyframe_outcome_e>> finished_;
  };

  /// One session's real mail, its real idr event, and a channel that holds the event weakly.
  struct idr_t {
    std::shared_ptr<safe::mail_raw_t> box;
    safe::mail_raw_t::event_t<bool> event;
    std::shared_ptr<stream_recorder::keyframe_channel_t> channel;
  };

  /// State a helper thread shares with a seam; owned jointly so neither can outlive it.
  struct request_probe_t {
    signal_t raise_entered;
    signal_t helper_done;
    std::atomic<bool> popped {false};
    std::atomic<bool> locked {false};
    std::atomic<bool> status_returned {false};
    std::atomic<bool> raise_saw_helper {false};
  };

#define CHILD_CHECK(condition, code) \
  do { \
    if (!(condition)) { \
      std::fprintf(stderr, "child check %d failed: %s\n", (code), #condition); \
      return (code); \
    } \
  } while (false)

  /**
   * Each test starts from the recorder's process start state with recording disabled (default
   * off), no registered session, a fixed name stamp, and a not-yet-created output directory
   * inside a parent created exclusively. Explicit packet times start an hour ahead of the real
   * clock, so a lifecycle trigger (which uses the real clock) never passes a rate anchor set by
   * a packet. Every seam reads only fixture members, and every helper thread is a fixture
   * member. TearDown releases every gate, stops every fixture event (waking a helper blocked in
   * its real pop()), joins those threads, then joins the writer and clears the seams
   * (reset_replay_for_tests) before the members go.
   */
  class StreamRecorderKeyframeRecoveryTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_recording = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base_dir = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base_dir / (std::string {"polaris-keyframe-recovery-"} + info->name() + "-" +
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
      config::recording.enabled = false;
      config::recording.replay_buffer = false;
      stream_recorder::load_config();
      hooks.name_stamp = [] { return std::string {k_stamp}; };
      install();
      keyframe_hooks.before_dispatch = [this](const token_t &token) {
        dispatches.enter(token);
      };
      keyframe_hooks.after_dispatch = [this](const token_t &token, keyframe_outcome_e outcome) {
        dispatches.finish(token, outcome);
      };
      install_keyframe_hooks();
      base = clock_type::now() + 1h;
    }

    void TearDown() override {
      release_everything();
      threads.clear();  // Joins every helper thread.
      stream_recorder::detail::reset_replay_for_tests();
      config::recording = saved_recording;
      stream_recorder::load_config();
      if (!owned_parent.empty()) {
        std::error_code ec;
        fs::remove_all(owned_parent, ec);
      }
    }

    void release_everything() {
      write_gate.release();
      dispatches.release_all();
      for (auto &idr : idrs) {
        if (idr->event) {
          idr->event->stop();
        }
      }
    }

    /// The end of a death-test child: the same cleanup as TearDown, then a checked exit.
    [[noreturn]] void finish_child(int code) {
      release_everything();
      threads.clear();
      stream_recorder::detail::reset_replay_for_tests();
      std::error_code ec;
      fs::remove_all(owned_parent, ec);
      std::fflush(stderr);
      std::_Exit(code);
    }

    void install() {
      stream_recorder::detail::set_continuous_hooks_for_tests(hooks);
    }

    void install_keyframe_hooks() {
      stream_recorder::detail::set_keyframe_hooks_for_tests(keyframe_hooks);
    }

    std::shared_ptr<idr_t> make_idr() {
      auto idr = std::make_shared<idr_t>();
      idr->box = std::make_shared<safe::mail_raw_t>();
      idr->event = idr->box->event<bool>(::mail::idr);
      idr->channel = std::make_shared<stream_recorder::keyframe_channel_t>(idr->event);
      idrs.push_back(idr);
      return idr;
    }

    /// Run `work` on a fixture-owned thread; the signal is set when it returns.
    std::shared_ptr<signal_t> run(std::function<void()> work) {
      auto done = std::make_shared<signal_t>();
      threads.emplace_back([done, work = std::move(work)] {
        work();
        done->set();
      });
      return done;
    }

    /// The diagnostic entry at an explicit time (waits, bounded, for an admitted packet's write).
    static void packet_at(std::uint64_t generation, bool keyframe, const std::string &bytes, clock_type::time_point at,
                          int codec = k_hevc) {
      stream_recorder::detail::push_packet_at(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), true,
                                              keyframe, 0, generation, codec, at);
    }

    /// An owner packet with no data: a discontinuity, never admitted.
    static void invalid_at(std::uint64_t generation, clock_type::time_point at) {
      stream_recorder::detail::push_packet_at(nullptr, 0, true, false, 0, generation, k_hevc, at);
    }

    /// The real production entry on a helper thread: true when it returned within the bound.
    bool push(std::uint64_t generation, bool keyframe, const std::string &bytes) {
      return run([generation, keyframe, bytes] {
               stream_recorder::push_packet(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), true,
                                            keyframe, 0, generation, k_hevc);
             })
        ->wait();
    }

    /// A production packet with no data, on a helper thread.
    bool push_invalid(std::uint64_t generation) {
      return run([generation] {
               stream_recorder::push_packet(nullptr, 0, true, false, 0, generation, k_hevc);
             })
        ->wait();
    }

    static bool drain() {
      return stream_recorder::detail::wait_continuous_drained_for_tests(k_bounded);
    }

    static stream_recorder::continuous_status_t continuous() {
      return stream_recorder::recorder_status().continuous;
    }

    static stream_recorder::detail::keyframe_stats_t stats() {
      return stream_recorder::detail::keyframe_stats_for_tests();
    }

    /**
     * Register the owner with an unknown codec and start with no owner bound (the file's codec
     * comes from the active stream format), so the start itself requests nothing. The owner then
     * binds at its first keyframe through the unchanged admission rule, unless `bind_at` is empty.
     */
    void start_unbound(const std::shared_ptr<stream_recorder::keyframe_channel_t> &channel) {
      stream_recorder::set_active_video_format(k_hevc);
      stream_recorder::session_started(k_owner, true, -1, channel);
      stream_recorder::start_recording();
      ASSERT_TRUE(stream_recorder::is_recording());
      const auto s = continuous();
      ASSERT_EQ(s.owner_generation, 0u);
      ASSERT_EQ(s.file_video_format, k_hevc);
      ASSERT_EQ(stats().reservations, 0u);
    }

    void bind_unbound(const std::shared_ptr<stream_recorder::keyframe_channel_t> &channel,
                      const std::string &keyframe = "K") {
      ASSERT_NO_FATAL_FAILURE(start_unbound(channel));
      packet_at(k_owner, true, keyframe, base);
      const auto s = continuous();
      ASSERT_EQ(s.owner_generation, k_owner);
      ASSERT_EQ(s.state, continuous_state_e::writing);
      ASSERT_EQ(s.packets_written, 1u);
      ASSERT_EQ(stats().reservations, 0u);
    }

    /// Write natively, after holding the first call on `write_gate`.
    void hold_first_write() {
      hooks.write = [this](int fd, const std::uint8_t *data, std::size_t size) -> long long {
        if (writes.fetch_add(1) == 0) {
          write_gate.hold();
        }
        return stream_recorder::detail::native_write(fd, data, size);
      };
      install();
    }

    /// Write the first call natively; fail every later call with EIO, after holding the second
    /// on `write_gate` when `hold_failure`.
    void fail_after_first_write(bool hold_failure) {
      hooks.write = [this, hold_failure](int fd, const std::uint8_t *data, std::size_t size) -> long long {
        const int call = writes.fetch_add(1);
        if (call == 0) {
          return stream_recorder::detail::native_write(fd, data, size);
        }
        if (call == 1 && hold_failure) {
          write_gate.hold();
        }
        errno = EIO;
        return -1;
      };
      hooks.after_worker_failure = [this] {
        failure_published.set();
      };
      install();
    }

    /// The exhausted request sequence scenario, run only inside an isolated child process.
    int request_sequence_exhaustion_child() {
      stream_recorder::detail::set_keyframe_counters_for_tests(0, k_max - 1);
      CHILD_CHECK(stats().keyframe_request_sequence == k_max - 1, 1);
      const auto owner = make_idr();
      stream_recorder::set_active_video_format(k_hevc);
      stream_recorder::session_started(k_owner, true, -1, owner->channel);
      stream_recorder::start_recording();
      CHILD_CHECK(stream_recorder::is_recording(), 2);
      packet_at(k_owner, true, "K", base);
      CHILD_CHECK(continuous().owner_generation == k_owner, 3);

      invalid_at(k_owner, base + 1s);  // The first recovery reserves the last sequence
      auto s = stats();
      CHILD_CHECK(s.reservations == 1 && s.attempted == 1, 4);
      CHILD_CHECK(s.keyframe_request_sequence == k_max, 5);
      CHILD_CHECK(!s.in_flight && s.last_request_at, 6);
      const auto finished = dispatches.finished();
      CHILD_CHECK(finished.size() == 1 && finished[0].first.sequence == k_max, 7);
      CHILD_CHECK(finished[0].second == keyframe_outcome_e::attempted, 8);
      CHILD_CHECK(owner->event->try_pop(), 9);
      CHILD_CHECK(!owner->event->try_pop(), 10);

      const auto anchor = *s.last_request_at;
      packet_at(k_owner, false, "p", anchor + 1s);  // Due again, but no sequence is left
      s = stats();
      CHILD_CHECK(s.refused_request_sequence_exhausted == 1, 11);
      CHILD_CHECK(s.refused_rate == 0, 12);
      CHILD_CHECK(s.reservations == 1 && !s.in_flight && !s.authorized, 13);
      CHILD_CHECK(s.keyframe_request_sequence == k_max, 14);  // Neither wrapped nor reused
      CHILD_CHECK(s.last_request_at && *s.last_request_at == anchor, 15);
      invalid_at(k_owner, anchor + 2s);
      s = stats();
      CHILD_CHECK(s.refused_request_sequence_exhausted == 2 && s.reservations == 1, 16);
      CHILD_CHECK(s.keyframe_request_sequence == k_max, 17);
      CHILD_CHECK(dispatches.finished().size() == 1, 18);
      CHILD_CHECK(!owner->event->peek(), 19);
      stream_recorder::stop_recording();
      CHILD_CHECK(!stream_recorder::is_recording(), 20);
      return 0;
    }

    /// The exhausted incarnation scenario, run only inside an isolated child process.
    int incarnation_exhaustion_child() {
      stream_recorder::detail::set_keyframe_counters_for_tests(k_max, 0);
      CHILD_CHECK(stats().continuous_incarnation == k_max, 1);
      const auto owner = make_idr();
      // Known codec: the start binds the owner, which would otherwise request its first keyframe.
      stream_recorder::session_started(k_owner, true, k_hevc, owner->channel);
      stream_recorder::start_recording();
      CHILD_CHECK(stream_recorder::is_recording(), 2);
      auto s = stats();
      CHILD_CHECK(s.file_incarnation == 0, 3);
      CHILD_CHECK(s.continuous_incarnation == k_max, 4);
      CHILD_CHECK(continuous().owner_generation == k_owner, 5);
      CHILD_CHECK(s.reservations == 0, 6);

      packet_at(k_owner, true, "KKK", base);  // Recording itself works
      auto c = continuous();
      CHILD_CHECK(c.state == continuous_state_e::writing, 7);
      CHILD_CHECK(c.packets_written == 1 && c.bytes_written == 3, 8);
      invalid_at(k_owner, base + 1s);
      packet_at(k_owner, false, "p", base + 2s);
      c = continuous();
      CHILD_CHECK(c.state == continuous_state_e::awaiting_keyframe, 9);
      s = stats();
      CHILD_CHECK(s.reservations == 0 && s.attempted == 0 && !s.in_flight && !s.authorized, 10);
      CHILD_CHECK(s.file_incarnation == 0 && s.continuous_incarnation == k_max, 11);
      CHILD_CHECK(dispatches.finished().empty(), 12);
      CHILD_CHECK(!owner->event->peek(), 13);
      packet_at(k_owner, true, "LL", base + 3s);
      CHILD_CHECK(continuous().bytes_written == 5, 14);
      stream_recorder::stop_recording();
      CHILD_CHECK(!stream_recorder::is_recording(), 15);
      return 0;
    }

    config::recording_t saved_recording;
    fs::path owned_parent;
    fs::path output_dir;
    clock_type::time_point base;
    stream_recorder::detail::continuous_hooks_t hooks;
    stream_recorder::detail::keyframe_hooks_t keyframe_hooks;
    // State the seams read or write: fixture members, declared before the threads that use them.
    gate_t write_gate;
    signal_t failure_published;
    std::atomic<int> writes {0};
    dispatches_t dispatches;
    std::vector<std::shared_ptr<idr_t>> idrs;
    std::vector<std::jthread> threads;  // Last: joined (in TearDown) before anything above goes.
  };
}  // namespace

TEST_F(StreamRecorderKeyframeRecoveryTests, ThreeArgumentRegistrationNeverRequests) {
  // The production registration: no channel. A real event exists for the same generation.
  const auto unregistered = make_idr();
  stream_recorder::session_started(k_owner, true, k_hevc);
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  ASSERT_EQ(continuous().owner_generation, k_owner);  // Bound at start, awaiting its keyframe

  packet_at(k_owner, true, "K", base);
  ASSERT_EQ(continuous().state, continuous_state_e::writing);
  invalid_at(k_owner, base + 1s);
  packet_at(k_owner, false, "p", base + 2s);
  packet_at(k_owner, false, "q", base + 4s);

  const auto c = continuous();
  EXPECT_EQ(c.state, continuous_state_e::awaiting_keyframe);
  EXPECT_EQ(c.discontinuities, 1u);
  EXPECT_EQ(c.refused_awaiting_keyframe, 2u);
  const auto s = stats();
  EXPECT_TRUE(s.awaiting);
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(s.authorized);
  EXPECT_TRUE(dispatches.finished().empty());
  EXPECT_FALSE(unregistered->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, UnboundFileNeverRequests) {
  // Idle start: nothing registered, so nothing is selected and the file is unbound.
  stream_recorder::set_active_video_format(k_hevc);
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  const auto owner = make_idr();
  stream_recorder::session_started(k_owner, true, -1, owner->channel);
  ASSERT_EQ(stream_recorder::replay_stats().selected_generation, k_owner);

  packet_at(k_owner, false, "p", base);
  invalid_at(k_owner, base + 1s);
  packet_at(k_owner, false, "q", base + 3s);
  auto c = continuous();
  EXPECT_EQ(c.owner_generation, 0u);
  EXPECT_EQ(c.state, continuous_state_e::awaiting_owner);
  EXPECT_EQ(c.refused_awaiting_keyframe, 2u);
  EXPECT_EQ(c.refused_invalid, 1u);
  auto s = stats();
  EXPECT_FALSE(s.awaiting);
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_FALSE(owner->event->peek());

  // It binds at an admitted keyframe and is then already writing: nothing is owed.
  packet_at(k_owner, true, "K", base + 4s);
  c = continuous();
  EXPECT_EQ(c.owner_generation, k_owner);
  EXPECT_EQ(c.state, continuous_state_e::writing);
  EXPECT_EQ(c.packets_written, 1u);
  s = stats();
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_FALSE(owner->event->peek());

  // Only a later loss asks.
  invalid_at(k_owner, base + 5s);
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, StartBoundFileRequestsItsFirstKeyframe) {
  // A watcher with its own channel was registered (ineligible) and ended before the start: while
  // it was registered no session was sole (the unchanged selection rule counts every
  // registration), and its retired channel is never asked.
  const auto watcher = make_idr();
  const auto owner = make_idr();
  stream_recorder::session_started(k_watcher, false, k_hevc, watcher->channel);
  stream_recorder::session_ended(k_watcher);
  stream_recorder::session_started(k_owner, true, k_hevc, owner->channel);
  ASSERT_EQ(stream_recorder::replay_stats().selected_generation, k_owner);

  stream_recorder::start_recording();  // The commit is the trigger; the raise follows the unlock
  ASSERT_TRUE(stream_recorder::is_recording());
  EXPECT_EQ(continuous().owner_generation, k_owner);
  auto s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(s.authorized);
  EXPECT_TRUE(s.awaiting);
  ASSERT_TRUE(s.last_request_at);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
  EXPECT_FALSE(watcher->event->peek());

  // Dependent owner frames before the interval: no second reservation.
  const auto anchor = *s.last_request_at;
  packet_at(k_owner, false, "p", anchor + 500ms);
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.refused_rate, 1u);
  EXPECT_FALSE(owner->event->peek());

  // The first admitted owner keyframe ends recovery.
  packet_at(k_owner, true, "K", anchor + 600ms);
  s = stats();
  EXPECT_FALSE(s.awaiting);
  EXPECT_FALSE(s.last_request_at);
  EXPECT_EQ(continuous().state, continuous_state_e::writing);
  packet_at(k_owner, false, "q", anchor + 2s);
  EXPECT_EQ(stats().reservations, 1u);
  EXPECT_FALSE(owner->event->peek());
  EXPECT_FALSE(watcher->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, DiscontinuityAfterAdmittedKeyframeRequestsTheOwnerOnly) {
  const auto owner = make_idr();
  const auto watcher = make_idr();
  // While an ineligible watcher with its own channel is registered, no session is sole: owner
  // packets reach nobody, and nobody is asked.
  stream_recorder::session_started(k_watcher, false, k_hevc, watcher->channel);
  stream_recorder::set_active_video_format(k_hevc);
  stream_recorder::session_started(k_owner, true, -1, owner->channel);
  ASSERT_EQ(stream_recorder::replay_stats().selected_generation, 0u);
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  packet_at(k_owner, true, "K", base);
  invalid_at(k_owner, base + 1s);
  EXPECT_EQ(continuous().owner_generation, 0u);
  EXPECT_EQ(stats().reservations, 0u);
  EXPECT_FALSE(owner->event->peek());
  EXPECT_FALSE(watcher->event->peek());

  // The watcher ends; the owner is sole, binds at its admitted keyframe, then loses a frame.
  stream_recorder::session_ended(k_watcher);
  ASSERT_EQ(stream_recorder::replay_stats().selected_generation, k_owner);
  packet_at(k_owner, true, "K", base + 2s);
  ASSERT_EQ(continuous().owner_generation, k_owner);
  ASSERT_EQ(continuous().state, continuous_state_e::writing);
  EXPECT_EQ(stats().reservations, 0u);
  invalid_at(k_owner, base + 3s);

  const auto s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  const auto finished = dispatches.finished();
  ASSERT_EQ(finished.size(), 1u);
  EXPECT_EQ(finished[0].first.generation, k_owner);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
  EXPECT_FALSE(watcher->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, BackpressureDropDefersUntilCapacityThenRequestsOnce) {
  stream_recorder::detail::set_continuous_limits_for_tests(16, 8);
  hold_first_write();
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(start_unbound(owner->channel));

  ASSERT_TRUE(push(k_owner, true, "KKKK"));  // Bound and admitted; held in write()
  ASSERT_TRUE(write_gate.wait_entered());
  ASSERT_EQ(continuous().owner_generation, k_owner);
  ASSERT_TRUE(push(k_owner, false, std::string(12, 'x')));  // 4 + 12 == 16: admitted
  ASSERT_EQ(continuous().queued_bytes, 16u);
  ASSERT_TRUE(push(k_owner, false, "y"));  // No room: dropped, the owner must restart at a keyframe
  auto c = continuous();
  EXPECT_EQ(c.dropped_backpressure, 1u);
  EXPECT_EQ(c.cause, continuous_cause_e::backpressure);
  auto s = stats();
  EXPECT_TRUE(s.awaiting);
  EXPECT_EQ(s.refused_bytes, 1u);  // A request now would ask for a keyframe that cannot fit
  EXPECT_EQ(s.reservations, 0u);
  ASSERT_TRUE(push(k_owner, false, "z"));
  s = stats();
  EXPECT_EQ(s.refused_bytes, 2u);
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_FALSE(owner->event->peek());

  write_gate.release();
  ASSERT_TRUE(drain());
  ASSERT_TRUE(push(k_owner, false, "w"));  // Next packet: room for twice the last keyframe
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_EQ(s.refused_bytes, 2u);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, SuspendedToSoleRecoveryKeepsTheRateAnchor) {
  const auto owner = make_idr();
  const auto other = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  invalid_at(k_owner, base + 1s);
  auto s = stats();
  ASSERT_EQ(s.attempted, 1u);
  ASSERT_TRUE(s.last_request_at);
  const auto anchor = *s.last_request_at;
  EXPECT_EQ(anchor, base + 1s);
  ASSERT_TRUE(owner->event->try_pop());

  // An overlap suspends the owner, then the owner is sole again. Both lifecycle triggers run on
  // the real clock, which is before the anchor, so neither can reserve.
  stream_recorder::session_started(k_other, true, k_hevc, other->channel);
  EXPECT_EQ(continuous().state, continuous_state_e::suspended);
  stream_recorder::session_ended(k_other);
  EXPECT_EQ(continuous().state, continuous_state_e::awaiting_keyframe);
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  ASSERT_TRUE(s.last_request_at);
  EXPECT_EQ(*s.last_request_at, anchor);  // Kept through the overlap

  const auto refused = s.refused_rate;
  packet_at(k_owner, false, "p", anchor + 600ms);
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.refused_rate, refused + 1);
  EXPECT_FALSE(owner->event->peek());

  packet_at(k_owner, false, "q", anchor + 1s);
  s = stats();
  EXPECT_EQ(s.reservations, 2u);
  EXPECT_EQ(s.attempted, 2u);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
  EXPECT_FALSE(other->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, AdmittedOwnerKeyframeEndsRecovery) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  invalid_at(k_owner, base + 1s);
  auto s = stats();
  ASSERT_EQ(s.attempted, 1u);
  ASSERT_TRUE(s.awaiting);

  packet_at(k_owner, true, "KK", base + 1200ms);
  s = stats();
  EXPECT_FALSE(s.awaiting);
  EXPECT_FALSE(s.last_request_at);
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(s.authorized);
  EXPECT_EQ(s.last_keyframe_bytes, 2u);
  EXPECT_EQ(continuous().state, continuous_state_e::writing);

  packet_at(k_owner, false, "p", base + 3s);
  packet_at(k_owner, false, "q", base + 5s);
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.refused_rate, 0u);
  EXPECT_TRUE(owner->event->try_pop());  // Latched once, in total
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, RateLimitHoldsUntilIntervalAndIgnoresClockRegression) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  invalid_at(k_owner, base + 1s);
  auto s = stats();
  ASSERT_EQ(s.reservations, 1u);
  ASSERT_TRUE(s.last_request_at);
  const auto anchor = *s.last_request_at;
  ASSERT_TRUE(owner->event->try_pop());

  packet_at(k_owner, false, "p", anchor + 500ms);
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.refused_rate, 1u);
  EXPECT_EQ(*s.last_request_at, anchor);

  packet_at(k_owner, false, "q", anchor - 1s);  // The clock went backwards
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.refused_rate, 2u);
  EXPECT_EQ(*s.last_request_at, anchor);
  EXPECT_FALSE(owner->event->peek());

  packet_at(k_owner, false, "r", anchor + 1s);
  s = stats();
  EXPECT_EQ(s.reservations, 2u);
  EXPECT_EQ(s.attempted, 2u);
  EXPECT_EQ(s.refused_rate, 2u);
  ASSERT_TRUE(s.last_request_at);
  EXPECT_EQ(*s.last_request_at, anchor + 1s);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, CompletionReleasesOnlyTheMatchingReservation) {
  const auto owner = make_idr();
  const auto other = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  dispatches.hold(true);

  // S: reserved, then paused before the request gate.
  run([this] {
    invalid_at(k_owner, base + 1s);
  });
  const auto first = dispatches.wait_entered(1);
  ASSERT_TRUE(first);
  auto s = stats();
  ASSERT_EQ(s.in_flight, first);
  ASSERT_EQ(s.authorized, first);
  ASSERT_TRUE(s.last_request_at);
  const auto anchor = *s.last_request_at;

  // An overlap withdraws S and keeps the anchor; the owner is then sole again.
  stream_recorder::session_started(k_other, true, k_hevc, other->channel);
  stream_recorder::session_ended(k_other);
  s = stats();
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(s.authorized);
  ASSERT_TRUE(s.last_request_at);
  EXPECT_EQ(*s.last_request_at, anchor);
  EXPECT_EQ(s.reservations, 1u);

  // S+1, at the first time the anchor allows, paused independently.
  run([anchor] {
    packet_at(k_owner, false, "p", anchor + 1s);
  });
  const auto second = dispatches.wait_entered(2);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->incarnation, first->incarnation);
  EXPECT_EQ(second->sequence, first->sequence + 1);
  s = stats();
  ASSERT_EQ(s.in_flight, second);
  ASSERT_EQ(s.authorized, second);

  // S completes while S+1 is still pending: counted for this file, and S+1 is untouched.
  dispatches.release(first->sequence);
  EXPECT_EQ(dispatches.wait_finished(first->sequence), keyframe_outcome_e::fenced);
  s = stats();
  EXPECT_EQ(s.fenced, 1u);
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_EQ(s.keyframe_stale_completions_total, 0u);
  EXPECT_EQ(s.in_flight, second);
  EXPECT_EQ(s.authorized, second);
  EXPECT_FALSE(owner->event->peek());

  // Only S+1 raises the real event and releases its reservation.
  dispatches.release(second->sequence);
  EXPECT_EQ(dispatches.wait_finished(second->sequence), keyframe_outcome_e::attempted);
  threads.clear();  // Both dispatching threads have returned
  s = stats();
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_EQ(s.fenced, 1u);
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(s.authorized);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
  EXPECT_FALSE(other->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, LateCompletionFromAnOlderFileCountsOnlyAsStale) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  dispatches.hold(true);

  // File N: a reservation paused before the request gate.
  run([this] {
    invalid_at(k_owner, base + 1s);
  });
  const auto old_token = dispatches.wait_entered(1);
  ASSERT_TRUE(old_token);
  const auto old_incarnation = stats().file_incarnation;
  EXPECT_EQ(old_token->incarnation, old_incarnation);

  // Stop does not wait for it. File N+1 binds and reserves its own request, also paused.
  stream_recorder::stop_recording();
  ASSERT_FALSE(stream_recorder::is_recording());
  EXPECT_EQ(stats().file_incarnation, 0u);
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  packet_at(k_owner, true, "K", base + 2s);
  ASSERT_EQ(continuous().owner_generation, k_owner);
  run([this] {
    invalid_at(k_owner, base + 3s);
  });
  const auto new_token = dispatches.wait_entered(2);
  ASSERT_TRUE(new_token);
  EXPECT_GT(new_token->incarnation, old_incarnation);
  auto s = stats();
  EXPECT_EQ(s.file_incarnation, new_token->incarnation);
  ASSERT_EQ(s.in_flight, new_token);
  ASSERT_EQ(s.authorized, new_token);
  EXPECT_EQ(s.reservations, 1u);

  // N completes while N+1 is pending: only the process stale total moves.
  dispatches.release(old_token->sequence);
  EXPECT_EQ(dispatches.wait_finished(old_token->sequence), keyframe_outcome_e::fenced);
  s = stats();
  EXPECT_EQ(s.keyframe_stale_completions_total, 1u);
  EXPECT_EQ(s.fenced, 0u);
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.in_flight, new_token);
  EXPECT_EQ(s.authorized, new_token);
  EXPECT_FALSE(owner->event->peek());

  dispatches.release(new_token->sequence);
  EXPECT_EQ(dispatches.wait_finished(new_token->sequence), keyframe_outcome_e::attempted);
  threads.clear();
  s = stats();
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_EQ(s.keyframe_stale_completions_total, 1u);
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(s.authorized);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, RequestSequenceExhaustionRefusesWithoutWrap) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const auto before = stats();
  // The raised counter lives and dies with the child; the parent's never move.
  EXPECT_EXIT(finish_child(request_sequence_exhaustion_child()), ::testing::ExitedWithCode(0), "");
  const auto after = stats();
  EXPECT_EQ(after.keyframe_request_sequence, before.keyframe_request_sequence);
  EXPECT_EQ(after.continuous_incarnation, before.continuous_incarnation);
  EXPECT_LT(after.keyframe_request_sequence, k_max - 1);
}

TEST_F(StreamRecorderKeyframeRecoveryTests, IncarnationExhaustionDisablesRequestsForTheFile) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  const auto before = stats();
  EXPECT_EXIT(finish_child(incarnation_exhaustion_child()), ::testing::ExitedWithCode(0), "");
  const auto after = stats();
  EXPECT_EQ(after.continuous_incarnation, before.continuous_incarnation);
  EXPECT_EQ(after.keyframe_request_sequence, before.keyframe_request_sequence);
  EXPECT_LT(after.continuous_incarnation, k_max);
}

TEST_F(StreamRecorderKeyframeRecoveryTests, StopFencesAPausedDispatch) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  dispatches.hold(true);
  run([this] {
    invalid_at(k_owner, base + 1s);
  });
  const auto token = dispatches.wait_entered(1);
  ASSERT_TRUE(token);

  // Stop returns while the dispatch is still paused: it never waits for a request.
  ASSERT_TRUE(run([] {
                stream_recorder::stop_recording();
              })->wait());
  ASSERT_FALSE(stream_recorder::is_recording());
  auto s = stats();
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(s.authorized);
  EXPECT_EQ(s.file_incarnation, 0u);
  EXPECT_EQ(s.reservations, 1u);  // Kept after stop until the next start

  dispatches.release(token->sequence);
  EXPECT_EQ(dispatches.wait_finished(token->sequence), keyframe_outcome_e::fenced);
  threads.clear();
  s = stats();
  EXPECT_EQ(s.keyframe_stale_completions_total, 1u);  // Its file is closed
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_EQ(s.fenced, 0u);
  EXPECT_FALSE(owner->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, ShutdownFencesAPausedDispatch) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  dispatches.hold(true);
  run([this] {
    invalid_at(k_owner, base + 1s);
  });
  const auto token = dispatches.wait_entered(1);
  ASSERT_TRUE(token);

  ASSERT_TRUE(run([] {
                stream_recorder::shutdown();
              })->wait());
  ASSERT_FALSE(stream_recorder::is_recording());
  dispatches.release(token->sequence);
  EXPECT_EQ(dispatches.wait_finished(token->sequence), keyframe_outcome_e::fenced);
  threads.clear();
  auto s = stats();
  EXPECT_EQ(s.keyframe_stale_completions_total, 1u);
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(owner->event->peek());

  stream_recorder::start_recording();  // The existing shutdown fence
  EXPECT_FALSE(stream_recorder::is_recording());
  EXPECT_EQ(stats().file_incarnation, 0u);
  EXPECT_FALSE(owner->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, OwnerEndRetiresTheChannelBeforeAPausedDispatchRaises) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  dispatches.hold(true);
  run([this] {
    invalid_at(k_owner, base + 1s);
  });
  const auto token = dispatches.wait_entered(1);
  ASSERT_TRUE(token);

  stream_recorder::session_ended(k_owner);
  EXPECT_EQ(continuous().cause, continuous_cause_e::owner_ended);
  auto s = stats();
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(s.authorized);

  dispatches.release(token->sequence);
  const auto outcome = dispatches.wait_finished(token->sequence);
  ASSERT_TRUE(outcome);
  EXPECT_NE(*outcome, keyframe_outcome_e::attempted);
  EXPECT_EQ(*outcome, keyframe_outcome_e::fenced);
  threads.clear();
  s = stats();
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_EQ(s.fenced, 1u);
  EXPECT_FALSE(owner->event->peek());
  // The recorder retired the channel: a later request can never raise this session's event.
  EXPECT_EQ(owner->channel->request(), keyframe_outcome_e::retired);
  EXPECT_TRUE(owner->event->running());
  EXPECT_FALSE(owner->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, OverlapFencesAPausedDispatch) {
  const auto owner = make_idr();
  const auto other = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  dispatches.hold(true);
  run([this] {
    invalid_at(k_owner, base + 1s);
  });
  const auto token = dispatches.wait_entered(1);
  ASSERT_TRUE(token);

  stream_recorder::session_started(k_other, true, k_hevc, other->channel);
  EXPECT_EQ(continuous().state, continuous_state_e::suspended);
  dispatches.release(token->sequence);
  EXPECT_EQ(dispatches.wait_finished(token->sequence), keyframe_outcome_e::fenced);
  threads.clear();
  const auto s = stats();
  EXPECT_EQ(s.fenced, 1u);
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_FALSE(s.in_flight);
  EXPECT_FALSE(owner->event->peek());
  EXPECT_FALSE(other->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, CodecFenceStopsRequestsPermanently) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  invalid_at(k_owner, base + 1s);
  ASSERT_EQ(stats().attempted, 1u);
  ASSERT_TRUE(owner->event->try_pop());

  packet_at(k_owner, true, "A", base + 1100ms, k_av1);  // One file is one codec
  auto c = continuous();
  EXPECT_EQ(c.cause, continuous_cause_e::codec);
  EXPECT_EQ(c.refused_codec, 1u);
  packet_at(k_owner, false, "p", base + 3s);
  invalid_at(k_owner, base + 5s);
  packet_at(k_owner, true, "K", base + 7s);
  const auto s = stats();
  EXPECT_FALSE(s.awaiting);
  EXPECT_FALSE(s.last_request_at);
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_EQ(continuous().packets_written, 1u);
  EXPECT_FALSE(owner->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, WriterFailedBeforeDecisionRefuses) {
  // The failing write is held before it fails, so the admitted packet's own keyframe decision is
  // always taken while the writer is still healthy: only the decision after the failure counts.
  fail_after_first_write(true);
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  // Admitted; its write is held, then fails. packet_at waits for that write, so it runs on a helper.
  const auto admitted = run([at = base + 100ms] {
    packet_at(k_owner, false, "p", at);
  });
  ASSERT_TRUE(write_gate.wait_entered());
  // The write was published inside the recorder lock that also took the packet's decision, and
  // stats() takes that lock: the decision is complete, and the writer has not failed yet.
  auto s = stats();
  RecordProperty("refusedWriterUnavailableWhileWriteHeld", static_cast<int>(s.refused_writer_unavailable));
  RecordProperty("writeCallsWhileWriteHeld", writes.load());
  ASSERT_EQ(s.refused_writer_unavailable, 0u);
  ASSERT_EQ(s.reservations, 0u);
  ASSERT_FALSE(s.in_flight);
  ASSERT_TRUE(dispatches.finished().empty());
  ASSERT_FALSE(owner->event->peek());

  write_gate.release();
  ASSERT_TRUE(failure_published.wait());
  ASSERT_TRUE(admitted->wait());
  ASSERT_TRUE(drain());
  ASSERT_NE(continuous().error, continuous_error_e::none);
  s = stats();
  RecordProperty("refusedWriterUnavailableAfterFailureBeforeInvalid", static_cast<int>(s.refused_writer_unavailable));
  ASSERT_EQ(s.refused_writer_unavailable, 0u);

  invalid_at(k_owner, base + 1s);
  s = stats();
  RecordProperty("refusedWriterUnavailableAfterInvalid", static_cast<int>(s.refused_writer_unavailable));
  EXPECT_EQ(s.refused_writer_unavailable, 1u);
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_FALSE(s.in_flight);
  EXPECT_TRUE(dispatches.finished().empty());
  EXPECT_FALSE(owner->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, WriterFailureAfterReservationAllowsAtMostOneAttempt) {
  fail_after_first_write(true);
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(start_unbound(owner->channel));
  ASSERT_TRUE(push(k_owner, true, "K"));  // Bound, admitted and written
  ASSERT_TRUE(drain());
  ASSERT_EQ(continuous().packets_written, 1u);
  ASSERT_TRUE(push(k_owner, false, "q"));  // Admitted; its write is held, then fails
  ASSERT_TRUE(write_gate.wait_entered());

  dispatches.hold(true);
  run([] {
    stream_recorder::push_packet(nullptr, 0, true, false, 0, k_owner, k_hevc);
  });
  const auto token = dispatches.wait_entered(1);
  ASSERT_TRUE(token);
  auto s = stats();
  ASSERT_EQ(s.reservations, 1u);
  ASSERT_TRUE(s.last_request_at);
  const auto anchor = *s.last_request_at;

  write_gate.release();
  ASSERT_TRUE(failure_published.wait());
  ASSERT_TRUE(drain());
  ASSERT_NE(continuous().error, continuous_error_e::none);

  // The reservation made before the failure may still raise, once.
  dispatches.release(token->sequence);
  EXPECT_EQ(dispatches.wait_finished(token->sequence), keyframe_outcome_e::attempted);
  dispatches.hold(false);
  invalid_at(k_owner, anchor + 1s);
  invalid_at(k_owner, anchor + 2s);
  threads.clear();
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_EQ(s.refused_writer_unavailable, 2u);
  EXPECT_EQ(dispatches.finished().size(), 1u);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, FullSlotQueueRefusesRequest) {
  stream_recorder::detail::set_continuous_limits_for_tests(64, 2);
  hold_first_write();
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(start_unbound(owner->channel));
  ASSERT_TRUE(push(k_owner, true, "K"));  // Held in write()
  ASSERT_TRUE(write_gate.wait_entered());
  ASSERT_TRUE(push(k_owner, false, "p"));
  ASSERT_EQ(continuous().queued_packets, 2u);  // Both slots really hold a packet

  ASSERT_TRUE(push_invalid(k_owner));
  auto s = stats();
  EXPECT_TRUE(s.awaiting);
  EXPECT_EQ(s.refused_no_slot, 1u);
  EXPECT_EQ(s.refused_bytes, 0u);
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_FALSE(owner->event->peek());

  write_gate.release();
  ASSERT_TRUE(drain());
  ASSERT_TRUE(push(k_owner, false, "q"));
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_EQ(s.refused_no_slot, 1u);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, ExhaustedWriterSequenceRefusesRequest) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  ASSERT_TRUE(stream_recorder::detail::set_continuous_next_sequence_for_tests(k_max));

  invalid_at(k_owner, base + 1s);
  const auto s = stats();
  EXPECT_TRUE(s.awaiting);
  EXPECT_EQ(s.refused_queue_sequence_exhausted, 1u);
  EXPECT_EQ(s.refused_no_slot, 0u);
  EXPECT_EQ(s.refused_bytes, 0u);
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_FALSE(owner->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, InsufficientFreeBytesRefusesRequest) {
  stream_recorder::detail::set_continuous_limits_for_tests(16, 8);
  hold_first_write();
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(start_unbound(owner->channel));
  ASSERT_TRUE(push(k_owner, true, std::string(8, 'K')));  // Held: 8 of 16 bytes stay used
  ASSERT_TRUE(write_gate.wait_entered());
  ASSERT_EQ(continuous().queued_bytes, 8u);

  // The proposal asks for twice the last owner keyframe (16) free; 8 are.
  ASSERT_TRUE(push_invalid(k_owner));
  auto s = stats();
  EXPECT_EQ(s.last_keyframe_bytes, 8u);
  EXPECT_EQ(s.refused_bytes, 1u);
  EXPECT_EQ(s.refused_no_slot, 0u);
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_FALSE(owner->event->peek());

  write_gate.release();
  ASSERT_TRUE(drain());
  ASSERT_TRUE(push(k_owner, false, "p"));  // 16 free now
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, OversizedOwnerKeyframeDisablesRequestsForTheFile) {
  stream_recorder::detail::set_continuous_limits_for_tests(8, 4);
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(start_unbound(owner->channel));
  packet_at(k_owner, true, std::string(9, 'X'), base);  // Binds, then cannot fit the whole queue
  auto c = continuous();
  EXPECT_EQ(c.owner_generation, k_owner);
  EXPECT_EQ(c.dropped_backpressure, 1u);
  EXPECT_EQ(c.state, continuous_state_e::awaiting_keyframe);
  auto s = stats();
  EXPECT_TRUE(s.keyframe_exceeds_queue);
  EXPECT_TRUE(s.awaiting);

  packet_at(k_owner, false, "p", base + 2s);
  invalid_at(k_owner, base + 4s);
  s = stats();
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_EQ(s.refused_bytes, 0u);
  EXPECT_FALSE(owner->event->peek());

  // A new file starts without the mark and may ask again.
  stream_recorder::stop_recording();
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  s = stats();
  EXPECT_FALSE(s.keyframe_exceeds_queue);
  packet_at(k_owner, true, "KKK", base + 5s);
  ASSERT_EQ(continuous().owner_generation, k_owner);
  invalid_at(k_owner, base + 6s);
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, StoppedEventIsAnAttemptNeverAnAcknowledgment) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  owner->event->stop();

  invalid_at(k_owner, base + 1s);
  auto s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.stopped, 1u);
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_FALSE(s.in_flight);
  EXPECT_TRUE(s.awaiting);  // Nothing was acknowledged
  EXPECT_EQ(continuous().state, continuous_state_e::awaiting_keyframe);
  EXPECT_FALSE(owner->event->peek());
  ASSERT_EQ(dispatches.finished().size(), 1u);
  EXPECT_EQ(dispatches.finished()[0].second, keyframe_outcome_e::stopped);

  packet_at(k_owner, true, "K", base + 1500ms);  // Only an admitted keyframe recovers
  s = stats();
  EXPECT_FALSE(s.awaiting);
  EXPECT_EQ(continuous().state, continuous_state_e::writing);
}

TEST_F(StreamRecorderKeyframeRecoveryTests, TheChannelDoesNotKeepSessionMailAlive) {
  std::weak_ptr<safe::mail_raw_t> weak_mail;
  {
    auto box = std::make_shared<safe::mail_raw_t>();
    auto event = box->event<bool>(::mail::idr);
    weak_mail = box;
    ASSERT_NO_FATAL_FAILURE(bind_unbound(std::make_shared<stream_recorder::keyframe_channel_t>(event)));
  }  // Every strong mail and event pointer is gone
  EXPECT_TRUE(weak_mail.expired());

  invalid_at(k_owner, base + 1s);
  const auto s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.expired, 1u);
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_FALSE(s.in_flight);
  EXPECT_TRUE(weak_mail.expired());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, TheRealRequestRunsWithoutTheRecorderLock) {
  const auto owner = make_idr();
  const auto probe = std::make_shared<request_probe_t>();
  // Blocked in the real pop() until the request raises the event.
  run([probe, event = owner->event] {
    probe->popped = event->pop();
    if (!probe->raise_entered.wait()) {
      return;
    }
    // request() has raised and not returned: the recorder lock must be free.
    probe->locked = stream_recorder::detail::recorder_mutex_try_lock_for_tests();
    probe->status_returned = stream_recorder::recorder_status().recording;
    probe->helper_done.set();
  });
  keyframe_hooks.after_raise = [probe] {
    probe->raise_entered.set();
    probe->raise_saw_helper = probe->helper_done.wait();
  };
  install_keyframe_hooks();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));

  invalid_at(k_owner, base + 1s);  // Dispatches on this thread; after_raise waits for the helper
  EXPECT_TRUE(probe->raise_saw_helper);
  EXPECT_TRUE(probe->popped);
  EXPECT_TRUE(probe->locked);
  EXPECT_TRUE(probe->status_returned);
  threads.clear();
  const auto s = stats();
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_FALSE(s.in_flight);
}

TEST_F(StreamRecorderKeyframeRecoveryTests, ThreeArgumentRepeatKeepsTheChannel) {
  const auto owner = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  stream_recorder::session_started(k_owner, true, k_hevc);  // Metadata only: the codec is learned
  stream_recorder::session_started(k_owner, true, k_hevc);  // Unchanged
  EXPECT_EQ(stream_recorder::replay_stats().selected_generation, k_owner);
  EXPECT_EQ(stream_recorder::replay_stats().registered_sessions, 1u);

  invalid_at(k_owner, base + 1s);
  const auto s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_EQ(s.keyframe_registration_conflicts, 0u);
  EXPECT_TRUE(owner->event->try_pop());
  EXPECT_FALSE(owner->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, DistinctChannelRegistrationIsRejectedWhole) {
  const auto a = make_idr();
  const auto b = make_idr();
  stream_recorder::session_started(k_owner, true, k_hevc, a->channel);
  ASSERT_EQ(stream_recorder::replay_stats().selected_generation, k_owner);

  stream_recorder::session_started(k_owner, false, k_av1, b->channel);
  auto r = stream_recorder::replay_stats();
  EXPECT_EQ(r.selected_generation, k_owner);  // Still eligible
  EXPECT_EQ(r.registered_sessions, 1u);
  EXPECT_EQ(stats().keyframe_registration_conflicts, 1u);

  // The start binds the owner in its registered codec and asks channel A, never B.
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  EXPECT_EQ(continuous().owner_generation, k_owner);
  EXPECT_EQ(continuous().file_video_format, k_hevc);
  const auto s = stats();
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_EQ(s.keyframe_registration_conflicts, 1u);
  EXPECT_TRUE(a->event->try_pop());
  EXPECT_FALSE(a->event->try_pop());
  EXPECT_FALSE(b->event->peek());
  // The recorder never retired B: it still raises its own event when asked directly.
  EXPECT_EQ(b->channel->request(), keyframe_outcome_e::attempted);
  EXPECT_TRUE(b->event->try_pop());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, ZeroAndUnknownGenerationsNeverInsert) {
  const auto owner = make_idr();
  const auto zero = make_idr();
  stream_recorder::session_started(k_owner, true, k_hevc, owner->channel);
  const auto before = stream_recorder::replay_stats();
  ASSERT_EQ(before.registered_sessions, 1u);

  stream_recorder::session_started(0, true, k_hevc, zero->channel);
  stream_recorder::session_ended(k_unknown);
  stream_recorder::session_ended(0);
  auto r = stream_recorder::replay_stats();
  EXPECT_EQ(r.registered_sessions, 1u);
  EXPECT_EQ(r.refused_lifecycle, before.refused_lifecycle + 1);
  EXPECT_EQ(r.selected_generation, k_owner);

  stream_recorder::start_recording();  // The owner's own start request
  ASSERT_TRUE(stream_recorder::is_recording());
  auto s = stats();
  ASSERT_EQ(s.reservations, 1u);
  ASSERT_TRUE(s.last_request_at);
  const auto anchor = *s.last_request_at;
  ASSERT_TRUE(owner->event->try_pop());

  // Packets of zero and unknown generations, past the interval, insert and request nothing.
  invalid_at(k_unknown, anchor + 2s);
  packet_at(k_unknown, false, "p", anchor + 3s);
  invalid_at(0, anchor + 4s);
  packet_at(0, false, "q", anchor + 5s);
  r = stream_recorder::replay_stats();
  EXPECT_EQ(r.registered_sessions, 1u);
  EXPECT_EQ(r.refused_lifecycle, before.refused_lifecycle + 1);
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.refused_rate, 0u);
  EXPECT_EQ(dispatches.finished().size(), 1u);
  EXPECT_FALSE(owner->event->peek());
  EXPECT_FALSE(zero->event->peek());
}

TEST_F(StreamRecorderKeyframeRecoveryTests, DiagnosticStatsResetSemantics) {
  const auto owner = make_idr();
  const auto intruder = make_idr();
  ASSERT_NO_FATAL_FAILURE(bind_unbound(owner->channel));
  invalid_at(k_owner, base + 1s);
  const auto first = stats();
  ASSERT_EQ(first.reservations, 1u);
  ASSERT_EQ(first.attempted, 1u);
  ASSERT_NE(first.file_incarnation, 0u);
  ASSERT_TRUE(first.last_request_at);
  EXPECT_EQ(first.continuous_incarnation, first.file_incarnation);

  // A process conflict, and a process stale completion from a dispatch paused across stop.
  stream_recorder::session_started(k_owner, true, -1, intruder->channel);
  EXPECT_EQ(stats().keyframe_registration_conflicts, 1u);
  dispatches.hold(true);
  run([anchor = *first.last_request_at] {
    packet_at(k_owner, false, "p", anchor + 1s);
  });
  const auto paused = dispatches.wait_entered(1 + 1);  // The first dispatch already finished
  ASSERT_TRUE(paused);
  stream_recorder::stop_recording();
  auto s = stats();
  EXPECT_EQ(s.file_incarnation, 0u);
  EXPECT_EQ(s.reservations, 2u);  // Per-file counters stay readable after stop
  EXPECT_EQ(s.attempted, 1u);
  dispatches.release(paused->sequence);
  EXPECT_EQ(dispatches.wait_finished(paused->sequence), keyframe_outcome_e::fenced);
  threads.clear();
  dispatches.hold(false);
  s = stats();
  EXPECT_EQ(s.keyframe_stale_completions_total, 1u);
  EXPECT_EQ(s.reservations, 2u);
  EXPECT_EQ(s.attempted, 1u);
  EXPECT_EQ(s.fenced, 0u);

  // The next start zeroes the per-file counters only.
  stream_recorder::start_recording();
  ASSERT_TRUE(stream_recorder::is_recording());
  s = stats();
  EXPECT_EQ(s.file_incarnation, first.continuous_incarnation + 1);
  EXPECT_EQ(s.continuous_incarnation, first.continuous_incarnation + 1);
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_EQ(s.refused_rate, 0u);
  EXPECT_FALSE(s.last_request_at);
  EXPECT_EQ(s.keyframe_stale_completions_total, 1u);
  EXPECT_EQ(s.keyframe_registration_conflicts, 1u);
  EXPECT_EQ(s.keyframe_request_sequence, first.keyframe_request_sequence + 1);
  packet_at(k_owner, true, "K", base + 3s);
  invalid_at(k_owner, base + 4s);
  s = stats();
  EXPECT_EQ(s.reservations, 1u);
  EXPECT_EQ(s.keyframe_request_sequence, first.keyframe_request_sequence + 2);

  // The test reset zeroes the process totals; the monotonic counters never go back.
  stream_recorder::detail::reset_replay_for_tests();
  s = stats();
  EXPECT_EQ(s.file_incarnation, 0u);
  EXPECT_EQ(s.reservations, 0u);
  EXPECT_EQ(s.attempted, 0u);
  EXPECT_EQ(s.keyframe_stale_completions_total, 0u);
  EXPECT_EQ(s.keyframe_registration_conflicts, 0u);
  EXPECT_EQ(s.continuous_incarnation, first.continuous_incarnation + 1);
  EXPECT_EQ(s.keyframe_request_sequence, first.keyframe_request_sequence + 2);
}
