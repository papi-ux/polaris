/**
 * @file src/platform/linux/wlgrab_capture_policy.h
 * @brief Pure policy helpers for selecting the direct wlroots capture path.
 */
#pragma once

#include "src/platform/common.h"

#include <cstdint>
#include <drm_fourcc.h>
#include <optional>

namespace wlgrab_capture_policy {
  enum class gpu_native_capture_route_e {
    headless_extcopy,
    windowed_nested,
    direct_wayland,
  };

  enum class direct_capture_path_e {
    ram,
    gpu_native,
  };

  inline bool gpu_native_dmabuf_probe_is_allowed(
    platf::mem_type_e hwdevice_type,
    gpu_native_capture_route_e route
  ) {
    if (hwdevice_type == platf::mem_type_e::cuda) {
      return true;
    }

    return hwdevice_type == platf::mem_type_e::vaapi &&
           route == gpu_native_capture_route_e::headless_extcopy;
  }

  inline bool gpu_native_dmabuf_is_safe(
    platf::mem_type_e hwdevice_type,
    gpu_native_capture_route_e route,
    std::optional<std::uint64_t> modifier
  ) {
    if (hwdevice_type == platf::mem_type_e::cuda) {
      return true;
    }

    return hwdevice_type == platf::mem_type_e::vaapi &&
           route == gpu_native_capture_route_e::headless_extcopy &&
           modifier == DRM_FORMAT_MOD_LINEAR;
  }

  inline direct_capture_path_e select_direct_capture_path(
    platf::mem_type_e hwdevice_type,
    bool gpu_native_backend_available
  ) {
    if (!gpu_native_backend_available ||
        !gpu_native_dmabuf_is_safe(
          hwdevice_type,
          gpu_native_capture_route_e::direct_wayland,
          std::nullopt
        )) {
      return direct_capture_path_e::ram;
    }

    return direct_capture_path_e::gpu_native;
  }
}  // namespace wlgrab_capture_policy
