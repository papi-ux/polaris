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

#else

TEST(DefaultRenderDevice, LinuxOnly) {
  GTEST_SKIP() << "Linux-only render-device selection tests";
}

#endif  // __linux__
