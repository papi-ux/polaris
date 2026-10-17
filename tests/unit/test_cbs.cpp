/**
 * @file tests/unit/test_cbs.cpp
 * @brief Test how Polaris reads the parameter sets of an encoded frame.
 */
#include "../tests_common.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/cbs_h265.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

#include <src/cbs.h>
#include <src/video.h>

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

  std::vector<std::uint8_t> without_hevc_vui(const std::vector<std::uint8_t> &bytes) {
    CodedBitstreamContext *ctx {};
    CodedBitstreamFragment fragment {};
    auto cleanup = util::fail_guard([&]() {
      ff_cbs_fragment_free(&fragment);
      ff_cbs_close(&ctx);
    });
    const auto packet = packet_of(bytes);
    const int init = ff_cbs_init(&ctx, AV_CODEC_ID_HEVC, nullptr);
    EXPECT_EQ(init, 0);
    if (init != 0) {
      return {};
    }
    const int read = ff_cbs_read_packet(ctx, &fragment, packet.get());
    EXPECT_EQ(read, 0);
    if (read != 0) {
      return {};
    }
    H265RawSPS *sps {};
    for (int i = 0; i < fragment.nb_units; ++i) {
      if (fragment.units[i].type == HEVC_NAL_SPS) {
        sps = static_cast<H265RawSPS *>(fragment.units[i].content);
        break;
      }
    }
    EXPECT_NE(sps, nullptr);
    if (!sps) {
      return {};
    }
    sps->vui_parameters_present_flag = 0;
    // CBS also validates the inferred values when VUI syntax is absent.
    auto &vui = sps->vui;
    std::memset(&vui, 0, sizeof(vui));
    vui.video_format = 5;
    vui.colour_primaries = AVCOL_PRI_UNSPECIFIED;
    vui.transfer_characteristics = AVCOL_TRC_UNSPECIFIED;
    vui.matrix_coefficients = AVCOL_SPC_UNSPECIFIED;
    vui.motion_vectors_over_pic_boundaries_flag = 1;
    vui.max_bytes_per_pic_denom = 2;
    vui.max_bits_per_min_cu_denom = 1;
    vui.log2_max_mv_length_horizontal = 15;
    vui.log2_max_mv_length_vertical = 15;
    const int write = ff_cbs_write_fragment_data(ctx, &fragment);
    EXPECT_EQ(write, 0);
    if (write != 0) {
      return {};
    }
    return {fragment.data, fragment.data + fragment.data_size};
  }

  std::vector<std::uint8_t> with_truncated_hevc_idr(const std::vector<std::uint8_t> &bytes) {
    for (std::size_t i = 0; i + 5 < bytes.size(); ++i) {
      std::size_t header {};
      if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
        header = i + 3;
      } else if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1) {
        header = i + 4;
      } else {
        continue;
      }
      const auto type = (bytes[header] >> 1) & 0x3f;
      if (type == HEVC_NAL_IDR_W_RADL || type == HEVC_NAL_IDR_N_LP) {
        // Keep the real VPS/SPS/PPS, but no slice header after the two-byte NAL header.
        return {bytes.begin(), bytes.begin() + header + 2};
      }
    }
    return {};
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
  EXPECT_EQ(cbs::inspect_sps(only_filler.get(), AV_CODEC_ID_H264), cbs::sps_vui_status_e::unreadable);
  EXPECT_FALSE(cbs::validate_sps(only_filler.get(), AV_CODEC_ID_H264));
  codec_ctx_ptr ctx {avcodec_alloc_context3(nullptr)};
  EXPECT_EQ(cbs::make_sps_h264(ctx.get(), only_filler.get()).sps.old.size(), 0u);
}

TEST(CbsReadPacket, DistinguishesUnreadableHevcFromReadableSpsWithoutVui) {
  if (!avcodec_find_encoder_by_name("libx265")) {
    GTEST_SKIP() << "this FFmpeg has no libx265";
  }
  codec_ctx_ptr ctx;
  const auto frame = encode_one_frame("libx265", ctx);
  ASSERT_FALSE(frame.empty()) << "the available libx265 must encode the control packet";
  const auto plain = packet_of(frame);
  ASSERT_EQ(cbs::inspect_sps(plain.get(), AV_CODEC_ID_HEVC), cbs::sps_vui_status_e::present);
  EXPECT_GT(video::validate_avcodec_probe_packet_for_tests(plain.get(), AV_CODEC_ID_HEVC, "hevc_vulkan"), 0);

  const auto no_vui_bytes = without_hevc_vui(frame);
  ASSERT_FALSE(no_vui_bytes.empty());
  const auto no_vui = packet_of(no_vui_bytes);
  EXPECT_EQ(cbs::inspect_sps(no_vui.get(), AV_CODEC_ID_HEVC), cbs::sps_vui_status_e::absent);
  EXPECT_FALSE(cbs::validate_sps(no_vui.get(), AV_CODEC_ID_HEVC));
  EXPECT_EQ(video::validate_avcodec_probe_packet_for_tests(no_vui.get(), AV_CODEC_ID_HEVC, "hevc_vulkan"), 0)
    << "a readable packet without VUI remains eligible for parameter-set repair";

  const auto unreadable_bytes = with_truncated_hevc_idr(frame);
  ASSERT_FALSE(unreadable_bytes.empty());
  const auto unreadable = packet_of(unreadable_bytes);
  EXPECT_EQ(cbs::inspect_sps(unreadable.get(), AV_CODEC_ID_HEVC), cbs::sps_vui_status_e::unreadable);
  EXPECT_FALSE(cbs::validate_sps(unreadable.get(), AV_CODEC_ID_HEVC))
    << "the old VUI predicate returns false for both cases; only a readable SPS can be repaired";
  EXPECT_LT(video::validate_avcodec_probe_packet_for_tests(unreadable.get(), AV_CODEC_ID_HEVC, "hevc_vulkan"), 0);
  EXPECT_EQ(video::validate_avcodec_probe_packet_for_tests(unreadable.get(), AV_CODEC_ID_HEVC, "hevc_nvenc"), 0)
    << "the new refusal is confined to Vulkan HEVC";
  const auto h264_filler = packet_of({0x00, 0x00, 0x00, 0x01, 0x0c, 0xff, 0xff, 0x80});
  EXPECT_EQ(video::validate_avcodec_probe_packet_for_tests(h264_filler.get(), AV_CODEC_ID_H264, "h264_vulkan"), 0);
}

namespace {
  std::vector<std::uint8_t> stripped(std::vector<std::uint8_t> bytes, int codec_id) {
    bytes.resize(cbs::strip_filler_data(bytes.data(), bytes.size(), codec_id));
    return bytes;
  }

  std::vector<std::uint8_t> joined(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> bytes;
    for (const auto &part : parts) {
      bytes.insert(bytes.end(), part.begin(), part.end());
    }
    return bytes;
  }

  const std::vector<std::uint8_t> h264_filler {0x00, 0x00, 0x00, 0x01, 0x0c, 0xff, 0xff, 0xff, 0x80};
  const std::vector<std::uint8_t> hevc_filler {0x00, 0x00, 0x00, 0x01, 0x4c, 0x01, 0xff, 0xff, 0xff, 0x80};
}  // namespace

// What radeonsi and RADV send in CBR on a still screen: the picture, then filler to the target bitrate.
TEST(CbsStripFillerData, LeavesAnH264FrameExactlyAsItWasWithoutItsPadding) {
  codec_ctx_ptr ctx;
  const auto frame = encode_one_frame("libx264", ctx);
  if (frame.empty()) {
    GTEST_SKIP() << "this FFmpeg has no libx264";
  }

  EXPECT_EQ(stripped(joined({frame, h264_filler}), AV_CODEC_ID_H264), frame);
  EXPECT_EQ(stripped(joined({frame, h264_filler, h264_filler, h264_filler}), AV_CODEC_ID_H264), frame);
  // Without trailing bits, and with a three-byte start code.
  EXPECT_EQ(stripped(joined({frame, {0x00, 0x00, 0x01, 0x0c, 0xff, 0xff, 0xff, 0xff}}), AV_CODEC_ID_H264), frame);
  // In front of the picture, where the picture's own start code has to survive.
  EXPECT_EQ(stripped(joined({h264_filler, frame}), AV_CODEC_ID_H264), frame);

  const auto packet = packet_of(stripped(joined({frame, h264_filler}), AV_CODEC_ID_H264));
  EXPECT_TRUE(cbs::validate_sps(packet.get(), AV_CODEC_ID_H264));
}

TEST(CbsStripFillerData, LeavesAnHevcFrameExactlyAsItWasWithoutItsPadding) {
  codec_ctx_ptr ctx;
  const auto frame = encode_one_frame("libx265", ctx);
  if (frame.empty()) {
    GTEST_SKIP() << "this FFmpeg has no libx265";
  }

  EXPECT_EQ(stripped(joined({frame, hevc_filler}), AV_CODEC_ID_H265), frame);
  EXPECT_EQ(stripped(joined({hevc_filler, frame, hevc_filler}), AV_CODEC_ID_H265), frame);
}

TEST(CbsStripFillerData, RemovesFillerBetweenUnitsWithTheZeroBytesInFrontOfIt) {
  const std::vector<std::uint8_t> sps {0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1f};
  // A slice whose payload holds an escaped 00 00 01, which must not read as a start code.
  const std::vector<std::uint8_t> slice {0x00, 0x00, 0x01, 0x65, 0x88, 0x00, 0x00, 0x03, 0x01, 0x80};
  // trailing_zero_8bits after the SPS, then the filler.
  const std::vector<std::uint8_t> zeros_then_filler {0x00, 0x00, 0x00, 0x00, 0x01, 0x0c, 0xff, 0x80};

  EXPECT_EQ(stripped(joined({sps, zeros_then_filler, slice}), AV_CODEC_ID_H264), joined({sps, slice}));
  EXPECT_EQ(stripped(joined({sps, slice, h264_filler}), AV_CODEC_ID_H264), joined({sps, slice}));
}

TEST(CbsStripFillerData, HandlesTheHevcUnitsNextToFiller) {
  // TRAIL_N's header is 00 01, so the unit opens with the same bytes a start code ends with.
  const std::vector<std::uint8_t> trail_n {0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0xaf, 0x80};
  const std::vector<std::uint8_t> short_trail_n {0x00, 0x00, 0x01, 0x00, 0x01, 0xaf, 0x80};
  // A three-byte start code, without trailing bits.
  const std::vector<std::uint8_t> short_filler {0x00, 0x00, 0x01, 0x4c, 0x01, 0xff, 0xff};
  // The high bit of nuh_layer_id is the last bit of the first header byte, so 0x4d is still FD_NUT.
  const std::vector<std::uint8_t> layer_filler {0x00, 0x00, 0x00, 0x01, 0x4d, 0x01, 0xff, 0x80};

  EXPECT_EQ(stripped(joined({trail_n, short_filler}), AV_CODEC_ID_H265), trail_n);
  EXPECT_EQ(stripped(joined({short_filler, trail_n}), AV_CODEC_ID_H265), trail_n);
  EXPECT_EQ(stripped(joined({short_filler, short_trail_n}), AV_CODEC_ID_H265), short_trail_n);
  EXPECT_EQ(stripped(joined({trail_n, short_filler, short_trail_n}), AV_CODEC_ID_H265), joined({trail_n, short_trail_n}));
  EXPECT_EQ(stripped(joined({trail_n, layer_filler}), AV_CODEC_ID_H265), trail_n);
  EXPECT_EQ(stripped(joined({layer_filler, short_trail_n, hevc_filler}), AV_CODEC_ID_H265), short_trail_n);
}

TEST(CbsStripFillerData, LeavesEverythingElseAlone) {
  const std::vector<std::uint8_t> slice {0x00, 0x00, 0x00, 0x01, 0x41, 0x9a, 0x00, 0x00, 0x03, 0x01, 0x80};
  EXPECT_EQ(stripped(slice, AV_CODEC_ID_H264), slice) << "a frame with no filler";

  // HEVC reads H.264's filler header, 0x0c, as type 6: a RADL_N slice, which stays.
  const std::vector<std::uint8_t> hevc_slice {0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xaf, 0x80};
  EXPECT_EQ(stripped(joined({hevc_slice, h264_filler}), AV_CODEC_ID_H265), joined({hevc_slice, h264_filler}));

  EXPECT_EQ(stripped(joined({slice, h264_filler}), AV_CODEC_ID_AV1), joined({slice, h264_filler})) << "not H.264 or HEVC";
  EXPECT_EQ(stripped(h264_filler, AV_CODEC_ID_H264), h264_filler) << "a frame of nothing but filler is not sent empty";
  EXPECT_EQ(stripped(joined({h264_filler, h264_filler, {0x00, 0x00, 0x01}}), AV_CODEC_ID_H264), joined({h264_filler, h264_filler, {0x00, 0x00, 0x01}}));

  const std::vector<std::uint8_t> not_annex_b {0x0c, 0xff, 0xff, 0x80};
  EXPECT_EQ(stripped(not_annex_b, AV_CODEC_ID_H264), not_annex_b);
  EXPECT_EQ(stripped({}, AV_CODEC_ID_H264), std::vector<std::uint8_t> {});
}
