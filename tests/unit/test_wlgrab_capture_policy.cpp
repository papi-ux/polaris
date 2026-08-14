/**
 * @file tests/unit/test_wlgrab_capture_policy.cpp
 * @brief Regression tests for direct wlroots capture-path selection.
 */
#include <gtest/gtest.h>

#include "src/platform/linux/wlgrab_capture_policy.h"

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
