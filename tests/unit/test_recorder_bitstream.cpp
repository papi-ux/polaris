/**
 * @file tests/unit/test_recorder_bitstream.cpp
 * @brief Positive observations of real libx264/libx265 fixture packets (#365): how FFmpeg's coded
 * bitstream (CBS) reader reads them, and that a raw replay save writes them back byte for byte.
 *
 * Every case first runs the bounded CPU encoder fixture (a test-only copy of #363's) and requires
 * its frozen baseline: 8 packets, key flags at 0 and 4. Then:
 *
 * - The CBS cases read the 8 packets in order through ONE persistent CBS context per codec and run,
 *   so parameter sets read from earlier packets stay available to later ones, and record each
 *   read's return, unit count, unit types and unit byte sizes, whether the reader holds an active
 *   SPS afterwards, the packet's leading bytes, and the length and SHA-256 of an original and a
 *   filler-stripped copy. They assert only that every read succeeded with at least one unit.
 *   Unit sequences, IDR or parameter-set placement, Annex B framing, filler absence and what a
 *   later key frame carries are observations, not assertions, until a run freezes them.
 * - The replay-save cases push the 8 packets, in order, into the replay buffer of one eligible
 *   session whose codec is known, at a controlled arrival clock, with explicit synthetic producer
 *   counters as provenance; then save into a directory the fixture created. They assert that the
 *   encoder packets, the retained replay payloads and the saved file are the same bytes.
 *
 * CBS reading here is coded-bitstream structure reading only. It is not an AVCodecParser, not a
 * decoder and not playback, and the saved file is raw elementary video, not a container or a
 * playable-file claim. cbs::validate_sps answers only whether the active SPS has a VUI, so it is
 * recorded under that name for the first packet and never used as a read-success test. The packet
 * PTS is the fixture's frame counter; the provenance counters below are separate synthetic
 * producer counters. Neither is a product clock or a presentation-time contract. A missing encoder
 * or a failed open, read or save is a failure, never a skip.
 */
#include "recorder_encoder_fixture_support.h"

#include <src/cbs.h>
#include <src/config.h>
#include <src/stream_recorder.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/cbs_h264.h>
#include <libavcodec/cbs_h265.h>
}

namespace {
  namespace fs = std::filesystem;
  using steady = std::chrono::steady_clock;
  using namespace std::chrono_literals;
  using recorder_fixture::properties_t;

  constexpr auto k_marker = "POLARIS_RECORDER_BITSTREAM_JSON";
  constexpr std::uint64_t k_session = 365;
  constexpr auto k_stamp = "20261010_094000";
  /// Explicit synthetic producer counters for provenance: deliberately not the packet PTS.
  constexpr std::int64_t k_producer_counter_base = 36500;
  /// The controlled arrival clock: packets 16 ms apart from t0. Not a presentation time.
  constexpr auto k_arrival_step = 16ms;

  struct codec_case_t {
    const recorder_fixture::encoder_spec_t *spec;
    AVCodecID codec_id;
    int video_format;  ///< The recorder's codec number: 0=H.264, 1=HEVC
    const char *label;
  };

  const codec_case_t k_h264 {&recorder_fixture::k_libx264, AV_CODEC_ID_H264, 0, "H.264"};
  const codec_case_t k_hevc {&recorder_fixture::k_libx265, AV_CODEC_ID_HEVC, 1, "HEVC"};

  // The units the product's own CBS reads decompose (src/cbs.cpp): parameter sets and slice
  // headers, where the reader records the active SPS. Every other unit is split and kept as it
  // came. Using the same policy makes a read here the read the product performs.
  const CodedBitstreamUnitType k_h264_decompose[] = {
    H264_NAL_SLICE,
    H264_NAL_IDR_SLICE,
    H264_NAL_SPS,
    H264_NAL_PPS,
  };
  const CodedBitstreamUnitType k_hevc_decompose[] = {
    HEVC_NAL_TRAIL_N,
    HEVC_NAL_TRAIL_R,
    HEVC_NAL_TSA_N,
    HEVC_NAL_TSA_R,
    HEVC_NAL_STSA_N,
    HEVC_NAL_STSA_R,
    HEVC_NAL_RADL_N,
    HEVC_NAL_RADL_R,
    HEVC_NAL_RASL_N,
    HEVC_NAL_RASL_R,
    HEVC_NAL_BLA_W_LP,
    HEVC_NAL_BLA_W_RADL,
    HEVC_NAL_BLA_N_LP,
    HEVC_NAL_IDR_W_RADL,
    HEVC_NAL_IDR_N_LP,
    HEVC_NAL_CRA_NUT,
    HEVC_NAL_VPS,
    HEVC_NAL_SPS,
    HEVC_NAL_PPS,
  };

  struct cbs_context_deleter {
    void operator()(CodedBitstreamContext *c) const {
      ff_cbs_close(&c);
    }
  };

  /// A zeroed fragment, freed once at the end.
  struct fragment_t {
    CodedBitstreamFragment frag {};

    ~fragment_t() {
      ff_cbs_fragment_free(&frag);
    }
  };

  /// What one ordered packet read through the persistent context observed.
  struct packet_read_t {
    int read_return = 1;  ///< 1 until read
    int unit_count = 0;
    std::string unit_types;  ///< Comma-separated unit types, in order
    std::string unit_bytes;  ///< Comma-separated unit data sizes, in order
    bool active_sps = false;  ///< The reader held an active SPS after this read
    std::string leading;  ///< "4-byte start code", "3-byte start code" or "other"
    std::size_t stripped_size = 0;
    std::string stripped_sha256;
  };

  std::string leading_bytes(const std::vector<std::uint8_t> &b) {
    if (b.size() >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) {
      return "4-byte start code";
    }
    if (b.size() >= 3 && b[0] == 0 && b[1] == 0 && b[2] == 1) {
      return "3-byte start code";
    }
    return "other";
  }

  bool reader_active_sps(const CodedBitstreamContext *ctx, AVCodecID codec_id) {
    if (codec_id == AV_CODEC_ID_H264) {
      return static_cast<const CodedBitstreamH264Context *>(ctx->priv_data)->active_sps != nullptr;
    }
    return static_cast<const CodedBitstreamH265Context *>(ctx->priv_data)->active_sps != nullptr;
  }

  /// A refcounted, padded packet holding `bytes`, with the encoder's flags; null on failure.
  std::unique_ptr<AVPacket, recorder_fixture::packet_deleter> packet_of(const recorder_fixture::packet_record_t &p) {
    std::unique_ptr<AVPacket, recorder_fixture::packet_deleter> packet {av_packet_alloc()};
    if (!packet || av_new_packet(packet.get(), p.size) != 0) {
      return {};
    }
    std::memcpy(packet->data, p.bytes.data(), p.bytes.size());
    packet->flags = p.flags;
    packet->pts = p.pts;
    packet->dts = p.dts;
    return packet;
  }

  /// Read every packet, in order, through one persistent CBS context. Returns an empty string when
  /// every read succeeded with at least one unit, else the first reason. It asserts nothing.
  std::string read_stream(const codec_case_t &c, const recorder_fixture::run_t &run, std::vector<packet_read_t> &reads,
                          int &init_return, std::string &first_packet_vui) {
    CodedBitstreamContext *raw = nullptr;
    init_return = ff_cbs_init(&raw, c.codec_id, nullptr);
    std::unique_ptr<CodedBitstreamContext, cbs_context_deleter> ctx {raw};
    if (init_return < 0 || !ctx) {
      return "ff_cbs_init returned " + std::to_string(init_return);
    }
    if (c.codec_id == AV_CODEC_ID_H264) {
      ctx->decompose_unit_types = k_h264_decompose;
      ctx->nb_decompose_unit_types = static_cast<int>(std::size(k_h264_decompose));
    } else {
      ctx->decompose_unit_types = k_hevc_decompose;
      ctx->nb_decompose_unit_types = static_cast<int>(std::size(k_hevc_decompose));
    }

    fragment_t fragment;
    std::string reason;
    for (std::size_t i = 0; i < run.packets.size(); ++i) {
      const auto &p = run.packets[i];
      packet_read_t read;
      read.leading = leading_bytes(p.bytes);

      // The product's filler strip runs in place; observe it on a copy so the original stays intact.
      std::vector<std::uint8_t> copy = p.bytes;
      read.stripped_size = cbs::strip_filler_data(copy.data(), copy.size(), c.codec_id);
      read.stripped_sha256 = read.stripped_size <= copy.size() ? recorder_fixture::sha256_hex(copy.data(), read.stripped_size) : "";

      const auto packet = packet_of(p);
      if (!packet) {
        reads.push_back(read);
        return reason.empty() ? "packet allocation failed at " + std::to_string(i) : reason;
      }
      if (i == 0) {
        // A separate, fresh reader inside the product's helper: active SPS VUI presence only.
        first_packet_vui = cbs::validate_sps(packet.get(), c.codec_id) ? "1" : "0";
      }

      ff_cbs_fragment_reset(&fragment.frag);
      read.read_return = ff_cbs_read_packet(ctx.get(), &fragment.frag, packet.get());
      read.unit_count = fragment.frag.nb_units;
      for (int u = 0; u < fragment.frag.nb_units; ++u) {
        read.unit_types += (u ? "," : "") + std::to_string(fragment.frag.units[u].type);
        read.unit_bytes += (u ? "," : "") + std::to_string(fragment.frag.units[u].data_size);
      }
      read.active_sps = read.read_return >= 0 && reader_active_sps(ctx.get(), c.codec_id);
      reads.push_back(read);

      if (reason.empty() && read.read_return < 0) {
        reason = "ff_cbs_read_packet returned " + std::to_string(read.read_return) + " at packet " + std::to_string(i);
      } else if (reason.empty() && read.unit_count < 1) {
        reason = "no units read at packet " + std::to_string(i);
      }
    }
    return reason;
  }

  void add_common(properties_t &properties, const char *scope) {
    properties.emplace_back("scope", scope);
    properties.emplace_back("processUid", std::to_string(::getuid()));
    properties.emplace_back("ptsNote", "packet PTS is the fixture's synthetic frame counter; not producer provenance, a presentation time or a product clock");
  }

  void add_packets(properties_t &properties, const recorder_fixture::run_t &run) {
    for (std::size_t i = 0; i < run.packets.size(); ++i) {
      const auto &p = run.packets[i];
      const std::string k = "packet" + std::to_string(i);
      properties.emplace_back(k + "Size", std::to_string(p.size));
      properties.emplace_back(k + "Key", (p.flags & AV_PKT_FLAG_KEY) ? "1" : "0");
      properties.emplace_back(k + "SyntheticPts", std::to_string(p.pts));
      properties.emplace_back(k + "Sha256", p.sha256);
    }
  }

  void check_cbs_stream(const codec_case_t &c) {
    recorder_fixture::run_t run;
    const std::string encode_result = recorder_fixture::encode_run(*c.spec, run);
    const std::string baseline = encode_result.empty() ? recorder_fixture::validate_baseline(run) : std::string {};

    std::vector<packet_read_t> reads;
    int init_return = 1;
    std::string first_packet_vui = "not reached";
    std::string read_result = "not run";
    if (encode_result.empty() && baseline.empty()) {
      read_result = read_stream(c, run, reads, init_return, first_packet_vui);
      if (read_result.empty()) {
        read_result = "ok";
      }
    }

    properties_t properties;
    add_common(properties, "CBS coded-bitstream reads of real fixture packets through one persistent context; not a parser, decoder or playback");
    recorder_fixture::add_encoder_run(properties, *c.spec, run, encode_result, baseline);
    properties.emplace_back("codec", c.label);
    properties.emplace_back("cbsContext", "one persistent context for all packets of this run, in order; the fragment is reset between packets");
    properties.emplace_back("cbsDecompose", "parameter sets and slice headers, as src/cbs.cpp; other units split and kept as read");
    properties.emplace_back("cbsInitReturn", init_return == 1 ? "not reached" : std::to_string(init_return));
    properties.emplace_back("cbsReadResult", read_result);
    properties.emplace_back("productValidateSpsActiveVuiPresentPacket0", first_packet_vui);
    properties.emplace_back("fillerNote", "strip_filler_data ran on a copy; original and stripped sizes and hashes are observations");
    add_packets(properties, run);
    for (std::size_t i = 0; i < reads.size(); ++i) {
      const auto &r = reads[i];
      const std::string k = "packet" + std::to_string(i);
      properties.emplace_back(k + "Leading", r.leading);
      properties.emplace_back(k + "CbsReadReturn", r.read_return == 1 ? "not reached" : std::to_string(r.read_return));
      properties.emplace_back(k + "CbsUnitCount", std::to_string(r.unit_count));
      properties.emplace_back(k + "CbsUnitTypes", r.unit_types);
      properties.emplace_back(k + "CbsUnitBytes", r.unit_bytes);
      properties.emplace_back(k + "CbsActiveSpsAfterRead", r.active_sps ? "1" : "0");
      properties.emplace_back(k + "FillerStrippedSize", std::to_string(r.stripped_size));
      properties.emplace_back(k + "FillerStrippedSha256", r.stripped_sha256);
    }
    recorder_fixture::record_properties(k_marker, properties);

    ASSERT_EQ(encode_result, "") << "encoder run";
    ASSERT_EQ(baseline, "") << "frozen encoder baseline";
    EXPECT_EQ(read_result, "ok") << "every ordered packet must read through the persistent CBS context";
    EXPECT_EQ(reads.size(), run.packets.size());
  }

  /**
   * One eligible session with a known codec in replay mode, saving into a `recordings` directory
   * inside a parent this fixture created exclusively. TearDown restores the recorder and the
   * configuration and removes only that parent.
   */
  class RecorderBitstreamTests: public ::testing::Test {
  protected:
    void SetUp() override {
      saved_config = config::recording;
      stream_recorder::detail::reset_replay_for_tests();
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      const auto base = fs::temp_directory_path();
      for (int attempt = 0; attempt < 100 && owned_parent.empty(); ++attempt) {
        const auto candidate = base / (std::string {"polaris-bitstream-"} + info->name() + "-" +
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

    void check_replay_save(const codec_case_t &c) {
      recorder_fixture::run_t run;
      const std::string encode_result = recorder_fixture::encode_run(*c.spec, run);
      const std::string baseline = encode_result.empty() ? recorder_fixture::validate_baseline(run) : std::string {};
      const bool encoded = encode_result.empty() && baseline.empty();

      std::vector<std::uint8_t> expected;
      std::vector<stream_recorder::detail::provenance_entry_t> retained;
      stream_recorder::detail::save_result_t result;
      std::vector<std::uint8_t> file;
      std::size_t live_packets = 0;
      std::string push_result = "not run";
      if (encoded) {
        stream_recorder::session_started(k_session, true, c.video_format);
        for (std::size_t i = 0; i < run.packets.size(); ++i) {
          const auto &p = run.packets[i];
          stream_recorder::packet_provenance_t provenance;
          provenance.frame_index = k_producer_counter_base + static_cast<std::int64_t>(i);
          stream_recorder::detail::push_video_at(p.bytes.data(), p.bytes.size(), (p.flags & AV_PKT_FLAG_KEY) != 0,
                                                 k_session, c.video_format, provenance, t0 + k_arrival_step * static_cast<int>(i));
          expected.insert(expected.end(), p.bytes.begin(), p.bytes.end());
        }
        push_result = "ok";
        live_packets = stream_recorder::detail::replay_stats_at(t0 + 1s).packets;
        retained = stream_recorder::detail::replay_provenance_for_tests();
        result = stream_recorder::detail::save_replay_result_at(t0 + 1s);
        if (result.status == stream_recorder::detail::save_status_e::saved) {
          std::ifstream in(result.path, std::ios::binary);
          file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
      }

      // Per-packet equality: encoder payload, retained payload and the saved file's slice.
      std::string retained_equal = encoded ? "ok" : "not run";
      std::string counters_equal = encoded ? "ok" : "not run";
      if (encoded) {
        if (retained.size() != run.packets.size()) {
          retained_equal = "retained " + std::to_string(retained.size()) + " entries";
        }
        for (std::size_t i = 0; retained_equal == "ok" && i < retained.size(); ++i) {
          if (retained[i].payload != run.packets[i].bytes) {
            retained_equal = "payload " + std::to_string(i) + " differs";
          }
        }
        for (std::size_t i = 0; counters_equal == "ok" && i < retained.size(); ++i) {
          const auto &index = retained[i].record.supplied.frame_index;
          if (!index || *index != k_producer_counter_base + static_cast<std::int64_t>(i)) {
            counters_equal = "producer counter " + std::to_string(i) + " differs";
          }
        }
      }
      const bool saved = result.status == stream_recorder::detail::save_status_e::saved;
      const std::string file_equal = !saved ? "not saved" : (file == expected ? "identical" : "differs");
      const bool in_owned_dir = saved && fs::path(result.path).parent_path() == output_dir;

      properties_t properties;
      add_common(properties, "raw replay save of real fixture packets read back byte for byte; raw elementary video, no container or playback claim");
      recorder_fixture::add_encoder_run(properties, *c.spec, run, encode_result, baseline);
      properties.emplace_back("codec", c.label);
      properties.emplace_back("recorderVideoFormat", std::to_string(c.video_format));
      properties.emplace_back("sessionGeneration", std::to_string(k_session));
      properties.emplace_back("sessionEligibleKnownCodec", "1");
      properties.emplace_back("replayCaps", "defaults restored by reset_replay_for_tests");
      properties.emplace_back("arrivalClock", "controlled steady clock: t0 + i * 16 ms; not a presentation time");
      properties.emplace_back("producerCounterNote", "explicit synthetic producer counters " + std::to_string(k_producer_counter_base) +
                                                       "+i as provenance frame_index; independent of the packet PTS");
      properties.emplace_back("pushResult", push_result);
      add_packets(properties, run);
      properties.emplace_back("liveReplayPackets", std::to_string(live_packets));
      properties.emplace_back("retainedEntries", std::to_string(retained.size()));
      properties.emplace_back("retainedPayloadsEqualEncoderPackets", retained_equal);
      properties.emplace_back("retainedProducerCountersAsSupplied", counters_equal);
      properties.emplace_back("saveStatus", saved ? "saved" : std::to_string(static_cast<int>(result.status)));
      properties.emplace_back("saveError", result.error);
      properties.emplace_back("saveInOwnedDirectory", in_owned_dir ? "1" : "0");
      properties.emplace_back("savePackets", std::to_string(result.packets));
      properties.emplace_back("saveBytes", std::to_string(result.bytes));
      properties.emplace_back("saveKeyframes", std::to_string(result.keyframes));
      properties.emplace_back("saveVideoFormat", std::to_string(result.video_format));
      properties.emplace_back("expectedBytes", std::to_string(expected.size()));
      properties.emplace_back("expectedSha256", encoded ? recorder_fixture::sha256_hex(expected.data(), expected.size()) : "");
      properties.emplace_back("fileBytes", std::to_string(file.size()));
      properties.emplace_back("fileSha256", saved ? recorder_fixture::sha256_hex(file.data(), file.size()) : "");
      properties.emplace_back("fileEqualsEncoderPackets", file_equal);
      recorder_fixture::record_properties(k_marker, properties);

      ASSERT_EQ(encode_result, "") << "encoder run";
      ASSERT_EQ(baseline, "") << "frozen encoder baseline";
      EXPECT_EQ(live_packets, run.packets.size()) << "every packet is retained under the default caps";
      EXPECT_EQ(retained_equal, "ok") << "retained replay payloads must equal the encoder packets";
      EXPECT_EQ(counters_equal, "ok") << "provenance keeps the synthetic producer counters as supplied";
      ASSERT_TRUE(saved) << result.error;
      EXPECT_TRUE(in_owned_dir) << result.path;
      EXPECT_EQ(result.packets, run.packets.size());
      EXPECT_EQ(result.bytes, expected.size());
      EXPECT_EQ(result.keyframes, 2u);
      EXPECT_EQ(result.video_format, c.video_format);
      EXPECT_EQ(result.generation, k_session);
      EXPECT_EQ(file.size(), expected.size());
      EXPECT_EQ(file_equal, "identical") << "the saved file must be the encoder packets, in order, byte for byte";
    }

    const steady::time_point t0 = steady::time_point {} + 1h;
    config::recording_t saved_config;
    fs::path owned_parent;
    fs::path output_dir;
  };
}  // namespace

TEST_F(RecorderBitstreamTests, H264FixturePacketsReadThroughOnePersistentCbsContext) {
  check_cbs_stream(k_h264);
}

TEST_F(RecorderBitstreamTests, HevcFixturePacketsReadThroughOnePersistentCbsContext) {
  check_cbs_stream(k_hevc);
}

TEST_F(RecorderBitstreamTests, H264RealPacketsReplaySaveByteForByte) {
  check_replay_save(k_h264);
}

TEST_F(RecorderBitstreamTests, HevcRealPacketsReplaySaveByteForByte) {
  check_replay_save(k_hevc);
}
