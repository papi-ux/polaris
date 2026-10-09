/**
 * @file tests/unit/test_recorder_encoder_fixture.cpp
 * @brief Bounded in-process libx264/libx265 CPU encoder fixtures for the recorder's media work (#363).
 *
 * Each case opens one named software encoder from the linked FFmpeg, feeds it a fixed number of
 * deterministic YUV420P frames with explicit single-thread, no-B-frame, fixed key-frame interval and
 * scene-cut-disabled settings, and drains it with a bounded send/receive/flush loop. The first run
 * must pass a positive baseline (packet count, non-empty bounded payloads, order, key-frame
 * placement, hashes) before an identical repeat run starts; the repeat must match byte for byte.
 * Every dictionary-set, open, send, receive and flush result of both runs, and each packet's size,
 * flags, synthetic PTS, DTS and SHA-256, is recorded as GTest XML properties and in one matching
 * console JSON line, before any assertion, so a failing run still leaves its evidence.
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
  /// Generous per-packet cap for 64x64 frames; a larger or empty payload is invalid here.
  constexpr int k_max_packet_bytes = 1 << 20;

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

  /// Everything one run observed. Codes are the raw FFmpeg return values.
  struct run_t {
    bool attempted = false;
    std::vector<std::pair<std::string, int>> dict_set_returns;  ///< av_dict_set results (dictionary only)
    int open_return = 1;      ///< 1 until avcodec_open2 was called
    int leftover_options = -1;
    int flush_return = 1;     ///< 1 until the flush was sent
    int terminal_receive = 1; ///< The receive result that ended the final drain
    int receive_calls = 0;
    std::vector<std::string> calls;  ///< Every send/receive/flush, in order, with its result
    std::vector<packet_record_t> packets;
  };

  std::string code_text(int code) {
    if (code == AVERROR(EAGAIN)) {
      return "EAGAIN";
    }
    if (code == AVERROR_EOF) {
      return "EOF";
    }
    return std::to_string(code);
  }

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

  /// Drain every packet available now. Returns the receive result that stopped it, or
  /// AVERROR_INVALIDDATA for an invalid packet, or AVERROR(ERANGE) when the bound is reached.
  int drain(AVCodecContext *ctx, AVPacket *packet, run_t &run) {
    while (run.receive_calls < k_max_receive_calls) {
      ++run.receive_calls;
      const int ret = avcodec_receive_packet(ctx, packet);
      if (ret < 0) {
        run.calls.push_back("receive=" + code_text(ret));
        return ret;  // AVERROR(EAGAIN), AVERROR_EOF or a real error
      }
      // Validate before any pointer arithmetic or hashing.
      if (!packet->data || packet->size <= 0 || packet->size > k_max_packet_bytes) {
        run.calls.push_back("receive=invalid-packet size " + std::to_string(packet->size));
        av_packet_unref(packet);
        return AVERROR_INVALIDDATA;
      }
      run.calls.push_back("receive=0 size " + std::to_string(packet->size));
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
    run.calls.push_back("receive=bound reached");
    return AVERROR(ERANGE);
  }

  /// One complete bounded run of `spec`. Returns an empty string on success, else the reason.
  /// It asserts nothing, so the caller can record the evidence first.
  std::string encode_run(const encoder_spec_t &spec, run_t &run) {
    run.attempted = true;
    const AVCodec *codec = avcodec_find_encoder_by_name(spec.name);
    if (!codec) {
      return std::string {spec.name} + " is not registered in the linked FFmpeg (dependency finding)";
    }
    std::unique_ptr<AVCodecContext, context_deleter> ctx {avcodec_alloc_context3(codec)};
    if (!ctx) {
      return "context allocation failed";
    }
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
      run.dict_set_returns.emplace_back(key, av_dict_set(&raw, key, value, 0));
    }
    std::unique_ptr<AVDictionary, dict_deleter> options {raw};
    for (const auto &[key, ret] : run.dict_set_returns) {
      if (ret < 0) {
        return std::string {"av_dict_set "} + key + " returned " + std::to_string(ret);
      }
    }

    AVDictionary *remaining = options.release();
    run.open_return = avcodec_open2(ctx.get(), codec, &remaining);
    options.reset(remaining);
    run.leftover_options = av_dict_count(options.get());
    if (run.open_return != 0) {
      char error[AV_ERROR_MAX_STRING_SIZE] {};
      av_strerror(run.open_return, error, sizeof error);
      return std::string {spec.name} + " failed to open: " + error + " (dependency finding)";
    }
    if (run.leftover_options != 0) {
      return std::string {"an option was not consumed by "} + spec.name;
    }

    std::unique_ptr<AVFrame, frame_deleter> frame {av_frame_alloc()};
    std::unique_ptr<AVPacket, packet_deleter> packet {av_packet_alloc()};
    if (!frame || !packet) {
      return "frame or packet allocation failed";
    }
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = k_width;
    frame->height = k_height;
    if (av_frame_get_buffer(frame.get(), 0) != 0) {
      return "frame buffer allocation failed";
    }

    for (int i = 0; i < k_frames; ++i) {
      if (av_frame_make_writable(frame.get()) != 0) {
        return "frame not writable at " + std::to_string(i);
      }
      fill_frame(frame.get(), i);
      frame->pts = i;  // Synthetic fixture counter, not producer provenance or presentation time
      int sent = avcodec_send_frame(ctx.get(), frame.get());
      run.calls.push_back("send" + std::to_string(i) + "=" + code_text(sent));
      if (sent == AVERROR(EAGAIN)) {
        // The encoder wants output read first: drain once, then the same frame must be accepted.
        const int made_room = drain(ctx.get(), packet.get(), run);
        if (made_room != AVERROR(EAGAIN)) {
          return "receive while making room ended with " + code_text(made_room);
        }
        sent = avcodec_send_frame(ctx.get(), frame.get());
        run.calls.push_back("resend" + std::to_string(i) + "=" + code_text(sent));
      }
      if (sent != 0) {
        return "send_frame " + std::to_string(i) + " returned " + code_text(sent);
      }
      const int drained = drain(ctx.get(), packet.get(), run);
      if (drained != AVERROR(EAGAIN)) {
        return "receive after frame " + std::to_string(i) + " ended with " + code_text(drained);
      }
    }
    run.flush_return = avcodec_send_frame(ctx.get(), nullptr);
    run.calls.push_back("flush=" + code_text(run.flush_return));
    if (run.flush_return != 0) {
      return "flush returned " + code_text(run.flush_return);
    }
    run.terminal_receive = drain(ctx.get(), packet.get(), run);
    if (run.terminal_receive != AVERROR_EOF) {
      return "the final drain ended with " + code_text(run.terminal_receive) + " instead of EOF";
    }
    return {};
  }

  /// The positive baseline the configuration justifies: no B-frames and no dropped frames, so one
  /// packet per input frame in input order; keyint 4 with scene cut off, so key frames exactly at
  /// 0 and 4. DTS is only required to increase and not exceed PTS. Empty string when it holds.
  std::string validate_baseline(const run_t &run) {
    if (run.packets.size() != static_cast<std::size_t>(k_frames)) {
      return "expected " + std::to_string(k_frames) + " packets, got " + std::to_string(run.packets.size());
    }
    for (int i = 0; i < k_frames; ++i) {
      const auto &p = run.packets[i];
      const std::string at = " at packet " + std::to_string(i);
      if (p.size <= 0 || p.size > k_max_packet_bytes || static_cast<std::size_t>(p.size) != p.bytes.size()) {
        return "invalid size" + at;
      }
      if (p.pts != i) {
        return "synthetic PTS out of order" + at;
      }
      if (p.dts > p.pts || (i > 0 && p.dts <= run.packets[i - 1].dts)) {
        return "DTS not increasing or above PTS" + at;
      }
      if (((p.flags & AV_PKT_FLAG_KEY) != 0) != (i % k_gop == 0)) {
        return "key-frame flag not at the configured interval" + at;
      }
      if (p.sha256.size() != 64) {
        return "missing SHA-256" + at;
      }
    }
    return {};
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

  void add_run(std::vector<std::pair<std::string, std::string>> &properties, const std::string &prefix, const run_t &run,
               const std::string &result) {
    const auto add = [&properties, &prefix](const std::string &key, std::string value) {
      properties.emplace_back(prefix + key, std::move(value));
    };
    add("Attempted", run.attempted ? "1" : "0");
    add("Result", result.empty() ? (run.attempted ? "ok" : "not run") : result);
    for (std::size_t i = 0; i < 3; ++i) {
      add("AvDictSetReturn" + std::to_string(i),
          i < run.dict_set_returns.size() ? run.dict_set_returns[i].first + "=" + std::to_string(run.dict_set_returns[i].second) : "not reached");
    }
    add("OpenReturn", run.open_return == 1 ? "not reached" : std::to_string(run.open_return));
    add("LeftoverOptions", run.leftover_options < 0 ? "not reached" : std::to_string(run.leftover_options));
    add("FlushReturn", run.flush_return == 1 ? "not reached" : code_text(run.flush_return));
    add("TerminalReceive", run.terminal_receive == 1 ? "not reached" : code_text(run.terminal_receive));
    add("ReceiveCalls", std::to_string(run.receive_calls));
    std::string calls;
    for (std::size_t i = 0; i < run.calls.size(); ++i) {
      calls += (i ? ";" : "") + run.calls[i];
    }
    add("CallSequence", calls);
    add("PacketCount", std::to_string(run.packets.size()));
    for (std::size_t i = 0; i < run.packets.size(); ++i) {
      const auto &p = run.packets[i];
      const std::string k = "Packet" + std::to_string(i);
      add(k + "Size", std::to_string(p.size));
      add(k + "Key", (p.flags & AV_PKT_FLAG_KEY) ? "1" : "0");
      add(k + "Flags", std::to_string(p.flags));
      add(k + "SyntheticPts", std::to_string(p.pts));
      add(k + "Dts", std::to_string(p.dts));
      add(k + "Sha256", p.sha256);
    }
  }

  /// Record everything observed, identically as XML properties and one console JSON line.
  void record_evidence(const encoder_spec_t &spec, const run_t &first, const std::string &first_result,
                       const std::string &baseline, const run_t &repeat, const std::string &repeat_result,
                       const std::string &comparison) {
    std::vector<std::pair<std::string, std::string>> properties;
    const auto add = [&properties](std::string key, std::string value) {
      properties.emplace_back(std::move(key), std::move(value));
    };
    add("scope", "in-process CPU encoder baseline; no decode, structure parse, recorder or playback claim");
    add("ptsNote", "fixture-assigned synthetic frame counter; not producer provenance or presentation time");
    add("dictSetNote", "av_dict_set results only populate the options dictionary; the encoder validates them at open");
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
    add("maxReceiveCallsPerRun", std::to_string(k_max_receive_calls));
    add("maxPacketBytes", std::to_string(k_max_packet_bytes));
    add_run(properties, "run1", first, first_result);
    add("baselineResult", !first.attempted || !first_result.empty() ? "not evaluated" : (baseline.empty() ? "ok" : baseline));
    add_run(properties, "run2", repeat, repeat_result);
    add("repeatComparison", comparison);

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
    const std::string first_result = encode_run(spec, first);
    const std::string baseline = first_result.empty() ? validate_baseline(first) : std::string {};

    // The repeat starts only once the first run's positive baseline holds.
    run_t repeat;
    std::string repeat_result;
    std::string comparison = "not run";
    if (first_result.empty() && baseline.empty()) {
      repeat_result = encode_run(spec, repeat);
      if (repeat_result.empty()) {
        // The repeat must pass the same baseline as the first run (including a real SHA-256 for
        // every packet), so a hash that could not be computed fails it exactly as it fails run 1.
        const std::string repeat_baseline = validate_baseline(repeat);
        if (!repeat_baseline.empty()) {
          repeat_result = "repeat baseline: " + repeat_baseline;
        }
      }
      if (repeat_result.empty()) {
        comparison = "identical";
        if (repeat.packets.size() != first.packets.size()) {
          comparison = "packet count differs";
        }
        for (std::size_t i = 0; comparison == "identical" && i < first.packets.size(); ++i) {
          const auto &a = first.packets[i];
          const auto &b = repeat.packets[i];
          if (a.bytes != b.bytes || a.sha256 != b.sha256 || a.flags != b.flags || a.pts != b.pts || a.dts != b.dts) {
            comparison = "packet " + std::to_string(i) + " differs";
          }
        }
      }
    }

    record_evidence(spec, first, first_result, baseline, repeat, repeat_result, comparison);

    ASSERT_EQ(first_result, "") << "first run";
    ASSERT_EQ(baseline, "") << "first-run positive baseline";
    ASSERT_EQ(repeat_result, "") << "repeat run";
    EXPECT_EQ(comparison, "identical") << "the repeated run must reproduce every packet byte for byte";
  }
}  // namespace

TEST(RecorderEncoderFixtureTests, Libx264ProducesRepeatableRealPackets) {
  check_and_record(k_libx264);
}

TEST(RecorderEncoderFixtureTests, Libx265ProducesRepeatableRealPackets) {
  check_and_record(k_libx265);
}
