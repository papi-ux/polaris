/**
 * @file src/platform/linux/vulkan_capture_admission.h
 * @brief Metadata admission for the packed RGB DMA-BUF layouts the Vulkan sampler reads.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <drm_fourcc.h>
#include <vulkan/vulkan.h>

namespace vk::capture_admission {
  struct format_t {
    VkFormat format;
    VkComponentMapping swizzle;
    std::uint32_t bytes_per_pixel;
  };

  inline std::optional<format_t> format_info(std::uint32_t fourcc) {
    constexpr VkComponentMapping identity {
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY
    };
    constexpr VkComponentMapping bgr_swap {
      VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
      VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A
    };
    switch (fourcc) {
      case DRM_FORMAT_XRGB8888:
      case DRM_FORMAT_ARGB8888:
        return format_t {VK_FORMAT_B8G8R8A8_UNORM, identity, 4};
      case DRM_FORMAT_XBGR8888:
      case DRM_FORMAT_ABGR8888:
        return format_t {VK_FORMAT_R8G8B8A8_UNORM, identity, 4};
      case DRM_FORMAT_XRGB2101010:
      case DRM_FORMAT_ARGB2101010:
        return format_t {VK_FORMAT_A2R10G10B10_UNORM_PACK32, identity, 4};
      case DRM_FORMAT_XBGR2101010:
      case DRM_FORMAT_ABGR2101010:
        return format_t {VK_FORMAT_A2B10G10R10_UNORM_PACK32, identity, 4};
      case DRM_FORMAT_XBGR16161616:
      case DRM_FORMAT_ABGR16161616:
        return format_t {VK_FORMAT_R16G16B16A16_UNORM, identity, 8};
      case DRM_FORMAT_XRGB16161616:
      case DRM_FORMAT_ARGB16161616:
        return format_t {VK_FORMAT_R16G16B16A16_UNORM, bgr_swap, 8};
      case DRM_FORMAT_XBGR16161616F:
      case DRM_FORMAT_ABGR16161616F:
        return format_t {VK_FORMAT_R16G16B16A16_SFLOAT, identity, 8};
      case DRM_FORMAT_XRGB16161616F:
      case DRM_FORMAT_ARGB16161616F:
        return format_t {VK_FORMAT_R16G16B16A16_SFLOAT, bgr_swap, 8};
      default:
        return std::nullopt;
    }
  }

  // An empty reason admits only the metadata. The caller still queries the
  // exact physical device, modifier, memory planes and external import support.
  inline std::string_view metadata_error(int width, int height, std::uint64_t modifier,
                                        std::span<const int, 4> fds,
                                        std::span<const std::uint32_t, 4> pitches,
                                        std::uint32_t bytes_per_pixel) {
    if (width <= 0 || height <= 0) {
      return "invalid capture dimensions";
    }
    if (modifier == DRM_FORMAT_MOD_INVALID) {
      return "capture has no explicit DRM modifier";
    }
    if (fds[0] < 0) {
      return "capture has no first memory plane";
    }
    bool ended = false;
    for (std::size_t i = 0; i < fds.size(); ++i) {
      if (fds[i] < 0) {
        ended = true;
      } else if (ended) {
        return "capture memory planes are not contiguous";
      } else if (pitches[i] == 0) {
        return "capture memory plane has no row pitch";
      }
    }
    if (modifier == DRM_FORMAT_MOD_LINEAR &&
        static_cast<std::uint64_t>(pitches[0]) < static_cast<std::uint64_t>(width) * bytes_per_pixel) {
      return "linear capture row pitch is shorter than its packed pixels";
    }
    return {};
  }
}  // namespace vk::capture_admission
