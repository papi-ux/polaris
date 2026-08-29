/**
 * @file tests/unit/test_stream_display_policy.cpp
 * @brief Test Linux stream display policy user-facing capability contract.
 */

#include <src/platform/linux/stream_display_policy.h>
#include <src/platform/linux/virtual_display.h>
#include <src/config.h>

#include <algorithm>
#include <gtest/gtest.h>

namespace {
  struct LinuxDisplayPolicyGuard {
    LinuxDisplayPolicyGuard():
        headless_mode {config::video.linux_display.headless_mode},
        use_cage_compositor {config::video.linux_display.use_cage_compositor},
        prefer_gpu_native_capture {config::video.linux_display.prefer_gpu_native_capture},
        auto_manage_displays {config::video.linux_display.auto_manage_displays},
        stream_mode {config::video.linux_display.stream_mode},
        private_runtime {config::video.linux_display.private_runtime},
        headless_swap_mode {config::video.linux_display.headless_swap_mode},
        streaming_output {config::video.linux_display.streaming_output},
        primary_output {config::video.linux_display.primary_output},
        capture {config::video.capture},
        output_name {config::video.output_name} {
    }

    ~LinuxDisplayPolicyGuard() {
      config::video.linux_display.headless_mode = headless_mode;
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
      config::video.linux_display.auto_manage_displays = auto_manage_displays;
      config::video.linux_display.stream_mode = stream_mode;
      config::video.linux_display.private_runtime = private_runtime;
      config::video.linux_display.headless_swap_mode = headless_swap_mode;
      config::video.linux_display.streaming_output = streaming_output;
      config::video.linux_display.primary_output = primary_output;
      config::video.capture = capture;
      config::video.output_name = output_name;
    }

    bool headless_mode;
    bool use_cage_compositor;
    bool prefer_gpu_native_capture;
    bool auto_manage_displays;
    std::string stream_mode;
    std::string private_runtime;
    std::string headless_swap_mode;
    std::string streaming_output;
    std::string primary_output;
    std::string capture;
    std::string output_name;
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

TEST(StreamDisplayPolicyTests, LegacyVirtualDisplayLaunchPromotesOnlyWhenTheClientDidNotChoose) {
  using stream_display_policy::effective_session_selection_for_launch;

  EXPECT_EQ(
    effective_session_selection_for_launch("", false, false, true, false),
    "host_virtual_display"
  );
  EXPECT_EQ(
    effective_session_selection_for_launch("", false, true, false, true),
    "host_virtual_display"
  );
  EXPECT_EQ(
    effective_session_selection_for_launch("", false, false, true, true),
    ""
  ) << "an explicit client virtual-display choice must beat the app default";
  EXPECT_EQ(
    effective_session_selection_for_launch("", false, false, true, false, true),
    ""
  ) << "optimizer false must suppress an unlocked legacy app default";
  EXPECT_EQ(
    effective_session_selection_for_launch("", false, true, true, false, true),
    "host_virtual_display"
  );
  EXPECT_EQ(
    effective_session_selection_for_launch("", true, true, true, false),
    "desktop_display"
  ) << "mirrorDesktop must override a host_virtual_display default for this session";
  EXPECT_EQ(
    effective_session_selection_for_launch("headless_stream", false, true, true, true),
    "headless_stream"
  ) << "an explicit accepted streamMode remains authoritative";
  EXPECT_EQ(
    effective_session_selection_for_launch("gamescope_stream", true, false, false, true),
    "desktop_display"
  ) << "explicit desktop mirroring must still beat a stale private streamMode";
  EXPECT_EQ(
    effective_session_selection_for_launch("headless_stream", false, false, true, false),
    "host_virtual_display"
  ) << "an unlocked paired mode must not override the app's display semantic";
}

TEST(StreamDisplayPolicyTests, PrivateAndVirtualModesOwnTheirLaunchRefreshRate) {
  using stream_display_policy::selection_owns_launch_refresh_rate;

  EXPECT_TRUE(selection_owns_launch_refresh_rate("headless_stream"));
  EXPECT_TRUE(selection_owns_launch_refresh_rate("windowed_stream"));
  EXPECT_TRUE(selection_owns_launch_refresh_rate("host_virtual_display"));
  EXPECT_TRUE(selection_owns_launch_refresh_rate("gamescope_stream"));
  EXPECT_FALSE(selection_owns_launch_refresh_rate("desktop_display"));
  EXPECT_FALSE(selection_owns_launch_refresh_rate("headless_dongle"));
}

TEST(StreamDisplayPolicyTests, HostVirtualClearsStaleAutoManage) {
  LinuxDisplayPolicyGuard guard;
  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("host_virtual_display", error)) << error;
  config::video.linux_display.auto_manage_displays = true;
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("host_virtual_display"));
  ASSERT_TRUE(stream_display_policy::apply_selection("host_virtual_display", error)) << error;
  EXPECT_FALSE(config::video.linux_display.auto_manage_displays);
}

TEST(StreamDisplayPolicyTests, VirtualCaptureOutputNameNeverLosesTheCreatedConnector) {
  using stream_display_policy::capture_output_name_for_virtual_display;

  EXPECT_EQ(
    capture_output_name_for_virtual_display("POLARIS-HEADLESS-512536-0", ""),
    "POLARIS-HEADLESS-512536-0"
  );
  EXPECT_EQ(
    capture_output_name_for_virtual_display("POLARIS-HEADLESS-512536-0", "1"),
    "1"
  );
}

TEST(StreamDisplayPolicyTests, HostVirtualCaptureFollowsTheVirtualDisplayBackend) {
  using stream_display_policy::capture_for_host_virtual_display_backend;
  using virtual_display::backend_e;

  for (const auto current : {"", "auto", "portal", "kms"}) {
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend_e::WAYLAND_WLR, current), "wlr")
      << current;
  }

  for (const auto backend : {backend_e::EVDI, backend_e::KSCREEN_DOCTOR}) {
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, ""), "portal");
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, "auto"), "portal");
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, "portal"), "portal");
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, "wlr"), "portal");
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, "kms"), "portal");
  }

  EXPECT_EQ(capture_for_host_virtual_display_backend(backend_e::NONE, "portal"), "portal");
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

TEST(StreamDisplayPolicyTests, ReapplyingHeadlessSelectionClearsStaleCompanionState) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;

  linux_display.stream_mode = "headless_stream";
  linux_display.headless_mode = true;
  linux_display.use_cage_compositor = true;
  linux_display.prefer_gpu_native_capture = true;
  linux_display.private_runtime = "labwc";

  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_stream"));

  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  EXPECT_EQ(linux_display.stream_mode, "headless_stream");
  EXPECT_TRUE(linux_display.headless_mode);
  EXPECT_TRUE(linux_display.use_cage_compositor);
  EXPECT_FALSE(linux_display.prefer_gpu_native_capture);
  EXPECT_TRUE(stream_display_policy::selection_companion_state_matches("headless_stream"));
  linux_display.stream_mode = "HEADLESS_STREAM";
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_stream"));
  linux_display.stream_mode = "headless_stream";

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});
  EXPECT_EQ(resolved.selection, "headless_stream");
}

TEST(StreamDisplayPolicyTests, CommonModeCompanionStateMatchesAllRegisteredSelections) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;
  struct mode_case_t {
    const char *selection;
    const char *runtime;
  };
  const mode_case_t cases[] = {
    {"headless_stream", "labwc"},
    {"windowed_stream", "labwc"},
    {"desktop_display", ""},
    {"host_virtual_display", ""},
  };

  for (const auto &test_case : cases) {
    SCOPED_TRACE(test_case.selection);
    const auto expected = stream_display_policy::legacy_booleans_for_selection(test_case.selection);
    linux_display.stream_mode = test_case.selection;
    linux_display.headless_mode = expected.headless_mode;
    linux_display.use_cage_compositor = expected.use_cage_compositor;
    linux_display.prefer_gpu_native_capture = expected.prefer_gpu_native_capture;
    linux_display.private_runtime = test_case.runtime;
    config::video.capture = test_case.selection == std::string_view {"host_virtual_display"} ?
                              stream_display_policy::capture_for_host_virtual_display_backend(
                                virtual_display::detect_backend(),
                                ""
                              ) :
                              std::string {};
    EXPECT_TRUE(stream_display_policy::selection_companion_state_matches(test_case.selection));

    linux_display.prefer_gpu_native_capture = !expected.prefer_gpu_native_capture;
    EXPECT_FALSE(stream_display_policy::selection_companion_state_matches(test_case.selection));
  }
}

TEST(StreamDisplayPolicyTests, GamescopeCompanionStateIncludesCaptureDefault) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;

  linux_display.stream_mode = "gamescope_stream";
  linux_display.headless_mode = true;
  linux_display.use_cage_compositor = false;
  linux_display.prefer_gpu_native_capture = false;
  linux_display.private_runtime = "gamescope";
  config::video.capture.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("gamescope_stream"));

  config::video.capture = "portal";
  EXPECT_TRUE(stream_display_policy::selection_companion_state_matches("gamescope_stream"));
}

TEST(StreamDisplayPolicyTests, HeadlessDongleCompanionStateIncludesConditionalDefaults) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;

  linux_display.stream_mode = "headless_dongle";
  linux_display.headless_mode = true;
  linux_display.use_cage_compositor = false;
  linux_display.prefer_gpu_native_capture = false;
  linux_display.private_runtime.clear();
  linux_display.auto_manage_displays = true;
  linux_display.headless_swap_mode = "privacy";
  linux_display.streaming_output = "DP-1";
  linux_display.primary_output = "eDP-1";
  config::video.capture = "portal";
  config::video.output_name = "DP-1";
  EXPECT_TRUE(stream_display_policy::selection_companion_state_matches("headless_dongle"));

  linux_display.auto_manage_displays = false;
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.auto_manage_displays = true;
  linux_display.headless_swap_mode.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.headless_swap_mode = "privacy";
  linux_display.streaming_output.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.streaming_output = "DP-1";
  linux_display.primary_output.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.primary_output = "DP-1";
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.primary_output = "eDP-1";
  config::video.capture = "auto";
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  config::video.capture = "portal";
  config::video.output_name.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
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

TEST(StreamDisplayPolicyTests, NormalizeConfigClearsStaleGamescopeRuntime) {
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  for (const auto mode : {"desktop_display", "host_virtual_display", "headless_dongle"}) {
    d.stream_mode = mode;
    d.private_runtime = "gamescope";
    stream_display_policy::normalize_config_from_load();
    EXPECT_TRUE(d.private_runtime.empty()) << mode;
  }
}

TEST(StreamDisplayPolicyTests, NormalizeConfigRepairsHostVirtualState) {
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  d.stream_mode = "host_virtual_display";
  d.auto_manage_displays = true;
  stream_display_policy::normalize_config_from_load();
  EXPECT_FALSE(d.auto_manage_displays);

  d.stream_mode.clear();
  d.headless_mode = true;
  d.use_cage_compositor = false;
  d.auto_manage_displays = true;
  stream_display_policy::normalize_config_from_load();
  EXPECT_EQ(d.stream_mode, "host_virtual_display");
  EXPECT_FALSE(d.auto_manage_displays);
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
