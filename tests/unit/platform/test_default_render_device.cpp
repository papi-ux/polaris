/**
 * @file tests/unit/platform/test_default_render_device.cpp
 * @brief The default render-device choice must prefer the GPU games and
 *        capture should land on, not the first node enumerated (issue #367).
 *
 * Node numbering is probe order: on the reported host the headless compositor
 * auto-picked the APU's node while VAAPI encoded on the 7900 XTX, so every
 * frame crossed a CPU copy. choose_default_render_device() is the pure choice
 * over sysfs facts; these tests pin its preference order.
 */
#include <gtest/gtest.h>

#ifdef __linux__

#include "src/config.h"
#include "src/platform/linux/misc.h"
#include "src/platform/linux/encoder_auto_policy.h"
#include "src/platform/linux/stream_display_policy.h"

namespace {

  platf::render_device_candidate_t node(
    std::string path,
    std::string driver,
    long long vram_total_bytes = 0,
    bool boot_vga = false
  ) {
    platf::render_device_candidate_t candidate;
    candidate.path = std::move(path);
    candidate.driver = std::move(driver);
    candidate.vram_total_bytes = vram_total_bytes;
    candidate.boot_vga = boot_vga;
    return candidate;
  }

  constexpr long long operator""_gib(unsigned long long value) {
    return static_cast<long long>(value) << 30;
  }

  constexpr long long operator""_mib(unsigned long long value) {
    return static_cast<long long>(value) << 20;
  }

}  // namespace

TEST(DefaultRenderDevice, EmptyWhenNoCandidates) {
  EXPECT_EQ(platf::choose_default_render_device({}), "");
}

TEST(DefaultRenderDevice, SingleNodeIsChosenUnconditionally) {
  EXPECT_EQ(
    platf::choose_default_render_device({node("/dev/dri/renderD128", "amdgpu", 512_mib)}),
    "/dev/dri/renderD128"
  );
}

TEST(DefaultRenderDevice, DiscreteAmdBeatsApuWhateverTheNodeOrder) {
  // The issue #367 host: APU enumerated first, 7900 XTX second. The dGPU must
  // win even though the APU owns the lower node number and the boot display.
  EXPECT_EQ(
    platf::choose_default_render_device({
      node("/dev/dri/renderD128", "amdgpu", 512_mib, true),
      node("/dev/dri/renderD129", "amdgpu", 24_gib, false),
    }),
    "/dev/dri/renderD129"
  );
}

TEST(DefaultRenderDevice, NvidiaBeatsIntegratedGpuWithoutVramInfo) {
  // The nvidia driver exposes no mem_info_vram_total; being bound by the
  // proprietary driver is itself the discrete signal.
  EXPECT_EQ(
    platf::choose_default_render_device({
      node("/dev/dri/renderD128", "i915", 0, true),
      node("/dev/dri/renderD129", "nvidia", 0, false),
    }),
    "/dev/dri/renderD129"
  );
}

TEST(DefaultRenderDevice, NvidiaBeatsAmdApu) {
  EXPECT_EQ(
    platf::choose_default_render_device({
      node("/dev/dri/renderD128", "amdgpu", 512_mib, true),
      node("/dev/dri/renderD129", "nvidia", 0, false),
    }),
    "/dev/dri/renderD129"
  );
}

TEST(DefaultRenderDevice, LargerVramWinsWithinTheSameRank) {
  EXPECT_EQ(
    platf::choose_default_render_device({
      node("/dev/dri/renderD128", "amdgpu", 8_gib),
      node("/dev/dri/renderD129", "amdgpu", 24_gib),
    }),
    "/dev/dri/renderD129"
  );
}

TEST(DefaultRenderDevice, BootVgaBreaksRemainingTies) {
  EXPECT_EQ(
    platf::choose_default_render_device({
      node("/dev/dri/renderD128", "i915", 0, false),
      node("/dev/dri/renderD129", "i915", 0, true),
    }),
    "/dev/dri/renderD129"
  );
}

TEST(DefaultRenderDevice, NouveauCountsAsDiscreteLikeTheProprietaryDriver) {
  // nouveau only binds NVIDIA discrete GPUs; without this rank an Intel iGPU
  // with boot_vga would win the tie and games would land on the weaker card.
  EXPECT_EQ(
    platf::choose_default_render_device({
      node("/dev/dri/renderD128", "i915", 0, true),
      node("/dev/dri/renderD129", "nouveau", 0, false),
    }),
    "/dev/dri/renderD129"
  );
}

TEST(DefaultRenderDevice, DedicatedMemoryPoolRanksIntelDiscreteAboveIgpu) {
  // Intel discrete parts expose lmem_total_bytes, surfaced through the same
  // vram_total_bytes field by enumeration; the chooser only sees the pool.
  EXPECT_EQ(
    platf::choose_default_render_device({
      node("/dev/dri/renderD128", "i915", 0, true),
      node("/dev/dri/renderD129", "xe", 16_gib, false),
    }),
    "/dev/dri/renderD129"
  );
}

TEST(DefaultRenderDevice, LowestPathIsTheFinalTiebreakForStability) {
  EXPECT_EQ(
    platf::choose_default_render_device({
      node("/dev/dri/renderD129", "amdgpu", 8_gib),
      node("/dev/dri/renderD128", "amdgpu", 8_gib),
    }),
    "/dev/dri/renderD128"
  );
}

namespace {
  using linux_encoder_auto_policy::codec_settings_t;
  using linux_encoder_auto_policy::route_t;

  constexpr route_t labwc {.private_compositor_live_probe_available = true};
  constexpr route_t desktop {};
  constexpr route_t gamescope {.gamescope_stream = true};
  constexpr codec_settings_t defaults {};
}  // namespace

TEST(LinuxEncoderAutoPolicy, NvidiaKeepsNvencWithoutVulkanCandidate) {
  const auto decision = linux_encoder_auto_policy::decide("nvidia", labwc, defaults);
  EXPECT_FALSE(decision.include_vulkan);
  EXPECT_FALSE(decision.prefer_vulkan);
  EXPECT_EQ(decision.preferred_encoder, "nvenc");
  EXPECT_FALSE(decision.exact_live_probe_required);
}

TEST(LinuxEncoderAutoPolicy, NouveauUsesCapabilityProbeWithoutNvencPreference) {
  const auto decision = linux_encoder_auto_policy::decide("nouveau", labwc, defaults);
  EXPECT_FALSE(decision.include_vulkan);
  EXPECT_FALSE(decision.prefer_vulkan);
  EXPECT_EQ(decision.policy, "nouveau_availability_probe");
  EXPECT_EQ(decision.preferred_encoder, "automatic");
  EXPECT_FALSE(decision.exact_live_probe_required);
}

TEST(LinuxEncoderAutoPolicy, IntelKeepsVaapiWithoutVulkanCandidate) {
  const auto decision = linux_encoder_auto_policy::decide("xe", labwc, defaults);
  EXPECT_FALSE(decision.include_vulkan);
  EXPECT_EQ(decision.preferred_encoder, "vaapi");
}

TEST(LinuxEncoderAutoPolicy, AmdPrivateRoutePrefersVulkanWithExactLiveProbe) {
  const auto decision = linux_encoder_auto_policy::decide("amdgpu", labwc, defaults);
  EXPECT_TRUE(decision.include_vulkan);
  EXPECT_TRUE(decision.prefer_vulkan);
  EXPECT_TRUE(decision.exact_live_probe_required);
  EXPECT_EQ(decision.preferred_encoder, "vulkan");
  EXPECT_EQ(decision.fallback_encoder, "vaapi");
}

TEST(LinuxEncoderAutoPolicy, AmdDesktopStaysOnEstablishedBackend) {
  const auto decision = linux_encoder_auto_policy::decide("amdgpu", desktop, defaults);
  EXPECT_FALSE(decision.include_vulkan);
  EXPECT_FALSE(decision.prefer_vulkan);
  EXPECT_EQ(decision.preferred_encoder, "vaapi");
  EXPECT_EQ(decision.policy, "amd_established_desktop");
  EXPECT_EQ(decision.fallback_encoder, "next_available");
}

TEST(LinuxEncoderAutoPolicy, AmdOutsideLabwcSaysVulkanIsNotACandidateAndWhatChoosingItCosts) {
  // #635: a Gamescope Stream host was told it sat on a "desktop capture route" and went looking
  // for a Vulkan probe that had failed. None had run. Auto treats every AMD route but the labwc
  // private compositor and Gamescope Stream as an established desktop route, so the reason has to
  // say Vulkan was never a candidate there, how to ask for it, and what asking costs on that route.
  constexpr auto npos = std::string_view::npos;
  const auto decision = linux_encoder_auto_policy::decide("amdgpu", desktop, defaults);
  const auto reason = linux_encoder_auto_policy::reason(decision.policy, true, true);

  EXPECT_EQ(reason.find("desktop capture route"), npos) << reason;
  EXPECT_NE(reason.find("labwc"), npos) << reason;
  EXPECT_NE(reason.find("Gamescope Stream"), npos) << reason;
  // The answer opens it, as what Auto prefers: after a fallback this sentence follows the encoder
  // Auto fell back to, so it must not say VA-API is in use.
  EXPECT_EQ(
    reason.rfind("Auto prefers VA-API on AMD outside labwc and Gamescope Stream through the portal; Vulkan Video is not tried.", 0),
    0u
  ) << reason;
  EXPECT_EQ(reason.find("uses VA-API"), npos) << reason;
  EXPECT_NE(reason.find("encoder = vulkan"), npos) << reason;
  EXPECT_NE(reason.find("AV1 is then unavailable"), npos) << reason;
  EXPECT_NE(reason.find("on portal capture frames reach the encoder through system memory."), npos) << reason;
  for (const auto dash : {std::string_view {"\xE2\x80\x94"}, std::string_view {"\xE2\x80\x93"}, std::string_view {" - "}}) {
    EXPECT_EQ(reason.find(dash), npos) << reason;
  }

  // The labwc route and the driver-unknown case keep their own sentences.
  EXPECT_NE(
    linux_encoder_auto_policy::reason(linux_encoder_auto_policy::decide("amdgpu", labwc, defaults).policy, true, true).find("prefer Vulkan Video"),
    npos
  );
  EXPECT_NE(linux_encoder_auto_policy::reason(decision.policy, false, true).find("could not identify"), npos);

  // A build without Vulkan Video has nothing to offer, so it must not point at encoder = vulkan.
  const auto without_vulkan = linux_encoder_auto_policy::reason(decision.policy, true, false);
  EXPECT_EQ(without_vulkan.find("Gamescope Stream"), npos) << without_vulkan;
  EXPECT_EQ(without_vulkan.find("encoder = vulkan"), npos) << without_vulkan;
  EXPECT_NE(without_vulkan.find("no Vulkan Video support"), npos) << without_vulkan;
  EXPECT_EQ(without_vulkan.rfind("Auto prefers VA-API on AMD outside labwc", 0), 0u) << without_vulkan;
  EXPECT_EQ(without_vulkan.find("uses VA-API"), npos) << without_vulkan;
  EXPECT_NE(without_vulkan.find("labwc"), npos) << without_vulkan;
  EXPECT_EQ(without_vulkan.find("desktop capture route"), npos) << without_vulkan;
}

TEST(LinuxEncoderAutoPolicy, AGamescopeStreamHostOffThePortalIsToldItsCaptureKeepsVaapi) {
  // Gamescope Stream on kms, wlr or x11 capture decides amd_established_desktop, and its sentence
  // told that host it was "outside labwc and Gamescope Stream" and that Auto tries Vulkan Video
  // first on Gamescope Stream: the #635 failure, a Gamescope Stream host told it is somewhere else.
  // The sentence names the portal as what decides it, and the captures that keep VA-API. The route
  // is read as video.cpp reads it, from the capture as Gamescope Stream fills it.
  constexpr auto npos = std::string_view::npos;
  const auto gamescope_route = [](std::string_view capture) {
    return linux_encoder_auto_policy::route_of(
      false,
      "gamescope",
      stream_display_policy::canonical_capture_backend(
        stream_display_policy::capture_filled_for_mode(stream_display_policy::k_gamescope_stream, capture)
      )
    );
  };
  for (const auto capture : {"kms", "wlr", "x11"}) {
    const auto decision = linux_encoder_auto_policy::decide("amdgpu", gamescope_route(capture), defaults);
    ASSERT_EQ(decision.policy, "amd_established_desktop") << capture;
    const auto reason = linux_encoder_auto_policy::reason(decision.policy, true, true);
    EXPECT_EQ(reason.find("outside labwc and Gamescope Stream;"), npos) << reason;
    EXPECT_NE(reason.find("outside labwc and Gamescope Stream through the portal;"), npos) << reason;
    EXPECT_NE(reason.find("on Gamescope Stream captured through the portal,"), npos) << reason;
    EXPECT_NE(reason.find("Gamescope Stream with capture set to kms, wlr or x11 stays on VA-API."), npos) << reason;
  }

  // Autodetect is not one of them. The settings file's auto loads as an unset capture, which
  // Gamescope Stream fills with the portal, so Auto tries Vulkan Video there, and the sentence for
  // the captures that keep VA-API must not name it.
  std::unordered_map<std::string, std::string> vars {{"capture", "auto"}};
  const auto autodetect = config::capture_setting(vars, {});
  EXPECT_TRUE(autodetect.empty()) << autodetect;
  const auto decision = linux_encoder_auto_policy::decide("amdgpu", gamescope_route(autodetect), defaults);
  EXPECT_EQ(decision.policy, "amd_gamescope_vulkan_ram");
  EXPECT_TRUE(decision.prefer_vulkan);
  const auto kept = linux_encoder_auto_policy::reason("amd_established_desktop", true, true);
  EXPECT_EQ(kept.find("or auto"), npos) << kept;
}

TEST(LinuxEncoderAutoPolicy, ExplicitVulkanReasonSaysWhatTheAutoSentenceSaysItCosts) {
  // #635 was reported on encoder = vulkan, whose reason said only that it passed validation. The
  // explicit reason says what the Auto sentences say choosing Vulkan Video costs, so they cannot
  // drift apart: no AV1, and on the portal, frames through system memory. It opens with Vulkan
  // Video and its first cost instead of the sentence every explicit encoder gives, for the
  // Selection reason on the web console's Troubleshooting page and the system stats route, which
  // show it in full. Nova's Doctor card never shows it: a passing explicit encoder grades pass.
  constexpr auto npos = std::string_view::npos;
  constexpr std::string_view portal_clause =
    "on portal capture, which Gamescope Stream uses, frames reach the encoder through system memory.";
  constexpr std::string_view system_memory = "frames reach the encoder through system memory.";
  const auto explicit_reason = linux_encoder_auto_policy::explicit_encoder_reason("vulkan");
  const auto auto_reason = linux_encoder_auto_policy::reason("amd_established_desktop", true, true);
  const auto gamescope_reason = linux_encoder_auto_policy::reason("amd_gamescope_vulkan_ram", true, true);

  EXPECT_EQ(
    explicit_reason.rfind("Vulkan Video, configured explicitly, passed runtime validation and offers no AV1 here;", 0),
    0u
  ) << explicit_reason;
  EXPECT_NE(explicit_reason.find(portal_clause), npos) << explicit_reason;
  // The Auto Gamescope Stream sentence names HDR among Vulkan Video's costs there, and the explicit
  // reason has to name it too: with encoder = vulkan off Gamescope Stream, or with HEVC Support set
  // to advertise HDR on it, the host still offers HDR, and an HDR stream ends when the portal hands
  // it a 10-bit frame.
  EXPECT_NE(
    explicit_reason.find("The upload reads 8-bit frames only, so an HDR stream there ends when the portal hands it a 10-bit frame"),
    npos
  ) << explicit_reason;
  EXPECT_NE(auto_reason.find(system_memory), npos) << auto_reason;
  EXPECT_NE(auto_reason.find("on portal capture"), npos) << auto_reason;
  EXPECT_NE(auto_reason.find("AV1 is then unavailable"), npos) << auto_reason;
  // Auto on Gamescope Stream states the same two costs, of the encoder it prefers there.
  EXPECT_NE(gamescope_reason.find("Gamescope Stream, through system memory"), npos) << gamescope_reason;
  EXPECT_NE(gamescope_reason.find("AV1 and HDR are not offered with it"), npos) << gamescope_reason;
  for (const auto dash : {std::string_view {"\xE2\x80\x94"}, std::string_view {"\xE2\x80\x93"}, std::string_view {" - "}}) {
    EXPECT_EQ(explicit_reason.find(dash), npos) << explicit_reason;
  }
  for (const auto encoder : {"nvenc", "vaapi", "software", ""}) {
    EXPECT_TRUE(linux_encoder_auto_policy::explicit_encoder_reason(encoder).empty()) << encoder;
  }
}

TEST(LinuxEncoderAutoPolicy, ExplicitVulkanOffersNoHdrOnGamescopeStreamUnlessHevcSupportAsksForIt) {
  // #635. An explicit encoder = vulkan reads Gamescope Stream frames
  // through the same 8-bit system memory upload as Auto's Vulkan Video, and the host offered Main10
  // with it from a probe that encoded a zeroed 8-bit frame. HEVC Support set to advertise HDR is an
  // explicit request and is kept. At 1 or 2 no Main10 is offered anyway, so nothing HDR is left.
  using linux_encoder_auto_policy::explicit_vulkan_offers_no_hdr;
  constexpr auto npos = std::string_view::npos;
  for (const int hevc_mode : {0, 1, 2}) {
    EXPECT_TRUE(explicit_vulkan_offers_no_hdr("vulkan", gamescope, codec_settings_t {.hevc_mode = hevc_mode})) << hevc_mode;
    EXPECT_TRUE(explicit_vulkan_offers_no_hdr("vulkan", gamescope, codec_settings_t {.hevc_mode = hevc_mode, .av1_mode = 3}))
      << hevc_mode;
  }
  EXPECT_FALSE(explicit_vulkan_offers_no_hdr("vulkan", gamescope, codec_settings_t {.hevc_mode = 3}));
  // Gamescope Stream on kms, wlr or x11 hands Vulkan Video GPU frames, not this upload.
  EXPECT_FALSE(explicit_vulkan_offers_no_hdr("vulkan", linux_encoder_auto_policy::route_of(false, "gamescope", "kms"), defaults));
  for (const auto route : {labwc, desktop}) {
    EXPECT_FALSE(explicit_vulkan_offers_no_hdr("vulkan", route, defaults));
  }
  for (const auto encoder : {"vaapi", "nvenc", "software", ""}) {
    EXPECT_FALSE(explicit_vulkan_offers_no_hdr(encoder, gamescope, defaults)) << encoder;
  }

  const auto withheld = linux_encoder_auto_policy::explicit_encoder_reason("vulkan", true);
  EXPECT_EQ(withheld.rfind("Vulkan Video, configured explicitly, passed runtime validation and offers no AV1 here;", 0), 0u)
    << withheld;
  EXPECT_NE(withheld.find("so on Gamescope Stream this host offers no HDR with it."), npos) << withheld;
  EXPECT_NE(withheld.find("HEVC Support set to advertise HDR offers it anyway"), npos) << withheld;
  EXPECT_EQ(withheld.find("so an HDR stream there ends when"), npos) << withheld;
  for (const auto dash : {std::string_view {"\xE2\x80\x94"}, std::string_view {"\xE2\x80\x93"}, std::string_view {" - "}}) {
    EXPECT_EQ(withheld.find(dash), npos) << withheld;
  }
  EXPECT_TRUE(linux_encoder_auto_policy::explicit_encoder_reason("vaapi", true).empty());
}

TEST(LinuxEncoderAutoPolicy, AmdGamescopeStreamPrefersVulkanThroughSystemMemoryWithVaapiFallback) {
  // #635, papi's option (a): on an RX 9070 XT at 4K60 Vulkan Video took 9 ms a frame on this route
  // and VA-API 16 ms. The probe builds the system memory upload the stream uses, so a pass has run
  // the live device, and a failure falls back to VA-API. No exact live probe is asked for.
  for (const codec_settings_t codecs : {
         codec_settings_t {},
         codec_settings_t {.hevc_mode = 1},
         codec_settings_t {.hevc_mode = 2},
         codec_settings_t {.av1_mode = 1},
         codec_settings_t {.hevc_mode = 2, .av1_mode = 1},
       }) {
    const auto decision = linux_encoder_auto_policy::decide("amdgpu", gamescope, codecs);
    const auto label = "hevc_mode=" + std::to_string(codecs.hevc_mode) + " av1_mode=" + std::to_string(codecs.av1_mode);
    EXPECT_EQ(decision.policy, "amd_gamescope_vulkan_ram") << label;
    EXPECT_TRUE(decision.include_vulkan) << label;
    EXPECT_TRUE(decision.prefer_vulkan) << label;
    EXPECT_FALSE(decision.exact_live_probe_required) << label;
    EXPECT_EQ(decision.preferred_encoder, "vulkan") << label;
    EXPECT_EQ(decision.fallback_encoder, "vaapi") << label;
    EXPECT_TRUE(linux_encoder_auto_policy::admits_vulkan(decision.policy)) << label;
    EXPECT_TRUE(linux_encoder_auto_policy::vulkan_offers_no_hdr(decision.policy)) << label;
  }
}

TEST(LinuxEncoderAutoPolicy, AmdGamescopeStreamKeepsVaapiWhenAV1OrHdrIsAskedFor) {
  // Vulkan Video carries no AV1, and on this route it reads frames as 8-bit BGRA, so it offers no
  // HDR. A host that asks for either keeps VA-API, and never probes Vulkan Video for it. These are
  // host settings, fixed between what a client is offered and the stream it starts.
  for (const codec_settings_t codecs : {
         codec_settings_t {.av1_mode = 2},
         codec_settings_t {.av1_mode = 3},
         codec_settings_t {.hevc_mode = 3},
         codec_settings_t {.hevc_mode = 3, .av1_mode = 1},
         codec_settings_t {.hevc_mode = 1, .av1_mode = 2},
       }) {
    const auto decision = linux_encoder_auto_policy::decide("amdgpu", gamescope, codecs);
    const auto label = "hevc_mode=" + std::to_string(codecs.hevc_mode) + " av1_mode=" + std::to_string(codecs.av1_mode);
    EXPECT_EQ(decision.policy, "amd_gamescope_vaapi_codec_setting") << label;
    EXPECT_FALSE(decision.include_vulkan) << label;
    EXPECT_FALSE(decision.prefer_vulkan) << label;
    EXPECT_FALSE(decision.exact_live_probe_required) << label;
    EXPECT_EQ(decision.preferred_encoder, "vaapi") << label;
    EXPECT_EQ(decision.fallback_encoder, "next_available") << label;
    EXPECT_FALSE(linux_encoder_auto_policy::admits_vulkan(decision.policy)) << label;
  }
}

TEST(LinuxEncoderAutoPolicy, OnlyAmdOnGamescopeStreamChangesWhatAutoDecides) {
  // Every driver but amdgpu decides the same on Gamescope Stream as on a desktop route, whatever the
  // codec settings say, and amdgpu keeps its labwc and desktop decisions. The codec settings change
  // nothing off Gamescope Stream: every one of them decides as the defaults do there.
  for (int hevc_mode = 0; hevc_mode <= 3; ++hevc_mode) {
    for (int av1_mode = 0; av1_mode <= 3; ++av1_mode) {
      const codec_settings_t codecs {hevc_mode, av1_mode};
      const auto label = "hevc_mode=" + std::to_string(hevc_mode) + " av1_mode=" + std::to_string(av1_mode);
      for (const auto driver : {"nvidia", "nouveau", "xe", "i915", "radeon", ""}) {
        const auto on_gamescope = linux_encoder_auto_policy::decide(driver, gamescope, codecs);
        const auto on_desktop = linux_encoder_auto_policy::decide(driver, desktop, codecs);
        const auto on_desktop_defaults = linux_encoder_auto_policy::decide(driver, desktop, defaults);
        for (const auto &other : {on_desktop, on_desktop_defaults}) {
          EXPECT_EQ(on_gamescope.policy, other.policy) << driver << ' ' << label;
          EXPECT_EQ(on_gamescope.preferred_encoder, other.preferred_encoder) << driver << ' ' << label;
          EXPECT_EQ(on_gamescope.fallback_encoder, other.fallback_encoder) << driver << ' ' << label;
          EXPECT_EQ(on_gamescope.include_vulkan, other.include_vulkan) << driver << ' ' << label;
        }
        EXPECT_FALSE(on_gamescope.include_vulkan) << driver << ' ' << label;
      }
      for (const auto route : {labwc, desktop}) {
        const auto amd = linux_encoder_auto_policy::decide("amdgpu", route, codecs);
        const auto amd_defaults = linux_encoder_auto_policy::decide("amdgpu", route, defaults);
        EXPECT_EQ(amd.policy, amd_defaults.policy) << label;
        EXPECT_EQ(amd.preferred_encoder, amd_defaults.preferred_encoder) << label;
        EXPECT_EQ(amd.fallback_encoder, amd_defaults.fallback_encoder) << label;
        EXPECT_EQ(amd.include_vulkan, amd_defaults.include_vulkan) << label;
        EXPECT_EQ(amd.prefer_vulkan, amd_defaults.prefer_vulkan) << label;
        EXPECT_EQ(amd.exact_live_probe_required, amd_defaults.exact_live_probe_required) << label;
      }
      const auto amd_labwc = linux_encoder_auto_policy::decide("amdgpu", labwc, codecs);
      EXPECT_EQ(amd_labwc.policy, "amd_private_vulkan_live_probe") << label;
      EXPECT_TRUE(amd_labwc.include_vulkan) << label;
      EXPECT_EQ(amd_labwc.preferred_encoder, "vulkan") << label;
      EXPECT_EQ(amd_labwc.fallback_encoder, "vaapi") << label;
      EXPECT_TRUE(amd_labwc.exact_live_probe_required) << label;
      const auto amd_desktop = linux_encoder_auto_policy::decide("amdgpu", desktop, codecs);
      EXPECT_EQ(amd_desktop.policy, "amd_established_desktop") << label;
      EXPECT_FALSE(amd_desktop.include_vulkan) << label;
      EXPECT_EQ(amd_desktop.preferred_encoder, "vaapi") << label;
      EXPECT_EQ(amd_desktop.fallback_encoder, "next_available") << label;
    }
  }
  EXPECT_EQ(linux_encoder_auto_policy::decide("nvidia", gamescope, defaults).policy, "nvidia_nvenc");
  EXPECT_EQ(linux_encoder_auto_policy::decide("xe", gamescope, defaults).policy, "intel_vaapi");
  EXPECT_EQ(linux_encoder_auto_policy::decide("i915", gamescope, defaults).policy, "intel_vaapi");
}

TEST(LinuxEncoderAutoPolicy, EveryCodecSettingOnAmdGamescopeStreamGetsItsOwnPolicy) {
  // The rule papi chose: Vulkan Video unless AV1 Support always advertises AV1 (2 or 3) or HEVC
  // Support advertises HDR (3), which keep VA-API. The lists above sample it; this holds all sixteen
  // settings to it, so a changed rule fails here and not only in a source search.
  for (int hevc_mode = 0; hevc_mode <= 3; ++hevc_mode) {
    for (int av1_mode = 0; av1_mode <= 3; ++av1_mode) {
      const auto label = "hevc_mode=" + std::to_string(hevc_mode) + " av1_mode=" + std::to_string(av1_mode);
      const bool keeps_vaapi = av1_mode >= 2 || hevc_mode == 3;
      const auto decision = linux_encoder_auto_policy::decide("amdgpu", gamescope, {hevc_mode, av1_mode});
      EXPECT_EQ(decision.policy, keeps_vaapi ? "amd_gamescope_vaapi_codec_setting" : "amd_gamescope_vulkan_ram") << label;
      EXPECT_EQ(decision.preferred_encoder, keeps_vaapi ? "vaapi" : "vulkan") << label;
      EXPECT_EQ(decision.fallback_encoder, keeps_vaapi ? "next_available" : "vaapi") << label;
      EXPECT_EQ(decision.include_vulkan, !keeps_vaapi) << label;
      EXPECT_EQ(decision.prefer_vulkan, !keeps_vaapi) << label;
      EXPECT_FALSE(decision.exact_live_probe_required) << label;
    }
  }
}

TEST(LinuxEncoderAutoPolicy, TheRouteComesFromTheStateTheStreamModeWrites) {
  // labwc sets use_cage_compositor; Gamescope Stream alone writes private_runtime gamescope. Every
  // other mode, and Mirror Desktop while Steam Game Mode's screen is streamed, writes neither.
  const auto labwc_route = linux_encoder_auto_policy::route_of(true, "labwc", "wlr");
  EXPECT_TRUE(labwc_route.private_compositor_live_probe_available);
  EXPECT_FALSE(labwc_route.gamescope_stream);

  const auto gamescope_route = linux_encoder_auto_policy::route_of(false, "gamescope", "portal");
  EXPECT_FALSE(gamescope_route.private_compositor_live_probe_available);
  EXPECT_TRUE(gamescope_route.gamescope_stream);

  const auto desktop_route = linux_encoder_auto_policy::route_of(false, "", "portal");
  EXPECT_FALSE(desktop_route.private_compositor_live_probe_available);
  EXPECT_FALSE(desktop_route.gamescope_stream);

  // A cage host is labwc whatever the runtime string says; it never reads as both routes.
  const auto both = linux_encoder_auto_policy::route_of(true, "gamescope", "portal");
  EXPECT_TRUE(both.private_compositor_live_probe_available);
  EXPECT_FALSE(both.gamescope_stream);
}

TEST(LinuxEncoderAutoPolicy, ABuildWithoutVulkanVideoDecidesGamescopeStreamAsItDidBefore) {
  // Such a build rewrote every decision that preferred Vulkan Video to amd_private_vulkan_not_built,
  // a labwc name, so AMD Gamescope Stream on its default codecs reported that where it used to report
  // amd_established_desktop, and amd_gamescope_vaapi_codec_setting for AV1 or HDR: two policies for
  // one VA-API outcome, neither named in the change that caused them.
  for (int hevc_mode = 0; hevc_mode <= 3; ++hevc_mode) {
    for (int av1_mode = 0; av1_mode <= 3; ++av1_mode) {
      const codec_settings_t codecs {hevc_mode, av1_mode};
      const auto label = "hevc_mode=" + std::to_string(hevc_mode) + " av1_mode=" + std::to_string(av1_mode);
      const auto decision = linux_encoder_auto_policy::decision_without_vulkan_video(
        linux_encoder_auto_policy::decide("amdgpu", gamescope, codecs)
      );
      EXPECT_EQ(decision.policy, "amd_established_desktop") << label;
      EXPECT_EQ(decision.policy, linux_encoder_auto_policy::decide("amdgpu", desktop, codecs).policy) << label;
      EXPECT_FALSE(decision.include_vulkan) << label;
      EXPECT_FALSE(decision.prefer_vulkan) << label;
      EXPECT_FALSE(decision.exact_live_probe_required) << label;
      EXPECT_EQ(decision.preferred_encoder, "vaapi") << label;
      EXPECT_EQ(decision.fallback_encoder, "next_available") << label;
    }
  }
  EXPECT_NE(
    linux_encoder_auto_policy::reason("amd_established_desktop", true, false).find("This build has no Vulkan Video support."),
    std::string_view::npos
  );

  // labwc keeps its own name for the missing build, and nothing that does not prefer Vulkan Video
  // changes.
  const auto labwc_without = linux_encoder_auto_policy::decision_without_vulkan_video(
    linux_encoder_auto_policy::decide("amdgpu", labwc, defaults)
  );
  EXPECT_EQ(labwc_without.policy, "amd_private_vulkan_not_built");
  EXPECT_EQ(labwc_without.preferred_encoder, "vaapi");
  EXPECT_FALSE(labwc_without.include_vulkan);
  for (const auto driver : {"amdgpu", "nvidia", "nouveau", "xe", "i915", ""}) {
    for (const auto route : {labwc, desktop, gamescope}) {
      const auto decision = linux_encoder_auto_policy::decide(driver, route, defaults);
      if (decision.prefer_vulkan) {
        continue;
      }
      EXPECT_EQ(linux_encoder_auto_policy::decision_without_vulkan_video(decision).policy, decision.policy) << driver;
    }
  }
}

TEST(LinuxEncoderAutoPolicy, GamescopeStreamIsTheVulkanRouteOnlyThroughThePortal) {
  // Gamescope Stream keeps a configured kms, wlr or x11 capture. Those hand Vulkan Video GPU frames
  // (kmsgrab builds it the VRAM device), which is the route nothing retires, and the reason's
  // "through system memory" would be false there. The caller passes the capture as the mode fills it,
  // so unset arrives as portal and auto arrives empty.
  for (const auto capture : {"kms", "wlr", "x11", "nvfbc", "", "auto"}) {
    EXPECT_FALSE(linux_encoder_auto_policy::route_of(false, "gamescope", capture).gamescope_stream) << capture;
    EXPECT_EQ(
      linux_encoder_auto_policy::decide("amdgpu", linux_encoder_auto_policy::route_of(false, "gamescope", capture), defaults).policy,
      "amd_established_desktop"
    ) << capture;
  }
  EXPECT_EQ(
    linux_encoder_auto_policy::decide("amdgpu", linux_encoder_auto_policy::route_of(false, "gamescope", "portal"), defaults).policy,
    "amd_gamescope_vulkan_ram"
  );
}

TEST(LinuxEncoderAutoPolicy, TheProbeAdmitsVulkanExactlyWhenTheDecisionIncludesIt) {
  // The probe asks admits_vulkan(policy) at its erase gate and vulkan_offers_no_hdr(policy) before
  // it works out the advertised modes, so both have to agree with decide() on every input.
  for (const auto driver : {"amdgpu", "nvidia", "nouveau", "xe", "i915", ""}) {
    for (const auto route : {labwc, desktop, gamescope}) {
      for (int hevc_mode = 0; hevc_mode <= 3; ++hevc_mode) {
        for (int av1_mode = 0; av1_mode <= 3; ++av1_mode) {
          const auto decision = linux_encoder_auto_policy::decide(driver, route, {hevc_mode, av1_mode});
          EXPECT_EQ(linux_encoder_auto_policy::admits_vulkan(decision.policy), decision.include_vulkan)
            << driver << ' ' << decision.policy;
          EXPECT_EQ(linux_encoder_auto_policy::vulkan_offers_no_hdr(decision.policy),
                    decision.include_vulkan && !decision.exact_live_probe_required)
            << driver << ' ' << decision.policy;
        }
      }
    }
  }
}

TEST(LinuxEncoderAutoPolicy, GamescopeStreamReasonsOpenWithTheAnswerAndItsCosts) {
  // The first sentence is the whole answer, for a surface that shows two lines. After a fallback it
  // follows the encoder Auto fell back to, so it says what Auto prefers, never what is in use.
  constexpr auto npos = std::string_view::npos;
  const auto vulkan = linux_encoder_auto_policy::reason("amd_gamescope_vulkan_ram", true, true);
  EXPECT_EQ(
    vulkan.rfind("Auto prefers Vulkan Video on AMD Gamescope Stream, through system memory; AV1 and HDR are not offered with it.", 0),
    0u
  ) << vulkan;
  EXPECT_NE(vulkan.find("If Vulkan Video fails its probe, Auto uses VA-API."), npos) << vulkan;
  EXPECT_NE(vulkan.find("AV1 Support set to always advertise AV1, or HEVC or AV1 Support set to advertise HDR, keeps VA-API here"), npos) << vulkan;
  EXPECT_NE(vulkan.find("encoder = vaapi"), npos) << vulkan;
  EXPECT_EQ(vulkan.find("uses Vulkan Video"), npos) << vulkan;
  EXPECT_EQ(vulkan.find("desktop"), npos) << vulkan;

  const auto vaapi = linux_encoder_auto_policy::reason("amd_gamescope_vaapi_codec_setting", true, true);
  EXPECT_EQ(
    vaapi.rfind("Auto prefers VA-API on AMD Gamescope Stream because AV1 Support or HEVC Support asks for AV1 or HDR", 0),
    0u
  ) << vaapi;
  EXPECT_NE(vaapi.find("Vulkan Video does not offer here"), npos) << vaapi;
  EXPECT_EQ(vaapi.find("uses VA-API"), npos) << vaapi;

  // A build without Vulkan Video has no Vulkan Video to explain.
  const auto without_vulkan = linux_encoder_auto_policy::reason("amd_gamescope_vaapi_codec_setting", true, false);
  EXPECT_NE(without_vulkan.find("no Vulkan Video support"), npos) << without_vulkan;
  EXPECT_EQ(without_vulkan.find("Auto prefers Vulkan Video"), npos) << without_vulkan;

  for (const auto text : {vulkan, vaapi, without_vulkan}) {
    for (const auto dash : {std::string_view {"\xE2\x80\x94"}, std::string_view {"\xE2\x80\x93"}, std::string_view {" - "}}) {
      EXPECT_EQ(text.find(dash), npos) << text;
    }
  }
}

#else

TEST(DefaultRenderDevice, LinuxOnly) {
  GTEST_SKIP() << "Linux-only render-device selection tests";
}

#endif  // __linux__

TEST(DefaultRenderDevice, VirtualDisplayDriversAreNotEncoderCandidates) {
  // The CachyOS laptop from the freeze thread: i915, nvidia, and a third node from another
  // streaming stack's virtual display module. Two GPUs, not three.
  for (const auto driver : {"evdi", "vkms", "hermes-kms", "hermes_kms", "vibeshine_drm", "udl"}) {
    EXPECT_TRUE(platf::is_virtual_display_driver(driver)) << driver;
  }
  for (const auto driver : {"i915", "xe", "amdgpu", "nvidia", "nouveau", ""}) {
    EXPECT_FALSE(platf::is_virtual_display_driver(driver)) << "'" << driver << "'";
  }

  const auto kept = platf::without_virtual_display_nodes({
    node("/dev/dri/renderD128", "i915", 0, true),
    node("/dev/dri/renderD129", "nvidia", 0),
    node("/dev/dri/renderD130", "hermes-kms", 0),
  });
  ASSERT_EQ(kept.size(), 2u);
  EXPECT_EQ(kept[0].path, "/dev/dri/renderD128");
  EXPECT_EQ(kept[1].path, "/dev/dri/renderD129");
  EXPECT_EQ(platf::choose_default_render_device(kept), "/dev/dri/renderD129");
}
