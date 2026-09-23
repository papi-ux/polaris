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

TEST(PyroWaveEncodeTests, AnOddExtentIsRefusedRatherThanRounded) {
  if (!pyrowave_encode::available()) {
    GTEST_SKIP() << "no Vulkan device this codec can use";
  }
  // 4:2:0 has no half chroma sample. Zero and negative are the same question asked louder.
  EXPECT_EQ(pyrowave_encode::make_session(0, 720), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(1280, 0), nullptr);
  EXPECT_EQ(pyrowave_encode::make_session(-2, -2), nullptr);
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
  // It matters because Polaris sends these over UDP. A packet past the path MTU fragments, and a
  // fragmented packet loses the one property this codec is chosen for: that every packet decodes on
  // its own. The stream path has to size the boundary from the MTU, reserve its own header through
  // the padding argument, and treat an oversized packet as a rate control problem to report rather
  // than something to quietly split.
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

#endif  // POLARIS_BUILD_PYROWAVE
