/**
 * @file tests/unit/test_recorder_bitstream_window.cpp
 * @brief Real libx264/libx265 fixture packets through the replay buffer's cap trimming and keyframe
 * realignment (#371 under #329).
 *
 * Every case runs the bounded CPU encoder fixture (#363's support, unchanged) and requires its
 * frozen baseline: 8 packets, key flags exactly at 0 and 4. It then sets test-only replay caps,
 * pushes the 8 packets in order for one eligible session at a controlled arrival clock, and saves
 * into a directory the fixture created:
 *
 * - Entry cap 6: room for packet 6 removes packet 0, and realignment removes 1..3. The save must be
 *   exactly packets 4..7, starting with an access unit that carries its parameter sets before an
 *   IDR slice, and the recorded cause is entry_cap.
 * - Payload cap (total - 1): room for packet 7 removes packet 0, and realignment removes 1..3. The
 *   same save, with payload_cap as the cause.
 * - Payload cap at the largest non-key packet, below both keys: both keys are refused as oversized,
 *   the six dependent packets wait for a keyframe, and nothing is saved.
 *
 * Each case isolates one cause; neither cap precedence nor duration expiry is claimed. Assertions
 * are in-run byte and counter properties only. No packet hash is frozen here: whether these bytes
 * equal earlier runs, or anything a decoder showed about them, is a separate readback's question.
 * The saved file is raw elementary video, not a container or a playable-file claim.
 *
 * Evidence: CTest sets POLARIS_RECORDER_WINDOW_EVIDENCE_DIR. Each case creates its own directory
 * there exclusively and writes the 8 source packets, the published save (if any) and a manifest,
 * before any assertion, so a failed case stays diagnosable. A missing, relative, non-directory or
 * symlinked root, an existing case directory or any write or reread error fails the case; there is
 * no fallback and no skip.
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

  constexpr auto k_marker = "POLARIS_RECORDER_WINDOW_JSON";
  constexpr auto k_evidence_env = "POLARIS_RECORDER_WINDOW_EVIDENCE_DIR";
  constexpr std::uint64_t k_session = 371;
  constexpr auto k_stamp = "20261010_191100";
  /// Explicit synthetic producer counters for provenance: deliberately not the packet PTS.
  constexpr std::int64_t k_producer_counter_base = 37100;
  /// The controlled arrival clock: packets 16 ms apart from t0. Not a presentation time.
  constexpr auto k_arrival_step = 16ms;
  constexpr std::size_t k_large_bytes = std::size_t {1} << 20;
  constexpr std::size_t k_large_packets = 1000;
  constexpr auto k_scope = "real fixture packets through test-only replay caps; in-run byte and counter properties only; "
                           "no cap precedence, duration, decode, playback, container or memory-safety claim";

  struct codec_case_t {
    const recorder_fixture::encoder_spec_t *spec;
    int video_format;  ///< The recorder's codec number: 0=H.264, 1=HEVC
    const char *label;
    const char *tag;
  };

  const codec_case_t k_h264 {&recorder_fixture::k_libx264, 0, "H.264", "h264"};
  const codec_case_t k_hevc {&recorder_fixture::k_libx265, 1, "HEVC", "hevc"};

  enum class cap_e {
    entry,      ///< max packets 6: the trimmed save is packets 4..7
    payload,    ///< max bytes = total - 1: the trimmed save is packets 4..7
    oversized,  ///< max bytes = largest non-key packet: both keys refused, nothing saved
  };

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

  std::string two_digits(std::size_t i) {
    return (i < 10 ? "0" : "") + std::to_string(i);
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

  std::string join(const std::vector<int> &values) {
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

  std::string write_exclusive(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    return write_exclusive(path, bytes.data(), bytes.size());
  }

  std::string write_exclusive(const fs::path &path, const std::string &text) {
    return write_exclusive(path, reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
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

  /// One push: what the case expects of it and what the counters showed after it.
  struct push_t {
    std::size_t index = 0;
    std::string expected;
    std::string observed = "not run";
  };

  /// Everything one case observed, kept for evidence and properties before any assertion.
  struct observed_t {
    std::string encode_result;
    std::string baseline;
    std::string guard = "not run";  ///< The fixture assumptions the expectations rely on
    std::size_t max_bytes = 0;
    std::size_t max_packets = 0;
    std::string cap_derivation;
    std::vector<push_t> pushes;
    stream_recorder::replay_stats_t stats;
    std::vector<stream_recorder::detail::provenance_entry_t> retained;
    stream_recorder::detail::save_result_t result;
    std::vector<std::uint8_t> file;
    std::vector<std::uint8_t> expected;  ///< Packets 4..7, or nothing
    std::vector<int> first_types;        ///< NAL types of the saved file's first access unit
    std::string evidence = "not written";
  };

  /**
   * One eligible session with a known codec in replay mode, saving into a `recordings` directory
   * inside a parent this fixture created exclusively. TearDown restores the recorder and the
   * configuration and removes only that parent; the retained evidence is never touched.
   */
  class RecorderBitstreamWindowTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_config = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      case_name = info->name();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-bitstream-window-"} + case_name + "-" +
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
      config::recording.replay_buffer_minutes = 1;
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

    /// The caps for `cap`, from this run's own packet sizes, after checking what they rely on.
    static std::string choose_caps(cap_e cap, const recorder_fixture::run_t &run, observed_t &o) {
      std::size_t total = 0;
      std::size_t largest_dependent = 0;
      std::size_t smallest_key = SIZE_MAX;
      for (std::size_t i = 0; i < run.packets.size(); ++i) {
        const auto &p = run.packets[i];
        const bool key = (p.flags & AV_PKT_FLAG_KEY) != 0;
        if (p.bytes.empty() || key != (i == 0 || i == 4)) {
          return "packet " + std::to_string(i) + " is empty or its key flag is not at the expected position";
        }
        total += p.bytes.size();
        if (key) {
          smallest_key = std::min(smallest_key, p.bytes.size());
        } else {
          largest_dependent = std::max(largest_dependent, p.bytes.size());
        }
      }
      if (run.packets.size() != static_cast<std::size_t>(recorder_fixture::k_frames) || total >= k_large_bytes) {
        return "expected 8 packets below " + std::to_string(k_large_bytes) + " bytes in total";
      }
      switch (cap) {
        case cap_e::entry:
          o.max_bytes = k_large_bytes;
          o.max_packets = 6;
          o.cap_derivation = "maxPackets=6; maxBytes=1MiB above the run's total";
          break;
        case cap_e::payload:
          o.max_bytes = total - 1;
          o.max_packets = k_large_packets;
          o.cap_derivation = "maxBytes=total-1 (" + std::to_string(total) + "-1); maxPackets=1000";
          break;
        case cap_e::oversized:
          if (largest_dependent == 0 || largest_dependent >= smallest_key) {
            return "the largest dependent packet (" + std::to_string(largest_dependent) +
                   ") is not below the smallest key (" + std::to_string(smallest_key) + ")";
          }
          o.max_bytes = largest_dependent;
          o.max_packets = k_large_packets;
          o.cap_derivation = "maxBytes=largest dependent packet (" + std::to_string(largest_dependent) +
                             ") below the smallest key (" + std::to_string(smallest_key) + "); maxPackets=1000";
          break;
      }
      return {};
    }

    /// Push the run's packets one by one, recording each push's outcome from the counter deltas.
    void push_all(const codec_case_t &c, cap_e cap, const recorder_fixture::run_t &run, observed_t &o) {
      stream_recorder::session_started(k_session, true, c.video_format);
      stream_recorder::detail::set_replay_limits_for_tests(o.max_bytes, o.max_packets);
      auto before = stream_recorder::detail::replay_stats_at(t0);
      for (std::size_t i = 0; i < run.packets.size(); ++i) {
        const auto &p = run.packets[i];
        push_t push;
        push.index = i;
        push.expected = cap != cap_e::oversized ? "admitted" : (i == 0 || i == 4 ? "refused-oversized" : "refused-awaiting-keyframe");
        stream_recorder::packet_provenance_t provenance;
        provenance.frame_index = k_producer_counter_base + static_cast<std::int64_t>(i);
        const auto at = t0 + k_arrival_step * static_cast<int>(i);
        stream_recorder::detail::push_video_at(p.bytes.data(), p.bytes.size(), (p.flags & AV_PKT_FLAG_KEY) != 0,
                                               k_session, c.video_format, provenance, at);
        const auto after = stream_recorder::detail::replay_stats_at(at);
        if (after.refused_oversized == before.refused_oversized + 1) {
          push.observed = "refused-oversized";
        } else if (after.refused_awaiting_keyframe == before.refused_awaiting_keyframe + 1) {
          push.observed = "refused-awaiting-keyframe";
        } else if (after.refused_unselected != before.refused_unselected || after.refused_invalid != before.refused_invalid) {
          push.observed = "refused-other";
        } else {
          push.observed = "admitted";
        }
        before = after;
        o.pushes.push_back(push);
      }
    }

    std::string write_evidence(const codec_case_t &c, const recorder_fixture::run_t &run, const observed_t &o) const {
      fs::path root;
      if (auto error = evidence_root(root); !error.empty()) {
        return error;
      }
      const fs::path dir = root / case_name;
      std::error_code ec;
      if (!fs::create_directory(dir, ec)) {
        return "the case directory already exists or cannot be created: " + dir.string();
      }
      std::string manifest = "{\"marker\":" + recorder_fixture::json_string(k_marker) +
                             ",\"case\":" + recorder_fixture::json_string(case_name) +
                             ",\"codec\":" + recorder_fixture::json_string(c.tag) + ",\"schemaVersion\":1" +
                             ",\"caps\":{\"maxBytes\":" + std::to_string(o.max_bytes) +
                             ",\"maxPackets\":" + std::to_string(o.max_packets) +
                             ",\"derivation\":" + recorder_fixture::json_string(o.cap_derivation) + "},\"sources\":[";
      for (std::size_t i = 0; i < run.packets.size() && i < static_cast<std::size_t>(recorder_fixture::k_frames); ++i) {
        const auto &p = run.packets[i];
        const std::string name = std::string {"source-"} + c.tag + "-" + two_digits(i) + ".bin";
        if (auto error = write_exclusive(dir / name, p.bytes); !error.empty()) {
          return error;
        }
        manifest += std::string {i ? "," : ""} + "{\"file\":" + recorder_fixture::json_string(name) +
                    ",\"index\":" + std::to_string(i) + ",\"key\":" + ((p.flags & AV_PKT_FLAG_KEY) ? "true" : "false") +
                    ",\"size\":" + std::to_string(p.bytes.size()) + ",\"sha256\":" + recorder_fixture::json_string(p.sha256) +
                    ",\"nalTypes\":" + recorder_fixture::json_string(join(nal_types(p.bytes, c.video_format == 1))) + "}";
      }
      manifest += "],\"pushes\":[";
      for (std::size_t i = 0; i < o.pushes.size(); ++i) {
        const auto &p = o.pushes[i];
        manifest += std::string {i ? "," : ""} + "{\"index\":" + std::to_string(p.index) +
                    ",\"source\":" + recorder_fixture::json_string(std::string {c.tag} + ":" + std::to_string(p.index)) +
                    ",\"arrivalOffsetMs\":" + std::to_string(k_arrival_step.count() * static_cast<long long>(p.index)) +
                    ",\"expected\":" + recorder_fixture::json_string(p.expected) +
                    ",\"observed\":" + recorder_fixture::json_string(p.observed) + "}";
      }
      const bool saved = o.result.status == stream_recorder::detail::save_status_e::saved;
      manifest += std::string {"],\"expectedSuffix\":"} + (o.expected.empty() ? "[]" : "[4,5,6,7]") +
                  ",\"stats\":{\"liveEntries\":" + std::to_string(o.stats.packets) +
                  ",\"lastWindowLimit\":" + recorder_fixture::json_string(limit_name(o.stats.last_window_limit)) +
                  ",\"refusedOversized\":" + std::to_string(o.stats.refused_oversized) +
                  ",\"refusedAwaitingKeyframe\":" + std::to_string(o.stats.refused_awaiting_keyframe) +
                  ",\"trimmed\":" + std::to_string(o.stats.trimmed) + "},\"saveStatus\":" +
                  recorder_fixture::json_string(saved ? "saved" : std::to_string(static_cast<int>(o.result.status))) + ",\"saved\":";
      if (saved) {
        if (auto error = write_exclusive(dir / "saved.bin", o.file); !error.empty()) {
          return error;
        }
        manifest += "{\"file\":\"saved.bin\",\"size\":" + std::to_string(o.file.size()) +
                    ",\"sha256\":" + recorder_fixture::json_string(recorder_fixture::sha256_hex(o.file.data(), o.file.size())) + "}";
      } else {
        manifest += "null";
      }
      manifest += ",\"fixture\":{\"encoder\":" + recorder_fixture::json_string(c.spec->name) +
                  ",\"params\":" + recorder_fixture::json_string(c.spec->params_value) +
                  ",\"ffmpegVersionInfo\":" + recorder_fixture::json_string(av_version_info()) +
                  ",\"libavcodecIdent\":" + recorder_fixture::json_string(LIBAVCODEC_IDENT) + "}" +
                  ",\"scope\":" + recorder_fixture::json_string(k_scope) + "}\n";
      return write_exclusive(dir / "manifest.json", manifest);
    }

    void check(const codec_case_t &c, cap_e cap) {
      recorder_fixture::run_t run;
      observed_t o;
      o.encode_result = recorder_fixture::encode_run(*c.spec, run);
      o.baseline = o.encode_result.empty() ? recorder_fixture::validate_baseline(run) : std::string {};
      const bool encoded = o.encode_result.empty() && o.baseline.empty();
      if (encoded) {
        o.guard = choose_caps(cap, run, o);
        if (o.guard.empty()) {
          o.guard = "ok";
        }
      }
      const bool ready = encoded && o.guard == "ok";
      if (ready) {
        push_all(c, cap, run, o);
        o.stats = stream_recorder::detail::replay_stats_at(t0 + 1s);
        o.retained = stream_recorder::detail::replay_provenance_for_tests();
        o.result = stream_recorder::detail::save_replay_result_at(t0 + 1s);
        if (o.result.status == stream_recorder::detail::save_status_e::saved) {
          std::ifstream in(o.result.path, std::ios::binary);
          o.file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (cap != cap_e::oversized) {
          for (std::size_t i = 4; i < run.packets.size(); ++i) {
            o.expected.insert(o.expected.end(), run.packets[i].bytes.begin(), run.packets[i].bytes.end());
          }
          // The saved file's first access unit is as long as packet 4, if the save is what it should be.
          const auto first_size = static_cast<std::ptrdiff_t>(run.packets[4].bytes.size());
          if (o.file.size() >= run.packets[4].bytes.size()) {
            o.first_types = nal_types(std::vector<std::uint8_t>(o.file.begin(), o.file.begin() + first_size), c.video_format == 1);
          }
        }
      }

      // Retained payloads and their provenance: packets 4..7, with the producer counters supplied.
      std::string retained_equal = ready ? "ok" : "not run";
      if (ready) {
        const std::size_t want = cap == cap_e::oversized ? 0 : 4;
        if (o.retained.size() != want) {
          retained_equal = "retained " + std::to_string(o.retained.size()) + " entries";
        }
        for (std::size_t i = 0; retained_equal == "ok" && i < o.retained.size(); ++i) {
          const auto &index = o.retained[i].record.supplied.frame_index;
          if (o.retained[i].payload != run.packets[4 + i].bytes) {
            retained_equal = "payload " + std::to_string(4 + i) + " differs";
          } else if (!index || *index != k_producer_counter_base + static_cast<std::int64_t>(4 + i)) {
            retained_equal = "producer counter " + std::to_string(4 + i) + " differs";
          }
        }
      }
      std::string pushes_as_expected = ready ? "ok" : "not run";
      for (const auto &p : o.pushes) {
        if (pushes_as_expected == "ok" && p.observed != p.expected) {
          pushes_as_expected = "push " + std::to_string(p.index) + " " + p.observed + ", expected " + p.expected;
        }
      }
      const bool saved = o.result.status == stream_recorder::detail::save_status_e::saved;
      const std::string file_equal = !saved ? "not saved" : (o.file == o.expected ? "identical" : "differs");
      const bool in_owned_dir = saved && fs::path(o.result.path).parent_path() == output_dir;
      std::size_t files_in_output = 0;
      {
        std::error_code ec;
        if (fs::is_directory(output_dir, ec)) {
          for (const auto &entry : fs::directory_iterator(output_dir, ec)) {
            (void) entry;
            ++files_in_output;
          }
        }
      }

      // Evidence first, so a failed case below remains diagnosable from its retained bytes.
      o.evidence = write_evidence(c, run, o);

      properties_t properties;
      const auto add = [&properties](std::string key, std::string value) {
        properties.emplace_back(std::move(key), std::move(value));
      };
      add("scope", k_scope);
      add("case", case_name);
      add("ffmpegVersionInfo", av_version_info());
      add("libavcodecIdent", LIBAVCODEC_IDENT);
      recorder_fixture::add_encoder_run(properties, *c.spec, run, o.encode_result, o.baseline);
      add("codec", c.label);
      add("recorderVideoFormat", std::to_string(c.video_format));
      add("sessionGeneration", std::to_string(k_session));
      add("arrivalClock", "controlled steady clock: t0 + i * 16 ms; not a presentation time");
      add("producerCounterNote", "explicit synthetic producer counters " + std::to_string(k_producer_counter_base) +
                                   "+i as provenance frame_index; independent of the packet PTS");
      add("fixtureGuard", o.guard);
      add("capMaxBytes", std::to_string(o.max_bytes));
      add("capMaxPackets", std::to_string(o.max_packets));
      add("capDerivation", o.cap_derivation);
      for (std::size_t i = 0; i < run.packets.size(); ++i) {
        const auto &p = run.packets[i];
        const std::string k = "packet" + std::to_string(i);
        add(k + "Size", std::to_string(p.bytes.size()));
        add(k + "Sha256", p.sha256);
        add(k + "Key", (p.flags & AV_PKT_FLAG_KEY) ? "1" : "0");
        add(k + "NalTypes", join(nal_types(p.bytes, c.video_format == 1)));
      }
      for (const auto &p : o.pushes) {
        add("push" + std::to_string(p.index) + "Expected", p.expected);
        add("push" + std::to_string(p.index) + "Observed", p.observed);
      }
      add("pushesAsExpected", pushes_as_expected);
      add("liveEntries", std::to_string(o.stats.packets));
      add("lastWindowLimit", limit_name(o.stats.last_window_limit));
      add("refusedOversized", std::to_string(o.stats.refused_oversized));
      add("refusedAwaitingKeyframe", std::to_string(o.stats.refused_awaiting_keyframe));
      add("trimmed", std::to_string(o.stats.trimmed));
      add("discontinuities", std::to_string(o.stats.discontinuities));
      add("retainedPayloadsAndCounters", retained_equal);
      add("saveStatus", saved ? "saved" : std::to_string(static_cast<int>(o.result.status)));
      add("saveError", o.result.error);
      add("saveInOwnedDirectory", in_owned_dir ? "1" : "0");
      add("savePackets", std::to_string(o.result.packets));
      add("saveBytes", std::to_string(o.result.bytes));
      add("saveKeyframes", std::to_string(o.result.keyframes));
      add("filesInOutputDirectory", std::to_string(files_in_output));
      add("expectedSuffix", o.expected.empty() ? "" : "4,5,6,7");
      add("expectedBytes", std::to_string(o.expected.size()));
      add("expectedSha256", o.expected.empty() ? "" : recorder_fixture::sha256_hex(o.expected.data(), o.expected.size()));
      add("fileBytes", std::to_string(o.file.size()));
      add("fileSha256", saved ? recorder_fixture::sha256_hex(o.file.data(), o.file.size()) : "");
      add("fileEqualsPackets4To7", file_equal);
      add("fileFirstAccessUnitNalTypes", join(o.first_types));
      add("evidenceDirectory", case_name);
      add("evidenceResult", o.evidence.empty() ? "ok" : o.evidence);
      recorder_fixture::record_properties(k_marker, properties);

      ASSERT_EQ(o.encode_result, "") << "encoder run";
      ASSERT_EQ(o.baseline, "") << "frozen encoder baseline";
      ASSERT_EQ(o.guard, "ok") << "fixture assumption the expected outcome relies on";
      EXPECT_EQ(o.evidence, "") << "raw evidence";
      EXPECT_EQ(pushes_as_expected, "ok");
      EXPECT_EQ(retained_equal, "ok") << "the live run must be packets 4..7 with their producer counters";
      if (cap == cap_e::oversized) {
        EXPECT_EQ(o.stats.refused_oversized, 2u);
        EXPECT_EQ(o.stats.refused_awaiting_keyframe, 6u);
        EXPECT_EQ(o.stats.packets, 0u);
        EXPECT_STREQ(limit_name(o.stats.last_window_limit), "discontinuity");
        EXPECT_TRUE(o.result.status == stream_recorder::detail::save_status_e::empty)
          << "save status " << static_cast<int>(o.result.status) << " " << o.result.error;
        EXPECT_EQ(files_in_output, 0u) << "an empty save creates no file";
        return;
      }
      EXPECT_EQ(o.stats.packets, 4u);
      EXPECT_STREQ(limit_name(o.stats.last_window_limit), cap == cap_e::entry ? "entry_cap" : "payload_cap");
      ASSERT_TRUE(saved) << o.result.error;
      EXPECT_TRUE(in_owned_dir) << o.result.path;
      EXPECT_EQ(files_in_output, 1u);
      EXPECT_EQ(o.result.packets, 4u);
      EXPECT_EQ(o.result.keyframes, 1u);
      EXPECT_EQ(o.result.bytes, o.expected.size());
      EXPECT_EQ(o.result.video_format, c.video_format);
      EXPECT_EQ(o.result.generation, k_session);
      EXPECT_EQ(file_equal, "identical") << "the trimmed save must be packets 4..7, in order, byte for byte";
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

TEST_F(RecorderBitstreamWindowTests, H264EntryCapTrimRealignsToTheNextKey) {
  check(k_h264, cap_e::entry);
}

TEST_F(RecorderBitstreamWindowTests, HevcEntryCapTrimRealignsToTheNextKey) {
  check(k_hevc, cap_e::entry);
}

TEST_F(RecorderBitstreamWindowTests, H264PayloadCapTrimRealignsToTheNextKey) {
  check(k_h264, cap_e::payload);
}

TEST_F(RecorderBitstreamWindowTests, HevcPayloadCapTrimRealignsToTheNextKey) {
  check(k_hevc, cap_e::payload);
}

TEST_F(RecorderBitstreamWindowTests, H264OversizedKeysAreRefusedAndNothingIsSaved) {
  check(k_h264, cap_e::oversized);
}

TEST_F(RecorderBitstreamWindowTests, HevcOversizedKeysAreRefusedAndNothingIsSaved) {
  check(k_hevc, cap_e::oversized);
}
