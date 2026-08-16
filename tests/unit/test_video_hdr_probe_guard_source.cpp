/**
 * @file tests/unit/test_video_hdr_probe_guard_source.cpp
 * @brief Source guard: the HDR/YUV444 *capability* probes in validate_encoder
 *        must never be able to fail the whole encoder.
 *
 * The real defect this pins can only reproduce on specific encoder + capture
 * combinations (observed live: the 10-bit-SDR -> 8-bit-NV12 demotion on a
 * labwc ext-image-copy DMA-BUF path deterministically THREW during the 10-bit
 * HEVC capability probe). That exception propagated out of validate_encoder and
 * tripped its fail_guard, rejecting nvenc entirely and tearing down an
 * otherwise-fine SDR session — the arc's long-standing "intermittent nvenc"
 * disconnect. A trial encode that hits real hardware can't be exercised from a
 * unit test, so this guards the invariant at the source level, the same way
 * test_kmsgrab_logging_source.cpp guards its non-fatal-probe contract: an HDR /
 * YUV444 capability probe raising must be caught and downgraded to
 * "unsupported", never allowed to fail the encoder.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

  std::string read_video_source() {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/video.cpp";
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

}  // namespace

TEST(VideoHdrProbeGuardSource, HdrCapabilityProbeCannotFailTheEncoder) {
  const auto source = read_video_source();
  ASSERT_FALSE(source.empty()) << "could not read src/video.cpp via POLARIS_SOURCE_DIR";

  // The HDR/YUV444 probe lambda must catch anything the trial encode raises and
  // keep the encoder available for SDR rather than propagating the failure.
  const auto probe_pos = source.find("auto test_hdr_and_yuv444 = [&]");
  ASSERT_NE(probe_pos, std::string::npos) << "test_hdr_and_yuv444 probe not found";

  const auto probe_body = source.substr(probe_pos, 2048);
  EXPECT_NE(probe_body.find("try {"), std::string::npos)
    << "the HDR/YUV444 capability probe must run inside a try block";
  EXPECT_NE(probe_body.find("catch (const std::exception"), std::string::npos)
    << "the HDR/YUV444 capability probe must catch std::exception";
  EXPECT_NE(probe_body.find("catch (...)"), std::string::npos)
    << "the HDR/YUV444 capability probe must also catch non-standard exceptions";
  EXPECT_NE(probe_body.find("keeping the encoder for SDR"), std::string::npos)
    << "a caught capability-probe exception must keep the encoder for SDR";

  // The H.264 YUV444 capability probe (outside the lambda) must be guarded too.
  EXPECT_NE(source.find("H.264 YUV444 capability probe raised"), std::string::npos)
    << "the H.264 YUV444 capability probe must catch and downgrade a raised exception";

  EXPECT_NE(source.find("if (result && !encoder_probe_in_progress)"), std::string::npos)
    << "capability probes must not demote their requested 10-bit input to the live-session 8-bit path";
  EXPECT_NE(source.find("hevc_profile_for_input(colorspace.bit_depth"), std::string::npos)
    << "HEVC profile selection must follow the actual encoder input depth";
}
