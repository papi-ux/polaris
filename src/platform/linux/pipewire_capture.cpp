/**
 * @file src/platform/linux/pipewire_capture.cpp
 * @brief PipeWire capture format, copy, and frame metadata policy.
 */

#include "src/platform/linux/pipewire_capture.h"

#include <algorithm>
#include <utility>

#include <drm_fourcc.h>
#include <spa/param/video/raw.h>

namespace pipewire_capture {
  std::optional<std::uint32_t> drm_format_for_spa(std::uint32_t spa_format) {
    switch (spa_format) {
      case SPA_VIDEO_FORMAT_BGRx:
        return DRM_FORMAT_XRGB8888;
      case SPA_VIDEO_FORMAT_BGRA:
        return DRM_FORMAT_ARGB8888;
      case SPA_VIDEO_FORMAT_RGBx:
        return DRM_FORMAT_XBGR8888;
      case SPA_VIDEO_FORMAT_RGBA:
        return DRM_FORMAT_ABGR8888;
      default:
        return std::nullopt;
    }
  }

  std::size_t safe_row_copy_bytes(
    std::size_t width_bytes,
    std::int32_t source_stride,
    std::size_t destination_stride
  ) {
    if (source_stride <= 0) {
      return 0;
    }

    return std::min({width_bytes, static_cast<std::size_t>(source_stride), destination_stride});
  }

  platf::frame_metadata_t cpu_frame_metadata() {
    return {
      .transport = platf::frame_transport_e::shm,
      .residency = platf::frame_residency_e::cpu,
      .format = platf::frame_format_e::bgra8,
    };
  }

  platf::frame_metadata_t dmabuf_frame_metadata(std::string render_node) {
    return {
      .transport = platf::frame_transport_e::dmabuf,
      .residency = platf::frame_residency_e::gpu,
      .format = platf::frame_format_e::bgra8,
      .device = std::move(render_node),
    };
  }
}  // namespace pipewire_capture
