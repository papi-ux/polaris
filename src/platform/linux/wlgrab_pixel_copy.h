/**
 * @file src/platform/linux/wlgrab_pixel_copy.h
 * @brief Pixel copy helpers for wlgrab SHM capture.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <drm_fourcc.h>

namespace wl {
  inline bool shm_format_needs_bgra_swap(std::uint32_t format) {
    switch (format) {
      case DRM_FORMAT_XBGR8888:
      case DRM_FORMAT_ABGR8888:
        return true;
      default:
        return false;
    }
  }

  inline void copy_shm_4bpp_to_bgra(
    const std::uint8_t *src,
    int src_stride,
    std::uint8_t *dst,
    int dst_stride,
    int copy_width,
    int copy_height,
    std::uint32_t format
  ) {
    const int copy_bytes = std::min(src_stride, dst_stride);

    if (shm_format_needs_bgra_swap(format)) {
      // wlgrab advertises the software frame as bgra8. These SHM formats
      // arrive in RGB byte order on little-endian hosts, so convert on copy.
      for (int y = 0; y < copy_height; ++y) {
        const std::uint8_t *src_line = src + y * src_stride;
        std::uint8_t *dst_line = dst + y * dst_stride;

        for (int x = 0; x < copy_width; ++x) {
          dst_line[x * 4 + 0] = src_line[x * 4 + 2];
          dst_line[x * 4 + 1] = src_line[x * 4 + 1];
          dst_line[x * 4 + 2] = src_line[x * 4 + 0];
          dst_line[x * 4 + 3] = src_line[x * 4 + 3];
        }
      }

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
