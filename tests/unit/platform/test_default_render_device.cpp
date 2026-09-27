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

#include "src/platform/linux/misc.h"
#include "src/platform/linux/encoder_auto_policy.h"

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

TEST(LinuxEncoderAutoPolicy, NvidiaKeepsNvencWithoutVulkanCandidate) {
  const auto decision = linux_encoder_auto_policy::decide("nvidia", true);
  EXPECT_FALSE(decision.include_vulkan);
  EXPECT_FALSE(decision.prefer_vulkan);
  EXPECT_EQ(decision.preferred_encoder, "nvenc");
  EXPECT_FALSE(decision.exact_live_probe_required);
}

TEST(LinuxEncoderAutoPolicy, NouveauUsesCapabilityProbeWithoutNvencPreference) {
  const auto decision = linux_encoder_auto_policy::decide("nouveau", true);
  EXPECT_FALSE(decision.include_vulkan);
  EXPECT_FALSE(decision.prefer_vulkan);
  EXPECT_EQ(decision.policy, "nouveau_availability_probe");
  EXPECT_EQ(decision.preferred_encoder, "automatic");
  EXPECT_FALSE(decision.exact_live_probe_required);
}

TEST(LinuxEncoderAutoPolicy, IntelKeepsVaapiWithoutVulkanCandidate) {
  const auto decision = linux_encoder_auto_policy::decide("xe", true);
  EXPECT_FALSE(decision.include_vulkan);
  EXPECT_EQ(decision.preferred_encoder, "vaapi");
}

TEST(LinuxEncoderAutoPolicy, AmdPrivateRoutePrefersVulkanWithExactLiveProbe) {
  const auto decision = linux_encoder_auto_policy::decide("amdgpu", true);
  EXPECT_TRUE(decision.include_vulkan);
  EXPECT_TRUE(decision.prefer_vulkan);
  EXPECT_TRUE(decision.exact_live_probe_required);
  EXPECT_EQ(decision.preferred_encoder, "vulkan");
  EXPECT_EQ(decision.fallback_encoder, "vaapi");
}

TEST(LinuxEncoderAutoPolicy, AmdDesktopStaysOnEstablishedBackend) {
  const auto decision = linux_encoder_auto_policy::decide("amdgpu", false);
  EXPECT_FALSE(decision.include_vulkan);
  EXPECT_FALSE(decision.prefer_vulkan);
  EXPECT_EQ(decision.preferred_encoder, "vaapi");
  EXPECT_EQ(decision.policy, "amd_established_desktop");
  EXPECT_EQ(decision.fallback_encoder, "next_available");
}

TEST(LinuxEncoderAutoPolicy, AmdOutsideLabwcSaysVulkanIsNotACandidateAndWhatChoosingItCosts) {
  // #635: a Gamescope Stream host was told it sat on a "desktop capture route" and went looking
  // for a Vulkan probe that had failed. None had run. Auto treats every AMD route but the labwc
  // private compositor as an established desktop route, so the reason has to say Vulkan was never
  // a candidate there, how to ask for it, and what asking costs on that route.
  constexpr auto npos = std::string_view::npos;
  const auto decision = linux_encoder_auto_policy::decide("amdgpu", false);
  const auto reason = linux_encoder_auto_policy::reason(decision.policy, true, true);

  EXPECT_EQ(reason.find("desktop capture route"), npos) << reason;
  EXPECT_NE(reason.find("labwc"), npos) << reason;
  EXPECT_NE(reason.find("Gamescope Stream"), npos) << reason;
  // The answer opens it, as what Auto prefers: after a fallback this sentence follows the encoder
  // Auto fell back to, so it must not say VA-API is in use.
  EXPECT_EQ(
    reason.rfind("Auto prefers VA-API on AMD outside labwc, including Gamescope Stream; Vulkan Video is not tried.", 0),
    0u
  ) << reason;
  EXPECT_EQ(reason.find("uses VA-API"), npos) << reason;
  EXPECT_NE(reason.find("encoder = vulkan"), npos) << reason;
  EXPECT_NE(reason.find("AV1 is then unavailable"), npos) << reason;
  EXPECT_NE(reason.find("through system memory"), npos) << reason;
  for (const auto dash : {std::string_view {"\xE2\x80\x94"}, std::string_view {"\xE2\x80\x93"}, std::string_view {" - "}}) {
    EXPECT_EQ(reason.find(dash), npos) << reason;
  }

  // The labwc route and the driver-unknown case keep their own sentences.
  EXPECT_NE(
    linux_encoder_auto_policy::reason(linux_encoder_auto_policy::decide("amdgpu", true).policy, true, true).find("prefer Vulkan Video"),
    npos
  );
  EXPECT_NE(linux_encoder_auto_policy::reason(decision.policy, false, true).find("could not identify"), npos);

  // A build without Vulkan Video has nothing to offer, so it must not point at encoder = vulkan.
  const auto without_vulkan = linux_encoder_auto_policy::reason(decision.policy, true, false);
  EXPECT_EQ(without_vulkan.find("encoder = vulkan"), npos) << without_vulkan;
  EXPECT_NE(without_vulkan.find("no Vulkan Video support"), npos) << without_vulkan;
  EXPECT_EQ(without_vulkan.rfind("Auto prefers VA-API on AMD outside labwc", 0), 0u) << without_vulkan;
  EXPECT_EQ(without_vulkan.find("uses VA-API"), npos) << without_vulkan;
  EXPECT_NE(without_vulkan.find("labwc"), npos) << without_vulkan;
  EXPECT_EQ(without_vulkan.find("desktop capture route"), npos) << without_vulkan;
}

TEST(LinuxEncoderAutoPolicy, ExplicitVulkanReasonSaysWhatTheAutoSentenceSaysItCosts) {
  // #635 was reported on encoder = vulkan, whose reason said only that it passed validation. The
  // explicit reason says what the Auto sentence says choosing Vulkan Video costs, with the same
  // qualification, so the two cannot drift apart. It opens with Vulkan Video and its first cost
  // instead of the sentence every explicit encoder gives, for the Selection reason on the web
  // console's Troubleshooting page and the system stats route, which show it in full. Nova's Doctor
  // card never shows it: a passing explicit encoder grades pass.
  constexpr auto npos = std::string_view::npos;
  constexpr std::string_view portal_clause =
    "on portal capture, which Gamescope Stream uses, frames reach the encoder through system memory.";
  const auto explicit_reason = linux_encoder_auto_policy::explicit_encoder_reason("vulkan");
  const auto auto_reason = linux_encoder_auto_policy::reason("amd_established_desktop", true, true);

  EXPECT_EQ(
    explicit_reason.rfind("Vulkan Video, configured explicitly, passed runtime validation and offers no AV1 here;", 0),
    0u
  ) << explicit_reason;
  EXPECT_NE(explicit_reason.find(portal_clause), npos) << explicit_reason;
  EXPECT_NE(auto_reason.find(portal_clause), npos) << auto_reason;
  for (const auto dash : {std::string_view {"\xE2\x80\x94"}, std::string_view {"\xE2\x80\x93"}, std::string_view {" - "}}) {
    EXPECT_EQ(explicit_reason.find(dash), npos) << explicit_reason;
  }
  for (const auto encoder : {"nvenc", "vaapi", "software", ""}) {
    EXPECT_TRUE(linux_encoder_auto_policy::explicit_encoder_reason(encoder).empty()) << encoder;
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
