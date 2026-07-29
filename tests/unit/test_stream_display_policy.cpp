/**
 * @file tests/unit/test_stream_display_policy.cpp
 * @brief Test Linux stream display policy user-facing capability contract.
 */

#include <src/platform/linux/stream_display_policy.h>
#include <src/config.h>

#include <algorithm>
#include <gtest/gtest.h>

namespace {
  struct LinuxDisplayPolicyGuard {
    LinuxDisplayPolicyGuard():
        headless_mode {config::video.linux_display.headless_mode},
        use_cage_compositor {config::video.linux_display.use_cage_compositor},
        prefer_gpu_native_capture {config::video.linux_display.prefer_gpu_native_capture},
        stream_mode {config::video.linux_display.stream_mode},
        private_runtime {config::video.linux_display.private_runtime} {
    }

    ~LinuxDisplayPolicyGuard() {
      config::video.linux_display.headless_mode = headless_mode;
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
      config::video.linux_display.stream_mode = stream_mode;
      config::video.linux_display.private_runtime = private_runtime;
    }

    bool headless_mode;
    bool use_cage_compositor;
    bool prefer_gpu_native_capture;
    std::string stream_mode;
    std::string private_runtime;
  };

  void configure_headless_cage(bool prefer_gpu_native_capture) {
    config::video.linux_display.headless_mode = true;
    config::video.linux_display.use_cage_compositor = true;
    config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
    config::video.linux_display.stream_mode.clear();
    config::video.linux_display.private_runtime = "labwc";
  }
}  // namespace

TEST(StreamDisplayPolicyTests, GpuNativePreferenceLabelsPrivateStreamCaptureCapability) {
  LinuxDisplayPolicyGuard guard;
  configure_headless_cage(true);

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});

  EXPECT_EQ(resolved.selection, "windowed_stream");
  EXPECT_EQ(resolved.label, "Private Stream (GPU-native)");
  // selection string is the mode id (no parallel mode_e).
  EXPECT_TRUE(resolved.prefer_gpu_native_capture);
  EXPECT_TRUE(resolved.requested_headless);
  EXPECT_TRUE(resolved.uses_labwc());
  EXPECT_TRUE(resolved.use_private_runtime);
  EXPECT_EQ(resolved.runtime, stream_path::runtime_kind_e::LABWC);
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
  EXPECT_EQ(resolved.label, "Private Stream (GPU-native)");
  EXPECT_EQ(resolved.reason, "Polaris can force a windowed private compositor when hidden Private Stream capture cannot stay GPU-native.");
}

TEST(StreamDisplayPolicyTests, WindowedCageDefersEncoderProbeUntilRuntimeExists) {
  LinuxDisplayPolicyGuard guard;
  config::video.linux_display.headless_mode = false;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;
  config::video.linux_display.stream_mode.clear();

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});

  EXPECT_EQ(resolved.selection, "windowed_stream");
  EXPECT_EQ(resolved.label, "Private Stream (windowed)");
  EXPECT_FALSE(resolved.requested_headless);
  EXPECT_FALSE(resolved.effective_headless);
  EXPECT_TRUE(resolved.uses_labwc());
  EXPECT_TRUE(resolved.should_defer_encoder_probe);
}

TEST(StreamDisplayPolicyTests, LegacyBooleansMapToSelections) {
  using stream_display_policy::selection_from_legacy_booleans;
  using stream_display_policy::legacy_booleans_t;

  EXPECT_EQ(selection_from_legacy_booleans({true, true, false}), "headless_stream");
  EXPECT_EQ(selection_from_legacy_booleans({true, true, true}), "windowed_stream");
  EXPECT_EQ(selection_from_legacy_booleans({true, false, false}), "host_virtual_display");
  EXPECT_EQ(selection_from_legacy_booleans({false, false, false}), "desktop_display");
}

TEST(StreamDisplayPolicyTests, ApplySelectionSyncsModeAndLegacyBooleans) {
  LinuxDisplayPolicyGuard guard;
  std::string error;

  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  EXPECT_EQ(config::video.linux_display.stream_mode, "headless_stream");
  EXPECT_TRUE(config::video.linux_display.headless_mode);
  EXPECT_TRUE(config::video.linux_display.use_cage_compositor);
  EXPECT_FALSE(config::video.linux_display.prefer_gpu_native_capture);
  EXPECT_EQ(config::video.linux_display.private_runtime, "labwc");

  ASSERT_TRUE(stream_display_policy::apply_selection("desktop_display", error)) << error;
  EXPECT_EQ(config::video.linux_display.stream_mode, "desktop_display");
  EXPECT_FALSE(config::video.linux_display.headless_mode);
  EXPECT_FALSE(config::video.linux_display.use_cage_compositor);
}

TEST(StreamDisplayPolicyTests, GamescopeStreamRegisteredWithGamescopeRuntime) {
  const auto options = stream_display_policy::mode_options(false);
  const auto gamescope = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.value == "gamescope_stream";
  });
  ASSERT_NE(gamescope, options.end());
  EXPECT_EQ(gamescope->runtime, "gamescope");
  EXPECT_EQ(gamescope->capture, "portal");
  // Availability depends on PATH; either way apply must not crash.
  std::string error;
  stream_display_policy::apply_selection("gamescope_stream", error);
}

TEST(StreamDisplayPolicyTests, ExplicitStreamModeWinsOverBooleans) {
  LinuxDisplayPolicyGuard guard;
  config::video.linux_display.stream_mode = "host_virtual_display";
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.use_cage_compositor = true;  // would be private stream if mode empty
  config::video.linux_display.prefer_gpu_native_capture = false;

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {true, false, false});
  EXPECT_EQ(resolved.selection, "host_virtual_display");
  EXPECT_TRUE(resolved.use_host_virtual_display);
  EXPECT_FALSE(resolved.use_private_runtime);
}

TEST(StreamDisplayPolicyTests, NormalizeConfigDerivesStreamModeFromLegacyBooleans) {
  LinuxDisplayPolicyGuard guard;
  config::video.linux_display.stream_mode.clear();
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;
  config::video.linux_display.private_runtime.clear();

  stream_display_policy::normalize_config_from_load();

  EXPECT_EQ(config::video.linux_display.stream_mode, "headless_stream");
  EXPECT_EQ(config::video.linux_display.private_runtime, "labwc");
}

TEST(StreamDisplayPolicyTests, AllowedLaunchModesExcludeUnavailableByDefault) {
  const auto allowed = stream_display_policy::allowed_launch_modes(true, false);
  EXPECT_NE(std::find(allowed.begin(), allowed.end(), "headless_stream"), allowed.end());
  EXPECT_NE(std::find(allowed.begin(), allowed.end(), "host_virtual_display"), allowed.end());
  // gamescope_stream is available when gamescope is on PATH (may or may not be listed).
  // Unwired reserved path ids are not registered.
  EXPECT_EQ(std::find(allowed.begin(), allowed.end(), "family_isolated"), allowed.end());
  EXPECT_EQ(std::find(allowed.begin(), allowed.end(), "headless_evdi"), allowed.end());
}

TEST(StreamDisplayPolicyTests, ModeOptionsMatchSelectionAvailableForGamescope) {
  // Dual-truth footgun: mode_options must apply the same gamescope_present
  // probe as selection_available / apply_selection.
  const auto options = stream_display_policy::mode_options(false);
  const auto gamescope = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.value == "gamescope_stream";
  });
  ASSERT_NE(gamescope, options.end());
  EXPECT_EQ(gamescope->available, stream_display_policy::selection_available("gamescope_stream"));
  if (!gamescope->available) {
    EXPECT_FALSE(gamescope->unavailable_reason.empty());
  }

  const auto allowed = stream_display_policy::allowed_launch_modes(true, false);
  const bool listed = std::find(allowed.begin(), allowed.end(), "gamescope_stream") != allowed.end();
  EXPECT_EQ(listed, gamescope->available);
}

TEST(StreamDisplayPolicyTests, ModeOptionsExposeRuntimeCaptureTopologyForPlugins) {
  const auto options = stream_display_policy::mode_options(false);
  const auto gamescope = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.value == "gamescope_stream";
  });
  ASSERT_NE(gamescope, options.end());
  EXPECT_EQ(gamescope->runtime, "gamescope");
  EXPECT_EQ(gamescope->capture, "portal");

  const auto headless = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.value == "headless_stream";
  });
  ASSERT_NE(headless, options.end());
  EXPECT_EQ(headless->runtime, "labwc");
  EXPECT_EQ(headless->capture, "wlroots");
  EXPECT_TRUE(headless->available);
}

TEST(StreamDisplayPolicyTests, DesktopPathReportsHonestPortalOrHostBackend) {
  LinuxDisplayPolicyGuard guard;
  ASSERT_TRUE([&] {
    std::string error;
    return stream_display_policy::apply_selection("desktop_display", error);
  }());

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});
  EXPECT_EQ(resolved.selection, "desktop_display");
  EXPECT_FALSE(resolved.backend_name.empty());
  EXPECT_NE(resolved.backend_name, "labwc");
}
