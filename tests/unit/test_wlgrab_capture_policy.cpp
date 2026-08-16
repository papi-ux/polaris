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

TEST(WlgrabCapturePolicy, LinearVaapiHeadlessCaptureStillUsesShmFallback) {
  constexpr std::uint64_t tiled_modifier = 0x0100000000000002ULL;

  // #111 and #367 both reached the first-frame failure with modifier 0, so
  // DRM_FORMAT_MOD_LINEAR is not sufficient evidence of a safe VAAPI import.
  EXPECT_FALSE(wlgrab_capture_policy::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::headless_extcopy,
    DRM_FORMAT_MOD_LINEAR
  ));
  EXPECT_FALSE(wlgrab_capture_policy::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::headless_extcopy,
    std::nullopt
  ));
  EXPECT_FALSE(wlgrab_capture_policy::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::headless_extcopy,
    DRM_FORMAT_MOD_INVALID
  ));
  EXPECT_FALSE(wlgrab_capture_policy::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::headless_extcopy,
    tiled_modifier
  ));
}

TEST(WlgrabCapturePolicy, LinearVaapiBuffersRemainRefusedOutsideHeadlessPrivateCapture) {
  EXPECT_FALSE(wlgrab_capture_policy::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::windowed_nested,
    DRM_FORMAT_MOD_LINEAR
  ));
  EXPECT_FALSE(wlgrab_capture_policy::gpu_native_dmabuf_is_safe(
    platf::mem_type_e::vaapi,
    route_e::direct_wayland,
    DRM_FORMAT_MOD_LINEAR
  ));
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
