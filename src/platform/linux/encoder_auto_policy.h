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
   * Vulkan is promoted automatically only on the labwc private compositor,
   * where Polaris can validate a live DMA-BUF frame before relying on that
   * route. Every other route, Gamescope Stream included, counts as an
   * established desktop route and stays on the established encoder until it
   * has the same fail-closed live-frame contract.
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

  /**
   * @brief The sentence Auto gives for a policy.
   *
   * It reaches people as encoder_selection.reason on the system stats route and in session status,
   * which Nova parses, and through the Doctor in the live stream stats, whose encoder selection the
   * web console's Troubleshooting page shows in full as the Selection reason while one stream runs,
   * while every stream has the same selection, or while none runs. The setup report reads only the
   * policy name. A reason opens with what Auto does and gives the why after it. When Auto falls
   * back, finalize_encoder_selection_info in video.cpp puts the fallback ahead of this sentence, so
   * the sentence has to hold when the preferred encoder did not start: it says what Auto prefers,
   * never what is in use.
   *
   * Kept beside decide() so the words and the decision they describe are read and tested
   * together. The policy names are identifiers other surfaces key on; only this text is free.
   *
   * @param policy A policy from decide(), or amd_private_vulkan_not_built.
   * @param driver_known Whether the kernel driver of the selected render node was identified.
   * @param vulkan_built Whether this binary has Vulkan Video, so the text can offer it.
   */
  constexpr std::string_view reason(std::string_view policy, bool driver_known, bool vulkan_built) {
    if (!driver_known) {
      return "Auto could not identify the selected render-node driver; probing available encoders in the established order.";
    }
    if (policy == "amd_private_vulkan_live_probe") {
      return "Auto detected AMD on a private-compositor route; prefer Vulkan Video and verify the exact live GPU-native frame path, with VA-API fallback.";
    }
    if (policy == "amd_private_vulkan_not_built") {
      return "Auto detected AMD, but this build has no Vulkan Video support; prefer VA-API.";
    }
    if (policy == "amd_established_desktop" && !vulkan_built) {
      return "Auto prefers VA-API on AMD outside labwc, the private compositor that Private Stream "
             "runs. This build has no Vulkan Video support.";
    }
    if (policy == "amd_established_desktop") {
      // Every AMD route but labwc lands here, a private gamescope session too, so this cannot call
      // the route a desktop and stop. #635 read that as a Vulkan probe that had failed; none ran.
      // The first sentence is the answer, and it says what Auto prefers: after a fallback it
      // follows the encoder Auto fell back to, so it cannot say VA-API is in use.
      return "Auto prefers VA-API on AMD outside labwc, including Gamescope Stream; Vulkan Video is "
             "not tried. Only labwc, the private compositor that Private Stream runs, checks a live "
             "frame and can fall back if it fails, so Auto treats every other route as an "
             "established desktop route. To use Vulkan Video here, set encoder = vulkan or choose "
             "Vulkan Video for one launch. AV1 is then unavailable, and on portal capture, which "
             "Gamescope Stream uses, frames reach the encoder through system memory.";
    }
    if (policy == "nvidia_nvenc") {
      return "Auto detected NVIDIA; prefer NVENC.";
    }
    if (policy == "nouveau_availability_probe") {
      return "Auto detected Nouveau; probe available encoders because the proprietary NVENC stack is unavailable.";
    }
    if (policy == "intel_vaapi") {
      return "Auto detected Intel; prefer VA-API.";
    }
    return "Auto is probing the encoders available for the selected GPU.";
  }

  /**
   * @brief The reason an explicitly configured encoder gives once it passed validation, when it
   * has more to say than that it passed.
   *
   * Vulkan Video carries no AV1 in this build (NO_AV1 in video.cpp), and on portal capture it
   * takes its frames through system memory by policy. The amd_established_desktop sentence says so
   * to someone who has not chosen it; this says it, with the same qualification, to someone who
   * has, because #635 was reported on encoder = vulkan. It opens with Vulkan Video and its first
   * cost, not the sentence every explicit encoder gives, so the Selection reason on the web
   * console's Troubleshooting page and encoder_selection.reason on the system stats and session
   * status routes lead with what was chosen. Nova's Doctor card never shows it: an explicit encoder
   * that passed is graded pass, and encoder = vulkan never falls back. Every other encoder returns
   * nothing and keeps that sentence.
   *
   * @param encoder The encoder that passed validation.
   */
  constexpr std::string_view explicit_encoder_reason(std::string_view encoder) {
    if (encoder == "vulkan") {
      return "Vulkan Video, configured explicitly, passed runtime validation and offers no AV1 "
             "here; on portal capture, which Gamescope Stream uses, frames reach the encoder "
             "through system memory.";
    }
    return {};
  }

}  // namespace linux_encoder_auto_policy
