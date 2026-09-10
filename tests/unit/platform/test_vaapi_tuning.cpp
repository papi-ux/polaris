/** @file tests/unit/platform/test_vaapi_tuning.cpp */
#include "../../tests_common.h"
#include "src/platform/linux/vaapi_tuning.h"
#include <memory>

namespace {
  struct codec_options_t {
    AVCodecContext *ctx;
    AVDictionary *options {};
    explicit codec_options_t(const char *codec = "h264_vaapi") : ctx(avcodec_alloc_context3(avcodec_find_encoder_by_name(codec))) {
      if (ctx) {
        ctx->bit_rate = 12000000;
        ctx->framerate = {60000, 1001};
        ctx->rc_buffer_size = 12000000;
      }
    }
    ~codec_options_t() { av_dict_free(&options); avcodec_free_context(&ctx); }
    std::string option(const char *key) const {
      const auto entry = av_dict_get(options, key, nullptr, 0);
      return entry ? entry->value : "<unset>";
    }
  };
}

TEST(VaapiTuningTests, AutomaticPolicyPreservesExistingIntelAmdAv1AndStrictDefaults) {
  for (const auto *codec : {"h264_vaapi", "hevc_vaapi", "av1_vaapi"}) {
    for (const auto *vendor : {"Intel iHD", "Mesa AMD"}) {
      for (const bool strict : {false, true}) {
        for (const uint32_t mask : std::array<uint32_t, 5> {0, VA_RC_CQP, VA_RC_CBR, VA_RC_VBR, VA_RC_CBR | VA_RC_VBR}) {
          codec_options_t codec_options(codec);
          ASSERT_NE(codec_options.ctx, nullptr);
          ASSERT_NE(codec_options.ctx->codec, nullptr) << codec;
          const auto original_quality = codec_options.ctx->compression_level;
          const config::vaapi::settings_t settings {.strict_rc_buffer = strict};
          const auto effective = va::apply_tuning(codec_options.ctx, &codec_options.options, settings,
            {.rate_control = mask, .quality_range = 7}, vendor, 24);
          const bool single = strict || std::string_view(vendor).starts_with("Intel") || std::string_view(codec) == "av1_vaapi";
          const auto expected = single && (mask & VA_RC_VBR) ? "VBR" :
            (mask & VA_RC_CBR) ? "CBR" : (mask & VA_RC_VBR) ? "VBR" : "CQP";
          EXPECT_EQ(std::string_view(effective.rate_control), expected);
          EXPECT_EQ(codec_options.option("rc_mode"), std::string_view(expected) == "CQP" ? "<unset>" : expected);
          EXPECT_EQ(codec_options.option("qp"), std::string_view(expected) == "CQP" ? "24" : "<unset>");
          EXPECT_EQ(codec_options.ctx->rc_buffer_size, single ? 200200 : 12000000);
          EXPECT_EQ(codec_options.ctx->compression_level, original_quality);
          EXPECT_EQ(codec_options.option("blbrc"), "<unset>");
          EXPECT_FALSE(effective.explicit_rate_control);
          EXPECT_FALSE(effective.rate_control_fallback);
        }
      }
    }
  }
}

TEST(VaapiTuningTests, ExplicitSupportedModesUseRealCodecOptionsAndRespectExplicitBufferChoice) {
  for (const auto *name : {"h264_vaapi", "hevc_vaapi", "av1_vaapi"}) {
    for (const auto &mode : va::rc_modes) {
      for (const bool strict : {false, true}) {
        for (const bool qscale : {false, true}) {
          codec_options_t codec(name);
          ASSERT_NE(codec.ctx->codec, nullptr);
          if (qscale) codec.ctx->flags |= AV_CODEC_FLAG_QSCALE;
          ASSERT_TRUE(va::codec_has_rc_mode(codec.ctx, mode.name)) << name << ": " << mode.name;
          const config::vaapi::settings_t settings {.strict_rc_buffer = strict, .rc = mode.mode};
          const auto effective = va::apply_tuning(codec.ctx, &codec.options, settings, {.rate_control = mode.flag}, "Intel", 24);
          EXPECT_TRUE(effective.explicit_rate_control);
          EXPECT_FALSE(effective.rate_control_fallback);
          EXPECT_EQ(codec.option("rc_mode"), mode.name);
          const bool qp_option = av_opt_find(codec.ctx->priv_data, "qp", nullptr, 0, 0) != nullptr;
          EXPECT_EQ(codec.option("qp"), mode.uses_qp && qp_option ? "24" : "<unset>");
          if (mode.uses_qp && !qp_option) {
            EXPECT_EQ(codec.ctx->global_quality, qscale ? 24 * FF_QP2LAMBDA : 24);
          }
          EXPECT_EQ(codec.ctx->rc_buffer_size, strict ? 200200 : 12000000);
        }
      }
    }
  }
}

TEST(VaapiTuningTests, UnsupportedModesAndFailedAttributesFallBackWithoutTreatingSentinelAsCapabilities) {
  EXPECT_FALSE(va::supported_attribute(VA_STATUS_ERROR_OPERATION_FAILED, VA_RC_CBR));
  EXPECT_FALSE(va::supported_attribute(VA_STATUS_SUCCESS, VA_ATTRIB_NOT_SUPPORTED));
  EXPECT_EQ(va::supported_attribute(VA_STATUS_SUCCESS, VA_RC_VBR), VA_RC_VBR);
  codec_options_t codec;
  const auto effective = va::apply_tuning(codec.ctx, &codec.options,
    {.rc = config::vaapi::rc_e::icq}, {.rate_control = VA_RC_CBR}, "Mesa AMD", 24);
  EXPECT_TRUE(effective.rate_control_fallback);
  EXPECT_FALSE(effective.explicit_rate_control);
  EXPECT_EQ(codec.option("rc_mode"), "CBR");
  EXPECT_EQ(codec.option("qp"), "<unset>");
}

TEST(VaapiTuningTests, QualityPresetsStayInTheReportedRangeAndUnknownSupportPreservesDefaults) {
  for (const unsigned range : {0u, 1u, 2u, 7u, VA_ATTRIB_NOT_SUPPORTED}) {
    for (const auto quality : {config::vaapi::quality_e::automatic, config::vaapi::quality_e::speed,
                              config::vaapi::quality_e::balanced, config::vaapi::quality_e::quality}) {
      codec_options_t codec;
      const auto original = codec.ctx->compression_level;
      const auto effective = va::apply_tuning(codec.ctx, &codec.options, {.quality = quality},
        {.quality_range = va::supported_attribute(VA_STATUS_SUCCESS, range)}, "", 24);
      const bool unavailable = range == 0 || range == VA_ATTRIB_NOT_SUPPORTED;
      if (quality == config::vaapi::quality_e::automatic || unavailable) {
        EXPECT_EQ(codec.ctx->compression_level, original);
      } else {
        EXPECT_GE(codec.ctx->compression_level, 1);
        EXPECT_LE(codec.ctx->compression_level, static_cast<int>(range));
        const int expected = quality == config::vaapi::quality_e::speed ? range :
          quality == config::vaapi::quality_e::balanced ? std::max(1u, range / 2) : 1;
        EXPECT_EQ(codec.ctx->compression_level, expected);
      }
      EXPECT_EQ(effective.quality_fallback, quality != config::vaapi::quality_e::automatic && unavailable);
    }
  }
}

TEST(VaapiTuningTests, BlockBitrateControlRequiresDriverAndCodecSupportAndNonCqpMode) {
  for (const auto block : {std::optional<bool> {}, std::optional<bool> {false}, std::optional<bool> {true}}) {
    for (const uint32_t mask : {VA_RC_CQP, VA_RC_CQP | VA_RC_MB, VA_RC_CBR, VA_RC_CBR | VA_RC_MB}) {
      codec_options_t codec;
      const auto effective = va::apply_tuning(codec.ctx, &codec.options, {.blbrc = block}, {.rate_control = mask}, "", 24);
      const bool enabled = block.value_or(false) && (mask & VA_RC_MB) && (mask & VA_RC_CBR);
      EXPECT_EQ(codec.option("blbrc"), block ? (enabled ? "1" : "0") : "<unset>");
      EXPECT_EQ(effective.blbrc_fallback, block.value_or(false) && !enabled);
    }
  }
  codec_options_t unsupported("libx264");
  const auto effective = va::apply_tuning(unsupported.ctx, &unsupported.options,
    {.rc = config::vaapi::rc_e::icq, .blbrc = true}, {.rate_control = VA_RC_ICQ | VA_RC_MB}, "", 24);
  EXPECT_TRUE(effective.rate_control_fallback);
  EXPECT_TRUE(effective.blbrc_fallback);
  EXPECT_EQ(unsupported.option("blbrc"), "<unset>");
}

TEST(VaapiTuningTests, SettingsChangesApplyToNewCodecInitializationOnly) {
  const auto saved = config::vaapi::snapshot();
  auto restore = util::fail_guard([&] { config::vaapi::publish(saved); });
  const va::tuning_capabilities_t caps {.rate_control = VA_RC_CBR | VA_RC_VBR | VA_RC_MB, .quality_range = 7};
  config::vaapi::publish({});
  codec_options_t first;
  va::apply_tuning(first.ctx, &first.options, config::vaapi::snapshot(), caps, "Mesa AMD", 24);
  config::vaapi::publish({.quality = config::vaapi::quality_e::quality, .rc = config::vaapi::rc_e::vbr, .blbrc = true});
  codec_options_t next;
  va::apply_tuning(next.ctx, &next.options, config::vaapi::snapshot(), caps, "Mesa AMD", 24);
  EXPECT_EQ(first.option("rc_mode"), "CBR");
  EXPECT_EQ(first.option("blbrc"), "<unset>");
  EXPECT_EQ(next.option("rc_mode"), "VBR");
  EXPECT_EQ(next.option("blbrc"), "1");
  EXPECT_EQ(next.ctx->compression_level, 1);
}
