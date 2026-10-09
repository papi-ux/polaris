/**
 * @file tests/unit/test_stream_recorder_provenance.cpp
 * @brief Test passive packet provenance in src/stream_recorder.* (#359): the frame index,
 * frame_timestamp and source-coordinate flags a packet carried are kept beside each replay
 * payload, live and in a held save snapshot, with evidence marks, and change nothing else.
 *
 * Every byte here is synthetic and never decoded. Nothing in these tests is a presentation
 * timestamp: the times are fixture values recorded and compared exactly. Saves write only into a
 * directory this fixture created.
 */

#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using steady = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  using stream_recorder::packet_provenance_t;
  using stream_recorder::detail::provenance_marks_t;
  using stream_recorder::detail::save_status_e;

  constexpr std::uint64_t k_session = 359;
  constexpr std::uint64_t k_other_session = 360;
  constexpr int k_hevc = 1;
  constexpr int k_av1 = 2;
  constexpr auto k_stamp = "20261009_150000";
  constexpr std::int64_t k_min = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t k_max = std::numeric_limits<std::int64_t>::max();

  packet_provenance_t provenance(std::optional<std::int64_t> index, std::optional<steady::time_point> time = std::nullopt,
                                 bool source_clock = false, bool paced = false) {
    packet_provenance_t p;
    p.frame_index = index;
    p.frame_timestamp = time;
    p.source_clock_timestamp = source_clock;
    p.source_time_paced = paced;
    return p;
  }

  provenance_marks_t marks(std::initializer_list<bool provenance_marks_t::*> set) {
    provenance_marks_t m;
    for (auto member : set) {
      m.*member = true;
    }
    return m;
  }

  /// The raw-buffer facts a provenance record must never change, as one comparable list.
  std::vector<std::uint64_t> raw_facts(const stream_recorder::replay_stats_t &s) {
    return {
      s.packets,
      s.bytes,
      s.selected_generation,
      static_cast<std::uint64_t>(s.run_video_format + 1),
      s.max_bytes,
      s.max_packets,
      s.refused_unselected,
      s.refused_invalid,
      s.refused_oversized,
      s.refused_awaiting_keyframe,
      s.trimmed,
      s.discontinuities,
      s.save_in_progress,
      s.snapshot_packets,
      s.snapshot_bytes,
      s.metadata_entries,
      s.keyframe_start_run,
      s.run_keyframes,
      static_cast<std::uint64_t>(s.run_span.count()),
      s.awaiting_keyframe,
      static_cast<std::uint64_t>(s.last_window_limit),
      s.saves_completed,
      s.saves_failed,
    };
  }

  /**
   * Each test starts from the recorder's process start state, in replay mode with one eligible
   * session, saving into a `recordings` directory inside a parent this fixture created
   * exclusively. TearDown resets the recorder (releasing any open state) and removes only that parent.
   */
  class StreamRecorderProvenanceTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved = config::recording;
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-provenance-"} + info->name() + "-" +
                                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                                       std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directory(candidate, ec)) {
          owned_parent = candidate;
        }
      }
      ASSERT_FALSE(owned_parent.empty()) << "no exclusive temporary directory";
      output_dir = owned_parent / "recordings";
      config::recording.output_dir = output_dir.string();
      start_replay();
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

    /// Process start state, replay mode, one eligible session, fixed save names.
    void start_replay() {
      stream_recorder::detail::reset_replay_for_tests();
      config::recording.replay_buffer_minutes = 1;
      publish(true, true);
      stream_recorder::session_started(k_session, true);
      install();
    }

    void install() {
      hooks.name_stamp = [] { return std::string {k_stamp}; };
      stream_recorder::detail::set_save_hooks_for_tests(hooks);
    }

    static void publish(bool enabled, bool replay_buffer) {
      config::recording.enabled = enabled;
      config::recording.replay_buffer = replay_buffer;
      stream_recorder::load_config();
    }

    static std::vector<std::uint8_t> bytes(std::size_t size, std::uint8_t fill) {
      return std::vector<std::uint8_t>(size, fill);
    }

    static void legacy(bool keyframe, steady::time_point at, const std::vector<std::uint8_t> &payload,
                       std::uint64_t generation = k_session, int codec = k_hevc) {
      stream_recorder::detail::push_packet_at(payload.data(), payload.size(), true, keyframe, 0, generation, codec, at);
    }

    static void video(bool keyframe, steady::time_point at, const std::vector<std::uint8_t> &payload,
                      const packet_provenance_t &p, std::uint64_t generation = k_session, int codec = k_hevc) {
      stream_recorder::detail::push_video_at(payload.data(), payload.size(), keyframe, generation, codec, p, at);
    }

    static std::vector<stream_recorder::detail::provenance_entry_t> live() {
      return stream_recorder::detail::replay_provenance_for_tests();
    }

    static stream_recorder::replay_stats_t stats(steady::time_point at) {
      return stream_recorder::detail::replay_stats_at(at);
    }

    static std::string read_file(const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    static std::string joined(const std::vector<stream_recorder::detail::provenance_entry_t> &entries) {
      std::string out;
      for (const auto &e : entries) {
        out.append(e.payload.begin(), e.payload.end());
      }
      return out;
    }

    config::recording_t saved;
    fs::path owned_parent;
    fs::path output_dir;
    stream_recorder::detail::save_hooks_t hooks;
    const steady::time_point t0 = steady::time_point {} + 1h;
  };

  /// One step of a scripted sequence, pushed the legacy way or with provenance.
  struct step_t {
    bool keyframe;
    std::chrono::milliseconds at;
    std::size_t size;  ///< 0 pushes a null payload (an invalid packet)
    std::uint8_t fill;
    packet_provenance_t provenance;
    std::uint64_t generation = k_session;
    int codec = k_hevc;
  };
}  // namespace

// Every raw fact (retention, refusals, caps, trimming, alignment, resets, saved bytes) is the same
// whether the packets carry provenance or not, including provenance full of marks.
TEST_F(StreamRecorderProvenanceTests, ProvenanceLeavesRawRetentionRefusalsCapsResetsAndSavedBytesUnchanged) {
  const steady::time_point ts = steady::time_point {} + 5h;  // Deliberately unrelated to arrival
  const std::vector<step_t> script {
    {false, 0ms, 8, 'a', provenance(0, ts)},                       // Refused: awaiting a keyframe
    {true, 1ms, 10, 'K', provenance(1, ts + 1ms)},
    {false, 2ms, 10, 'b', provenance(1, ts + 1ms)},                // Duplicate index and time
    {false, 3ms, 10, 'c', provenance(0, std::nullopt)},            // Regressed index, untimed
    {false, 4ms, 10, 'd', provenance(9, ts)},                      // Gap, regressed time
    {false, 5ms, 10, 'e', provenance(std::nullopt, ts + 5ms)},     // Unsupplied index
    {false, 6ms, 10, 'f', provenance(11, ts + 6ms, true, true)},   // Packet cap: trims and realigns, then refused
    {true, 7ms, 10, 'L', provenance(12, ts + 7ms)},
    {false, 8ms, 10, 'g', provenance(13, ts + 8ms), k_other_session},  // Refused: unselected
    {false, 9ms, 0, 0, provenance(14, ts + 9ms)},                  // Invalid: discontinuity
    {true, 10ms, 10, 'M', provenance(15, ts + 10ms)},
    {false, 11ms, 10, 'h', provenance(16, ts + 11ms)},
    {false, 5ms, 10, 'i', provenance(17, ts + 12ms)},              // Arrival regression: discontinuity, then refused
    {true, 12ms, 40, 'N', provenance(18, ts + 13ms)},
    {false, 13ms, 30, 'j', provenance(19, ts + 14ms)},             // Payload cap: trims N, then refused
    {true, 14ms, 20, 'O', provenance(20, ts + 15ms)},
    {false, 15ms, 100, 'k', provenance(21, ts + 16ms)},            // Oversized: discontinuity
    {true, 16ms, 10, 'P', provenance(22, ts + 17ms)},
    {false, 17ms, 10, 'l', provenance(23, ts + 18ms)},
    {true, 18ms, 10, 'Q', provenance(24, ts + 19ms), k_session, k_av1},  // Codec change: discontinuity
    {false, 19ms, 10, 'm', provenance(25, ts + 20ms), k_session, k_av1},
  };
  const auto run = [&](bool with_provenance) {
    start_replay();
    stream_recorder::detail::set_replay_limits_for_tests(64, 5);
    for (const auto &s : script) {
      const auto payload = bytes(s.size, s.fill);
      const auto *data = s.size ? payload.data() : nullptr;
      if (with_provenance) {
        stream_recorder::detail::push_video_at(data, s.size, s.keyframe, s.generation, s.codec, s.provenance, t0 + s.at);
      } else {
        stream_recorder::detail::push_packet_at(data, s.size, true, s.keyframe, 0, s.generation, s.codec, t0 + s.at);
      }
    }
    const auto before_save = raw_facts(stats(t0 + 20ms));
    const auto contents = joined(live());
    const auto result = stream_recorder::detail::save_replay_result_at(t0 + 20ms);
    EXPECT_EQ(result.status, save_status_e::saved);
    const auto file = read_file(result.path);
    std::error_code ec;
    fs::remove_all(output_dir, ec);  // The next run may reuse the same name
    return std::make_tuple(before_save, raw_facts(stats(t0 + 20ms)), contents, file, result.packets, result.bytes,
                           result.keyframes, result.span);
  };
  const auto legacy_run = run(false);
  const auto provenance_run = run(true);
  EXPECT_EQ(std::get<0>(legacy_run), std::get<0>(provenance_run));
  EXPECT_EQ(std::get<1>(legacy_run), std::get<1>(provenance_run));
  EXPECT_EQ(std::get<2>(legacy_run), std::get<2>(provenance_run));
  EXPECT_EQ(std::get<3>(legacy_run), std::get<3>(provenance_run));
  EXPECT_EQ(std::get<4>(legacy_run), std::get<4>(provenance_run));
  EXPECT_EQ(std::get<5>(legacy_run), std::get<5>(provenance_run));
  EXPECT_EQ(std::get<6>(legacy_run), std::get<6>(provenance_run));
  EXPECT_EQ(std::get<7>(legacy_run), std::get<7>(provenance_run));
  // The script exercised every raw path it claims to.
  const auto &facts = std::get<0>(legacy_run);
  EXPECT_EQ(std::get<3>(legacy_run), std::string(10, 'Q') + std::string(10, 'm'));
  EXPECT_EQ(facts[6], 1u);   // refused_unselected
  EXPECT_EQ(facts[7], 1u);   // refused_invalid
  EXPECT_EQ(facts[8], 1u);   // refused_oversized
  EXPECT_EQ(facts[9], 4u);   // refused_awaiting_keyframe: first frame, two after realignment, one after a reset
  EXPECT_GT(facts[10], 0u);  // trimmed
  EXPECT_EQ(facts[11], 4u);  // discontinuities: invalid, arrival regression, oversized, codec
}

// An unsupplied index (legacy push_packet(), whatever its pts) is not index 0, and 0 is a real
// index. Marks compare only adjacent retained entries: an unsupplied neighbour is never compared.
TEST_F(StreamRecorderProvenanceTests, AnUnsuppliedIndexIsDistinctFromARealIndexZero) {
  legacy(true, t0, bytes(4, 'a'));                                   // pts 0, but unsupplied
  video(false, t0 + 1ms, bytes(4, 'b'), provenance(0));              // Real 0; neighbour unsupplied: not compared
  video(false, t0 + 2ms, bytes(4, 'c'), provenance(0));              // Real 0 after real 0: duplicate
  legacy(false, t0 + 3ms, bytes(4, 'd'));                            // Unsupplied
  video(false, t0 + 4ms, bytes(4, 'e'), provenance(0));              // Neighbour unsupplied: not compared
  video(false, t0 + 5ms, bytes(4, 'f'), provenance(1));              // After 0: no mark
  const auto entries = live();
  ASSERT_EQ(entries.size(), 6u);
  const auto untimed = &provenance_marks_t::time_missing;
  EXPECT_FALSE(entries[0].record.supplied.frame_index.has_value());
  EXPECT_EQ(entries[0].record.marks, marks({&provenance_marks_t::index_unsupplied, untimed}));
  ASSERT_TRUE(entries[1].record.supplied.frame_index.has_value());
  EXPECT_EQ(*entries[1].record.supplied.frame_index, 0);
  EXPECT_EQ(entries[1].record.marks, marks({untimed}));
  EXPECT_EQ(entries[2].record.marks, marks({&provenance_marks_t::index_duplicate, untimed}));
  EXPECT_FALSE(entries[3].record.supplied.frame_index.has_value());
  EXPECT_EQ(entries[3].record.marks, marks({&provenance_marks_t::index_unsupplied, untimed}));
  EXPECT_EQ(entries[4].record.marks, marks({untimed}));  // Not a duplicate of the 0 two entries back
  EXPECT_EQ(entries[5].record.marks, marks({untimed}));
  for (const auto &e : entries) {
    EXPECT_EQ(e.record.version, packet_provenance_t::k_version);
  }
}

// Duplicates, regressions and gaps are marked against the adjacent retained entry and change nothing:
// an encoder may drop frames, so a gap is evidence, not a cut.
TEST_F(StreamRecorderProvenanceTests, IndexDuplicatesRegressionsAndGapsAreMarkedWithoutCuttingTheRun) {
  const auto before = stats(t0);
  ASSERT_EQ(before.last_window_limit, stream_recorder::window_limit_e::selection);
  const std::vector<std::int64_t> indices {10, 11, 11, 9, 12, 13, 20};
  for (std::size_t i = 0; i < indices.size(); ++i) {
    video(i == 0, t0 + std::chrono::milliseconds(i), bytes(4, static_cast<std::uint8_t>('a' + i)), provenance(indices[i]));
  }
  const auto entries = live();
  ASSERT_EQ(entries.size(), indices.size());
  const auto untimed = &provenance_marks_t::time_missing;
  EXPECT_EQ(entries[0].record.marks, marks({untimed}));
  EXPECT_EQ(entries[1].record.marks, marks({untimed}));
  EXPECT_EQ(entries[2].record.marks, marks({&provenance_marks_t::index_duplicate, untimed}));
  EXPECT_EQ(entries[3].record.marks, marks({&provenance_marks_t::index_regressed, untimed}));
  EXPECT_EQ(entries[4].record.marks, marks({&provenance_marks_t::index_gap, untimed}));  // 9 -> 12
  EXPECT_EQ(entries[5].record.marks, marks({untimed}));
  EXPECT_EQ(entries[6].record.marks, marks({&provenance_marks_t::index_gap, untimed}));  // 13 -> 20
  const auto s = stats(t0 + 10ms);
  EXPECT_EQ(s.packets, indices.size());
  EXPECT_EQ(s.discontinuities, 0u);
  EXPECT_EQ(s.trimmed, 0u);
  EXPECT_EQ(s.last_window_limit, before.last_window_limit);
}

// Comparisons at both ends of int64 never overflow and still classify exactly.
TEST_F(StreamRecorderProvenanceTests, IndexComparisonsAtTheSignedEndpointsNeverOverflow) {
  const std::vector<std::int64_t> indices {k_min, k_min + 1, k_max, k_max, k_min, -1, 0, k_max - 1, k_max};
  for (std::size_t i = 0; i < indices.size(); ++i) {
    video(i == 0, t0 + std::chrono::milliseconds(i), bytes(2, 'x'), provenance(indices[i], t0 + std::chrono::milliseconds(i)));
  }
  const auto entries = live();
  ASSERT_EQ(entries.size(), indices.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    ASSERT_TRUE(entries[i].record.supplied.frame_index.has_value());
    EXPECT_EQ(*entries[i].record.supplied.frame_index, indices[i]);
  }
  EXPECT_EQ(entries[0].record.marks, provenance_marks_t {});
  EXPECT_EQ(entries[1].record.marks, provenance_marks_t {});                                     // min -> min+1
  EXPECT_EQ(entries[2].record.marks, marks({&provenance_marks_t::index_gap}));                   // min+1 -> max
  EXPECT_EQ(entries[3].record.marks, marks({&provenance_marks_t::index_duplicate}));             // max -> max
  EXPECT_EQ(entries[4].record.marks, marks({&provenance_marks_t::index_regressed}));             // max -> min
  EXPECT_EQ(entries[5].record.marks, marks({&provenance_marks_t::index_gap}));                   // min -> -1
  EXPECT_EQ(entries[6].record.marks, provenance_marks_t {});                                     // -1 -> 0
  EXPECT_EQ(entries[7].record.marks, marks({&provenance_marks_t::index_gap}));                   // 0 -> max-1
  EXPECT_EQ(entries[8].record.marks, provenance_marks_t {});                                     // max-1 -> max
}

// The optional frame_timestamp and both source flags round-trip exactly; an absent time stays
// absent (never the arrival time), and time marks compare only adjacent known times.
TEST_F(StreamRecorderProvenanceTests, TimestampsAndSourceFlagsRoundTripExactlyAndAbsentTimesStayAbsent) {
  const steady::time_point far = steady::time_point {} + 7h + 123ns;  // Not an arrival time
  const std::vector<packet_provenance_t> supplied {
    provenance(1, far, false, false),
    provenance(2, far, false, true),                 // Same time as its neighbour: duplicate
    provenance(3, std::nullopt, true, false),        // Untimed
    provenance(4, far - 1ns, true, true),            // Earlier than far, but its neighbour is untimed
    provenance(5, far - 2ns, false, false),          // Earlier than its neighbour: regressed
    provenance(6, far + 1s, true, false),
  };
  for (std::size_t i = 0; i < supplied.size(); ++i) {
    video(i == 0, t0 + std::chrono::milliseconds(i), bytes(3, static_cast<std::uint8_t>(i)), supplied[i]);
  }
  const auto entries = live();
  ASSERT_EQ(entries.size(), supplied.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    SCOPED_TRACE(i);
    const auto &got = entries[i].record.supplied;
    EXPECT_EQ(got.frame_index, supplied[i].frame_index);
    EXPECT_EQ(got.frame_timestamp, supplied[i].frame_timestamp);
    EXPECT_EQ(got.source_clock_timestamp, supplied[i].source_clock_timestamp);
    EXPECT_EQ(got.source_time_paced, supplied[i].source_time_paced);
  }
  EXPECT_EQ(entries[0].record.marks, provenance_marks_t {});
  EXPECT_EQ(entries[1].record.marks, marks({&provenance_marks_t::time_duplicate}));
  EXPECT_EQ(entries[2].record.marks, marks({&provenance_marks_t::time_missing}));
  EXPECT_EQ(entries[3].record.marks, provenance_marks_t {});  // Neighbour untimed: not compared
  EXPECT_EQ(entries[4].record.marks, marks({&provenance_marks_t::time_regressed}));
  EXPECT_EQ(entries[5].record.marks, provenance_marks_t {});
}

// A run clear empties the buffer: the next run's first entry is compared with nothing.
TEST_F(StreamRecorderProvenanceTests, ARunResetStartsFreshBaselinesWithoutChangingTheReset) {
  video(true, t0, bytes(4, 'a'), provenance(50, t0));
  video(false, t0 + 1ms, bytes(4, 'b'), provenance(51, t0 + 1ms));
  stream_recorder::detail::push_packet_at(nullptr, 0, true, false, 0, k_session, k_hevc, t0 + 2ms);  // Invalid: reset
  video(true, t0 + 3ms, bytes(4, 'c'), provenance(10, t0 - 1s));  // Lower than before the reset
  const auto entries = live();
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].record.marks, provenance_marks_t {});
  EXPECT_EQ(stats(t0 + 3ms).discontinuities, 1u);
}

// Duration expiry that empties the buffer leaves nothing adjacent: the next entry is compared with
// nothing, though its index and time are lower than the expired entry's. Raw facts are those of
// the same pushes without provenance.
TEST_F(StreamRecorderProvenanceTests, ExpiryThatEmptiesTheBufferLeavesTheNextEntryUncompared) {
  const auto script = [&](bool with_provenance) {
    start_replay();  // One-minute window
    const auto push = [&](bool keyframe, steady::time_point at, char fill, const packet_provenance_t &p) {
      with_provenance ? video(keyframe, at, bytes(6, fill), p) : legacy(keyframe, at, bytes(6, fill));
    };
    push(true, t0, 'a', provenance(10, t0));
    push(false, t0 + 1ms, 'b', provenance(11, t0 + 1ms));
    // Two minutes later both have expired before this keyframe is stored.
    push(true, t0 + 2min, 'c', provenance(3, t0 - 1s));
    push(false, t0 + 2min + 1ms, 'd', provenance(3, t0 - 1s));  // Adjacent duplicate, for contrast
    return std::make_pair(raw_facts(stats(t0 + 2min + 1ms)), live());
  };
  const auto without = script(false);
  const auto with = script(true);
  EXPECT_EQ(with.first, without.first);
  ASSERT_EQ(with.second.size(), 2u);
  ASSERT_EQ(without.second.size(), 2u);
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(with.second[i].payload, without.second[i].payload);
  }
  EXPECT_EQ(with.second[0].record.marks, provenance_marks_t {});  // Nothing adjacent after expiry
  EXPECT_EQ(with.second[1].record.marks,
            marks({&provenance_marks_t::index_duplicate, &provenance_marks_t::time_duplicate}));
  const auto &facts = without.first;
  EXPECT_EQ(facts[0], 2u);                // packets
  EXPECT_EQ(facts[10], 2u);               // trimmed: both expired
  EXPECT_EQ(facts[11], 0u);               // no discontinuity
  EXPECT_EQ(facts[20], static_cast<std::uint64_t>(stream_recorder::window_limit_e::duration));
}

// A cap trim whose keyframe realignment empties the buffer leaves nothing adjacent either: the
// next stored keyframe is compared with nothing, while an unemptied trim still compares.
TEST_F(StreamRecorderProvenanceTests, ACapTrimThatRealignsToEmptyLeavesTheNextEntryUncompared) {
  const auto script = [&](bool with_provenance) {
    start_replay();
    stream_recorder::detail::set_replay_limits_for_tests(1024, 3);
    const auto push = [&](bool keyframe, int ms, char fill, const packet_provenance_t &p) {
      const auto at = t0 + std::chrono::milliseconds(ms);
      with_provenance ? video(keyframe, at, bytes(4, fill), p) : legacy(keyframe, at, bytes(4, fill));
    };
    push(true, 0, 'K', provenance(20, t0));
    push(false, 1, 'a', provenance(21, t0 + 1ms));
    push(false, 2, 'b', provenance(22, t0 + 2ms));
    // The packet cap trims K, alignment drops a and b, and the dependent frame is refused.
    push(false, 3, 'c', provenance(23, t0 + 3ms));
    // Stored into the empty buffer: lower index and time than anything seen, yet unmarked.
    push(true, 4, 'L', provenance(1, t0 - 1h));
    // An adjacent entry exists: compared, and regressed.
    push(false, 5, 'd', provenance(0, t0 - 2h));
    return std::make_pair(raw_facts(stats(t0 + 5ms)), live());
  };
  const auto without = script(false);
  const auto with = script(true);
  EXPECT_EQ(with.first, without.first);
  ASSERT_EQ(with.second.size(), 2u);
  ASSERT_EQ(without.second.size(), 2u);
  for (std::size_t i = 0; i < 2; ++i) {
    EXPECT_EQ(with.second[i].payload, without.second[i].payload);
  }
  EXPECT_EQ(with.second[0].record.marks, provenance_marks_t {});
  EXPECT_EQ(with.second[1].record.marks,
            marks({&provenance_marks_t::index_regressed, &provenance_marks_t::time_regressed}));
  const auto &facts = without.first;
  EXPECT_EQ(facts[0], 2u);   // packets: L, d
  EXPECT_EQ(facts[9], 1u);   // refused_awaiting_keyframe: c
  EXPECT_EQ(facts[10], 3u);  // trimmed: K (cap), a and b (alignment)
  EXPECT_EQ(facts[11], 0u);  // no discontinuity
}

// Trimming removes each payload together with its own provenance; the pairing is exact.
TEST_F(StreamRecorderProvenanceTests, TrimmingDropsEachPayloadTogetherWithItsProvenance) {
  stream_recorder::detail::set_replay_limits_for_tests(1024, 4);
  for (std::int64_t i = 0; i < 11; ++i) {
    // Keyframes every third frame, so cap trimming also realigns to a keyframe.
    video(i % 3 == 0, t0 + std::chrono::milliseconds(i), bytes(5, static_cast<std::uint8_t>(i)), provenance(i, t0 + std::chrono::milliseconds(i)));
  }
  const auto entries = live();
  ASSERT_FALSE(entries.empty());
  EXPECT_LE(entries.size(), 4u);
  for (const auto &e : entries) {
    ASSERT_EQ(e.payload.size(), 5u);
    ASSERT_TRUE(e.record.supplied.frame_index.has_value());
    EXPECT_EQ(static_cast<std::int64_t>(e.payload[0]), *e.record.supplied.frame_index);
    EXPECT_EQ(e.record.supplied.frame_timestamp, t0 + std::chrono::milliseconds(e.payload[0]));
  }
  EXPECT_EQ(*entries.front().record.supplied.frame_index % 3, 0);  // Aligned to a keyframe
  EXPECT_EQ(stats(t0 + 11ms).packets, entries.size());
}

// A held save keeps its captured provenance, paired with its payloads, across a live reset, the
// session's end and a configuration reload, until its own lease releases it.
TEST_F(StreamRecorderProvenanceTests, AHeldSnapshotKeepsItsProvenanceAcrossResetSessionEndAndReload) {
  for (std::int64_t i = 0; i < 4; ++i) {
    video(i == 0, t0 + std::chrono::milliseconds(i), bytes(6, static_cast<std::uint8_t>('A' + i)),
          provenance(100 + i, i == 2 ? std::nullopt : std::optional {t0 - 1h + std::chrono::milliseconds(i)}, i == 1, i == 3));
  }
  const auto captured = live();
  auto admission = stream_recorder::detail::admit_replay_save(t0 + 4ms);
  ASSERT_TRUE(admission.result.admitted);
  const auto held = stream_recorder::detail::snapshot_provenance_for_tests(admission.save);

  // The live run changes: new frames, an invalid packet (reset), the session's end and a reload.
  video(false, t0 + 5ms, bytes(6, 'z'), provenance(104));
  stream_recorder::detail::push_packet_at(nullptr, 0, true, false, 0, k_session, k_hevc, t0 + 6ms);
  stream_recorder::session_ended(k_session);
  config::recording.replay_buffer_minutes = 2;
  stream_recorder::load_config();
  EXPECT_TRUE(live().empty());

  const auto after = stream_recorder::detail::snapshot_provenance_for_tests(admission.save);
  ASSERT_EQ(held.payload_count, captured.size());
  ASSERT_EQ(held.provenance_count, captured.size());
  ASSERT_EQ(after.entries.size(), captured.size());
  EXPECT_EQ(after.payload_count, held.payload_count);
  EXPECT_EQ(after.provenance_count, held.provenance_count);
  for (std::size_t i = 0; i < captured.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(after.entries[i].payload, captured[i].payload);
    EXPECT_EQ(after.entries[i].record.version, captured[i].record.version);
    EXPECT_EQ(after.entries[i].record.supplied.frame_index, captured[i].record.supplied.frame_index);
    EXPECT_EQ(after.entries[i].record.supplied.frame_timestamp, captured[i].record.supplied.frame_timestamp);
    EXPECT_EQ(after.entries[i].record.supplied.source_clock_timestamp, captured[i].record.supplied.source_clock_timestamp);
    EXPECT_EQ(after.entries[i].record.supplied.source_time_paced, captured[i].record.supplied.source_time_paced);
    EXPECT_EQ(after.entries[i].record.marks, captured[i].record.marks);
  }
  auto during = stats(t0 + 7ms);
  EXPECT_TRUE(during.save_in_progress);
  EXPECT_EQ(during.snapshot_packets, captured.size());

  // Its own completion stores exactly the captured payloads, then releases everything.
  const auto result = stream_recorder::detail::complete_replay_save(std::move(admission.save));
  ASSERT_EQ(result.status, save_status_e::saved);
  EXPECT_EQ(read_file(result.path), joined(captured));
  const auto released = stats(t0 + 8ms);
  EXPECT_FALSE(released.save_in_progress);
  EXPECT_EQ(released.snapshot_packets, 0u);
  EXPECT_EQ(released.snapshot_bytes, 0u);
  EXPECT_EQ(stream_recorder::detail::snapshot_provenance_for_tests(admission.save).entries.size(), 0u);
}

// An admission failure injected at a seam (a bad_alloc thrown by a test hook; no real allocation
// is made to fail) releases everything and leaves no accounting or reservation behind, at each of
// three boundaries:
// - before the capture state is allocated;
// - between the payload array's reserve and the provenance array's reserve, the partial path the
//   provenance array adds;
// - after both arrays were reserved and filled.
// The live run and its provenance are untouched, and a save admitted right after each one works.
TEST_F(StreamRecorderProvenanceTests, AFailedAdmissionReleasesItsProvenanceWithoutAccountingLeaks) {
  for (std::int64_t i = 0; i < 3; ++i) {
    video(i == 0, t0 + std::chrono::milliseconds(i), bytes(4, static_cast<std::uint8_t>('a' + i)), provenance(i, t0 + std::chrono::milliseconds(i)));
  }
  const auto before = live();
  const auto clear_hooks = [&] {
    hooks.before_admission_allocation = {};
    hooks.before_snapshot_provenance_allocation = {};
    hooks.after_snapshot_payloads = {};
  };
  for (const std::string boundary : {"before_admission_allocation", "before_snapshot_provenance_allocation", "after_snapshot_payloads"}) {
    SCOPED_TRACE(boundary);
    int observed = 0;
    const auto fail = [&observed] {
      ++observed;
      throw std::bad_alloc {};
    };
    clear_hooks();
    if (boundary == "before_admission_allocation") {
      hooks.before_admission_allocation = fail;
    } else if (boundary == "before_snapshot_provenance_allocation") {
      hooks.before_snapshot_provenance_allocation = fail;
    } else {
      hooks.after_snapshot_payloads = fail;
    }
    install();
    const auto counted = stats(t0 + 3ms);
    {
      auto admission = stream_recorder::detail::admit_replay_save(t0 + 3ms);
      EXPECT_EQ(observed, 1);
      EXPECT_FALSE(admission.result.admitted);
      EXPECT_EQ(admission.result.status, save_status_e::storage_error);
      EXPECT_FALSE(static_cast<bool>(admission.save));
    }
    const auto s = stats(t0 + 3ms);
    EXPECT_FALSE(s.save_in_progress);
    EXPECT_EQ(s.snapshot_packets, 0u);
    EXPECT_EQ(s.snapshot_bytes, 0u);
    EXPECT_EQ(s.snapshot_only_bytes, 0u);
    EXPECT_EQ(s.metadata_entries, before.size());
    EXPECT_EQ(s.saves_failed, counted.saves_failed);            // Nothing was admitted
    EXPECT_EQ(s.saves_refused_busy, counted.saves_refused_busy);
    const auto now_live = live();
    ASSERT_EQ(now_live.size(), before.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
      EXPECT_EQ(now_live[i].sequence, before[i].sequence);
      EXPECT_EQ(now_live[i].payload, before[i].payload);
      EXPECT_EQ(now_live[i].record.supplied.frame_index, before[i].record.supplied.frame_index);
      EXPECT_EQ(now_live[i].record.supplied.frame_timestamp, before[i].record.supplied.frame_timestamp);
      EXPECT_EQ(now_live[i].record.marks, before[i].record.marks);
    }
    // The reservation was released: the next admission is not refused as busy, and captures
    // matching payload and provenance arrays.
    clear_hooks();
    install();
    auto admission = stream_recorder::detail::admit_replay_save(t0 + 3ms);
    ASSERT_TRUE(admission.result.admitted);
    const auto held = stream_recorder::detail::snapshot_provenance_for_tests(admission.save);
    EXPECT_EQ(held.provenance_count, before.size());
    EXPECT_EQ(held.payload_count, before.size());
  }  // Each admitted save is dropped unsaved here, releasing its snapshot and reservation.
  const auto end = stats(t0 + 3ms);
  EXPECT_FALSE(end.save_in_progress);
  EXPECT_EQ(end.snapshot_packets, 0u);
  EXPECT_EQ(end.saves_refused_busy, 0u);
}

// Disabled mode keeps nothing: no entry, no provenance, no refusal count.
TEST_F(StreamRecorderProvenanceTests, DisabledModeKeepsNoEntryAndNoProvenance) {
  publish(false, false);
  video(true, t0, bytes(4, 'a'), provenance(0, t0, true, true));
  EXPECT_TRUE(live().empty());
  const auto s = stats(t0);
  EXPECT_EQ(s.packets, 0u);
  EXPECT_EQ(s.refused_unselected + s.refused_invalid + s.refused_oversized + s.refused_awaiting_keyframe, 0u);
  EXPECT_FALSE(stream_recorder::recorder_status().recording);
}

// A continuous file receives the same bytes whether or not packets carry provenance.
TEST_F(StreamRecorderProvenanceTests, ContinuousFileBytesAreTheSameWithOrWithoutProvenance) {
  const auto record_file = [&](bool with_provenance) {
    stream_recorder::detail::reset_replay_for_tests();
    stream_recorder::detail::continuous_hooks_t continuous_hooks;
    continuous_hooks.name_stamp = [with_provenance] { return std::string {with_provenance ? "with" : "without"}; };
    stream_recorder::detail::set_continuous_hooks_for_tests(continuous_hooks);
    publish(true, false);
    stream_recorder::session_started(k_session, true, k_hevc);
    stream_recorder::start_recording();
    EXPECT_TRUE(stream_recorder::is_recording());
    const std::vector<std::pair<bool, std::string>> frames {{false, "p0"}, {true, "K1"}, {false, "p2"}, {false, "p3"}};
    std::int64_t i = 0;
    for (const auto &[keyframe, text] : frames) {
      const std::vector<std::uint8_t> payload(text.begin(), text.end());
      if (with_provenance) {
        video(keyframe, t0 + std::chrono::milliseconds(i), payload, provenance(7 - i, std::nullopt, true, true));
      } else {
        legacy(keyframe, t0 + std::chrono::milliseconds(i), payload);
      }
      ++i;
    }
    const auto status = stream_recorder::recorder_status();
    const std::string path = status.file;
    const auto written = status.continuous.packets_written;
    stream_recorder::stop_recording();
    return std::make_pair(read_file(path), written);
  };
  const auto without = record_file(false);
  const auto with = record_file(true);
  EXPECT_EQ(without.first, "K1p2p3");
  EXPECT_EQ(with.first, without.first);
  EXPECT_EQ(with.second, without.second);
}

// Provenance entries never exceed the packet cap, live or held, and the compiled sizes and held
// capacities are recorded (bounded accounting only: not allocator overhead or process RSS).
TEST_F(StreamRecorderProvenanceTests, ProvenanceStaysWithinThePacketCapAndRecordsCompiledSizes) {
  constexpr std::size_t k_cap = 8;
  stream_recorder::detail::set_replay_limits_for_tests(1 << 20, k_cap);
  for (std::int64_t i = 0; i < static_cast<std::int64_t>(3 * k_cap); ++i) {
    video(true, t0 + std::chrono::milliseconds(i), bytes(16, static_cast<std::uint8_t>(i)), provenance(i, t0 + std::chrono::milliseconds(i)));
  }
  const auto entries = live();
  EXPECT_EQ(entries.size(), k_cap);
  const auto s = stats(t0 + 30ms);
  EXPECT_EQ(s.packets, k_cap);
  EXPECT_EQ(s.max_packets, k_cap);

  auto admission = stream_recorder::detail::admit_replay_save(t0 + 30ms);
  ASSERT_TRUE(admission.result.admitted);
  const auto held = stream_recorder::detail::snapshot_provenance_for_tests(admission.save);
  EXPECT_EQ(held.payload_count, k_cap);
  EXPECT_EQ(held.provenance_count, k_cap);
  // Measured, not assumed: reserve() guarantees at least the requested capacity, not exactly it.
  EXPECT_GE(held.provenance_capacity, held.provenance_count);
  EXPECT_GE(held.payload_capacity, held.payload_count);

  // The live run moves on while the save is held: the live entries change, the held ones do not.
  for (std::int64_t i = 0; i < 3; ++i) {
    video(true, t0 + 31ms + std::chrono::milliseconds(i), bytes(16, 0xA0), provenance(100 + i, t0 + 31ms));
  }
  const auto live_now = live();
  const auto after = stats(t0 + 40ms);  // Read after admission and the later pushes
  EXPECT_EQ(live_now.size(), k_cap);
  EXPECT_EQ(after.packets, live_now.size());
  EXPECT_TRUE(after.save_in_progress);
  EXPECT_EQ(after.snapshot_packets, held.provenance_count);
  // Logical entries: every live entry and every held entry has exactly one provenance record.
  EXPECT_EQ(after.metadata_entries, live_now.size() + held.provenance_count);
  EXPECT_LE(after.metadata_entries, after.max_metadata_entries);
  EXPECT_LE(held.provenance_count, after.max_snapshot_packets);

  const auto sizes = stream_recorder::detail::provenance_sizes_for_tests();
  EXPECT_GT(sizes.packet_provenance, 0u);
  EXPECT_GT(sizes.live_entry, sizes.pre_provenance_entry_mirror);
  EXPECT_GE(sizes.provenance_record, sizes.packet_provenance);
  EXPECT_GE(sizes.live_entry, sizes.provenance_record + sizes.snapshot_payload_ref);
  const auto property = [](const char *key, std::size_t value) {
    ::testing::Test::RecordProperty(key, std::to_string(value));
  };
  // Compiled sizes (this build only).
  property("sizeofPacketProvenance", sizes.packet_provenance);
  property("sizeofProvenanceRecord", sizes.provenance_record);
  property("sizeofLiveReplayEntry", sizes.live_entry);
  // Same compiler, same build: a struct with exactly the pre-#359 entry members, in their order.
  property("sizeofPreProvenanceEntryMirror", sizes.pre_provenance_entry_mirror);
  property("liveEntryDeltaVsMirror", sizes.live_entry - sizes.pre_provenance_entry_mirror);
  property("sizeofSnapshotPayloadRef", sizes.snapshot_payload_ref);
  // Logical counts (bounded by the caps), read after admission.
  property("packetCap", after.max_packets);
  property("liveEntries", live_now.size());
  property("heldEntries", held.provenance_count);
  property("metadataEntries", after.metadata_entries);
  property("maxMetadataEntries", after.max_metadata_entries);
  // Measured capacities of the held arrays (what this standard library actually reserved).
  property("heldProvenanceCapacity", held.provenance_capacity);
  property("heldPayloadCapacity", held.payload_capacity);
  property("heldProvenanceCapacityBytes", held.provenance_capacity * sizes.provenance_record);
  // Logical size only: the live entries' added bytes, not the deque's actual allocation.
  property("liveEntryDeltaLogicalBytes", live_now.size() * (sizes.live_entry - sizes.pre_provenance_entry_mirror));
  ::testing::Test::RecordProperty("sizingNote",
                                  "compiled sizeof, logical entry counts and measured vector capacity for this build; "
                                  "not allocator overhead, deque block allocation, heap totals or process RSS");
}
