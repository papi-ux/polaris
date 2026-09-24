/**
 * @file src/platform/linux/vulkan_encode.h
 * @brief Declarations for FFmpeg Vulkan Video encoder.
 */
#pragma once

#include <array>
#include <string>

#include "src/platform/common.h"

extern "C" struct AVBufferRef;

namespace vk {

  /**
   * @brief Initialize Vulkan hardware device for FFmpeg encoding.
   * @param encode_device The encode device (vk_t).
   * @param hw_device_buf Output hardware device buffer.
   * @return 0 on success, negative on error.
   */
  int vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  /**
   * @brief Create a Vulkan encode device for RAM capture.
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param render_device Exact render node to use, or empty for Vulkan's
   *        normal physical-device selection.
   * @return Constructed AVCodec encode device ram object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_ram(
    int width,
    int height,
    std::string render_device = {}
  );

  /**
   * @brief Create a Vulkan encode device for VRAM capture.
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param offset_x Offset x.
   * @param offset_y Offset y.
   * @return Constructed AVCodec encode device VRAM object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_vram(
    int width,
    int height,
    int offset_x,
    int offset_y,
    std::string render_device
  );

  /**
   * @brief Query the driver's reported maxQualityLevels for each Vulkan Video codec.
   * @details Reads the encode capabilities of the exact physical device backing
   *          the given FFmpeg hardware device context, resolving the query entry
   *          point exactly like FFmpeg does (same instance, same loader function),
   *          so the result matches what FFmpeg will see when it opens a session.
   *          Each codec is queried with its most basic profile (Main, 8-bit) and carries
   *          that codec's own capabilities struct in the pNext chain exactly like FFmpeg
   *          does - the spec requires it and RADV dereferences it unconditionally. A codec whose query
   *          fails or is unsupported reports -1 and simply does not constrain the
   *          quality level; this never aborts an encode session.
   * @param hw_device_buf FFmpeg Vulkan hardware device buffer.
   * @param out Per-codec maxQualityLevels counts (0 - H.264, 1 - HEVC, 2 - AV1);
   *        each entry is the driver's count or -1 when unknown.
   * @return 0 when at least one codec count was read, negative otherwise (all entries are -1).
   */
  int query_vulkan_quality_levels(AVBufferRef *hw_device_buf, std::array<int, 3> &out);

  /**
   * @brief Check if FFmpeg Vulkan Video encoding is available.
   *
   * @return True when FFmpeg Vulkan Video encoding is available.
   */
  bool validate();

}  // namespace vk
