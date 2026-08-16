/**
 * @file src/platform/linux/wlgrab_capture_policy.h
 * @brief Pure policy helpers for selecting the direct wlroots capture path.
 */
#pragma once

#include "src/platform/common.h"

#include <cstdint>
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
    gpu_native_capture_route_e
  ) {
    // A linear modifier describes layout, not whether the VAAPI import and
    // conversion lifetime is safe on a particular driver. RDNA3 field evidence
    // in #367 reproduced the historical first-frame crash after linear headless
    // VAAPI capture was re-enabled, so keep every VAAPI route on SHM until the
    // import boundary has affected-host proof.
    return hwdevice_type == platf::mem_type_e::cuda;
  }

  inline bool gpu_native_dmabuf_is_safe(
    platf::mem_type_e hwdevice_type,
    gpu_native_capture_route_e,
    std::optional<std::uint64_t>
  ) {
    return hwdevice_type == platf::mem_type_e::cuda;
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
