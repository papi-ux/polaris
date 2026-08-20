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

TEST(VideoCaptureGuardSource, MissingEncoderStopsBeforeCaptureDereference) {
  const auto source = read_video_source();
  ASSERT_FALSE(source.empty()) << "could not read src/video.cpp via POLARIS_SOURCE_DIR";

  const auto dereference_pos = source.find("if (chosen_encoder->flags & PARALLEL_ENCODING)");
  ASSERT_NE(dereference_pos, std::string::npos) << "capture encoder dereference not found";

  const auto capture_pos = source.rfind("void capture(", dereference_pos);
  ASSERT_NE(capture_pos, std::string::npos) << "capture overload not found";

  const auto guard_pos = source.find("if (!chosen_encoder)", capture_pos);
  ASSERT_NE(guard_pos, std::string::npos) << "missing-encoder guard not found";
  EXPECT_LT(guard_pos, dereference_pos) << "missing-encoder guard must run before dereference";

  const auto guarded_prefix = source.substr(guard_pos, dereference_pos - guard_pos);
  EXPECT_NE(guarded_prefix.find("return;"), std::string::npos)
    << "missing encoder must stop video capture rather than continue";
}

TEST(VideoCaptureGuardSource, EncoderProbeCannotMutateStateDuringCapture) {
  const auto source = read_video_source();
  ASSERT_FALSE(source.empty()) << "could not read src/video.cpp via POLARIS_SOURCE_DIR";

  const auto probe_pos = source.find(
    "int probe_encoders(bool strict_configured_encoder, bool save_successful_cache)"
  );
  ASSERT_NE(probe_pos, std::string::npos) << "encoder probe entry point not found";
  const auto probe_prefix = source.substr(probe_pos, 1024);
  EXPECT_NE(
    probe_prefix.find("std::unique_lock encoder_state_lock {encoder_state_mutex, std::defer_lock};"),
    std::string::npos
  ) << "encoder probing must hold the encoder-state writer lock";
  EXPECT_NE(probe_prefix.find("encoder_state_lock.try_lock_for(2s)"), std::string::npos)
    << "encoder probing must fail closed instead of waiting indefinitely for capture";

  const auto dereference_pos = source.find("if (chosen_encoder->flags & PARALLEL_ENCODING)");
  ASSERT_NE(dereference_pos, std::string::npos) << "capture encoder dereference not found";
  const auto capture_pos = source.rfind("void capture(", dereference_pos);
  ASSERT_NE(capture_pos, std::string::npos) << "capture overload not found";
  const auto capture_prefix = source.substr(capture_pos, dereference_pos - capture_pos);
  const auto capture_lock_pos = capture_prefix.find(
    "std::shared_lock encoder_state_lock {encoder_state_mutex};"
  );
  const auto missing_encoder_guard_pos = capture_prefix.find("if (!chosen_encoder)");
  ASSERT_NE(capture_lock_pos, std::string::npos)
    << "capture must hold an encoder-state reader lock";
  ASSERT_NE(missing_encoder_guard_pos, std::string::npos)
    << "capture missing-encoder guard not found";
  EXPECT_LT(capture_lock_pos, missing_encoder_guard_pos)
    << "the reader lock must cover the first selected-encoder read";

  const auto reset_pos = source.find("void reset_encoder_probe_state()");
  ASSERT_NE(reset_pos, std::string::npos) << "encoder state reset entry point not found";
  const auto reset_body = source.substr(reset_pos, 512);
  EXPECT_NE(
    reset_body.find("std::unique_lock encoder_state_lock {encoder_state_mutex, std::defer_lock};"),
    std::string::npos
  ) << "external encoder-state resets must hold the writer lock";
  EXPECT_NE(reset_body.find("encoder_state_lock.try_lock_for(2s)"), std::string::npos)
    << "external resets must not wait indefinitely for active capture";
}

TEST(VideoHdrDiagnosticsSource, WarnsWhenAClientHdrRequestBecomesAnSdrStream) {
  const auto source = read_video_source();
  ASSERT_FALSE(source.empty()) << "could not read src/video.cpp via POLARIS_SOURCE_DIR";

  const auto encode_device_pos = source.find(
    "std::unique_ptr<platf::encode_device_t> make_encode_device("
  );
  ASSERT_NE(encode_device_pos, std::string::npos) << "make_encode_device not found";

  const auto decision = source.substr(encode_device_pos, 8192);
  const auto result_pos = decision.find(
    "if (result) {\n      result->colorspace = colorspace;"
  );
  ASSERT_NE(result_pos, std::string::npos)
    << "successful encode-device finalization and final colorspace assignment not found";

  const auto colorspace_assignment_pos = decision.find(
    "result->colorspace = colorspace;",
    result_pos
  );
  ASSERT_NE(colorspace_assignment_pos, std::string::npos)
    << "final encode-device colorspace assignment not found";

  const auto find_matching_brace = [&](const std::size_t open_brace_pos) {
    if (open_brace_pos == std::string::npos || decision[open_brace_pos] != '{') {
      return std::string::npos;
    }

    std::size_t depth = 0;
    for (auto pos = open_brace_pos; pos < decision.size(); ++pos) {
      if (decision[pos] == '{') {
        ++depth;
      } else if (decision[pos] == '}' && --depth == 0) {
        return pos;
      }
    }
    return std::string::npos;
  };

  const auto result_open_brace_pos = decision.find('{', result_pos);
  const auto result_block_end_pos = find_matching_brace(result_open_brace_pos);
  ASSERT_NE(result_block_end_pos, std::string::npos)
    << "successful encode-device finalization block is malformed";

  const auto return_pos = decision.find("return result;", result_block_end_pos);
  ASSERT_NE(return_pos, std::string::npos) << "make_encode_device return not found";
  const auto after_finalization = decision.substr(
    result_block_end_pos + 1,
    return_pos - result_block_end_pos - 1
  );
  EXPECT_EQ(after_finalization.find_first_not_of(" \t\r\n"), std::string::npos)
    << "successful result finalization must remain the last operation before return";

  const auto warning_guard_pos = decision.find(
    "!encoder_probe_in_progress && config.dynamicRange > 0 && !colorspace_is_hdr(colorspace)",
    colorspace_assignment_pos
  );
  ASSERT_NE(warning_guard_pos, std::string::npos)
    << "the warning must be limited to live requested HDR that was downgraded to SDR";
  EXPECT_GT(warning_guard_pos, colorspace_assignment_pos)
    << "the warning must use the final encode-device colorspace";
  EXPECT_LT(warning_guard_pos, result_block_end_pos)
    << "the warning must not escape successful encode-device finalization";
  const auto later_colorspace_assignment_pos = decision.find("colorspace =", warning_guard_pos);
  EXPECT_TRUE(
    later_colorspace_assignment_pos == std::string::npos ||
    later_colorspace_assignment_pos > result_block_end_pos
  ) << "the warning must follow the final colorspace mutation";
  const auto later_colorspace_member_pos = decision.find("colorspace.", warning_guard_pos);
  EXPECT_TRUE(
    later_colorspace_member_pos == std::string::npos ||
    later_colorspace_member_pos > result_block_end_pos
  ) << "the warning must follow final colorspace member mutations";

  const auto warning_open_brace_pos = decision.find('{', warning_guard_pos);
  const auto warning_block_end_pos = find_matching_brace(warning_open_brace_pos);
  ASSERT_NE(warning_block_end_pos, std::string::npos)
    << "the diagnostic warning block is malformed";
  EXPECT_LT(warning_block_end_pos, result_block_end_pos)
    << "the warning guard itself must remain inside successful encode-device finalization";

  const auto warning_log_pos = decision.find("BOOST_LOG(warning)", warning_guard_pos);
  ASSERT_NE(warning_log_pos, std::string::npos)
    << "an HDR-to-SDR downgrade must be visible at warning severity";
  EXPECT_LT(warning_log_pos, warning_block_end_pos)
    << "the diagnostic warning must be controlled by the live HDR-to-SDR guard";

  const auto warning = decision.substr(warning_log_pos, warning_block_end_pos - warning_log_pos);
  EXPECT_NE(warning.find("display or capture path"), std::string::npos)
    << "the warning must cover both metadata and 8-bit capture demotions";
  EXPECT_NE(warning.find("black video with working audio"), std::string::npos)
    << "the warning must identify the distinctive client symptom";
  EXPECT_NE(warning.find("disable HDR on the client"), std::string::npos)
    << "the warning must include the immediate recovery action";
}
