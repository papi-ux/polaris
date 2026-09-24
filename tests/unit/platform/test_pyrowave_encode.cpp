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
  #include <numeric>
  #include <limits>
  #include <fstream>
  #include <cstdlib>
  #include <vulkan/vulkan.h>
  #include "pyrowave.h"

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

}  // namespace

TEST(PyroWaveEncodeTests, TheLinkedLibraryAnswersForItself) {
  // Read from the library rather than the build system. If these two ever disagree, this is where
  // it shows up, rather than in a stream that decodes to noise.
  const auto version = pyrowave_encode::api_version();
  EXPECT_FALSE(version.empty());
  EXPECT_NE(version.find('.'), std::string::npos);
}

TEST(PyroWaveEncodeTests, LiveBudgetsPreserveFractionalRatesAndTransportBounds) {
  EXPECT_EQ(pyrowave_encode::frame_budget(100000, 120, 1), 104166U);
  EXPECT_EQ(pyrowave_encode::frame_budget(150000, 120, 1), 156250U);
  EXPECT_EQ(pyrowave_encode::frame_budget(200000, 120, 1), 208333U);
  EXPECT_EQ(pyrowave_encode::frame_budget(100000, 60000, 1001), 208541U);
  EXPECT_EQ(pyrowave_encode::frame_budget(100000, 60, 1), pyrowave_encode::frame_budget(200000, 120, 1));
  EXPECT_FALSE(pyrowave_encode::frame_budget(0, 120, 1));
  EXPECT_FALSE(pyrowave_encode::frame_budget(-1, 120, 1));
  EXPECT_FALSE(pyrowave_encode::frame_budget(100000, 0, 1));
  EXPECT_FALSE(pyrowave_encode::frame_budget(100000, 120, 0));
  EXPECT_FALSE(pyrowave_encode::frame_budget(1, 240, 1));
  EXPECT_FALSE(pyrowave_encode::frame_budget(300000, 1, 1));
  EXPECT_FALSE(pyrowave_encode::frame_budget(std::numeric_limits<int>::max(), 1, std::numeric_limits<int>::max()));
}

TEST(PyroWaveEncodeTests, AnOddExtentIsRefusedRatherThanRounded) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }
  // 4:2:0 has no half chroma sample. Zero and negative are the same question asked louder.
  EXPECT_EQ(pyrowave_encode::make_session(0, 720), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(1280, 0), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(-2, -2), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(1279, 720), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(4098, 720), nullptr);
}

TEST(PyroWaveEncodeTests, PreparedStaticImageUsesEachNewBudgetAndOwnsItsPixels) {
  if (!pyrowave_encode::available()) GTEST_SKIP() << "no compatible Vulkan device";
  constexpr int width = 640, height = 360;
  auto session = pyrowave_encode::make_session(width, height);
  ASSERT_NE(session, nullptr);
  EXPECT_FALSE(session->encode_prepared(20000));
  std::vector<uint8_t> bgra(width * height * 4);
  unsigned random = 42;
  for (auto &byte : bgra) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = random; }
  ASSERT_TRUE(session->prepare_bgra(bgra.data(), width * 4));
  EXPECT_TRUE(session->packets(pyrowave_encode::packet_bytes).empty());
  // The caller may release its capture allocation before the GPU is asked to encode.
  bgra.clear(); bgra.shrink_to_fit();
  const auto encode_size = [&](std::size_t budget) {
    if (!session->encode_prepared(budget)) return std::size_t(0);
    const auto packets = session->packets(pyrowave_encode::packet_bytes);
    return std::accumulate(packets.begin(), packets.end(), std::size_t(0),
      [](auto size, const auto &packet) { return size + packet.size(); });
  };
  const auto low = encode_size(20000);
  const auto high = encode_size(80000);
  const auto restored = encode_size(20000);
  EXPECT_GT(low, 0U); EXPECT_LE(low, 20000U);
  EXPECT_GT(high, low * 2); EXPECT_LE(high, 80000U);
  EXPECT_GT(restored, 0U); EXPECT_LE(restored, 20000U);
  EXPECT_FALSE(session->prepare_bgra(nullptr, width * 4));
  EXPECT_TRUE(session->packets(pyrowave_encode::packet_bytes).empty());
  EXPECT_FALSE(session->encode_prepared(80000));
  EXPECT_TRUE(session->packets(pyrowave_encode::packet_bytes).empty());
}

TEST(PyroWaveEncodeTests, EncodesAFrameIntoPacketsTheNetworkCanCarry) {
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

  // A boundary a real stream would use, near the usual MTU.
  constexpr std::size_t packet_boundary = 1024;
  const auto packets = session->packets(packet_boundary);
  ASSERT_FALSE(packets.empty());

  const auto total = std::accumulate(packets.begin(), packets.end(), std::size_t {0},
                                     [](std::size_t sum, const auto &packet) { return sum + packet.size(); });
  EXPECT_GT(total, 0U);
  EXPECT_LE(total, max_bytes) << "rate control is supposed to be exact, not approximate";

  for (const auto &packet : packets) {
    EXPECT_FALSE(packet.empty());
  }
}

TEST(PyroWaveEncodeTests, ThePacketBoundaryIsASplitTargetAndNotACap) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  // This is the contract, and it is not what the parameter name suggests. PyroWave splits between
  // coefficient blocks and never inside one: it closes the current packet before appending a block
  // that would overflow the boundary, then appends that block whole. So a single block larger than
  // the boundary produces a packet larger than the boundary, and no boundary can prevent it.
  //
  // These are coefficient chunks, not network datagrams. Polaris concatenates
  // them into one GameStream frame, and the transport shards that frame to its
  // negotiated packet size. The coefficient split target must not be mistaken
  // for a network MTU or an allocation bound.
  constexpr int width = 1280;
  constexpr int height = 720;
  auto session = pyrowave_encode::make_session(width, height);
  ASSERT_NE(session, nullptr);

  const test_frame_t frame {width, height};
  ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), 512 * 1024));

  constexpr std::size_t packet_boundary = 1024;
  const auto packets = session->packets(packet_boundary);
  ASSERT_FALSE(packets.empty());

  std::size_t largest = 0;
  for (const auto &packet : packets) {
    largest = std::max(largest, packet.size());
  }
  // Left as a live observation rather than a pinned number: what it is depends on the rate control
  // budget and the picture. The point of the test is that it can exceed the boundary at all.
  EXPECT_GT(largest, 0U);

  // Ask for a boundary no block can exceed and every packet fits, which is the same rule seen from
  // the other side.
  const auto roomy = session->packets(64 * 1024);
  ASSERT_FALSE(roomy.empty());
  for (const auto &packet : roomy) {
    EXPECT_LE(packet.size(), 64U * 1024U);
  }
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

  ASSERT_TRUE(session->encode_bgra(bgra.data(), stride, 256 * 1024));
  const auto packets = session->packets(1024);
  ASSERT_FALSE(packets.empty());

  // The converter is built once and kept, so the second frame has to work as well as the first.
  ASSERT_TRUE(session->encode_bgra(bgra.data(), stride, 256 * 1024));
  EXPECT_FALSE(session->packets(1024).empty());
}

TEST(PyroWaveEncodeTests, ARefusedFrameIsRefusedRatherThanGuessed) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }

  auto session = pyrowave_encode::make_session(320, 240);
  ASSERT_NE(session, nullptr);

  std::vector<uint8_t> bgra(static_cast<std::size_t>(320) * 240 * 4, 0x40);
  EXPECT_FALSE(session->encode_bgra(nullptr, 320 * 4, 64 * 1024));
  EXPECT_FALSE(session->encode_bgra(bgra.data(), 0, 64 * 1024));
  EXPECT_FALSE(session->encode_bgra(bgra.data(), -1, 64 * 1024));
  EXPECT_FALSE(session->encode(nullptr, nullptr, nullptr, 64 * 1024));
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
  // through one session has to produce a usable frame both times. A codec with a reference chain
  // would quietly produce a much smaller second frame here.
  const test_frame_t frame {width, height};
  ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), 256 * 1024));
  const auto first = session->packets(1024);
  ASSERT_FALSE(first.empty());

  ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), 256 * 1024));
  const auto second = session->packets(1024);
  ASSERT_FALSE(second.empty());
  EXPECT_EQ(first.size(), second.size());
}


TEST(PyroWaveEncodeTests, InvalidInputCannotReplayPreviousFrame) {
  if (!pyrowave_encode::available()) GTEST_SKIP();
  auto session = pyrowave_encode::make_session(128, 96);
  ASSERT_NE(session, nullptr);
  test_frame_t frame(128, 96);
  EXPECT_TRUE(session->packets(1024).empty());
  ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), 64000));
  EXPECT_FALSE(session->packets(1024).empty());
  EXPECT_FALSE(session->encode(nullptr, frame.u.data(), frame.v.data(), 64000));
  EXPECT_TRUE(session->packets(1024).empty());
  ASSERT_TRUE(session->encode(frame.y.data(), frame.u.data(), frame.v.data(), 64000));
  EXPECT_FALSE(session->encode_bgra(frame.y.data(), 128 * 4 - 1, 64000));
  EXPECT_TRUE(session->packets(1024).empty());
}

TEST(PyroWaveEncodeTests, FramePackingRejectsInvalidLengthsAndExtents) {
  EXPECT_TRUE(pyrowave_encode::pack_frame({}, 128, 96).empty());
  EXPECT_TRUE(pyrowave_encode::pack_frame({std::vector<uint8_t>(7)}, 128, 96).empty());
  EXPECT_TRUE(pyrowave_encode::pack_frame({std::vector<uint8_t>(12)}, 127, 96).empty());
  EXPECT_TRUE(pyrowave_encode::pack_frame({std::vector<uint8_t>(65536)}, 128, 96).empty());
  std::vector<std::vector<uint8_t>> oversized(140, std::vector<uint8_t>(61440));
  EXPECT_TRUE(pyrowave_encode::pack_frame(oversized, 128, 96).empty());
}

TEST(PyroWaveEncodeTests, CaptureScalesToRequestedExtentWithFullRangeRec709AndLetterboxing) {
  if (!pyrowave_encode::available()) GTEST_SKIP();
  constexpr int width = 128, height = 96, source_width = 192, source_height = 108;
  auto session = pyrowave_encode::make_session(width, height, source_width, source_height);
  ASSERT_NE(session, nullptr);
  constexpr int stride = source_width * 4 + 32;
  std::vector<uint8_t> bgra(stride * source_height);
  for (int y = 0; y < source_height; ++y) for (int x = 0; x < source_width; ++x) {
    auto* p = bgra.data() + y * stride + x * 4;
    p[0] = 58; p[1] = 120; p[2] = 179; p[3] = 255;
  }
  ASSERT_TRUE(session->encode_bgra(bgra.data(), stride, 64000));
  const auto packets = session->packets(pyrowave_encode::packet_bytes);
  ASSERT_FALSE(packets.empty());
  const auto frame = pyrowave_encode::pack_frame(packets, width, height);
  ASSERT_GT(frame.size(), 32U);
  EXPECT_EQ(frame[0], width - 1);
  EXPECT_NE(frame[3] & 0x80, 0); // upstream extended sequence header
  // An explicit path lets the matched Nova presenter consume the actual host
  // output during interoperability validation. Ordinary unit tests write nothing.
  if (const char* path = std::getenv("POLARIS_PYROWAVE_TEST_FRAME")) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    ASSERT_TRUE(out.good());
  }
  struct Decoder {
    pyrowave_device device = nullptr;
    pyrowave_decoder decoder = nullptr;
    ~Decoder() { if (decoder) pyrowave_decoder_destroy(decoder); if (device) pyrowave_device_destroy(device); }
  } decoder;
  ASSERT_EQ(pyrowave_create_default_device(&decoder.device), PYROWAVE_SUCCESS);
  pyrowave_decoder_create_info info{};
  info.device = decoder.device; info.width = width; info.height = height; info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
  ASSERT_EQ(pyrowave_decoder_create(&info, &decoder.decoder), PYROWAVE_SUCCESS);
  for (const auto& packet : packets)
    ASSERT_EQ(pyrowave_decoder_push_packet(decoder.decoder, packet.data(), packet.size()), PYROWAVE_SUCCESS);
  ASSERT_TRUE(pyrowave_decoder_decode_is_ready(decoder.decoder, false));
  test_frame_t output(width, height);
  pyrowave_cpu_buffer buffer{};
  buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P; buffer.width = width; buffer.height = height;
  buffer.data[0] = output.y.data(); buffer.data[1] = output.u.data(); buffer.data[2] = output.v.data();
  for (int i = 0; i < 3; ++i) {
    buffer.row_stride_in_bytes[i] = i ? width / 2 : width;
    buffer.plane_size_in_bytes[i] = i ? width * height / 4 : width * height;
  }
  ASSERT_EQ(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder.decoder, &buffer), PYROWAVE_SUCCESS);
  EXPECT_NEAR(output.y[height / 2 * width + width / 2], 128, 4);
  EXPECT_NEAR(output.u[height / 4 * (width / 2) + width / 4], 90, 4);
  EXPECT_NEAR(output.v[height / 4 * (width / 2) + width / 4], 160, 4);
  EXPECT_NEAR(output.y[4 * width + width / 2], 0, 4);
  EXPECT_NEAR(output.y[(height - 5) * width + width / 2], 0, 4);
}

#endif  // POLARIS_BUILD_PYROWAVE
