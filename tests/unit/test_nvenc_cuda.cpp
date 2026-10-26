/**
 * @file tests/unit/test_nvenc_cuda.cpp
 * @brief Guard converter-owned device memory before native CUDA NVENC registration.
 */
#include "../tests_common.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <src/nvenc/nvenc_cuda.h>

TEST(NvencCudaLayoutTests, AcceptsPitchedNv12ButRefusesOtherPlaneLayouts) {
  constexpr std::uintptr_t y = 0x100000;
  constexpr int pitch = 2048;
  constexpr int height = 1080;
  constexpr auto uv = y + pitch * height;
  EXPECT_TRUE(nvenc::contiguous_nv12_layout(y, uv, pitch, pitch, 1920, height));
  EXPECT_FALSE(nvenc::contiguous_nv12_layout(y, uv + 32, pitch, pitch, 1920, height));
  EXPECT_FALSE(nvenc::contiguous_nv12_layout(y, uv, pitch, pitch / 2, 1920, height));
  EXPECT_FALSE(nvenc::contiguous_nv12_layout(y, uv, 1024, 1024, 1920, height));
  EXPECT_FALSE(nvenc::contiguous_nv12_layout(y, uv, pitch, pitch, 1919, height));
  EXPECT_FALSE(nvenc::contiguous_nv12_layout(y, uv, pitch, pitch, 1920, 1079));
}

TEST(NvencCudaLayoutTests, RefusesMissingSurfacesAndOverflowingDeviceAddresses) {
  EXPECT_FALSE(nvenc::contiguous_nv12_layout(0, 4096, 64, 64, 64, 64));
  EXPECT_FALSE(nvenc::contiguous_nv12_layout(4096, 0, 64, 64, 64, 64));
  EXPECT_FALSE(nvenc::contiguous_nv12_layout(4096, 8192, -64, -64, 64, 64));
  const auto y = std::numeric_limits<std::uintptr_t>::max() - 1023;
  EXPECT_FALSE(nvenc::contiguous_nv12_layout(y, y + 4096, 64, 64, 64, 64));
}
#endif
