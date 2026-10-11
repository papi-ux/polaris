/**
 * @file tests/unit/test_recorder_bitstream_expiry.cpp
 * @brief Real libx264/libx265 fixture packets through the replay buffer's duration expiry and
 * keyframe realignment (#375 under #329).
 *
 * Every case runs the bounded CPU encoder fixture (#363's support, unchanged) and requires its
 * frozen baseline: 8 packets, key flags exactly at 0 and 4. The configured duration is D = 60 s
 * (one minute) and the default hard caps stay in force, far above these 8 packets, so no cap can
 * remove anything. Packets 0..3 arrive at t0 + i * 16 ms and packets 4..7 at t0 + 30 s + (i - 4) *
 * 16 ms on a controlled steady arrival clock. A packet expires when the read, append or save time
 * minus its arrival is strictly greater than D. Every expected value below comes from those arrival
 * offsets and D only; the packet PTS is recorded and never compared.
 *
 * - E1, read-triggered expiry: at exactly t0 + D packet 0 is D old and all 8 stay. At t0 + 60033 ms
 *   packets 0..2 have expired and realignment removes the still-fresh packet 3, so the run, the
 *   retained provenance and the save are exactly packets 4..7. A later read stamped t0 + D does not
 *   restore anything.
 * - E2, append-triggered expiry: packets 0..6 arrive with no status read, then packet 7 arrives at
 *   t0 + 60033 ms. A read stamped earlier, t0 + 30048 ms, where nothing could expire, already shows
 *   packets 4..7 and the duration cause, so the append performed the expiry. The save is packets 4..7.
 * - E3, a fully stale run: at t0 + 90049 ms every packet has expired. The save is empty, admits
 *   nothing and creates no file; replay awaits a keyframe and refuses a dependent packet, and the run
 *   restarts only at a key.
 *
 * Each case isolates expiry as the only removal cause: no cap, discontinuity, codec change or
 * reconfiguration happens after the session starts. Assertions are in-run byte, counter and arrival
 * clock properties only. No packet hash is frozen here: whether these bytes equal earlier runs, or
 * the inputs a decoder was shown, is a separate readback's question. The saved file is raw
 * elementary video, not a container or a playable-file claim.
 *
 * Evidence: CTest sets POLARIS_RECORDER_EXPIRY_EVIDENCE_DIR. Each case creates its own directory
 * there exclusively and writes the 8 source packets, the published save (if any) and a manifest,
 * each created exclusively, fully written, synced, closed and reread, before any assertion, so a
 * failed case stays diagnosable. A case's evidence stays under 1 MiB. A missing, relative,
 * non-directory or symlinked root, an existing case directory or any write or reread error fails
 * the case; there is no fallback and no skip.
 */
#include "recorder_encoder_fixture_support.h"

#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

namespace {
  namespace fs = std::filesystem;
  using steady = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  using recorder_fixture::properties_t;
  using stream_recorder::window_limit_e;

  constexpr auto k_marker = "POLARIS_RECORDER_EXPIRY_JSON";
  constexpr auto k_evidence_env = "POLARIS_RECORDER_EXPIRY_EVIDENCE_DIR";
  constexpr std::uint64_t k_session = 375;
  constexpr auto k_stamp = "20261011_003231";
  /// Explicit synthetic producer counters for provenance: deliberately not the packet PTS.
  constexpr std::int64_t k_producer_counter_base = 37500;
  /// config::recording.replay_buffer_minutes for every case: D = 60 s.
  constexpr int k_configured_minutes = 1;
  constexpr std::chrono::milliseconds k_duration = 60s;
  /// The controlled arrival clock. Not a presentation time.
  constexpr std::chrono::milliseconds k_step = 16ms;
  constexpr std::chrono::milliseconds k_gap = 30s;
  /// A case's sources, save and manifest together stay below this.
  constexpr std::size_t k_evidence_budget = std::size_t {1} << 20;
  /// The run's payload total the guard allows, so the sources plus a save of at most the same bytes
  /// and the manifest stay below k_evidence_budget.
  constexpr std::size_t k_max_run_bytes = std::size_t {448} * 1024;
  constexpr auto k_scope = "real fixture packets through replay duration expiry at a controlled arrival clock; in-run byte, "
                           "counter and arrival-clock properties only; PTS recorded, never compared; no cap precedence, "
                           "decode, playback, container, presentation timing or memory-safety claim";

  /// Arrival offset of fixture packet `i` from t0.
  constexpr std::chrono::milliseconds arrival(std::size_t i) {
    return i < 4 ? k_step * static_cast<int>(i) : k_gap + k_step * static_cast<int>(i - 4);
  }

  /// E1: packet 0 is exactly D old here, which the strict comparison keeps.
  constexpr std::chrono::milliseconds k_boundary = arrival(0) + k_duration;
  /// E1 and E2: packets 0..2 are older than D, packet 3 is not.
  constexpr std::chrono::milliseconds k_expiry = arrival(2) + k_duration + 1ms;
  /// E2: earlier than k_expiry and no packet is older than D.
  constexpr std::chrono::milliseconds k_earlier = arrival(7);
  /// E3: every packet, packet 7 included, is older than D.
  constexpr std::chrono::milliseconds k_stale = arrival(7) + k_duration + 1ms;

  static_assert(k_boundary == 60000ms);
  static_assert(k_expiry == 60033ms);
  static_assert(k_earlier == 30048ms);
  static_assert(k_stale == 90049ms);
  static_assert(k_expiry - arrival(3) < k_duration, "packet 3 is fresh at k_expiry: only realignment removes it");
  static_assert(k_earlier - arrival(0) < k_duration, "nothing can expire at k_earlier");

  struct codec_case_t {
    const recorder_fixture::encoder_spec_t *spec;
    int video_format;  ///< The recorder's codec number: 0=H.264, 1=HEVC
    const char *label;
    const char *tag;
  };

  const codec_case_t k_h264 {&recorder_fixture::k_libx264, 0, "H.264", "h264"};
  const codec_case_t k_hevc {&recorder_fixture::k_libx265, 1, "HEVC", "hevc"};

  const char *limit_name(window_limit_e l) {
    switch (l) {
      case window_limit_e::none:
        return "none";
      case window_limit_e::duration:
        return "duration";
      case window_limit_e::payload_cap:
        return "payload_cap";
      case window_limit_e::entry_cap:
        return "entry_cap";
      case window_limit_e::configuration:
        return "configuration";
      case window_limit_e::selection:
        return "selection";
      case window_limit_e::codec:
        return "codec";
      case window_limit_e::discontinuity:
        return "discontinuity";
    }
    return "unknown";
  }

  const char *status_name(stream_recorder::detail::save_status_e s) {
    switch (s) {
      case stream_recorder::detail::save_status_e::saved:
        return "saved";
      case stream_recorder::detail::save_status_e::busy:
        return "busy";
      case stream_recorder::detail::save_status_e::disabled:
        return "disabled";
      case stream_recorder::detail::save_status_e::empty:
        return "empty";
      case stream_recorder::detail::save_status_e::storage_error:
        return "storage_error";
    }
    return "unknown";
  }

  std::string two_digits(std::size_t i) {
    return (i < 10 ? "0" : "") + std::to_string(i);
  }

  /// Whole milliseconds, for evidence. Comparisons use the exact steady duration instead.
  std::string ms_text(steady::duration d) {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
  }

  std::string age_text(const std::optional<steady::duration> &d) {
    return d ? ms_text(*d) : std::string {"unknown"};
  }

  /// NAL unit types of an Annex B access unit, in order, from its 3- or 4-byte start codes.
  std::vector<int> nal_types(const std::vector<std::uint8_t> &au, bool hevc) {
    std::vector<int> types;
    for (std::size_t i = 0; i + 3 < au.size(); ++i) {
      if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1) {
        const std::uint8_t header = au[i + 3];
        types.push_back(hevc ? (header >> 1) & 0x3f : header & 0x1f);
        i += 2;
      }
    }
    return types;
  }

  template<class T>
  std::string join(const std::vector<T> &values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
      out += (i ? "," : "") + std::to_string(values[i]);
    }
    return out;
  }

  /// The access unit opens with its parameter sets, and its first slice is an IDR slice:
  /// H.264 SPS (7) and PPS (8) before IDR (5); HEVC VPS (32), SPS (33) and PPS (34) before IDR (19, 20).
  bool parameter_sets_then_idr(const std::vector<int> &types, bool hevc) {
    std::vector<int> before;
    for (const int t : types) {
      const bool vcl = hevc ? t < 32 : (t >= 1 && t <= 5);
      if (vcl) {
        const bool idr = hevc ? (t == 19 || t == 20) : t == 5;
        const auto has = [&before](int wanted) {
          return std::find(before.begin(), before.end(), wanted) != before.end();
        };
        return idr && (hevc ? has(32) && has(33) && has(34) : has(7) && has(8));
      }
      before.push_back(t);
    }
    return false;
  }

  // ---------------------------------------------------------------------------------------------
  // Raw evidence: exclusive creation, full write, sync, close and reread.
  // ---------------------------------------------------------------------------------------------

  std::string write_exclusive(const fs::path &path, const std::uint8_t *data, std::size_t size) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
      return "cannot create " + path.string() + ": " + std::strerror(errno);
    }
    std::size_t done = 0;
    while (done < size) {
      const auto n = ::write(fd, data + done, size - done);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        const int error = errno;
        ::close(fd);
        return "short write to " + path.string() + ": " + std::strerror(error);
      }
      done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
      const int error = errno;
      ::close(fd);
      return "fsync failed for " + path.string() + ": " + std::strerror(error);
    }
    if (::close(fd) != 0) {
      return "close failed for " + path.string() + ": " + std::strerror(errno);
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
      return "reread failed for " + path.string();
    }
    const std::vector<std::uint8_t> back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (back.size() != size || (size && std::memcmp(back.data(), data, size) != 0)) {
      return "reread differs for " + path.string();
    }
    return {};
  }

  /// The evidence root CTest names: absolute, an existing directory, and not a symlink.
  std::string evidence_root(fs::path &root) {
    const char *value = std::getenv(k_evidence_env);
    if (!value || !*value) {
      return std::string {k_evidence_env} + " is not set";
    }
    root = fs::path {value};
    if (!root.is_absolute()) {
      return std::string {k_evidence_env} + " is not absolute: " + root.string();
    }
    std::error_code ec;
    const auto status = fs::symlink_status(root, ec);
    if (ec || fs::is_symlink(status)) {
      return std::string {k_evidence_env} + " is missing or a symlink: " + root.string();
    }
    if (!fs::is_directory(status)) {
      return std::string {k_evidence_env} + " is not a directory: " + root.string();
    }
    return {};
  }

  /// Writes a case's evidence files exclusively, keeping their total below k_evidence_budget.
  struct evidence_writer_t {
    fs::path dir;
    std::size_t written = 0;

    std::string write(const std::string &name, const std::uint8_t *data, std::size_t size) {
      if (size >= k_evidence_budget - written) {
        return "evidence budget of " + std::to_string(k_evidence_budget) + " bytes exceeded by " + name;
      }
      if (auto error = write_exclusive(dir / name, data, size); !error.empty()) {
        return error;
      }
      written += size;
      return {};
    }

    std::string write(const std::string &name, const std::vector<std::uint8_t> &bytes) {
      return write(name, bytes.data(), bytes.size());
    }

    std::string write(const std::string &name, const std::string &text) {
      return write(name, reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
    }
  };

  // ---------------------------------------------------------------------------------------------
  // Scenarios: the steps, in order, and what each read, the save and the final run must show.
  // ---------------------------------------------------------------------------------------------

  enum class step_e {
    push,  ///< Push fixture packet `packet` at t0 + `at`
    read,  ///< replay_stats_at(t0 + `at`)
    save,  ///< save_replay_result_at(t0 + `at`)
  };

  struct step_t {
    step_e kind;
    std::chrono::milliseconds at;
    std::size_t packet = 0;     ///< For a push
    bool admit = true;          ///< For a push: whether it must become the newest live entry
    const char *label = "";     ///< For a read
  };

  /// What one read must show. Bytes and keyframes follow from `live`.
  struct read_expect_t {
    std::vector<std::size_t> live;  ///< Fixture packet indices of the live run, in order
    std::chrono::milliseconds span;
    std::chrono::milliseconds front_age;
    std::optional<std::chrono::milliseconds> key_age;
    std::uint64_t trimmed;
    const char *limit;
    bool awaiting;
    std::uint64_t refused_awaiting;
  };

  struct scenario_t {
    const char *name;
    std::vector<step_t> steps;
    std::vector<read_expect_t> reads;  ///< One per read step, in order
    bool saves;                        ///< The save publishes exactly packets 4..7
    std::chrono::milliseconds save_span;
    std::vector<std::size_t> retained_pushes;  ///< Push ordinals whose entries form the final live run
  };

  std::vector<step_t> push_steps(std::size_t first, std::size_t last) {
    std::vector<step_t> steps;
    for (std::size_t i = first; i <= last; ++i) {
      steps.push_back(step_t {step_e::push, arrival(i), i});
    }
    return steps;
  }

  /**
   * After session_started() the last window limit is `selection`: the start changes the selected
   * session, which clears the (empty) run for that reason. Nothing has trimmed the run before the
   * duration does, so `selection` is the exact value until then.
   */
  scenario_t read_expiry_scenario() {
    scenario_t s {"E1-read-expiry", push_steps(0, 7), {}, true, arrival(7) - arrival(4), {4, 5, 6, 7}};
    s.steps.push_back(step_t {step_e::read, k_boundary, 0, true, "boundary"});
    s.steps.push_back(step_t {step_e::read, k_expiry, 0, true, "expiry"});
    s.steps.push_back(step_t {step_e::read, k_boundary, 0, true, "laterReadStampedAtBoundary"});
    s.steps.push_back(step_t {step_e::save, k_expiry});
    s.reads = {
      {{0, 1, 2, 3, 4, 5, 6, 7}, arrival(7) - arrival(0), k_boundary - arrival(0), k_boundary - arrival(4), 0, "selection", false, 0},
      {{4, 5, 6, 7}, arrival(7) - arrival(4), k_expiry - arrival(4), k_expiry - arrival(4), 4, "duration", false, 0},
      {{4, 5, 6, 7}, arrival(7) - arrival(4), k_boundary - arrival(4), k_boundary - arrival(4), 4, "duration", false, 0},
    };
    return s;
  }

  scenario_t append_expiry_scenario() {
    scenario_t s {"E2-append-expiry", push_steps(0, 6), {}, true, k_expiry - arrival(4), {4, 5, 6, 7}};
    s.steps.push_back(step_t {step_e::push, k_expiry, 7});
    s.steps.push_back(step_t {step_e::read, k_earlier, 0, true, "earlierStamp"});
    s.steps.push_back(step_t {step_e::save, k_expiry});
    s.reads = {
      {{4, 5, 6, 7}, k_expiry - arrival(4), k_earlier - arrival(4), k_earlier - arrival(4), 4, "duration", false, 0},
    };
    return s;
  }

  scenario_t stale_run_scenario() {
    scenario_t s {"E3-stale-run", push_steps(0, 7), {}, false, 0ms, {9}};
    s.steps.push_back(step_t {step_e::save, k_stale});
    s.steps.push_back(step_t {step_e::read, k_stale, 0, true, "stale"});
    s.steps.push_back(step_t {step_e::push, k_stale, 5, false});
    s.steps.push_back(step_t {step_e::read, k_stale, 0, true, "afterDependent"});
    s.steps.push_back(step_t {step_e::push, k_stale + k_step, 4});
    s.steps.push_back(step_t {step_e::read, k_stale + k_step, 0, true, "afterKey"});
    s.reads = {
      {{}, 0ms, 0ms, k_stale - arrival(4), 8, "duration", true, 0},
      {{}, 0ms, 0ms, k_stale - arrival(4), 8, "duration", true, 1},
      {{4}, 0ms, 0ms, 0ms, 8, "duration", false, 1},
    };
    return s;
  }

  /// One push: what it must do and what the live run showed right after it.
  struct push_t {
    std::size_t packet = 0;
    std::chrono::milliseconds at {0};
    bool expected_admit = true;
    bool admitted = false;
    std::uint64_t sequence = 0;  ///< The new live entry's sequence, when admitted
    std::size_t live_after = 0;
  };

  struct read_t {
    std::string label;
    std::chrono::milliseconds at {0};
    stream_recorder::replay_stats_t stats;
    std::string check = "not compared";
  };

  /// Everything one case observed, kept for evidence and properties before any assertion.
  struct observed_t {
    std::string encode_result;
    std::string baseline;
    std::string guard = "not run";  ///< The fixture assumptions the expectations rely on
    std::vector<push_t> pushes;
    std::vector<read_t> reads;
    std::optional<std::chrono::milliseconds> save_at;
    stream_recorder::detail::save_result_t result;
    std::vector<stream_recorder::detail::provenance_entry_t> retained;
    std::vector<std::uint8_t> file;
    std::vector<std::uint8_t> expected;  ///< Packets 4..7, or nothing
    std::vector<int> first_types;        ///< NAL types of the saved file's first access unit
    std::size_t files_in_output = 0;
    std::string evidence = "not written";
  };

  /// Compare one read with its expectation, exactly; "ok" or every differing field.
  std::string compare_read(const read_t &r, const read_expect_t &e, const recorder_fixture::run_t &run) {
    std::size_t bytes = 0;
    std::size_t keys = 0;
    for (const auto i : e.live) {
      bytes += run.packets[i].bytes.size();
      keys += (run.packets[i].flags & AV_PKT_FLAG_KEY) ? 1 : 0;
    }
    std::string out;
    const auto field = [&out](const char *name, const std::string &got, const std::string &want) {
      if (got != want) {
        out += std::string {out.empty() ? "" : "; "} + name + " " + got + ", expected " + want;
      }
    };
    const auto ticks = [](steady::duration d) {
      return std::to_string(d.count());
    };
    const auto exact = [](std::chrono::milliseconds d) {
      return std::to_string(std::chrono::duration_cast<steady::duration>(d).count());
    };
    const auto &s = r.stats;
    field("packets", std::to_string(s.packets), std::to_string(e.live.size()));
    field("bytes", std::to_string(s.bytes), std::to_string(bytes));
    field("trimmed", std::to_string(s.trimmed), std::to_string(e.trimmed));
    field("runKeyframes", std::to_string(s.run_keyframes), std::to_string(keys));
    field("keyframeStartRun", s.keyframe_start_run ? "1" : "0", e.live.empty() ? "0" : "1");
    field("runSpan(steady ticks)", ticks(s.run_span), exact(e.span));
    field("frontAge(steady ticks)", ticks(s.run_front_age), exact(e.front_age));
    field("lastAcceptedKeyframeAge(steady ticks)", s.last_accepted_keyframe_age ? ticks(*s.last_accepted_keyframe_age) : "unknown",
          e.key_age ? exact(*e.key_age) : "unknown");
    field("lastWindowLimit", limit_name(s.last_window_limit), e.limit);
    field("awaitingKeyframe", s.awaiting_keyframe ? "1" : "0", e.awaiting ? "1" : "0");
    field("refusedAwaitingKeyframe", std::to_string(s.refused_awaiting_keyframe), std::to_string(e.refused_awaiting));
    field("refusedOversized", std::to_string(s.refused_oversized), "0");
    field("refusedInvalid", std::to_string(s.refused_invalid), "0");
    field("refusedUnselected", std::to_string(s.refused_unselected), "0");
    field("discontinuities", std::to_string(s.discontinuities), "0");
    field("selectedGeneration", std::to_string(s.selected_generation), std::to_string(k_session));
    field("durationMinutes", std::to_string(s.duration.count()), std::to_string(k_configured_minutes));
    field("maxBytes", std::to_string(s.max_bytes), std::to_string(stream_recorder::k_replay_max_payload_bytes));
    field("maxPackets", std::to_string(s.max_packets), std::to_string(stream_recorder::k_replay_max_packets));
    return out.empty() ? "ok" : out;
  }

  /**
   * One eligible session with a known codec in replay mode, saving into a `recordings` directory
   * inside a parent this fixture created exclusively. TearDown restores the recorder and the
   * configuration and removes only that parent; the retained evidence is never touched.
   */
  class RecorderBitstreamExpiryTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_config = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      case_name = info->name();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-bitstream-expiry-"} + case_name + "-" +
                                       std::to_string(::getpid()) + "-" + std::to_string(attempt));
        std::error_code ec;
        // create_directory() reports true only when this call made the directory.
        if (fs::create_directory(candidate, ec)) {
          owned_parent = candidate;
        }
      }
      ASSERT_FALSE(owned_parent.empty()) << "no exclusive temporary directory";
      output_dir = owned_parent / "recordings";
      config::recording.output_dir = output_dir.string();
      config::recording.replay_buffer_minutes = k_configured_minutes;
      config::recording.enabled = true;
      config::recording.replay_buffer = true;
      stream_recorder::load_config();
      stream_recorder::detail::save_hooks_t hooks;
      hooks.name_stamp = [] {
        return std::string {k_stamp};
      };
      stream_recorder::detail::set_save_hooks_for_tests(hooks);
    }

    void TearDown() override {
      stream_recorder::detail::reset_replay_for_tests();
      config::recording = saved_config;
      stream_recorder::load_config();
      if (!owned_parent.empty()) {
        std::error_code ec;
        fs::remove_all(owned_parent, ec);
      }
    }

    /// The fixture assumptions every expectation relies on, checked before any push.
    static std::string check_fixture(const recorder_fixture::run_t &run) {
      if (run.packets.size() != static_cast<std::size_t>(recorder_fixture::k_frames)) {
        return "expected " + std::to_string(recorder_fixture::k_frames) + " packets";
      }
      std::size_t total = 0;
      for (std::size_t i = 0; i < run.packets.size(); ++i) {
        const auto &p = run.packets[i];
        if (p.bytes.empty() || p.bytes.size() > static_cast<std::size_t>(recorder_fixture::k_max_packet_bytes)) {
          return "packet " + std::to_string(i) + " is empty or above its maximum";
        }
        if (((p.flags & AV_PKT_FLAG_KEY) != 0) != (i == 0 || i == 4)) {
          return "packet " + std::to_string(i) + " has its key flag at an unexpected position";
        }
        total += p.bytes.size();
      }
      if (total >= k_max_run_bytes) {
        return "the run's " + std::to_string(total) + " bytes are not below " + std::to_string(k_max_run_bytes);
      }
      // The default hard caps hold the whole run, so no cap can remove a packet.
      if (total > stream_recorder::k_replay_max_payload_bytes || run.packets.size() >= stream_recorder::k_replay_max_packets) {
        return "the run does not fit the default replay caps";
      }
      return {};
    }

    /// Perform the scenario's steps in order. Pushes are observed through the live entries, which
    /// releases nothing; only the scenario's own read and save steps can expire packets.
    void run_steps(const codec_case_t &c, const scenario_t &s, const recorder_fixture::run_t &run, observed_t &o) {
      stream_recorder::session_started(k_session, true, c.video_format);
      std::uint64_t newest_sequence = 0;
      for (const auto &step : s.steps) {
        const auto at = t0 + step.at;
        switch (step.kind) {
          case step_e::push:
            {
              const auto &p = run.packets[step.packet];
              push_t push;
              push.packet = step.packet;
              push.at = step.at;
              push.expected_admit = step.admit;
              stream_recorder::packet_provenance_t provenance;
              provenance.frame_index = k_producer_counter_base + static_cast<std::int64_t>(step.packet);
              stream_recorder::detail::push_video_at(p.bytes.data(), p.bytes.size(), (p.flags & AV_PKT_FLAG_KEY) != 0, k_session, c.video_format, provenance, at);
              const auto live = stream_recorder::detail::replay_provenance_for_tests();
              push.live_after = live.size();
              if (!live.empty() && live.back().sequence > newest_sequence && live.back().payload == p.bytes) {
                push.admitted = true;
                push.sequence = live.back().sequence;
                newest_sequence = push.sequence;
              }
              o.pushes.push_back(push);
              break;
            }
          case step_e::read:
            o.reads.push_back(read_t {step.label, step.at, stream_recorder::detail::replay_stats_at(at)});
            break;
          case step_e::save:
            o.save_at = step.at;
            o.result = stream_recorder::detail::save_replay_result_at(at);
            break;
        }
      }
      o.retained = stream_recorder::detail::replay_provenance_for_tests();
    }

    std::string write_evidence(const codec_case_t &c, const scenario_t &s, const recorder_fixture::run_t &run, const observed_t &o) const {
      fs::path root;
      if (auto error = evidence_root(root); !error.empty()) {
        return error;
      }
      evidence_writer_t writer {root / case_name};
      std::error_code ec;
      if (!fs::create_directory(writer.dir, ec)) {
        return "the case directory already exists or cannot be created: " + writer.dir.string();
      }
      std::string manifest = "{\"marker\":" + recorder_fixture::json_string(k_marker) +
                             ",\"case\":" + recorder_fixture::json_string(case_name) +
                             ",\"codec\":" + recorder_fixture::json_string(c.tag) +
                             ",\"scenario\":" + recorder_fixture::json_string(s.name) + ",\"schemaVersion\":1" +
                             ",\"clock\":{\"configuredDurationS\":" + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(k_duration).count()) +
                             ",\"arrivalStepMs\":" + std::to_string(k_step.count()) + ",\"gapMs\":" + std::to_string(k_gap.count()) +
                             ",\"note\":\"steady arrival clock; expiry is arrival age strictly above the duration; PTS recorded only\"}" +
                             ",\"sources\":[";
      for (std::size_t i = 0; i < run.packets.size() && i < static_cast<std::size_t>(recorder_fixture::k_frames); ++i) {
        const auto &p = run.packets[i];
        const std::string name = std::string {"source-"} + c.tag + "-" + two_digits(i) + ".bin";
        if (auto error = writer.write(name, p.bytes); !error.empty()) {
          return error;
        }
        manifest += std::string {i ? "," : ""} + "{\"file\":" + recorder_fixture::json_string(name) +
                    ",\"index\":" + std::to_string(i) + ",\"key\":" + ((p.flags & AV_PKT_FLAG_KEY) ? "true" : "false") +
                    ",\"size\":" + std::to_string(p.bytes.size()) + ",\"sha256\":" + recorder_fixture::json_string(p.sha256) +
                    ",\"pts\":" + std::to_string(p.pts) +
                    ",\"nalTypes\":" + recorder_fixture::json_string(join(nal_types(p.bytes, c.video_format == 1))) + "}";
      }
      manifest += "],\"pushes\":[";
      for (std::size_t i = 0; i < o.pushes.size(); ++i) {
        const auto &p = o.pushes[i];
        manifest += std::string {i ? "," : ""} + "{\"ordinal\":" + std::to_string(i) +
                    ",\"source\":" + recorder_fixture::json_string(std::string {c.tag} + ":" + std::to_string(p.packet)) +
                    ",\"arrivalOffsetMs\":" + std::to_string(p.at.count()) +
                    ",\"expected\":" + recorder_fixture::json_string(p.expected_admit ? "admitted" : "not admitted") +
                    ",\"observed\":" + recorder_fixture::json_string(p.admitted ? "admitted" : "not admitted") +
                    ",\"sequence\":" + std::to_string(p.sequence) + ",\"liveAfter\":" + std::to_string(p.live_after) + "}";
      }
      manifest += "],\"reads\":[";
      for (std::size_t i = 0; i < o.reads.size(); ++i) {
        const auto &r = o.reads[i];
        manifest += std::string {i ? "," : ""} + "{\"label\":" + recorder_fixture::json_string(r.label) +
                    ",\"atOffsetMs\":" + std::to_string(r.at.count()) + ",\"packets\":" + std::to_string(r.stats.packets) +
                    ",\"bytes\":" + std::to_string(r.stats.bytes) + ",\"trimmed\":" + std::to_string(r.stats.trimmed) +
                    ",\"runKeyframes\":" + std::to_string(r.stats.run_keyframes) + ",\"runSpanMs\":" + ms_text(r.stats.run_span) +
                    ",\"frontAgeMs\":" + ms_text(r.stats.run_front_age) +
                    ",\"lastAcceptedKeyframeAgeMs\":" + recorder_fixture::json_string(age_text(r.stats.last_accepted_keyframe_age)) +
                    ",\"lastWindowLimit\":" + recorder_fixture::json_string(limit_name(r.stats.last_window_limit)) +
                    ",\"awaitingKeyframe\":" + (r.stats.awaiting_keyframe ? "true" : "false") +
                    ",\"refusedAwaitingKeyframe\":" + std::to_string(r.stats.refused_awaiting_keyframe) +
                    ",\"check\":" + recorder_fixture::json_string(r.check) + "}";
      }
      const bool saved = o.result.status == stream_recorder::detail::save_status_e::saved;
      manifest += std::string {"],\"save\":{\"atOffsetMs\":"} + (o.save_at ? std::to_string(o.save_at->count()) : "null") +
                  ",\"status\":" + recorder_fixture::json_string(status_name(o.result.status)) +
                  ",\"admitted\":" + (o.result.admitted ? "true" : "false") + ",\"packets\":" + std::to_string(o.result.packets) +
                  ",\"bytes\":" + std::to_string(o.result.bytes) + ",\"keyframes\":" + std::to_string(o.result.keyframes) +
                  ",\"spanMs\":" + ms_text(o.result.span) + "},\"expectedSuffix\":" + (o.expected.empty() ? "[]" : "[4,5,6,7]") +
                  ",\"saved\":";
      if (saved) {
        if (auto error = writer.write("saved.bin", o.file); !error.empty()) {
          return error;
        }
        manifest += "{\"file\":\"saved.bin\",\"size\":" + std::to_string(o.file.size()) +
                    ",\"sha256\":" + recorder_fixture::json_string(recorder_fixture::sha256_hex(o.file.data(), o.file.size())) + "}";
      } else {
        manifest += "null";
      }
      std::vector<std::uint64_t> sequences;
      for (const auto &entry : o.retained) {
        sequences.push_back(entry.sequence);
      }
      manifest += ",\"retainedSequences\":" + recorder_fixture::json_string(join(sequences)) +
                  ",\"fixture\":{\"encoder\":" + recorder_fixture::json_string(c.spec->name) +
                  ",\"params\":" + recorder_fixture::json_string(c.spec->params_value) +
                  ",\"ffmpegVersionInfo\":" + recorder_fixture::json_string(av_version_info()) +
                  ",\"libavcodecIdent\":" + recorder_fixture::json_string(LIBAVCODEC_IDENT) + "}" +
                  ",\"scope\":" + recorder_fixture::json_string(k_scope) + "}\n";
      return writer.write("manifest.json", manifest);
    }

    void check(const codec_case_t &c, const scenario_t &s) {
      recorder_fixture::run_t run;
      observed_t o;
      o.encode_result = recorder_fixture::encode_run(*c.spec, run);
      o.baseline = o.encode_result.empty() ? recorder_fixture::validate_baseline(run) : std::string {};
      const bool encoded = o.encode_result.empty() && o.baseline.empty();
      if (encoded) {
        o.guard = check_fixture(run);
        if (o.guard.empty()) {
          o.guard = "ok";
        }
      }
      const bool ready = encoded && o.guard == "ok";
      if (ready) {
        run_steps(c, s, run, o);
        if (o.result.status == stream_recorder::detail::save_status_e::saved) {
          std::ifstream in(o.result.path, std::ios::binary);
          o.file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (s.saves) {
          for (std::size_t i = 4; i < run.packets.size(); ++i) {
            o.expected.insert(o.expected.end(), run.packets[i].bytes.begin(), run.packets[i].bytes.end());
          }
          // The saved file's first access unit is as long as packet 4, if the save is what it should be.
          const auto first_size = static_cast<std::ptrdiff_t>(run.packets[4].bytes.size());
          if (o.file.size() >= run.packets[4].bytes.size()) {
            o.first_types = nal_types(std::vector<std::uint8_t>(o.file.begin(), o.file.begin() + first_size), c.video_format == 1);
          }
        }
        for (std::size_t i = 0; i < o.reads.size() && i < s.reads.size(); ++i) {
          o.reads[i].check = compare_read(o.reads[i], s.reads[i], run);
        }
      }

      std::size_t push_steps_count = 0;
      for (const auto &step : s.steps) {
        push_steps_count += step.kind == step_e::push ? 1 : 0;
      }
      std::string pushes_as_expected = ready ? "ok" : "not run";
      if (ready && o.pushes.size() != push_steps_count) {
        pushes_as_expected = "observed " + std::to_string(o.pushes.size()) + " pushes";
      }
      for (std::size_t i = 0; pushes_as_expected == "ok" && i < o.pushes.size(); ++i) {
        if (o.pushes[i].admitted != o.pushes[i].expected_admit) {
          pushes_as_expected = "push " + std::to_string(i) + (o.pushes[i].admitted ? " admitted" : " not admitted");
        }
      }
      const std::string reads_compared = !ready ? "not run" : (o.reads.size() == s.reads.size() ? "ok" : "observed " + std::to_string(o.reads.size()) + " reads");

      // The final live run and its provenance: each retained entry is the named push's packet,
      // with its producer counter and the sequence that push received.
      std::string retained_equal = ready ? "ok" : "not run";
      if (ready && o.retained.size() != s.retained_pushes.size()) {
        retained_equal = "retained " + std::to_string(o.retained.size()) + " entries";
      }
      for (std::size_t i = 0; retained_equal == "ok" && i < o.retained.size(); ++i) {
        const auto ordinal = s.retained_pushes[i];
        if (ordinal >= o.pushes.size() || !o.pushes[ordinal].admitted) {
          retained_equal = "push " + std::to_string(ordinal) + " was not admitted";
          break;
        }
        const auto &push = o.pushes[ordinal];
        const auto &index = o.retained[i].record.supplied.frame_index;
        if (o.retained[i].payload != run.packets[push.packet].bytes) {
          retained_equal = "payload of push " + std::to_string(ordinal) + " differs";
        } else if (!index || *index != k_producer_counter_base + static_cast<std::int64_t>(push.packet)) {
          retained_equal = "producer counter of push " + std::to_string(ordinal) + " differs";
        } else if (o.retained[i].sequence != push.sequence) {
          retained_equal = "sequence of push " + std::to_string(ordinal) + " differs";
        }
      }
      std::vector<std::uint64_t> retained_sequences;
      for (const auto &entry : o.retained) {
        retained_sequences.push_back(entry.sequence);
      }

      const bool saved = o.result.status == stream_recorder::detail::save_status_e::saved;
      const std::string file_equal = !saved ? "not saved" : (o.file == o.expected ? "identical" : "differs");
      const bool in_owned_dir = saved && fs::path(o.result.path).parent_path() == output_dir;
      {
        std::error_code ec;
        if (fs::is_directory(output_dir, ec)) {
          for (const auto &entry : fs::directory_iterator(output_dir, ec)) {
            (void) entry;
            ++o.files_in_output;
          }
        }
      }
      std::string save_check = ready ? "ok" : "not run";
      if (ready) {
        const auto exact_span = std::chrono::duration_cast<steady::duration>(s.save_span);
        if (s.saves && !(saved && o.result.admitted && o.result.packets == 4 && o.result.keyframes == 1 &&
                         o.result.bytes == o.expected.size() && o.result.span == exact_span &&
                         o.result.video_format == c.video_format && o.result.generation == k_session)) {
          save_check = "the save is not exactly packets 4..7 of this session's run";
        } else if (!s.saves && !(o.result.status == stream_recorder::detail::save_status_e::empty && !o.result.admitted &&
                                 o.result.packets == 0 && o.result.bytes == 0 && o.result.keyframes == 0 && o.result.path.empty())) {
          save_check = "the save is not an empty, unadmitted refusal";
        }
      }

      // Evidence first, so a failed case below remains diagnosable from its retained bytes.
      o.evidence = write_evidence(c, s, run, o);

      properties_t properties;
      const auto add = [&properties](std::string key, std::string value) {
        properties.emplace_back(std::move(key), std::move(value));
      };
      add("scope", k_scope);
      add("case", case_name);
      add("scenario", s.name);
      add("ffmpegVersionInfo", av_version_info());
      add("libavcodecIdent", LIBAVCODEC_IDENT);
      recorder_fixture::add_encoder_run(properties, *c.spec, run, o.encode_result, o.baseline);
      add("codec", c.label);
      add("recorderVideoFormat", std::to_string(c.video_format));
      add("sessionGeneration", std::to_string(k_session));
      add("configuredDurationS", std::to_string(std::chrono::duration_cast<std::chrono::seconds>(k_duration).count()));
      add("arrivalClock", "controlled steady clock: packets 0..3 at t0 + i * 16 ms, 4..7 at t0 + 30 s + (i - 4) * 16 ms "
                          "unless a push says otherwise; not a presentation time");
      add("expiryRule", "a packet expires when the stamp minus its arrival is strictly greater than the duration");
      add("ptsNote", "packet PTS is the fixture's synthetic counter, recorded only; no expectation is derived from it");
      add("producerCounterNote", "explicit synthetic producer counters " + std::to_string(k_producer_counter_base) +
                                   "+i as provenance frame_index; independent of the packet PTS");
      add("fixtureGuard", o.guard);
      for (std::size_t i = 0; i < run.packets.size(); ++i) {
        const auto &p = run.packets[i];
        const std::string k = "packet" + std::to_string(i);
        add(k + "Size", std::to_string(p.bytes.size()));
        add(k + "Sha256", p.sha256);
        add(k + "Key", (p.flags & AV_PKT_FLAG_KEY) ? "1" : "0");
        add(k + "NalTypes", join(nal_types(p.bytes, c.video_format == 1)));
      }
      for (std::size_t i = 0; i < o.pushes.size(); ++i) {
        const auto &p = o.pushes[i];
        const std::string k = "push" + std::to_string(i);
        add(k + "Packet", std::to_string(p.packet));
        add(k + "ArrivalOffsetMs", std::to_string(p.at.count()));
        add(k + "Expected", p.expected_admit ? "admitted" : "not admitted");
        add(k + "Observed", p.admitted ? "admitted" : "not admitted");
        add(k + "Sequence", std::to_string(p.sequence));
        add(k + "LiveAfter", std::to_string(p.live_after));
      }
      add("pushesAsExpected", pushes_as_expected);
      add("readsCompared", reads_compared);
      for (std::size_t i = 0; i < o.reads.size(); ++i) {
        const auto &r = o.reads[i];
        const std::string k = "read" + std::to_string(i);
        add(k + "Label", r.label);
        add(k + "AtOffsetMs", std::to_string(r.at.count()));
        add(k + "Packets", std::to_string(r.stats.packets));
        add(k + "Bytes", std::to_string(r.stats.bytes));
        add(k + "Trimmed", std::to_string(r.stats.trimmed));
        add(k + "Keyframes", std::to_string(r.stats.run_keyframes));
        add(k + "RunSpanMs", ms_text(r.stats.run_span));
        add(k + "FrontAgeMs", ms_text(r.stats.run_front_age));
        add(k + "LastAcceptedKeyframeAgeMs", age_text(r.stats.last_accepted_keyframe_age));
        add(k + "LastWindowLimit", limit_name(r.stats.last_window_limit));
        add(k + "AwaitingKeyframe", r.stats.awaiting_keyframe ? "1" : "0");
        add(k + "RefusedAwaitingKeyframe", std::to_string(r.stats.refused_awaiting_keyframe));
        add(k + "RunVideoFormat", std::to_string(r.stats.run_video_format));
        add(k + "Check", r.check);
      }
      add("saveAtOffsetMs", o.save_at ? std::to_string(o.save_at->count()) : "");
      add("saveStatus", status_name(o.result.status));
      add("saveAdmitted", o.result.admitted ? "1" : "0");
      add("saveError", o.result.error);
      add("saveInOwnedDirectory", in_owned_dir ? "1" : "0");
      add("savePackets", std::to_string(o.result.packets));
      add("saveBytes", std::to_string(o.result.bytes));
      add("saveKeyframes", std::to_string(o.result.keyframes));
      add("saveSpanMs", ms_text(o.result.span));
      add("saveCheck", save_check);
      add("filesInOutputDirectory", std::to_string(o.files_in_output));
      add("expectedSuffix", o.expected.empty() ? "" : "4,5,6,7");
      add("expectedBytes", std::to_string(o.expected.size()));
      add("expectedSha256", o.expected.empty() ? "" : recorder_fixture::sha256_hex(o.expected.data(), o.expected.size()));
      add("fileBytes", std::to_string(o.file.size()));
      add("fileSha256", saved ? recorder_fixture::sha256_hex(o.file.data(), o.file.size()) : "");
      add("fileEqualsPackets4To7", file_equal);
      add("fileFirstAccessUnitNalTypes", join(o.first_types));
      add("retainedSequences", join(retained_sequences));
      add("retainedPayloadsCountersAndSequences", retained_equal);
      std::vector<std::int64_t> pts;
      for (const auto &p : run.packets) {
        pts.push_back(p.pts);
      }
      add("pts", join(pts));
      add("evidenceDirectory", case_name);
      add("evidenceResult", o.evidence.empty() ? "ok" : o.evidence);
      recorder_fixture::record_properties(k_marker, properties);

      ASSERT_EQ(o.encode_result, "") << "encoder run";
      ASSERT_EQ(o.baseline, "") << "frozen encoder baseline";
      ASSERT_EQ(o.guard, "ok") << "fixture assumption the expected outcome relies on";
      EXPECT_EQ(o.evidence, "") << "raw evidence";
      EXPECT_EQ(pushes_as_expected, "ok");
      ASSERT_EQ(reads_compared, "ok");
      for (const auto &r : o.reads) {
        EXPECT_EQ(r.check, "ok") << "read " << r.label << " at t0 + " << r.at.count() << " ms";
        EXPECT_STRNE(limit_name(r.stats.last_window_limit), "payload_cap") << "no cap may contribute: " << r.label;
        EXPECT_STRNE(limit_name(r.stats.last_window_limit), "entry_cap") << "no cap may contribute: " << r.label;
      }
      EXPECT_EQ(retained_equal, "ok") << "the final live run, with its producer counters and sequences";
      EXPECT_EQ(save_check, "ok") << status_name(o.result.status) << " " << o.result.error;
      if (!s.saves) {
        EXPECT_FALSE(o.result.admitted);
        EXPECT_EQ(o.files_in_output, 0u) << "an empty save creates no file";
        return;
      }
      ASSERT_TRUE(saved) << o.result.error;
      EXPECT_TRUE(in_owned_dir) << o.result.path;
      EXPECT_EQ(o.files_in_output, 1u);
      EXPECT_EQ(file_equal, "identical") << "the expired save must be packets 4..7, in order, byte for byte";
      EXPECT_TRUE(parameter_sets_then_idr(o.first_types, c.video_format == 1))
        << "the saved file must open with parameter sets and an IDR slice: " << join(o.first_types);
    }

    const steady::time_point t0 = steady::time_point {} + 1h;
    config::recording_t saved_config;
    std::string case_name;
    fs::path owned_parent;
    fs::path output_dir;
  };
}  // namespace

TEST_F(RecorderBitstreamExpiryTests, H264ExactDurationIsKeptAndALaterReadExpiresToTheNextKey) {
  check(k_h264, read_expiry_scenario());
}

TEST_F(RecorderBitstreamExpiryTests, HevcExactDurationIsKeptAndALaterReadExpiresToTheNextKey) {
  check(k_hevc, read_expiry_scenario());
}

TEST_F(RecorderBitstreamExpiryTests, H264AnAppendExpiresToTheNextKeyAsAnEarlierReadShows) {
  check(k_h264, append_expiry_scenario());
}

TEST_F(RecorderBitstreamExpiryTests, HevcAnAppendExpiresToTheNextKeyAsAnEarlierReadShows) {
  check(k_hevc, append_expiry_scenario());
}

TEST_F(RecorderBitstreamExpiryTests, H264AFullyStaleRunSavesNothingAndRestartsOnlyAtAKey) {
  check(k_h264, stale_run_scenario());
}

TEST_F(RecorderBitstreamExpiryTests, HevcAFullyStaleRunSavesNothingAndRestartsOnlyAtAKey) {
  check(k_hevc, stale_run_scenario());
}
