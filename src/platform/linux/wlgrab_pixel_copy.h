/**
 * @file src/platform/linux/wlgrab_pixel_copy.h
 * @brief Pixel copy helpers for wlgrab SHM capture.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wl {
  inline void copy_shm_3bpp_rgb_to_bgra(
    const std::uint8_t *src,
    int src_stride,
    std::uint8_t *dst,
    int dst_stride,
    int copy_width,
    int copy_height
  ) {
    const int copy_pixels = std::min(copy_width, std::min(src_stride / 3, dst_stride / 4));
    if (copy_pixels <= 0 || copy_height <= 0) {
      return;
    }

    for (int y = 0; y < copy_height; ++y) {
      const std::uint8_t *src_line = src + y * src_stride;
      std::uint8_t *dst_line = dst + y * dst_stride;
      for (int x = 0; x < copy_pixels; ++x) {
        dst_line[x * 4 + 0] = src_line[x * 3 + 2];
        dst_line[x * 4 + 1] = src_line[x * 3 + 1];
        dst_line[x * 4 + 2] = src_line[x * 3 + 0];
        dst_line[x * 4 + 3] = 255;
      }
    }
  }

  inline void copy_shm_4bpp_to_bgra(
    const std::uint8_t *src,
    int src_stride,
    std::uint8_t *dst,
    int dst_stride,
    int copy_width,
    int copy_height,
    std::uint32_t /* format */
  ) {
    const int row_bytes = copy_width * 4;
    const int copy_bytes = std::min(row_bytes, std::min(src_stride, dst_stride));
    if (copy_bytes <= 0 || copy_height <= 0) {
      return;
    }

    if (src_stride == dst_stride && copy_bytes == src_stride) {
      std::memcpy(dst, src, static_cast<std::size_t>(copy_height) * copy_bytes);
      return;
    }

    for (int y = 0; y < copy_height; ++y) {
      std::memcpy(dst + y * dst_stride, src + y * src_stride, copy_bytes);
    }
  }
}  // namespace wl
