/**
 * @file tests/unit/test_recorder_bitstream_timing.cpp
 * @brief Real libx264/libx265 fixture packets pushed with supplied tap times: the replay buffer keeps
 * every supplied provenance coordinate and its evidence marks live and in a held save, and the
 * save is the same bytes (#377 under #329).
 *
 * Every case runs the bounded CPU encoder fixture (#363's support, unchanged) and requires its
 * frozen baseline: 8 packets, key flags exactly at 0 and 4. Three coordinates are supplied for each
 * push and kept apart throughout:
 * - the arrival clock, the steady time passed to push_video_at(): t0 + i * 20 ms. It drives the
 *   recorder's spans and ages. It is not a presentation time.
 * - the tap time, packet_provenance_t::frame_timestamp: t0 - 3 ms + i * 16667 us, earlier than
 *   that packet's arrival for every i (tapToArrivalUs = 3000 + 3333 * i). It is an injected test
 *   value standing where the production tap copies packet->frame_timestamp. It is evidence only:
 *   the recorder decides nothing from it, and nothing here calls it a presentation timestamp or a
 *   measured presentation duration.
 * - the producer counter, packet_provenance_t::frame_index = 37700 + i: explicit synthetic
 *   counters, deliberately not the packet PTS. The encoder's PTS/DTS are recorded as unrelated
 *   fixture coordinates and never compared with anything.
 * The configured duration (5 minutes) and the default caps hold the whole 140 ms run, so no expiry,
 * cap trim or realignment can act, and the first retained entry is packet 0.
 *
 * - T1, steady tap times (H.264 and HEVC): all 8 pushes are admitted; the live provenance read before
 *   the save and the held snapshot provenance of the admitted save both carry every supplied field
 *   exactly with no mark; the save is packets 0..7 byte for byte with 2 keyframes and an arrival span
 *   of exactly 140 ms. From the observed held records alone the test derives tapOffsetsUs, normalized
 *   to the first held entry's tap time, exactly [0, 16667, 33334, 50001, 66668, 83335, 100002,
 *   116669] (strictly increasing, tapSpanUs 116669), and shows that this tap span is not the
 *   observed arrival span (140000 us): the two coordinates are compared as complete vectors and
 *   final spans, never offset by offset (both are 0 at the first frame).
 * - T2, missing, duplicate and regressed tap times (H.264 and HEVC): packet 2 is untimed, packet 5
 *   repeats packet 4's tap time and packet 6 is 1 us earlier than packet 5. The run, the save and
 *   the bytes are exactly those of T1; the observed marks are exactly [none, none, time_missing,
 *   none, none, time_duplicate, time_regressed, none] (packet 3 is not compared because its
 *   neighbour is untimed), live and held; the derived tapOffsetsUs are [0, 16667, null, 50001,
 *   66668, 66668, 66667, 116669], not strictly increasing, first violation at 5, untimed [2], and
 *   nothing is substituted from the arrival clock. A tap-time regression never cuts the run; the
 *   recorder's own arrival-clock regression rule is a different, untouched thing.
 *
 * The marks are also recomputed by the test from the observed supplied values with the documented
 * adjacent-entry rule and must equal the observed marks; that recomputation is valid here because
 * nothing trims, so each entry's adjacent retained entry at admission is the previous entry.
 *
 * The before state of this slice is missing real-packet timing coverage (the earlier real-packet
 * slices supply no frame_timestamp), not a demonstrated product failure. Assertions are in-run byte,
 * counter and clock properties only. No packet hash is frozen here: whether these bytes equal
 * earlier runs, or the inputs a decoder was shown, is a separate readback's question. The saved file
 * is raw elementary video, not a container, playable-file, A/V or presentation claim.
 *
 * Evidence: CTest sets POLARIS_RECORDER_TIMING_EVIDENCE_DIR. Each case creates its own directory
 * there exclusively and writes the 8 source packets, the published save and a manifest whose
 * retained and held sections serialize what the recorder actually returned (not the declared
 * inputs), each created exclusively, fully written, synced, closed and reread, before any
 * assertion, so a failed case stays diagnosable. A case's evidence stays under 1 MiB. A missing,
 * relative, non-directory or symlinked root, an existing case directory or any write or reread
 * error fails the case; there is no fallback and no skip.
 */

#include "recorder_encoder_fixture_support.h"

#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
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
  using stream_recorder::packet_provenance_t;
  using stream_recorder::window_limit_e;
  using stream_recorder::detail::provenance_entry_t;
  using stream_recorder::detail::provenance_marks_t;
  using stream_recorder::detail::save_status_e;

  constexpr auto k_marker = "POLARIS_RECORDER_TIMING_JSON";
  constexpr auto k_evidence_env = "POLARIS_RECORDER_TIMING_EVIDENCE_DIR";
  constexpr std::uint64_t k_issue = 377;
  constexpr std::uint64_t k_session = 377;
  constexpr auto k_stamp = "20261011_192202";
  constexpr std::size_t k_packets = static_cast<std::size_t>(recorder_fixture::k_frames);
  /// Explicit synthetic producer counters for provenance: deliberately not the packet PTS.
  constexpr std::int64_t k_producer_counter_base = 37700;
  /// config::recording.replay_buffer_minutes for every case: the default, far above this run.
  constexpr int k_configured_minutes = 5;
  /// The controlled arrival clock, relative to t0, in microseconds. Not a presentation time.
  constexpr std::int64_t k_arrival_step_us = 20000;
  /// The supplied tap time, relative to t0, in microseconds: before each packet's arrival.
  constexpr std::int64_t k_tap_base_offset_us = -3000;
  constexpr std::int64_t k_tap_step_us = 16667;
  /// Every read and the save are stamped at the last arrival.
  constexpr std::int64_t k_read_at_us = k_arrival_step_us * static_cast<std::int64_t>(k_packets - 1);
  /// A case's sources, save and manifest together stay below this.
  constexpr std::size_t k_evidence_budget = std::size_t {1} << 20;
  /// The run's payload total the guard allows, so the sources plus a save of the same bytes and the
  /// manifest stay below k_evidence_budget.
  constexpr std::size_t k_max_run_bytes = std::size_t {448} * 1024;
  constexpr auto k_scope = "real fixture packets pushed with supplied tap times at a controlled arrival clock; supplied "
                           "provenance and marks observed live and held through a byte-identical save; tap offsets, "
                           "spans and marks recomputed from the observed held records; in-run byte, counter and clock "
                           "properties only; PTS/DTS recorded, never compared; no presentation, container, audio, A/V, "
                           "decode, playback or memory-safety claim";
  constexpr auto k_clock_note = "arrival is the steady clock passed to push_video_at (t0 + i * 20 ms); tap is the supplied "
                                "frame_timestamp (t0 - 3 ms + i * 16667 us unless a push says otherwise); the producer "
                                "counter is 37700 + i; none of them is a presentation time";

  constexpr std::int64_t arrival_us(std::size_t i) {
    return k_arrival_step_us * static_cast<std::int64_t>(i);
  }

  constexpr std::int64_t steady_tap_us(std::size_t i) {
    return k_tap_base_offset_us + k_tap_step_us * static_cast<std::int64_t>(i);
  }

  static_assert(k_read_at_us == 140000);
  static_assert(steady_tap_us(7) - steady_tap_us(0) == 116669);
  static_assert(arrival_us(7) - steady_tap_us(7) == 26331, "tapToArrivalUs is 3000 + 3333 * i");
  static_assert(steady_tap_us(7) < arrival_us(7), "every tap time precedes its packet's arrival");

  struct codec_case_t {
    const recorder_fixture::encoder_spec_t *spec;
    int video_format;  ///< The recorder's codec number: 0=H.264, 1=HEVC
    const char *label;
    const char *tag;
  };

  const codec_case_t k_h264 {&recorder_fixture::k_libx264, 0, "H.264", "h264"};
  const codec_case_t k_hevc {&recorder_fixture::k_libx265, 1, "HEVC", "hevc"};

  /// The supplied tap offsets (relative to t0, microseconds; empty = untimed) and what the observed
  /// held records must then derive to.
  struct scenario_t {
    const char *name = "";
    std::array<std::optional<std::int64_t>, 8> tap_us;
    std::array<provenance_marks_t, 8> marks;
    std::vector<std::optional<std::int64_t>> tap_offsets_us;  ///< Normalized to the first held entry
    std::int64_t tap_span_us = 0;
    bool strictly_increasing = false;
    std::optional<std::size_t> first_violation;
    std::vector<std::size_t> untimed;
  };

  provenance_marks_t marks_of(std::initializer_list<bool provenance_marks_t::*> set) {
    provenance_marks_t m;
    for (auto member : set) {
      m.*member = true;
    }
    return m;
  }

  scenario_t steady_scenario() {
    scenario_t s;
    s.name = "T1-steady";
    for (std::size_t i = 0; i < k_packets; ++i) {
      s.tap_us[i] = steady_tap_us(i);
      s.marks[i] = provenance_marks_t {};
      s.tap_offsets_us.push_back(steady_tap_us(i) - steady_tap_us(0));
    }
    s.tap_span_us = steady_tap_us(7) - steady_tap_us(0);
    s.strictly_increasing = true;
    s.first_violation = std::nullopt;
    return s;
  }

  scenario_t marks_scenario() {
    scenario_t s = steady_scenario();
    s.name = "T2-marks";
    s.tap_us[2] = std::nullopt;                  // untimed at the tap
    s.tap_us[5] = s.tap_us[4];                   // the same tap time as its neighbour
    s.tap_us[6] = *s.tap_us[5] - 1;              // 1 us earlier than its neighbour
    s.marks[2] = marks_of({&provenance_marks_t::time_missing});
    s.marks[5] = marks_of({&provenance_marks_t::time_duplicate});
    s.marks[6] = marks_of({&provenance_marks_t::time_regressed});
    s.tap_offsets_us.clear();
    for (std::size_t i = 0; i < k_packets; ++i) {
      if (s.tap_us[i]) {
        s.tap_offsets_us.push_back(*s.tap_us[i] - *s.tap_us[0]);
      } else {
        s.tap_offsets_us.push_back(std::nullopt);
      }
    }
    s.tap_span_us = *s.tap_us[7] - *s.tap_us[0];
    s.strictly_increasing = false;
    s.first_violation = 5;
    s.untimed = {2};
    return s;
  }

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

  const char *status_name(save_status_e s) {
    switch (s) {
      case save_status_e::saved:
        return "saved";
      case save_status_e::busy:
        return "busy";
      case save_status_e::disabled:
        return "disabled";
      case save_status_e::empty:
        return "empty";
      case save_status_e::storage_error:
        return "storage_error";
    }
    return "unknown";
  }

  std::string two_digits(std::size_t i) {
    return (i < 10 ? "0" : "") + std::to_string(i);
  }

  /// Whole microseconds, for evidence and comparison; every value here is a whole microsecond.
  std::int64_t us_count(steady::duration d) {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
  }

  std::string us_text(steady::duration d) {
    return std::to_string(us_count(d));
  }

  std::string age_text(const std::optional<steady::duration> &d) {
    return d ? us_text(*d) : std::string {"unknown"};
  }

  std::string optional_text(const std::optional<std::int64_t> &v) {
    return v ? std::to_string(*v) : std::string {"null"};
  }

  std::string bool_text(bool b) {
    return b ? "true" : "false";
  }

  /// The set marks by name, in declaration order.
  std::vector<std::string> mark_list(const provenance_marks_t &m) {
    std::vector<std::string> names;
    if (m.index_unsupplied) {
      names.push_back("index_unsupplied");
    }
    if (m.index_duplicate) {
      names.push_back("index_duplicate");
    }
    if (m.index_regressed) {
      names.push_back("index_regressed");
    }
    if (m.index_gap) {
      names.push_back("index_gap");
    }
    if (m.time_missing) {
      names.push_back("time_missing");
    }
    if (m.time_duplicate) {
      names.push_back("time_duplicate");
    }
    if (m.time_regressed) {
      names.push_back("time_regressed");
    }
    return names;
  }

  /// The set marks joined with '+', or "none".
  std::string mark_names(const provenance_marks_t &m) {
    const auto names = mark_list(m);
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
      out += (i ? "+" : "") + names[i];
    }
    return out.empty() ? "none" : out;
  }

  /// The same marks as a JSON array of names (empty when none is set).
  std::string mark_json(const provenance_marks_t &m) {
    const auto names = mark_list(m);
    std::string out = "[";
    for (std::size_t i = 0; i < names.size(); ++i) {
      out += (i ? "," : "") + recorder_fixture::json_string(names[i]);
    }
    return out + "]";
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

  std::string join_optional(const std::vector<std::optional<std::int64_t>> &values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
      out += (i ? "," : "") + optional_text(values[i]);
    }
    return out;
  }

  std::string join_marks(const std::vector<provenance_marks_t> &values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
      out += (i ? "," : "") + mark_names(values[i]);
    }
    return out;
  }

  /// A JSON array of integers or nulls.
  std::string json_optional_array(const std::vector<std::optional<std::int64_t>> &values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
      out += (i ? "," : "") + optional_text(values[i]);
    }
    return out + "]";
  }

  template<class T>
  std::string json_array(const std::vector<T> &values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
      out += (i ? "," : "") + std::to_string(values[i]);
    }
    return out + "]";
  }

  std::string json_marks_array(const std::vector<provenance_marks_t> &values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
      out += (i ? "," : "") + mark_json(values[i]);
    }
    return out + "]";
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
  // Observations: what the recorder actually returned, kept apart from the declared inputs.
  // ---------------------------------------------------------------------------------------------

  /// One push: its declared coordinates and what the live run showed right after it.
  struct push_t {
    std::size_t packet = 0;
    std::int64_t arrival_us = 0;
    std::optional<std::int64_t> tap_us;
    std::int64_t frame_index = 0;
    bool admitted = false;
    std::uint64_t sequence = 0;  ///< The new live entry's sequence, when admitted
    std::size_t live_after = 0;
  };

  struct read_t {
    std::string label;
    std::int64_t at_us = 0;
    stream_recorder::replay_stats_t stats;
    std::string check = "not compared";
  };

  /// One live or held provenance record exactly as the recorder returned it.
  struct observed_record_t {
    std::uint64_t sequence = 0;
    std::size_t payload_size = 0;
    std::string payload_sha256;
    bool payload_equals_source = false;
    std::uint16_t version = 0;
    std::optional<std::int64_t> frame_index;
    std::optional<std::int64_t> tap_us;  ///< frame_timestamp minus t0, when present
    bool source_clock_timestamp = false;
    bool source_time_paced = false;
    provenance_marks_t marks;
  };

  /// What the test derives from the observed held records alone.
  struct derived_t {
    std::optional<std::int64_t> anchor_us;                     ///< The first held entry's tap time
    std::vector<std::optional<std::int64_t>> tap_offsets_us;   ///< Normalized to the anchor
    std::optional<std::int64_t> tap_span_us;                   ///< Last timed entry minus the anchor
    bool strictly_increasing = false;                          ///< Over consecutive timed entries
    std::optional<std::size_t> first_violation;
    std::vector<std::size_t> untimed;
    std::vector<provenance_marks_t> recomputed_marks;
    bool recomputed_equal_observed = false;
    bool held_equals_retained = false;
    std::vector<std::int64_t> arrival_offsets_us;              ///< Declared inputs, normalized
    std::optional<std::int64_t> arrival_span_us;               ///< Observed: the save's span
    bool arrival_span_is_not_tap_span = false;
    std::vector<std::optional<std::int64_t>> tap_to_arrival_us;
    std::string check = "not derived";
  };

  /// Everything one case observed, kept for evidence and properties before any assertion.
  struct observed_t {
    std::string encode_result;
    std::string baseline;
    std::string guard = "not run";  ///< The fixture assumptions the expectations rely on
    std::vector<push_t> pushes;
    std::vector<read_t> reads;
    std::vector<observed_record_t> retained;  ///< Live provenance read before admission
    std::vector<observed_record_t> held;      ///< The admitted save's snapshot provenance
    std::size_t held_payload_count = 0;
    std::size_t held_provenance_count = 0;
    std::vector<std::uint64_t> retained_after_save;  ///< Live sequences after completion
    bool retained_after_identical = false;
    stream_recorder::detail::save_result_t admission;
    stream_recorder::detail::save_result_t result;
    derived_t derived;
    std::vector<std::uint8_t> file;
    std::vector<std::uint8_t> expected;  ///< Packets 0..7 concatenated
    std::vector<int> first_types;        ///< NAL types of the saved file's first access unit
    std::size_t files_in_output = 0;
    std::string evidence = "not written";
  };

  /// What one read must show. Bytes and keyframes follow from the 8 packets.
  struct read_expect_t {
    const char *label;
    bool save_in_progress;
    std::size_t snapshot_packets;
    std::uint64_t saves_completed;
  };

  observed_record_t observe(const provenance_entry_t &entry, const recorder_fixture::run_t &run, std::size_t position,
                            steady::time_point t0) {
    observed_record_t r;
    r.sequence = entry.sequence;
    r.payload_size = entry.payload.size();
    r.payload_sha256 = recorder_fixture::sha256_hex(entry.payload.data(), entry.payload.size());
    r.payload_equals_source = position < run.packets.size() && entry.payload == run.packets[position].bytes;
    r.version = entry.record.version;
    r.frame_index = entry.record.supplied.frame_index;
    if (entry.record.supplied.frame_timestamp) {
      r.tap_us = us_count(*entry.record.supplied.frame_timestamp - t0);
    }
    r.source_clock_timestamp = entry.record.supplied.source_clock_timestamp;
    r.source_time_paced = entry.record.supplied.source_time_paced;
    r.marks = entry.record.marks;
    return r;
  }

  bool same_record(const observed_record_t &a, const observed_record_t &b) {
    return a.payload_size == b.payload_size && a.payload_sha256 == b.payload_sha256 && a.version == b.version &&
           a.frame_index == b.frame_index && a.tap_us == b.tap_us && a.source_clock_timestamp == b.source_clock_timestamp &&
           a.source_time_paced == b.source_time_paced && a.marks == b.marks;
  }

  /// The documented adjacent-entry rule applied to the observed supplied values. Valid here because
  /// nothing trims this run, so each entry's adjacent retained entry at admission is the previous one.
  provenance_marks_t recompute_marks(const std::vector<observed_record_t> &records, std::size_t i) {
    provenance_marks_t m;
    const auto &cur = records[i];
    m.index_unsupplied = !cur.frame_index;
    m.time_missing = !cur.tap_us;
    if (i == 0) {
      return m;
    }
    const auto &prev = records[i - 1];
    if (cur.frame_index && prev.frame_index) {
      m.index_duplicate = *cur.frame_index == *prev.frame_index;
      m.index_regressed = *cur.frame_index < *prev.frame_index;
      m.index_gap = *cur.frame_index > *prev.frame_index &&
                    static_cast<std::uint64_t>(*cur.frame_index) - static_cast<std::uint64_t>(*prev.frame_index) > 1;
    }
    if (cur.tap_us && prev.tap_us) {
      m.time_duplicate = *cur.tap_us == *prev.tap_us;
      m.time_regressed = *cur.tap_us < *prev.tap_us;
    }
    return m;
  }

  derived_t derive(const observed_t &o) {
    derived_t d;
    const auto &held = o.held;
    if (held.empty()) {
      d.check = "no held records";
      return d;
    }
    d.anchor_us = held[0].tap_us;
    std::optional<std::int64_t> previous;
    for (std::size_t i = 0; i < held.size(); ++i) {
      if (held[i].tap_us && d.anchor_us) {
        const std::int64_t offset = *held[i].tap_us - *d.anchor_us;
        d.tap_offsets_us.push_back(offset);
        d.tap_span_us = offset;
        if (previous && offset <= *previous && !d.first_violation) {
          d.first_violation = i;
        }
        previous = offset;
      } else {
        d.tap_offsets_us.push_back(std::nullopt);
        d.untimed.push_back(i);
      }
      d.recomputed_marks.push_back(recompute_marks(held, i));
    }
    d.strictly_increasing = !d.first_violation && d.untimed.empty() && d.anchor_us.has_value();
    d.recomputed_equal_observed = true;
    for (std::size_t i = 0; i < held.size(); ++i) {
      d.recomputed_equal_observed = d.recomputed_equal_observed && d.recomputed_marks[i] == held[i].marks;
    }
    d.held_equals_retained = held.size() == o.retained.size();
    for (std::size_t i = 0; d.held_equals_retained && i < held.size(); ++i) {
      d.held_equals_retained = same_record(held[i], o.retained[i]);
    }
    for (std::size_t i = 0; i < o.pushes.size(); ++i) {
      d.arrival_offsets_us.push_back(o.pushes[i].arrival_us - o.pushes[0].arrival_us);
      if (i < held.size() && held[i].tap_us) {
        d.tap_to_arrival_us.push_back(o.pushes[i].arrival_us - *held[i].tap_us);
      } else {
        d.tap_to_arrival_us.push_back(std::nullopt);
      }
    }
    if (o.result.admitted) {
      d.arrival_span_us = us_count(o.result.span);
    }
    d.arrival_span_is_not_tap_span = d.arrival_span_us && d.tap_span_us && *d.arrival_span_us != *d.tap_span_us;
    d.check = "ok";
    return d;
  }

  /// Compare the derived values with the scenario, exactly; "ok" or every differing field.
  std::string compare_derived(const derived_t &d, const scenario_t &s, const observed_t &o) {
    std::string out;
    const auto field = [&out](const char *name, const std::string &got, const std::string &want) {
      if (got != want) {
        out += std::string {out.empty() ? "" : "; "} + name + " " + got + ", expected " + want;
      }
    };
    field("tapOffsetsUs", join_optional(d.tap_offsets_us), join_optional(s.tap_offsets_us));
    field("tapSpanUs", optional_text(d.tap_span_us), std::to_string(s.tap_span_us));
    field("tapStrictlyIncreasing", bool_text(d.strictly_increasing), bool_text(s.strictly_increasing));
    field("firstTapViolation", d.first_violation ? std::to_string(*d.first_violation) : "null",
          s.first_violation ? std::to_string(*s.first_violation) : "null");
    field("untimed", join(d.untimed), join(s.untimed));
    std::vector<provenance_marks_t> expected_marks(s.marks.begin(), s.marks.end());
    field("recomputedMarks", join_marks(d.recomputed_marks), join_marks(expected_marks));
    field("recomputedMarksEqualObserved", bool_text(d.recomputed_equal_observed), "true");
    field("heldEqualsRetained", bool_text(d.held_equals_retained), "true");
    std::vector<std::int64_t> expected_arrivals;
    std::vector<std::optional<std::int64_t>> expected_tap_to_arrival;
    for (std::size_t i = 0; i < k_packets; ++i) {
      expected_arrivals.push_back(arrival_us(i) - arrival_us(0));
      if (s.tap_us[i]) {
        expected_tap_to_arrival.push_back(arrival_us(i) - *s.tap_us[i]);
      } else {
        expected_tap_to_arrival.push_back(std::nullopt);
      }
    }
    field("arrivalOffsetsUs", join(d.arrival_offsets_us), join(expected_arrivals));
    field("arrivalSpanUs", optional_text(d.arrival_span_us), std::to_string(arrival_us(7) - arrival_us(0)));
    field("arrivalSpanIsNotTapSpan", bool_text(d.arrival_span_is_not_tap_span), "true");
    field("tapToArrivalUs", join_optional(d.tap_to_arrival_us), join_optional(expected_tap_to_arrival));
    // The observed arrival span must also be what the live run reported before the save.
    if (!o.reads.empty()) {
      field("runSpanUsBeforeSave", us_text(o.reads[0].stats.run_span), std::to_string(arrival_us(7) - arrival_us(0)));
    }
    return out.empty() ? "ok" : out;
  }

  /// Compare one provenance list with the scenario, exactly; "ok" or the first difference.
  std::string compare_records(const std::vector<observed_record_t> &records, const scenario_t &s, const std::vector<push_t> &pushes,
                              bool live) {
    if (records.size() != k_packets) {
      return "observed " + std::to_string(records.size()) + " entries";
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
      const auto &r = records[i];
      const std::string at = " at entry " + std::to_string(i);
      if (!r.payload_equals_source) {
        return "payload differs from source packet" + at;
      }
      if (r.version != packet_provenance_t::k_version) {
        return "version " + std::to_string(r.version) + at;
      }
      if (!r.frame_index || *r.frame_index != k_producer_counter_base + static_cast<std::int64_t>(i)) {
        return "producer counter " + optional_text(r.frame_index) + at;
      }
      if (r.tap_us != s.tap_us[i]) {
        return "tap time " + optional_text(r.tap_us) + ", supplied " + optional_text(s.tap_us[i]) + at;
      }
      if (r.source_clock_timestamp || r.source_time_paced) {
        return "a source flag is set" + at;
      }
      if (!(r.marks == s.marks[i])) {
        return "marks " + mark_names(r.marks) + ", expected " + mark_names(s.marks[i]) + at;
      }
      if (live) {
        if (i >= pushes.size() || !pushes[i].admitted || r.sequence != pushes[i].sequence) {
          return "sequence " + std::to_string(r.sequence) + " differs from push" + at;
        }
        if (i > 0 && r.sequence <= records[i - 1].sequence) {
          return "sequence not increasing" + at;
        }
      } else if (r.sequence != 0) {
        return "held entry carries a live sequence" + at;
      }
    }
    return "ok";
  }

  /// Compare one read with its expectation, exactly; "ok" or every differing field.
  std::string compare_read(const read_t &r, const read_expect_t &e, const recorder_fixture::run_t &run, int video_format) {
    std::size_t bytes = 0;
    for (const auto &p : run.packets) {
      bytes += p.bytes.size();
    }
    std::string out;
    const auto field = [&out](const char *name, const std::string &got, const std::string &want) {
      if (got != want) {
        out += std::string {out.empty() ? "" : "; "} + name + " " + got + ", expected " + want;
      }
    };
    const auto &s = r.stats;
    const std::string span = std::to_string(arrival_us(7) - arrival_us(0));
    field("packets", std::to_string(s.packets), std::to_string(k_packets));
    field("bytes", std::to_string(s.bytes), std::to_string(bytes));
    field("trimmed", std::to_string(s.trimmed), "0");
    field("runKeyframes", std::to_string(s.run_keyframes), "2");
    field("keyframeStartRun", s.keyframe_start_run ? "1" : "0", "1");
    field("runSpanUs", us_text(s.run_span), span);
    field("frontAgeUs", us_text(s.run_front_age), std::to_string(k_read_at_us - arrival_us(0)));
    field("lastAcceptedKeyframeAgeUs", age_text(s.last_accepted_keyframe_age), std::to_string(k_read_at_us - arrival_us(4)));
    field("lastWindowLimit", limit_name(s.last_window_limit), "selection");
    field("awaitingKeyframe", s.awaiting_keyframe ? "1" : "0", "0");
    field("refusedAwaitingKeyframe", std::to_string(s.refused_awaiting_keyframe), "0");
    field("refusedOversized", std::to_string(s.refused_oversized), "0");
    field("refusedInvalid", std::to_string(s.refused_invalid), "0");
    field("refusedUnselected", std::to_string(s.refused_unselected), "0");
    field("discontinuities", std::to_string(s.discontinuities), "0");
    field("selectedGeneration", std::to_string(s.selected_generation), std::to_string(k_session));
    field("registeredSessions", std::to_string(s.registered_sessions), "1");
    field("runVideoFormat", std::to_string(s.run_video_format), std::to_string(video_format));
    field("durationMinutes", std::to_string(s.duration.count()), std::to_string(k_configured_minutes));
    field("maxBytes", std::to_string(s.max_bytes), std::to_string(stream_recorder::k_replay_max_payload_bytes));
    field("maxPackets", std::to_string(s.max_packets), std::to_string(stream_recorder::k_replay_max_packets));
    field("saveInProgress", s.save_in_progress ? "1" : "0", e.save_in_progress ? "1" : "0");
    field("snapshotPackets", std::to_string(s.snapshot_packets), std::to_string(e.snapshot_packets));
    field("snapshotBytes", std::to_string(s.snapshot_bytes), std::to_string(e.save_in_progress ? bytes : std::size_t {0}));
    field("snapshotKeyframes", std::to_string(s.snapshot_keyframes), e.save_in_progress ? "2" : "0");
    field("snapshotSpanUs", us_text(s.snapshot_span), e.save_in_progress ? span : "0");
    field("metadataEntries", std::to_string(s.metadata_entries), std::to_string(k_packets + e.snapshot_packets));
    field("savesCompleted", std::to_string(s.saves_completed), std::to_string(e.saves_completed));
    field("savesFailed", std::to_string(s.saves_failed), "0");
    field("savesRefusedBusy", std::to_string(s.saves_refused_busy), "0");
    return out.empty() ? "ok" : out;
  }

  /**
   * One eligible session with a known codec in replay mode, saving into a `recordings` directory
   * inside a parent this fixture created exclusively. TearDown restores the recorder and the
   * configuration and removes only that parent; the retained evidence is never touched.
   */
  class RecorderBitstreamTimingTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_config = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      case_name = info->name();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-bitstream-timing-"} + case_name + "-" +
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
      if (run.packets.size() != k_packets) {
        return "expected " + std::to_string(k_packets) + " packets";
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

    /// Push the 8 packets, read, admit the save, inspect it held, complete it, read again. Pushes
    /// are observed through the live entries, which releases nothing.
    void run_scenario(const codec_case_t &c, const scenario_t &s, const recorder_fixture::run_t &run, observed_t &o) {
      stream_recorder::session_started(k_session, true, c.video_format);
      std::uint64_t newest_sequence = 0;
      for (std::size_t i = 0; i < k_packets; ++i) {
        const auto &p = run.packets[i];
        push_t push;
        push.packet = i;
        push.arrival_us = arrival_us(i);
        push.tap_us = s.tap_us[i];
        push.frame_index = k_producer_counter_base + static_cast<std::int64_t>(i);
        packet_provenance_t provenance;
        provenance.frame_index = push.frame_index;
        if (push.tap_us) {
          provenance.frame_timestamp = t0 + std::chrono::microseconds(*push.tap_us);
        }
        provenance.source_clock_timestamp = false;
        provenance.source_time_paced = false;
        stream_recorder::detail::push_video_at(p.bytes.data(), p.bytes.size(), (p.flags & AV_PKT_FLAG_KEY) != 0, k_session,
                                               c.video_format, provenance, t0 + std::chrono::microseconds(push.arrival_us));
        const auto live = stream_recorder::detail::replay_provenance_for_tests();
        push.live_after = live.size();
        if (!live.empty() && live.back().sequence > newest_sequence && live.back().payload == p.bytes) {
          push.admitted = true;
          push.sequence = live.back().sequence;
          newest_sequence = push.sequence;
        }
        o.pushes.push_back(push);
      }
      const auto at = t0 + std::chrono::microseconds(k_read_at_us);
      o.reads.push_back(read_t {"beforeSave", k_read_at_us, stream_recorder::detail::replay_stats_at(at)});
      {
        const auto live = stream_recorder::detail::replay_provenance_for_tests();
        for (std::size_t i = 0; i < live.size(); ++i) {
          o.retained.push_back(observe(live[i], run, i, t0));
        }
      }
      auto admission = stream_recorder::detail::admit_replay_save(at);
      o.admission = admission.result;
      if (admission.result.admitted) {
        const auto snapshot = stream_recorder::detail::snapshot_provenance_for_tests(admission.save);
        o.held_payload_count = snapshot.payload_count;
        o.held_provenance_count = snapshot.provenance_count;
        for (std::size_t i = 0; i < snapshot.entries.size(); ++i) {
          o.held.push_back(observe(snapshot.entries[i], run, i, t0));
        }
        o.reads.push_back(read_t {"whileHeld", k_read_at_us, stream_recorder::detail::replay_stats_at(at)});
        o.result = stream_recorder::detail::complete_replay_save(std::move(admission.save));
      } else {
        o.result = admission.result;
      }
      o.reads.push_back(read_t {"afterSave", k_read_at_us, stream_recorder::detail::replay_stats_at(at)});
      {
        const auto live = stream_recorder::detail::replay_provenance_for_tests();
        o.retained_after_identical = live.size() == o.retained.size();
        for (std::size_t i = 0; i < live.size(); ++i) {
          o.retained_after_save.push_back(live[i].sequence);
          if (o.retained_after_identical) {
            const auto again = observe(live[i], run, i, t0);
            o.retained_after_identical = again.sequence == o.retained[i].sequence && same_record(again, o.retained[i]);
          }
        }
      }
    }

    /// One observed record. A held entry's sequence is the snapshot API's sentinel 0 (the snapshot
    /// carries no live sequence); its payload size and hash come from the copied snapshot payload.
    std::string record_json(const observed_record_t &r, std::size_t position, bool held) const {
      return "{\"position\":" + std::to_string(position) + ",\"sequence\":" + std::to_string(r.sequence) +
             ",\"sequenceIsApiSentinel\":" + bool_text(held) +
             ",\"payloadSize\":" + std::to_string(r.payload_size) + ",\"payloadSha256\":" + recorder_fixture::json_string(r.payload_sha256) +
             ",\"payloadEqualsSource\":" + bool_text(r.payload_equals_source) + ",\"version\":" + std::to_string(r.version) +
             ",\"frameIndex\":" + optional_text(r.frame_index) + ",\"tapOffsetUs\":" + optional_text(r.tap_us) +
             ",\"sourceClockTimestamp\":" + bool_text(r.source_clock_timestamp) + ",\"sourceTimePaced\":" + bool_text(r.source_time_paced) +
             ",\"marks\":" + mark_json(r.marks) + "}";
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
                             ",\"issue\":" + std::to_string(k_issue) + ",\"session\":" + std::to_string(k_session) +
                             ",\"clock\":{\"arrivalStepUs\":" + std::to_string(k_arrival_step_us) +
                             ",\"tapBaseOffsetUs\":" + std::to_string(k_tap_base_offset_us) + ",\"tapStepUs\":" + std::to_string(k_tap_step_us) +
                             ",\"readAtUs\":" + std::to_string(k_read_at_us) + ",\"saveAtUs\":" + std::to_string(k_read_at_us) +
                             ",\"configuredMinutes\":" + std::to_string(k_configured_minutes) + ",\"capsDefault\":true" +
                             ",\"note\":" + recorder_fixture::json_string(k_clock_note) + "}" +
                             ",\"sources\":[";
      for (std::size_t i = 0; i < run.packets.size() && i < k_packets; ++i) {
        const auto &p = run.packets[i];
        const std::string name = std::string {"source-"} + c.tag + "-" + two_digits(i) + ".bin";
        if (auto error = writer.write(name, p.bytes); !error.empty()) {
          return error;
        }
        manifest += std::string {i ? "," : ""} + "{\"file\":" + recorder_fixture::json_string(name) +
                    ",\"index\":" + std::to_string(i) + ",\"key\":" + ((p.flags & AV_PKT_FLAG_KEY) ? "true" : "false") +
                    ",\"size\":" + std::to_string(p.bytes.size()) + ",\"sha256\":" + recorder_fixture::json_string(p.sha256) +
                    ",\"pts\":" + std::to_string(p.pts) + ",\"dts\":" + std::to_string(p.dts) +
                    ",\"nalTypes\":" + recorder_fixture::json_string(join(nal_types(p.bytes, c.video_format == 1))) + "}";
      }
      manifest += "],\"pushes\":[";
      for (std::size_t i = 0; i < o.pushes.size(); ++i) {
        const auto &p = o.pushes[i];
        manifest += std::string {i ? "," : ""} + "{\"ordinal\":" + std::to_string(i) +
                    ",\"source\":" + recorder_fixture::json_string(std::string {c.tag} + ":" + std::to_string(p.packet)) +
                    ",\"arrivalOffsetUs\":" + std::to_string(p.arrival_us) + ",\"tapOffsetUs\":" + optional_text(p.tap_us) +
                    ",\"frameIndex\":" + std::to_string(p.frame_index) + ",\"sourceClockTimestamp\":false,\"sourceTimePaced\":false" +
                    ",\"expected\":\"admitted\",\"observed\":" + recorder_fixture::json_string(p.admitted ? "admitted" : "not admitted") +
                    ",\"sequence\":" + std::to_string(p.sequence) + ",\"liveAfter\":" + std::to_string(p.live_after) + "}";
      }
      manifest += "],\"reads\":[";
      for (std::size_t i = 0; i < o.reads.size(); ++i) {
        const auto &r = o.reads[i];
        manifest += std::string {i ? "," : ""} + "{\"label\":" + recorder_fixture::json_string(r.label) +
                    ",\"atOffsetUs\":" + std::to_string(r.at_us) + ",\"packets\":" + std::to_string(r.stats.packets) +
                    ",\"bytes\":" + std::to_string(r.stats.bytes) + ",\"trimmed\":" + std::to_string(r.stats.trimmed) +
                    ",\"runKeyframes\":" + std::to_string(r.stats.run_keyframes) + ",\"runSpanUs\":" + us_text(r.stats.run_span) +
                    ",\"frontAgeUs\":" + us_text(r.stats.run_front_age) +
                    ",\"lastAcceptedKeyframeAgeUs\":" + recorder_fixture::json_string(age_text(r.stats.last_accepted_keyframe_age)) +
                    ",\"lastWindowLimit\":" + recorder_fixture::json_string(limit_name(r.stats.last_window_limit)) +
                    ",\"awaitingKeyframe\":" + bool_text(r.stats.awaiting_keyframe) +
                    ",\"refusedAwaitingKeyframe\":" + std::to_string(r.stats.refused_awaiting_keyframe) +
                    ",\"discontinuities\":" + std::to_string(r.stats.discontinuities) +
                    ",\"saveInProgress\":" + bool_text(r.stats.save_in_progress) +
                    ",\"snapshotPackets\":" + std::to_string(r.stats.snapshot_packets) +
                    ",\"snapshotKeyframes\":" + std::to_string(r.stats.snapshot_keyframes) +
                    ",\"snapshotSpanUs\":" + us_text(r.stats.snapshot_span) +
                    ",\"savesCompleted\":" + std::to_string(r.stats.saves_completed) +
                    ",\"maxBytes\":" + std::to_string(r.stats.max_bytes) + ",\"maxPackets\":" + std::to_string(r.stats.max_packets) +
                    ",\"check\":" + recorder_fixture::json_string(r.check) + "}";
      }
      manifest += "],\"retained\":[";
      for (std::size_t i = 0; i < o.retained.size(); ++i) {
        manifest += std::string {i ? "," : ""} + record_json(o.retained[i], i, false);
      }
      manifest += "],\"held\":{\"payloadCount\":" + std::to_string(o.held_payload_count) +
                  ",\"provenanceCount\":" + std::to_string(o.held_provenance_count) +
                  ",\"sequenceNote\":\"snapshot_provenance_for_tests returns copied payloads paired with provenance and the sequence sentinel 0; "
                  "heldEqualsRetained compares the copied payload and provenance fields, never the sentinel\"" +
                  ",\"entries\":[";
      for (std::size_t i = 0; i < o.held.size(); ++i) {
        manifest += std::string {i ? "," : ""} + record_json(o.held[i], i, true);
      }
      const auto &d = o.derived;
      std::vector<provenance_marks_t> expected_marks(s.marks.begin(), s.marks.end());
      manifest += "]},\"retainedAfterSave\":{\"entries\":" + std::to_string(o.retained_after_save.size()) +
                  ",\"sequences\":" + json_array(o.retained_after_save) +
                  ",\"identicalToRetained\":" + bool_text(o.retained_after_identical) + "}" +
                  ",\"derived\":{\"anchor\":\"the first held entry's tap time\",\"anchorTapOffsetUs\":" + optional_text(d.anchor_us) +
                  ",\"tapOffsetsUs\":" + json_optional_array(d.tap_offsets_us) + ",\"tapSpanUs\":" + optional_text(d.tap_span_us) +
                  ",\"tapStrictlyIncreasing\":" + bool_text(d.strictly_increasing) +
                  ",\"firstTapViolation\":" + (d.first_violation ? std::to_string(*d.first_violation) : "null") +
                  ",\"untimed\":" + json_array(d.untimed) + ",\"recomputedMarks\":" + json_marks_array(d.recomputed_marks) +
                  ",\"recomputedMarksEqualObserved\":" + bool_text(d.recomputed_equal_observed) +
                  ",\"heldEqualsRetained\":" + bool_text(d.held_equals_retained) +
                  ",\"arrivalOffsetsUs\":" + json_array(d.arrival_offsets_us) + ",\"arrivalSpanUs\":" + optional_text(d.arrival_span_us) +
                  ",\"arrivalSpanIsNotTapSpan\":" + bool_text(d.arrival_span_is_not_tap_span) +
                  ",\"tapToArrivalUs\":" + json_optional_array(d.tap_to_arrival_us) +
                  ",\"note\":\"recomputed from the observed held records and the observed save span; arrivalOffsetsUs are the declared inputs\"" +
                  ",\"check\":" + recorder_fixture::json_string(d.check) + "}";
      const bool saved = o.result.status == save_status_e::saved;
      manifest += ",\"save\":{\"atOffsetUs\":" + std::to_string(k_read_at_us) +
                  ",\"status\":" + recorder_fixture::json_string(status_name(o.result.status)) +
                  ",\"admitted\":" + bool_text(o.result.admitted) + ",\"packets\":" + std::to_string(o.result.packets) +
                  ",\"bytes\":" + std::to_string(o.result.bytes) + ",\"keyframes\":" + std::to_string(o.result.keyframes) +
                  ",\"spanUs\":" + us_text(o.result.span) + ",\"videoFormat\":" + std::to_string(o.result.video_format) +
                  ",\"generation\":" + std::to_string(o.result.generation) +
                  ",\"inOwnedDirectory\":" + bool_text(saved && fs::path(o.result.path).parent_path() == output_dir) +
                  ",\"filesInOutputDirectory\":" + std::to_string(o.files_in_output) + "}" + ",\"saved\":";
      if (saved) {
        if (auto error = writer.write("saved.bin", o.file); !error.empty()) {
          return error;
        }
        manifest += "{\"file\":\"saved.bin\",\"size\":" + std::to_string(o.file.size()) +
                    ",\"sha256\":" + recorder_fixture::json_string(recorder_fixture::sha256_hex(o.file.data(), o.file.size())) +
                    ",\"equalsPackets0To7\":" + bool_text(o.file == o.expected) +
                    ",\"firstAccessUnitNalTypes\":" + recorder_fixture::json_string(join(o.first_types)) +
                    ",\"parameterSetsThenIdr\":" + bool_text(parameter_sets_then_idr(o.first_types, c.video_format == 1)) + "}";
      } else {
        manifest += "null";
      }
      manifest += ",\"expected\":{\"tapOffsetsUs\":" + json_optional_array(s.tap_offsets_us) + ",\"tapSpanUs\":" + std::to_string(s.tap_span_us) +
                  ",\"arrivalSpanUs\":" + std::to_string(arrival_us(7) - arrival_us(0)) + ",\"marks\":" + json_marks_array(expected_marks) +
                  ",\"note\":\"declared for the reader's discrimination only; the retained, held and derived sections are observations\"}" +
                  ",\"fixture\":{\"encoder\":" + recorder_fixture::json_string(c.spec->name) +
                  ",\"params\":" + recorder_fixture::json_string(c.spec->params_value) +
                  ",\"ffmpegVersionInfo\":" + recorder_fixture::json_string(av_version_info()) +
                  ",\"libavcodecIdent\":" + recorder_fixture::json_string(LIBAVCODEC_IDENT) + "}" +
                  ",\"scope\":" + recorder_fixture::json_string(k_scope) + "}\n";
      return writer.write("manifest.json", manifest);
    }

    void add_record_properties(properties_t &properties, const char *prefix, const std::vector<observed_record_t> &records) const {
      const auto add = [&properties](std::string key, std::string value) {
        properties.emplace_back(std::move(key), std::move(value));
      };
      for (std::size_t i = 0; i < records.size(); ++i) {
        const auto &r = records[i];
        const std::string k = prefix + std::to_string(i);
        add(k + "Sequence", std::to_string(r.sequence));
        add(k + "PayloadSize", std::to_string(r.payload_size));
        add(k + "PayloadSha256", r.payload_sha256);
        add(k + "PayloadEqualsSource", r.payload_equals_source ? "1" : "0");
        add(k + "Version", std::to_string(r.version));
        add(k + "FrameIndex", optional_text(r.frame_index));
        add(k + "TapOffsetUs", optional_text(r.tap_us));
        add(k + "SourceClockTimestamp", r.source_clock_timestamp ? "1" : "0");
        add(k + "SourceTimePaced", r.source_time_paced ? "1" : "0");
        add(k + "Marks", mark_names(r.marks));
      }
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
      const std::vector<read_expect_t> expected_reads {
        {"beforeSave", false, 0, 0},
        {"whileHeld", true, k_packets, 0},
        {"afterSave", false, 0, 1},
      };
      if (ready) {
        run_scenario(c, s, run, o);
        if (o.result.status == save_status_e::saved) {
          std::ifstream in(o.result.path, std::ios::binary);
          o.file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        for (const auto &p : run.packets) {
          o.expected.insert(o.expected.end(), p.bytes.begin(), p.bytes.end());
        }
        // The saved file's first access unit is as long as packet 0, if the save is what it should be.
        if (o.file.size() >= run.packets[0].bytes.size()) {
          const auto first_size = static_cast<std::ptrdiff_t>(run.packets[0].bytes.size());
          o.first_types = nal_types(std::vector<std::uint8_t>(o.file.begin(), o.file.begin() + first_size), c.video_format == 1);
        }
        for (std::size_t i = 0; i < o.reads.size(); ++i) {
          const auto expectation = std::find_if(expected_reads.begin(), expected_reads.end(), [&](const read_expect_t &e) {
            return o.reads[i].label == e.label;
          });
          o.reads[i].check = expectation == expected_reads.end() ? "unexpected read" : compare_read(o.reads[i], *expectation, run, c.video_format);
        }
        o.derived = derive(o);
      }

      std::string pushes_as_expected = ready ? "ok" : "not run";
      if (ready && o.pushes.size() != k_packets) {
        pushes_as_expected = "observed " + std::to_string(o.pushes.size()) + " pushes";
      }
      for (std::size_t i = 0; pushes_as_expected == "ok" && i < o.pushes.size(); ++i) {
        if (!o.pushes[i].admitted || o.pushes[i].live_after != i + 1) {
          pushes_as_expected = "push " + std::to_string(i) + (o.pushes[i].admitted ? " admitted with " + std::to_string(o.pushes[i].live_after) + " live entries" : " not admitted");
        }
      }
      const std::string reads_compared = !ready ? "not run" : (o.reads.size() == expected_reads.size() ? "ok" : "observed " + std::to_string(o.reads.size()) + " reads");
      const std::string retained_check = ready ? compare_records(o.retained, s, o.pushes, true) : "not run";
      std::string held_check = ready ? compare_records(o.held, s, o.pushes, false) : "not run";
      if (held_check == "ok" && (o.held_payload_count != k_packets || o.held_provenance_count != k_packets)) {
        held_check = "held counts " + std::to_string(o.held_payload_count) + "/" + std::to_string(o.held_provenance_count);
      }
      const std::string derived_check = ready ? compare_derived(o.derived, s, o) : "not run";

      const bool saved = o.result.status == save_status_e::saved;
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
        const auto exact_span = std::chrono::duration_cast<steady::duration>(std::chrono::microseconds(arrival_us(7) - arrival_us(0)));
        if (!(o.admission.admitted && o.admission.packets == k_packets && o.admission.keyframes == 2 && o.admission.bytes == o.expected.size() &&
              o.admission.span == exact_span)) {
          save_check = "the admission did not capture exactly packets 0..7";
        } else if (!(saved && o.result.admitted && o.result.packets == k_packets && o.result.keyframes == 2 &&
                     o.result.bytes == o.expected.size() && o.result.span == exact_span &&
                     o.result.video_format == c.video_format && o.result.generation == k_session)) {
          save_check = "the save is not exactly packets 0..7 of this session's run";
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
      add("issue", std::to_string(k_issue));
      add("ffmpegVersionInfo", av_version_info());
      add("libavcodecIdent", LIBAVCODEC_IDENT);
      recorder_fixture::add_encoder_run(properties, *c.spec, run, o.encode_result, o.baseline);
      add("codec", c.label);
      add("recorderVideoFormat", std::to_string(c.video_format));
      add("sessionGeneration", std::to_string(k_session));
      add("configuredMinutes", std::to_string(k_configured_minutes));
      add("capsDefault", "1");
      add("arrivalStepUs", std::to_string(k_arrival_step_us));
      add("tapBaseOffsetUs", std::to_string(k_tap_base_offset_us));
      add("tapStepUs", std::to_string(k_tap_step_us));
      add("readAtUs", std::to_string(k_read_at_us));
      add("saveAtUs", std::to_string(k_read_at_us));
      add("clockNote", k_clock_note);
      add("ptsNote", "packet PTS/DTS are the fixture's synthetic counters, recorded only; no expectation is derived from them");
      add("producerCounterNote", "explicit synthetic producer counters " + std::to_string(k_producer_counter_base) +
                                   "+i as provenance frame_index; independent of the packet PTS");
      add("fixtureGuard", o.guard);
      for (std::size_t i = 0; i < run.packets.size(); ++i) {
        const auto &p = run.packets[i];
        const std::string k = "packet" + std::to_string(i);
        add(k + "Size", std::to_string(p.bytes.size()));
        add(k + "Sha256", p.sha256);
        add(k + "Key", (p.flags & AV_PKT_FLAG_KEY) ? "1" : "0");
        add(k + "Pts", std::to_string(p.pts));
        add(k + "Dts", std::to_string(p.dts));
        add(k + "NalTypes", join(nal_types(p.bytes, c.video_format == 1)));
      }
      for (std::size_t i = 0; i < o.pushes.size(); ++i) {
        const auto &p = o.pushes[i];
        const std::string k = "push" + std::to_string(i);
        add(k + "Packet", std::to_string(p.packet));
        add(k + "ArrivalOffsetUs", std::to_string(p.arrival_us));
        add(k + "TapOffsetUs", optional_text(p.tap_us));
        add(k + "FrameIndex", std::to_string(p.frame_index));
        add(k + "SourceClockTimestamp", "0");
        add(k + "SourceTimePaced", "0");
        add(k + "Expected", "admitted");
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
        add(k + "AtOffsetUs", std::to_string(r.at_us));
        add(k + "Packets", std::to_string(r.stats.packets));
        add(k + "Bytes", std::to_string(r.stats.bytes));
        add(k + "Trimmed", std::to_string(r.stats.trimmed));
        add(k + "Keyframes", std::to_string(r.stats.run_keyframes));
        add(k + "RunSpanUs", us_text(r.stats.run_span));
        add(k + "FrontAgeUs", us_text(r.stats.run_front_age));
        add(k + "LastAcceptedKeyframeAgeUs", age_text(r.stats.last_accepted_keyframe_age));
        add(k + "LastWindowLimit", limit_name(r.stats.last_window_limit));
        add(k + "AwaitingKeyframe", r.stats.awaiting_keyframe ? "1" : "0");
        add(k + "RefusedAwaitingKeyframe", std::to_string(r.stats.refused_awaiting_keyframe));
        add(k + "Discontinuities", std::to_string(r.stats.discontinuities));
        add(k + "SaveInProgress", r.stats.save_in_progress ? "1" : "0");
        add(k + "SnapshotPackets", std::to_string(r.stats.snapshot_packets));
        add(k + "SnapshotKeyframes", std::to_string(r.stats.snapshot_keyframes));
        add(k + "SnapshotSpanUs", us_text(r.stats.snapshot_span));
        add(k + "SavesCompleted", std::to_string(r.stats.saves_completed));
        add(k + "MaxBytes", std::to_string(r.stats.max_bytes));
        add(k + "MaxPackets", std::to_string(r.stats.max_packets));
        add(k + "RunVideoFormat", std::to_string(r.stats.run_video_format));
        add(k + "Check", r.check);
      }
      add("retainedEntries", std::to_string(o.retained.size()));
      add_record_properties(properties, "retained", o.retained);
      add("retainedCheck", retained_check);
      add("heldEntries", std::to_string(o.held.size()));
      add("heldPayloadCount", std::to_string(o.held_payload_count));
      add("heldProvenanceCount", std::to_string(o.held_provenance_count));
      add("heldSequenceNote", "held sequences are the snapshot API sentinel 0; heldEqualsRetained compares copied payload and provenance fields only");
      add_record_properties(properties, "held", o.held);
      add("heldCheck", held_check);
      add("retainedAfterSaveEntries", std::to_string(o.retained_after_save.size()));
      add("retainedAfterSaveSequences", join(o.retained_after_save));
      add("retainedAfterSaveIdenticalToRetained", o.retained_after_identical ? "1" : "0");
      const auto &d = o.derived;
      add("derivedAnchorTapOffsetUs", optional_text(d.anchor_us));
      add("derivedTapOffsetsUs", join_optional(d.tap_offsets_us));
      add("derivedTapSpanUs", optional_text(d.tap_span_us));
      add("derivedTapStrictlyIncreasing", d.strictly_increasing ? "1" : "0");
      add("derivedFirstTapViolation", d.first_violation ? std::to_string(*d.first_violation) : "null");
      add("derivedUntimed", join(d.untimed));
      add("derivedRecomputedMarks", join_marks(d.recomputed_marks));
      add("derivedRecomputedMarksEqualObserved", d.recomputed_equal_observed ? "1" : "0");
      add("derivedHeldEqualsRetained", d.held_equals_retained ? "1" : "0");
      add("derivedArrivalOffsetsUs", join(d.arrival_offsets_us));
      add("derivedArrivalSpanUs", optional_text(d.arrival_span_us));
      add("derivedArrivalSpanIsNotTapSpan", d.arrival_span_is_not_tap_span ? "1" : "0");
      add("derivedTapToArrivalUs", join_optional(d.tap_to_arrival_us));
      add("derivedCheck", derived_check);
      std::vector<provenance_marks_t> expected_marks(s.marks.begin(), s.marks.end());
      add("expectedTapOffsetsUs", join_optional(s.tap_offsets_us));
      add("expectedTapSpanUs", std::to_string(s.tap_span_us));
      add("expectedArrivalSpanUs", std::to_string(arrival_us(7) - arrival_us(0)));
      add("expectedMarks", join_marks(expected_marks));
      add("admissionAdmitted", o.admission.admitted ? "1" : "0");
      add("admissionStatus", status_name(o.admission.status));
      add("admissionPackets", std::to_string(o.admission.packets));
      add("admissionSpanUs", us_text(o.admission.span));
      add("saveAtOffsetUs", std::to_string(k_read_at_us));
      add("saveStatus", status_name(o.result.status));
      add("saveAdmitted", o.result.admitted ? "1" : "0");
      add("saveError", o.result.error);
      add("saveInOwnedDirectory", in_owned_dir ? "1" : "0");
      add("savePackets", std::to_string(o.result.packets));
      add("saveBytes", std::to_string(o.result.bytes));
      add("saveKeyframes", std::to_string(o.result.keyframes));
      add("saveSpanUs", us_text(o.result.span));
      add("saveVideoFormat", std::to_string(o.result.video_format));
      add("saveGeneration", std::to_string(o.result.generation));
      add("saveCheck", save_check);
      add("filesInOutputDirectory", std::to_string(o.files_in_output));
      add("expectedBytes", std::to_string(o.expected.size()));
      add("expectedSha256", o.expected.empty() ? "" : recorder_fixture::sha256_hex(o.expected.data(), o.expected.size()));
      add("fileBytes", std::to_string(o.file.size()));
      add("fileSha256", saved ? recorder_fixture::sha256_hex(o.file.data(), o.file.size()) : "");
      add("fileEqualsPackets0To7", file_equal);
      add("fileFirstAccessUnitNalTypes", join(o.first_types));
      add("fileParameterSetsThenIdr", parameter_sets_then_idr(o.first_types, c.video_format == 1) ? "1" : "0");
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
        EXPECT_EQ(r.check, "ok") << "read " << r.label << " at t0 + " << r.at_us << " us";
      }
      EXPECT_EQ(retained_check, "ok") << "the live provenance read before the save";
      EXPECT_EQ(held_check, "ok") << "the held snapshot provenance of the admitted save";
      EXPECT_EQ(derived_check, "ok") << "the values derived from the observed held records";
      EXPECT_TRUE(o.retained_after_identical) << "the save must leave the live run and its provenance unchanged";
      EXPECT_EQ(save_check, "ok") << status_name(o.result.status) << " " << o.result.error;
      ASSERT_TRUE(saved) << o.result.error;
      EXPECT_TRUE(in_owned_dir) << o.result.path;
      EXPECT_EQ(o.files_in_output, 1u);
      EXPECT_EQ(file_equal, "identical") << "the save must be packets 0..7, in order, byte for byte";
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

TEST_F(RecorderBitstreamTimingTests, H264SteadyTapTimesAreKeptLiveAndHeldAndArrivalSpanIsNotTapSpan) {
  check(k_h264, steady_scenario());
}

TEST_F(RecorderBitstreamTimingTests, HevcSteadyTapTimesAreKeptLiveAndHeldAndArrivalSpanIsNotTapSpan) {
  check(k_hevc, steady_scenario());
}

TEST_F(RecorderBitstreamTimingTests, H264MissingDuplicateAndRegressedTapTimesAreMarkedKeptAndSavedUnchanged) {
  check(k_h264, marks_scenario());
}

TEST_F(RecorderBitstreamTimingTests, HevcMissingDuplicateAndRegressedTapTimesAreMarkedKeptAndSavedUnchanged) {
  check(k_hevc, marks_scenario());
}
