/**
 * @file src/nvenc/nvenc_config.h
 * @brief Declarations for NVENC encoder configuration.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstdint>

namespace nvenc {

  /**
   * @brief A VBV buffer grown by nvenc_vbv_increase percent, in 64 bits and held to the encoder's field.
   *
   * The buffer starts as one frame's bits at the stream's bitrate. At the 500 Mbps a client may set by
   * hand, an increase of 258% or more overflowed FFmpeg's int rc_buffer_size at 60 fps, before the
   * division by 100, and wrapped the NVENC SDK's uint32 vbvBufferSize at 30 fps or fewer.
   * @param buffer_bits The buffer before the increase, in bits.
   * @param percentage_increase nvenc_vbv_increase, 0 to 400. One that is not positive leaves the buffer as it is.
   * @param limit The most the encoder's field holds.
   */
  inline std::int64_t grown_vbv_buffer_bits(std::int64_t buffer_bits, int percentage_increase, std::int64_t limit) {
    std::int64_t grown = buffer_bits;
    if (percentage_increase > 0) {
      grown += buffer_bits * percentage_increase / 100;
    }
    return std::clamp<std::int64_t>(grown, 0, limit);
  }

  enum class nvenc_two_pass {
    disabled,  ///< Single pass, the fastest and no extra vram
    quarter_resolution,  ///< Larger motion vectors being caught, faster and uses less extra vram
    full_resolution,  ///< Better overall statistics, slower and uses more extra vram
  };

  enum class nvenc_split_encode_mode {
    disabled,  ///< Disable FFmpeg NVENC split-frame encoding.
    auto_mode,  ///< Let FFmpeg/NVENC select split-frame behavior.
    forced,  ///< Force split-frame encoding when FFmpeg/NVENC supports it.
    two_way,  ///< Use two NVENC engines.
    three_way,  ///< Use three NVENC engines.
  };

  /**
   * @brief NVENC encoder configuration.
   */
  struct nvenc_config {
    // Quality preset from 1 to 7, higher is slower
    int quality_preset = 1;

    // Use optional preliminary pass for better motion vectors, bitrate distribution and stricter VBV(HRD), uses CUDA cores
    nvenc_two_pass two_pass = nvenc_two_pass::quarter_resolution;

    // FFmpeg NVENC split-frame encoding mode for HEVC/AV1 on multi-NVENC Linux GPUs
    nvenc_split_encode_mode split_encode_mode = nvenc_split_encode_mode::disabled;

    // Percentage increase of VBV/HRD from the default single frame, allows low-latency variable bitrate
    int vbv_percentage_increase = 0;

    // Improves fades compression, uses CUDA cores
    bool weighted_prediction = false;

    // Allocate more bitrate to flat regions since they're visually more perceptible, uses CUDA cores
    bool adaptive_quantization = false;

    // Don't use QP below certain value, limits peak image quality to save bitrate
    bool enable_min_qp = false;

    // Min QP value for H.264 when enable_min_qp is selected
    unsigned min_qp_h264 = 19;

    // Min QP value for HEVC when enable_min_qp is selected
    unsigned min_qp_hevc = 23;

    // Min QP value for AV1 when enable_min_qp is selected
    unsigned min_qp_av1 = 23;

    // Use CAVLC entropy coding in H.264 instead of CABAC, not relevant and here for historical reasons
    bool h264_cavlc = false;

    // Add filler data to encoded frames to stay at target bitrate, mainly for testing
    bool insert_filler_data = false;

    // Intra refresh for clients that doesn't request keyframe correctly
    bool intra_refresh = false;
  };

}  // namespace nvenc
