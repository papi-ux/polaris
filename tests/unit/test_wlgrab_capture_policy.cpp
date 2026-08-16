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

TEST(WlgrabCapturePolicy, VaapiGpuNativeProbeIsFailClosedAcrossAllRoutes) {
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

TEST(WlgrabCapturePolicy, VaapiGpuNativeCaptureIsFailClosedRegardlessOfModifier) {
  constexpr std::uint64_t tiled_modifier = 0x0100000000000002ULL;
  const std::optional<std::uint64_t> modifiers[] = {
    std::nullopt,
    DRM_FORMAT_MOD_LINEAR,
    DRM_FORMAT_MOD_INVALID,
    tiled_modifier,
  };

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
