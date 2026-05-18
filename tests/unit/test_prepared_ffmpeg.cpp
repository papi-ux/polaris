/**
 * @file tests/unit/test_prepared_ffmpeg.cpp
 * @brief Test bundled FFmpeg capabilities required by Polaris.
 */
#include "../tests_common.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace {
  bool encoder_has_private_option(const char *codec_name, const char *option_name) {
    auto *codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
      return false;
    }

    auto *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
      return false;
    }

    const bool has_option = av_opt_find(ctx, option_name, nullptr, 0, AV_OPT_SEARCH_CHILDREN) != nullptr;
    avcodec_free_context(&ctx);
    return has_option;
  }
}  // namespace

TEST(PreparedFfmpegTests, NvencHevcAndAv1ExposeSplitEncodeMode) {
  EXPECT_TRUE(encoder_has_private_option("hevc_nvenc", "split_encode_mode"));
  EXPECT_TRUE(encoder_has_private_option("av1_nvenc", "split_encode_mode"));
}
