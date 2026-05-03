/**
 * @file tests/unit/test_wlgrab_pixel_copy.cpp
 * @brief Tests for wlgrab SHM pixel copy helpers.
 */
#include <gtest/gtest.h>

#include "src/platform/linux/wlgrab_pixel_copy.h"

#include <array>
#include <cstdint>

#include <drm_fourcc.h>

namespace {
  void expect_pixel(
    const std::uint8_t *pixels,
    int offset,
    std::uint8_t b,
    std::uint8_t g,
    std::uint8_t r,
    std::uint8_t a
  ) {
    EXPECT_EQ(pixels[offset + 0], b);
    EXPECT_EQ(pixels[offset + 1], g);
    EXPECT_EQ(pixels[offset + 2], r);
    EXPECT_EQ(pixels[offset + 3], a);
  }
}  // namespace

TEST(WlgrabPixelCopy, Rgb888ExpandsToBgra) {
  const std::array<std::uint8_t, 3> src {0x11, 0x22, 0x33};
  std::array<std::uint8_t, 4> dst {};

  wl::copy_shm_3bpp_rgb_to_bgra(src.data(), 3, dst.data(), 4, 1, 1);

  expect_pixel(dst.data(), 0, 0x33, 0x22, 0x11, 0xFF);
}

TEST(WlgrabPixelCopy, Rgb888ExpansionHandlesRowsAndPadding) {
  constexpr int width = 2;
  constexpr int height = 2;
  constexpr int src_stride = width * 3 + 2;
  constexpr int dst_stride = width * 4 + 4;

  std::array<std::uint8_t, src_stride * height> src {};
  src[0] = 0x01; src[1] = 0x02; src[2] = 0x03;
  src[3] = 0x04; src[4] = 0x05; src[5] = 0x06;
  src[src_stride + 0] = 0x07; src[src_stride + 1] = 0x08; src[src_stride + 2] = 0x09;
  src[src_stride + 3] = 0x0A; src[src_stride + 4] = 0x0B; src[src_stride + 5] = 0x0C;

  std::array<std::uint8_t, dst_stride * height> dst {};
  wl::copy_shm_3bpp_rgb_to_bgra(src.data(), src_stride, dst.data(), dst_stride, width, height);

  expect_pixel(dst.data(), 0, 0x03, 0x02, 0x01, 0xFF);
  expect_pixel(dst.data(), 4, 0x06, 0x05, 0x04, 0xFF);
  expect_pixel(dst.data(), dst_stride, 0x09, 0x08, 0x07, 0xFF);
  expect_pixel(dst.data(), dst_stride + 4, 0x0C, 0x0B, 0x0A, 0xFF);
}

TEST(WlgrabPixelCopy, XBGR8888SwapsRedAndBlue) {
  const std::array<std::uint8_t, 4> src {0x11, 0x22, 0x33, 0xFF};
  std::array<std::uint8_t, 4> dst {};

  wl::copy_shm_4bpp_to_bgra(src.data(), 4, dst.data(), 4, 1, 1, DRM_FORMAT_XBGR8888);

  expect_pixel(dst.data(), 0, 0x33, 0x22, 0x11, 0xFF);
}

TEST(WlgrabPixelCopy, ABGR8888SwapsRedAndBlue) {
  const std::array<std::uint8_t, 4> src {0xAA, 0xBB, 0xCC, 0x80};
  std::array<std::uint8_t, 4> dst {};

  wl::copy_shm_4bpp_to_bgra(src.data(), 4, dst.data(), 4, 1, 1, DRM_FORMAT_ABGR8888);

  expect_pixel(dst.data(), 0, 0xCC, 0xBB, 0xAA, 0x80);
}

TEST(WlgrabPixelCopy, XRGB8888SwapsRedAndBlue) {
  const std::array<std::uint8_t, 4> src {0x11, 0x22, 0x33, 0xFF};
  std::array<std::uint8_t, 4> dst {};

  wl::copy_shm_4bpp_to_bgra(src.data(), 4, dst.data(), 4, 1, 1, DRM_FORMAT_XRGB8888);

  expect_pixel(dst.data(), 0, 0x33, 0x22, 0x11, 0xFF);
}

TEST(WlgrabPixelCopy, ARGB8888SwapsRedAndBlue) {
  const std::array<std::uint8_t, 4> src {0xAA, 0xBB, 0xCC, 0x40};
  std::array<std::uint8_t, 4> dst {};

  wl::copy_shm_4bpp_to_bgra(src.data(), 4, dst.data(), 4, 1, 1, DRM_FORMAT_ARGB8888);

  expect_pixel(dst.data(), 0, 0xCC, 0xBB, 0xAA, 0x40);
}

TEST(WlgrabPixelCopy, Unknown4bppFormatCopiesWithoutSwap) {
  const std::array<std::uint8_t, 4> src {0x11, 0x22, 0x33, 0xFF};
  std::array<std::uint8_t, 4> dst {};

  wl::copy_shm_4bpp_to_bgra(src.data(), 4, dst.data(), 4, 1, 1, 0);

  expect_pixel(dst.data(), 0, 0x11, 0x22, 0x33, 0xFF);
}

TEST(WlgrabPixelCopy, XBGR8888SwapAppliedToEveryRowAndPixel) {
  const std::array<std::uint8_t, 16> src {
    0x10, 0x20, 0x30, 0xFF,
    0x40, 0x50, 0x60, 0xFF,
    0x70, 0x80, 0x90, 0xFF,
    0xA0, 0xB0, 0xC0, 0xFF,
  };
  std::array<std::uint8_t, 16> dst {};

  wl::copy_shm_4bpp_to_bgra(src.data(), 8, dst.data(), 8, 2, 2, DRM_FORMAT_XBGR8888);

  expect_pixel(dst.data(), 0, 0x30, 0x20, 0x10, 0xFF);
  expect_pixel(dst.data(), 4, 0x60, 0x50, 0x40, 0xFF);
  expect_pixel(dst.data(), 8, 0x90, 0x80, 0x70, 0xFF);
  expect_pixel(dst.data(), 12, 0xC0, 0xB0, 0xA0, 0xFF);
}

TEST(WlgrabPixelCopy, XBGR8888SwapIgnoresSourceRowPadding) {
  constexpr int width = 2;
  constexpr int height = 2;
  constexpr int src_stride = width * 4 + 4;
  constexpr int dst_stride = width * 4;

  std::array<std::uint8_t, src_stride * height> src {};
  src[0] = 0x01; src[1] = 0x02; src[2] = 0x03; src[3] = 0xFF;
  src[4] = 0x04; src[5] = 0x05; src[6] = 0x06; src[7] = 0xFF;
  src[src_stride + 0] = 0x07; src[src_stride + 1] = 0x08;
  src[src_stride + 2] = 0x09; src[src_stride + 3] = 0xFF;
  src[src_stride + 4] = 0x0A; src[src_stride + 5] = 0x0B;
  src[src_stride + 6] = 0x0C; src[src_stride + 7] = 0xFF;

  std::array<std::uint8_t, dst_stride * height> dst {};
  wl::copy_shm_4bpp_to_bgra(src.data(), src_stride, dst.data(), dst_stride, width, height, DRM_FORMAT_XBGR8888);

  expect_pixel(dst.data(), 0, 0x03, 0x02, 0x01, 0xFF);
  expect_pixel(dst.data(), 4, 0x06, 0x05, 0x04, 0xFF);
  expect_pixel(dst.data(), dst_stride, 0x09, 0x08, 0x07, 0xFF);
  expect_pixel(dst.data(), dst_stride + 4, 0x0C, 0x0B, 0x0A, 0xFF);
}
