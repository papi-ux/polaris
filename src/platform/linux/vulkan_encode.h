/**
 * @file src/platform/linux/vulkan_encode.h
 * @brief Declarations for FFmpeg Vulkan Video encoder.
 */
#pragma once

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
   * @brief Check if FFmpeg Vulkan Video encoding is available.
   *
   * @return True when FFmpeg Vulkan Video encoding is available.
   */
  bool validate();

}  // namespace vk
