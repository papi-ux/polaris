/**
 * @file tests/unit/test_wlgrab_pixel_copy.cpp
 * @brief Tests for the SHM pixel copy logic in the wlgrab screencopy path.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {
  // Wayland DRM fourcc values used by the SHM copy path in wlgrab.cpp.
  constexpr uint32_t FMT_XBGR8888 = 0x34324258;
  constexpr uint32_t FMT_ABGR8888 = 0x34324241;
  constexpr uint32_t FMT_XRGB8888 = 0x34325258;

  // Mirrors the 4bpp branch of the SHM copy path in wlgrab.cpp.
  // Keep this in sync with that branch if the source changes.
  //
  // XBGR8888/ABGR8888 arrive with memory layout [R,G,B,X] on little-endian
  // and need B↔R swapped so the encoder receives [B,G,R,X/A].
  // All other 4bpp formats are already [B,G,R,X] and are copied directly.
  void copy_4bpp_shm(
    const uint8_t *src, int src_stride,
    uint8_t *dst,       int dst_stride,
    int copy_w, int copy_h,
    uint32_t fmt
  ) {
    const int copy_bytes = std::min(src_stride, dst_stride);
    const bool needs_rb_swap = (fmt == FMT_XBGR8888 || fmt == FMT_ABGR8888);
    if (needs_rb_swap) {
      for (int y = 0; y < copy_h; ++y) {
        const uint8_t *src_line = src + y * src_stride;
        uint8_t *dst_line = dst + y * dst_stride;
        for (int x = 0; x < copy_w; ++x) {
          dst_line[x * 4 + 0] = src_line[x * 4 + 2];
          dst_line[x * 4 + 1] = src_line[x * 4 + 1];
          dst_line[x * 4 + 2] = src_line[x * 4 + 0];
          dst_line[x * 4 + 3] = src_line[x * 4 + 3];
        }
      }
    } else if (src_stride == dst_stride && copy_bytes == src_stride) {
      std::memcpy(dst, src, static_cast<std::size_t>(copy_h) * copy_bytes);
    } else {
      for (int y = 0; y < copy_h; ++y) {
        std::memcpy(dst + y * dst_stride, src + y * src_stride, copy_bytes);
      }
    }
  }
}  // namespace

// XBGR8888 on LE stores pixels as [R,G,B,X] in memory.
// The encoder expects [B,G,R,X], so bytes 0 and 2 must be swapped.
TEST(WlgrabPixelCopy, XBGR8888SwapsRedAndBlue) {
  const uint8_t src[] = { 0x11 /*R*/, 0x22 /*G*/, 0x33 /*B*/, 0xFF /*X*/ };
  uint8_t dst[4] = {};

  copy_4bpp_shm(src, 4, dst, 4, 1, 1, FMT_XBGR8888);

  EXPECT_EQ(dst[0], 0x33u);  // B
  EXPECT_EQ(dst[1], 0x22u);  // G unchanged
  EXPECT_EQ(dst[2], 0x11u);  // R
  EXPECT_EQ(dst[3], 0xFFu);  // X unchanged
}

TEST(WlgrabPixelCopy, ABGR8888SwapsRedAndBlue) {
  const uint8_t src[] = { 0xAA /*R*/, 0xBB /*G*/, 0xCC /*B*/, 0x80 /*A*/ };
  uint8_t dst[4] = {};

  copy_4bpp_shm(src, 4, dst, 4, 1, 1, FMT_ABGR8888);

  EXPECT_EQ(dst[0], 0xCCu);  // B
  EXPECT_EQ(dst[1], 0xBBu);  // G unchanged
  EXPECT_EQ(dst[2], 0xAAu);  // R
  EXPECT_EQ(dst[3], 0x80u);  // A unchanged
}

// XRGB8888 on LE stores pixels as [B,G,R,X] — already correct for the encoder.
TEST(WlgrabPixelCopy, XRGB8888CopiesWithoutSwap) {
  const uint8_t src[] = { 0x11, 0x22, 0x33, 0xFF };
  uint8_t dst[4] = {};

  copy_4bpp_shm(src, 4, dst, 4, 1, 1, FMT_XRGB8888);

  EXPECT_EQ(dst[0], 0x11u);
  EXPECT_EQ(dst[1], 0x22u);
  EXPECT_EQ(dst[2], 0x33u);
  EXPECT_EQ(dst[3], 0xFFu);
}

TEST(WlgrabPixelCopy, XBGR8888SwapAppliedToEveryRowAndPixel) {
  // 2×2 frame, stride == width * 4 (no row padding)
  const uint8_t src[] = {
    0x10, 0x20, 0x30, 0xFF,  // row 0 px 0
    0x40, 0x50, 0x60, 0xFF,  // row 0 px 1
    0x70, 0x80, 0x90, 0xFF,  // row 1 px 0
    0xA0, 0xB0, 0xC0, 0xFF,  // row 1 px 1
  };
  uint8_t dst[16] = {};

  copy_4bpp_shm(src, 8, dst, 8, 2, 2, FMT_XBGR8888);

  EXPECT_EQ(dst[0],  0x30u); EXPECT_EQ(dst[2],  0x10u);   // row 0 px 0
  EXPECT_EQ(dst[4],  0x60u); EXPECT_EQ(dst[6],  0x40u);   // row 0 px 1
  EXPECT_EQ(dst[8],  0x90u); EXPECT_EQ(dst[10], 0x70u);   // row 1 px 0
  EXPECT_EQ(dst[12], 0xC0u); EXPECT_EQ(dst[14], 0xA0u);   // row 1 px 1
}

TEST(WlgrabPixelCopy, XBGR8888SwapWithPaddedSourceStride) {
  // Screencopy buffers often have padding bytes at the end of each row.
  // The swap loop uses copy_w (pixel count), not copy_bytes, so padding is ignored.
  constexpr int W = 2;
  constexpr int src_stride = W * 4 + 4;  // 12 bytes/row — 4 bytes padding
  constexpr int dst_stride = W * 4;       // 8 bytes/row — tight

  uint8_t src[src_stride * 2] = {};
  src[0] = 0x01; src[1] = 0x02; src[2] = 0x03; src[3] = 0xFF;  // row 0 px 0
  src[4] = 0x04; src[5] = 0x05; src[6] = 0x06; src[7] = 0xFF;  // row 0 px 1
  // src[8..11]: padding — left zero, must not appear in output
  src[src_stride + 0] = 0x07; src[src_stride + 1] = 0x08;  // row 1 px 0
  src[src_stride + 2] = 0x09; src[src_stride + 3] = 0xFF;
  src[src_stride + 4] = 0x0A; src[src_stride + 5] = 0x0B;  // row 1 px 1
  src[src_stride + 6] = 0x0C; src[src_stride + 7] = 0xFF;

  uint8_t dst[dst_stride * 2] = {};
  copy_4bpp_shm(src, src_stride, dst, dst_stride, W, 2, FMT_XBGR8888);

  EXPECT_EQ(dst[0], 0x03u); EXPECT_EQ(dst[2], 0x01u);  // row 0 px 0: B=src[2], R=src[0]
  EXPECT_EQ(dst[4], 0x06u); EXPECT_EQ(dst[6], 0x04u);  // row 0 px 1
  EXPECT_EQ(dst[dst_stride + 0], 0x09u); EXPECT_EQ(dst[dst_stride + 2], 0x07u);  // row 1 px 0
  EXPECT_EQ(dst[dst_stride + 4], 0x0Cu); EXPECT_EQ(dst[dst_stride + 6], 0x0Au);  // row 1 px 1
}
