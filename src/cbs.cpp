/**
 * @file src/cbs.cpp
 * @brief Definitions for FFmpeg Coded Bitstream API.
 */
// standard includes
#include <cstring>
#include <iterator>

extern "C" {
// lib includes
#include <libavcodec/avcodec.h>
#include <libavcodec/cbs_h264.h>
#include <libavcodec/cbs_h265.h>
#include <libavcodec/h264_levels.h>
#include <libavutil/pixdesc.h>
}

// local includes
#include "cbs.h"
#include "logging.h"
#include "utility.h"

using namespace std::literals;

namespace cbs {
  void close(CodedBitstreamContext *c) {
    ff_cbs_close(&c);
  }

  using ctx_t = util::safe_ptr<CodedBitstreamContext, close>;

  class frag_t: public CodedBitstreamFragment {
  public:
    frag_t(frag_t &&o) {
      std::copy((std::uint8_t *) &o, (std::uint8_t *) (&o + 1), (std::uint8_t *) this);

      o.data = nullptr;
      o.units = nullptr;
    };

    frag_t() {
      std::fill_n((std::uint8_t *) this, sizeof(*this), 0);
    }

    frag_t &operator=(frag_t &&o) {
      std::copy((std::uint8_t *) &o, (std::uint8_t *) (&o + 1), (std::uint8_t *) this);

      o.data = nullptr;
      o.units = nullptr;

      return *this;
    };

    ~frag_t() {
      if (data || units) {
        ff_cbs_fragment_free(this);
      }
    }
  };

  util::buffer_t<std::uint8_t> write(cbs::ctx_t &cbs_ctx, std::uint8_t nal, void *uh, AVCodecID codec_id) {
    cbs::frag_t frag;
    auto err = ff_cbs_insert_unit_content(&frag, -1, nal, uh, nullptr);
    if (err < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Could not insert NAL unit SPS: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return {};
    }

    err = ff_cbs_write_fragment_data(cbs_ctx.get(), &frag);
    if (err < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Could not write fragment data: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return {};
    }

    // frag.data_size * 8 - frag.data_bit_padding == bits in fragment
    util::buffer_t<std::uint8_t> data {frag.data_size};
    std::copy_n(frag.data, frag.data_size, std::begin(data));

    return data;
  }

  util::buffer_t<std::uint8_t> write(std::uint8_t nal, void *uh, AVCodecID codec_id) {
    cbs::ctx_t cbs_ctx;
    ff_cbs_init(&cbs_ctx, codec_id, nullptr);

    return write(cbs_ctx, nal, uh, codec_id);
  }

  namespace {
    // What reading a frame needs decomposed: its parameter sets, and its slice headers, which is where
    // FFmpeg records the SPS that is active. Every other unit is kept as it came. A VA-API encoder at
    // CBR pads frames with filler data FFmpeg cannot decompose, and one unit it could not read failed
    // the whole packet, so the SPS went unread and the encoder was taken to have no VUI.
    const CodedBitstreamUnitType h264_units_read[] = {
      H264_NAL_SLICE,
      H264_NAL_IDR_SLICE,
      H264_NAL_SPS,
      H264_NAL_PPS,
    };
    const CodedBitstreamUnitType hevc_units_read[] = {
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

    void read_parameter_sets_and_slices(CodedBitstreamContext *ctx, AVCodecID codec_id) {
      if (codec_id == AV_CODEC_ID_H264) {
        ctx->decompose_unit_types = h264_units_read;
        ctx->nb_decompose_unit_types = static_cast<int>(std::size(h264_units_read));
      } else if (codec_id == AV_CODEC_ID_H265) {
        ctx->decompose_unit_types = hevc_units_read;
        ctx->nb_decompose_unit_types = static_cast<int>(std::size(hevc_units_read));
      }
    }
  }  // namespace

  h264_t make_sps_h264(const AVCodecContext *avctx, const AVPacket *packet) {
    cbs::ctx_t ctx;
    if (ff_cbs_init(&ctx, AV_CODEC_ID_H264, nullptr)) {
      return {};
    }
    read_parameter_sets_and_slices(ctx.get(), AV_CODEC_ID_H264);

    cbs::frag_t frag;

    int err = ff_cbs_read_packet(ctx.get(), &frag, packet);
    if (err < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Couldn't read packet: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return {};
    }

    auto sps_p = ((CodedBitstreamH264Context *) ctx->priv_data)->active_sps;
    if (!sps_p) {
      // Only a slice makes an SPS active, and a packet can arrive without one.
      BOOST_LOG(warning) << "No active H.264 SPS in the packet"sv;
      return {};
    }

    // This is a very large struct that cannot safely be stored on the stack
    auto sps = std::make_unique<H264RawSPS>(*sps_p);

    if (avctx->refs > 0) {
      sps->max_num_ref_frames = avctx->refs;
    }

    sps->vui_parameters_present_flag = 1;

    auto &vui = sps->vui;
    std::memset(&vui, 0, sizeof(vui));

    vui.video_format = 5;
    vui.colour_description_present_flag = 1;
    vui.video_signal_type_present_flag = 1;
    vui.video_full_range_flag = avctx->color_range == AVCOL_RANGE_JPEG;
    vui.colour_primaries = avctx->color_primaries;
    vui.transfer_characteristics = avctx->color_trc;
    vui.matrix_coefficients = avctx->colorspace;

    vui.low_delay_hrd_flag = 1 - vui.fixed_frame_rate_flag;

    vui.bitstream_restriction_flag = 1;
    vui.motion_vectors_over_pic_boundaries_flag = 1;
    vui.log2_max_mv_length_horizontal = 16;
    vui.log2_max_mv_length_vertical = 16;
    vui.max_num_reorder_frames = 0;
    vui.max_dec_frame_buffering = sps->max_num_ref_frames;

    cbs::ctx_t write_ctx;
    ff_cbs_init(&write_ctx, AV_CODEC_ID_H264, nullptr);

    return h264_t {
      write(write_ctx, sps->nal_unit_header.nal_unit_type, (void *) &sps->nal_unit_header, AV_CODEC_ID_H264),
      write(ctx, sps_p->nal_unit_header.nal_unit_type, (void *) &sps_p->nal_unit_header, AV_CODEC_ID_H264)
    };
  }

  hevc_t make_sps_hevc(const AVCodecContext *avctx, const AVPacket *packet) {
    cbs::ctx_t ctx;
    if (ff_cbs_init(&ctx, AV_CODEC_ID_H265, nullptr)) {
      return {};
    }
    read_parameter_sets_and_slices(ctx.get(), AV_CODEC_ID_H265);

    cbs::frag_t frag;

    int err = ff_cbs_read_packet(ctx.get(), &frag, packet);
    if (err < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Couldn't read packet: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return {};
    }

    auto vps_p = ((CodedBitstreamH265Context *) ctx->priv_data)->active_vps;
    auto sps_p = ((CodedBitstreamH265Context *) ctx->priv_data)->active_sps;
    if (!vps_p || !sps_p) {
      // Only a slice makes a parameter set active, and a packet can arrive without one.
      BOOST_LOG(warning) << "No active HEVC VPS and SPS in the packet"sv;
      return {};
    }

    // These are very large structs that cannot safely be stored on the stack
    auto sps = std::make_unique<H265RawSPS>(*sps_p);
    auto vps = std::make_unique<H265RawVPS>(*vps_p);

    vps->profile_tier_level.general_profile_compatibility_flag[4] = 1;
    sps->profile_tier_level.general_profile_compatibility_flag[4] = 1;

    auto &vui = sps->vui;
    std::memset(&vui, 0, sizeof(vui));

    sps->vui_parameters_present_flag = 1;

    // skip sample aspect ratio

    vui.video_format = 5;
    vui.colour_description_present_flag = 1;
    vui.video_signal_type_present_flag = 1;
    vui.video_full_range_flag = avctx->color_range == AVCOL_RANGE_JPEG;
    vui.colour_primaries = avctx->color_primaries;
    vui.transfer_characteristics = avctx->color_trc;
    vui.matrix_coefficients = avctx->colorspace;

    vui.vui_timing_info_present_flag = vps->vps_timing_info_present_flag;
    vui.vui_num_units_in_tick = vps->vps_num_units_in_tick;
    vui.vui_time_scale = vps->vps_time_scale;
    vui.vui_poc_proportional_to_timing_flag = vps->vps_poc_proportional_to_timing_flag;
    vui.vui_num_ticks_poc_diff_one_minus1 = vps->vps_num_ticks_poc_diff_one_minus1;
    vui.vui_hrd_parameters_present_flag = 0;

    vui.bitstream_restriction_flag = 1;
    vui.motion_vectors_over_pic_boundaries_flag = 1;
    vui.restricted_ref_pic_lists_flag = 1;
    vui.max_bytes_per_pic_denom = 0;
    vui.max_bits_per_min_cu_denom = 0;
    vui.log2_max_mv_length_horizontal = 15;
    vui.log2_max_mv_length_vertical = 15;

    cbs::ctx_t write_ctx;
    ff_cbs_init(&write_ctx, AV_CODEC_ID_H265, nullptr);

    return hevc_t {
      nal_t {
        write(write_ctx, vps->nal_unit_header.nal_unit_type, (void *) &vps->nal_unit_header, AV_CODEC_ID_H265),
        write(ctx, vps_p->nal_unit_header.nal_unit_type, (void *) &vps_p->nal_unit_header, AV_CODEC_ID_H265),
      },

      nal_t {
        write(write_ctx, sps->nal_unit_header.nal_unit_type, (void *) &sps->nal_unit_header, AV_CODEC_ID_H265),
        write(ctx, sps_p->nal_unit_header.nal_unit_type, (void *) &sps_p->nal_unit_header, AV_CODEC_ID_H265),
      },
    };
  }

  /**
   * This function initializes a Coded Bitstream Context and reads the packet into a Coded Bitstream Fragment.
   * It then checks if the SPS->VUI (Video Usability Information) is present in the active SPS of the packet.
   * This is done for both H264 and H265 codecs.
   */
  bool validate_sps(const AVPacket *packet, int codec_id) {
    cbs::ctx_t ctx;
    if (ff_cbs_init(&ctx, (AVCodecID) codec_id, nullptr)) {
      return false;
    }
    read_parameter_sets_and_slices(ctx.get(), (AVCodecID) codec_id);

    cbs::frag_t frag;

    int err = ff_cbs_read_packet(ctx.get(), &frag, packet);
    if (err < 0) {
      char err_str[AV_ERROR_MAX_STRING_SIZE] {0};
      BOOST_LOG(error) << "Couldn't read packet: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

      return false;
    }

    if (codec_id == AV_CODEC_ID_H264) {
      const auto *sps = ((CodedBitstreamH264Context *) ctx->priv_data)->active_sps;
      return sps && sps->vui_parameters_present_flag;
    }

    const auto *sps = ((CodedBitstreamH265Context *) ctx->priv_data)->active_sps;
    return sps && sps->vui_parameters_present_flag;
  }

  namespace {
    /// Where the next Annex B start code (00 00 01) begins at or after `from`, or `size` if none does.
    std::size_t find_start_code(const std::uint8_t *data, std::size_t from, std::size_t size) {
      while (size >= 3 && from <= size - 3) {
        const auto *one = static_cast<const std::uint8_t *>(std::memchr(data + from + 2, 0x01, size - from - 2));
        if (!one) {
          break;
        }
        const auto at = static_cast<std::size_t>(one - data);
        if (data[at - 1] == 0 && data[at - 2] == 0) {
          return at - 2;
        }
        from = at - 1;
      }
      return size;
    }

    bool is_filler_data(std::uint8_t header, int codec_id) {
      if (codec_id == AV_CODEC_ID_H264) {
        return (header & 0x1f) == H264_NAL_FILLER_DATA;
      }
      return ((header >> 1) & 0x3f) == HEVC_NAL_FD_NUT;
    }
  }  // namespace

  // radeonsi and RADV pad H.264 and HEVC frames in CBR with filler data up to the target bitrate,
  // and FFmpeg has no option to turn it off. A decoder discards filler data, so the client has no
  // use for it.
  //
  // A unit runs from the zero bytes in front of its start code to the zero bytes in front of the
  // next one. A unit never ends in a zero byte, so those zeros are all framing, and removing a unit
  // with them leaves the units on either side exactly as they were. The payload is escaped, so a
  // start code can only mark the start of a unit. A start code with no unit after it goes too.
  //
  // Only units that are kept move, and every one of them counts as something other than filler, so
  // a frame returned whole was never touched.
  std::size_t strip_filler_data(std::uint8_t *data, std::size_t size, int codec_id) {
    if (codec_id != AV_CODEC_ID_H264 && codec_id != AV_CODEC_ID_H265) {
      return size;
    }

    auto code = find_start_code(data, 0, size);
    if (code == size) {
      return size;
    }

    std::size_t begin = 0;
    std::size_t kept = 0;
    bool removed = false;
    bool anything_else = false;
    while (begin < size) {
      const auto header = code + 3;
      const auto next_code = header < size ? find_start_code(data, header, size) : size;
      auto next_begin = next_code;
      while (next_code < size && next_begin > header && data[next_begin - 1] == 0) {
        --next_begin;
      }

      if (header >= size || is_filler_data(data[header], codec_id)) {
        removed = true;
      } else {
        if (kept != begin) {
          std::memmove(data + kept, data + begin, next_begin - begin);
        }
        kept += next_begin - begin;
        anything_else = true;
      }

      begin = next_begin;
      code = next_code;
    }

    // A frame of nothing but filler is left whole rather than sent empty.
    return removed && anything_else ? kept : size;
  }
}  // namespace cbs
