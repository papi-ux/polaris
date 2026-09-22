/**
 * @file tests/unit/test_cbs.cpp
 * @brief Test how Polaris reads the parameter sets of an encoded frame.
 */
#include "../tests_common.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

#include <src/cbs.h>

namespace {
  struct codec_ctx_deleter {
    void operator()(AVCodecContext *ctx) const {
      avcodec_free_context(&ctx);
    }
  };

  struct packet_deleter {
    void operator()(AVPacket *packet) const {
      av_packet_free(&packet);
    }
  };

  using codec_ctx_ptr = std::unique_ptr<AVCodecContext, codec_ctx_deleter>;
  using packet_ptr = std::unique_ptr<AVPacket, packet_deleter>;

  /// One grey frame from a bundled software encoder, Annex B, so its SPS names a colour description.
  std::vector<std::uint8_t> encode_one_frame(const char *encoder_name, codec_ctx_ptr &ctx) {
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
      return {};
    }
    ctx.reset(avcodec_alloc_context3(codec));
    ctx->width = 64;
    ctx->height = 64;
    ctx->time_base = {1, 60};
    ctx->framerate = {60, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->color_range = AVCOL_RANGE_MPEG;
    ctx->colorspace = AVCOL_SPC_BT709;
    ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->color_trc = AVCOL_TRC_BT709;
    av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) {
      return {};
    }

    AVFrame *frame = av_frame_alloc();
    frame->format = ctx->pix_fmt;
    frame->width = ctx->width;
    frame->height = ctx->height;
    std::vector<std::uint8_t> bytes;
    if (av_frame_get_buffer(frame, 0) == 0) {
      for (int plane = 0; plane < 3; ++plane) {
        const int rows = plane == 0 ? frame->height : frame->height / 2;
        std::memset(frame->data[plane], 128, static_cast<std::size_t>(frame->linesize[plane]) * rows);
      }
      frame->pts = 0;
      packet_ptr packet {av_packet_alloc()};
      if (avcodec_send_frame(ctx.get(), frame) == 0 && avcodec_send_frame(ctx.get(), nullptr) == 0 &&
          avcodec_receive_packet(ctx.get(), packet.get()) == 0) {
        bytes.assign(packet->data, packet->data + packet->size);
      }
    }
    av_frame_free(&frame);
    return bytes;
  }

  packet_ptr packet_of(const std::vector<std::uint8_t> &bytes) {
    packet_ptr packet {av_packet_alloc()};
    if (av_new_packet(packet.get(), static_cast<int>(bytes.size())) == 0) {
      std::memcpy(packet->data, bytes.data(), bytes.size());
      packet->flags |= AV_PKT_FLAG_KEY;
    }
    return packet;
  }

  /// A filler data unit the way a VA-API encoder at CBR pads a frame, which FFmpeg's reader cannot decompose.
  std::vector<std::uint8_t> padded_with_filler(std::vector<std::uint8_t> bytes, std::vector<std::uint8_t> filler) {
    bytes.insert(bytes.end(), {0x00, 0x00, 0x00, 0x01});
    bytes.insert(bytes.end(), filler.begin(), filler.end());
    return bytes;
  }
}  // namespace

// Found on a Steam Deck: the encoder check read the SPS of its own test frame, the frame carried
// filler data, and one unit FFmpeg could not read failed the whole packet. The SPS went unread, the
// encoder was taken to have no VUI, and every H.264 stream from that host had its SPS rewritten.
TEST(CbsReadPacket, FillerDataDoesNotHideTheH264Sps) {
  codec_ctx_ptr ctx;
  const auto frame = encode_one_frame("libx264", ctx);
  if (frame.empty()) {
    GTEST_SKIP() << "this FFmpeg has no libx264";
  }

  const auto plain = packet_of(frame);
  const bool plain_has_vui = cbs::validate_sps(plain.get(), AV_CODEC_ID_H264);
  EXPECT_TRUE(plain_has_vui) << "a frame with a colour description carries VUI";

  // Type 12 with no trailing bits, and type 12 as a padding run that ends properly.
  for (const auto &filler : std::vector<std::vector<std::uint8_t>> {{0x0c, 0xff, 0xff, 0xff, 0xff}, {0x0c, 0xff, 0xff, 0x80}}) {
    const auto padded = packet_of(padded_with_filler(frame, filler));
    EXPECT_EQ(cbs::validate_sps(padded.get(), AV_CODEC_ID_H264), plain_has_vui);
    const auto rewritten = cbs::make_sps_h264(ctx.get(), padded.get());
    EXPECT_GT(rewritten.sps.old.size(), 0u) << "the SPS a stream would rewrite is found despite the filler";
  }
}

TEST(CbsReadPacket, FillerDataDoesNotHideTheHevcSps) {
  codec_ctx_ptr ctx;
  const auto frame = encode_one_frame("libx265", ctx);
  if (frame.empty()) {
    GTEST_SKIP() << "this FFmpeg has no libx265";
  }

  const auto plain = packet_of(frame);
  const bool plain_has_vui = cbs::validate_sps(plain.get(), AV_CODEC_ID_H265);
  // FD_NUT, type 38, in a two-byte header, with no trailing bits.
  const auto padded = packet_of(padded_with_filler(frame, {0x4c, 0x01, 0xff, 0xff, 0xff}));
  EXPECT_EQ(cbs::validate_sps(padded.get(), AV_CODEC_ID_H265), plain_has_vui);
  const auto rewritten = cbs::make_sps_hevc(ctx.get(), padded.get());
  EXPECT_GT(rewritten.sps.old.size(), 0u);
}

// A packet with no slice leaves FFmpeg with no active SPS, and reading one must not crash.
TEST(CbsReadPacket, APacketWithNoSliceHasNoActiveSps) {
  const auto only_filler = packet_of({0x00, 0x00, 0x00, 0x01, 0x0c, 0xff, 0xff, 0x80});
  EXPECT_FALSE(cbs::validate_sps(only_filler.get(), AV_CODEC_ID_H264));
  codec_ctx_ptr ctx {avcodec_alloc_context3(nullptr)};
  EXPECT_EQ(cbs::make_sps_h264(ctx.get(), only_filler.get()).sps.old.size(), 0u);
}
