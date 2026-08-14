/**
 * @file src/platform/linux/wlgrab_capture_policy.h
 * @brief Pure policy helpers for selecting the direct wlroots capture path.
 */
#pragma once

#include "src/platform/common.h"

namespace wlgrab_capture_policy {
  enum class direct_capture_path_e {
    ram,
    gpu_native,
  };

  inline bool gpu_native_dmabuf_is_safe(platf::mem_type_e hwdevice_type) {
    return hwdevice_type == platf::mem_type_e::cuda;
  }

  inline direct_capture_path_e select_direct_capture_path(
    platf::mem_type_e hwdevice_type,
    bool gpu_native_backend_available
  ) {
    if (!gpu_native_backend_available || !gpu_native_dmabuf_is_safe(hwdevice_type)) {
      return direct_capture_path_e::ram;
    }

    return direct_capture_path_e::gpu_native;
  }
}  // namespace wlgrab_capture_policy
