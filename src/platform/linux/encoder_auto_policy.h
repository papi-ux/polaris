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
   * @brief The stream route Auto decides for.
   */
  struct route_t {
    /// labwc, the private compositor that Private Stream runs. It checks a live GPU-native frame
    /// and can retire a failed route to the RAM uploader or to VA-API.
    bool private_compositor_live_probe_available = false;
    /// Gamescope Stream, the private gamescope Polaris runs, captured through the portal. The portal
    /// gives Vulkan Video its frames through the system memory upload, and the encoder probe builds
    /// that same upload, so a probe that passes has run the device the stream uses. Steam Game
    /// Mode's own screen is not this route: while Game Mode runs, the host mirrors its desktop.
    bool gamescope_stream = false;
  };

  /**
   * @brief The route a host is on, from the state its stream mode writes when it is applied or
   *        loaded, and the capture that mode resolves to.
   *
   * @param use_cage_compositor linux_display.use_cage_compositor, which only the labwc modes set.
   * @param private_runtime linux_display.private_runtime, which is "gamescope" for Gamescope
   *        Stream alone. Game Mode's hold swaps the mode to Mirror Desktop and clears it.
   * @param capture The capture setting as the mode fills it and dispatch reads it: unset is portal
   *        on Gamescope Stream (stream_display_policy::capture_filled_for_mode()), and kwin is portal
   *        (canonical_capture_backend()). Gamescope Stream keeps a configured kms, wlr or x11, and
   *        those hand Vulkan Video GPU frames that nothing retires when they fail, so only the portal
   *        makes this the Gamescope Stream route. A per launch switch into it always sets portal.
   */
  constexpr route_t route_of(bool use_cage_compositor, std::string_view private_runtime, std::string_view capture) {
    return {
      .private_compositor_live_probe_available = use_cage_compositor,
      .gamescope_stream = !use_cage_compositor && private_runtime == "gamescope" && capture == "portal",
    };
  }

  /**
   * @brief The host's codec settings, as polaris.conf holds them.
   *
   * Each runs 0 (advertise what the encoder passed), 1 (never), 2 (the 8-bit profile) and
   * 3 (the 8-bit and 10-bit HDR profiles).
   */
  struct codec_settings_t {
    int hevc_mode = 0;
    int av1_mode = 0;
  };

  /**
   * @brief Whether the codec settings ask for what Vulkan Video cannot give on Gamescope Stream.
   *
   * Vulkan Video carries no AV1 (NO_AV1 in video.cpp), so AV1 Support set to always advertise AV1
   * needs another encoder. On Gamescope Stream it reads each frame through the system memory
   * upload, which takes 8-bit BGRA only, so a setting that advertises an HDR profile needs another
   * one too. Both are host settings, fixed between the codecs a client is offered and the stream it
   * starts, so the encoder they choose cannot change under a client that has already picked a codec.
   */
  constexpr bool gamescope_codec_settings_need_vaapi(codec_settings_t codecs) {
    return codecs.av1_mode >= 2 || codecs.hevc_mode == 3;
  }

  /**
   * @brief Choose the encoder family to try first for the selected render node.
   *
   * On AMD, Vulkan Video is tried first on two routes. On the labwc private compositor Polaris
   * validates a live DMA-BUF frame before relying on the route. On Gamescope Stream the probe runs
   * the system memory upload the stream uses, and Auto falls back to VA-API when it fails, but
   * nothing retires Vulkan Video if it passes the probe and fails on the live stream. Every other
   * route counts as an established desktop route and stays on the established encoder until it has
   * a live-frame contract of its own.
   *
   * @param kernel_driver The kernel driver of the selected render node.
   * @param route The stream route, from route_of().
   * @param codecs The host's HEVC and AV1 settings. On Gamescope Stream a setting Vulkan Video
   *        cannot serve keeps VA-API.
   */
  constexpr decision_t decide(
    std::string_view kernel_driver,
    route_t route,
    codec_settings_t codecs
  ) {
    if (kernel_driver == "amdgpu" && route.private_compositor_live_probe_available) {
      return {
        .include_vulkan = true,
        .prefer_vulkan = true,
        .exact_live_probe_required = true,
        .policy = "amd_private_vulkan_live_probe",
        .preferred_encoder = "vulkan",
        .fallback_encoder = "vaapi",
      };
    }

    if (kernel_driver == "amdgpu" && route.gamescope_stream) {
      if (gamescope_codec_settings_need_vaapi(codecs)) {
        return {
          .policy = "amd_gamescope_vaapi_codec_setting",
          .preferred_encoder = "vaapi",
          .fallback_encoder = "next_available",
        };
      }
      // #635: on an RX 9070 XT at 4K60, VA-API took 16 ms a frame on this route and Vulkan Video
      // 9 ms, holding under load. No live probe is asked for, because the probe device is the
      // live one, and none could be reused anyway: only NVENC on labwc reuses a probe.
      return {
        .include_vulkan = true,
        .prefer_vulkan = true,
        .exact_live_probe_required = false,
        .policy = "amd_gamescope_vulkan_ram",
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
   * @brief The decision a build without Vulkan Video makes in place of decide()'s.
   *
   * labwc names the missing build in its own policy, amd_private_vulkan_not_built, as it always has.
   * Gamescope Stream decides as it did before Auto tried Vulkan Video there, amd_established_desktop,
   * whatever the codec settings say: that policy's sentence already says the build has no Vulkan
   * Video, and one build then reports one policy for one outcome, where it reported the labwc policy
   * for one codec setting and the Gamescope Stream codec setting policy for another. Off Gamescope
   * Stream, every decision that does not prefer Vulkan Video is kept.
   */
  constexpr decision_t decision_without_vulkan_video(decision_t decision) {
    if (decision.policy == "amd_gamescope_vulkan_ram" || decision.policy == "amd_gamescope_vaapi_codec_setting") {
      return {
        .policy = "amd_established_desktop",
        .preferred_encoder = "vaapi",
        .fallback_encoder = "next_available",
      };
    }
    if (decision.prefer_vulkan) {
      return {
        .policy = "amd_private_vulkan_not_built",
        .preferred_encoder = "vaapi",
        .fallback_encoder = "next_available",
      };
    }
    return decision;
  }

  /**
   * @brief Whether Auto keeps Vulkan Video among the encoders it probes under a policy.
   *
   * The probe removes Vulkan Video from Auto's candidates under every other policy, so the policies
   * that prefer it are the only ones that can reach it, and a stale encoder cache cannot.
   */
  constexpr bool admits_vulkan(std::string_view policy) {
    return policy == "amd_private_vulkan_live_probe" || policy == "amd_gamescope_vulkan_ram";
  }

  /**
   * @brief Whether Auto offers no HDR through Vulkan Video under a policy.
   *
   * On Gamescope Stream Vulkan Video reads each frame through the system memory upload, which takes
   * 8-bit BGRA only. The portal hands an HDR stream packed 10-bit frames at the same four bytes a
   * pixel, and read as BGRA they are noise. The probe's Main10 pass says nothing about that: it
   * encoded a zeroed 8-bit frame. So the host offers no HDR profile while Vulkan Video is the
   * encoder there, and a client is not invited to start a stream the upload cannot read.
   */
  constexpr bool vulkan_offers_no_hdr(std::string_view policy) {
    return policy == "amd_gamescope_vulkan_ram";
  }

  /**
   * @brief Whether an explicit encoder = vulkan offers no HDR on a route.
   *
   * The same upload as vulkan_offers_no_hdr(): on Gamescope Stream through the portal every frame
   * reaches Vulkan Video through system memory as 8-bit BGRA, whether Auto chose it or polaris.conf
   * did. Unless HEVC Support asks for HDR, the Main10 profile the host would offer comes only from
   * the probe, which encoded a zeroed 8-bit frame, so it is withheld. HEVC Support set to advertise
   * HDR (hevc_mode = 3) is an explicit request and is kept as written: withholding only the dynamic
   * range would let an HDR launch through to a session that is refused, and lowering hevc_mode would
   * override the setting. That HDR stream ends at its first 10-bit frame, with a named reason.
   *
   * @param configured_encoder The encoder setting as polaris.conf holds it.
   * @param route The stream route, from route_of().
   * @param codecs The host's HEVC and AV1 settings.
   */
  constexpr bool explicit_vulkan_offers_no_hdr(std::string_view configured_encoder, route_t route, codec_settings_t codecs) {
    return configured_encoder == "vulkan" && route.gamescope_stream && codecs.hevc_mode != 3;
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
      // Every AMD route but labwc and Gamescope Stream through the portal lands here, so this cannot
      // call the route a desktop and stop. #635 read that as a Vulkan probe that had failed; none
      // ran. Gamescope Stream on another capture lands here too, and must not read that it is
      // outside Gamescope Stream. The first sentence is the answer, and it says what Auto prefers:
      // after a fallback it follows the encoder Auto fell back to, so it cannot say VA-API is in use.
      return "Auto prefers VA-API on AMD outside labwc and Gamescope Stream through the portal; Vulkan "
             "Video is not tried. Auto tries Vulkan Video first only on labwc, the private compositor "
             "that Private Stream runs, which checks a live frame and can fall back if it fails, and on "
             "Gamescope Stream captured through the portal, whose probe runs the same system memory "
             "upload as its stream. Gamescope Stream with capture set to kms, wlr or x11 stays on "
             "VA-API. To use Vulkan Video here, set encoder = vulkan or choose Vulkan Video for one "
             "launch. AV1 is then unavailable, and on portal capture frames reach the encoder through "
             "system memory.";
    }
    if (policy == "amd_gamescope_vulkan_ram") {
      // #635. The first sentence carries the whole answer and both costs, for a surface that shows
      // two lines. After a fallback it follows the encoder Auto fell back to, so it says what Auto
      // prefers, and the costs belong to Vulkan Video, not to the encoder in use.
      return "Auto prefers Vulkan Video on AMD Gamescope Stream, through system memory; AV1 and HDR "
             "are not offered with it. If Vulkan Video fails its probe, Auto uses VA-API. AV1 Support "
             "set to always advertise AV1, or HEVC or AV1 Support set to advertise HDR, keeps VA-API "
             "here, as does encoder = vaapi.";
    }
    if (policy == "amd_gamescope_vaapi_codec_setting" && !vulkan_built) {
      return "Auto prefers VA-API on AMD Gamescope Stream. This build has no Vulkan Video support.";
    }
    if (policy == "amd_gamescope_vaapi_codec_setting") {
      return "Auto prefers VA-API on AMD Gamescope Stream because AV1 Support or HEVC Support asks for "
             "AV1 or HDR, which Vulkan Video does not offer here. When neither setting asks for them, "
             "Auto prefers Vulkan Video on this route, through system memory.";
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
   * takes its frames through system memory by policy, which reads 8-bit frames only. The Auto
   * sentences say so to someone who has not chosen it; this says it to someone who has, because #635
   * was reported on encoder = vulkan. The host still offers HDR with an explicit encoder = vulkan,
   * so this names what an HDR stream through the portal meets: convert() ends it at a 10-bit frame.
   * It opens with Vulkan Video and its first cost, not the sentence every explicit encoder gives,
   * so the Selection reason on the web console's Troubleshooting page and encoder_selection.reason
   * on the system stats and session status routes lead with what was chosen. Nova's Doctor card
   * never shows it: an explicit encoder that passed is graded pass, and encoder = vulkan never
   * falls back. Every other encoder returns nothing and keeps that sentence.
   *
   * @param encoder The encoder that passed validation.
   */
  constexpr std::string_view explicit_encoder_reason(std::string_view encoder, bool hdr_withheld = false) {
    if (encoder == "vulkan" && hdr_withheld) {
      // explicit_vulkan_offers_no_hdr(): the host offers no HDR with it on this route.
      return "Vulkan Video, configured explicitly, passed runtime validation and offers no AV1 "
             "here; on portal capture, which Gamescope Stream uses, frames reach the encoder "
             "through system memory. The upload reads 8-bit frames only, so on Gamescope Stream this "
             "host offers no HDR with it. HEVC Support set to advertise HDR offers it anyway, and an "
             "HDR stream there then ends when the portal hands it a 10-bit frame.";
    }
    if (encoder == "vulkan") {
      return "Vulkan Video, configured explicitly, passed runtime validation and offers no AV1 "
             "here; on portal capture, which Gamescope Stream uses, frames reach the encoder "
             "through system memory. The upload reads 8-bit frames only, so an HDR stream there ends "
             "when the portal hands it a 10-bit frame, as Gamescope Stream does for HDR.";
    }
    return {};
  }

}  // namespace linux_encoder_auto_policy
