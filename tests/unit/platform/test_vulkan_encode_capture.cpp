/**
 * @file tests/unit/platform/test_vulkan_encode_capture.cpp
 * @brief Tests the Vulkan Video encoder's live capture path: a real DMA-BUF and cursor into an encoder frame.
 */
// test includes
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_VULKAN)

  // standard includes
  #include <algorithm>
  #include <cstring>
  #include <memory>
  #include <sstream>
  #include <string>
  #include <string_view>
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

  // lib includes
  #include <boost/core/null_deleter.hpp>
  #include <boost/log/core.hpp>
  #include <boost/log/sinks/sync_frontend.hpp>
  #include <boost/log/sinks/text_ostream_backend.hpp>
  #include <boost/smart_ptr/make_shared_object.hpp>
  #include <boost/smart_ptr/shared_ptr.hpp>

  // local includes
  #include "src/logging.h"
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
   * An encoder frame and the device that converts into it, set up the way video.cpp sets them up for
   * a session. The hardware device and the frames context are declared before the converting device
   * so they outlive it: its teardown still uses the Vulkan device, and it keeps a plain pointer to the
   * frames context.
   */
  struct encoder_frame_t {
    buffer_t hw_device;
    buffer_t frames_ref;
    std::unique_ptr<platf::avcodec_encode_device_t> device;
    AVFrame *frame = nullptr;  // The device owns it once set_frame() takes it.

    bool open(std::unique_ptr<platf::avcodec_encode_device_t> made) {
      device = std::move(made);
      if (!device || !device->data) {
        return false;
      }
      using init_hw_device_fn = int (*)(platf::avcodec_encode_device_t *, AVBufferRef **);
      AVBufferRef *created_device = nullptr;
      if (reinterpret_cast<init_hw_device_fn>(device->data)(device.get(), &created_device) != 0) {
        return false;
      }
      hw_device.reset(created_device);

      frames_ref.reset(av_hwframe_ctx_alloc(hw_device.get()));
      if (!frames_ref) {
        return false;
      }
      auto *frames = reinterpret_cast<AVHWFramesContext *>(frames_ref->data);
      frames->format = AV_PIX_FMT_VULKAN;
      frames->sw_format = AV_PIX_FMT_NV12;
      frames->width = frame_width;
      frames->height = frame_height;
      frames->initial_pool_size = 0;
      device->init_hwframes(frames);
      if (av_hwframe_ctx_init(frames_ref.get()) < 0) {
        return false;
      }

      frame = av_frame_alloc();
      if (!frame || device->set_frame(frame, frames_ref.get()) != 0) {
        return false;
      }
      device->colorspace = {video::colorspace_e::rec709, false, 8};
      device->apply_colorspace();
      return true;
    }
  };

  /// A frame in host memory, the way portal shared memory capture hands one over.
  struct host_frame_t: platf::img_t {
    explicit host_frame_t(std::uint8_t level):
        pixels(static_cast<std::size_t>(frame_width) * frame_height * 4, level) {
      data = pixels.data();
      width = frame_width;
      height = frame_height;
      pixel_pitch = 4;
      row_pitch = frame_width * 4;
    }

    std::vector<std::uint8_t> pixels;
  };

  /// The lines the host logs while it is alive, as an operator reads them.
  class log_capture_t {
  public:
    log_capture_t():
        stream_ {boost::make_shared<std::ostringstream>()} {
      auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
      backend->add_stream(boost::shared_ptr<std::ostream> {stream_.get(), boost::null_deleter {}});
      backend->auto_flush(true);
      sink_ = boost::make_shared<sink_t>(backend);
      sink_->set_formatter(&logging::formatter);
      boost::log::core::get()->add_sink(sink_);
    }

    ~log_capture_t() {
      boost::log::core::get()->remove_sink(sink_);
    }

    log_capture_t(const log_capture_t &) = delete;
    log_capture_t &operator=(const log_capture_t &) = delete;

    /// Every captured line that holds needle.
    [[nodiscard]] std::vector<std::string> lines_with(std::string_view needle) const {
      std::istringstream input {stream_->str()};
      std::vector<std::string> out;
      for (std::string line; std::getline(input, line);) {
        if (line.find(needle) != std::string::npos) {
          out.push_back(line);
        }
      }
      return out;
    }

  private:
    using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    boost::shared_ptr<std::ostringstream> stream_;
    boost::shared_ptr<sink_t> sink_;
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

/**
 * The refusal belongs to the system memory upload alone.
 *
 * VRAM capture hands over exactly the frame that upload refuses whenever no cursor is drawn: a
 * DMA-BUF with nothing in host memory, since KMS capture sets the cursor pixels to null when none is
 * visible. Refused here, it would end every KMS or wlroots Vulkan Video stream on its first frame
 * without a cursor.
 */
TEST(VulkanEncodeCaptureTests, AVramFrameWithNoCursorIsImportedNotRefused) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t white {0xFF};
  if (!white.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_vram(frame_width, frame_height, 0, 0, render_node)));

  egl::img_descriptor_t img;
  white.describe(img, 1);
  ASSERT_EQ(img.data, nullptr) << "a cursor was drawn, so this is not the frame the refusal matches";
  ASSERT_TRUE(vk::ram_upload_cannot_read(img)) << "this is not the frame the system memory upload refuses";

  EXPECT_EQ(encoder.device->convert(img), 0) << "a VRAM frame with no cursor was refused, which ends the stream";
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the DMA-BUF did not reach the encoder frame";
}

/**
 * The system memory upload refuses a live DMA-BUF frame and keeps the primer.
 *
 * It reads pixels from host memory and read a frame with none as black. A portal that negotiated
 * DMA-BUF hands over two such frames: the primer, which has no DMA-BUF either and is rightly black,
 * and every live frame after it, which has a DMA-BUF this route cannot read. Encoding those as black
 * streamed a black picture at full frame rate with nothing in any log (#635).
 */
TEST(VulkanRamUploadTests, OnlyALiveDmabufFrameIsRefused) {
  // The primer, exactly as the portal makes it once DMA-BUF is negotiated: alloc_img() leaves the
  // descriptors closed and the pixels unset, and dummy_img() sets the sequence to zero.
  egl::img_descriptor_t primer;
  primer.width = frame_width;
  primer.height = frame_height;
  primer.pixel_pitch = 4;
  primer.row_pitch = frame_width * 4;
  primer.sequence = 0;
  EXPECT_FALSE(vk::ram_upload_cannot_read(primer)) << "the primer is refused, so no session ever starts";

  const host_frame_t shared_memory {0x80};
  EXPECT_FALSE(vk::ram_upload_cannot_read(shared_memory)) << "shared memory is the frame this route exists for";

  // A live frame as portal capture fills one in: a DMA-BUF, no pixels in host memory.
  egl::img_descriptor_t live;
  live.width = frame_width;
  live.height = frame_height;
  live.sd.fds[0] = ::open("/dev/null", O_RDONLY | O_CLOEXEC);  // The descriptor closes it.
  ASSERT_GE(live.sd.fds[0], 0);
  live.sequence = 1;
  EXPECT_TRUE(vk::ram_upload_cannot_read(live)) << "a DMA-BUF frame is read as black";
}

TEST(VulkanRamUploadTests, ThePrimerStillEncodesBlack) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_ram(frame_width, frame_height, render_node)));

  egl::img_descriptor_t primer;
  primer.width = frame_width;
  primer.height = frame_height;
  primer.pixel_pitch = 4;
  primer.row_pitch = frame_width * 4;
  primer.sequence = 0;
  ASSERT_EQ(encoder.device->convert(primer), 0) << "the frame every session is primed with was refused";

  const auto [centre, corner] = centre_and_corner_luma(encoder.frame);
  EXPECT_LE(centre, 26) << "the primer is not black";
  EXPECT_LE(corner, 26);
}

TEST(VulkanRamUploadTests, ALiveDmabufFrameEndsTheStreamInsteadOfEncodingBlack) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  const grey_dmabuf_t captured {0x00};
  if (!captured.ok) {
    GTEST_SKIP() << "Could not allocate a linear DMA-BUF on " << render_node;
  }
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_ram(frame_width, frame_height, render_node)));

  // A shared memory frame first, which this route reads, so a black frame after it can only be the
  // refused frame encoded anyway.
  host_frame_t white {0xFF};
  ASSERT_EQ(encoder.device->convert(white), 0);
  ASSERT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the shared memory frame did not reach the encoder";

  egl::img_descriptor_t live;
  captured.describe(live, 1);
  live.width = frame_width;
  live.height = frame_height;
  live.pixel_pitch = 4;
  live.row_pitch = frame_width * 4;
  const log_capture_t log;
  EXPECT_EQ(encoder.device->convert(live), platf::convert_capture_unreadable)
    << "the DMA-BUF frame was taken, so the stream goes on without a picture";
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the refused frame was encoded as black";

  // A capture that hands this route a DMA-BUF hands it one for every frame, and the log says so once,
  // at error, naming the route and that Vulkan Video on the portal takes shared memory only.
  EXPECT_EQ(encoder.device->convert(live), platf::convert_capture_unreadable);
  const auto said = log.lines_with("DMA-BUF frame reached the system memory upload route");
  ASSERT_EQ(said.size(), 1u) << "the refusal was logged " << said.size() << " times for two frames";
  EXPECT_NE(said.front().find("Error: "), std::string::npos) << said.front();
  EXPECT_NE(said.front().find("Vulkan Video on the portal takes shared memory only"), std::string::npos) << said.front();
}


/**
 * The system memory upload refuses a packed 10-bit frame and reads an 8-bit one.
 *
 * The portal hands an HDR stream packed 10-bit frames in shared memory, at the same four bytes a
 * pixel as BGRA, and the upload copied them into an 8-bit BGRA image. What it encoded was noise, with
 * nothing in any log (#635).
 */
TEST(VulkanRamUploadTests, OnlyAFrameCaptureTaggedTenBitIsRefusedForItsDepth) {
  host_frame_t ten_bit {0x80};
  ten_bit.frame_metadata = {platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::p010};
  EXPECT_TRUE(vk::ram_upload_frame_is_ten_bit(ten_bit)) << "a 10-bit frame is read as 8-bit BGRA";

  host_frame_t eight_bit {0x80};
  eight_bit.frame_metadata = {platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::bgra8};
  EXPECT_FALSE(vk::ram_upload_frame_is_ten_bit(eight_bit)) << "shared memory BGRA is the frame this route exists for";

  const host_frame_t untagged {0x80};
  EXPECT_FALSE(vk::ram_upload_frame_is_ten_bit(untagged)) << "a backend that tags nothing hands over BGRA";

  // The primer carries no pixels to misread; a live frame with none is the DMA-BUF refusal's.
  egl::img_descriptor_t primer;
  primer.width = frame_width;
  primer.height = frame_height;
  primer.pixel_pitch = 4;
  primer.row_pitch = frame_width * 4;
  primer.frame_metadata.format = platf::frame_format_e::p010;
  EXPECT_FALSE(vk::ram_upload_frame_is_ten_bit(primer));
}

TEST(VulkanRamUploadTests, ATenBitFrameEndsTheStreamInsteadOfEncodingNoise) {
  if (!vk::validate()) {
    GTEST_SKIP() << "No Vulkan Video encoder on this host";
  }
  encoder_frame_t encoder;
  ASSERT_TRUE(encoder.open(vk::make_avcodec_encode_device_ram(frame_width, frame_height, render_node)));

  // An 8-bit frame first, which this route reads, so a black frame after it can only be the refused
  // frame encoded anyway.
  host_frame_t white {0xFF};
  white.frame_metadata = {platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::bgra8};
  ASSERT_EQ(encoder.device->convert(white), 0);
  ASSERT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the shared memory frame did not reach the encoder";

  // Zero words, which read as 8-bit BGRA are black.
  host_frame_t ten_bit {0x00};
  ten_bit.frame_metadata = {platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::p010};
  const log_capture_t log;
  EXPECT_EQ(encoder.device->convert(ten_bit), platf::convert_capture_unreadable)
    << "the 10-bit frame was read as 8-bit BGRA";
  EXPECT_GE(centre_and_corner_luma(encoder.frame).first, 225) << "the refused frame was encoded";

  // The portal fixes the format for the whole capture, and the log says so once, at error.
  EXPECT_EQ(encoder.device->convert(ten_bit), platf::convert_capture_unreadable);
  const auto said = log.lines_with("10-bit frame reached the system memory upload route");
  ASSERT_EQ(said.size(), 1u) << "the refusal was logged " << said.size() << " times for two frames";
  EXPECT_NE(said.front().find("Error: "), std::string::npos) << said.front();
  EXPECT_NE(said.front().find("Vulkan Video on the portal streams SDR only"), std::string::npos) << said.front();
}

#endif
