/**
 * @file src/platform/linux/kms_capture_metadata.h
 * @brief Frame metadata helpers for DRM/KMS capture.
 */
#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include <drm_fourcc.h>

#include "src/platform/common.h"

namespace platf::kms_capture {

  /**
   * @brief What a scanout buffer in this DRM format holds, in the terms the encoders read.
   *
   * p010 stands in for a ten bit source the same way the PipeWire path uses it, because the enum has
   * no packed ten bit RGB. Anything else is reported as eight bit BGRA, which is what it was before
   * and what every consumer already expects.
   */
  inline frame_format_e frame_format_for_fourcc(std::uint32_t fourcc) {
    switch (fourcc) {
      case DRM_FORMAT_XRGB2101010:
      case DRM_FORMAT_ARGB2101010:
      case DRM_FORMAT_XBGR2101010:
      case DRM_FORMAT_ABGR2101010:
        return frame_format_e::p010;
      default:
        return frame_format_e::bgra8;
    }
  }

  /**
   * @param format What the consumer of this frame will actually read. Defaulted, because only the
   *        path that hands the buffer over untouched can claim anything but eight bit BGRA: a frame
   *        read back through the GL path arrives as eight bit whatever the scanout was in.
   */
  inline frame_metadata_t frame_metadata(
    bool gpu_resident,
    std::string render_node,
    frame_format_e format = frame_format_e::bgra8
  ) {
    return {
      .transport = frame_transport_e::dmabuf,
      .residency = gpu_resident ? frame_residency_e::gpu : frame_residency_e::cpu,
      .format = format,
      .device = std::move(render_node),
    };
  }

}  // namespace platf::kms_capture
