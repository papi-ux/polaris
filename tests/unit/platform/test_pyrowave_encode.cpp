/**
 * @file tests/unit/platform/test_pyrowave_encode.cpp
 * @brief Tests for the PyroWave compute encoder.
 */
// test includes
#include "../../tests_common.h"

#ifdef POLARIS_BUILD_PYROWAVE

  // local includes
  #include "src/platform/linux/pyrowave_encode.h"

  #include <algorithm>
  #include <cstring>

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
  // 4:2:0 has no half chroma sample. Zero and negative are the same question asked louder.
  EXPECT_EQ(pyrowave_encode::make_session(0, 720), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(1280, 0), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(-2, -2), nullptr);
}

TEST(PyroWaveEncodeTests, AFrameArrivesAsOneBitstreamADecoderCanOpen) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  constexpr int width = 1280;
  constexpr int height = 720;
  auto session = pyrowave_encode::make_session(width, height);
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
  auto session = pyrowave_encode::make_session(width, height);
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
  auto session = pyrowave_encode::make_session(width, height);
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
  auto session = pyrowave_encode::make_session(stream_width, stream_height);
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
  auto session = pyrowave_encode::make_session(width, height);
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
  auto session = pyrowave_encode::make_session(width, height);
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

#endif  // POLARIS_BUILD_PYROWAVE
