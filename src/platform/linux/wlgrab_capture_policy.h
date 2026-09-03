/**
 * @file src/platform/linux/wlgrab_capture_policy.h
 * @brief Pure policy helpers for selecting the direct wlroots capture path.
 */
#pragma once

#include "src/capture_generation.h"
#include "src/platform/common.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wlgrab_capture_policy {
  struct generation_policy_t {
    bool owned = false;
    bool use_private_compositor = false;
    std::string adapter_name;
    std::string private_wayland_socket;
    std::string private_runtime_instance_id;
  };

  inline generation_policy_t resolve_generation_policy(
    const capture_generation::identity_t &generation,
    bool global_use_private_compositor,
    std::string_view global_adapter_name
  ) {
    if (!generation.empty()) {
      return {
        .owned = true,
        .use_private_compositor = generation.use_cage_compositor,
        .adapter_name = generation.adapter_name,
        .private_wayland_socket = generation.private_wayland_socket,
        .private_runtime_instance_id = generation.private_runtime_instance_id,
      };
    }
    return {
      .owned = false,
      .use_private_compositor = global_use_private_compositor,
      .adapter_name = std::string {global_adapter_name},
    };
  }

  inline bool private_runtime_matches_generation(
    const generation_policy_t &policy,
    std::string_view live_socket,
    std::string_view live_instance_id
  ) {
    return policy.owned &&
           policy.use_private_compositor &&
           !policy.private_wayland_socket.empty() &&
           !policy.private_runtime_instance_id.empty() &&
           policy.private_wayland_socket == live_socket &&
           policy.private_runtime_instance_id == live_instance_id;
  }

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
    return hwdevice_type == platf::mem_type_e::cuda ||
           hwdevice_type == platf::mem_type_e::vulkan;
  }

  inline bool gpu_native_dmabuf_is_safe(
    platf::mem_type_e hwdevice_type,
    gpu_native_capture_route_e,
    std::optional<std::uint64_t>
  ) {
    return hwdevice_type == platf::mem_type_e::cuda ||
           hwdevice_type == platf::mem_type_e::vulkan;
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
