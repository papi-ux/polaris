/**
 * @file tests/unit/test_wlgrab_capture_policy.cpp
 * @brief Regression tests for direct wlroots capture-path selection.
 */
#include <gtest/gtest.h>
#include <drm_fourcc.h>

#include "src/platform/linux/wlgrab_capture_policy.h"

namespace {
  using route_e = wlgrab_capture_policy::gpu_native_capture_route_e;
}

TEST(WlgrabCapturePolicy, EnumeratedOutputsPreferStableConnectorIdentity) {
  EXPECT_EQ(
    wlgrab_capture_policy::enumerated_monitor_identity(0, "POLARIS-HEADLESS-512536-0"),
    "POLARIS-HEADLESS-512536-0"
  );
  EXPECT_EQ(wlgrab_capture_policy::enumerated_monitor_identity(1, ""), "1");
}

TEST(WlgrabCapturePolicy, ImmutableGenerationWinsAfterGlobalConfigDrift) {
  const capture_generation::identity_t generation {
    .generation_id = 41,
    .stream_mode = "headless_stream",
    .capture_backend = "wlr",
    .private_wayland_socket = "wayland-polaris-41",
    .private_runtime_instance_id = "session-41",
    .adapter_name = "/dev/dri/renderD128",
    .headless_mode = true,
    .use_cage_compositor = true,
  };

  const auto policy = wlgrab_capture_policy::resolve_generation_policy(
    generation,
    false,
    "/dev/dri/renderD129"
  );

  EXPECT_TRUE(policy.owned);
  EXPECT_TRUE(policy.use_private_compositor);
  EXPECT_EQ(policy.adapter_name, "/dev/dri/renderD128");
  EXPECT_EQ(policy.private_wayland_socket, "wayland-polaris-41");
  EXPECT_EQ(policy.private_runtime_instance_id, "session-41");
  EXPECT_TRUE(wlgrab_capture_policy::private_runtime_matches_generation(
    policy,
    "wayland-polaris-41",
    "session-41"
  ));
  EXPECT_FALSE(wlgrab_capture_policy::private_runtime_matches_generation(
    policy,
    "wayland-polaris-42",
    "session-41"
  ));
  EXPECT_FALSE(wlgrab_capture_policy::private_runtime_matches_generation(
    policy,
    "wayland-polaris-41",
    "session-42"
  ));
}

TEST(WlgrabCapturePolicy, RequestedMonitorSelectionIsExactAndFailClosed) {
  const std::vector<std::string> monitors {
    "POLARIS-HEADLESS-512536-0",
    "HDMI-A-1",
  };

  EXPECT_EQ(wlgrab_capture_policy::select_monitor_index("", monitors), 0u);
  EXPECT_EQ(wlgrab_capture_policy::select_monitor_index("1", monitors), 1u);
  EXPECT_EQ(
    wlgrab_capture_policy::select_monitor_index("POLARIS-HEADLESS-512536-0", monitors),
    0u
  );
  EXPECT_EQ(wlgrab_capture_policy::select_monitor_index("HDMI-A-1", monitors), 1u);
  EXPECT_FALSE(wlgrab_capture_policy::select_monitor_index("2", monitors).has_value());
  EXPECT_FALSE(wlgrab_capture_policy::select_monitor_index("DP-9", monitors).has_value());
  EXPECT_FALSE(wlgrab_capture_policy::select_monitor_index("", {}).has_value());
}

TEST(WlgrabCapturePolicy, HostVirtualOutputDoesNotFallBackToPhysicalMonitorZero) {
  // Issue #556: Hyprland advertises the physical monitor first and the newly
  // created headless connector second. A connector name must never be parsed
  // as index zero or silently fall back to it.
  const std::vector<std::string> monitors {
    "DP-2",
    "POLARIS-HEADLESS-1627120-0",
  };

  const auto selected = wlgrab_capture_policy::select_monitor_index(
    "POLARIS-HEADLESS-1627120-0",
    monitors
  );

  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ(*selected, 1u);
  EXPECT_FALSE(wlgrab_capture_policy::select_monitor_index(
    "POLARIS-HEADLESS-unknown",
    monitors
  ).has_value());
}

TEST(WlgrabCapturePolicy, DirectVaapiCaptureUsesRamFallback) {
  EXPECT_EQ(
    wlgrab_capture_policy::select_direct_capture_path(platf::mem_type_e::vaapi, true),
    wlgrab_capture_policy::direct_capture_path_e::ram
  );
}

TEST(WlgrabCapturePolicy, DirectCudaCaptureMayRemainGpuNative) {
  EXPECT_EQ(
    wlgrab_capture_policy::select_direct_capture_path(platf::mem_type_e::cuda, true),
    wlgrab_capture_policy::direct_capture_path_e::gpu_native
  );
}

TEST(WlgrabCapturePolicy, MissingGpuNativeBackendUsesRamFallback) {
  EXPECT_EQ(
    wlgrab_capture_policy::select_direct_capture_path(platf::mem_type_e::cuda, false),
    wlgrab_capture_policy::direct_capture_path_e::ram
  );
}

TEST(WlgrabCapturePolicy, SystemMemoryEncoderUsesRamCapture) {
  EXPECT_EQ(
    wlgrab_capture_policy::select_direct_capture_path(platf::mem_type_e::system, true),
    wlgrab_capture_policy::direct_capture_path_e::ram
  );
}

TEST(WlgrabCapturePolicy, VaapiNeverProbesGpuNativeDmabuf) {
  for (const auto route : {
         route_e::headless_extcopy,
         route_e::windowed_nested,
         route_e::direct_wayland,
       }) {
    EXPECT_FALSE(wlgrab_capture_policy::gpu_native_dmabuf_probe_is_allowed(
      platf::mem_type_e::vaapi,
      route
    ));
  }
}

TEST(WlgrabCapturePolicy, VaapiGpuNativeCaptureIsFailClosedRegardlessOfRouteOrModifier) {
  constexpr std::uint64_t tiled_modifier = 0x0100000000000002ULL;
  const std::optional<std::uint64_t> modifiers[] = {
    std::nullopt,
    DRM_FORMAT_MOD_LINEAR,
    DRM_FORMAT_MOD_INVALID,
    tiled_modifier,
  };

  // #111 and #367 both reached the first-frame failure with modifier 0, so
  // every VAAPI route remains contained even when the buffer reports linear.
  for (const auto route : {
         route_e::headless_extcopy,
         route_e::windowed_nested,
         route_e::direct_wayland,
       }) {
    for (const auto modifier : modifiers) {
      EXPECT_FALSE(wlgrab_capture_policy::gpu_native_dmabuf_is_safe(
        platf::mem_type_e::vaapi,
        route,
        modifier
      ));
    }
  }
}

TEST(WlgrabCapturePolicy, CudaSafetyDoesNotDependOnRouteOrModifier) {
  for (const auto route : {
         route_e::headless_extcopy,
         route_e::windowed_nested,
         route_e::direct_wayland,
       }) {
    EXPECT_TRUE(wlgrab_capture_policy::gpu_native_dmabuf_is_safe(
      platf::mem_type_e::cuda,
      route,
      std::nullopt
    ));
  }
}

TEST(WlgrabCapturePolicy, UnsupportedEncoderTypesAreNeverGpuNativeSafe) {
  for (const auto hwdevice_type : {
         platf::mem_type_e::system,
         platf::mem_type_e::unknown,
       }) {
    EXPECT_FALSE(wlgrab_capture_policy::gpu_native_dmabuf_is_safe(
      hwdevice_type,
      route_e::headless_extcopy,
      DRM_FORMAT_MOD_LINEAR
    ));
  }
}
