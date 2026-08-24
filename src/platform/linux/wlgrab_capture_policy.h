/**
 * @file src/platform/linux/wlgrab_capture_policy.h
 * @brief Pure policy helpers for selecting the direct wlroots capture path.
 */
#pragma once

#include "src/platform/common.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

  inline std::string enumerated_monitor_identity(
    std::size_t monitor_index,
    std::string_view connector_name
  ) {
    return connector_name.empty() ? std::to_string(monitor_index) : std::string {connector_name};
  }

  inline std::optional<std::size_t> select_monitor_index(
    std::string_view requested_display,
    const std::vector<std::string> &monitor_names
  ) {
    if (monitor_names.empty()) {
      return std::nullopt;
    }
    if (requested_display.empty()) {
      return 0;
    }

    std::size_t requested_index = 0;
    const auto *begin = requested_display.data();
    const auto *end = begin + requested_display.size();
    const auto parsed = std::from_chars(begin, end, requested_index);
    if (parsed.ec == std::errc {} && parsed.ptr == end) {
      return requested_index < monitor_names.size() ?
               std::optional<std::size_t> {requested_index} :
               std::nullopt;
    }

    for (std::size_t index = 0; index < monitor_names.size(); ++index) {
      if (monitor_names[index] == requested_display) {
        return index;
      }
    }
    return std::nullopt;
  }

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
