/**
 * @file tests/unit/test_stream_recorder_continuous.cpp
 * @brief Test continuous recording ownership in src/stream_recorder.*: one file holds at most
 * one selected eligible session's video in one codec, starting at a keyframe; overlaps, owner
 * end, codec violations and invalid input never mix or retain another session's packets; the
 * file is created exclusively and privately, and every write is checked.
 *
 * Bytes are synthetic raw video; nothing here claims a playable file, audio or a container.
 */

#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using steady = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  using stream_recorder::continuous_cause_e;
  using stream_recorder::continuous_error_e;
  using stream_recorder::continuous_state_e;

  constexpr std::uint64_t k_owner = 701;
  constexpr std::uint64_t k_other = 702;
  constexpr std::uint64_t k_watcher = 703;
  constexpr int k_h264 = 0;
  constexpr int k_hevc = 1;
  constexpr int k_av1 = 2;
  constexpr auto k_stamp = "20261009_010033";

  /**
   * Each test starts from the recorder's process start state with recording disabled (default
   * off), no session, no codec known and empty codec allow-lists, writing into a not-yet-created
   * directory inside a parent created exclusively. Every seam reads only fixture members, and
   * TearDown clears the seams (reset_replay_for_tests) before those members go.
   */
  class StreamRecorderContinuousOwnershipTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_recording = config::recording;
      saved_hevc_mode = config::video.hevc_mode;
      saved_av1_mode = config::video.av1_mode;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-continuous-"} + info->name() + "-" +
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
      config::video.hevc_mode = 0;
      config::video.av1_mode = 0;
      publish(false, false);
      hooks.name_stamp = [] { return std::string {k_stamp}; };
      install();
    }

    void TearDown() override {
      // Closes any open file and clears every seam before the members they read go.
      stream_recorder::detail::reset_replay_for_tests();
      config::recording = saved_recording;
      config::video.hevc_mode = saved_hevc_mode;
      config::video.av1_mode = saved_av1_mode;
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

    static void push(std::uint64_t generation, bool keyframe, steady::time_point at, const std::string &bytes, int codec = k_hevc) {
      stream_recorder::detail::push_packet_at(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size(), true, keyframe,
                                              0, generation, codec, at);
    }

    static stream_recorder::recorder_status_t status() {
      return stream_recorder::recorder_status();
    }

    static stream_recorder::continuous_status_t continuous() {
      return status().continuous;
    }

    static std::string read_file(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    /// The open file's bytes.
    static std::string written() {
      return read_file(status().file);
    }

    /// Regular files and links in the output directory, by name.
    std::vector<std::string> output_names() const {
      std::vector<std::string> names;
      std::error_code ec;
      for (fs::directory_iterator it {output_dir, ec}, end; !ec && it != end; it.increment(ec)) {
        names.push_back(it->path().filename().string());
      }
      std::sort(names.begin(), names.end());
      return names;
    }

    static void expect_idle() {
      const auto s = status();
      EXPECT_FALSE(s.recording);
      EXPECT_FALSE(stream_recorder::is_recording());
      EXPECT_TRUE(s.file.empty());
      EXPECT_EQ(s.continuous.state, continuous_state_e::idle);
      EXPECT_EQ(s.continuous.owner_generation, 0u);
      EXPECT_EQ(s.continuous.file_video_format, -1);
      EXPECT_EQ(s.continuous.packets_written, 0u);
      EXPECT_EQ(s.continuous.bytes_written, 0u);
      EXPECT_EQ(s.continuous.error, continuous_error_e::none);
    }

    const steady::time_point t0 = steady::now();
    config::recording_t saved_recording;
    int saved_hevc_mode = 0;
    int saved_av1_mode = 0;
    fs::path owned_parent;
    fs::path output_dir;
    stream_recorder::detail::continuous_hooks_t hooks;
    // State the seams read or write: fixture members, so no seam outlives what it touches.
    int writes = 0;
    int opens = 0;
    int closes = 0;
    int closed_fd = -1;
    stream_recorder::recorder_status_t seen_at_close;
  };
}  // namespace

TEST_F(StreamRecorderContinuousOwnershipTests, ASoleEligibleSessionOwnsTheFileFromItsFirstKeyframe) {
  stream_recorder::session_started(k_owner, true, k_hevc);
  stream_recorder::start_recording();
  auto s = status();
  ASSERT_TRUE(s.recording);
  EXPECT_EQ(fs::path {s.file}.parent_path(), output_dir);
  EXPECT_EQ(fs::path {s.file}.filename(), std::string {"polaris_"} + k_stamp + ".hevc");
  EXPECT_EQ(s.continuous.state, continuous_state_e::awaiting_keyframe);
  EXPECT_EQ(s.continuous.owner_generation, k_owner);
  EXPECT_EQ(s.continuous.file_video_format, k_hevc);
  EXPECT_EQ(written(), "");

  push(k_owner, false, t0, "p");          // No keyframe yet: not written
  push(k_owner, true, t0 + 1ms, "K1");
  push(k_other, true, t0 + 2ms, "X");     // Unregistered generation
  push(0, true, t0 + 2ms, "Z");           // Zero generation
  push(k_owner, false, t0 + 3ms, "p2");
  EXPECT_EQ(written(), "K1p2");
  s = status();
  EXPECT_EQ(s.continuous.state, continuous_state_e::writing);
  EXPECT_EQ(s.continuous.packets_written, 2u);
  EXPECT_EQ(s.continuous.bytes_written, 4u);
  EXPECT_EQ(s.continuous.refused_awaiting_keyframe, 1u);

  const auto path = s.file;
  stream_recorder::stop_recording();
  expect_idle();
  EXPECT_EQ(read_file(path), "K1p2");
  // Packets after stop go nowhere.
  push(k_owner, true, t0 + 4ms, "after");
  EXPECT_EQ(read_file(path), "K1p2");
}

TEST_F(StreamRecorderContinuousOwnershipTests, WatcherAndInputOnlySessionsNeverFeedTheFile) {
  // The adapter registers watchers and input-only sessions as ineligible.
  stream_recorder::set_active_video_format(k_hevc);  // The idle file is named for HEVC
  stream_recorder::session_started(k_watcher, false, k_hevc);
  stream_recorder::start_recording();
  EXPECT_EQ(continuous().state, continuous_state_e::awaiting_owner);
  EXPECT_EQ(continuous().owner_generation, 0u);
  push(k_watcher, true, t0, "W");
  push(k_watcher, false, t0 + 1ms, "w");
  EXPECT_EQ(written(), "");
  EXPECT_EQ(continuous().refused_invalid + continuous().refused_codec + continuous().refused_awaiting_keyframe, 0u);

  // An eligible session alongside the watcher is an overlap: still nothing.
  stream_recorder::session_started(k_owner, true, k_hevc);
  push(k_owner, true, t0 + 2ms, "O");
  EXPECT_EQ(written(), "");

  // Once the watcher leaves, the sole eligible session binds at its keyframe.
  stream_recorder::session_ended(k_watcher);
  push(k_owner, false, t0 + 3ms, "o");
  push(k_owner, true, t0 + 4ms, "K");
  push(k_watcher, true, t0 + 5ms, "W2");  // Retired
  EXPECT_EQ(written(), "K");
  EXPECT_EQ(continuous().owner_generation, k_owner);
}

TEST_F(StreamRecorderContinuousOwnershipTests, AnOverlapSuspendsEverySessionAndOnlyTheOwnerResumesAtAKeyframe) {
  stream_recorder::session_started(k_owner, true, k_hevc);
  stream_recorder::start_recording();
  push(k_owner, true, t0, "A");
  push(k_owner, false, t0 + 1ms, "a");

  stream_recorder::session_started(k_other, true, k_hevc);
  EXPECT_EQ(continuous().state, continuous_state_e::suspended);
  EXPECT_EQ(continuous().cause, continuous_cause_e::selection);
  push(k_owner, false, t0 + 2ms, "x");
  push(k_owner, true, t0 + 3ms, "X");
  push(k_other, true, t0 + 3ms, "Y");
  EXPECT_EQ(written(), "Aa");

  stream_recorder::session_ended(k_other);
  EXPECT_EQ(continuous().state, continuous_state_e::awaiting_keyframe);
  push(k_owner, false, t0 + 4ms, "m");  // Its reference frame was missed
  push(k_owner, true, t0 + 5ms, "B");
  EXPECT_EQ(written(), "AaB");

  // A watcher overlap suspends too, and the owner made ineligible is no different.
  stream_recorder::session_started(k_watcher, false, k_hevc);
  push(k_owner, true, t0 + 6ms, "W");
  stream_recorder::session_ended(k_watcher);
  stream_recorder::session_started(k_owner, false, k_hevc);
  push(k_owner, true, t0 + 7ms, "I");
  EXPECT_EQ(continuous().cause, continuous_cause_e::selection);
  stream_recorder::session_started(k_owner, true, k_hevc);
  push(k_owner, false, t0 + 8ms, "n");
  push(k_owner, true, t0 + 9ms, "C");
  EXPECT_EQ(written(), "AaBC");
  EXPECT_EQ(continuous().owner_generation, k_owner);
  EXPECT_EQ(continuous().packets_written, 4u);
}

TEST_F(StreamRecorderContinuousOwnershipTests, ADifferentSessionNeverJoinsAfterTheOwnerEnds) {
  stream_recorder::session_started(k_owner, true, k_hevc);
  stream_recorder::start_recording();
  push(k_owner, true, t0, "A");
  const auto path = status().file;

  stream_recorder::session_ended(k_owner);
  EXPECT_EQ(continuous().state, continuous_state_e::suspended);
  EXPECT_EQ(continuous().cause, continuous_cause_e::owner_ended);
  // A new sole eligible session of the same codec, and the owner's own late or repeated packets.
  stream_recorder::session_started(k_other, true, k_hevc);
  push(k_other, true, t0 + 1ms, "B");
  push(k_owner, true, t0 + 1ms, "late");
  stream_recorder::session_ended(k_other);
  stream_recorder::session_started(k_owner, true, k_hevc);  // A repeated registration revives nothing
  push(k_owner, true, t0 + 2ms, "again");
  const auto s = status();
  EXPECT_TRUE(s.recording);
  EXPECT_EQ(s.file, path);
  EXPECT_EQ(s.continuous.owner_generation, k_owner);
  EXPECT_EQ(s.continuous.cause, continuous_cause_e::owner_ended);
  EXPECT_EQ(read_file(path), "A");

  // Only a new file can take another session.
  stream_recorder::session_ended(k_owner);
  stream_recorder::session_started(k_other, true, k_hevc);
  stream_recorder::stop_recording();
  stream_recorder::start_recording();
  EXPECT_EQ(fs::path {status().file}.filename(), std::string {"polaris_"} + k_stamp + "-1.hevc");
  push(k_other, true, t0 + 3ms, "B");
  EXPECT_EQ(written(), "B");
  EXPECT_EQ(read_file(path), "A");
}

TEST_F(StreamRecorderContinuousOwnershipTests, ZeroForeignAndRetiredGenerationsNeverTouchTheWriter) {
  stream_recorder::session_started(k_watcher, true, k_hevc);
  stream_recorder::session_ended(k_watcher);  // Retired
  stream_recorder::session_started(k_owner, true, k_hevc);
  stream_recorder::start_recording();
  const auto before = continuous();
  const std::string one {"x"};
  for (const std::uint64_t generation : {std::uint64_t {0}, std::uint64_t {999}, k_watcher}) {
    push(generation, true, t0, "K");
    push(generation, false, t0, "p");
    push(generation, true, t0, "K", k_h264);  // A foreign codec
    push(generation, true, t0 - 1s, "K");     // A foreign clock
    // Malformed packets of other generations are never read.
    stream_recorder::detail::push_packet_at(nullptr, 64, true, true, 0, generation, k_hevc, t0);
    stream_recorder::detail::push_packet_at(reinterpret_cast<const std::uint8_t *>(one.data()),
                                            stream_recorder::k_continuous_max_packet_bytes + 1, true, true, 0, generation, k_hevc, t0);
  }
  const auto after = continuous();
  EXPECT_EQ(after.state, before.state);
  EXPECT_EQ(after.cause, before.cause);
  EXPECT_EQ(after.owner_generation, before.owner_generation);
  EXPECT_EQ(after.packets_written, 0u);
  EXPECT_EQ(after.bytes_written, 0u);
  EXPECT_EQ(after.refused_invalid, 0u);
  EXPECT_EQ(after.refused_codec, 0u);
  EXPECT_EQ(after.refused_awaiting_keyframe, 0u);
  EXPECT_EQ(after.discontinuities, 0u);
  EXPECT_EQ(written(), "");

  // The owner's first keyframe is unaffected by any of that, including the foreign clock.
  push(k_owner, true, t0, "K");
  EXPECT_EQ(written(), "K");
}

TEST_F(StreamRecorderContinuousOwnershipTests, StartBindsTheSelectedSessionsCodec) {
  stream_recorder::set_active_video_format(k_hevc);  // A stale global codec must not name the file
  stream_recorder::session_started(k_owner, true, k_av1);
  stream_recorder::start_recording();
  const auto s = status();
  EXPECT_EQ(fs::path {s.file}.filename(), std::string {"polaris_"} + k_stamp + ".av1");
  EXPECT_EQ(s.continuous.file_video_format, k_av1);
  EXPECT_EQ(s.continuous.owner_generation, k_owner);
  push(k_owner, true, t0, "V", k_av1);
  EXPECT_EQ(written(), "V");
}

TEST_F(StreamRecorderContinuousOwnershipTests, IdleStartUsesTheFallbackAndBindsOnlyAMatchingSession) {
  stream_recorder::set_active_video_format(k_hevc);
  stream_recorder::start_recording();
  auto s = status();
  EXPECT_EQ(fs::path {s.file}.filename(), std::string {"polaris_"} + k_stamp + ".hevc");
  EXPECT_EQ(s.continuous.state, continuous_state_e::awaiting_owner);
  EXPECT_EQ(s.continuous.file_video_format, k_hevc);

  // A sole eligible session whose known codec differs is suspended, and never written.
  stream_recorder::session_started(k_other, true, k_h264);
  EXPECT_EQ(continuous().state, continuous_state_e::suspended);
  EXPECT_EQ(continuous().cause, continuous_cause_e::codec);
  push(k_other, true, t0, "H", k_h264);
  EXPECT_EQ(continuous().refused_codec, 1u);
  EXPECT_EQ(continuous().owner_generation, 0u);
  stream_recorder::session_ended(k_other);
  EXPECT_EQ(continuous().state, continuous_state_e::awaiting_owner);

  // A session registered with no codec binds at its first keyframe of the file's codec.
  stream_recorder::session_started(k_owner, true);
  push(k_owner, false, t0 + 1ms, "p");
  push(k_owner, true, t0 + 2ms, "K");
  push(k_owner, false, t0 + 3ms, "q");
  EXPECT_EQ(written(), "Kq");
  EXPECT_EQ(continuous().owner_generation, k_owner);
  EXPECT_EQ(continuous().refused_awaiting_keyframe, 1u);
}

TEST_F(StreamRecorderContinuousOwnershipTests, IdleFallbackFollowsTheAllowListsWhenNoCodecIsKnown) {
  config::video.av1_mode = 1;  // Only AV1 allowed
  stream_recorder::start_recording();
  EXPECT_EQ(fs::path {status().file}.filename(), std::string {"polaris_"} + k_stamp + ".av1");
  EXPECT_EQ(continuous().file_video_format, k_av1);
  stream_recorder::session_started(k_owner, true);
  push(k_owner, true, t0, "H", k_hevc);  // An unbound candidate of another codec is refused
  EXPECT_EQ(written(), "");
  EXPECT_EQ(continuous().cause, continuous_cause_e::codec);
  push(k_owner, true, t0 + 1ms, "A", k_av1);
  EXPECT_EQ(written(), "A");
}

TEST_F(StreamRecorderContinuousOwnershipTests, ABoundOwnersCodecViolationIsPermanent) {
  stream_recorder::session_started(k_owner, true, k_hevc);
  stream_recorder::start_recording();
  push(k_owner, true, t0, "A");
  push(k_owner, true, t0 + 1ms, "B", k_h264);
  EXPECT_EQ(continuous().state, continuous_state_e::suspended);
  EXPECT_EQ(continuous().cause, continuous_cause_e::codec);
  EXPECT_EQ(continuous().refused_codec, 1u);
  push(k_owner, true, t0 + 2ms, "C");
  stream_recorder::session_started(k_other, true, k_hevc);
  stream_recorder::session_ended(k_other);
  push(k_owner, true, t0 + 3ms, "D");
  EXPECT_EQ(written(), "A");
  EXPECT_EQ(continuous().cause, continuous_cause_e::codec);
  EXPECT_TRUE(stream_recorder::is_recording());

  // Codec metadata that contradicts the bound file fences it the same way.
  stream_recorder::stop_recording();
  stream_recorder::start_recording();
  push(k_owner, true, t0 + 4ms, "E");
  stream_recorder::session_started(k_owner, true, k_av1);
  EXPECT_EQ(continuous().cause, continuous_cause_e::codec);
  push(k_owner, true, t0 + 5ms, "F");
  EXPECT_EQ(written(), "E");
}

TEST_F(StreamRecorderContinuousOwnershipTests, UnknownCodecMetadataNeverErasesAKnownOneOrResetsReplay) {
  publish(true, true);
  stream_recorder::session_started(k_owner, true, k_hevc);
  push(k_owner, true, t0, "K");
  const auto before = stream_recorder::detail::replay_stats_at(t0);
  ASSERT_EQ(before.packets, 1u);

  stream_recorder::session_started(k_owner, true);       // The old two-argument call
  stream_recorder::session_started(k_owner, true, 7);    // Not a codec
  stream_recorder::session_started(k_owner, true, -1);
  auto after = stream_recorder::detail::replay_stats_at(t0);
  EXPECT_EQ(after.packets, 1u);
  EXPECT_EQ(after.selected_generation, k_owner);
  EXPECT_EQ(after.last_window_limit, before.last_window_limit);

  // The known codec still names a new file.
  stream_recorder::start_recording();
  EXPECT_EQ(fs::path {status().file}.filename(), std::string {"polaris_"} + k_stamp + ".hevc");
  EXPECT_EQ(continuous().owner_generation, k_owner);

  // Learning a codec alone does not reset the replay run.
  stream_recorder::stop_recording();
  stream_recorder::session_started(k_owner, true, k_av1);
  after = stream_recorder::detail::replay_stats_at(t0);
  EXPECT_EQ(after.packets, 1u);
  EXPECT_EQ(after.last_window_limit, before.last_window_limit);
}

TEST_F(StreamRecorderContinuousOwnershipTests, InvalidOversizedAndRegressedOwnerPacketsRestartAtAKeyframe) {
  stream_recorder::session_started(k_owner, true, k_hevc);
  stream_recorder::start_recording();
  const std::string one {"1"};
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(one.data());
  push(k_owner, true, t0, "A");

  // None of these is read: a null pointer, a size above the cap, no size, an unknown codec.
  stream_recorder::detail::push_packet_at(nullptr, 5, true, true, 0, k_owner, k_hevc, t0 + 1ms);
  EXPECT_EQ(continuous().state, continuous_state_e::awaiting_keyframe);
  EXPECT_EQ(continuous().cause, continuous_cause_e::discontinuity);
  push(k_owner, false, t0 + 2ms, "b");
  push(k_owner, true, t0 + 3ms, "C");
  stream_recorder::detail::push_packet_at(bytes, stream_recorder::k_continuous_max_packet_bytes + 1, true, true, 0, k_owner, k_hevc, t0 + 4ms);
  stream_recorder::detail::push_packet_at(bytes, 0, true, true, 0, k_owner, k_hevc, t0 + 4ms);
  stream_recorder::detail::push_packet_at(bytes, 1, true, true, 0, k_owner, -1, t0 + 4ms);
  push(k_owner, false, t0 + 5ms, "c");
  push(k_owner, true, t0 + 6ms, "D");

  // A clock regression drops that packet, even a keyframe, then restarts at the next keyframe.
  push(k_owner, true, t0 + 1ms, "E");
  EXPECT_EQ(continuous().state, continuous_state_e::awaiting_keyframe);
  push(k_owner, false, t0 + 1ms, "f");
  push(k_owner, true, t0 + 1ms, "G");
  push(k_owner, false, t0 + 2ms, "h");

  EXPECT_EQ(written(), "ACDGh");
  const auto s = continuous();
  EXPECT_EQ(s.refused_invalid, 4u);
  EXPECT_EQ(s.discontinuities, 5u);
  EXPECT_EQ(s.refused_awaiting_keyframe, 3u);
  EXPECT_EQ(s.packets_written, 5u);
  EXPECT_EQ(s.state, continuous_state_e::writing);
}

TEST_F(StreamRecorderContinuousOwnershipTests, ModeOutputAndConfigurationChangesNeverStartOrRedirectTheFile) {
  // Loading a mode never starts a recording, continuous included, and packets go nowhere.
  stream_recorder::session_started(k_owner, true, k_hevc);
  publish(true, false);
  EXPECT_EQ(stream_recorder::current_mode(), stream_recorder::mode_t::continuous);
  push(k_owner, true, t0, "K");
  expect_idle();
  EXPECT_FALSE(fs::exists(output_dir));

  // A manual start works in the disabled default, and repeating it changes nothing.
  publish(false, false);
  stream_recorder::start_recording();
  const auto path = status().file;
  ASSERT_FALSE(path.empty());
  stream_recorder::start_recording();
  EXPECT_EQ(status().file, path);
  push(k_owner, true, t0 + 1ms, "A");

  const auto moved = owned_parent / "moved";
  config::recording.output_dir = moved.string();
  for (const auto &[enabled, replay] : {std::pair {true, false}, std::pair {true, true}, std::pair {false, true}, std::pair {false, false}}) {
    publish(enabled, replay);
    const auto s = status();
    EXPECT_TRUE(s.recording);
    EXPECT_EQ(s.file, path);
    EXPECT_EQ(s.continuous.owner_generation, k_owner);
  }
  push(k_owner, false, t0 + 2ms, "b");
  EXPECT_EQ(read_file(path), "Ab");
  EXPECT_FALSE(fs::exists(moved));

  stream_recorder::stop_recording();
  stream_recorder::stop_recording();
  expect_idle();
  EXPECT_EQ(read_file(path), "Ab");
}

TEST_F(StreamRecorderContinuousOwnershipTests, SameStampStartsNeverReplaceAnEarlierFile) {
  stream_recorder::session_started(k_owner, true, k_hevc);
  std::vector<std::string> paths;
  for (const std::string bytes : {"A", "B", "C"}) {
    stream_recorder::start_recording();
    paths.push_back(status().file);
    push(k_owner, true, t0, bytes);
    stream_recorder::stop_recording();
  }
  const std::string stem = std::string {"polaris_"} + k_stamp;
  EXPECT_EQ(output_names(), (std::vector<std::string> {stem + "-1.hevc", stem + "-2.hevc", stem + ".hevc"}));
  EXPECT_EQ(read_file(paths[0]), "A");
  EXPECT_EQ(read_file(paths[1]), "B");
  EXPECT_EQ(read_file(paths[2]), "C");
}

#ifndef _WIN32
TEST_F(StreamRecorderContinuousOwnershipTests, AnExistingSymlinkIsNeverFollowedAndFilesArePrivate) {
  ASSERT_TRUE(fs::create_directories(output_dir));
  const auto victim = owned_parent / "victim";
  std::ofstream(victim, std::ios::binary) << "victim";
  const auto link = output_dir / (std::string {"polaris_"} + k_stamp + ".hevc");
  fs::create_symlink(victim, link);

  stream_recorder::session_started(k_owner, true, k_hevc);
  stream_recorder::start_recording();
  const auto path = fs::path {status().file};
  EXPECT_EQ(path.filename(), std::string {"polaris_"} + k_stamp + "-1.hevc");
  push(k_owner, true, t0, "A");
  EXPECT_EQ(read_file(victim), "victim");
  EXPECT_TRUE(fs::is_symlink(link));
  EXPECT_EQ(fs::status(path).permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write);
  EXPECT_EQ(read_file(path), "A");
}
#endif

TEST_F(StreamRecorderContinuousOwnershipTests, AFailedOpenLeavesNoFileAndNoState) {
  stream_recorder::session_started(k_owner, true, k_hevc);

  // An allocation failure before anything is created.
  hooks.before_open = [](const fs::path &) { throw std::bad_alloc {}; };
  install();
  stream_recorder::start_recording();
  expect_idle();
  EXPECT_TRUE(output_names().empty());

  // An open error other than "exists" stops at once.
  hooks.before_open = {};
  hooks.open = [this](const fs::path &) {
    ++opens;
    errno = EACCES;
    return -1;
  };
  install();
  stream_recorder::start_recording();
  expect_idle();
  EXPECT_EQ(opens, 1);

  // Every candidate taken: bounded attempts, then nothing.
  opens = 0;
  hooks.open = [this](const fs::path &) {
    ++opens;
    errno = EEXIST;
    return -1;
  };
  install();
  stream_recorder::start_recording();
  expect_idle();
  EXPECT_EQ(opens, 100);

  // A failure after the exclusive open closes and removes the file it created.
  hooks.open = {};
  hooks.after_open = [](const fs::path &) { throw std::bad_alloc {}; };
  install();
  stream_recorder::start_recording();
  expect_idle();
  EXPECT_TRUE(output_names().empty());

  // Nothing is left that blocks a clean start under the first name.
  hooks.after_open = {};
  install();
  stream_recorder::start_recording();
  EXPECT_EQ(fs::path {status().file}.filename(), std::string {"polaris_"} + k_stamp + ".hevc");
  push(k_owner, true, t0, "A");
  EXPECT_EQ(written(), "A");
}

TEST_F(StreamRecorderContinuousOwnershipTests, InterruptedAndPartialWritesCompleteThePacket) {
  stream_recorder::session_started(k_owner, true, k_hevc);
  hooks.write = [this](int fd, const std::uint8_t *data, std::size_t size) -> long long {
    switch (writes++) {
      case 0:
        errno = EINTR;
        return -1;
      case 1:
        return stream_recorder::detail::native_write(fd, data, 1);
      case 2:
        return stream_recorder::detail::native_write(fd, data, 2);
      default:
        return stream_recorder::detail::native_write(fd, data, size);
    }
  };
  install();
  stream_recorder::start_recording();
  push(k_owner, true, t0, "ABCDE");
  push(k_owner, false, t0 + 1ms, "f");
  EXPECT_EQ(written(), "ABCDEf");
  EXPECT_EQ(writes, 5);
  const auto s = continuous();
  EXPECT_EQ(s.state, continuous_state_e::writing);
  EXPECT_EQ(s.packets_written, 2u);
  EXPECT_EQ(s.bytes_written, 6u);
  EXPECT_EQ(s.error, continuous_error_e::none);
}

TEST_F(StreamRecorderContinuousOwnershipTests, TheFirstWriteFailureFencesTheFileUntilStop) {
  stream_recorder::session_started(k_owner, true, k_hevc);
  struct case_t {
    long long result;
    int error;
    continuous_error_e expected;
  };
  // A failure, no progress, and a result larger than asked; each after two bytes went through.
  for (const auto &c : {case_t {-1, ENOSPC, continuous_error_e::write}, case_t {0, 0, continuous_error_e::no_progress},
                        case_t {1000, 0, continuous_error_e::no_progress}}) {
    SCOPED_TRACE(static_cast<int>(c.expected));
    writes = 0;
    hooks.write = [this, c](int fd, const std::uint8_t *data, std::size_t) -> long long {
      if (writes++ == 0) {
        return stream_recorder::detail::native_write(fd, data, 2);
      }
      errno = c.error;
      return c.result;
    };
    install();
    stream_recorder::start_recording();
    const auto path = status().file;
    push(k_owner, true, t0, "ABCD");
    auto s = continuous();
    EXPECT_EQ(s.state, continuous_state_e::failed);
    EXPECT_EQ(s.cause, continuous_cause_e::storage);
    EXPECT_EQ(s.error, c.expected);
    EXPECT_EQ(s.error_number, c.expected == continuous_error_e::write ? ENOSPC : 0);
    EXPECT_EQ(s.packets_written, 0u);
    EXPECT_EQ(s.bytes_written, 2u);
    // Fenced: nothing more is attempted, and lifecycle changes do not revive it.
    push(k_owner, true, t0 + 1ms, "E");
    stream_recorder::session_started(k_other, true, k_hevc);
    stream_recorder::session_ended(k_other);
    push(k_owner, true, t0 + 2ms, "F");
    EXPECT_EQ(writes, 2);
    s = continuous();
    EXPECT_EQ(s.state, continuous_state_e::failed);
    // The file stays open and reported until stop; its torn prefix stays as written.
    EXPECT_TRUE(stream_recorder::is_recording());
    EXPECT_EQ(status().file, path);
    stream_recorder::stop_recording();
    expect_idle();
    EXPECT_EQ(read_file(path), "AB");
  }
}

TEST_F(StreamRecorderContinuousOwnershipTests, StopReleasesTheFileBeforeClosingItOnce) {
  stream_recorder::session_started(k_owner, true, k_hevc);
  hooks.close = [this](int fd) {
    ++closes;
    closed_fd = fd;
    // stop_recording() calls this with no recorder lock, after releasing the file's state.
    seen_at_close = stream_recorder::recorder_status();
    stream_recorder::detail::native_close(fd);
    errno = EIO;
    return -1;
  };
  install();
  stream_recorder::start_recording();
  push(k_owner, true, t0, "A");
  const auto path = status().file;
  stream_recorder::stop_recording();
  EXPECT_EQ(closes, 1);
  EXPECT_GE(closed_fd, 0);
  EXPECT_FALSE(seen_at_close.recording);
  EXPECT_TRUE(seen_at_close.file.empty());
  EXPECT_EQ(seen_at_close.continuous.state, continuous_state_e::idle);
  // A close error is reported only; the recorder is idle and a second stop closes nothing.
  expect_idle();
  stream_recorder::stop_recording();
  EXPECT_EQ(closes, 1);
  EXPECT_EQ(read_file(path), "A");
}

TEST_F(StreamRecorderContinuousOwnershipTests, StatusPairsTheFileWithItsOwnershipInOneSnapshot) {
  // Exercises the lock while another thread opens and closes files; not a race-freedom proof.
  stream_recorder::session_started(k_owner, true, k_hevc);
  std::atomic<bool> done {false};
  int inconsistent = 0;
  int reads = 0;
  {
    std::jthread toggler([&] {
      for (int i = 0; i < 40; ++i) {
        stream_recorder::start_recording();
        stream_recorder::stop_recording();
      }
      done.store(true);
    });
    do {
      const auto s = stream_recorder::recorder_status();
      const bool open = s.continuous.state != continuous_state_e::idle;
      inconsistent += s.recording == s.file.empty() || open != s.recording ||
                      (s.recording && s.continuous.owner_generation != k_owner);
      ++reads;
    } while (!done.load());
  }
  EXPECT_EQ(inconsistent, 0);
  EXPECT_GE(reads, 1);
  expect_idle();
}
