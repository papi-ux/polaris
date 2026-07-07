/**
 * @file tests/unit/test_stream_display_policy.cpp
 * @brief Test Linux stream display policy user-facing capability contract.
 */

#include <src/platform/linux/stream_display_policy.h>
#include <src/config.h>

#include <gtest/gtest.h>

namespace {
  struct LinuxDisplayPolicyGuard {
    LinuxDisplayPolicyGuard():
        headless_mode {config::video.linux_display.headless_mode},
        use_cage_compositor {config::video.linux_display.use_cage_compositor},
        prefer_gpu_native_capture {config::video.linux_display.prefer_gpu_native_capture} {
    }

    ~LinuxDisplayPolicyGuard() {
      config::video.linux_display.headless_mode = headless_mode;
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
    }

    bool headless_mode;
    bool use_cage_compositor;
    bool prefer_gpu_native_capture;
  };

  void configure_headless_cage(bool prefer_gpu_native_capture) {
    config::video.linux_display.headless_mode = true;
    config::video.linux_display.use_cage_compositor = true;
    config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
  }
}  // namespace

TEST(StreamDisplayPolicyTests, GpuNativePreferenceIsPrimaryStreamMode) {
  LinuxDisplayPolicyGuard guard;
  configure_headless_cage(true);

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});

  EXPECT_EQ(resolved.selection, "windowed_stream");
  EXPECT_EQ(resolved.label, "GPU-Native Stream");
  EXPECT_EQ(resolved.mode, stream_display_policy::mode_e::GPU_NATIVE_STREAM);
  EXPECT_TRUE(resolved.requested_headless);
  EXPECT_TRUE(resolved.use_cage_runtime);
}

TEST(StreamDisplayPolicyTests, EncoderGpuNativeRequirementPromotesCapableHostPath) {
  LinuxDisplayPolicyGuard guard;
  configure_headless_cage(false);

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {
    false,
    true,
    false,
  });

  EXPECT_EQ(resolved.selection, "windowed_stream");
  EXPECT_EQ(resolved.label, "GPU-Native Stream");
  EXPECT_EQ(resolved.reason, "Polaris can force a windowed private compositor when hidden headless capture cannot stay GPU-native.");
}

TEST(StreamDisplayPolicyTests, WindowedCageDefersEncoderProbeUntilRuntimeExists) {
  LinuxDisplayPolicyGuard guard;
  config::video.linux_display.headless_mode = false;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});

  EXPECT_EQ(resolved.selection, "windowed_stream");
  EXPECT_EQ(resolved.label, "Windowed Stream");
  EXPECT_EQ(resolved.mode, stream_display_policy::mode_e::WINDOWED_STREAM);
  EXPECT_FALSE(resolved.requested_headless);
  EXPECT_FALSE(resolved.effective_headless);
  EXPECT_TRUE(resolved.use_cage_runtime);
  EXPECT_TRUE(resolved.should_defer_encoder_probe);
  EXPECT_TRUE(resolved.should_probe_against_runtime);
}
