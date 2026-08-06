/**
 * @file tests/unit/platform/test_cage_display_router.cpp
 * @brief Test Linux labwc runtime policy helpers.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/cage_display_router.h>
  #include <src/platform/linux/wayland.h>

TEST(WaylandOutputRegistryStateTests, OutputHotplugMarksTopologyDirtyUntilCleared) {
  wl::output_registry_state_t state;

  state.add_output(10);
  EXPECT_TRUE(state.output_topology_dirty());

  state.clear_output_topology_dirty();
  EXPECT_FALSE(state.output_topology_dirty());

  state.remove_global(10);
  EXPECT_TRUE(state.output_topology_dirty());
  EXPECT_FALSE(state.has_output(10));
}

TEST(WaylandOutputRegistryStateTests, RemovingNonOutputGlobalDoesNotDirtyOutputTopology) {
  wl::output_registry_state_t state;

  state.add_output(10);
  state.clear_output_topology_dirty();

  state.remove_global(99);

  EXPECT_FALSE(state.output_topology_dirty());
  EXPECT_TRUE(state.has_output(10));
}

#ifdef POLARIS_TESTS
TEST(WaylandInterfaceTests, RemovedOutputMarksDirtyButKeepsMonitorStorageUntilReinit) {
  wl::interface_t interface;

  interface.add_monitor_for_tests(57);
  EXPECT_TRUE(interface.consume_output_topology_dirty());
  ASSERT_EQ(interface.monitors.size(), 1u);

  interface.remove_global_for_tests(57);

  EXPECT_TRUE(interface.consume_output_topology_dirty());
  EXPECT_EQ(interface.monitors.size(), 1u);
}
#endif

// Trivial AND/negation helpers (windowed_gpu_native_probe, gpu_native_cage_capture,
// windowed_ram_fallback, headless_extcopy_probe_succeeded) live only under
// stream_runtime::labwc as one-liners — no dual cage export / matrix tests.

TEST(CageDisplayRouterPolicyTests, EffectiveHeadlessVaapiSkipsExtcopyDmabufForShmFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::vaapi
  ));
}

TEST(CageDisplayRouterPolicyTests, EffectiveHeadlessCudaCanAttemptExtcopyDmabuf) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::cuda
  ));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideDoesNotAttemptHeadlessExtcopyDmabuf) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::cuda
  ));
}

TEST(CageDisplayRouterPolicyTests, GpuNativeCaptureNeedsTheOverride) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state));
}

TEST(CageDisplayRouterPolicyTests, TheTwoGpuNativeRoutesContainFailureDifferently) {
  // Deliberately asymmetric, and the asymmetry is the point. The headless
  // ext-image-copy route refuses VAAPI by encoder type up front; the windowed
  // override lets it run and retires the path only after a real conversion
  // failure, because the surface-lifetime fix is what makes it viable and a
  // blanket refusal would keep that fix from ever executing.
  const platf::runtime_state_t headless {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };
  const platf::runtime_state_t overridden {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_attempt_headless_extcopy_dmabuf(headless, platf::mem_type_e::vaapi));
  EXPECT_TRUE(cage_display_router::should_attempt_gpu_native_cage_capture(overridden));
}

TEST(CageDisplayRouterPolicyTests, OnlyCudaHasASafeGpuNativeDmabufPath) {
  EXPECT_TRUE(cage_display_router::gpu_native_dmabuf_is_safe(platf::mem_type_e::cuda));
  EXPECT_FALSE(cage_display_router::gpu_native_dmabuf_is_safe(platf::mem_type_e::vaapi));
  EXPECT_FALSE(cage_display_router::gpu_native_dmabuf_is_safe(platf::mem_type_e::system));
  // An encoder that has not resolved yet must not read as safe.
  EXPECT_FALSE(cage_display_router::gpu_native_dmabuf_is_safe(platf::mem_type_e::unknown));
}

TEST(CageDisplayRouterPolicyTests, HeadlessDmabufGpuConversionFailureDisablesExtcopyFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };
  const platf::frame_metadata_t metadata {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
    .format = platf::frame_format_e::bgra8,
  };

  EXPECT_TRUE(cage_display_router::should_disable_headless_extcopy_after_conversion_failure(
    runtime_state,
    metadata
  ));
}

TEST(CageDisplayRouterPolicyTests, NonHeadlessDmabufConversionFailureDoesNotDisableExtcopyFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };
  const platf::frame_metadata_t metadata {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
    .format = platf::frame_format_e::bgra8,
  };

  EXPECT_FALSE(cage_display_router::should_disable_headless_extcopy_after_conversion_failure(
    runtime_state,
    metadata
  ));
}

TEST(CageDisplayRouterPolicyTests, CpuFrameConversionFailureDoesNotDisableExtcopyFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };
  const platf::frame_metadata_t metadata {
    .transport = platf::frame_transport_e::shm,
    .residency = platf::frame_residency_e::cpu,
    .format = platf::frame_format_e::bgra8,
  };

  EXPECT_FALSE(cage_display_router::should_disable_headless_extcopy_after_conversion_failure(
    runtime_state,
    metadata
  ));
}

TEST(CageDisplayRouterPolicyTests, RequestedHeadlessWithoutOverrideStillReportsHeadlessFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_report_headless_ram_capture_fallback(runtime_state));
}

TEST(CageDisplayRouterPolicyTests, WindowedOverrideDoesNotReportHeadlessFallback) {
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = false,
    .gpu_native_override_active = true,
    .backend_name = "labwc",
  };

  EXPECT_FALSE(cage_display_router::should_report_headless_ram_capture_fallback(runtime_state));
}

#ifdef POLARIS_TESTS
TEST(WaylandDrmDeviceTests, InvalidDeviceDoesNotFabricateRenderNode) {
  EXPECT_TRUE(wl::render_node_from_drm_device_for_tests(dev_t {}).empty());
}

TEST(CageDisplayRouterPolicyTests, WindowedRamCaptureFallbackWarningLogsOnlyOnce) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_TRUE(cage_display_router::should_log_windowed_ram_capture_warning());
  EXPECT_FALSE(cage_display_router::should_log_windowed_ram_capture_warning());
}

TEST(CageDisplayRouterPolicyTests, HeadlessRamCaptureFallbackWarningLogsOnlyOnce) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_TRUE(cage_display_router::should_log_headless_ram_capture_warning());
  EXPECT_FALSE(cage_display_router::should_log_headless_ram_capture_warning());
}

TEST(CageDisplayRouterPolicyTests, WindowedGpuNativeProbeResultDefaultsToUnknown) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_FALSE(cage_display_router::cached_windowed_gpu_native_probe_result().has_value());
}

TEST(CageDisplayRouterPolicyTests, WindowedGpuNativeProbeResultCachesFailure) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_windowed_gpu_native_probe_result(false);

  EXPECT_EQ(cage_display_router::cached_windowed_gpu_native_probe_result(), std::optional<bool> {false});
}

TEST(CageDisplayRouterPolicyTests, WindowedGpuNativeProbeResultCachesSuccess) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_windowed_gpu_native_probe_result(true);

  EXPECT_EQ(cage_display_router::cached_windowed_gpu_native_probe_result(), std::optional<bool> {true});
}

TEST(CageDisplayRouterPolicyTests, HeadlessExtcopyDmabufProbeResultDefaultsToUnknown) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  EXPECT_FALSE(cage_display_router::cached_headless_extcopy_dmabuf_probe_result().has_value());
}

TEST(CageDisplayRouterPolicyTests, HeadlessExtcopyDmabufProbeResultCachesFailure) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);

  EXPECT_EQ(cage_display_router::cached_headless_extcopy_dmabuf_probe_result(), std::optional<bool> {false});
}

TEST(CageDisplayRouterPolicyTests, HeadlessExtcopyDmabufProbeResultCachesSuccess) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();

  cage_display_router::update_headless_extcopy_dmabuf_probe_result(true);

  EXPECT_EQ(cage_display_router::cached_headless_extcopy_dmabuf_probe_result(), std::optional<bool> {true});
}

TEST(CageDisplayRouterPolicyTests, HeadlessExtcopyDmabufProbeFailureSuppressesRetry) {
  cage_display_router::reset_windowed_ram_capture_warning_for_tests();
  const platf::runtime_state_t runtime_state {
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
  };

  EXPECT_TRUE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::cuda
  ));

  cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);

  EXPECT_FALSE(cage_display_router::should_attempt_headless_extcopy_dmabuf(
    runtime_state,
    platf::mem_type_e::cuda
  ));
}

TEST(CageDisplayRouterPolicyTests, MangoHudPrefixIsSuppressedForSteamBigPicture) {
  EXPECT_TRUE(cage_display_router::mangohud_prefix_for_command_for_tests(
    "steam -gamepadui",
    true,
    "1",
    "fps_limit=60"
  ).empty());

  EXPECT_TRUE(cage_display_router::mangohud_prefix_for_command_for_tests(
    "setsid steam steam://open/bigpicture",
    true,
    "1",
    "fps_limit=60"
  ).empty());
}

TEST(CageDisplayRouterPolicyTests, MangoHudPrefixStillAppliesToRegularGames) {
  EXPECT_EQ(
    cage_display_router::mangohud_prefix_for_command_for_tests(
      "steam steam://rungameid/12345",
      true,
      "1",
      "fps_limit=60"
    ),
    "MANGOHUD=1 MANGOHUD_DLSYM=1 MANGOHUD_CONFIG=fps_limit=60 "
  );
}

TEST(CageDisplayRouterPolicyTests, LabwcProcessEnvironmentDisablesHardwareCursors) {
  EXPECT_EQ(
    cage_display_router::labwc_process_environment_value_for_tests(
      true,
      "WLR_NO_HARDWARE_CURSORS"
    ),
    "1"
  );
  EXPECT_EQ(
    cage_display_router::labwc_process_environment_value_for_tests(
      false,
      "WLR_NO_HARDWARE_CURSORS"
    ),
    "1"
  );
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRecognizesRequestedCurrentMode) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1280x720 px, 60.000000 Hz
    1920x1080 px, 60.000000 Hz (current)
)";

  EXPECT_TRUE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080
  ));
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRecognizesRequestedCurrentRefresh) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1920x1080 px, 60.000000 Hz
    1920x1080 px, 120.000000 Hz (current)
)";

  EXPECT_TRUE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080,
    120
  ));
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRejectsWrongCurrentRefresh) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1920x1080 px, 60.000000 Hz (current)
    1920x1080 px, 120.000000 Hz
)";

  EXPECT_FALSE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080,
    120
  ));
}

TEST(CageDisplayRouterPolicyTests, OutputModeParserRejectsNonCurrentRequestedMode) {
  constexpr std::string_view output = R"(HEADLESS-1 "Headless output 1"
  Enabled: yes
  Modes:
    1280x720 px, 60.000000 Hz (current)
    1920x1080 px, 60.000000 Hz
)";

  EXPECT_FALSE(cage_display_router::output_reports_current_mode_for_tests(
    output,
    "HEADLESS-1",
    1920,
    1080
  ));
}

TEST(CageDisplayRouterPolicyTests, FormatWlrCustomModeIncludesRefresh) {
  EXPECT_EQ(
    cage_display_router::format_wlr_custom_mode_for_tests(1920, 1080, 120),
    "1920x1080@120Hz"
  );
}

TEST(CageDisplayRouterPolicyTests, WaylandSocketNameParserAcceptsNumberedSocketsOnly) {
  EXPECT_TRUE(cage_display_router::is_wayland_socket_name_for_tests("wayland-0"));
  EXPECT_TRUE(cage_display_router::is_wayland_socket_name_for_tests("wayland-50"));
  EXPECT_TRUE(cage_display_router::is_wayland_socket_name_for_tests("wayland-123"));

  EXPECT_FALSE(cage_display_router::is_wayland_socket_name_for_tests("wayland-"));
  EXPECT_FALSE(cage_display_router::is_wayland_socket_name_for_tests("wayland-1.lock"));
  EXPECT_FALSE(cage_display_router::is_wayland_socket_name_for_tests("pipewire-0"));
  EXPECT_FALSE(cage_display_router::is_wayland_socket_name_for_tests("nested-wayland-1"));
}
#endif
#else
TEST(CageDisplayRouterPolicyTests, LinuxOnly) {
  GTEST_SKIP() << "Linux-only runtime policy tests";
}
#endif
