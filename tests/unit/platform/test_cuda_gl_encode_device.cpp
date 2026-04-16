/**
 * @file tests/unit/platform/test_cuda_gl_encode_device.cpp
 * @brief Reproduces the Linux GL->CUDA encode-device path used by headless DMA-BUF capture.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_CUDA)
extern "C" {
  #include <libavutil/frame.h>
  #include <libavutil/hwcontext.h>
}

  #include <glad/egl.h>
  #include <src/platform/linux/cuda.h>
  #include <src/platform/linux/graphics.h>

TEST(CudaGlEncodeDeviceTests, CudaGlEncodeDeviceInitializesForHeadlessDmabufPath) {
  ASSERT_EQ(gbm::init(), 0) << "GBM initialization failed";
  ASSERT_TRUE(gladLoaderLoadEGL(EGL_NO_DISPLAY)) << "EGL loader initialization failed";
  ASSERT_NE(eglGetPlatformDisplay, nullptr) << "eglGetPlatformDisplay was not loaded";
  auto device = cuda::make_avcodec_gl_encode_device(1920, 1080, 0, 0);
  ASSERT_NE(device, nullptr) << "GL->CUDA encode device initialization failed";
}

TEST(CudaGlEncodeDeviceTests, CudaGlEncodeDeviceSetFrameInitializesNv12Pipeline) {
  ASSERT_EQ(gbm::init(), 0) << "GBM initialization failed";
  ASSERT_TRUE(gladLoaderLoadEGL(EGL_NO_DISPLAY)) << "EGL loader initialization failed";
  ASSERT_NE(eglGetPlatformDisplay, nullptr) << "eglGetPlatformDisplay was not loaded";

  auto run_once = []() {
    auto device = cuda::make_avcodec_gl_encode_device(1920, 1080, 0, 0);
    EXPECT_NE(device, nullptr) << "GL->CUDA encode device initialization failed";
    if (!device) {
      return;
    }

    AVBufferRef *hw_device_buf = nullptr;
    ASSERT_GE(av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1), 0)
      << "Failed to create CUDA hwdevice";

    AVBufferRef *hw_frames_buf = av_hwframe_ctx_alloc(hw_device_buf);
    ASSERT_NE(hw_frames_buf, nullptr) << "Failed to allocate CUDA hwframe context";

    auto *frames_ctx = reinterpret_cast<AVHWFramesContext *>(hw_frames_buf->data);
    frames_ctx->format = AV_PIX_FMT_CUDA;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width = 1920;
    frames_ctx->height = 1080;
    frames_ctx->initial_pool_size = 0;
    ASSERT_GE(av_hwframe_ctx_init(hw_frames_buf), 0) << "Failed to initialize CUDA hwframe context";

    AVFrame *frame = av_frame_alloc();
    ASSERT_NE(frame, nullptr) << "Failed to allocate AVFrame";
    frame->format = AV_PIX_FMT_CUDA;
    frame->width = 1920;
    frame->height = 1080;

    const int set_frame_status = device->set_frame(frame, hw_frames_buf);
    EXPECT_EQ(set_frame_status, 0) << "GL->CUDA set_frame initialization failed";

    av_buffer_unref(&hw_frames_buf);
    av_buffer_unref(&hw_device_buf);
  };

  run_once();
  run_once();
}

TEST(CudaGlEncodeDeviceTests, CudaGlEncodeDeviceSetFrameInitializesP010Pipeline) {
  ASSERT_EQ(gbm::init(), 0) << "GBM initialization failed";
  ASSERT_TRUE(gladLoaderLoadEGL(EGL_NO_DISPLAY)) << "EGL loader initialization failed";
  ASSERT_NE(eglGetPlatformDisplay, nullptr) << "eglGetPlatformDisplay was not loaded";

  auto device = cuda::make_avcodec_gl_encode_device(1920, 1080, 0, 0);
  ASSERT_NE(device, nullptr) << "GL->CUDA encode device initialization failed";

  AVBufferRef *hw_device_buf = nullptr;
  ASSERT_GE(av_hwdevice_ctx_create(&hw_device_buf, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 1), 0)
    << "Failed to create CUDA hwdevice";

  AVBufferRef *hw_frames_buf = av_hwframe_ctx_alloc(hw_device_buf);
  ASSERT_NE(hw_frames_buf, nullptr) << "Failed to allocate CUDA hwframe context";

  auto *frames_ctx = reinterpret_cast<AVHWFramesContext *>(hw_frames_buf->data);
  frames_ctx->format = AV_PIX_FMT_CUDA;
  frames_ctx->sw_format = AV_PIX_FMT_P010;
  frames_ctx->width = 1920;
  frames_ctx->height = 1080;
  frames_ctx->initial_pool_size = 0;
  ASSERT_GE(av_hwframe_ctx_init(hw_frames_buf), 0) << "Failed to initialize CUDA hwframe context";

  AVFrame *frame = av_frame_alloc();
  ASSERT_NE(frame, nullptr) << "Failed to allocate AVFrame";
  frame->format = AV_PIX_FMT_CUDA;
  frame->width = 1920;
  frame->height = 1080;

  const int set_frame_status = device->set_frame(frame, hw_frames_buf);
  EXPECT_EQ(set_frame_status, 0) << "GL->CUDA set_frame initialization failed for P010";

  av_buffer_unref(&hw_frames_buf);
  av_buffer_unref(&hw_device_buf);
}
#else
TEST(CudaGlEncodeDeviceTests, LinuxCudaOnly) {
  GTEST_SKIP() << "Linux/CUDA-only test";
}
#endif
