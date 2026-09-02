/**
 * @file src/platform/linux/encoder_auto_policy.h
 * @brief Pure Linux encoder auto-selection policy.
 */
#pragma once

#include <string_view>

namespace linux_encoder_auto_policy {

  struct decision_t {
    bool include_vulkan = false;
    bool prefer_vulkan = false;
    bool exact_live_probe_required = false;
    std::string_view policy;
    std::string_view preferred_encoder;
    std::string_view fallback_encoder;
  };

  /**
   * @brief Choose the encoder family to try first for the selected render node.
   *
   * Vulkan is promoted automatically only where Polaris owns the private
   * compositor and can validate a live DMA-BUF frame before relying on that
   * route. Desktop Portal/KMS selection remains on the established encoder
   * until it has the same fail-closed live-frame contract.
   */
  constexpr decision_t decide(
    std::string_view kernel_driver,
    bool private_compositor_live_probe_available
  ) {
    if (kernel_driver == "amdgpu" && private_compositor_live_probe_available) {
      return {
        .include_vulkan = true,
        .prefer_vulkan = true,
        .exact_live_probe_required = true,
        .policy = "amd_private_vulkan_live_probe",
        .preferred_encoder = "vulkan",
        .fallback_encoder = "vaapi",
      };
    }

    if (kernel_driver == "amdgpu") {
      return {
        .policy = "amd_established_desktop",
        .preferred_encoder = "vaapi",
        .fallback_encoder = "next_available",
      };
    }

    if (kernel_driver == "nvidia") {
      return {
        .policy = "nvidia_nvenc",
        .preferred_encoder = "nvenc",
        .fallback_encoder = "next_available",
      };
    }

    // Nouveau does not provide the proprietary NVENC userspace stack. Keep
    // selection capability-driven instead of repeatedly preferring an encoder
    // that cannot initialize on this driver.
    if (kernel_driver == "nouveau") {
      return {
        .policy = "nouveau_availability_probe",
        .preferred_encoder = "automatic",
        .fallback_encoder = "software",
      };
    }

    if (kernel_driver == "i915" || kernel_driver == "xe") {
      return {
        .policy = "intel_vaapi",
        .preferred_encoder = "vaapi",
        .fallback_encoder = "next_available",
      };
    }

    return {
      .policy = "availability_probe",
      .preferred_encoder = "automatic",
      .fallback_encoder = "software",
    };
  }

}  // namespace linux_encoder_auto_policy
