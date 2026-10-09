/**
 * @file tests/unit/test_recorder_encoder_fixture.cpp
 * @brief Bounded in-process libx264/libx265 CPU encoder fixtures for the recorder's media work (#363).
 *
 * Each case opens one named software encoder from the linked FFmpeg, feeds it a fixed number of
 * deterministic YUV420P frames with explicit single-thread, no-B-frame, fixed closed-GOP and
 * scene-cut-disabled settings, drains it with a bounded send/receive/flush loop, and then repeats
 * the identical run and compares every packet's bytes. Every option-set return code, the open
 * result, leftover options and each packet's size, flags, synthetic PTS/DTS and SHA-256 are
 * recorded as GTest XML properties and in one matching console JSON line.
 *
 * This is an encoder baseline only. Nothing here decodes, parses bitstream structure, touches the
 * recorder, or claims a playable file. The PTS values are the fixture's own frame counter, not
 * producer provenance or a presentation time. A missing encoder or a failed open is a failure
 * (a dependency finding), never a skip.
 */
#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

namespace {
  constexpr int k_width = 64;
  constexpr int k_height = 64;
  constexpr int k_frames = 8;  ///< Fixed input count
  constexpr int k_gop = 4;     ///< keyint 4 with scene cut disabled: a key frame every 4 frames
  /// Upper bound on receive calls per run: one per frame, a few EAGAINs, the flush and EOF.
  constexpr int k_max_receive_calls = 4 * k_frames + 8;

  struct encoder_spec_t {
    const char *name;     ///< The encoder looked up by name
    const char *preset;
    const char *tune;
    const char *params_key;    ///< The encoder's private parameter-string option
    const char *params_value;  ///< Explicit threads, GOP, B-frames and scene cut
  };

  constexpr encoder_spec_t k_libx264 {
    "libx264", "ultrafast", "zerolatency", "x264-params",
    "threads=1:sliced-threads=0:lookahead-threads=1:bframes=0:keyint=4:min-keyint=4:scenecut=0:open-gop=0",
  };
  constexpr encoder_spec_t k_libx265 {
    "libx265", "ultrafast", "zerolatency", "x265-params",
    "pools=none:frame-threads=1:wpp=0:bframes=0:keyint=4:min-keyint=4:scenecut=0:open-gop=0:log-level=error",
  };

  struct context_deleter {
    void operator()(AVCodecContext *c) const {
      avcodec_free_context(&c);
    }
  };

  struct frame_deleter {
    void operator()(AVFrame *f) const {
      av_frame_free(&f);
    }
  };

  struct packet_deleter {
    void operator()(AVPacket *p) const {
      av_packet_free(&p);
    }
  };

  struct dict_deleter {
    void operator()(AVDictionary *d) const {
      av_dict_free(&d);
    }
  };

  struct sha_deleter {
    void operator()(AVSHA *s) const {
      av_free(s);
    }
  };

  struct packet_record_t {
    int size = 0;
    int flags = 0;
    std::int64_t pts = 0;
    std::int64_t dts = 0;
    std::string sha256;
    std::vector<std::uint8_t> bytes;
  };

  struct run_t {
    std::vector<std::pair<std::string, int>> option_returns;  ///< av_dict_set results, in order
    int open_return = 0;
    int leftover_options = -1;
    int receive_calls = 0;
    int flush_return = 0;
    int terminal_receive = 0;  ///< The receive result that ended the drain (AVERROR_EOF)
    std::vector<packet_record_t> packets;
  };

  std::string sha256_hex(const std::uint8_t *data, std::size_t size) {
    std::unique_ptr<AVSHA, sha_deleter> sha {av_sha_alloc()};
    if (!sha || av_sha_init(sha.get(), 256) < 0) {
      return {};
    }
    av_sha_update(sha.get(), data, size);
    std::array<std::uint8_t, 32> digest {};
    av_sha_final(sha.get(), digest.data());
    std::string hex;
    char byte[3];
    for (const auto b : digest) {
      std::snprintf(byte, sizeof byte, "%02x", b);
      hex += byte;
    }
    return hex;
  }

  /// Deterministic planes: each frame differs, each value depends only on frame, plane and position.
  void fill_frame(AVFrame *frame, int index) {
    for (int plane = 0; plane < 3; ++plane) {
      const int w = plane ? k_width / 2 : k_width;
      const int h = plane ? k_height / 2 : k_height;
      for (int y = 0; y < h; ++y) {
        std::uint8_t *row = frame->data[plane] + static_cast<std::ptrdiff_t>(y) * frame->linesize[plane];
        for (int x = 0; x < w; ++x) {
          row[x] = static_cast<std::uint8_t>((x * 3 + y * 5 + index * 17 + plane * 61) & 0xff);
        }
      }
    }
  }

  /// Drain every packet available now; returns the receive result that stopped it.
  int drain(AVCodecContext *ctx, AVPacket *packet, run_t &run) {
    while (run.receive_calls < k_max_receive_calls) {
      ++run.receive_calls;
      const int ret = avcodec_receive_packet(ctx, packet);
      if (ret < 0) {
        return ret;  // AVERROR(EAGAIN), AVERROR_EOF or a real error
      }
      packet_record_t record;
      record.size = packet->size;
      record.flags = packet->flags;
      record.pts = packet->pts;
      record.dts = packet->dts;
      record.bytes.assign(packet->data, packet->data + packet->size);
      record.sha256 = sha256_hex(packet->data, static_cast<std::size_t>(packet->size));
      run.packets.push_back(std::move(record));
      av_packet_unref(packet);
    }
    return AVERROR(ERANGE);  // The bound was reached: reported as a failure by the caller
  }

  /// One complete bounded run of `spec`. Fatal problems are reported through gtest assertions.
  void encode_run(const encoder_spec_t &spec, run_t &run) {
    const AVCodec *codec = avcodec_find_encoder_by_name(spec.name);
    ASSERT_NE(codec, nullptr) << spec.name << " is not registered in the linked FFmpeg (dependency finding)";
    std::unique_ptr<AVCodecContext, context_deleter> ctx {avcodec_alloc_context3(codec)};
    ASSERT_NE(ctx, nullptr) << "context allocation failed";
    ctx->width = k_width;
    ctx->height = k_height;
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->time_base = AVRational {1, 60};
    ctx->framerate = AVRational {60, 1};
    ctx->gop_size = k_gop;
    ctx->max_b_frames = 0;
    ctx->thread_count = 1;

    AVDictionary *raw = nullptr;
    for (const auto &[key, value] : {std::pair {"preset", spec.preset}, std::pair {"tune", spec.tune},
                                     std::pair {spec.params_key, spec.params_value}}) {
      run.option_returns.emplace_back(key, av_dict_set(&raw, key, value, 0));
    }
    std::unique_ptr<AVDictionary, dict_deleter> options {raw};
    for (const auto &[key, ret] : run.option_returns) {
      ASSERT_GE(ret, 0) << "av_dict_set " << key;
    }

    AVDictionary *remaining = options.release();
    run.open_return = avcodec_open2(ctx.get(), codec, &remaining);
    options.reset(remaining);
    run.leftover_options = av_dict_count(options.get());
    char error[AV_ERROR_MAX_STRING_SIZE] {};
    av_strerror(run.open_return, error, sizeof error);
    ASSERT_EQ(run.open_return, 0) << spec.name << " failed to open: " << error << " (dependency finding)";
    ASSERT_EQ(run.leftover_options, 0) << "an option was not consumed by " << spec.name;

    std::unique_ptr<AVFrame, frame_deleter> frame {av_frame_alloc()};
    std::unique_ptr<AVPacket, packet_deleter> packet {av_packet_alloc()};
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(packet, nullptr);
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = k_width;
    frame->height = k_height;
    ASSERT_EQ(av_frame_get_buffer(frame.get(), 0), 0);

    for (int i = 0; i < k_frames; ++i) {
      ASSERT_EQ(av_frame_make_writable(frame.get()), 0);
      fill_frame(frame.get(), i);
      frame->pts = i;  // Synthetic fixture counter, not producer provenance or presentation time
      int sent = avcodec_send_frame(ctx.get(), frame.get());
      if (sent == AVERROR(EAGAIN)) {
        // The encoder wants output read first: drain once, then the same frame must be accepted.
        const int drained = drain(ctx.get(), packet.get(), run);
        ASSERT_EQ(drained, AVERROR(EAGAIN)) << "unexpected receive result while making room";
        sent = avcodec_send_frame(ctx.get(), frame.get());
      }
      ASSERT_EQ(sent, 0) << "send_frame " << i;
      const int drained = drain(ctx.get(), packet.get(), run);
      ASSERT_EQ(drained, AVERROR(EAGAIN)) << "receive after frame " << i << " ended with " << drained;
    }
    run.flush_return = avcodec_send_frame(ctx.get(), nullptr);
    ASSERT_EQ(run.flush_return, 0) << "flush";
    run.terminal_receive = drain(ctx.get(), packet.get(), run);
    ASSERT_EQ(run.terminal_receive, AVERROR_EOF) << "the drain must end at EOF within the bound";
  }

  std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
      if (c == '"' || c == '\\') {
        out += '\\';
        out += static_cast<char>(c);
      } else if (c < 0x20) {
        char escaped[8];
        std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
        out += escaped;
      } else {
        out += static_cast<char>(c);
      }
    }
    return out + "\"";
  }

  /// Record everything observed, identically as XML properties and one console JSON line. Called
  /// before any positive assertion, so a failing run still leaves its evidence.
  void record_evidence(const encoder_spec_t &spec, const run_t &first, const run_t *repeat, bool repeat_identical) {
    std::vector<std::pair<std::string, std::string>> properties;
    const auto add = [&properties](std::string key, std::string value) {
      properties.emplace_back(std::move(key), std::move(value));
    };
    add("scope", "in-process CPU encoder baseline; no decode, structure parse, recorder or playback claim");
    add("ptsNote", "fixture-assigned synthetic frame counter; not producer provenance or presentation time");
    add("encoder", spec.name);
    add("optionPreset", spec.preset);
    add("optionTune", spec.tune);
    add("optionParamsKey", spec.params_key);
    add("optionParams", spec.params_value);
    add("contextGopSize", std::to_string(k_gop));
    add("contextMaxBFrames", "0");
    add("contextThreadCount", "1");
    add("inputFrames", std::to_string(k_frames));
    add("inputSize", std::to_string(k_width) + "x" + std::to_string(k_height) + " yuv420p");
    for (std::size_t i = 0; i < first.option_returns.size(); ++i) {
      add("optionSetReturn" + std::to_string(i), first.option_returns[i].first + "=" + std::to_string(first.option_returns[i].second));
    }
    add("openReturn", std::to_string(first.open_return));
    add("leftoverOptions", std::to_string(first.leftover_options));
    add("flushReturn", std::to_string(first.flush_return));
    add("terminalReceive", first.terminal_receive == AVERROR_EOF ? "AVERROR_EOF" : std::to_string(first.terminal_receive));
    add("receiveCalls", std::to_string(first.receive_calls));
    add("packetCount", std::to_string(first.packets.size()));
    for (std::size_t i = 0; i < first.packets.size(); ++i) {
      const auto &p = first.packets[i];
      const std::string k = "packet" + std::to_string(i);
      add(k + "Size", std::to_string(p.size));
      add(k + "Key", (p.flags & AV_PKT_FLAG_KEY) ? "1" : "0");
      add(k + "Flags", std::to_string(p.flags));
      add(k + "SyntheticPts", std::to_string(p.pts));
      add(k + "Dts", std::to_string(p.dts));
      add(k + "Sha256", p.sha256);
    }
    add("repeatRunPacketCount", repeat ? std::to_string(repeat->packets.size()) : "not run");
    add("repeatRunIdentical", repeat ? (repeat_identical ? "1" : "0") : "not run");

    std::set<std::string> keys;
    std::string json = "{";
    for (std::size_t i = 0; i < properties.size(); ++i) {
      EXPECT_TRUE(keys.insert(properties[i].first).second) << "repeated property " << properties[i].first;
      ::testing::Test::RecordProperty(properties[i].first, properties[i].second);
      json += (i ? "," : "") + json_string(properties[i].first) + ":" + json_string(properties[i].second);
    }
    json += "}";
    std::cout << "POLARIS_RECORDER_ENCODER_FIXTURE_JSON " << json << std::endl;
  }

  void check_and_record(const encoder_spec_t &spec) {
    run_t first;
    encode_run(spec, first);
    if (::testing::Test::HasFatalFailure()) {
      record_evidence(spec, first, nullptr, false);
      return;
    }
    run_t repeat;
    encode_run(spec, repeat);
    // The repeated, identically configured run must produce the same packets, byte for byte.
    bool repeat_identical = !::testing::Test::HasFatalFailure() && repeat.packets.size() == first.packets.size();
    for (std::size_t i = 0; repeat_identical && i < first.packets.size(); ++i) {
      const auto &a = first.packets[i];
      const auto &b = repeat.packets[i];
      repeat_identical = a.bytes == b.bytes && a.flags == b.flags && a.pts == b.pts && a.dts == b.dts;
    }
    record_evidence(spec, first, &repeat, repeat_identical);
    if (::testing::Test::HasFatalFailure()) {
      return;
    }
    EXPECT_TRUE(repeat_identical) << "the repeated run produced different packets";
    EXPECT_EQ(repeat.packets.size(), first.packets.size());

    // Justified by the configuration: no B-frames and no dropped frames, so one packet per input
    // frame in input order; keyint 4 with scene cut off, so key frames exactly at 0 and 4.
    ASSERT_EQ(first.packets.size(), static_cast<std::size_t>(k_frames));
    for (int i = 0; i < k_frames; ++i) {
      const auto &p = first.packets[i];
      EXPECT_GT(p.size, 0) << "packet " << i;
      EXPECT_EQ(p.pts, i) << "packet " << i;
      EXPECT_LE(p.dts, p.pts) << "packet " << i;
      if (i > 0) {
        EXPECT_GT(p.dts, first.packets[i - 1].dts) << "packet " << i;
      }
      EXPECT_EQ((p.flags & AV_PKT_FLAG_KEY) != 0, i % k_gop == 0) << "packet " << i;
      EXPECT_EQ(p.sha256.size(), 64u) << "packet " << i;
    }
  }
}  // namespace

TEST(RecorderEncoderFixtureTests, Libx264ProducesRepeatableRealPackets) {
  check_and_record(k_libx264);
}

TEST(RecorderEncoderFixtureTests, Libx265ProducesRepeatableRealPackets) {
  check_and_record(k_libx265);
}
