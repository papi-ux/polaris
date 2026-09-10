/** @file src/platform/linux/vaapi_tuning.h
 * Apply driver-checked VA-API settings when creating an encoder session.
 */
#pragma once
#include "src/vaapi_config.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <string_view>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <va/va.h>
}

namespace va {
  struct tuning_capabilities_t {
    std::optional<uint32_t> rate_control;
    std::optional<uint32_t> quality_range;
  };

  inline std::optional<uint32_t> supported_attribute(VAStatus status, uint32_t value) {
    if (status != VA_STATUS_SUCCESS || value == VA_ATTRIB_NOT_SUPPORTED) return std::nullopt;
    return value;
  }

  struct rc_mode_t {
    config::vaapi::rc_e mode;
    const char *name;
    uint32_t flag;
    bool uses_qp;
  };
  inline constexpr std::array rc_modes {
    rc_mode_t {config::vaapi::rc_e::avbr, "AVBR", VA_RC_AVBR, false},
    rc_mode_t {config::vaapi::rc_e::cbr, "CBR", VA_RC_CBR, false},
    rc_mode_t {config::vaapi::rc_e::cqp, "CQP", VA_RC_CQP, true},
    rc_mode_t {config::vaapi::rc_e::icq, "ICQ", VA_RC_ICQ, true},
    rc_mode_t {config::vaapi::rc_e::qvbr, "QVBR", VA_RC_QVBR, true},
    rc_mode_t {config::vaapi::rc_e::vbr, "VBR", VA_RC_VBR, false},
  };

  struct tuning_result_t {
    const char *rate_control {"CQP"};
    bool explicit_rate_control {false};
    bool rate_control_fallback {false};
    bool single_frame_buffer {false};
    bool quality_fallback {false};
    bool blbrc_fallback {false};
    std::optional<bool> blbrc;
  };

  inline bool codec_has_rc_mode(AVCodecContext *ctx, const char *name) {
    return ctx->priv_data && av_opt_find(ctx->priv_data, "rc_mode", nullptr, 0, 0) &&
           av_opt_find(ctx->priv_data, name, "rc_mode", 0, 0);
  }

  inline tuning_result_t apply_tuning(AVCodecContext *ctx, AVDictionary **options,
                                      const config::vaapi::settings_t &settings,
                                      const tuning_capabilities_t &caps,
                                      std::string_view vendor, int qp) {
    tuning_result_t result;
    const auto mask = caps.rate_control.value_or(0);
    const rc_mode_t *selected = nullptr;
    if (settings.rc != config::vaapi::rc_e::automatic) {
      for (const auto &mode : rc_modes) {
        if (mode.mode == settings.rc && (mask & mode.flag) && codec_has_rc_mode(ctx, mode.name)) {
          selected = &mode;
          break;
        }
      }
      result.explicit_rate_control = selected != nullptr;
      result.rate_control_fallback = !selected;
    }
    // Preserve Polaris's automatic policy, including strict-buffer preference
    // for VBR and the AMD VBR-only fallback. A supported explicit mode opts out
    // of the Intel/AV1 preference, but still respects the user's buffer setting.
    result.single_frame_buffer = settings.strict_rc_buffer ||
      (!selected && (vendor.find("Intel") != std::string_view::npos || ctx->codec_id == AV_CODEC_ID_AV1));
    if (!selected) {
      const auto automatic = result.single_frame_buffer && (mask & VA_RC_VBR) ? config::vaapi::rc_e::vbr :
        (mask & VA_RC_CBR) ? config::vaapi::rc_e::cbr :
        (mask & VA_RC_VBR) ? config::vaapi::rc_e::vbr : config::vaapi::rc_e::cqp;
      for (const auto &mode : rc_modes) {
        if (mode.mode == automatic) { selected = &mode; break; }
      }
    }
    result.rate_control = selected->name;
    // The existing automatic CQP fallback is requested through qp, leaving the
    // codec's CQP selection behavior intact when a driver cannot report modes.
    if (selected->mode != config::vaapi::rc_e::cqp || result.explicit_rate_control) {
      av_dict_set(options, "rc_mode", selected->name, 0);
    }
    if (selected->uses_qp) {
      if (!result.explicit_rate_control || (ctx->priv_data && av_opt_find(ctx->priv_data, "qp", nullptr, 0, 0))) {
        av_dict_set_int(options, "qp", qp, 0);
      } else {
        // AV1 exposes global_quality instead of the H.264/HEVC qp option.
        // FFmpeg interprets global_quality in lambda units only with QSCALE.
        ctx->global_quality = static_cast<int>(std::clamp<int64_t>(
          static_cast<int64_t>(qp) * ((ctx->flags & AV_CODEC_FLAG_QSCALE) ? FF_QP2LAMBDA : 1), INT_MIN, INT_MAX));
      }
    }
    if (result.single_frame_buffer && ctx->framerate.num > 0 && ctx->framerate.den > 0) {
      ctx->rc_buffer_size = static_cast<int>(std::clamp<int64_t>(
        av_rescale(ctx->bit_rate, ctx->framerate.den, ctx->framerate.num), 0, INT_MAX));
    }

    if (settings.quality != config::vaapi::quality_e::automatic) {
      const auto maximum = caps.quality_range.value_or(0);
      if (maximum > 0 && maximum <= INT_MAX) {
        switch (settings.quality) {
          case config::vaapi::quality_e::speed: ctx->compression_level = maximum; break;
          case config::vaapi::quality_e::balanced: ctx->compression_level = std::max(1u, maximum / 2); break;
          case config::vaapi::quality_e::quality: ctx->compression_level = 1; break;
          default: break;
        }
      } else {
        result.quality_fallback = true;
      }
    }

    if (settings.blbrc.has_value()) {
      const bool available = ctx->priv_data && av_opt_find(ctx->priv_data, "blbrc", nullptr, 0, 0);
      const bool supported = (mask & VA_RC_MB) && selected->mode != config::vaapi::rc_e::cqp;
      result.blbrc_fallback = !available || (*settings.blbrc && !supported);
      if (available) {
        result.blbrc = *settings.blbrc && supported;
        av_dict_set_int(options, "blbrc", *result.blbrc, 0);
      }
    }
    return result;
  }
}
