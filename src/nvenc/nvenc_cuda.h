/**
 * @file src/nvenc/nvenc_cuda.h
 * @brief Native NVENC over a converter-owned Linux CUDA NV12 surface.
 */
#pragma once
#if defined(__linux__) && !defined(__ANDROID__)

#include <cstdint>
#include <limits>

#include "nvenc_base.h"

namespace nvenc {

  // CUDA NV12 has one pitched allocation: the UV plane immediately follows Y.
  // Compare integer device addresses; host pointer arithmetic cannot inspect it.
  inline bool contiguous_nv12_layout(std::uintptr_t y, std::uintptr_t uv,
                                     int pitch, int uv_pitch, int width, int height) {
    if (!y || width <= 0 || height <= 0 || (width & 1) || (height & 1) ||
        pitch < width || uv_pitch != pitch) {
      return false;
    }
    const auto offset = static_cast<std::uint64_t>(pitch) * height;
    return offset <= std::numeric_limits<std::uintptr_t>::max() - y && uv == y + offset;
  }

  /**
   * The caller retains the FFmpeg CUDA context and surface until destruction.
   * Conversion completes its CUDA stream before encode_frame() reads the input.
   * NVENC receives the context as its device; no thread's CUDA context is changed.
   */
  class nvenc_cuda: public nvenc_base {
  public:
    nvenc_cuda(void *cuda_context, void *input, std::uint32_t pitch);
    ~nvenc_cuda() override;

  protected:
    bool init_library() override;
    bool create_and_register_input_buffer() override;

  private:
    void *library = nullptr;
    void *const input;
    const std::uint32_t pitch;
  };

}  // namespace nvenc
#endif
