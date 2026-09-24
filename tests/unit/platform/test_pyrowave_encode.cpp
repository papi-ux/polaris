/**
 * @file tests/unit/platform/test_pyrowave_encode.cpp
 * @brief Tests for the PyroWave compute encoder.
 */
// test includes
#include "../../tests_common.h"

#ifdef POLARIS_BUILD_PYROWAVE

  // local includes
  #include "src/platform/linux/pyrowave_encode.h"

  // The codec itself, for the other half of the round trip. Its header wants the Vulkan API declared
  // before it and says so with an #error.
  #include <vulkan/vulkan.h>

  #include "pyrowave.h"

  // A real dmabuf, because the only way to test importing one is to have one. gbm is what every
  // capture backend on this platform allocates through, so a buffer from here is the same kind of
  // object the portal hands over.
  #include <fcntl.h>
  #include <unistd.h>

  #include <drm_fourcc.h>
  #include <gbm.h>

  #include <algorithm>
  #include <cmath>
  #include <cstdlib>
  #include <cstring>
  #include <filesystem>
  #include <fstream>
  #include <memory>
  #include <sstream>
  #include <string>

namespace {

  /**
   * A source file, for the contracts that are about what the code says rather than what it does.
   */
  std::string read_source_for_contract(const char *relative_path) {
    const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / relative_path;
    std::ifstream in(path);
    if (!in) {
      return {};
    }

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  /**
   * A frame with structure in it. A flat colour compresses to almost nothing and would pass a
   * broken encoder, and a gradient is the same signal upstream's own conformance test uses.
   */
  struct test_frame_t {
    test_frame_t(int width, int height):
        width {width},
        height {height},
        y(static_cast<std::size_t>(width) * height),
        u(static_cast<std::size_t>(width / 2) * (height / 2)),
        v(static_cast<std::size_t>(width / 2) * (height / 2)) {
      for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
          y[static_cast<std::size_t>(row) * width + col] = static_cast<uint8_t>(3 * col + 5 * row);
        }
      }
      for (int row = 0; row < height / 2; ++row) {
        for (int col = 0; col < width / 2; ++col) {
          const auto at = static_cast<std::size_t>(row) * (width / 2) + col;
          u[at] = static_cast<uint8_t>(7 * col + 3 * row);
          v[at] = static_cast<uint8_t>(3 * col + 5 * row);
        }
      }
    }

    int width;
    int height;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
  };

  /**
   * A frame decoded back to planar YUV, the way a client decodes it.
   *
   * On its own device, created the way any other application would create one, because the point is
   * to read what a decoder somewhere else reads rather than to ask the encoder what it meant. Nothing
   * else in this file can tell a full range Rec. 709 frame from a limited range BT.601 one, and that
   * is the mistake that looks plausible on a desktop and wrong on anything saturated.
   */
  struct decoded_frame_t {
    decoded_frame_t(const std::vector<uint8_t> &frame, int width, int height, bool chroma_444):
        width {width},
        height {height},
        shift {chroma_444 ? 0 : 1} {
      if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS || !device) {
        return;
      }

      pyrowave_decoder_create_info info = {};
      info.device = device;
      info.width = width;
      info.height = height;
      info.chroma = chroma_444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
      if (pyrowave_decoder_create(&info, &decoder) != PYROWAVE_SUCCESS || !decoder) {
        return;
      }

      // One push for the whole frame. The bitstream delimits itself, so this is byte for byte the
      // same work as pushing every packet in turn, and it is what Nova's renderer does.
      if (pyrowave_decoder_push_packet(decoder, frame.data(), frame.size()) != PYROWAVE_SUCCESS) {
        return;
      }
      if (!pyrowave_decoder_decode_is_ready(decoder, false)) {
        return;
      }

      y.assign(static_cast<std::size_t>(width) * height, 0);
      u.assign(static_cast<std::size_t>(width >> shift) * (height >> shift), 0);
      v.assign(u.size(), 0);

      pyrowave_cpu_buffer buffer = {};
      buffer.format = chroma_444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P
                                 : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
      buffer.width = width;
      buffer.height = height;
      buffer.data[0] = y.data();
      buffer.data[1] = u.data();
      buffer.data[2] = v.data();
      buffer.row_stride_in_bytes[0] = static_cast<std::size_t>(width);
      buffer.row_stride_in_bytes[1] = static_cast<std::size_t>(width >> shift);
      buffer.row_stride_in_bytes[2] = static_cast<std::size_t>(width >> shift);
      buffer.plane_size_in_bytes[0] = y.size();
      buffer.plane_size_in_bytes[1] = u.size();
      buffer.plane_size_in_bytes[2] = v.size();

      ok = pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) == PYROWAVE_SUCCESS;
    }

    ~decoded_frame_t() {
      if (decoder) {
        pyrowave_decoder_destroy(decoder);
      }
      if (device) {
        pyrowave_device_destroy(device);
      }
    }

    decoded_frame_t(const decoded_frame_t &) = delete;
    decoded_frame_t &operator=(const decoded_frame_t &) = delete;

    /// The luma sample at a point, which is where the picture is rather than where its edges are.
    int luma_at(int col, int row) const {
      return y[static_cast<std::size_t>(row) * width + col];
    }

    int chroma_u_at(int col, int row) const {
      return u[static_cast<std::size_t>(row >> shift) * (width >> shift) + (col >> shift)];
    }

    int chroma_v_at(int col, int row) const {
      return v[static_cast<std::size_t>(row >> shift) * (width >> shift) + (col >> shift)];
    }

    bool ok = false;
    int width;
    int height;
    int shift;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;

  private:
    pyrowave_device device = nullptr;
    pyrowave_decoder decoder = nullptr;
  };

  /// Which primaries a stream's colour matrix is built from, which its profile token names.
  enum class primaries_e {
    bt709,
    bt2020,
  };

  /**
   * Full range YCbCr with centred chroma, which is what a profile token promises a client.
   *
   * The matrix is applied to the samples as they arrive rather than to linear light, for both
   * primaries, which is what every video codec does and what the codec's own shader does. So an
   * expectation computed from the code values is the right expectation whether those code values
   * carry sRGB gamma or PQ.
   */
  struct expected_ycbcr_t {
    expected_ycbcr_t(double r, double g, double b, primaries_e primaries = primaries_e::bt709) {
      const bool wide = primaries == primaries_e::bt2020;
      const double kr = wide ? 0.2627 : 0.2126;
      const double kg = wide ? 0.6780 : 0.7152;
      const double kb = wide ? 0.0593 : 0.0722;
      const double cb_span = wide ? 1.8814 : 1.8556;
      const double cr_span = wide ? 1.4746 : 1.5748;

      const double luma = kr * r + kg * g + kb * b;
      y = std::clamp(luma, 0.0, 255.0);
      u = std::clamp((b - luma) / cb_span + 128.0, 0.0, 255.0);
      v = std::clamp((r - luma) / cr_span + 128.0, 0.0, 255.0);
    }

    double y;
    double u;
    double v;
  };

  /**
   * Four solid quadrants packed the way ten bit capture hands them over.
   *
   * Two bits unused and ten each for blue, green and red from the top of the word down, which is
   * XBGR2101010 to DRM and A2B10G10R10 to Vulkan. Getting this order wrong swaps red and blue, and
   * the test that reads the quadrants back is what would catch it.
   */
  struct quadrant_frame_10bit_t {
    quadrant_frame_10bit_t(int width, int height, int stride):
        width {width},
        height {height},
        stride {stride},
        pixels(static_cast<std::size_t>(stride) * height, 0) {
      for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
          const auto which = (row < height / 2 ? 0 : 2) + (col < width / 2 ? 0 : 1);
          const uint32_t r = static_cast<uint32_t>(codes[which][0]);
          const uint32_t g = static_cast<uint32_t>(codes[which][1]);
          const uint32_t b = static_cast<uint32_t>(codes[which][2]);
          const uint32_t word = (3u << 30) | (b << 20) | (g << 10) | r;
          std::memcpy(&pixels[static_cast<std::size_t>(row) * stride +
                              static_cast<std::size_t>(col) * 4],
                      &word, sizeof(word));
        }
      }
    }

    std::pair<int, int> centre_of(int which) const {
      const int col = (which % 2 == 0 ? width / 4 : width - width / 4);
      const int row = (which < 2 ? height / 4 : height - height / 4);
      return {col, row};
    }

    /// The same sample as an eight bit expectation wants, which is where the two can be compared.
    double as_eight_bit(int which, int channel) const {
      return codes[which][channel] * 255.0 / 1023.0;
    }

    // Ten bit code values. The primaries are saturated because a wrong matrix hides on grey, and the
    // grey quadrant sits well below the top of the range because PQ spends most of its code space
    // down there and a bug that clipped would still pass at full scale.
    static constexpr int codes[4][3] = {
      {1023, 0, 0},
      {0, 1023, 0},
      {0, 0, 1023},
      {520, 520, 520},
    };

    int width;
    int height;
    int stride;
    std::vector<uint8_t> pixels;
  };

  /// Four solid quadrants, so a sample from the middle of each says what the colour became.
  struct quadrant_frame_t {
    quadrant_frame_t(int width, int height, int stride):
        width {width},
        height {height},
        stride {stride},
        bgra(static_cast<std::size_t>(stride) * height, 0) {
      for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
          const auto which = (row < height / 2 ? 0 : 2) + (col < width / 2 ? 0 : 1);
          auto *pixel = &bgra[static_cast<std::size_t>(row) * stride + static_cast<std::size_t>(col) * 4];
          pixel[0] = static_cast<uint8_t>(colours[which][2]);
          pixel[1] = static_cast<uint8_t>(colours[which][1]);
          pixel[2] = static_cast<uint8_t>(colours[which][0]);
          pixel[3] = 0xff;
        }
      }
    }

    /// The middle of a quadrant, far enough from every edge that no ringing reaches it.
    std::pair<int, int> centre_of(int which) const {
      const int col = (which % 2 == 0 ? width / 4 : width - width / 4);
      const int row = (which < 2 ? height / 4 : height - height / 4);
      return {col, row};
    }

    // Saturated primaries, because a wrong matrix or a wrong range is nearly invisible on grey and
    // impossible to miss on these.
    static constexpr int colours[4][3] = {
      {255, 0, 0},
      {0, 255, 0},
      {0, 0, 255},
      {128, 128, 128},
    };

    int width;
    int height;
    int stride;
    std::vector<uint8_t> bgra;
  };

  /**
   * Hold a decoded frame to what profile_token promises a client: full range Rec. 709.
   *
   * The colourimetry is the one thing about this stream that is written down in a token rather than
   * in the bitstream, because upstream reserves the fields and writes none of them. So this is the
   * only place the promise is checked, and it is checked on every path a frame can take, because a
   * client that reads these frames as limited range or as BT.601 sees something plausible on a
   * desktop and wrong on anything saturated, with nothing in any log to say why.
   */
  void expect_full_range_rec709(const decoded_frame_t &decoded, const quadrant_frame_t &source,
                                const char *path) {
    for (int which = 0; which < 4; ++which) {
      const auto [col, row] = source.centre_of(which);
      const expected_ycbcr_t want {source.colours[which][0], source.colours[which][1],
                                   source.colours[which][2]};

      EXPECT_NEAR(decoded.luma_at(col, row), want.y, 6.0)
        << path << ", quadrant " << which << " luma. Limited range would land sixteen high on black "
        << "and twenty low on white";
      EXPECT_NEAR(decoded.chroma_u_at(col, row), want.u, 8.0) << path << ", quadrant " << which << " Cb";
      EXPECT_NEAR(decoded.chroma_v_at(col, row), want.v, 8.0) << path << ", quadrant " << which << " Cr";
    }
  }

  /**
   * A buffer the GPU owns, filled with a known picture, described the way capture describes one.
   *
   * Linear on purpose: it is what Polaris asks the portal for, and it is the only layout a test can
   * write into by hand. A buffer with a real tiling modifier would need the GPU to fill it, which
   * proves the driver's tiling rather than this code's import.
   */
  struct test_dmabuf_t {
    test_dmabuf_t(int width, int height, const quadrant_frame_t &picture):
        width {width},
        height {height} {
      node = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      if (node < 0) {
        return;
      }
      device = gbm_create_device(node);
      if (!device) {
        return;
      }

      // XRGB8888 is BGRA in memory, which is what capture hands over and what the codec reads, and
      // an explicit linear modifier is what Polaris asks the portal for. Asked for by modifier rather
      // than by use flags because the combination that says the same thing in flags is refused by at
      // least one driver here, and because this is the shape the real buffer arrives in: a modifier
      // the importer has to honour rather than a tiling it may assume.
      const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
      bo = gbm_bo_create_with_modifiers(device, static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height), GBM_FORMAT_XRGB8888, &linear, 1);
      if (!bo) {
        return;
      }

      void *map_data = nullptr;
      uint32_t map_stride = 0;
      auto *mapped = static_cast<uint8_t *>(
        gbm_bo_map(bo, 0, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                   GBM_BO_TRANSFER_WRITE, &map_stride, &map_data));
      if (!mapped) {
        return;
      }
      for (int row = 0; row < height; ++row) {
        std::memcpy(mapped + static_cast<std::size_t>(row) * map_stride,
                    picture.bgra.data() + static_cast<std::size_t>(row) * picture.stride,
                    static_cast<std::size_t>(width) * 4);
      }
      gbm_bo_unmap(bo, map_data);

      buffer.fds[0] = gbm_bo_get_fd(bo);
      buffer.fourcc = DRM_FORMAT_XRGB8888;
      buffer.modifier = gbm_bo_get_modifier(bo);
      buffer.pitches[0] = gbm_bo_get_stride(bo);
      buffer.offsets[0] = gbm_bo_get_offset(bo, 0);
      buffer.width = width;
      buffer.height = height;
      ok = buffer.fds[0] >= 0;
    }

    ~test_dmabuf_t() {
      if (buffer.fds[0] >= 0) {
        ::close(buffer.fds[0]);
      }
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

    test_dmabuf_t(const test_dmabuf_t &) = delete;
    test_dmabuf_t &operator=(const test_dmabuf_t &) = delete;

    bool ok = false;
    int width;
    int height;
    pyrowave_encode::dmabuf_t buffer;

  private:
    int node = -1;
    gbm_device *device = nullptr;
    gbm_bo *bo = nullptr;
  };

  /**
   * The start of frame header a PyroWave bitstream opens with, read from the bytes.
   *
   * Mirrors the codec's own two words rather than including its private header, which is the point:
   * these are the bytes Polaris puts on the wire and Nova's decoder reads back, so reading them the
   * way a decoder would is what proves the frame begins where a decoder expects it to. If the codec
   * ever moves a field, this is a test that fails rather than a stream that decodes to noise.
   */
  struct sequence_header_t {
    explicit sequence_header_t(const std::vector<uint8_t> &frame) {
      if (frame.size() < 8) {
        return;
      }
      uint32_t first = 0;
      uint32_t second = 0;
      std::memcpy(&first, frame.data(), sizeof(first));
      std::memcpy(&second, frame.data() + 4, sizeof(second));
      present = true;
      width = static_cast<int>(first & 0x3fff) + 1;
      height = static_cast<int>((first >> 14) & 0x3fff) + 1;
      extended = (first >> 31) & 0x1;
      total_blocks = second & 0xffffff;
      code = (second >> 24) & 0x3;
      chroma_resolution = (second >> 26) & 0x1;
      // Three bits, which is all a decoder needs to tell this frame from the last one.
      sequence = (first >> 28) & 0x7;
    }

    bool present = false;
    int width = 0;
    int height = 0;
    uint32_t extended = 0;
    uint32_t total_blocks = 0;
    uint32_t code = 0;
    uint32_t chroma_resolution = 0;
    uint32_t sequence = 0;
  };

  constexpr uint32_t start_of_frame = 0;
  constexpr uint32_t chroma_420 = 0;
  constexpr uint32_t chroma_444 = 1;

  /// The shape every test but the chroma one wants, so they say what they are about instead.
  std::unique_ptr<pyrowave_encode::session_t> make_session_420(int width, int height) {
    return pyrowave_encode::make_session(width, height, pyrowave_encode::chroma_e::yuv420);
  }

}  // namespace

TEST(PyroWaveEncodeTests, TheLinkedLibraryAnswersForItself) {
  // Read from the library rather than the build system. If these two ever disagree, this is where
  // it shows up, rather than in a stream that decodes to noise.
  const auto version = pyrowave_encode::api_version();
  EXPECT_FALSE(version.empty());
  EXPECT_NE(version.find('.'), std::string::npos);
}

TEST(PyroWaveEncodeTests, AnOddExtentIsRefusedRatherThanRounded) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }
  // Zero and negative are the same question asked louder, whichever chroma is asked for.
  EXPECT_EQ(make_session_420(0, 720), nullptr);
  EXPECT_EQ(make_session_420(1280, 0), nullptr);
  EXPECT_EQ(make_session_420(-2, -2), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(0, 720, pyrowave_encode::chroma_e::yuv444), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(1280, 0, pyrowave_encode::chroma_e::yuv444), nullptr);
}

TEST(PyroWaveEncodeTests, AFrameArrivesAsOneBitstreamADecoderCanOpen) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 1280;
  constexpr int height = 720;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  const test_frame_t frame {width, height};
  // A generous ceiling: this test is about the plumbing, and rate control has its own opinions that
  // a fixed expectation would only encode twice.
  constexpr std::size_t max_bytes = 512 * 1024;
  ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), max_bytes));

  const auto &encoded = session->bitstream();
  ASSERT_FALSE(encoded.empty());
  EXPECT_LE(encoded.size(), max_bytes) << "rate control is supposed to be exact, not approximate";

  // The frame has to begin at its beginning. Polaris sends this blob as one frame and Nova pushes
  // it to the decoder in one call, so a buffer that started one packet in would decode to nothing
  // with no error to read: the decoder would look for a sequence header, find a block header, and
  // drop the frame.
  const sequence_header_t header {encoded};
  ASSERT_TRUE(header.present);
  EXPECT_EQ(header.extended, 1U) << "the first header is not an extended one, so not a sequence header";
  EXPECT_EQ(header.code, start_of_frame);
  EXPECT_EQ(header.width, width);
  EXPECT_EQ(header.height, height);
  EXPECT_EQ(header.chroma_resolution, chroma_420);
  EXPECT_GT(header.total_blocks, 0U);
}

TEST(PyroWaveEncodeTests, EncodesTheBgraFrameCaptureActuallyHandsOver) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  // A padded stride, because capture rarely hands over rows packed to exactly four bytes a pixel
  // and a converter that assumes it produces a sheared picture rather than an error.
  constexpr int stride = (width + 37) * 4;
  std::vector<uint8_t> bgra(static_cast<std::size_t>(stride) * height, 0);
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      auto *pixel = &bgra[static_cast<std::size_t>(row) * stride + static_cast<std::size_t>(col) * 4];
      pixel[0] = static_cast<uint8_t>(3 * col);
      pixel[1] = static_cast<uint8_t>(5 * row);
      pixel[2] = static_cast<uint8_t>(col + row);
      pixel[3] = 0xff;
    }
  }

  ASSERT_TRUE(session->encode_packed(bgra.data(), width, height, stride, 256 * 1024));
  ASSERT_FALSE(session->bitstream().empty());
  const sequence_header_t header {session->bitstream()};
  ASSERT_TRUE(header.present);
  EXPECT_EQ(header.width, width);
  EXPECT_EQ(header.height, height);

  // The converter is built once and kept, so the second frame has to work as well as the first.
  ASSERT_TRUE(session->encode_packed(bgra.data(), width, height, stride, 256 * 1024));
  EXPECT_FALSE(session->bitstream().empty());
}

TEST(PyroWaveEncodeTests, ARefusedFrameLeavesNothingToRead) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 320;
  constexpr int height = 240;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  std::vector<uint8_t> bgra(static_cast<std::size_t>(width) * height * 4, 0x40);
  EXPECT_FALSE(session->encode_packed(nullptr, width, height, width * 4, 64 * 1024));
  EXPECT_FALSE(session->encode_packed(bgra.data(), width, height, 0, 64 * 1024));
  EXPECT_FALSE(session->encode_packed(bgra.data(), width, height, -1, 64 * 1024));
  EXPECT_FALSE(session->encode_packed(bgra.data(), 0, height, width * 4, 64 * 1024));
  EXPECT_FALSE(session->encode_packed(bgra.data(), width, 0, width * 4, 64 * 1024));
  // A stride that cannot hold the row it claims to. Reading it would run off the end of the buffer
  // capture handed over, which is the one argument here that is not merely wrong but unsafe.
  EXPECT_FALSE(session->encode_packed(bgra.data(), width, height, width * 4 - 1, 64 * 1024));
  EXPECT_FALSE(session->encode(nullptr, nullptr, nullptr, 64 * 1024));

  // A refusal has to leave the frame empty and not the previous picture. The caller checks the
  // return value, but the whole point of holding the frame in the session is that a missed check
  // sends nothing rather than sending the last good frame again under a new frame number.
  const test_frame_t frame {width, height};
  ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), 64 * 1024));
  ASSERT_FALSE(session->bitstream().empty());
  EXPECT_FALSE(session->encode(nullptr, nullptr, nullptr, 64 * 1024));
  EXPECT_TRUE(session->bitstream().empty()) << "a refused frame left the previous one readable";
}

TEST(PyroWaveEncodeTests, TheStreamKeepsItsOwnSizeWhateverCaptureHandsOver) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  // The size in the sequence header is the one the client created its decoder with, and a decoder
  // handed anything else drops the frame with a line about the dimensions and nothing about the
  // picture. So it has to be the session's size no matter what capture is: an ultrawide monitor
  // feeding a tablet is the ordinary case, not the exception.
  constexpr int stream_width = 1280;
  constexpr int stream_height = 800;
  auto session = make_session_420(stream_width, stream_height);
  ASSERT_NE(session, nullptr);

  struct {
    int width;
    int height;
    const char *what;
  } sources[] = {
    {7680, 2160, "an ultrawide, far wider than the stream"},
    {1280, 800, "exactly the stream's size"},
    {640, 480, "smaller than the stream, and a different shape"},
    {1920, 1080, "the same shape at a different size"},
  };

  for (const auto &source : sources) {
    const int stride = source.width * 4 + 64;
    std::vector<uint8_t> bgra(static_cast<std::size_t>(stride) * source.height, 0x30);
    for (int row = 0; row < source.height; ++row) {
      for (int col = 0; col < source.width; ++col) {
        auto *pixel = &bgra[static_cast<std::size_t>(row) * stride + static_cast<std::size_t>(col) * 4];
        pixel[0] = static_cast<uint8_t>(col);
        pixel[1] = static_cast<uint8_t>(row);
        pixel[2] = static_cast<uint8_t>(col + row);
        pixel[3] = 0xff;
      }
    }

    ASSERT_TRUE(session->encode_packed(bgra.data(), source.width, source.height, stride, 256 * 1024))
      << source.what;
    const sequence_header_t header {session->bitstream()};
    ASSERT_TRUE(header.present) << source.what;
    EXPECT_EQ(header.width, stream_width) << source.what;
    EXPECT_EQ(header.height, stream_height) << source.what;
  }
}

TEST(PyroWaveEncodeTests, EveryFrameStandsAlone) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  // Intra-only means the second frame owes nothing to the first, so encoding the same input twice
  // through one session has to produce a frame of much the same size both times. A codec with a
  // reference chain would quietly produce a far smaller second frame here.
  const test_frame_t frame {width, height};
  ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), 256 * 1024));
  const auto first = session->bitstream().size();
  ASSERT_GT(first, 0U);

  ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), 256 * 1024));
  const auto second = session->bitstream().size();
  ASSERT_GT(second, 0U);

  const auto larger = std::max(first, second);
  const auto smaller = std::min(first, second);
  EXPECT_GT(smaller * 2, larger) << "the second frame leaned on the first";
}

TEST(PyroWaveEncodeTests, TheSequenceNumberMovesSoADecoderKnowsTheFrameChanged) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 320;
  constexpr int height = 240;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  // The decoder decides a new frame has begun by the sequence field in the header moving, and it
  // silently drops a frame whose number it has already seen. A session that never advanced it would
  // encode happily and stream one picture forever.
  const test_frame_t frame {width, height};
  std::vector<uint32_t> sequences;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), 64 * 1024));
    const sequence_header_t header {session->bitstream()};
    ASSERT_TRUE(header.present);
    sequences.push_back(header.sequence);
  }

  EXPECT_NE(sequences[0], sequences[1]);
  EXPECT_NE(sequences[1], sequences[2]);
}

TEST(PyroWaveEncodeTests, ChromaIsCarriedIntoTheBitstreamAndCostsWhatItShould) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  // The decoder refuses a frame whose chroma does not match the one it was created with, and it
  // reads that from the sequence header rather than being told, so what the encoder writes there is
  // the whole of the agreement.
  constexpr int width = 640;
  constexpr int height = 360;
  const test_frame_t frame {width, height};

  auto narrow = pyrowave_encode::make_session(width, height, pyrowave_encode::chroma_e::yuv420);
  ASSERT_NE(narrow, nullptr);
  ASSERT_TRUE(narrow->encode(frame.y.data(), frame.u.data(), frame.v.data(), 512 * 1024));
  const sequence_header_t narrow_header {narrow->bitstream()};
  ASSERT_TRUE(narrow_header.present);
  EXPECT_EQ(narrow_header.chroma_resolution, chroma_420);

  // 4:4:4 wants a chroma plane per pixel rather than a quarter of one, so the caller has to hand
  // over full sized planes. A test that passed the 4:2:0 frame would be reading past the end of it.
  std::vector<uint8_t> u(static_cast<std::size_t>(width) * height);
  std::vector<uint8_t> v(u.size());
  for (int row = 0; row < height; ++row) {
    for (int col = 0; col < width; ++col) {
      const auto at = static_cast<std::size_t>(row) * width + col;
      u[at] = static_cast<uint8_t>(7 * col + 3 * row);
      v[at] = static_cast<uint8_t>(3 * col + 5 * row);
    }
  }

  auto full = pyrowave_encode::make_session(width, height, pyrowave_encode::chroma_e::yuv444);
  ASSERT_NE(full, nullptr);
  ASSERT_TRUE(full->encode(frame.y.data(), u.data(), v.data(), 512 * 1024));
  const sequence_header_t full_header {full->bitstream()};
  ASSERT_TRUE(full_header.present);
  EXPECT_EQ(full_header.chroma_resolution, chroma_444);

  // And it carries three times the samples rather than one and a half, so at the same generous
  // ceiling it has more to say. Not a fixed ratio, because that is rate control's business, but a
  // frame that came back smaller would mean the extra chroma went nowhere.
  EXPECT_GT(full->bitstream().size(), narrow->bitstream().size())
    << "4:4:4 carried no more than 4:2:0, so the extra chroma was not encoded";
}

TEST(PyroWaveEncodeTests, TheFrameCaptureHandsOverIsEncodedOnTheGpu) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  // Nothing has been offered yet, so there is nothing to have decided.
  EXPECT_FALSE(session->uses_gpu_input());

  // A padded stride, because that is what capture hands over and the upload has to be told how wide
  // a row really is rather than assuming.
  const quadrant_frame_t source {width, height, (width + 37) * 4};
  ASSERT_TRUE(session->encode_packed(source.bgra.data(), width, height, source.stride, 512 * 1024));
  EXPECT_TRUE(session->uses_gpu_input())
    << "the stream is the shape capture handed over, so nothing needed converting on the CPU";

  // And the frame is a frame, not just a path that returned true.
  const sequence_header_t header {session->bitstream()};
  ASSERT_TRUE(header.present);
  EXPECT_EQ(header.width, width);
  EXPECT_EQ(header.height, height);
}

TEST(PyroWaveEncodeTests, AWiderSourceGetsBarsAboveAndBelowWithoutStretching) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  // Twice as wide for its height as the stream is, which is the shape an ultrawide monitor has when
  // a client asks for sixteen by nine. The codec's scaler fills its output with its input and has no
  // letterbox in it, so this only comes out right if the picture it is given was padded first.
  const quadrant_frame_t source {1280, 360, 1280 * 4};
  ASSERT_TRUE(session->encode_packed(source.bgra.data(), source.width, source.height, source.stride,
                                   512 * 1024));
  ASSERT_TRUE(session->uses_gpu_input());

  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok);

  // Worked out from the shapes here rather than read back from the code that did it, so this is a
  // second opinion and not an echo.
  const int padded_height = source.width * height / width;
  const int top_bar = (padded_height - source.height) / 2;
  const auto to_output = [&](int col, int row) {
    return std::pair<int, int> {col * width / source.width,
                                (row + top_bar) * height / padded_height};
  };

  EXPECT_LT(decoded.luma_at(width / 2, 8), 8) << "the bar above the picture is not black";
  EXPECT_LT(decoded.luma_at(width / 2, height - 8), 8) << "the bar below it is not black";

  // The colours landing where the geometry says they should is what proves nothing was stretched: a
  // stretched picture would put the quadrant boundary in the wrong place and these samples would
  // read the neighbouring colour.
  for (int which = 0; which < 4; ++which) {
    const auto [col, row] = source.centre_of(which);
    const auto [out_col, out_row] = to_output(col, row);
    const expected_ycbcr_t want {source.colours[which][0], source.colours[which][1],
                                 source.colours[which][2]};
    EXPECT_NEAR(decoded.luma_at(out_col, out_row), want.y, 8.0)
      << "quadrant " << which << " should be at " << out_col << ',' << out_row;
    EXPECT_NEAR(decoded.chroma_v_at(out_col, out_row), want.v, 10.0) << "quadrant " << which << " Cr";
  }
}

TEST(PyroWaveEncodeTests, ATallerSourceGetsBarsEitherSide) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  // Four by three into sixteen by nine, which is every emulator and every older game.
  const quadrant_frame_t source {640, 480, 640 * 4};
  ASSERT_TRUE(session->encode_packed(source.bgra.data(), source.width, source.height, source.stride,
                                   512 * 1024));
  ASSERT_TRUE(session->uses_gpu_input());

  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok);

  EXPECT_LT(decoded.luma_at(8, height / 2), 8) << "the bar to the left of the picture is not black";
  EXPECT_LT(decoded.luma_at(width - 8, height / 2), 8) << "the bar to its right is not black";

  // The picture keeps its own shape in the middle, so its centre row is still four colours across.
  const int padded_width = source.height * width / height;
  const int left_bar = (padded_width - source.width) / 2;
  const auto to_output = [&](int col, int row) {
    return std::pair<int, int> {(col + left_bar) * width / padded_width,
                                row * height / source.height};
  };

  for (int which = 0; which < 4; ++which) {
    const auto [col, row] = source.centre_of(which);
    const auto [out_col, out_row] = to_output(col, row);
    const expected_ycbcr_t want {source.colours[which][0], source.colours[which][1],
                                 source.colours[which][2]};
    EXPECT_NEAR(decoded.luma_at(out_col, out_row), want.y, 8.0)
      << "quadrant " << which << " should be at " << out_col << ',' << out_row;
  }
}

TEST(PyroWaveEncodeTests, ThePictureThatArrivesIsFullRangeRec709) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  const quadrant_frame_t source {width, height, (width + 8) * 4};
  ASSERT_TRUE(session->encode_packed(source.bgra.data(), width, height, source.stride, 512 * 1024));
  ASSERT_TRUE(session->uses_gpu_input());

  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok) << "the frame this host produced would not decode";
  expect_full_range_rec709(decoded, source, "on the GPU");
}

TEST(PyroWaveEncodeTests, BothPathsProduceTheSamePicture) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  const quadrant_frame_t source {width, height, width * 4};

  // The same picture, the same size at both ends, so neither path scales and the only difference
  // between them is where the colour conversion happened.
  setenv("POLARIS_PYROWAVE_GPU_INPUT", "off", 1);
  auto on_cpu = make_session_420(width, height);
  unsetenv("POLARIS_PYROWAVE_GPU_INPUT");
  auto on_gpu = make_session_420(width, height);
  ASSERT_NE(on_cpu, nullptr);
  ASSERT_NE(on_gpu, nullptr);

  ASSERT_TRUE(on_cpu->encode_packed(source.bgra.data(), width, height, source.stride, 512 * 1024));
  ASSERT_TRUE(on_gpu->encode_packed(source.bgra.data(), width, height, source.stride, 512 * 1024));
  ASSERT_FALSE(on_cpu->uses_gpu_input());
  ASSERT_TRUE(on_gpu->uses_gpu_input());

  const decoded_frame_t from_cpu {on_cpu->bitstream(), width, height, false};
  const decoded_frame_t from_gpu {on_gpu->bitstream(), width, height, false};
  ASSERT_TRUE(from_cpu.ok);
  ASSERT_TRUE(from_gpu.ok);

  // Held to the promise one at a time before they are held to each other, so a failure says which
  // path broke it rather than only that they differ.
  expect_full_range_rec709(from_cpu, source, "converted on the CPU");
  expect_full_range_rec709(from_gpu, source, "converted on the GPU");

  // Not identical: two converters, one in fixed point on the CPU and one in floating point with
  // dithering on the GPU, will not agree to the last bit and do not need to. Agreeing on average is
  // the claim, because that is what makes swapping one for the other invisible.
  double total = 0.0;
  int worst = 0;
  for (std::size_t at = 0; at < from_cpu.y.size(); ++at) {
    const int difference = std::abs(static_cast<int>(from_cpu.y[at]) - from_gpu.y[at]);
    total += difference;
    worst = std::max(worst, difference);
  }
  const double average = total / from_cpu.y.size();
  EXPECT_LT(average, 3.0) << "the two paths disagree by " << average << " on average, worst " << worst;
}

TEST(PyroWaveEncodeTests, FourFourFourGoesThroughTheGpuPathToo) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = pyrowave_encode::make_session(width, height, pyrowave_encode::chroma_e::yuv444);
  ASSERT_NE(session, nullptr);

  // The chroma a session carries changes what the codec's scaler writes, three full sized planes
  // rather than one and two quarters, and the only thing that says so is the enum it was created
  // with. A path that quietly produced 4:2:0 here would decode as a frame the wrong shape.
  const quadrant_frame_t source {width, height, (width + 12) * 4};
  ASSERT_TRUE(session->encode_packed(source.bgra.data(), width, height, source.stride, 512 * 1024));
  ASSERT_TRUE(session->uses_gpu_input());

  const decoded_frame_t decoded {session->bitstream(), width, height, true};
  ASSERT_TRUE(decoded.ok) << "a 4:4:4 frame from the GPU path would not decode as 4:4:4";
  expect_full_range_rec709(decoded, source, "4:4:4 on the GPU");

  // And the chroma really is per pixel. With a sample of its own on every pixel, the boundary
  // between two quadrants is one pixel wide; at 4:2:0 the two either side share one and the colour
  // bleeds across it.
  const int boundary = width / 2;
  const expected_ycbcr_t red {255, 0, 0};
  const expected_ycbcr_t green {0, 255, 0};
  EXPECT_NEAR(decoded.chroma_v_at(boundary - 2, height / 4), red.v, 12.0) << "just left of the seam";
  EXPECT_NEAR(decoded.chroma_v_at(boundary + 2, height / 4), green.v, 12.0) << "just right of it";
}

TEST(PyroWaveEncodeTests, ARepeatedFrameOnTheGpuIsTheSamePictureAgain) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  const quadrant_frame_t source {width, height, width * 4};
  ASSERT_TRUE(session->encode_packed(source.bgra.data(), width, height, source.stride, 512 * 1024));
  ASSERT_TRUE(session->uses_gpu_input());
  const sequence_header_t first {session->bitstream()};
  ASSERT_TRUE(first.present);

  // A host repeats a frame when capture has nothing new. On this path the picture is already on the
  // GPU in the layout the codec reads, so a repeat records the encode and copies nothing, and the
  // thing that has to come out of it is the same picture under a new sequence number: the same
  // bitstream twice is a frame the decoder throws away, which is what this used to do.
  ASSERT_TRUE(session->encode_retained(512 * 1024));
  const sequence_header_t again {session->bitstream()};
  ASSERT_TRUE(again.present);
  EXPECT_NE(again.sequence, first.sequence) << "a repeat has to be a new frame to the decoder";

  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok) << "the repeated frame would not decode";
  expect_full_range_rec709(decoded, source, "repeated on the GPU");

  // And again, because the interesting failure is the second repeat: a layout tracked wrongly shows
  // up when the image is read twice with no write in between.
  ASSERT_TRUE(session->encode_retained(512 * 1024));
  const decoded_frame_t twice {session->bitstream(), width, height, false};
  ASSERT_TRUE(twice.ok);
  expect_full_range_rec709(twice, source, "repeated twice on the GPU");
}

TEST(PyroWaveEncodeTests, AnHdrStreamCarriesBt2020AndPq) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = pyrowave_encode::make_session(width, height, pyrowave_encode::chroma_e::yuv420,
                                              pyrowave_encode::dynamic_range_e::hdr10);
  ASSERT_NE(session, nullptr);

  // A padded stride again, and ten bit samples packed the way capture packs them.
  const quadrant_frame_10bit_t source {width, height, (width + 16) * 4};
  ASSERT_TRUE(session->encode_packed(source.pixels.data(), width, height, source.stride, 768 * 1024));
  ASSERT_TRUE(session->uses_gpu_input()) << "HDR has no other path to take";

  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok) << "the HDR frame this host produced would not decode";

  // BT.2020 rather than Rec. 709, and the PQ transfer function applied once rather than twice. Both
  // are invisible from this side of the encoder and both ruin the picture: the wrong primaries shift
  // every saturated hue, and a transfer function applied twice crushes everything dark.
  for (int which = 0; which < 4; ++which) {
    const auto [col, row] = source.centre_of(which);
    const expected_ycbcr_t want {source.as_eight_bit(which, 0), source.as_eight_bit(which, 1),
                                 source.as_eight_bit(which, 2), primaries_e::bt2020};

    EXPECT_NEAR(decoded.luma_at(col, row), want.y, 6.0) << "quadrant " << which << " luma";
    EXPECT_NEAR(decoded.chroma_u_at(col, row), want.u, 8.0) << "quadrant " << which << " Cb";
    EXPECT_NEAR(decoded.chroma_v_at(col, row), want.v, 8.0) << "quadrant " << which << " Cr";
  }

  // And it is not the same numbers Rec. 709 would have produced, or this test would pass either way.
  const auto [col, row] = source.centre_of(0);
  const expected_ycbcr_t as_709 {source.as_eight_bit(0, 0), source.as_eight_bit(0, 1),
                                 source.as_eight_bit(0, 2), primaries_e::bt709};
  const expected_ycbcr_t as_2020 {source.as_eight_bit(0, 0), source.as_eight_bit(0, 1),
                                  source.as_eight_bit(0, 2), primaries_e::bt2020};
  ASSERT_GT(std::abs(as_709.y - as_2020.y), 12.0) << "the two matrices are too close to tell apart";
  EXPECT_GT(std::abs(decoded.luma_at(col, row) - as_709.y), 6.0)
    << "the picture came out as Rec. 709 on a stream that promised BT.2020";
}

TEST(PyroWaveEncodeTests, AnHdrSessionRefusesToBeMadeWithoutTheGpuPath) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  // The codec's system memory entry point takes eight bit planes and nothing else, so an HDR session
  // with the GPU path switched off has no way to carry what it promises. Refused when it is asked
  // for, rather than at the first frame, because by then a client has been told it is streaming.
  setenv("POLARIS_PYROWAVE_GPU_INPUT", "off", 1);
  auto refused = pyrowave_encode::make_session(1280, 720, pyrowave_encode::chroma_e::yuv420,
                                               pyrowave_encode::dynamic_range_e::hdr10);
  auto allowed = pyrowave_encode::make_session(1280, 720, pyrowave_encode::chroma_e::yuv420);
  unsetenv("POLARIS_PYROWAVE_GPU_INPUT");

  EXPECT_EQ(refused, nullptr);
  EXPECT_NE(allowed, nullptr) << "an SDR session still has the CPU converter to fall back on";
}

TEST(PyroWaveEncodeTests, EachDynamicRangeHasItsOwnProfileToken) {
  // A client agrees to one of these before a frame is sent, and the two describe different bytes.
  // Sharing a token would mean a client that agreed to Rec. 709 accepting PQ BT.2020 without
  // knowing, which is a plausible looking wrong picture and no error anywhere.
  EXPECT_STRNE(pyrowave_encode::profile_token_for(pyrowave_encode::dynamic_range_e::sdr),
               pyrowave_encode::profile_token_for(pyrowave_encode::dynamic_range_e::hdr10));
  EXPECT_STREQ(pyrowave_encode::profile_token_for(pyrowave_encode::dynamic_range_e::sdr),
               pyrowave_encode::profile_token);
}

TEST(PyroWaveEncodeTests, AFrameAlreadyOnTheGpuIsEncodedWhereItLies) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }
  if (!pyrowave_encode::dmabuf_import_available()) {
    GTEST_SKIP() << "this GPU cannot import a dmabuf";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  const quadrant_frame_t picture {width, height, width * 4};
  const test_dmabuf_t captured {width, height, picture};
  if (!captured.ok) {
    GTEST_SKIP() << "could not allocate a dmabuf on this machine";
  }

  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->encode_imported(captured.buffer, 512 * 1024));
  EXPECT_TRUE(session->uses_gpu_input());

  // The whole point is that no copy happened, and the only way to see that from here is that the
  // picture came out right anyway: the codec read the memory capture wrote, in the layout the
  // modifier described, from the queue family it was acquired from.
  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok) << "the frame encoded from an imported dmabuf would not decode";
  expect_full_range_rec709(decoded, picture, "imported from a dmabuf");
}

TEST(PyroWaveEncodeTests, AnImportedFrameThatNeedsBarsGetsThemOnTheGpu) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }
  if (!pyrowave_encode::dmabuf_import_available()) {
    GTEST_SKIP() << "this GPU cannot import a dmabuf";
  }

  constexpr int width = 640;
  constexpr int height = 360;

  // Twice as wide for its height as the stream, so the imported frame cannot be read directly: it is
  // copied into the middle of an image shaped like the stream, on the GPU, and the bars are cleared.
  const quadrant_frame_t picture {1280, 360, 1280 * 4};
  const test_dmabuf_t captured {1280, 360, picture};
  if (!captured.ok) {
    GTEST_SKIP() << "could not allocate a dmabuf on this machine";
  }

  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->encode_imported(captured.buffer, 512 * 1024));

  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok);

  const int padded_height = picture.width * height / width;
  const int top_bar = (padded_height - picture.height) / 2;
  EXPECT_LT(decoded.luma_at(width / 2, 8), 8) << "the bar above the picture is not black";
  EXPECT_LT(decoded.luma_at(width / 2, height - 8), 8) << "the bar below it is not black";

  for (int which = 0; which < 4; ++which) {
    const auto [col, row] = picture.centre_of(which);
    const int out_col = col * width / picture.width;
    const int out_row = (row + top_bar) * height / padded_height;
    const expected_ycbcr_t want {picture.colours[which][0], picture.colours[which][1],
                                 picture.colours[which][2]};
    EXPECT_NEAR(decoded.luma_at(out_col, out_row), want.y, 8.0)
      << "quadrant " << which << " should be at " << out_col << ',' << out_row;
  }
}

TEST(PyroWaveEncodeTests, ARepeatedFrameWorksAfterAnImportedOne) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }
  if (!pyrowave_encode::dmabuf_import_available()) {
    GTEST_SKIP() << "this GPU cannot import a dmabuf";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  const quadrant_frame_t picture {width, height, width * 4};
  const test_dmabuf_t captured {width, height, picture};
  if (!captured.ok) {
    GTEST_SKIP() << "could not allocate a dmabuf on this machine";
  }

  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->encode_imported(captured.buffer, 512 * 1024));
  const sequence_header_t first {session->bitstream()};
  ASSERT_TRUE(first.present);

  // The host repeats a frame when capture has nothing new, and on this path that means encoding a
  // picture whose buffer capture has already taken back. So the frame has to have been copied
  // somewhere this session still owns, and this is the test that says it was: a repeat that refuses
  // would take the session down with it, which is what a host does with a still screen.
  ASSERT_TRUE(session->encode_retained(512 * 1024))
    << "a frame that arrived as a dmabuf cannot be repeated, so a still screen ends the session";

  const sequence_header_t again {session->bitstream()};
  ASSERT_TRUE(again.present);
  EXPECT_NE(again.sequence, first.sequence);

  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok);
  expect_full_range_rec709(decoded, picture, "repeated after an import");
}

/**
 * The frame Polaris primes an encoder with, before capture has produced one.
 *
 * On the path where frames arrive as a dmabuf that frame carries no pixels at all, so the picture it
 * stands for is made rather than read. Getting it wrong is not subtle: a failure here tears the
 * session down, the host builds another, and it did that forty thousand times in ten seconds.
 */
TEST(PyroWaveEncodeTests, TheFrameWithNoPictureInItIsBlack) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  ASSERT_TRUE(session->encode_blank(512 * 1024))
    << "a session cannot make the frame it is primed with, so it ends and is built again";
  ASSERT_FALSE(session->bitstream().empty());

  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok) << "the frame this host produced would not decode";

  // Black, full range: no luma anywhere and neutral chroma. Wavelet ringing is why these are ranges
  // rather than equalities, and a picture that was not black would miss them by a hundred.
  const auto luma = *std::max_element(decoded.y.begin(), decoded.y.end());
  EXPECT_LE(luma, 4) << "the brightest sample of a black frame is " << static_cast<int>(luma);
  const auto blue = *std::max_element(decoded.u.begin(), decoded.u.end());
  const auto red = *std::max_element(decoded.v.begin(), decoded.v.end());
  EXPECT_NEAR(blue, 128, 4);
  EXPECT_NEAR(red, 128, 4);
}

/**
 * And it decides nothing, which is the part that bit.
 *
 * Clearing an image does not use the host copy the staged path needs and says nothing about how the
 * real frames will arrive, so a session promoted to the GPU by this frame is claiming something it
 * has not tested. It can no longer fall back to the converter afterwards, so the first real frame
 * that cannot be staged ends the session instead of being carried slowly.
 */
TEST(PyroWaveEncodeTests, TheFrameWithNoPictureInItLeavesThePathUndecided) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 640;
  constexpr int height = 360;
  auto session = make_session_420(width, height);
  ASSERT_NE(session, nullptr);

  ASSERT_TRUE(session->encode_blank(512 * 1024));
  EXPECT_FALSE(session->uses_gpu_input())
    << "the primer frame claimed the GPU path works, and now nothing can fall back to the converter";

  // The first frame with a picture in it is still the one that decides, and still gets it right.
  const quadrant_frame_t source {width, height, width * 4};
  ASSERT_TRUE(session->encode_packed(source.bgra.data(), width, height, source.stride, 512 * 1024));
  EXPECT_TRUE(session->uses_gpu_input());

  const decoded_frame_t decoded {session->bitstream(), width, height, false};
  ASSERT_TRUE(decoded.ok);
  expect_full_range_rec709(decoded, source, "after a blank frame");
}

/**
 * Every Linux display factory has to recognise this encoder's device type.
 *
 * The encoder asks for one of its own so the portal will offer it a dmabuf, and a factory that does
 * not know the type returns nothing. A capture thread with no display exits, while the codec is still
 * advertised to the client, so the session is negotiated and then dies with no picture and nothing in
 * any log to connect the two.
 *
 * That is what wlgrab and x11grab did the day this encoder was given its own type: every wlroots and
 * X11 host stopped being able to stream a codec that had worked on them the day before. The type is
 * read here rather than a behaviour, because opening either display needs a compositor.
 *
 * kmsgrab is not in this list on purpose. Its factory has no reject list: an unrecognised type falls
 * through to the RAM path, which is the right answer for this one.
 */
TEST(PyroWaveCaptureBackendTests, EveryDisplayFactoryAdmitsThisEncodersDeviceType) {
  for (const char *file : {"src/platform/linux/wlgrab.cpp", "src/platform/linux/x11grab.cpp"}) {
    const auto source = read_source_for_contract(file);
    ASSERT_FALSE(source.empty()) << file << " could not be read";
    EXPECT_NE(source.find("mem_type_e::vulkan_pyrowave"), std::string::npos)
      << file << " opens no display for this encoder, so no host on that backend can stream it";
  }
}

#endif  // POLARIS_BUILD_PYROWAVE
