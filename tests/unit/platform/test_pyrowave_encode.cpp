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

  #include <algorithm>
  #include <cmath>
  #include <cstdlib>
  #include <cstring>
  #include <memory>

namespace {

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

  /// Full range Rec. 709 with centred chroma, which is what profile_token promises a client.
  struct expected_ycbcr_t {
    expected_ycbcr_t(int r, int g, int b) {
      const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
      y = luma;
      u = (b - luma) / 1.8556 + 128.0;
      v = (r - luma) / 1.5748 + 128.0;
      y = std::clamp(y, 0.0, 255.0);
      u = std::clamp(u, 0.0, 255.0);
      v = std::clamp(v, 0.0, 255.0);
    }

    double y;
    double u;
    double v;
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
    }

    bool present = false;
    int width = 0;
    int height = 0;
    uint32_t extended = 0;
    uint32_t total_blocks = 0;
    uint32_t code = 0;
    uint32_t chroma_resolution = 0;
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

  ASSERT_TRUE(session->encode_bgra(bgra.data(), width, height, stride, 256 * 1024));
  ASSERT_FALSE(session->bitstream().empty());
  const sequence_header_t header {session->bitstream()};
  ASSERT_TRUE(header.present);
  EXPECT_EQ(header.width, width);
  EXPECT_EQ(header.height, height);

  // The converter is built once and kept, so the second frame has to work as well as the first.
  ASSERT_TRUE(session->encode_bgra(bgra.data(), width, height, stride, 256 * 1024));
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
  EXPECT_FALSE(session->encode_bgra(nullptr, width, height, width * 4, 64 * 1024));
  EXPECT_FALSE(session->encode_bgra(bgra.data(), width, height, 0, 64 * 1024));
  EXPECT_FALSE(session->encode_bgra(bgra.data(), width, height, -1, 64 * 1024));
  EXPECT_FALSE(session->encode_bgra(bgra.data(), 0, height, width * 4, 64 * 1024));
  EXPECT_FALSE(session->encode_bgra(bgra.data(), width, 0, width * 4, 64 * 1024));
  // A stride that cannot hold the row it claims to. Reading it would run off the end of the buffer
  // capture handed over, which is the one argument here that is not merely wrong but unsafe.
  EXPECT_FALSE(session->encode_bgra(bgra.data(), width, height, width * 4 - 1, 64 * 1024));
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

    ASSERT_TRUE(session->encode_bgra(bgra.data(), source.width, source.height, stride, 256 * 1024))
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
    const auto &encoded = session->bitstream();
    ASSERT_GE(encoded.size(), 8U);
    uint32_t first = 0;
    std::memcpy(&first, encoded.data(), sizeof(first));
    sequences.push_back((first >> 28) & 0x7);
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
  ASSERT_TRUE(session->encode_bgra(source.bgra.data(), width, height, source.stride, 512 * 1024));
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
  ASSERT_TRUE(session->encode_bgra(source.bgra.data(), source.width, source.height, source.stride,
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
  ASSERT_TRUE(session->encode_bgra(source.bgra.data(), source.width, source.height, source.stride,
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
  ASSERT_TRUE(session->encode_bgra(source.bgra.data(), width, height, source.stride, 512 * 1024));
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

  ASSERT_TRUE(on_cpu->encode_bgra(source.bgra.data(), width, height, source.stride, 512 * 1024));
  ASSERT_TRUE(on_gpu->encode_bgra(source.bgra.data(), width, height, source.stride, 512 * 1024));
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

#endif  // POLARIS_BUILD_PYROWAVE
