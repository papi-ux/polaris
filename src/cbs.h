/**
 * @file src/cbs.h
 * @brief Declarations for FFmpeg Coded Bitstream API.
 */
#pragma once

// local includes
#include "utility.h"

struct AVPacket;
struct AVCodecContext;

namespace cbs {

  struct nal_t {
    util::buffer_t<std::uint8_t> _new;
    util::buffer_t<std::uint8_t> old;
  };

  struct hevc_t {
    nal_t vps;
    nal_t sps;
  };

  struct h264_t {
    nal_t sps;
  };

  hevc_t make_sps_hevc(const AVCodecContext *ctx, const AVPacket *packet);
  h264_t make_sps_h264(const AVCodecContext *ctx, const AVPacket *packet);

  /**
   * @brief Validates the Sequence Parameter Set (SPS) of a given packet.
   * @param packet The packet to validate.
   * @param codec_id The ID of the codec used (either AV_CODEC_ID_H264 or AV_CODEC_ID_H265).
   * @return True if the SPS->VUI is present in the active SPS of the packet, false otherwise.
   */
  bool validate_sps(const AVPacket *packet, int codec_id);

  /**
   * @brief Removes the filler data units from an Annex B H.264 or HEVC frame, in place.
   * @param data The frame's bytes.
   * @param size The frame's size in bytes.
   * @param codec_id AV_CODEC_ID_H264 or AV_CODEC_ID_H265; any other codec is left as it is.
   * @return The frame's size without its filler, or `size` when there is none to remove or the
   *         frame holds nothing else.
   */
  std::size_t strip_filler_data(std::uint8_t *data, std::size_t size, int codec_id);
}  // namespace cbs
