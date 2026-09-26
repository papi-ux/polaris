/**
 * @file tests/unit/platform/test_vulkan_encode_capture.cpp
 * @brief Tests the Vulkan Video encoder's live capture path: a real DMA-BUF and cursor into an encoder frame.
 */
// test includes
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_VULKAN)

  // standard includes
  #include <cstring>
  #include <memory>
  #include <string>
  #include <utility>
  #include <vector>

  // platform includes
  #include <drm_fourcc.h>
  #include <fcntl.h>
  #include <gbm.h>
  #include <unistd.h>

// lib includes
extern "C" {
  #include <libavutil/frame.h>
  #include <libavutil/hwcontext.h>
}

  // local includes
  #include "src/platform/linux/graphics.h"
  #include "src/platform/linux/vulkan_encode.h"

namespace {
  constexpr int frame_width = 256;
  constexpr int frame_height = 144;
  constexpr const char *render_node = "/dev/dri/renderD128";

  struct buffer_unref_t {
    void operator()(AVBufferRef *buffer) const {
      av_buffer_unref(&buffer);
    }
  };

  using buffer_t = std::unique_ptr<AVBufferRef, buffer_unref_t>;

  /**
   * A linear XRGB8888 buffer the GPU owns, filled with one grey level. Linear because it is the only
   * layout a test can write by hand, and an explicit linear modifier because that is the shape a
   * portal buffer arrives in, so the import takes its modifier path rather than assuming a tiling.
   */
  struct grey_dmabuf_t {
    explicit grey_dmabuf_t(std::uint8_t level) {
      node = ::open(render_node, O_RDWR | O_CLOEXEC);
      if (node < 0) {
        return;
      }
      device = gbm_create_device(node);
      if (!device) {
        return;
      }
      const std::uint64_t linear = DRM_FORMAT_MOD_LINEAR;
      bo = gbm_bo_create_with_modifiers(device, frame_width, frame_height, GBM_FORMAT_XRGB8888, &linear, 1);
      if (!bo) {
        return;
      }

      void *map_data = nullptr;
      std::uint32_t map_stride = 0;
      auto *mapped = static_cast<std::uint8_t *>(
        gbm_bo_map(bo, 0, 0, frame_width, frame_height, GBM_BO_TRANSFER_WRITE, &map_stride, &map_data)
      );
      if (!mapped) {
        return;
      }
      for (int row = 0; row < frame_height; ++row) {
        std::memset(mapped + static_cast<std::size_t>(row) * map_stride, level, static_cast<std::size_t>(frame_width) * 4);
      }
      gbm_bo_unmap(bo, map_data);
      ok = true;
    }

    ~grey_dmabuf_t() {
      if (bo) {
        gbm_bo_destroy(bo);
      }
      if (device) {
        gbm_device_destroy(device);
      }
      if (node >= 0) {
        ::close(node);
      }
    }

    grey_dmabuf_t(const grey_dmabuf_t &) = delete;
    grey_dmabuf_t &operator=(const grey_dmabuf_t &) = delete;

    /**
     * Describe the buffer the way capture does. The descriptor owns the exported descriptor and
     * closes it, as it does for a captured frame.
     */
    void describe(egl::img_descriptor_t &img, std::uint64_t sequence) const {
      img.reset();
      img.sd.width = frame_width;
      img.sd.height = frame_height;
      img.sd.fds[0] = gbm_bo_get_fd(bo);
      img.sd.fourcc = DRM_FORMAT_XRGB8888;
      img.sd.modifier = gbm_bo_get_modifier(bo);
      img.sd.pitches[0] = gbm_bo_get_stride(bo);
      img.sd.offsets[0] = gbm_bo_get_offset(bo, 0);
      img.sequence = sequence;
    }

    bool ok = false;

  private:
    int node = -1;
    gbm_device *device = nullptr;
    gbm_bo *bo = nullptr;
  };

  /**
   * Luma samples from the encoder's frame, read back through FFmpeg, which waits on the timeline
   * semaphores the conversion signalled. Returns the centre and the top-left corner.
   */
  std::pair<int, int> centre_and_corner_luma(AVFrame *hw_frame) {
    std::unique_ptr<AVFrame, void (*)(AVFrame *)> sw_frame {av_frame_alloc(), [](AVFrame *frame) {
                                                              av_frame_free(&frame);
                                                            }};
    sw_frame->format = AV_PIX_FMT_NV12;
    if (av_hwframe_transfer_data(sw_frame.get(), hw_frame, 0) < 0) {
      return {-1, -1};
    }
    const auto *luma = sw_frame->data[0];
    const auto pitch = static_cast<std::size_t>(sw_frame->linesize[0]);
    return {luma[(frame_height / 2) * pitch + frame_width / 2], luma[4 * pitch + 4]};
  }
}  // namespace

TEST(VulkanEncodeCaptureTests, ImportsADmabufAndCursorIntoTheEncoderFrame) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  const grey_dmabuf_t black {0x00};
  if (!white.ok || !black.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }

  // Declared before the device so they outlive it: its teardown still uses the Vulkan device, and it
  // keeps a plain pointer to the frames context.
  buffer_t hw_device;
  buffer_t frames_ref;

  // The same sequence video.cpp runs for a VRAM capture session.
  auto device = vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node);
  ASSERT_TRUE(device);
  ASSERT_NE(device->data, nullptr);
  using init_hw_device_fn = int (*)(platf::avcodec_encode_device_t *, AVBufferRef **);
  AVBufferRef *created_device = nullptr;
  ASSERT_EQ(reinterpret_cast<init_hw_device_fn>(device->data)(device.get(), &created_device), 0);
  hw_device.reset(created_device);

  frames_ref.reset(av_hwframe_ctx_alloc(hw_device.get()));
  ASSERT_TRUE(frames_ref);
  auto *frames = reinterpret_cast<AVHWFramesContext *>(frames_ref->data);
  frames->format = AV_PIX_FMT_VULKAN;
  frames->sw_format = AV_PIX_FMT_NV12;
  frames->width = frame_width;
  frames->height = frame_height;
  frames->initial_pool_size = 0;
  device->init_hwframes(frames);
  ASSERT_GE(av_hwframe_ctx_init(frames_ref.get()), 0);

  auto *frame = av_frame_alloc();
  ASSERT_NE(frame, nullptr);
  ASSERT_EQ(device->set_frame(frame, frames_ref.get()), 0);  // The device owns the frame from here.
  device->colorspace = {video::colorspace_e::rec709, false, 8};
  device->apply_colorspace();

  // An opaque white cursor in the top-left corner, clear of the centre sample.
  std::vector<std::uint8_t> cursor_pixels(16 * 16 * 4, 0xFF);
  egl::img_descriptor_t img;
  img.data = cursor_pixels.data();
  img.src_w = img.width = 16;
  img.src_h = img.height = 16;
  img.x = img.y = 0;
  img.serial = 1;
  img.y_invert = false;

  white.describe(img, 1);
  ASSERT_EQ(device->convert(img), 0);
  const auto [white_centre, white_corner] = centre_and_corner_luma(frame);

  // A new capture sequence imports the next buffer and retires the previous one; the cursor, whose
  // serial has not changed, is drawn again from the image the first conversion uploaded.
  black.describe(img, 2);
  ASSERT_EQ(device->convert(img), 0);
  const auto [black_centre, black_corner] = centre_and_corner_luma(frame);

  // Rec. 709 limited range: white is 235 and black is 16. The margins only absorb rounding.
  EXPECT_GE(white_centre, 225) << "the white capture did not reach the encoder frame";
  EXPECT_LE(black_centre, 26) << "the black capture did not reach the encoder frame";
  EXPECT_GE(black_corner, 225) << "the cursor was not drawn over the capture";
  EXPECT_GE(white_corner, 225);
}

#endif
