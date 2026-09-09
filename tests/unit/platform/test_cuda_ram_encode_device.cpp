/** Physical CUDA conversion checks. No capture, input device or streaming server. */
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_CUDA)
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <src/platform/linux/cuda.h>
#include <src/video.h>
extern "C" {
#include <libavutil/hwcontext_cuda.h>
}
#include <array>
#include <chrono>
#include <cstdlib>

TEST(CudaRamEncodeDeviceTests, ExactArrayDimensionsAndPaddedRowsRoundTrip) {
  if (!getenv("POLARIS_TEST_CUDA_RAM")) GTEST_SKIP() << "Requires isolated physical CUDA validation";
  ASSERT_EQ(cudaSetDevice(0), cudaSuccess);
  EXPECT_FALSE(cuda::tex_t::make(0, 64));
  EXPECT_FALSE(cuda::tex_t::make(32, -1));
  constexpr int width = 66, height = 34, pitch = width * 4 + 20;
  auto texture = cuda::tex_t::make(height, width);
  ASSERT_TRUE(texture);
  cudaChannelFormatDesc format {};
  cudaExtent extent {};
  unsigned flags = 0;
  ASSERT_EQ(cudaArrayGetInfo(&format, &extent, &flags, texture->array), cudaSuccess);
  EXPECT_EQ(extent.width, width);
  EXPECT_EQ(extent.height, height);
  EXPECT_EQ(format.x + format.y + format.z + format.w, 32);
  std::vector<std::uint8_t> source(pitch * height, 0xee), result(width * 4 * height);
  for (int row = 0; row < height; ++row) {
    for (int byte = 0; byte < width * 4; ++byte) source[row * pitch + byte] = (row * 17 + byte) % 251;
  }
  platf::img_t image;
  image.data = source.data();
  image.width = width;
  image.height = height;
  image.row_pitch = pitch;
  image.pixel_pitch = 4;
  cuda::sws_t uploader;
  ASSERT_EQ(uploader.load_ram(image, texture->array), 0);
  ASSERT_EQ(cudaMemcpy2DFromArray(result.data(), width * 4, texture->array, 0, 0, width * 4, height, cudaMemcpyDeviceToHost), cudaSuccess);
  for (int row = 0; row < height; ++row) {
    EXPECT_TRUE(std::equal(result.begin() + row * width * 4, result.begin() + (row + 1) * width * 4, source.begin() + row * pitch)) << row;
  }
}

TEST(CudaRamEncodeDeviceTests, CompletesConversionBeforeIndependentStreamReadsTheFrame) {
  if (!getenv("POLARIS_TEST_CUDA_RAM")) GTEST_SKIP() << "Requires isolated physical CUDA validation";
  ASSERT_EQ(cudaSetDevice(0), cudaSuccess);
  for (const auto dimensions : {std::array {320, 180, 320, 180}, std::array {7680, 2160, 1920, 1080}}) {
    const auto [width, height, output_width, output_height] = dimensions;
    SCOPED_TRACE(width);
    AVBufferRef *raw_device = nullptr;
    ASSERT_GE(av_hwdevice_ctx_create(&raw_device, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, AV_CUDA_USE_PRIMARY_CONTEXT), 0);
    video::avcodec_buffer_t device_context {raw_device};
    auto *cuda_context = static_cast<AVCUDADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(raw_device->data)->hwctx);
    const auto encoder_stream = cuda_context->stream;
    video::avcodec_buffer_t frames {av_hwframe_ctx_alloc(raw_device)};
    ASSERT_TRUE(frames);
    auto *pool = reinterpret_cast<AVHWFramesContext *>(frames->data);
    pool->format = AV_PIX_FMT_CUDA;
    pool->sw_format = AV_PIX_FMT_NV12;
    pool->width = output_width;
    pool->height = output_height;
    ASSERT_GE(av_hwframe_ctx_init(frames.get()), 0);
    auto converter = cuda::make_avcodec_encode_device(width, height, false);
    ASSERT_TRUE(converter);
    video::avcodec_frame_t owned_frame {av_frame_alloc()};
    ASSERT_TRUE(owned_frame);
    auto *frame = owned_frame.get();
    frame->format = AV_PIX_FMT_CUDA;
    frame->width = output_width;
    frame->height = output_height;
    ASSERT_EQ(converter->set_frame(owned_frame.release(), frames.get()), 0);
    EXPECT_EQ(cuda_context->stream, encoder_stream);
    converter->colorspace = {video::colorspace_e::rec709, false, 8};
    converter->apply_colorspace();
    struct observer_t {
      cudaStream_t stream = nullptr;
      std::uint8_t *pixels = nullptr;
      ~observer_t() {
        if (stream) cudaStreamSynchronize(stream);
        if (pixels) cudaFreeHost(pixels);
        if (stream) cudaStreamDestroy(stream);
      }
    } observer;
    ASSERT_EQ(cudaStreamCreateWithFlags(&observer.stream, cudaStreamNonBlocking), cudaSuccess);
    ASSERT_EQ(cudaMallocHost(reinterpret_cast<void **>(&observer.pixels), output_width * output_height), cudaSuccess);
    platf::img_t image;
    image.width = width;
    image.height = height;
    image.pixel_pitch = 4;
    image.row_pitch = width * 4 + 32;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(image.row_pitch) * height, 0xee);
    image.data = pixels.data();
    double conversion_ms = 0;
    for (int iteration = 0; iteration < 24; ++iteration) {
      const bool white = iteration % 2 == 0;
      for (int row = 0; row < height; ++row) std::fill_n(pixels.data() + static_cast<std::size_t>(row) * image.row_pitch, width * 4, white ? 255 : 0);
      const auto start = std::chrono::steady_clock::now();
      ASSERT_EQ(converter->convert(image), 0);
      conversion_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      // A separate nonblocking stream has no implicit ordering with conversion.
      // Synchronizing this observer cannot complete another stream's kernels.
      ASSERT_EQ(cudaMemcpy2DAsync(observer.pixels, output_width, frame->data[0], frame->linesize[0], output_width, output_height, cudaMemcpyDeviceToHost, observer.stream), cudaSuccess);
      ASSERT_EQ(cudaStreamSynchronize(observer.stream), cudaSuccess);
      const int center = observer.pixels[(output_height / 2) * output_width + output_width / 2];
      if (white) EXPECT_GT(center, 200) << iteration;
      else EXPECT_LT(center, 24) << iteration;
      if (width == 7680) EXPECT_LT(observer.pixels[output_width / 2], 24) << "Letterbox padding must remain black";
    }
    RecordProperty("conversion_mean_ms_" + std::to_string(width), conversion_ms / 24);
  }
}
#endif
