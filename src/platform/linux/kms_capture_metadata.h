/**
 * @file src/platform/linux/kms_capture_metadata.h
 * @brief Frame metadata helpers for DRM/KMS capture.
 */
#pragma once

#include <string>
#include <utility>

#include "src/platform/common.h"

namespace platf::kms_capture {

  inline frame_metadata_t frame_metadata(bool gpu_resident, std::string render_node) {
    return {
      .transport = frame_transport_e::dmabuf,
      .residency = gpu_resident ? frame_residency_e::gpu : frame_residency_e::cpu,
      .format = frame_format_e::bgra8,
      .device = std::move(render_node),
    };
  }

}  // namespace platf::kms_capture
